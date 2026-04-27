#include "gso.h"
#include "csum.h"

#include <std/sys/crt.h>

using namespace stl;
using namespace gofra;

namespace {
    constexpr size_t IPV4_SRC_ADDR_OFFSET = 12;
    constexpr size_t IPV4_ADDR_LEN = 4;
    constexpr size_t TCP_FLAGS_OFFSET = 13;
    constexpr u8 TCP_FLAG_FIN = 0x01;
    constexpr u8 TCP_FLAG_PSH = 0x08;
    constexpr u8 IPPROTO_TCP_V = 6;

    inline void putBE16(u8* p, u16 v) noexcept {
        p[0] = (u8)(v >> 8);
        p[1] = (u8)v;
    }

    inline void putBE32(u8* p, u32 v) noexcept {
        p[0] = (u8)(v >> 24);
        p[1] = (u8)(v >> 16);
        p[2] = (u8)(v >> 8);
        p[3] = (u8)v;
    }

    inline u16 getBE16(const u8* p) noexcept {
        return (u16)(((u32)p[0] << 8) | (u32)p[1]);
    }

    inline u32 getBE32(const u8* p) noexcept {
        return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
    }
}

void gofra::decodeVirtioNetHdr(VirtioNetHdr* out, const u8* data) noexcept {
    memCpy(out, data, VIRTIO_NET_HDR_LEN);
}

void gofra::zeroVirtioNetHdr(u8* data) noexcept {
    memZero(data, data + VIRTIO_NET_HDR_LEN);
}

void gofra::gsoNoneChecksum(u8* pkt, size_t pktLen, u16 csumStart, u16 csumOffset) noexcept {
    size_t at = (size_t)csumStart + (size_t)csumOffset;

    // Kernel parked the pseudo-hdr sum at csumOffset; fold L4 on top.
    u64 initial = (u64)getBE16(pkt + at);

    pkt[at] = 0;
    pkt[at + 1] = 0;

    u16 csum = (u16)~checksum(pkt + csumStart, pktLen - csumStart, initial);

    putBE16(pkt + at, csum);
}

int gofra::gsoSplit(u8* in, size_t inLen, const VirtioNetHdr& hdr,
                    u8* const* outBufs, size_t* outSizes, size_t maxSegs) noexcept {
    if (hdr.gsoType != VIRTIO_NET_HDR_GSO_TCPV4) {
        return -1;
    }

    const size_t iphLen = (size_t)hdr.csumStart;
    const size_t hdrTotal = (size_t)hdr.hdrLen;
    const size_t srcAddrOffset = IPV4_SRC_ADDR_OFFSET;
    const size_t addrLen = IPV4_ADDR_LEN;
    const u8 protocol = IPPROTO_TCP_V;

    // Zero IPv4+L4 csum fields in `in` for clean recompute below.
    in[10] = 0;
    in[11] = 0;

    const size_t transportCsumAt = (size_t)hdr.csumStart + (size_t)hdr.csumOffset;

    in[transportCsumAt] = 0;
    in[transportCsumAt + 1] = 0;

    const u32 firstTcpSeq = getBE32(in + hdr.csumStart + 4);

    size_t nextDataAt = hdrTotal;
    size_t i = 0;

    while (nextDataAt < inLen) {
        if (i == maxSegs) {
            return -1;
        }

        size_t nextEnd = nextDataAt + (size_t)hdr.gsoSize;

        if (nextEnd > inLen) {
            nextEnd = inLen;
        }

        const size_t segDataLen = nextEnd - nextDataAt;
        const size_t totalLen = hdrTotal + segDataLen;

        outSizes[i] = totalLen;

        u8* out = outBufs[i];

        memCpy(out, in, iphLen);

        if (i > 0) {
            u16 id = getBE16(out + 4);
            id = (u16)(id + (u16)i);
            putBE16(out + 4, id);
        }

        putBE16(out + 2, (u16)totalLen);

        u16 ipv4Csum = (u16)~checksum(out, iphLen, 0);
        putBE16(out + 10, ipv4Csum);

        const size_t transportHdrLen = hdrTotal - iphLen;
        memCpy(out + iphLen, in + iphLen, transportHdrLen);

        const u32 tcpSeq = firstTcpSeq + (u32)hdr.gsoSize * (u32)i;
        putBE32(out + iphLen + 4, tcpSeq);

        if (nextEnd != inLen) {
            // FIN/PSH only on the last segment.
            out[iphLen + TCP_FLAGS_OFFSET] &= (u8)~(TCP_FLAG_FIN | TCP_FLAG_PSH);
        }

        memCpy(out + hdrTotal, in + nextDataAt, segDataLen);

        const u16 lenForPseudo = (u16)(transportHdrLen + segDataLen);
        const u64 pseudo = pseudoHeaderChecksumNoFold(
            protocol,
            in + srcAddrOffset,
            in + srcAddrOffset + addrLen,
            addrLen,
            lenForPseudo);

        const u16 trCsum = (u16)~checksum(out + iphLen, totalLen - iphLen, pseudo);
        putBE16(out + iphLen + (size_t)hdr.csumOffset, trCsum);

        nextDataAt += (size_t)hdr.gsoSize;
        ++i;
    }

    return (int)i;
}
