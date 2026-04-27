#pragma once

#include <std/sys/types.h>
#include <std/sys/atomic.h>
#include <std/sym/i_map.h>
#include <std/lib/vector.h>

#include <netinet/in.h>

namespace stl {
    class ObjPool;
}

namespace gofra {
    struct Config;

    // Peer is the dispatch entry for one remote VIP. `dsts` is the
    // pre-encoded list of underlay sockaddr_ins to stripe over;
    // `rr` is the per-peer round-robin counter (atomic, no lock).
    struct Peer {
        u32 vip;
        stl::Vector<sockaddr_in> dsts;
        u64 rr = 0;
    };

    class PeerTable {
        stl::IntMap<Peer*> byVip_;

    public:
        PeerTable(stl::ObjPool* pool)
            : byVip_(pool)
        {
        }

        void load(const Config& cfg);

        Peer* lookup(u32 vip) const noexcept;
    };

    // Pull the IPv4 dst out of an inner packet. Returns false (and
    // leaves *out untouched) if the buffer is too short or the first
    // nibble isn't 4 (we drop IPv6 for now).
    bool dstFromIPv4(const u8* pkt, size_t len, u32* out) noexcept;

    // Per-peer round-robin pick. Atomic increment, no locking.
    const sockaddr_in* pickDst(Peer* peer) noexcept;
}
