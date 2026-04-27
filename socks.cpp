#include "socks.h"

#include <std/lib/buffer.h>
#include <std/str/view.h>
#include <std/str/builder.h>
#include <std/sys/throw.h>

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

using namespace stl;

namespace {
    // Find the iface whose primary IPv4 matches `addr_ne` (network
    // byte order). Used for SO_BINDTODEVICE — gofra TX must leave on
    // the NIC that owns the source IP, regardless of the routing
    // table's own pick.
    void ifaceFor(u32 addr_ne, char out[IFNAMSIZ]) {
        ifaddrs* ifs = nullptr;
        if (getifaddrs(&ifs) != 0) {
            Errno().raise(StringBuilder() << StringView(u8"getifaddrs"));
        }

        out[0] = 0;

        for (ifaddrs* p = ifs; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) {
                continue;
            }

            sockaddr_in* sa = (sockaddr_in*)p->ifa_addr;
            if (sa->sin_addr.s_addr == addr_ne) {
                size_t n = strnlen(p->ifa_name, IFNAMSIZ - 1);
                memcpy(out, p->ifa_name, n);
                out[n] = 0;
                break;
            }
        }

        freeifaddrs(ifs);

        if (!out[0]) {
            char buf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &addr_ne, buf, sizeof(buf));
            Errno(0).raise(StringBuilder()
                           << StringView(u8"no iface owns ")
                           << StringView(buf));
        }
    }

    void setBufForce(int fd, int opt, int sz, StringView name) {
        if (::setsockopt(fd, SOL_SOCKET, opt, &sz, sizeof(sz)) < 0) {
            Errno().raise(StringBuilder() << name);
        }
    }
}

int gofra::openUdpSocket(const sockaddr_in* src, int rcvBuf, int sndBuf) {
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        Errno().raise(StringBuilder() << StringView(u8"socket(UDP)"));
    }

    char ifname[IFNAMSIZ];
    ifaceFor(src->sin_addr.s_addr, ifname);

    if (::setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname)) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"SO_BINDTODEVICE ") << StringView(ifname));
    }

    setBufForce(fd, SO_RCVBUFFORCE, rcvBuf, StringView(u8"SO_RCVBUFFORCE"));
    setBufForce(fd, SO_SNDBUFFORCE, sndBuf, StringView(u8"SO_SNDBUFFORCE"));

    int yes = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"SO_REUSEADDR"));
    }

    if (::bind(fd, (sockaddr*)src, sizeof(*src)) < 0) {
        char buf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &src->sin_addr, buf, sizeof(buf));
        Errno().raise(StringBuilder()
                      << StringView(u8"bind ")
                      << StringView(buf)
                      << StringView(u8":")
                      << (u64)ntohs(src->sin_port));
    }

    return fd;
}
