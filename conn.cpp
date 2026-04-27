#include "conn.h"
#include "peer.h"
#include "socks.h"

#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/sym/i_map.h>
#include <std/sys/atomic.h>

#include <sys/socket.h>

using namespace stl;
using namespace gofra;

namespace {
    struct ConnImpl: public Conn {
        u32 vip_;
        Vector<ConnSlot> slots_;
        u64 rr_ = 0;

        explicit ConnImpl(u32 vip) noexcept
            : vip_(vip)
        {
        }

        u32 vip() const noexcept override {
            return vip_;
        }

        const ConnSlot* next() noexcept override;
    };

    struct ConnTableImpl: public ConnTable {
        Vector<int> srcFds_;
        Vector<Conn*> conns_;
        IntMap<Conn*> byVip_;

        ConnTableImpl(ObjPool* pool, PeerTable* peers, Peer* self, int rcvBuf, int sndBuf);

        size_t size() const noexcept override {
            return conns_.length();
        }

        size_t srcCount() const noexcept override {
            return srcFds_.length();
        }

        int srcFd(size_t i) const noexcept override {
            return srcFds_[i];
        }

        Conn* lookup(u32 vip) const noexcept override;
    };
}

const ConnSlot* ConnImpl::next() noexcept {
    u64 c = stdAtomicAddAndFetch(&rr_, (u64)1, MemoryOrder::Relaxed) - 1;
    size_t i = (size_t)(c % slots_.length());

    return &slots_[i];
}

ConnTableImpl::ConnTableImpl(ObjPool* pool, PeerTable* peers, Peer* self, int rcvBuf, int sndBuf)
    : byVip_(pool)
{
    size_t n = self->dstCount();

    // Open N source sockets, one per local underlay. fds get pool-
    // tracked (ScopedFD) inside openUdpSocket, so we just stash the
    // ints.
    for (size_t i = 0; i < n; ++i) {
        srcFds_.pushBack(openUdpSocket(pool, self->dst(i), rcvBuf, sndBuf));
    }

    // For each peer other than self, lay out N*M slots in the order
    // [j*N + i] so the atomic counter rotates src fast and dst slow:
    //   k=0 → (s0,d0), k=1 → (s1,d0), ..., k=N → (s0,d1), ...
    // matches gofra1's stripe (srcIdx = c%N, dstIdx = (c/N)%M).
    for (size_t k = 0; k < peers->size(); ++k) {
        Peer* p = peers->at(k);

        if (p == self) {
            continue;
        }

        size_t m = p->dstCount();
        auto conn = pool->make<ConnImpl>(p->vip());

        for (size_t j = 0; j < m; ++j) {
            for (size_t i = 0; i < n; ++i) {
                ConnSlot s = {
                    self->dst(i),
                    srcFds_[i],
                    p->dst(j),
                };

                conn->slots_.pushBack(s);
            }
        }

        conns_.pushBack(conn);
        byVip_.insert(p->vip(), conn);
    }
}

Conn* ConnTableImpl::lookup(u32 vip) const noexcept {
    auto found = byVip_.find(vip);

    return found ? *found : nullptr;
}

ConnTable* ConnTable::create(ObjPool* pool, PeerTable* peers, Peer* self, int rcvBuf, int sndBuf) {
    return pool->make<ConnTableImpl>(pool, peers, self, rcvBuf, sndBuf);
}
