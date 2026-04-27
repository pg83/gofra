#include "peer.h"
#include "addr.h"
#include "ini.h"

#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/sym/i_map.h>
#include <std/sys/throw.h>

#include <netinet/in.h>

using namespace stl;
using namespace gofra;

namespace {
    struct PeerError: public Exception {
        Buffer msg_;
        Buffer full_;

        PeerError(Buffer&& m) noexcept
            : msg_(static_cast<Buffer&&>(m))
        {
        }

        ExceptionKind kind() const noexcept override {
            return ExceptionKind::Verify;
        }

        StringView description() override;
    };

    [[noreturn]] void raise(Buffer&& msg) {
        throw PeerError(static_cast<Buffer&&>(msg));
    }

    struct PeerImpl: public Peer {
        u32 vip_;
        Vector<sockaddr_in> dsts_;

        explicit PeerImpl(u32 vip) noexcept
            : vip_(vip)
        {
        }

        u32 vip() const noexcept override {
            return vip_;
        }

        size_t dstCount() const noexcept override {
            return dsts_.length();
        }

        const sockaddr_in* dst(size_t i) const noexcept override {
            return &dsts_[i];
        }
    };

    struct PeerTableImpl: public PeerTable {
        Vector<Peer*> peers_;
        IntMap<Peer*> byVip_;

        PeerTableImpl(ObjPool* pool, ini::Section* sec);

        size_t size() const noexcept override {
            return peers_.length();
        }

        Peer* at(size_t i) const noexcept override {
            return peers_[i];
        }

        Peer* lookup(u32 vip) const noexcept override;
    };
}

StringView PeerError::description() {
    if (!full_.empty()) {
        return full_;
    }

    (StringBuilder()
     << StringView(u8"peer: ")
     << msg_)
        .xchg(full_);

    return full_;
}

PeerTableImpl::PeerTableImpl(ObjPool* pool, ini::Section* sec)
    : byVip_(pool)
{
    if (!sec || sec->keys.length() == 0) {
        raise(StringBuilder() << StringView(u8"empty [peers] section"));
    }

    for (size_t i = 0; i < sec->keys.length(); ++i) {
        StringView vipStr = sec->keys[i];
        StringView dstStr = *sec->map.find(vipStr);

        u32 vip;
        u8 plen;
        parseCIDR(vipStr, &vip, &plen);

        auto p = pool->make<PeerImpl>(vip);

        forEachItem(dstStr, [&](StringView it) {
            p->dsts_.pushBack(parseSockAddr(it));
        });

        if (p->dsts_.length() == 0) {
            raise(StringBuilder() << StringView(u8"peer ") << vipStr << StringView(u8" has no dsts"));
        }

        peers_.pushBack(p);
        byVip_.insert(vip, p);
    }
}

Peer* PeerTableImpl::lookup(u32 vip) const noexcept {
    auto found = byVip_.find(vip);
    return found ? *found : nullptr;
}

PeerTable* PeerTable::create(ObjPool* pool, ini::Section* sec) {
    return pool->make<PeerTableImpl>(pool, sec);
}

bool gofra::dstFromIPv4(const u8* pkt, size_t len, u32* out) noexcept {
    if (len < 20) {
        return false;
    }

    if ((pkt[0] >> 4) != 4) {
        return false;
    }

    // dst is at offset 16..19, network byte order.
    u32 v = ((u32)pkt[16] << 24) | ((u32)pkt[17] << 16) | ((u32)pkt[18] << 8) | (u32)pkt[19];
    *out = v;

    return true;
}
