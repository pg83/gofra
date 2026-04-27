#pragma once

#include <std/sys/types.h>

struct sockaddr;

namespace stl {
    class ObjPool;
}

namespace gofra {
    struct Peer;
    struct PeerTable;

    // One slot in a Conn's stripe array; srcFd + dstAddr feed
    // sendmsg. srcAddr is informational.
    struct ConnSlot {
        const sockaddr* srcAddr;
        int srcFd;
        const sockaddr* dstAddr;
    };

    // Per-peer state: N*M precomputed (src,dst) slots + atomic rr
    // counter walking them round-robin.
    struct Conn {
        virtual u32 vip() const noexcept = 0;
        virtual const ConnSlot* next() noexcept = 0;
    };

    // vip → Conn for every peer != self; also owns the N local UDP
    // src sockets (pool-tracked via ScopedFD).
    struct ConnTable {
        virtual size_t size() const noexcept = 0;
        virtual size_t srcCount() const noexcept = 0;
        virtual int srcFd(size_t i) const noexcept = 0;
        virtual Conn* lookup(u32 vip) const noexcept = 0;

        // Opens N=self->dstCount() UDP sockets, builds a Conn per
        // non-self peer with N*M slots.
        static ConnTable* create(stl::ObjPool* pool, PeerTable* peers, Peer* self, int rcvBuf, int sndBuf);
    };
}
