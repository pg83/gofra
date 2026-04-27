#include "csum.h"

using namespace gofra;

namespace {
    // Native-endian load; bswap happens once at the boundary in
    // checksumNoFold.
    inline u64 loadU64(const u8* p) noexcept {
        u64 v;
        __builtin_memcpy(&v, p, 8);
        return v;
    }

    inline u32 loadU32(const u8* p) noexcept {
        u32 v;
        __builtin_memcpy(&v, p, 4);
        return v;
    }

    inline u16 loadU16(const u8* p) noexcept {
        u16 v;
        __builtin_memcpy(&v, p, 2);
        return v;
    }
}

u64 gofra::checksumNoFold(const u8* data, size_t len, u64 initial) noexcept {
    // Sum native-endian u64s in a 128-bit accumulator and bswap
    // once at the boundary; equivalent on LE, saves bswap/word.
    __uint128_t ac = (__uint128_t)__builtin_bswap64(initial);

    while (len >= 8) {
        ac += loadU64(data);
        data += 8;
        len -= 8;
    }

    if (len >= 4) {
        ac += loadU32(data);
        data += 4;
        len -= 4;
    }

    if (len >= 2) {
        ac += loadU16(data);
        data += 2;
        len -= 2;
    }

    if (len == 1) {
        // Odd tail: pad to LE u16 [b,0] = b.
        ac += (u64)*data;
    }

    // Fold the high 64 bits.
    u64 lo = (u64)ac;
    u64 hi = (u64)(ac >> 64);
    u64 sum = lo + hi;

    if (sum < lo) {
        ++sum;
    }

    return __builtin_bswap64(sum);
}

u16 gofra::checksum(const u8* data, size_t len, u64 initial) noexcept {
    u64 ac = checksumNoFold(data, len, initial);

    ac = (ac >> 16) + (ac & 0xffffu);
    ac = (ac >> 16) + (ac & 0xffffu);
    ac = (ac >> 16) + (ac & 0xffffu);
    ac = (ac >> 16) + (ac & 0xffffu);

    return (u16)ac;
}

u64 gofra::pseudoHeaderChecksumNoFold(u8 protocol,
                                      const u8* srcAddr, const u8* dstAddr,
                                      size_t addrLen, u16 totalLen) noexcept {
    u64 sum = checksumNoFold(srcAddr, addrLen, 0);
    sum = checksumNoFold(dstAddr, addrLen, sum);

    // Pseudo-hdr tail: 0x00 protocol len_hi len_lo
    u8 tail[4];
    tail[0] = 0;
    tail[1] = protocol;
    tail[2] = (u8)(totalLen >> 8);
    tail[3] = (u8)totalLen;

    return checksumNoFold(tail, sizeof(tail), sum);
}
