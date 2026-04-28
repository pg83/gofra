#pragma once

#include <std/sys/types.h>
#include <std/lib/visitor.h>

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
        u32 srcIdx;
        const sockaddr* dstAddr;
        u64 lastSeen;
    };

    struct Conn {
        virtual u32 vip() const noexcept = 0;
        virtual const ConnSlot* next(u64 nowMs) noexcept = 0;
    };

    struct ConnTable {
        virtual size_t size() const noexcept = 0;
        virtual size_t srcCount() const noexcept = 0;
        virtual int srcFd(size_t i) const noexcept = 0;
        virtual Conn* lookup(u32 vip) const noexcept = 0;

        // RX-side: look up the slot whose (srcIdx, dstAddr) matches the
        // incoming (our_src_socket_idx, peer_src_addr) — peer's TX slot
        // is symmetric to our TX slot.
        virtual ConnSlot* lookupSlot(u32 srcIdx, const sockaddr* peerSrc) const noexcept = 0;

        virtual void visitSlotsImpl(stl::VisitorFace&& v) = 0;

        template <typename V>
        void visitSlots(V v) {
            visitSlotsImpl(stl::makeVisitor([v](void* p) {
                v((ConnSlot*)p);
            }));
        }

        static ConnTable* create(stl::ObjPool* pool, PeerTable* peers, Peer* self,
                                 int rcvBuf, int sndBuf, u64 timeoutMs);
    };
}
