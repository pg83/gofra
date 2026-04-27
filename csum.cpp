#include "csum.h"

using namespace gofra;

namespace {
    // Read a 64-bit word from `p` natively (host endian). On x86 this
    // is a plain mov; the byte-swap dance to get the BE-equivalent
    // numeric value happens once at the boundary in checksumNoFold.
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
    // The trick: TCP/IP checksum is "sum of 16-bit big-endian words".
    // We sum native-endian u64s into a 128-bit accumulator and
    // byte-swap once at boundary; on LE hosts this is equivalent
    // (the carries cross byte boundaries the same way) and saves a
    // bswap per word.
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
        // Odd-length tail: original Go pads with [b, 0] and reads as
        // native-LE u16, which yields just b. Same here on LE.
        ac += (u64)*data;
    }

    // Fold the upper 64 bits down.
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

    // Pseudo-header byte sequence: 0x00 protocol totalLenHi totalLenLo
    u8 tail[4];
    tail[0] = 0;
    tail[1] = protocol;
    tail[2] = (u8)(totalLen >> 8);
    tail[3] = (u8)totalLen;

    return checksumNoFold(tail, sizeof(tail), sum);
}
