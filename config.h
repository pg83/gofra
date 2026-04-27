#pragma once

#include <std/sys/types.h>

namespace stl {
    class ObjPool;
    class StringView;
}

namespace gofra {
    struct Peer;
    struct PeerTable;

    struct Config {
        u32 tunVip;       // our overlay VIP, host byte order
        u8 tunPrefixLen;  // CIDR prefix
        int tunMtu;
        const char* tunDev;  // NUL-terminated; copied into ifr_name for ioctls

        PeerTable* peers;  // every cluster row, us included
        Peer* self;        // peers->lookup(tunVip) — our row, used for binding

        int udpRecvBuf;
        int udpSendBuf;
    };

    Config* loadConfig(stl::ObjPool* pool, stl::StringView path);
}
