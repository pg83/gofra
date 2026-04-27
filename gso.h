#pragma once

#include <std/sys/types.h>

namespace gofra {
    // Native byte order despite the name (Linux IFF_VNET_HDR).
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

    constexpr u8 VIRTIO_NET_HDR_F_NEEDS_CSUM = 0x01;

    constexpr u8 VIRTIO_NET_HDR_GSO_NONE  = 0x00;
    constexpr u8 VIRTIO_NET_HDR_GSO_TCPV4 = 0x01;
    constexpr u8 VIRTIO_NET_HDR_GSO_UDP   = 0x03;
    constexpr u8 VIRTIO_NET_HDR_GSO_TCPV6 = 0x04;

    void decodeVirtioNetHdr(VirtioNetHdr* out, const u8* data) noexcept;
    void zeroVirtioNetHdr(u8* data) noexcept;
    void gsoNoneChecksum(u8* pkt, size_t pktLen, u16 csumStart, u16 csumOffset) noexcept;

    // Each outBufs[i] needs ≥ mtu+slack. Returns -1 if too many segs.
    int gsoSplit(u8* in, size_t inLen, const VirtioNetHdr& hdr,
                 u8* const* outBufs, size_t* outSizes, size_t maxSegs) noexcept;
}
