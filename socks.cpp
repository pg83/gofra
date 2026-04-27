#include "socks.h"
#include "addr.h"

#include <std/sys/fd.h>
#include <std/str/view.h>
#include <std/sys/throw.h>
#include <std/str/builder.h>
#include <std/mem/obj_pool.h>

#include <errno.h>
#include <unistd.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

using namespace stl;

namespace {
    struct IfAddrs {
        ifaddrs* ifs = nullptr;

        ~IfAddrs() noexcept {
            if (ifs) {
                freeifaddrs(ifs);
            }
        }
    };

    // SO_BINDTODEVICE: TX must leave on the NIC owning src, not what routing picks.
    StringView ifaceFor(ObjPool* pool, u32 addr_ne) {
        auto* holder = pool->make<IfAddrs>();

        if (getifaddrs(&holder->ifs) != 0) {
            Errno().raise(StringBuilder() << StringView(u8"getifaddrs"));
        }

        for (ifaddrs* p = holder->ifs; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) {
                continue;
            }

            sockaddr_in* sa = (sockaddr_in*)p->ifa_addr;

            if (sa->sin_addr.s_addr == addr_ne) {
                StringView name(p->ifa_name);

                if (name.length() >= IFNAMSIZ) {
                    name = name.prefix(IFNAMSIZ - 1);
                }

                return name;
            }
        }

        char tmp[INET_ADDRSTRLEN] = {0};

        inet_ntop(AF_INET, &addr_ne, tmp, sizeof(tmp));

        Errno(0).raise(StringBuilder()
                       << StringView(u8"no iface owns ")
                       << StringView(tmp));
    }

    void setBufForce(int fd, int opt, int sz, StringView name) {
        if (::setsockopt(fd, SOL_SOCKET, opt, &sz, sizeof(sz)) < 0) {
            Errno().raise(StringBuilder() << name);
        }
    }
}

int gofra::openUdpSocket(ObjPool* pool, const sockaddr* src, int rcvBuf, int sndBuf) {
    int fd = ::socket(src->sa_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        Errno().raise(StringBuilder() << StringView(u8"socket(UDP)"));
    }

    pool->make<ScopedFD>(fd);

    auto* sin = (const sockaddr_in*)src;

    StringView name = ifaceFor(pool, sin->sin_addr.s_addr);

    if (::setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name.data(), name.length()) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"SO_BINDTODEVICE ") << name);
    }

    setBufForce(fd, SO_RCVBUFFORCE, rcvBuf, StringView(u8"SO_RCVBUFFORCE"));
    setBufForce(fd, SO_SNDBUFFORCE, sndBuf, StringView(u8"SO_SNDBUFFORCE"));

    int yes = 1;

    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"SO_REUSEADDR"));
    }

    if (::bind(fd, src, addrLen(src)) < 0) {
        char buf[INET_ADDRSTRLEN] = {0};

        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));

        Errno().raise(StringBuilder()
                      << StringView(u8"bind ")
                      << StringView(buf)
                      << StringView(u8":")
                      << (u64)ntohs(sin->sin_port));
    }

    return fd;
}
