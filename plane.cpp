#include "plane.h"
#include "peer.h"

#include <std/thr/io_reactor.h>
#include <std/str/view.h>
#include <std/ios/sys.h>

#include <string.h>
#include <stdint.h>
#include <netinet/in.h>

using namespace stl;
using namespace gofra;

namespace {
    constexpr u64 NEVER = UINT64_MAX;

    void warnErrno(StringView what, int err) {
        sysE << StringView(u8"gofra2: ") << what
             << StringView(u8": ")
             << (const char*)strerror(err)
             << endL;
    }
}

void gofra::tunReader(IoReactor* reactor, int tunFd, int udpFd, PeerTable* peers, int mtu) {
    // Per-coroutine scratch. Sized for one MTU plus a bit of headroom;
    // 10 KiB upper bound covers standard 9000-byte jumbo frames + slack.
    u8 buf[10000];
    size_t bufLen = (size_t)mtu + 128;

    if (bufLen > sizeof(buf)) {
        bufLen = sizeof(buf);
    }

    for (;;) {
        size_t n = 0;
        int err = reactor->read(tunFd, &n, buf, bufLen, NEVER);

        if (err) {
            warnErrno(StringView(u8"tun read"), err);
            continue;
        }

        if (n < 20) {
            continue;
        }

        u32 dstVip = 0;
        if (!dstFromIPv4(buf, n, &dstVip)) {
            continue;
        }

        auto peer = peers->lookup(dstVip);
        if (!peer) {
            continue;
        }

        const sockaddr_in* dst = pickDst(peer);

        size_t sent = 0;
        err = reactor->sendto(udpFd, &sent, buf, n, (const sockaddr*)dst, sizeof(*dst), NEVER);

        if (err) {
            warnErrno(StringView(u8"udp sendto"), err);
        }
    }
}

void gofra::udpReader(IoReactor* reactor, int udpFd, int tunFd) {
    u8 buf[10000];

    for (;;) {
        size_t n = 0;
        sockaddr_in src = {};
        u32 srcLen = sizeof(src);

        int err = reactor->recvfrom(udpFd, &n, buf, sizeof(buf), (sockaddr*)&src, &srcLen, NEVER);

        if (err) {
            warnErrno(StringView(u8"udp recvfrom"), err);
            continue;
        }

        if (n < 20) {
            continue;
        }

        size_t written = 0;
        err = reactor->write(tunFd, &written, buf, n, NEVER);

        if (err) {
            warnErrno(StringView(u8"tun write"), err);
        }
    }
}
