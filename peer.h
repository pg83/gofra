#pragma once

#include <std/sys/types.h>

struct sockaddr;

namespace stl {
    class ObjPool;
}

namespace gofra {
    namespace ini {
        struct Section;
    }

    // One VIP → its underlay endpoints. The hot-path stripe is in conn.h.
    struct Peer {
        virtual u32 vip() const noexcept = 0;
        virtual size_t dstCount() const noexcept = 0;
        virtual const sockaddr* dst(size_t i) const noexcept = 0;
    };

    // Cluster map: every overlay VIP → Peer (us included). No "self";
    // Config resolves that via lookup(my_vip).
    struct PeerTable {
        virtual size_t size() const noexcept = 0;
        virtual Peer* at(size_t i) const noexcept = 0;
        virtual Peer* lookup(u32 vip) const noexcept = 0;

        static PeerTable* create(stl::ObjPool* pool, ini::Section* sec);
    };

    // Read IPv4 dst from the packet. False (no write) if too short or v6.
    bool dstFromIPv4(const u8* pkt, size_t len, u32* out) noexcept;
}
