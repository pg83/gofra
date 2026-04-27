// gofra2 — C++ data plane on top of std/.
//
// Phase 1 skeleton: single TUN queue, single UDP socket, no
// virtio_net_hdr / GSO. Validates that std/'s io_uring reactor
// handles the TUN char device correctly. Phase 2 widens to
// multi-queue + IFF_VNET_HDR + gsoSplit for parity with gofra1.

#include "config.h"
#include "conn.h"
#include "peer.h"
#include "plane.h"
#include "tun.h"

#include <std/lib/vector.h>
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

        // ConnTable opens N=self->dstCount() UDP sockets internally
        // and pre-builds the N*M stripe slots per remote peer.
        auto* conns = ConnTable::create(pool.mutPtr(), cfg->peers, cfg->self,
                                        cfg->udpRecvBuf, cfg->udpSendBuf);

        // Open N TUN queues (paired with the N UDP sockets by index).
        // Iface-level mtu/addr/up runs once after all queues attach.
        size_t n = conns->srcCount();
        Vector<int> tunFds;
        for (size_t i = 0; i < n; ++i) {
            tunFds.pushBack(openTun(pool.mutPtr(), cfg->tunDev));
        }
        configureTun(cfg->tunDev, cfg->tunMtu, cfg->tunVip, cfg->tunPrefixLen);

        sysE << StringView(u8"gofra2: tun=") << StringView(cfg->tunDev)
             << StringView(u8" mtu=") << (u64)cfg->tunMtu
             << StringView(u8" queues=") << (u64)n
             << StringView(u8" peers=") << (u64)conns->size()
             << endL;

        auto exec = CoroExecutor::create(pool.mutPtr(), 8);
        auto reactor = exec->io();

        // 32 KiB stack: tunReader / udpReader carry a 10 KiB packet buffer
        // on the coroutine stack and call into io_uring + std/ formatting
        // helpers, which together can use a few KiB of frame space.
        constexpr size_t stackSize = 32 * 1024;

        // N tunReader + N udpReader, paired by index. Allocate
        // per-coroutine GSO scratch up front (single-threaded; pool
        // isn't thread-safe), then hand pointers to the lambdas.
        for (size_t i = 0; i < n; ++i) {
            int tunFd = tunFds[i];
            int srcFd = conns->srcFd(i);
            auto* scratch = makeTunReaderScratch(pool.mutPtr());

            exec->spawnRun(SpawnParams().setStackSize(stackSize).setRunable(
                [reactor, tunFd, conns, scratch] {
                    tunReader(reactor, tunFd, conns, scratch);
                }));

            exec->spawnRun(SpawnParams().setStackSize(stackSize).setRunable(
                [reactor, srcFd, tunFd] {
                    udpReader(reactor, srcFd, tunFd);
                }));
        }

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
