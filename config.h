#pragma once

#include <std/sys/types.h>
#include <std/lib/vector.h>

#include <netinet/in.h>

namespace stl {
    class ObjPool;
    class StringView;
}

namespace gofra {
    struct Peer;  // peer.h owns the definition

    struct Config {
        u16 listenPort;
        u32 tunVip;       // inner IPv4 of `me` (host byte order)
        u8 tunPrefixLen;  // CIDR prefix
        int tunMtu;
        const char* tunDev;  // NUL-terminated; copied into ifr_name for ioctls

        stl::Vector<sockaddr_in> underlay;  // local underlay addrs (port = listenPort)
        stl::Vector<Peer*> peers;

        int udpRecvBuf;
        int udpSendBuf;
    };

    Config* loadConfig(stl::ObjPool* pool, stl::StringView path);
}
