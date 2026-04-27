#include "tun.h"

#include <std/lib/buffer.h>
#include <std/str/view.h>
#include <std/str/builder.h>
#include <std/sys/throw.h>

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/if.h>
#include <linux/if_tun.h>

using namespace stl;

namespace {
    // ioctl-based netdev config — netlink would be cleaner but a
    // bigger dep. ioctl handles the IPv4-only paths we need.
    int ctlSock() {
        int s = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) {
            Errno().raise(StringBuilder() << StringView(u8"socket(AF_INET) for ifconfig"));
        }
        return s;
    }

    void setMtu(const char* dev, int mtu) {
        int s = ctlSock();
        ifreq ifr = {};
        strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);
        ifr.ifr_mtu = mtu;
        if (ioctl(s, SIOCSIFMTU, &ifr) < 0) {
            int e = errno;
            ::close(s);
            Errno(e).raise(StringBuilder() << StringView(u8"SIOCSIFMTU ") << StringView(dev));
        }
        ::close(s);
    }

    void setAddr(const char* dev, u32 vip) {
        int s = ctlSock();
        ifreq ifr = {};
        strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);

        sockaddr_in* sa = (sockaddr_in*)&ifr.ifr_addr;
        sa->sin_family = AF_INET;
        sa->sin_addr.s_addr = htonl(vip);

        if (ioctl(s, SIOCSIFADDR, &ifr) < 0) {
            int e = errno;
            ::close(s);
            Errno(e).raise(StringBuilder() << StringView(u8"SIOCSIFADDR ") << StringView(dev));
        }
        ::close(s);
    }

    void setNetmask(const char* dev, u8 prefixLen) {
        int s = ctlSock();
        ifreq ifr = {};
        strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);

        u32 mask = prefixLen == 0 ? 0 : (0xFFFFFFFFu << (32 - prefixLen));
        sockaddr_in* sa = (sockaddr_in*)&ifr.ifr_netmask;
        sa->sin_family = AF_INET;
        sa->sin_addr.s_addr = htonl(mask);

        if (ioctl(s, SIOCSIFNETMASK, &ifr) < 0) {
            int e = errno;
            ::close(s);
            Errno(e).raise(StringBuilder() << StringView(u8"SIOCSIFNETMASK ") << StringView(dev));
        }
        ::close(s);
    }

    void linkUp(const char* dev) {
        int s = ctlSock();
        ifreq ifr = {};
        strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);

        if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) {
            int e = errno;
            ::close(s);
            Errno(e).raise(StringBuilder() << StringView(u8"SIOCGIFFLAGS ") << StringView(dev));
        }
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
            int e = errno;
            ::close(s);
            Errno(e).raise(StringBuilder() << StringView(u8"SIOCSIFFLAGS ") << StringView(dev));
        }
        ::close(s);
    }
}

int gofra::openTun(const char* dev, int mtu, u32 vip, u8 prefixLen) {
    int fd = ::open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        Errno().raise(StringBuilder() << StringView(u8"open /dev/net/tun"));
    }

    ifreq ifr = {};
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        int e = errno;
        ::close(fd);
        Errno(e).raise(StringBuilder() << StringView(u8"TUNSETIFF ") << StringView(dev));
    }

    setMtu(dev, mtu);
    setAddr(dev, vip);
    setNetmask(dev, prefixLen);
    linkUp(dev);

    return fd;
}
