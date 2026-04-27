#include "plane.h"
#include "addr.h"
#include "conn.h"
#include "gso.h"
#include "peer.h"

#include <std/mem/obj_pool.h>
#include <std/sys/crt.h>
#include <std/str/view.h>
#include <std/ios/sys.h>

#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>

using namespace stl;
using namespace gofra;

// Per-tunReader scratch: 64 KiB super-packet read buf + up to 64
// MTU-sized segment buffers + parallel pointer/size tables for
// gsoSplit, plus iovecs for scatter-gather sendmsg(USO).
struct gofra::TunReaderScratch {
    static constexpr size_t MAX_SEGS  = 64;
    static constexpr size_t SEG_SIZE  = 2048;            // MTU + slack
    static constexpr size_t READ_SIZE = 10 + 65535 + 128;

    u8 readBuf[READ_SIZE];
    u8 segBufs[MAX_SEGS][SEG_SIZE];
    u8* segPtrs[MAX_SEGS];
    size_t segSizes[MAX_SEGS];
    iovec iovs[MAX_SEGS];

    TunReaderScratch() noexcept {
        for (size_t i = 0; i < MAX_SEGS; ++i) {
            segPtrs[i] = segBufs[i];
        }
    }
};

// Per-udpReader scratch: 64-deep recvmmsg batch. iov_base points
// buf+10 so the virtio_net_hdr prefix stays zero across calls and
// we hand the TUN [10 B zero hdr | payload]. addrs hold pool-
// allocated sockaddr_storage so the scratch is AF-agnostic.
struct gofra::UdpReaderScratch {
    static constexpr size_t BATCH    = 64;
    static constexpr size_t MSG_SIZE = 10 + 9000;        // virtio + jumbo

    mmsghdr msgs[BATCH];
    iovec iovs[BATCH];
    sockaddr* addrs[BATCH];
    u8 bufs[BATCH][MSG_SIZE];

    explicit UdpReaderScratch(ObjPool* pool) noexcept {
        for (size_t i = 0; i < BATCH; ++i) {
            addrs[i] = (sockaddr*)pool->make<sockaddr_storage>();

            // Pre-zero the virtio_net_hdr prefix; recvmmsg writes
            // into buf+10 only.
            zeroVirtioNetHdr(bufs[i]);

            iovs[i].iov_base = bufs[i] + VIRTIO_NET_HDR_LEN;
            iovs[i].iov_len  = MSG_SIZE - VIRTIO_NET_HDR_LEN;

            msgs[i].msg_hdr.msg_iov     = &iovs[i];
            msgs[i].msg_hdr.msg_iovlen  = 1;
            msgs[i].msg_hdr.msg_name    = addrs[i];
            msgs[i].msg_hdr.msg_namelen = sizeof(sockaddr_storage);
            msgs[i].msg_hdr.msg_control = nullptr;
            msgs[i].msg_hdr.msg_controllen = 0;
            msgs[i].msg_hdr.msg_flags   = 0;
            msgs[i].msg_len             = 0;
        }
    }
};

namespace {
    void warnErrno(StringView what, int err) {
        sysE << StringView(u8"gofra2: ") << what
             << StringView(u8": ")
             << (const char*)strerror(err)
             << endL;
    }

    void warn(StringView msg) {
        sysE << StringView(u8"gofra2: ") << msg << endL;
    }

    // Send one inner IP packet via the next stripe slot.
    void sendOne(Conn* conn, const u8* pkt, size_t len) {
        auto slot = conn->next();

        ssize_t r = ::sendto(slot->srcFd, pkt, len, 0,
                             slot->dstAddr, addrLen(slot->dstAddr));

        if (r < 0) {
            warnErrno(StringView(u8"udp sendto"), errno);
        }
    }

    // Send N gsoSplit segments as USO_PARTS sendmsg(UDP_SEGMENT)
    // calls, each part on its own stripe slot — restores RX-side
    // parallelism (segments hit multiple peer udpReader threads).
    // Parts stay well under the kernel's ~65515 B cork cap.
    constexpr int USO_PARTS = 8;

    void sendUso(Conn* conn, TunReaderScratch* sc, int segs) {
        int partSize = (segs + USO_PARTS - 1) / USO_PARTS;
        u16 gsoSize = (u16)sc->segSizes[0];

        int i = 0;

        while (i < segs) {
            int end = i + partSize;

            if (end > segs) {
                end = segs;
            }

            for (int k = i; k < end; ++k) {
                sc->iovs[k].iov_base = sc->segBufs[k];
                sc->iovs[k].iov_len  = sc->segSizes[k];
            }

            auto* slot = conn->next();

            msghdr msg = {};
            msg.msg_name    = (void*)slot->dstAddr;
            msg.msg_namelen = addrLen(slot->dstAddr);
            msg.msg_iov     = &sc->iovs[i];
            msg.msg_iovlen  = (size_t)(end - i);

            alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(u16))];
            msg.msg_control    = cbuf;
            msg.msg_controllen = sizeof(cbuf);

            cmsghdr* cm = CMSG_FIRSTHDR(&msg);
            cm->cmsg_level = SOL_UDP;
            cm->cmsg_type  = UDP_SEGMENT;
            cm->cmsg_len   = CMSG_LEN(sizeof(u16));
            *(u16*)CMSG_DATA(cm) = gsoSize;

            ssize_t r = ::sendmsg(slot->srcFd, &msg, 0);

            if (r < 0) {
                warnErrno(StringView(u8"udp sendmsg(USO)"), errno);
            }

            i = end;
        }
    }
}

TunReaderScratch* gofra::makeTunReaderScratch(ObjPool* pool) {
    return pool->make<TunReaderScratch>();
}

UdpReaderScratch* gofra::makeUdpReaderScratch(ObjPool* pool) {
    return pool->make<UdpReaderScratch>(pool);
}

void gofra::tunReader(int tunFd, ConnTable* conns, TunReaderScratch* sc) {
    for (;;) {
        ssize_t n = ::read(tunFd, sc->readBuf, sizeof(sc->readBuf));

        if (n < 0) {
            warnErrno(StringView(u8"tun read"), errno);
            continue;
        }

        if ((size_t)n < VIRTIO_NET_HDR_LEN + 20) {
            continue;
        }

        VirtioNetHdr hdr;
        decodeVirtioNetHdr(&hdr, sc->readBuf);

        u8* inner = sc->readBuf + VIRTIO_NET_HDR_LEN;
        size_t innerLen = (size_t)n - VIRTIO_NET_HDR_LEN;

        u32 dstVip = 0;
        if (!dstFromIPv4(inner, innerLen, &dstVip)) {
            continue;
        }

        auto conn = conns->lookup(dstVip);
        if (!conn) {
            continue;
        }

        if (hdr.gsoType == VIRTIO_NET_HDR_GSO_NONE) {
            if (hdr.flags & VIRTIO_NET_HDR_F_NEEDS_CSUM) {
                gsoNoneChecksum(inner, innerLen, hdr.csumStart, hdr.csumOffset);
            }
            sendOne(conn, inner, innerLen);
            continue;
        }

        if (hdr.gsoType == VIRTIO_NET_HDR_GSO_TCPV4) {
            int segs = gsoSplit(inner, innerLen, hdr,
                                sc->segPtrs, sc->segSizes,
                                TunReaderScratch::MAX_SEGS);
            if (segs < 0) {
                warn(StringView(u8"gsoSplit: too many segments / unsupported gso_type"));
                continue;
            }

            sendUso(conn, sc, segs);
            continue;
        }

        // TUNSETOFFLOAD is CSUM|TSO4, so UDP/TCPV6 shouldn't arrive.
        warn(StringView(u8"tunReader: unsupported gso_type"));
    }
}

void gofra::udpReader(int udpFd, int tunFd, UdpReaderScratch* sc) {
    for (;;) {
        // MSG_WAITFORONE blocks for the first, drains the rest.
        int n = ::recvmmsg(udpFd, sc->msgs, UdpReaderScratch::BATCH, MSG_WAITFORONE, nullptr);

        if (n < 0) {
            warnErrno(StringView(u8"udp recvmmsg"), errno);
            continue;
        }

        for (int i = 0; i < n; ++i) {
            u32 payloadLen = sc->msgs[i].msg_len;

            if (payloadLen < 20) {
                continue;
            }

            // recvmmsg shrunk namelen to the actual sender; reset.
            sc->msgs[i].msg_hdr.msg_namelen = sizeof(sockaddr_storage);

            // [10 B zero virtio_net_hdr | payload] → TUN.
            ssize_t w = ::write(tunFd, sc->bufs[i], VIRTIO_NET_HDR_LEN + payloadLen);

            if (w < 0) {
                warnErrno(StringView(u8"tun write"), errno);
            }
        }
    }
}
