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
        u32 tunVip;
        u8 tunPrefixLen;
        int tunMtu;
        const char* tunDev;

        PeerTable* peers;
        Peer* self;

        int udpRecvBuf;
        int udpSendBuf;
    };

    Config* loadConfig(stl::ObjPool* pool, stl::StringView path);
}
