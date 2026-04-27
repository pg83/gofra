#pragma once

#include <std/lib/vector.h>
#include <std/sym/s_map.h>
#include <std/str/view.h>

namespace stl {
    class ObjPool;
}

namespace gofra::ini {
    // One INI section. `map` answers `find(key)` in O(1); `keys`
    // preserves insertion order and the original key strings (the
    // SymbolMap stores only their hashes), so callers that need to
    // walk every key in a dynamic section — e.g. [peer] where the
    // keys are peer VIPs — can iterate `keys` and look the values up
    // in `map`.
    struct Section {
        stl::SymbolMap<stl::StringView> map;
        stl::Vector<stl::StringView> keys;

        explicit Section(stl::ObjPool* pool)
            : map(pool)
        {
        }
    };

    using Config = stl::SymbolMap<Section>;

    // Parse INI text. Section / key / value StringViews reference into
    // `src` — keep src alive while using the result. Lines outside any
    // [section] go into a section keyed by the empty StringView. The
    // returned Config lives in `pool` (pool->make).
    Config* parseConfig(stl::ObjPool* pool, stl::StringView src);
}
