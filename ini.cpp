#include "ini.h"

#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/str/view.h>
#include <std/str/builder.h>
#include <std/sys/throw.h>

using namespace stl;
using namespace gofra::ini;

namespace {
    struct IniError: public Exception {
        Buffer msg_;
        Buffer full_;

        IniError(Buffer&& m) noexcept
            : msg_(move(m))
        {
        }

        ExceptionKind kind() const noexcept override {
            return ExceptionKind::Verify;
        }

        StringView description() override;
    };

    [[noreturn]] void raise(Buffer&& msg) {
        throw IniError(move(msg));
    }
}

StringView IniError::description() {
    if (!full_.empty()) {
        return full_;
    }

    (StringBuilder()
     << StringView(u8"ini: ")
     << msg_)
        .xchg(full_);

    return full_;
}

Config* gofra::ini::parseConfig(ObjPool* pool, StringView src) {
    auto cfg = pool->make<Config>(pool);

    // Implicit "" section catches lines before the first [...].
    Section* sec = cfg->insert(StringView(), pool);

    while (!src.empty()) {
        StringView line, rest;

        if (!src.split('\n', line, rest)) {
            line = src;
            rest = StringView();
        }

        line = line.stripSpace();
        src = rest;

        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        if (line[0] == '[') {
            if (line.back() != ']') {
                raise(StringBuilder() << StringView(u8"bad section: ") << line);
            }

            StringView name = line.suffix(line.length() - 1)
                                  .prefix(line.length() - 2)
                                  .stripSpace();

            if (auto* existing = cfg->find(name); existing) {
                sec = existing;
            } else {
                sec = cfg->insert(name, pool);
            }

            continue;
        }

        StringView key, val;

        if (!line.split('=', key, val)) {
            raise(StringBuilder() << StringView(u8"missing '=': ") << line);
        }

        key = key.stripSpace();
        val = val.stripSpace();

        // Track first-seen key in `keys` (re-declarations replace the
        // value but don't duplicate the key in the iteration list).
        if (!sec->map.find(key)) {
            sec->keys.pushBack(key);
        }

        sec->map.insert(key, val);
    }

    return cfg;
}
