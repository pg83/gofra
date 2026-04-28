#include "socks.h"
#include "addr.h"

#include <std/sys/fd.h>
#include <std/str/view.h>
#include <std/sys/throw.h>
#include <std/str/builder.h>
#include <std/mem/obj_pool.h>

#include <errno.h>
#include <netdb.h>
#include <unistd.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>

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
    StringView ifaceFor(ObjPool* pool, const sockaddr* src) {
        auto* holder = pool->make<IfAddrs>();

        if (getifaddrs(&holder->ifs) != 0) {
            Errno().raise(StringBuilder() << StringView(u8"getifaddrs"));
        }

        u32 want = ((const sockaddr_in*)src)->sin_addr.s_addr;

        for (auto* p = holder->ifs; p; p = p->ifa_next) {
            auto* a = (sockaddr_in*)p->ifa_addr;

            if (a && a->sin_family == AF_INET && a->sin_addr.s_addr == want) {
                return StringView(p->ifa_name).prefix(IFNAMSIZ - 1);
            }
        }

        Errno(0).raise(StringBuilder() << StringView(u8"no iface owns src"));
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

    StringView name = ifaceFor(pool, src);

    if (::setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name.data(), name.length()) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"SO_BINDTODEVICE ") << name);
    }

    setBufForce(fd, SO_RCVBUFFORCE, rcvBuf, StringView(u8"SO_RCVBUFFORCE"));
    setBufForce(fd, SO_SNDBUFFORCE, sndBuf, StringView(u8"SO_SNDBUFFORCE"));

    int yes = 1;

    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"SO_REUSEADDR"));
    }

    if (::setsockopt(fd, SOL_UDP, UDP_GRO, &yes, sizeof(yes)) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"UDP_GRO"));
    }

    if (::bind(fd, src, addrLen(src)) < 0) {
        char host[INET6_ADDRSTRLEN] = {0};
        char serv[8] = {0};

        getnameinfo(src, addrLen(src), host, sizeof(host), serv, sizeof(serv),
                    NI_NUMERICHOST | NI_NUMERICSERV);

        Errno().raise(StringBuilder()
                      << StringView(u8"bind ")
                      << StringView(host)
                      << StringView(u8":")
                      << StringView(serv));
    }

    return fd;
}
