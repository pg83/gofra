#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>

struct sockaddr;

namespace stl {
    class ObjPool;
}

namespace gofra {
    u32 parseIPv4(stl::StringView s);
    void parseCIDR(stl::StringView s, u32* addr, u8* prefixLen);
    const sockaddr* parseSockAddr(stl::ObjPool* pool, stl::StringView s);
    u32 addrLen(const sockaddr* sa) noexcept;

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
