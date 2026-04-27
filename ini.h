#pragma once

#include <std/lib/vector.h>
#include <std/sym/s_map.h>
#include <std/str/view.h>

namespace stl {
    class ObjPool;
}

namespace gofra::ini {
    // SymbolMap hashes only; `keys` preserves originals for [peers]-style iteration.
    struct Section {
        stl::SymbolMap<stl::StringView> map;
        stl::Vector<stl::StringView> keys;

        explicit Section(stl::ObjPool* pool)
            : map(pool)
        {
        }
    };

    using Config = stl::SymbolMap<Section>;

    // Returned views reference into `src` (keep alive).
    Config* parseConfig(stl::ObjPool* pool, stl::StringView src);
}
