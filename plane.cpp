#include "plane.h"
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

using namespace stl;
using namespace gofra;

// Per-tunReader scratch.
//
// Wireguard-go's tunReader sizes: one super-packet up to 64 KiB
// inner + 10 B virtio_net_hdr + slack, plus up to 64 segment
// buffers (MTU + slack) and a parallel pointer table so gsoSplit
// can address them as `u8* const*`.
struct gofra::TunReaderScratch {
    static constexpr size_t MAX_SEGS  = 64;
    static constexpr size_t SEG_SIZE  = 2048;            // MTU + slack
    static constexpr size_t READ_SIZE = 10 + 65535 + 128;

    u8 readBuf[READ_SIZE];
    u8 segBufs[MAX_SEGS][SEG_SIZE];
    u8* segPtrs[MAX_SEGS];
    size_t segSizes[MAX_SEGS];

    TunReaderScratch() noexcept {
        for (size_t i = 0; i < MAX_SEGS; ++i) {
            segPtrs[i] = segBufs[i];
        }
    }
};

// Per-udpReader scratch.
//
// 64-deep recvmmsg batch. Each slot's iov_base points 10 bytes
// into its buffer so the virtio_net_hdr prefix stays zero — when
// we hand the packet to the TUN it's read as gso_type=NONE / no
// flags. addr/iov/msghdr indices match buffer index.
struct gofra::UdpReaderScratch {
    static constexpr size_t BATCH    = 64;
    static constexpr size_t MSG_SIZE = 10 + 9000;        // virtio + jumbo

    mmsghdr msgs[BATCH];
    iovec iovs[BATCH];
    sockaddr_in addrs[BATCH];
    u8 bufs[BATCH][MSG_SIZE];

    UdpReaderScratch() noexcept {
        for (size_t i = 0; i < BATCH; ++i) {
            // Pre-zero the virtio_net_hdr prefix. recvmmsg writes
            // into iov_base which is buf + 10; the prefix stays
            // untouched and zero across iterations.
            zeroVirtioNetHdr(bufs[i]);

            iovs[i].iov_base = bufs[i] + VIRTIO_NET_HDR_LEN;
            iovs[i].iov_len  = MSG_SIZE - VIRTIO_NET_HDR_LEN;

            msgs[i].msg_hdr.msg_iov     = &iovs[i];
            msgs[i].msg_hdr.msg_iovlen  = 1;
            msgs[i].msg_hdr.msg_name    = &addrs[i];
            msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
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

    // Send one fully-formed inner IP packet via the next stripe slot.
    void sendOne(Conn* conn, const u8* pkt, size_t len) {
        auto slot = conn->next();

        ssize_t r = ::sendto(slot->srcFd, pkt, len, 0,
                             (const sockaddr*)slot->dstAddr,
                             sizeof(*slot->dstAddr));
        if (r < 0) {
            warnErrno(StringView(u8"udp sendto"), errno);
        }
    }
}

TunReaderScratch* gofra::makeTunReaderScratch(ObjPool* pool) {
    return pool->make<TunReaderScratch>();
}

UdpReaderScratch* gofra::makeUdpReaderScratch(ObjPool* pool) {
    return pool->make<UdpReaderScratch>();
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

            for (int i = 0; i < segs; ++i) {
                sendOne(conn, sc->segBufs[i], sc->segSizes[i]);
            }
            continue;
        }

        // Unsupported gso_type (UDP / TCPV6). With our TUNSETOFFLOAD
        // mask of CSUM|TSO4, the kernel shouldn't produce these.
        warn(StringView(u8"tunReader: unsupported gso_type"));
    }
}

void gofra::udpReader(int udpFd, int tunFd, UdpReaderScratch* sc) {
    for (;;) {
        // recvmmsg drains up to BATCH packets in one syscall;
        // MSG_WAITFORONE blocks until at least one is available,
        // then reaps everything else already queued.
        int n = ::recvmmsg(udpFd, sc->msgs, UdpReaderScratch::BATCH,
                           MSG_WAITFORONE, nullptr);
        if (n < 0) {
            warnErrno(StringView(u8"udp recvmmsg"), errno);
            continue;
        }

        for (int i = 0; i < n; ++i) {
            u32 payloadLen = sc->msgs[i].msg_len;
            if (payloadLen < 20) {
                continue;
            }

            // Reset namelen for the next round; recvmmsg shrinks it
            // to the actual sender addr len (16 for IPv4) but we
            // also rely on the iov_len being preserved across calls.
            sc->msgs[i].msg_hdr.msg_namelen = sizeof(sc->addrs[i]);

            // [10 B virtio_net_hdr (zeroed once at scratch ctor) | payload].
            ssize_t w = ::write(tunFd, sc->bufs[i],
                                VIRTIO_NET_HDR_LEN + payloadLen);
            if (w < 0) {
                warnErrno(StringView(u8"tun write"), errno);
            }
        }
    }
}
