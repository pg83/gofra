#pragma once

#include <std/sys/types.h>

namespace gofra {
    // 16-bit one's-complement TCP/IP checksum (wireguard-go vendor).
    // checksumNoFold keeps the unfolded 64-bit accumulator so callers
    // can chain pseudo-hdr + L4-hdr + payload before one fold.
    u64 checksumNoFold(const u8* data, size_t len, u64 initial) noexcept;
    u16 checksum(const u8* data, size_t len, u64 initial) noexcept;

    // IPv4/IPv6 TCP/UDP pseudo-header. addrLen=4 or 16; totalLen is L4 wire len.
    u64 pseudoHeaderChecksumNoFold(u8 protocol,
                                   const u8* srcAddr, const u8* dstAddr,
                                   size_t addrLen, u16 totalLen) noexcept;
}
