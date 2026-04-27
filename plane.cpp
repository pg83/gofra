#include "plane.h"
#include "conn.h"
#include "gso.h"
#include "peer.h"

#include <std/mem/obj_pool.h>
#include <std/sys/crt.h>
#include <std/thr/io_reactor.h>
#include <std/str/view.h>
#include <std/ios/sys.h>

#include <string.h>
#include <stdint.h>
#include <netinet/in.h>

using namespace stl;
using namespace gofra;

// TunReaderScratch is a real type in the gofra namespace (forward-
// declared in plane.h) so main can pool->make one before spawning
// the coroutine — ObjPool isn't thread-safe, so all allocation has
// to happen on the main thread before any worker starts.
//
// Sized like wireguard-go's tunReader: one super-packet (up to 64
// KiB inner + 10 B virtio_net_hdr + slack) and up to 64 segment
// buffers (MTU + slack each), with a parallel pointer table so
// gsoSplit can address them as `u8* const*`.
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

namespace {
    constexpr u64 NEVER = UINT64_MAX;

    void warnErrno(StringView what, int err) {
        sysE << StringView(u8"gofra2: ") << what
             << StringView(u8": ")
             << (const char*)strerror(err)
             << endL;
    }

    void warn(StringView msg) {
        sysE << StringView(u8"gofra2: ") << msg << endL;
    }

    bool dstFromIPv4(const u8* pkt, size_t len, u32* out) noexcept {
        if (len < 20) {
            return false;
        }
        if ((pkt[0] >> 4) != 4) {
            return false;
        }
        *out = ((u32)pkt[16] << 24) | ((u32)pkt[17] << 16)
             | ((u32)pkt[18] <<  8) | (u32)pkt[19];
        return true;
    }

    // Send one fully-formed inner IP packet via the next stripe slot.
    void sendOne(IoReactor* reactor, Conn* conn, const u8* pkt, size_t len) {
        auto slot = conn->next();

        size_t sent = 0;
        int err = reactor->sendto(slot->srcFd, &sent, pkt, len,
                                  (const sockaddr*)slot->dstAddr,
                                  sizeof(*slot->dstAddr), NEVER);
        if (err) {
            warnErrno(StringView(u8"udp sendto"), err);
        }
    }
}

TunReaderScratch* gofra::makeTunReaderScratch(ObjPool* pool) {
    return pool->make<TunReaderScratch>();
}

void gofra::tunReader(IoReactor* reactor, int tunFd, ConnTable* conns,
                      TunReaderScratch* sc) {
    for (;;) {
        size_t n = 0;
        int err = reactor->read(tunFd, &n, sc->readBuf, sizeof(sc->readBuf), NEVER);

        if (err) {
            warnErrno(StringView(u8"tun read"), err);
            continue;
        }

        if (n < VIRTIO_NET_HDR_LEN + 20) {
            continue;
        }

        VirtioNetHdr hdr;
        decodeVirtioNetHdr(&hdr, sc->readBuf);

        u8* inner = sc->readBuf + VIRTIO_NET_HDR_LEN;
        size_t innerLen = n - VIRTIO_NET_HDR_LEN;

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
            sendOne(reactor, conn, inner, innerLen);
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
                sendOne(reactor, conn, sc->segBufs[i], sc->segSizes[i]);
            }
            continue;
        }

        // Unsupported gso_type (UDP / TCPV6). With our TUNSETOFFLOAD
        // mask of CSUM|TSO4, the kernel shouldn't produce these —
        // drop and log so we notice if the contract changes.
        warn(StringView(u8"tunReader: unsupported gso_type"));
    }
}

void gofra::udpReader(IoReactor* reactor, int udpFd, int tunFd) {
    // Layout: [10-byte virtio_net_hdr (kept zero)] [up to ~MTU payload].
    // Pre-zero the prefix once; the kernel's IFF_VNET_HDR side reads
    // gso_type=NONE / no flags / no csum work to do, then accepts the
    // following packet as fully-formed.
    u8 buf[VIRTIO_NET_HDR_LEN + 10000];
    zeroVirtioNetHdr(buf);

    for (;;) {
        size_t n = 0;
        sockaddr_in src = {};
        u32 srcLen = sizeof(src);

        int err = reactor->recvfrom(udpFd, &n,
                                    buf + VIRTIO_NET_HDR_LEN,
                                    sizeof(buf) - VIRTIO_NET_HDR_LEN,
                                    (sockaddr*)&src, &srcLen, NEVER);
        if (err) {
            warnErrno(StringView(u8"udp recvfrom"), err);
            continue;
        }

        if (n < 20) {
            continue;
        }

        size_t written = 0;
        err = reactor->write(tunFd, &written, buf, VIRTIO_NET_HDR_LEN + n, NEVER);
        if (err) {
            warnErrno(StringView(u8"tun write"), err);
        }
    }
}
