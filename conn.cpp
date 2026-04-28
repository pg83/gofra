#include "conn.h"
#include "peer.h"
#include "socks.h"

#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/sym/i_map.h>
#include <std/sys/atomic.h>

#include <time.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace stl;
using namespace gofra;

namespace {
    u64 nowMs() noexcept {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000;
    }

    u64 entryKey(u32 srcIdx, const sockaddr* sa) noexcept {
        u32 ip = ((const sockaddr_in*)sa)->sin_addr.s_addr;
        return ((u64)srcIdx << 32) | (u64)ip;
    }

    struct ConnImpl: public Conn {
        u32 vip_;
        u64 timeoutMs_;
        Vector<const ConnSlot*> slots_;
        u64 rr_ = 0;

        ConnImpl(u32 vip, u64 timeoutMs) noexcept
            : vip_(vip)
            , timeoutMs_(timeoutMs)
        {
        }

        u32 vip() const noexcept override {
            return vip_;
        }

        const ConnSlot* next(u64 now) noexcept override;
    };

    struct ConnTableImpl: public ConnTable {
        u64 timeoutMs_;
        Vector<int> srcFds_;
        Vector<Conn*> conns_;
        Vector<ConnSlot*> allSlots_;
        IntMap<Conn*> byVip_;
        IntMap<ConnSlot*> byEntry_;

        ConnTableImpl(ObjPool* pool, PeerTable* peers, Peer* self, int rcvBuf, int sndBuf, u64 timeoutMs);

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
        ConnSlot* lookupSlot(u32 srcIdx, const sockaddr* peerSrc) const noexcept override;
        void visitSlotsImpl(VisitorFace&& v) override;
    };
}

const ConnSlot* ConnImpl::next(u64 now) noexcept {
    size_t n = slots_.length();
    u64 c = stdAtomicAddAndFetch(&rr_, (u64)1, MemoryOrder::Relaxed) - 1;
    size_t base = (size_t)(c % n);

    for (size_t k = 0; k < n; ++k) {
        size_t i = (base + k) % n;
        u64 ls = stdAtomicFetch(&slots_[i]->lastSeen, MemoryOrder::Relaxed);

        if (now - ls <= timeoutMs_) {
            return slots_[i];
        }
    }

    // All slots dead — drop to base and let TCP retransmit handle it.
    return slots_[base];
}

ConnTableImpl::ConnTableImpl(ObjPool* pool, PeerTable* peers, Peer* self, int rcvBuf, int sndBuf, u64 timeoutMs)
    : timeoutMs_(timeoutMs)
    , byVip_(pool)
    , byEntry_(pool)
{
    size_t n = self->dstCount();

    for (size_t i = 0; i < n; ++i) {
        srcFds_.pushBack(openUdpSocket(pool, self->dst(i), rcvBuf, sndBuf));
    }

    u64 startMs = nowMs();

    // Slots [j*N + i]: rr rotates src fast, dst slow.
    for (size_t k = 0; k < peers->size(); ++k) {
        Peer* p = peers->at(k);

        if (p == self) {
            continue;
        }

        size_t m = p->dstCount();
        auto conn = pool->make<ConnImpl>(p->vip(), timeoutMs);

        for (size_t j = 0; j < m; ++j) {
            for (size_t i = 0; i < n; ++i) {
                auto* s = pool->make<ConnSlot>(ConnSlot{
                    self->dst(i),
                    srcFds_[i],
                    (u32)i,
                    p->dst(j),
                    startMs,
                });

                conn->slots_.pushBack(s);
                allSlots_.pushBack(s);
                byEntry_.insert(entryKey((u32)i, p->dst(j)), s);
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

ConnSlot* ConnTableImpl::lookupSlot(u32 srcIdx, const sockaddr* peerSrc) const noexcept {
    auto found = byEntry_.find(entryKey(srcIdx, peerSrc));

    return found ? *found : nullptr;
}

void ConnTableImpl::visitSlotsImpl(VisitorFace&& v) {
    for (size_t i = 0; i < allSlots_.length(); ++i) {
        v.visit(allSlots_[i]);
    }
}

ConnTable* ConnTable::create(ObjPool* pool, PeerTable* peers, Peer* self, int rcvBuf, int sndBuf, u64 timeoutMs) {
    return pool->make<ConnTableImpl>(pool, peers, self, rcvBuf, sndBuf, timeoutMs);
}
