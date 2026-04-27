#pragma once

#include <std/sys/types.h>

struct sockaddr;

namespace stl {
    class ObjPool;
}

namespace gofra {
    struct Peer;
    struct PeerTable;

    struct ConnSlot {
        const sockaddr* srcAddr;
        int srcFd;
        const sockaddr* dstAddr;
    };

    struct Conn {
        virtual u32 vip() const noexcept = 0;
        virtual const ConnSlot* next() noexcept = 0;
    };

    struct ConnTable {
        virtual size_t size() const noexcept = 0;
        virtual size_t srcCount() const noexcept = 0;
        virtual int srcFd(size_t i) const noexcept = 0;
        virtual Conn* lookup(u32 vip) const noexcept = 0;

        static ConnTable* create(stl::ObjPool* pool, PeerTable* peers, Peer* self, int rcvBuf, int sndBuf);
    };
}
