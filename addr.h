#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>

struct sockaddr_in;

namespace gofra {
    // Parse an IPv4 dotted string into host-byte-order u32. Throws on
    // a malformed input.
    u32 parseIPv4(stl::StringView s);

    // Parse "a.b.c.d/N" or bare "a.b.c.d" (defaults to /32). Sets
    // *addr (host order) and *prefixLen.
    void parseCIDR(stl::StringView s, u32* addr, u8* prefixLen);

    // Build a sockaddr_in from host-order ip + port.
    sockaddr_in makeAddr(u32 ip, u16 port);

    // Parse "a.b.c.d:port" into sockaddr_in.
    sockaddr_in parseSockAddr(stl::StringView s);

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
