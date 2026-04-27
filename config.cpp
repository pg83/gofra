#include "config.h"
#include "ini.h"
#include "peer.h"

#include <std/mem/obj_pool.h>
#include <std/lib/vector.h>
#include <std/lib/buffer.h>
#include <std/str/view.h>
#include <std/str/builder.h>
#include <std/sym/s_map.h>
#include <std/sys/crt.h>
#include <std/sys/throw.h>
#include <std/ios/fs_utils.h>
#include <std/dbg/verify.h>

#include <arpa/inet.h>
#include <netinet/in.h>

using namespace stl;
using namespace gofra;

namespace {
    struct ConfigError: public Exception {
        Buffer msg_;
        Buffer full_;

        ConfigError(Buffer&& m) noexcept
            : msg_(move(m))
        {
        }

        ExceptionKind kind() const noexcept override {
            return ExceptionKind::Verify;
        }

        StringView description() override;
    };

    [[noreturn]] void raise(Buffer&& msg) {
        throw ConfigError(move(msg));
    }

    u32 parseIPv4(StringView s) {
        // inet_pton wants NUL-terminated; copy into a small buffer.
        char buf[16];
        STD_VERIFY(s.length() < sizeof(buf));
        memCpy(buf, s.data(), s.length());
        buf[s.length()] = 0;

        in_addr a;
        if (inet_pton(AF_INET, buf, &a) != 1) {
            raise(StringBuilder() << StringView(u8"bad ipv4: ") << s);
        }

        return ntohl(a.s_addr);
    }

    void parseCIDR(StringView s, u32* addr, u8* prefixLen) {
        StringView before, after;

        if (s.split('/', before, after)) {
            *addr = parseIPv4(before);
            *prefixLen = (u8)after.stou();
        } else {
            *addr = parseIPv4(s);
            *prefixLen = 32;
        }
    }

    sockaddr_in makeAddr(u32 ip, u16 port) {
        sockaddr_in sa = {};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = htonl(ip);
        return sa;
    }

    template <typename F>
    void forEachItem(StringView s, F cb) {
        while (!s.empty()) {
            StringView item, rest;

            if (!s.split(',', item, rest)) {
                item = s;
                rest = StringView();
            }

            item = item.stripSpace();

            if (!item.empty()) {
                cb(item);
            }

            s = rest;
        }
    }

    const char* internCStr(ObjPool* pool, StringView s) {
        char* d = (char*)pool->allocate(s.length() + 1);
        memCpy(d, s.data(), s.length());
        d[s.length()] = 0;
        return d;
    }

    // ---- typed extraction over an ini::Section ----

    StringView require(ini::Section* sec, StringView key, StringView ctx) {
        auto* v = sec->map.find(key);
        if (!v) {
            raise(StringBuilder() << ctx << StringView(u8".") << key << StringView(u8" missing"));
        }
        return *v;
    }

    void loadTop(Config* cfg, ini::Section* sec) {
        if (auto* v = sec->map.find(StringView(u8"listen_port")); v) {
            cfg->listenPort = (u16)v->stou();
        }
        // log_level: accepted, currently ignored
    }

    void loadMe(ObjPool* pool, Config* cfg, ini::Section* sec) {
        forEachItem(require(sec, StringView(u8"underlay"), StringView(u8"me")), [&](StringView it) {
            cfg->underlay.pushBack(makeAddr(parseIPv4(it), 0));
        });

        if (auto* v = sec->map.find(StringView(u8"tun_dev")); v) {
            cfg->tunDev = internCStr(pool, *v);
        }
        if (auto* v = sec->map.find(StringView(u8"tun_mtu")); v) {
            cfg->tunMtu = (int)v->stou();
        }

        parseCIDR(require(sec, StringView(u8"tun_vip"), StringView(u8"me")),
                  &cfg->tunVip, &cfg->tunPrefixLen);
    }

    void loadPeers(ObjPool* pool, Config* cfg, ini::Section* sec) {
        for (size_t i = 0; i < sec->keys.length(); ++i) {
            StringView vipStr = sec->keys[i];
            StringView dstStr = *sec->map.find(vipStr);

            auto peer = pool->make<Peer>();
            u8 plen;
            parseCIDR(vipStr, &peer->vip, &plen);

            forEachItem(dstStr, [&](StringView it) {
                peer->dsts.pushBack(makeAddr(parseIPv4(it), 0));
            });

            if (peer->dsts.length() == 0) {
                raise(StringBuilder() << StringView(u8"peer ") << vipStr << StringView(u8" has no dsts"));
            }

            cfg->peers.pushBack(peer);
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

    cfg->listenPort = 0;
    cfg->tunVip = 0;
    cfg->tunPrefixLen = 0;
    cfg->tunMtu = 1400;
    cfg->tunDev = "gofra0";
    cfg->udpRecvBuf = 16 << 20;
    cfg->udpSendBuf = 16 << 20;

    Buffer pathBuf;
    pathBuf.append(path.data(), path.length());

    Buffer fileBuf;
    readFileContent(pathBuf, fileBuf);

    auto* ini = ini::parseConfig(pool, fileBuf);

    if (auto* sec = ini->find(StringView()); sec) {
        loadTop(cfg, sec);
    }

    if (auto* sec = ini->find(StringView(u8"me")); sec) {
        loadMe(pool, cfg, sec);
    } else {
        raise(StringBuilder() << StringView(u8"missing [me] section"));
    }

    if (auto* sec = ini->find(StringView(u8"peer")); sec) {
        loadPeers(pool, cfg, sec);
    }

    if (auto* sec = ini->find(StringView(u8"udp")); sec) {
        loadUdp(cfg, sec);
    }

    if (cfg->listenPort == 0) {
        raise(StringBuilder() << StringView(u8"listen_port missing"));
    }

    if (cfg->underlay.length() == 0) {
        raise(StringBuilder() << StringView(u8"me.underlay missing"));
    }

    if (cfg->tunVip == 0) {
        raise(StringBuilder() << StringView(u8"me.tun_vip missing"));
    }

    if (cfg->peers.length() == 0) {
        raise(StringBuilder() << StringView(u8"no peers configured"));
    }

    // listenPort known now — populate ports on every sockaddr_in.
    for (size_t i = 0; i < cfg->underlay.length(); ++i) {
        cfg->underlay.mut(i).sin_port = htons(cfg->listenPort);
    }

    for (size_t i = 0; i < cfg->peers.length(); ++i) {
        Peer* p = cfg->peers[i];
        for (size_t j = 0; j < p->dsts.length(); ++j) {
            p->dsts.mut(j).sin_port = htons(cfg->listenPort);
        }
    }

    return cfg;
}
