#pragma once

#include <std/lib/vector.h>
#include <std/sym/s_map.h>
#include <std/str/view.h>

namespace stl {
    class ObjPool;
}

namespace gofra::ini {
    // One INI section. `map` answers find(key) in O(1); `keys`
    // keeps insertion order + the original key strings (SymbolMap
    // hashes only), so dynamic sections like [peers] can iterate.
    struct Section {
        stl::SymbolMap<stl::StringView> map;
        stl::Vector<stl::StringView> keys;

        explicit Section(stl::ObjPool* pool)
            : map(pool)
        {
        }
    };

    using Config = stl::SymbolMap<Section>;

    // Parse INI. Returned views reference into `src` (keep alive).
    // Pre-section lines go to a section keyed by the empty view.
    Config* parseConfig(stl::ObjPool* pool, stl::StringView src);
}
