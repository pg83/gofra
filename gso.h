#pragma once

#include <std/sys/types.h>

namespace gofra {
    // virtio_net_hdr (10 bytes, native byte order); pinned via
    // TUNSETVNETHDRSZ.
    struct VirtioNetHdr {
        u8 flags;
        u8 gsoType;
        u16 hdrLen;
        u16 gsoSize;
        u16 csumStart;
        u16 csumOffset;
    };

    static_assert(sizeof(VirtioNetHdr) == 10, "virtio_net_hdr must be 10 bytes");

    constexpr size_t VIRTIO_NET_HDR_LEN = 10;

    // Flags (vendored from <linux/virtio_net.h>).
    constexpr u8 VIRTIO_NET_HDR_F_NEEDS_CSUM = 0x01;

    // gsoType.
    constexpr u8 VIRTIO_NET_HDR_GSO_NONE  = 0x00;
    constexpr u8 VIRTIO_NET_HDR_GSO_TCPV4 = 0x01;
    constexpr u8 VIRTIO_NET_HDR_GSO_UDP   = 0x03;
    constexpr u8 VIRTIO_NET_HDR_GSO_TCPV6 = 0x04;

    // Native-endian copy of the 10-byte hdr at the TUN read start.
    void decodeVirtioNetHdr(VirtioNetHdr* out, const u8* data) noexcept;

    // Zero virtio_net_hdr → flags=0, gso=NONE; prefix for TUN write.
    void zeroVirtioNetHdr(u8* data) noexcept;

    // Patch L4 csum of a NEEDS_CSUM non-GSO packet in place; pktLen
    // is the full inner-IP packet length.
    void gsoNoneChecksum(u8* pkt, size_t pktLen, u16 csumStart, u16 csumOffset) noexcept;

    // Split GSO_TCPV4 super-packet into per-MSS segments (TX). Each
    // outBufs[i] needs ≥ mtu+slack. Returns segs produced, or -1 if
    // the count would exceed maxSegs / gso_type unsupported.
    int gsoSplit(u8* in, size_t inLen, const VirtioNetHdr& hdr,
                 u8* const* outBufs, size_t* outSizes, size_t maxSegs) noexcept;
}
