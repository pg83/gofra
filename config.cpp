#include "config.h"
#include "addr.h"
#include "ini.h"
#include "peer.h"

#include <std/lib/buffer.h>
#include <std/mem/obj_pool.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/sym/s_map.h>
#include <std/sys/crt.h>
#include <std/sys/throw.h>
#include <std/ios/fs_utils.h>

#include <netinet/in.h>

using namespace stl;
using namespace gofra;

namespace {
    struct ConfigError: public Exception {
        Buffer msg_;
        Buffer full_;

        ConfigError(Buffer&& m) noexcept
            : msg_(static_cast<Buffer&&>(m))
        {
        }

        ExceptionKind kind() const noexcept override {
            return ExceptionKind::Verify;
        }

        StringView description() override;
    };

    [[noreturn]] void raise(Buffer&& msg) {
        throw ConfigError(static_cast<Buffer&&>(msg));
    }

    const char* internCStr(ObjPool* pool, StringView s) {
        char* d = (char*)pool->allocate(s.length() + 1);
        memCpy(d, s.data(), s.length());
        d[s.length()] = 0;
        return d;
    }

    StringView require(ini::Section* sec, StringView key, StringView ctx) {
        auto* v = sec->map.find(key);

        if (!v) {
            raise(StringBuilder() << ctx << StringView(u8".") << key << StringView(u8" missing"));
        }

        return *v;
    }

    void loadMe(ObjPool* pool, Config* cfg, ini::Section* sec) {
        parseCIDR(require(sec, StringView(u8"vip"), StringView(u8"me")),
                  &cfg->tunVip, &cfg->tunPrefixLen);

        if (auto* v = sec->map.find(StringView(u8"tun_dev")); v) {
            cfg->tunDev = internCStr(pool, *v);
        }

        if (auto* v = sec->map.find(StringView(u8"tun_mtu")); v) {
            cfg->tunMtu = (int)v->stou();
        }

        if (auto* v = sec->map.find(StringView(u8"redundancy")); v) {
            cfg->redundancy = (int)v->stou();
        }

        if (auto* v = sec->map.find(StringView(u8"user")); v) {
            cfg->user = internCStr(pool, *v);
        }
    }

    void loadUdp(Config* cfg, ini::Section* sec) {
        if (auto* v = sec->map.find(StringView(u8"recv_buf")); v) {
            cfg->udpRecvBuf = (int)v->stou();
        }

        if (auto* v = sec->map.find(StringView(u8"send_buf")); v) {
            cfg->udpSendBuf = (int)v->stou();
        }
    }

    void loadProbe(Config* cfg, ini::Section* sec) {
        if (auto* v = sec->map.find(StringView(u8"interval_ms")); v) {
            cfg->probeIntervalMs = v->stou();
        }

        if (auto* v = sec->map.find(StringView(u8"timeout_ms")); v) {
            cfg->probeTimeoutMs = v->stou();
        }

        if (auto* v = sec->map.find(StringView(u8"stats_interval_s")); v) {
            cfg->statsIntervalSec = v->stou();
        }
    }
}

StringView ConfigError::description() {
    if (!full_.empty()) {
        return full_;
    }

    (StringBuilder()
     << StringView(u8"config: ")
     << msg_)
        .xchg(full_);

    return full_;
}

Config* gofra::loadConfig(ObjPool* pool, StringView path) {
    auto cfg = pool->make<Config>();

    cfg->tunVip = 0;
    cfg->tunPrefixLen = 0;
    cfg->tunMtu = 1400;
    cfg->tunDev = "gofra0";
    cfg->peers = nullptr;
    cfg->self = nullptr;
    cfg->udpRecvBuf = 16 << 20;
    cfg->udpSendBuf = 16 << 20;
    cfg->probeIntervalMs = 200;
    cfg->probeTimeoutMs = 1000;
    cfg->statsIntervalSec = 10;
    cfg->redundancy = 1;
    cfg->user = nullptr;

    Buffer pathBuf;
    pathBuf.append(path.data(), path.length());

    Buffer fileBuf;
    readFileContent(pathBuf, fileBuf);

    auto* ini = ini::parseConfig(pool, fileBuf);

    if (auto* sec = ini->find(StringView(u8"me")); sec) {
        loadMe(pool, cfg, sec);
    } else {
        raise(StringBuilder() << StringView(u8"missing [me] section"));
    }

    if (cfg->tunVip == 0) {
        raise(StringBuilder() << StringView(u8"me.vip missing"));
    }

    cfg->peers = PeerTable::create(pool, ini->find(StringView(u8"peers")));
    cfg->self = cfg->peers->lookup(cfg->tunVip);

    if (!cfg->self) {
        raise(StringBuilder() << StringView(u8"my vip is not in [peers]"));
    }

    if (cfg->redundancy < 1) {
        raise(StringBuilder() << StringView(u8"me.redundancy must be >= 1, got ") << (u64)cfg->redundancy);
    }

    if (auto* sec = ini->find(StringView(u8"udp")); sec) {
        loadUdp(cfg, sec);
    }

    if (auto* sec = ini->find(StringView(u8"probe")); sec) {
        loadProbe(cfg, sec);
    }

    return cfg;
}
