#include "socks.h"
#include "addr.h"

#include <std/sys/fd.h>
#include <std/sys/crt.h>
#include <std/str/view.h>
#include <std/sys/throw.h>
#include <std/lib/buffer.h>
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
    // Find the iface whose primary IPv4 matches `addr_ne` (network
    // byte order). Used for SO_BINDTODEVICE — gofra TX must leave on
    // the NIC that owns the source IP, regardless of the routing
    // table's own pick. Writes the iface name into `out` (NUL-
    // terminated, length <= IFNAMSIZ-1) and returns the length.
    size_t ifaceFor(u32 addr_ne, char out[IFNAMSIZ]) {
        ifaddrs* ifs = nullptr;

        if (getifaddrs(&ifs) != 0) {
            Errno().raise(StringBuilder() << StringView(u8"getifaddrs"));
        }

        out[0] = 0;
        size_t outLen = 0;

        for (ifaddrs* p = ifs; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) {
                continue;
            }

            sockaddr_in* sa = (sockaddr_in*)p->ifa_addr;

            if (sa->sin_addr.s_addr == addr_ne) {
                StringView name(p->ifa_name);

                if (name.length() >= IFNAMSIZ) {
                    name = name.prefix(IFNAMSIZ - 1);
                }

                memCpy(out, name.data(), name.length());
                out[name.length()] = 0;
                outLen = name.length();

                break;
            }
        }

        freeifaddrs(ifs);

        if (!outLen) {
            char buf[INET_ADDRSTRLEN] = {0};

            inet_ntop(AF_INET, &addr_ne, buf, sizeof(buf));

            Errno(0).raise(StringBuilder()
                           << StringView(u8"no iface owns ")
                           << StringView(buf));
        }

        return outLen;
    }

    void setBufForce(int fd, int opt, int sz, StringView name) {
        if (::setsockopt(fd, SOL_SOCKET, opt, &sz, sizeof(sz)) < 0) {
            Errno().raise(StringBuilder() << name);
        }
    }
}

int gofra::openUdpSocket(ObjPool* pool, const sockaddr* src, int rcvBuf, int sndBuf) {
    // Blocking socket — udpReader pthreads block on recvmmsg with
    // MSG_WAITFORONE; non-blocking would just spin EAGAIN.
    int fd = ::socket(src->sa_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);

    if (fd < 0) {
        Errno().raise(StringBuilder() << StringView(u8"socket(UDP)"));
    }

    // Hand fd ownership to the pool — pool death triggers ::close.
    // From here on any throw still leaves a live fd that the pool's
    // destructor chain will reap when the surrounding scope unwinds.
    pool->make<ScopedFD>(fd);

    auto* sin = (const sockaddr_in*)src;

    char ifname[IFNAMSIZ];

    size_t ifnameLen = ifaceFor(sin->sin_addr.s_addr, ifname);

    if (::setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, ifnameLen) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"SO_BINDTODEVICE ") << StringView(ifname));
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
