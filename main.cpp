// gofra2 — C++ data plane on top of std/.
//
// Plain pthreads, blocking syscalls, no coroutines. Each TUN queue
// gets a dedicated tunReader thread (read → gsoSplit → per-segment
// sendto via stripe); each UDP socket gets a dedicated udpReader
// thread (recvmmsg(64) → write each to the paired TUN queue). N is
// the local underlay count (== self->dstCount()). This matches Go
// gofra1's runtime shape and lets us amortize RX syscalls via
// recvmmsg, which is the main reason gofra1 reaches 3 Gbps where
// our coroutine + io_uring per-packet variant capped at 1 Gbps.

#include "config.h"
#include "conn.h"
#include "peer.h"
#include "plane.h"
#include "tun.h"

#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/thr/runable.h>
#include <std/thr/thread.h>
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

        // Open N TUN queues paired with the N UDP sockets by index.
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

        // 2*N OS threads, paired by index. Scratch buffers are
        // pool-allocated up-front (single-threaded — pool isn't
        // thread-safe), runables go on the heap with self-delete via
        // makeRunablePtr so they survive the spawn call.
        Vector<Thread*> threads;
        for (size_t i = 0; i < n; ++i) {
            int tunFd = tunFds[i];
            int srcFd = conns->srcFd(i);

            auto* ts = makeTunReaderScratch(pool.mutPtr());
            auto* us = makeUdpReaderScratch(pool.mutPtr());

            auto* tr = makeRunablePtr([tunFd, conns, ts] {
                tunReader(tunFd, conns, ts);
            });

            auto* ur = makeRunablePtr([srcFd, tunFd, us] {
                udpReader(srcFd, tunFd, us);
            });

            threads.pushBack(Thread::create(pool.mutPtr(), *tr));
            threads.pushBack(Thread::create(pool.mutPtr(), *ur));
        }

        // Park forever — threads loop until process death; SIGINT/
        // SIGTERM kills us and pid1 reaps the worker threads.
        for (size_t i = 0; i < threads.length(); ++i) {
            threads[i]->join();
        }

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
