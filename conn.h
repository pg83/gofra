#pragma once

#include <std/sys/types.h>

struct sockaddr;

namespace stl {
    class ObjPool;
}

namespace gofra {
    struct Peer;
    struct PeerTable;

    // One precomputed slot in a Conn's stripe array. srcAddr is purely
    // informational (the self underlay this slot's socket is bound on);
    // dstAddr + srcFd are what tunReader hands to sendto.
    struct ConnSlot {
        const sockaddr* srcAddr;
        int srcFd;
        const sockaddr* dstAddr;
    };

    // Per-peer outgoing connection state. Holds an N*M array of
    // pre-computed (srcFd, dstAddr) slots — N local underlays times M
    // remote underlays — and an atomic counter that walks the array
    // round-robin so consecutive packets fan out across every (src, dst)
    // pair.
    struct Conn {
        virtual u32 vip() const noexcept = 0;
        virtual const ConnSlot* next() noexcept = 0;
    };

    // ConnTable: vip → Conn for every peer except self. ConnTable also
    // owns the N source UDP sockets bound on self->dst(0..N-1) (each
    // is registered in the pool via stl::ScopedFD); main spawns one
    // udpReader per srcFd(i).
    struct ConnTable {
        virtual size_t size() const noexcept = 0;
        virtual size_t srcCount() const noexcept = 0;
        virtual int srcFd(size_t i) const noexcept = 0;
        virtual Conn* lookup(u32 vip) const noexcept = 0;

        // Open N=self->dstCount() UDP sockets via openUdpSocket
        // (pool-owned), then build a Conn for each peer != self with
        // N*M precomputed slots.
        static ConnTable* create(stl::ObjPool* pool, PeerTable* peers, Peer* self, int rcvBuf, int sndBuf);
    };
}
