#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>

struct sockaddr;

namespace stl {
    class ObjPool;
}

namespace gofra {
    // "a.b.c.d" → host-order u32. Throws on bad input.
    u32 parseIPv4(stl::StringView s);

    // "a.b.c.d[/N]" → host-order *addr + *prefixLen (N defaults to 32).
    void parseCIDR(stl::StringView s, u32* addr, u8* prefixLen);

    // "a.b.c.d:port" → pool-owned sockaddr_storage as const sockaddr*.
    const sockaddr* parseSockAddr(stl::ObjPool* pool, stl::StringView s);

    // socklen_t for sendto/bind, dispatched on sa_family.
    u32 addrLen(const sockaddr* sa) noexcept;

    // Walk a comma-separated list, calling cb on each non-empty item.
    template <typename F>
    void forEachItem(stl::StringView s, F cb) {
        while (!s.empty()) {
            stl::StringView item, rest;

            if (!s.split(',', item, rest)) {
                item = s;
                rest = stl::StringView();
            }

            item = item.stripSpace();

            if (!item.empty()) {
                cb(item);
            }

            s = rest;
        }
    }
}
