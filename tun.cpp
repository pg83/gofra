#include "tun.h"

#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/str/view.h>
#include <std/str/builder.h>
#include <std/sys/crt.h>
#include <std/sys/fd.h>
#include <std/sys/throw.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/rtnetlink.h>

#include <libmnl/libmnl.h>

using namespace stl;

namespace {
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

    int ifIndexFor(const char* dev) {
        int rawS = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (rawS < 0) {
            Errno().raise(StringBuilder() << StringView(u8"socket(AF_INET,SOCK_DGRAM)"));
        }

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

    struct Netlink {
        mnl_socket* nl;
        u32 portId;
        u32 seq;

        Netlink();

        ~Netlink() noexcept {
            mnl_socket_close(nl);
        }

        Netlink(const Netlink&) = delete;
        Netlink& operator=(const Netlink&) = delete;

        u32 nextSeq() noexcept {
            return ++seq;
        }

        void run(nlmsghdr* nh, StringView what);
        void setMtu(int idx, int mtu);
        void addAddr(int idx, u32 vip, u8 prefixLen);
        void linkUp(int idx);
    };

}

// SIOCSIF* silently drops egress on a /24 P2P TUN; use rtnetlink.
void gofra::configureTun(const char* dev, int mtu, u32 vip, u8 prefixLen) {
    int idx = ifIndexFor(dev);

    Netlink nl;
    nl.setMtu(idx, mtu);
    nl.addAddr(idx, vip, prefixLen);
    nl.linkUp(idx);
}

Netlink::Netlink() {
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

void Netlink::run(nlmsghdr* nh, StringView what) {
    if (mnl_socket_sendto(nl, nh, nh->nlmsg_len) < 0) {
        Errno().raise(StringBuilder() << what
                      << StringView(u8": mnl_socket_sendto"));
    }

    alignas(u32) char rbuf[NL_BUF];

    ssize_t n = mnl_socket_recvfrom(nl, rbuf, sizeof(rbuf));

    if (n < 0) {
        Errno().raise(StringBuilder() << what << StringView(u8": mnl_socket_recvfrom"));
    }

    int rc = mnl_cb_run(rbuf, (size_t)n, nh->nlmsg_seq, portId, nullptr, nullptr);

    if (rc < 0) {
        Errno().raise(StringBuilder() << what << StringView(u8": netlink ack"));
    }
}

void Netlink::setMtu(int idx, int mtu) {
    alignas(u32) char buf[NL_BUF];

    nlmsghdr* nh = mnl_nlmsg_put_header(buf);

    nh->nlmsg_type = RTM_NEWLINK;
    nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nh->nlmsg_seq = nextSeq();

    ifinfomsg* ifi = (ifinfomsg*)mnl_nlmsg_put_extra_header(nh, sizeof(ifinfomsg));

    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = idx;
    ifi->ifi_flags = 0;
    ifi->ifi_change = 0;

    mnl_attr_put_u32(nh, IFLA_MTU, (u32)mtu);

    run(nh, StringView(u8"RTM_NEWLINK MTU"));
}

void Netlink::addAddr(int idx, u32 vip, u8 prefixLen) {
    alignas(u32) char buf[NL_BUF];
    nlmsghdr* nh = mnl_nlmsg_put_header(buf);
    nh->nlmsg_type = RTM_NEWADDR;
    nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
    nh->nlmsg_seq = nextSeq();

    ifaddrmsg* ifa = (ifaddrmsg*)mnl_nlmsg_put_extra_header(nh, sizeof(ifaddrmsg));
    ifa->ifa_family = AF_INET;
    ifa->ifa_prefixlen = prefixLen;
    ifa->ifa_flags = 0;
    ifa->ifa_scope = RT_SCOPE_UNIVERSE;
    ifa->ifa_index = (u32)idx;

    u32 vipNet = htonl(vip);
    mnl_attr_put_u32(nh, IFA_LOCAL, vipNet);
    mnl_attr_put_u32(nh, IFA_ADDRESS, vipNet);

    if (prefixLen < 32) {
        u32 hostMask = (prefixLen == 0)
            ? 0xFFFFFFFFu
            : ((1u << (32 - prefixLen)) - 1u);
        u32 brd = htonl(vip | hostMask);
        mnl_attr_put_u32(nh, IFA_BROADCAST, brd);
    }

    run(nh, StringView(u8"RTM_NEWADDR"));
}

void Netlink::linkUp(int idx) {
    alignas(u32) char buf[NL_BUF];
    nlmsghdr* nh = mnl_nlmsg_put_header(buf);
    nh->nlmsg_type = RTM_NEWLINK;
    nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nh->nlmsg_seq = nextSeq();

    ifinfomsg* ifi = (ifinfomsg*)mnl_nlmsg_put_extra_header(nh, sizeof(ifinfomsg));
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = idx;
    ifi->ifi_flags = IFF_UP;
    ifi->ifi_change = IFF_UP;

    run(nh, StringView(u8"RTM_NEWLINK UP"));
}

int gofra::openTun(ObjPool* pool, const char* dev) {
    int fd = ::open("/dev/net/tun", O_RDWR | O_CLOEXEC);

    if (fd < 0) {
        Errno().raise(StringBuilder() << StringView(u8"open /dev/net/tun"));
    }

    pool->make<ScopedFD>(fd);

    ifreq ifr = {};
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI | IFF_MULTI_QUEUE | IFF_VNET_HDR;
    copyIfName(ifr.ifr_name, StringView(dev));

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"TUNSETIFF ") << StringView(dev));
    }

    int hdrSize = 10;  // gso.cpp assumes 10-byte virtio_net_hdr

    if (ioctl(fd, TUNSETVNETHDRSZ, &hdrSize) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"TUNSETVNETHDRSZ ") << StringView(dev));
    }

    // Without offload, gso_type is always NONE → syscall/segment.
    unsigned long off = TUN_F_CSUM | TUN_F_TSO4;

    if (ioctl(fd, TUNSETOFFLOAD, off) < 0) {
        Errno().raise(StringBuilder() << StringView(u8"TUNSETOFFLOAD ") << StringView(dev));
    }

    return fd;
}
