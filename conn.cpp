#include "conn.h"
#include "peer.h"
#include "socks.h"

#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/str/builder.h>
#include <std/str/view.h>
#include <std/sym/i_map.h>
#include <std/sys/atomic.h>
#include <std/sys/throw.h>
#include <std/ios/sys.h>

#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace stl;
using namespace gofra;

namespace {
    struct ConnError: public Exception {
        Buffer msg_;
        Buffer full_;

        ConnError(Buffer&& m) noexcept
            : msg_(static_cast<Buffer&&>(m))
        {
        }

        ExceptionKind kind() const noexcept override {
            return ExceptionKind::Verify;
        }

        StringView description() override;
    };

    [[noreturn]] void raise(Buffer&& msg) {
        throw ConnError(static_cast<Buffer&&>(msg));
    }

    u64 nowMs() noexcept {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000;
    }

    u64 entryKey(u32 srcIdx, const sockaddr* sa) noexcept {
        u32 ip = ((const sockaddr_in*)sa)->sin_addr.s_addr;
        return ((u64)srcIdx << 32) | (u64)ip;
    }

    size_t gcdN(size_t a, size_t b) noexcept {
        while (b) {
            size_t t = b;
            b = a % b;
            a = t;
        }

        return a;
    }

    // 0 if no s in [N+1, N*M) satisfies bijectivity + src+dst diversity.
    size_t findStep(size_t N, size_t M) noexcept {
        size_t total = N * M;

        for (size_t s = N + 1; s < total; ++s) {
            if (gcdN(s, total) != 1) {
                continue;
            }

            if (s % N == 0) {
                continue;
            }

            size_t q = s / N;

            if (q % M == 0) {
                continue;
            }

            if ((q + 1) % M == 0) {
                continue;
            }

            return s;
        }

        return 0;
    }

    void formatVip(u32 vip, char* dst, size_t cap) noexcept {
        in_addr a;
        a.s_addr = htonl(vip);
        inet_ntop(AF_INET, &a, dst, (socklen_t)cap);
    }

    struct ConnImpl: public Conn {
        u32 vip_;
        u64 timeoutMs_;
        size_t step_;
        Vector<const ConnSlot*> slots_;
        u64 rr_ = 0;

        ConnImpl(u32 vip, u64 timeoutMs, size_t step) noexcept
            : vip_(vip)
            , timeoutMs_(timeoutMs)
            , step_(step)
        {
        }

        u32 vip() const noexcept override {
            return vip_;
        }

        const ConnSlot* next(u64 now) noexcept override;
        const ConnSlot* next(const ConnSlot* cur, u64 now) noexcept override;
    };

    struct ConnTableImpl: public ConnTable {
        u64 timeoutMs_;
        int redundancy_;
        Vector<int> srcFds_;
        Vector<Conn*> conns_;
        Vector<ConnSlot*> allSlots_;
        IntMap<Conn*> byVip_;
        IntMap<ConnSlot*> byEntry_;

        ConnTableImpl(ObjPool* pool, PeerTable* peers, Peer* self, int rcvBuf, int sndBuf, u64 timeoutMs, int redundancy);

        size_t size() const noexcept override {
            return conns_.length();
        }

        size_t srcCount() const noexcept override {
            return srcFds_.length();
        }

        int srcFd(size_t i) const noexcept override {
            return srcFds_[i];
        }

        int redundancy() const noexcept override {
            return redundancy_;
        }

        Conn* lookup(u32 vip) const noexcept override;
        ConnSlot* lookupSlot(u32 srcIdx, const sockaddr* peerSrc) const noexcept override;
        void visitSlotsImpl(VisitorFace&& v) override;
    };
}

StringView ConnError::description() {
    if (!full_.empty()) {
        return full_;
    }

    (StringBuilder()
     << StringView(u8"conn: ")
     << msg_)
        .xchg(full_);

    return full_;
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

    return slots_[base];
}

const ConnSlot* ConnImpl::next(const ConnSlot* cur, u64 now) noexcept {
    size_t total = slots_.length();
    size_t base = ((size_t)cur->connIdx + step_) % total;

    for (size_t k = 0; k < total; ++k) {
        size_t idx = (base + k * step_) % total;
        u64 ls = stdAtomicFetch(&slots_[idx]->lastSeen, MemoryOrder::Relaxed);

        if (now - ls <= timeoutMs_) {
            return slots_[idx];
        }
    }

    return slots_[base];
}

ConnTableImpl::ConnTableImpl(ObjPool* pool, PeerTable* peers, Peer* self, int rcvBuf, int sndBuf, u64 timeoutMs, int redundancy)
    : timeoutMs_(timeoutMs)
    , redundancy_(redundancy)
    , byVip_(pool)
    , byEntry_(pool)
{
    size_t n = self->dstCount();

    for (size_t i = 0; i < n; ++i) {
        srcFds_.pushBack(openUdpSocket(pool, self->dst(i), rcvBuf, sndBuf));
    }

    u64 startMs = nowMs();

    for (size_t k = 0; k < peers->size(); ++k) {
        Peer* p = peers->at(k);

        if (p == self) {
            continue;
        }

        size_t m = p->dstCount();
        size_t total = n * m;

        char vipStr[INET_ADDRSTRLEN];
        formatVip(p->vip(), vipStr, sizeof(vipStr));

        if ((size_t)redundancy > total) {
            raise(StringBuilder()
                  << StringView(u8"peer vip=") << StringView(vipStr)
                  << StringView(u8" N=") << (u64)n
                  << StringView(u8" M=") << (u64)m
                  << StringView(u8": redundancy=") << (u64)redundancy
                  << StringView(u8" exceeds N*M=") << (u64)total);
        }

        size_t step = 0;

        if (redundancy > 1) {
            step = findStep(n, m);

            if (step == 0) {
                raise(StringBuilder()
                      << StringView(u8"peer vip=") << StringView(vipStr)
                      << StringView(u8" N=") << (u64)n
                      << StringView(u8" M=") << (u64)m
                      << StringView(u8": no step satisfies full src+dst diversity")
                      << StringView(u8" (M=2 always fails geometrically); reduce redundancy to 1 or change underlay layout"));
            }
        }

        auto conn = pool->make<ConnImpl>(p->vip(), timeoutMs, step);

        for (size_t j = 0; j < m; ++j) {
            for (size_t i = 0; i < n; ++i) {
                auto* s = pool->make<ConnSlot>(ConnSlot{
                    self->dst(i),
                    srcFds_[i],
                    (u32)i,
                    p->dst(j),
                    startMs,
                    (u32)(j * n + i),
                });

                conn->slots_.pushBack(s);
                allSlots_.pushBack(s);
                byEntry_.insert(entryKey((u32)i, p->dst(j)), s);
            }
        }

        if (redundancy > 1) {
            for (size_t i = 0; i < total; ++i) {
                const ConnSlot* a = conn->slots_[i];
                size_t bIdx = (i + step) % total;
                const ConnSlot* b = conn->slots_[bIdx];

                if (a->srcAddr == b->srcAddr) {
                    raise(StringBuilder()
                          << StringView(u8"peer vip=") << StringView(vipStr)
                          << StringView(u8" step=") << (u64)step
                          << StringView(u8" slot ") << (u64)i
                          << StringView(u8": src unchanged after step (formula bug)"));
                }

                if (a->dstAddr == b->dstAddr) {
                    raise(StringBuilder()
                          << StringView(u8"peer vip=") << StringView(vipStr)
                          << StringView(u8" step=") << (u64)step
                          << StringView(u8" slot ") << (u64)i
                          << StringView(u8": dst unchanged after step (formula bug)"));
                }
            }
        }

        sysE << StringView(u8"gofra: conn vip=") << StringView(vipStr)
             << StringView(u8" N=") << (u64)n
             << StringView(u8" M=") << (u64)m
             << StringView(u8" step=") << (u64)step
             << endL;

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

ConnTable* ConnTable::create(ObjPool* pool, PeerTable* peers, Peer* self, int rcvBuf, int sndBuf, u64 timeoutMs, int redundancy) {
    return pool->make<ConnTableImpl>(pool, peers, self, rcvBuf, sndBuf, timeoutMs, redundancy);
}
