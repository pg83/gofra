#include "peer.h"
#include "config.h"

#include <std/mem/obj_pool.h>
#include <std/sys/atomic.h>

#include <netinet/in.h>

using namespace stl;
using namespace gofra;

void PeerTable::load(const Config& cfg) {
    for (size_t i = 0; i < cfg.peers.length(); ++i) {
        auto p = cfg.peers[i];
        byVip_.insert(p->vip, p);
    }
}

Peer* PeerTable::lookup(u32 vip) const noexcept {
    auto found = byVip_.find(vip);
    return found ? *found : nullptr;
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

const sockaddr_in* gofra::pickDst(Peer* peer) noexcept {
    u64 c = stdAtomicAddAndFetch(&peer->rr, (u64)1, MemoryOrder::Relaxed) - 1;
    size_t i = (size_t)(c % peer->dsts.length());
    return &peer->dsts[i];
}
