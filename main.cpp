#include "config.h"
#include "conn.h"
#include "peer.h"
#include "plane.h"
#include "tun.h"

#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/thr/runable.h>
#include <std/thr/thread.h>
#include <std/sys/throw.h>
#include <std/str/view.h>
#include <std/str/builder.h>
#include <std/ios/sys.h>

#include <pwd.h>
#include <grp.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

using namespace stl;
using namespace gofra;

namespace {
    [[noreturn]] void usage() {
        sysE << StringView(u8"usage: gofra --config /path/to/config.ini") << endL << flsH;
        exit(2);
    }

    void dropPrivs(const char* user) {
        auto* pw = getpwnam(user);

        if (!pw) {
            Errno(0).raise(StringBuilder() << StringView(u8"unknown user: ") << StringView(user));
        }

        if (setgroups(0, nullptr) != 0) {
            Errno().raise(StringBuilder() << StringView(u8"setgroups"));
        }

        if (setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) != 0) {
            Errno().raise(StringBuilder() << StringView(u8"setresgid"));
        }

        if (setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid) != 0) {
            Errno().raise(StringBuilder() << StringView(u8"setresuid"));
        }
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

        auto slave = ObjPool::fromMemory();
        auto* pool = ObjPool::fromHugePages(slave.mutPtr());

        auto cfg = loadConfig(pool, configPath);
        auto conns = ConnTable::create(pool, cfg->peers, cfg->self,
                                       cfg->udpRecvBuf, cfg->udpSendBuf,
                                       cfg->probeTimeoutMs);
        size_t n = conns->srcCount();

        Vector<int> tunFds;

        for (size_t i = 0; i < n; ++i) {
            tunFds.pushBack(openTun(pool, cfg->tunDev));
        }

        configureTun(cfg->tunDev, cfg->tunMtu, cfg->tunVip, cfg->tunPrefixLen);

        if (cfg->user) {
            dropPrivs(cfg->user);
        }

        sysE << StringView(u8"gofra: tun=") << StringView(cfg->tunDev)
             << StringView(u8" mtu=") << (u64)cfg->tunMtu
             << StringView(u8" queues=") << (u64)n
             << StringView(u8" peers=") << (u64)conns->size()
             << endL;

        // 2 MiB per thread stack so it sits on exactly one hugepage when fromHugePages succeeded; falls back to malloc-backed memory otherwise.
        constexpr size_t STACK_SIZE = (size_t)2 << 20;

        auto spawn = [&](Runable& r) {
            void* stack = pool->allocateOverAligned(STACK_SIZE, 4096);
            return Thread::create(pool, r, stack, STACK_SIZE);
        };

        // Pool isn't thread-safe → pre-alloc scratch on this thread.
        Vector<Thread*> threads;

        for (size_t i = 0; i < n; ++i) {
            int tunFd = tunFds[i];
            int srcFd = conns->srcFd(i);
            u32 srcIdx = (u32)i;

            auto* ts = makeTunReaderScratch(pool);
            auto* us = makeUdpReaderScratch(pool);

            auto* tr = makeRunablePtr([tunFd, conns, ts] {
                tunReader(tunFd, conns, ts);
            });

            auto* ur = makeRunablePtr([srcFd, tunFd, srcIdx, conns, us] {
                udpReader(srcFd, tunFd, srcIdx, conns, us);
            });

            threads.pushBack(spawn(*tr));
            threads.pushBack(spawn(*ur));
        }

        u64 probeIntervalMs = cfg->probeIntervalMs;
        u64 probeTimeoutMs = cfg->probeTimeoutMs;
        u64 statsIntervalSec = cfg->statsIntervalSec;

        auto* pr = makeRunablePtr([conns, probeIntervalMs] {
            prober(conns, probeIntervalMs);
        });

        auto* st = makeRunablePtr([conns, probeTimeoutMs, statsIntervalSec] {
            slotsStats(conns, probeTimeoutMs, statsIntervalSec);
        });

        threads.pushBack(spawn(*pr));
        threads.pushBack(spawn(*st));

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
