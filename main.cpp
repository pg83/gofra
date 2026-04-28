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
        sysE << StringView(u8"usage: gofra --config /path/to/config.ini") << endL << flsH;
        exit(2);
    }

    void run(int argc, char** argv) {
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

        signal(SIGPIPE, SIG_IGN);

        auto pool = ObjPool::fromMemory();
        auto cfg = loadConfig(pool.mutPtr(), configPath);
        auto conns = ConnTable::create(pool.mutPtr(), cfg->peers, cfg->self, cfg->udpRecvBuf, cfg->udpSendBuf);
        size_t n = conns->srcCount();

        Vector<int> tunFds;

        for (size_t i = 0; i < n; ++i) {
            tunFds.pushBack(openTun(pool.mutPtr(), cfg->tunDev));
        }

        configureTun(cfg->tunDev, cfg->tunMtu, cfg->tunVip, cfg->tunPrefixLen);

        sysE << StringView(u8"gofra: tun=") << StringView(cfg->tunDev)
             << StringView(u8" mtu=") << (u64)cfg->tunMtu
             << StringView(u8" queues=") << (u64)n
             << StringView(u8" peers=") << (u64)conns->size()
             << endL;

        // Pool isn't thread-safe → pre-alloc scratch on this thread.
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

        for (size_t i = 0; i < threads.length(); ++i) {
            threads[i]->join();
        }
    }
}

int main(int argc, char** argv) {
    try {
        return (run(argc, argv), 0);
    } catch (Exception& e) {
        sysE << e.description() << endL << flsH;
    } catch (...) {
        sysE << StringView(u8"gofra: unknown exception") << endL << flsH;
    }

    return 1;
}
