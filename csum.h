#pragma once

#include <std/sys/types.h>

namespace gofra {
    // 16-bit one's-complement TCP/IP checksum primitives, vendored
    // from wireguard-go (tun/checksum.go). The accumulator stays at
    // 64 bits / unfolded so callers can chain multiple checksumNoFold
    // calls (e.g. pseudo-header + transport header + payload) before
    // a single fold step in checksum().
    //
    // Reads u16 / u32 / u64 words from `data` natively; the result
    // is byte-swapped at the boundary so that on little-endian hosts
    // we still produce the same value as a literal "sum of 16-bit
    // big-endian words" definition.
    u64 checksumNoFold(const u8* data, size_t len, u64 initial) noexcept;
    u16 checksum(const u8* data, size_t len, u64 initial) noexcept;

    // Pseudo-header for IPv4 TCP/UDP checksum. addrLen is 4 (IPv4) or
    // 16 (IPv6). totalLen is the L4 segment length on the wire.
    u64 pseudoHeaderChecksumNoFold(u8 protocol,
                                   const u8* srcAddr, const u8* dstAddr,
                                   size_t addrLen, u16 totalLen) noexcept;
}
