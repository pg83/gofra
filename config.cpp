#include "config.h"
#include "peer.h"

#include <std/mem/obj_pool.h>
#include <std/lib/vector.h>
#include <std/lib/buffer.h>
#include <std/str/view.h>
#include <std/str/builder.h>
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

        StringView description() override {
            if (!full_.empty()) {
                return full_;
            }

            (StringBuilder()
             << StringView(u8"config: ")
             << msg_)
                .xchg(full_);

            return full_;
        }
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

    // Walk a comma-separated list, calling `cb(item)` on each
    // non-empty trimmed segment.
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

    // Copy `s` into pool-allocated NUL-terminated storage.
    const char* internCStr(ObjPool* pool, StringView s) {
        char* d = (char*)pool->allocate(s.length() + 1);
        memCpy(d, s.data(), s.length());
        d[s.length()] = 0;
        return d;
    }

    enum class Section { Top, Me, Peer, Udp };
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

    StringView text = fileBuf;
    Section section = Section::Top;

    while (!text.empty()) {
        StringView line, rest;

        if (!text.split('\n', line, rest)) {
            line = text;
            rest = StringView();
        }

        line = line.stripSpace();
        text = rest;

        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        if (line[0] == '[') {
            if (line.back() != ']') {
                raise(StringBuilder() << StringView(u8"bad section: ") << line);
            }

            StringView sec = line.suffix(line.length() - 1).prefix(line.length() - 2).stripSpace();

            if (sec == StringView(u8"me")) {
                section = Section::Me;
            } else if (sec == StringView(u8"peer")) {
                section = Section::Peer;
            } else if (sec == StringView(u8"udp")) {
                section = Section::Udp;
            } else {
                raise(StringBuilder() << StringView(u8"unknown section: ") << sec);
            }

            continue;
        }

        StringView key, val;
        if (!line.split('=', key, val)) {
            raise(StringBuilder() << StringView(u8"missing '=': ") << line);
        }

        key = key.stripSpace();
        val = val.stripSpace();

        if (section == Section::Top) {
            if (key == StringView(u8"listen_port")) {
                cfg->listenPort = (u16)val.stou();
            } else if (key == StringView(u8"log_level")) {
                // accepted, currently ignored
            } else {
                raise(StringBuilder() << StringView(u8"unknown top key: ") << key);
            }
        } else if (section == Section::Me) {
            if (key == StringView(u8"underlay")) {
                forEachItem(val, [&](StringView it) {
                    cfg->underlay.pushBack(makeAddr(parseIPv4(it), 0));
                });
            } else if (key == StringView(u8"tun_dev")) {
                cfg->tunDev = internCStr(pool, val);
            } else if (key == StringView(u8"tun_mtu")) {
                cfg->tunMtu = (int)val.stou();
            } else if (key == StringView(u8"tun_vip")) {
                parseCIDR(val, &cfg->tunVip, &cfg->tunPrefixLen);
            } else {
                raise(StringBuilder() << StringView(u8"unknown me key: ") << key);
            }
        } else if (section == Section::Peer) {
            // [peer] section: key is the peer VIP, val is comma-list of dsts
            auto peer = pool->make<Peer>();
            u8 plen;
            parseCIDR(key, &peer->vip, &plen);

            forEachItem(val, [&](StringView it) {
                peer->dsts.pushBack(makeAddr(parseIPv4(it), 0));
            });

            if (peer->dsts.length() == 0) {
                raise(StringBuilder() << StringView(u8"peer ") << key << StringView(u8" has no dsts"));
            }

            cfg->peers.pushBack(peer);
        } else if (section == Section::Udp) {
            if (key == StringView(u8"recv_buf")) {
                cfg->udpRecvBuf = (int)val.stou();
            } else if (key == StringView(u8"send_buf")) {
                cfg->udpSendBuf = (int)val.stou();
            } else {
                raise(StringBuilder() << StringView(u8"unknown udp key: ") << key);
            }
        }
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
