#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>

struct sockaddr;

namespace stl {
    class ObjPool;
}

namespace gofra {
    // Parse an IPv4 dotted string into host-byte-order u32. Throws on
    // a malformed input.
    u32 parseIPv4(stl::StringView s);

    // Parse "a.b.c.d/N" or bare "a.b.c.d" (defaults to /32). Sets
    // *addr (host order) and *prefixLen.
    void parseCIDR(stl::StringView s, u32* addr, u8* prefixLen);

    // Parse "a.b.c.d:port" into a pool-owned sockaddr_storage,
    // returned as a const sockaddr*. The storage outlives the pool.
    const sockaddr* parseSockAddr(stl::ObjPool* pool, stl::StringView s);

    // Length of a sockaddr by its sa_family — sendto / bind take a
    // socklen_t and the AF determines it.
    u32 addrLen(const sockaddr* sa) noexcept;

    // Walk a comma-separated list, calling cb on each non-empty
    // trimmed item. Template body lives in the header.
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
