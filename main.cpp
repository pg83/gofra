// gofra2 — C++ data plane on top of std/.
//
// Phase 1 skeleton: single TUN queue, single UDP socket, no
// virtio_net_hdr / GSO. Validates that std/'s io_uring reactor
// handles the TUN char device correctly. Phase 2 widens to
// multi-queue + IFF_VNET_HDR + gsoSplit for parity with gofra1.

#include "config.h"
#include "tun.h"
#include "socks.h"
#include "peer.h"
#include "plane.h"

#include <std/mem/obj_pool.h>
#include <std/thr/coro.h>
#include <std/thr/io_reactor.h>
#include <std/sys/throw.h>
#include <std/str/view.h>
#include <std/ios/sys.h>

#include <stdlib.h>
#include <signal.h>

using namespace stl;
using namespace gofra;

namespace {
    [[noreturn]] void usage() {
        sysE << StringView(u8"usage: gofra2 --config /path/to/config.ini") << endL << flsH;
        exit(2);
    }

    int run(int argc, char** argv) {
        StringView configPath;

        for (int i = 1; i < argc; ++i) {
            StringView arg(argv[i]);

            if (arg == StringView(u8"--config") && i + 1 < argc) {
                configPath = StringView(argv[++i]);
            } else {
                usage();
            }
        }

        if (configPath.empty()) {
            usage();
        }

        // SIGPIPE on a closed UDP socket would terminate the process.
        signal(SIGPIPE, SIG_IGN);

        auto pool = ObjPool::fromMemory();
        auto cfg = loadConfig(pool.mutPtr(), configPath);

        int tunFd = openTun(cfg->tunDev, cfg->tunMtu, cfg->tunVip, cfg->tunPrefixLen);
        int udpFd = openUdpSocket(&cfg->underlay[0], cfg->udpRecvBuf, cfg->udpSendBuf);

        sysE << StringView(u8"gofra2: tun=") << StringView(cfg->tunDev)
             << StringView(u8" mtu=") << (u64)cfg->tunMtu
             << StringView(u8" peers=") << (u64)cfg->peers.length()
             << endL;

        auto peers = pool->make<PeerTable>(pool.mutPtr());
        peers->load(*cfg);

        auto exec = CoroExecutor::create(pool.mutPtr(), 8);
        auto reactor = exec->io();

        // 32 KiB stack: tunReader / udpReader carry a 10 KiB packet buffer
        // on the coroutine stack and call into io_uring + std/ formatting
        // helpers, which together can use a few KiB of frame space.
        constexpr size_t stackSize = 32 * 1024;

        exec->spawnRun(SpawnParams().setStackSize(stackSize).setRunable([&] {
            tunReader(reactor, tunFd, udpFd, peers, cfg->tunMtu);
        }));

        exec->spawnRun(SpawnParams().setStackSize(stackSize).setRunable([&] {
            udpReader(reactor, udpFd, tunFd);
        }));

        exec->join();
        return 0;
    }
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (Exception& e) {
        sysE << e.description() << endL << flsH;
        return 1;
    } catch (...) {
        sysE << StringView(u8"gofra2: unknown exception") << endL << flsH;
        return 1;
    }
}
