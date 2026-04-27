#include "tun.h"

#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/str/view.h>
#include <std/str/builder.h>
#include <std/sys/crt.h>
#include <std/sys/fd.h>
#include <std/sys/throw.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/rtnetlink.h>
#include <libmnl/libmnl.h>

using namespace stl;

namespace {
    // Stack buffer for one netlink request. 8 KiB covers any rtnetlink
    // message we issue (RTM_NEWLINK / RTM_NEWADDR with a handful of
    // attrs); the alignas keeps `nlmsghdr` happy.
    constexpr size_t NL_BUF = 8192;

    void copyIfName(char dst[IFNAMSIZ], StringView name) {
        if (name.length() >= IFNAMSIZ) {
            Errno(0).raise(StringBuilder()
                           << StringView(u8"iface name too long: ")
                           << name);
        }
        memCpy(dst, name.data(), name.length());
        dst[name.length()] = 0;
    }

    // Look up the kernel iface index by name. SIOCGIFINDEX is a getter,
    // not a setter — it doesn't suffer from the SIOCSIF* P2P-route bug
    // we worked around by going to netlink for SET ops.
    int ifIndexFor(const char* dev) {
        int rawS = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (rawS < 0) {
            Errno().raise(StringBuilder() << StringView(u8"socket(AF_INET,SOCK_DGRAM)"));
        }

        // Function-scoped fd: stack RAII via stl::ScopedFD closes it on
        // any return / throw without bothering the caller's pool.
        ScopedFD s(rawS);

        ifreq ifr = {};
        copyIfName(ifr.ifr_name, StringView(dev));

        if (ioctl(s.get(), SIOCGIFINDEX, &ifr) < 0) {
            Errno().raise(StringBuilder()
                          << StringView(u8"SIOCGIFINDEX ")
                          << StringView(dev));
        }

        return ifr.ifr_ifindex;
    }

    // Thin RAII wrapper around an mnl_socket bound to NETLINK_ROUTE.
    // run(nh, what) sends the request and consumes the ACK; throws on
    // any failure with `what` as a tag.
    struct Netlink {
        mnl_socket* nl;
        u32 portId;
        u32 seq;

        Netlink() {
            nl = mnl_socket_open(NETLINK_ROUTE);
            if (!nl) {
                Errno().raise(StringBuilder() << StringView(u8"mnl_socket_open"));
            }
            if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
                int e = errno;
                mnl_socket_close(nl);
                Errno(e).raise(StringBuilder() << StringView(u8"mnl_socket_bind"));
            }
            portId = mnl_socket_get_portid(nl);
            seq = 0;
        }

        ~Netlink() noexcept {
            mnl_socket_close(nl);
        }

        Netlink(const Netlink&) = delete;
        Netlink& operator=(const Netlink&) = delete;

        u32 nextSeq() noexcept {
            return ++seq;
        }

        void run(nlmsghdr* nh, StringView what) {
            if (mnl_socket_sendto(nl, nh, nh->nlmsg_len) < 0) {
                Errno().raise(StringBuilder() << what
                              << StringView(u8": mnl_socket_sendto"));
            }

            alignas(u32) char rbuf[NL_BUF];
            ssize_t n = mnl_socket_recvfrom(nl, rbuf, sizeof(rbuf));
            if (n < 0) {
                Errno().raise(StringBuilder() << what
                              << StringView(u8": mnl_socket_recvfrom"));
            }

            int rc = mnl_cb_run(rbuf, (size_t)n, nh->nlmsg_seq, portId, nullptr, nullptr);
            if (rc < 0) {
                Errno().raise(StringBuilder() << what
                              << StringView(u8": netlink ack"));
            }
        }
    };

    void setMtu(Netlink& nl, int idx, int mtu) {
        alignas(u32) char buf[NL_BUF];
        nlmsghdr* nh = mnl_nlmsg_put_header(buf);
        nh->nlmsg_type = RTM_NEWLINK;
        nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
        nh->nlmsg_seq = nl.nextSeq();

        ifinfomsg* ifi = (ifinfomsg*)mnl_nlmsg_put_extra_header(nh, sizeof(ifinfomsg));
        ifi->ifi_family = AF_UNSPEC;
        ifi->ifi_index = idx;
        ifi->ifi_flags = 0;
        ifi->ifi_change = 0;

        mnl_attr_put_u32(nh, IFLA_MTU, (u32)mtu);

        nl.run(nh, StringView(u8"RTM_NEWLINK MTU"));
    }

    void addAddr(Netlink& nl, int idx, u32 vip, u8 prefixLen) {
        alignas(u32) char buf[NL_BUF];
        nlmsghdr* nh = mnl_nlmsg_put_header(buf);
        nh->nlmsg_type = RTM_NEWADDR;
        nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
        nh->nlmsg_seq = nl.nextSeq();

        ifaddrmsg* ifa = (ifaddrmsg*)mnl_nlmsg_put_extra_header(nh, sizeof(ifaddrmsg));
        ifa->ifa_family = AF_INET;
        ifa->ifa_prefixlen = prefixLen;
        ifa->ifa_flags = 0;
        ifa->ifa_scope = RT_SCOPE_UNIVERSE;
        ifa->ifa_index = (u32)idx;

        // host-order vip → network-order on the wire.
        u32 vipNet = htonl(vip);
        mnl_attr_put_u32(nh, IFA_LOCAL, vipNet);
        mnl_attr_put_u32(nh, IFA_ADDRESS, vipNet);

        // Broadcast = vip | host-mask. Skip for /32 (no broadcast).
        if (prefixLen < 32) {
            u32 hostMask = (prefixLen == 0)
                ? 0xFFFFFFFFu
                : ((1u << (32 - prefixLen)) - 1u);
            u32 brd = htonl(vip | hostMask);
            mnl_attr_put_u32(nh, IFA_BROADCAST, brd);
        }

        nl.run(nh, StringView(u8"RTM_NEWADDR"));
    }

    void linkUp(Netlink& nl, int idx) {
        alignas(u32) char buf[NL_BUF];
        nlmsghdr* nh = mnl_nlmsg_put_header(buf);
        nh->nlmsg_type = RTM_NEWLINK;
        nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
        nh->nlmsg_seq = nl.nextSeq();

        ifinfomsg* ifi = (ifinfomsg*)mnl_nlmsg_put_extra_header(nh, sizeof(ifinfomsg));
        ifi->ifi_family = AF_UNSPEC;
        ifi->ifi_index = idx;
        ifi->ifi_flags = IFF_UP;
        ifi->ifi_change = IFF_UP;

        nl.run(nh, StringView(u8"RTM_NEWLINK UP"));
    }

    // Configure addr/mtu/up via rtnetlink. Mirrors what gofra1's
    // vishvananda/netlink does — proper NEWADDR with all the right
    // attrs (IFA_LOCAL, IFA_ADDRESS, IFA_BROADCAST, prefixlen, scope).
    // The legacy SIOCSIF* ioctl path silently drops egress on a /24
    // P2P TUN even though the route table looks right.
    void configure(const char* dev, int mtu, u32 vip, u8 prefixLen) {
        int idx = ifIndexFor(dev);

        Netlink nl;
        setMtu(nl, idx, mtu);
        addAddr(nl, idx, vip, prefixLen);
        linkUp(nl, idx);
    }
}

int gofra::openTun(ObjPool* pool, const char* dev, int mtu, u32 vip, u8 prefixLen) {
    int fd = ::open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        Errno().raise(StringBuilder() << StringView(u8"open /dev/net/tun"));
    }

    // Hand fd ownership to the pool — any throw below leaves a live
    // fd that the pool's destructor chain will reap on unwind.
    pool->make<ScopedFD>(fd);

    ifreq ifr = {};
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    copyIfName(ifr.ifr_name, StringView(dev));

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"TUNSETIFF ") << StringView(dev));
    }

    configure(dev, mtu, vip, prefixLen);

    return fd;
}
