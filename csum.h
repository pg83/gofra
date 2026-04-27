#pragma once

#include <std/sys/types.h>

namespace gofra {
    // NoFold variants chain pseudo-hdr + L4-hdr + payload before one fold.
    u64 checksumNoFold(const u8* data, size_t len, u64 initial) noexcept;
    u16 checksum(const u8* data, size_t len, u64 initial) noexcept;

    u64 pseudoHeaderChecksumNoFold(u8 protocol,
                                   const u8* srcAddr, const u8* dstAddr,
                                   size_t addrLen, u16 totalLen) noexcept;
}
