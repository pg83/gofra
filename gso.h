#pragma once

#include <std/sys/types.h>

namespace gofra {
    // virtio_net_hdr layout (10 bytes, native byte order — Linux's
    // IFF_VNET_HDR uses host endian, not big endian as the name
    // implies). Pinned to 10 bytes via TUNSETVNETHDRSZ; matches
    // wireguard-go's vendored layout.
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

    // Decode the 10-byte virtio_net_hdr at the start of a TUN read
    // buffer. Native-endian copy.
    void decodeVirtioNetHdr(VirtioNetHdr* out, const u8* data) noexcept;

    // Zero a 10-byte virtio_net_hdr — flags=0, gso_type=NONE — so
    // the kernel treats the following packet as fully-formed, no
    // offload needed. Used as the prefix on TUN write from RX path.
    void zeroVirtioNetHdr(u8* data) noexcept;

    // Recompute and patch the L4 checksum of a non-GSO packet whose
    // virtio_net_hdr says NEEDS_CSUM (kernel handed us a packet with
    // only the pseudo-header sum staged at csum_offset). Mutates
    // `pkt` in place. `pktLen` is the full inner-IP packet length.
    void gsoNoneChecksum(u8* pkt, size_t pktLen, u16 csumStart, u16 csumOffset) noexcept;

    // Split a GSO super-packet into per-MSS segments. Mirrors the Go
    // gsoSplit from wireguard-go (TX-only). `in`/`inLen` is the inner
    // IP packet right after the virtio_net_hdr — must be >= hdrLen.
    // `outBufs[i]` must each be at least `mtu + slack` bytes.
    // Each output is a fully-formed inner IP packet ready to encap
    // and send via UDP.
    //
    // Returns the number of segments produced (0..maxSegs), or -1 if
    // the super-packet would need more than maxSegs.
    //
    // Currently supports VIRTIO_NET_HDR_GSO_TCPV4 only — the only
    // gso_type we ever ask the kernel for via TUNSETOFFLOAD.
    int gsoSplit(u8* in, size_t inLen, const VirtioNetHdr& hdr,
                 u8* const* outBufs, size_t* outSizes, size_t maxSegs) noexcept;
}
