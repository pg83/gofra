#pragma once

#include <std/sys/types.h>

struct sockaddr_in;

namespace stl {
    class ObjPool;
}

namespace gofra {
    namespace ini {
        struct Section;
    }

    // Peer: one VIP → its underlay endpoints. dst(i)/dstCount() expose
    // the endpoints (used by the bind path on the self entry);
    // pickDst is the hot-path RR pick from tunReader.
    struct Peer {
        virtual u32 vip() const noexcept = 0;
        virtual size_t dstCount() const noexcept = 0;
        virtual const sockaddr_in* dst(size_t i) const noexcept = 0;
        virtual const sockaddr_in* pickDst() noexcept = 0;
    };

    // PeerTable: pure cluster map, every overlay-VIP → Peer in the
    // cluster (us included). It has no notion of "self" — Config
    // resolves that via lookup(my_vip). Each entry's values are full
    // ip:port pairs parsed at config time, no global listen_port.
    struct PeerTable {
        virtual size_t size() const noexcept = 0;
        virtual Peer* lookup(u32 vip) const noexcept = 0;

        static PeerTable* create(stl::ObjPool* pool, ini::Section* sec);
    };

    // Pull the IPv4 dst out of an inner packet. Returns false (and
    // leaves *out untouched) if the buffer is too short or the first
    // nibble isn't 4 (we drop IPv6 for now).
    bool dstFromIPv4(const u8* pkt, size_t len, u32* out) noexcept;
}
