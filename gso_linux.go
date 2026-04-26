//go:build linux

// virtio_net_hdr-aware TX-GSO for TCPv4. Kernel hands us a single
// pseudo-segment (up to ~64 KB) prefixed with a 10-byte
// virtio_net_hdr; we walk it gso_size bytes at a time, replicating
// the IP+TCP header for each MTU-sized real segment with the right
// seq/id/checksums, and emit them as ordinary IP packets over UDP.
//
// References:
//   * Linux uapi/linux/virtio_net.h
//   * net/ipv4/tcp_offload.c::tcp4_gso_segment
//   * wireguard-go tun/{tcp,checksum}.go (cross-checked logic)
//
// Scope: TCPV4 only. Non-GSO packets pass through unsegmented.
// Other GSO types (TCPV6, UDP, ECN combos) are dropped with a debug
// log — out of scope for the lab MVP.
package main

import (
	"encoding/binary"
	"errors"
)

const (
	virtioNetHdrLen = 10

	virtioFNeedsCsum = 0x01
	virtioFDataValid = 0x02

	virtioGSONone  = 0
	virtioGSOTCPV4 = 1
	virtioGSOUDP   = 3
	virtioGSOTCPV6 = 4
	virtioGSOECN   = 0x80
)

type virtioNetHdr struct {
	flags      uint8
	gsoType    uint8
	hdrLen     uint16
	gsoSize    uint16
	csumStart  uint16
	csumOffset uint16
}

func parseVirtioNetHdr(b []byte) virtioNetHdr {
	return virtioNetHdr{
		flags:      b[0],
		gsoType:    b[1] &^ virtioGSOECN,
		hdrLen:     binary.LittleEndian.Uint16(b[2:4]),
		gsoSize:    binary.LittleEndian.Uint16(b[4:6]),
		csumStart:  binary.LittleEndian.Uint16(b[6:8]),
		csumOffset: binary.LittleEndian.Uint16(b[8:10]),
	}
}

// inetChecksum computes the standard 16-bit one's-complement
// checksum over `b`, in network byte order.
func inetChecksum(b []byte) uint16 {
	var sum uint32
	i := 0
	for ; i+1 < len(b); i += 2 {
		sum += uint32(binary.BigEndian.Uint16(b[i : i+2]))
	}
	if i < len(b) {
		sum += uint32(b[i]) << 8
	}
	for sum > 0xffff {
		sum = (sum & 0xffff) + (sum >> 16)
	}
	return ^uint16(sum)
}

// tcp4Checksum computes the TCP/IPv4 checksum: pseudo-header
// (src_ip, dst_ip, 0, proto, tcp_len) one's-complement-summed
// against tcp (header + payload, with the checksum field already
// zeroed by the caller).
func tcp4Checksum(srcIP, dstIP [4]byte, tcp []byte) uint16 {
	var sum uint32
	sum += uint32(binary.BigEndian.Uint16(srcIP[0:2]))
	sum += uint32(binary.BigEndian.Uint16(srcIP[2:4]))
	sum += uint32(binary.BigEndian.Uint16(dstIP[0:2]))
	sum += uint32(binary.BigEndian.Uint16(dstIP[2:4]))
	sum += 6 // protocol
	sum += uint32(len(tcp))
	i := 0
	for ; i+1 < len(tcp); i += 2 {
		sum += uint32(binary.BigEndian.Uint16(tcp[i : i+2]))
	}
	if i < len(tcp) {
		sum += uint32(tcp[i]) << 8
	}
	for sum > 0xffff {
		sum = (sum & 0xffff) + (sum >> 16)
	}
	return ^uint16(sum)
}

// segmentTCPv4 walks `pkt` (the IP+TCP+payload portion, no
// virtio_net_hdr prefix) gsoSize bytes at a time and writes each
// resulting MTU-sized segment into out[i] (caller-allocated). All
// per-segment TCP/IP fields are fixed up: total length, IP id,
// IP checksum, TCP seq, TCP flags (FIN/PSH cleared on every
// segment except the last), TCP checksum.
//
// Returns the number of segments produced.
func segmentTCPv4(pkt []byte, gsoSize int, out [][]byte) (int, error) {
	if gsoSize == 0 {
		return 0, errors.New("segmentTCPv4: gsoSize=0")
	}
	if len(pkt) < 20 {
		return 0, errors.New("segmentTCPv4: short ip header")
	}
	ipHdrLen := int(pkt[0]&0x0f) * 4
	if ipHdrLen < 20 || len(pkt) < ipHdrLen+20 {
		return 0, errors.New("segmentTCPv4: bad ip header length")
	}
	tcpHdrLen := int(pkt[ipHdrLen+12]>>4) * 4
	if tcpHdrLen < 20 || len(pkt) < ipHdrLen+tcpHdrLen {
		return 0, errors.New("segmentTCPv4: bad tcp header length")
	}
	hdrLen := ipHdrLen + tcpHdrLen
	payload := pkt[hdrLen:]
	if len(payload) == 0 {
		return 0, errors.New("segmentTCPv4: empty payload")
	}

	var srcIP, dstIP [4]byte
	copy(srcIP[:], pkt[12:16])
	copy(dstIP[:], pkt[16:20])
	origIPID := binary.BigEndian.Uint16(pkt[4:6])
	origSeq := binary.BigEndian.Uint32(pkt[ipHdrLen+4 : ipHdrLen+8])
	origFlags := pkt[ipHdrLen+13]

	n := 0
	for off := 0; off < len(payload); off += gsoSize {
		end := off + gsoSize
		last := false
		if end >= len(payload) {
			end = len(payload)
			last = true
		}
		segLen := end - off
		segTotal := hdrLen + segLen

		if n >= len(out) || cap(out[n]) < segTotal {
			return n, errors.New("segmentTCPv4: out buffer too small")
		}
		seg := out[n][:segTotal]
		copy(seg[:hdrLen], pkt[:hdrLen])
		copy(seg[hdrLen:], payload[off:end])

		// IP total length
		binary.BigEndian.PutUint16(seg[2:4], uint16(segTotal))
		// IP id
		binary.BigEndian.PutUint16(seg[4:6], origIPID+uint16(n))
		// IP checksum (zero before computing)
		seg[10] = 0
		seg[11] = 0
		ipCsum := inetChecksum(seg[:ipHdrLen])
		binary.BigEndian.PutUint16(seg[10:12], ipCsum)

		// TCP seq
		binary.BigEndian.PutUint32(seg[ipHdrLen+4:ipHdrLen+8], origSeq+uint32(off))
		// TCP flags — clear FIN(0x01) and PSH(0x08) on every
		// non-last segment.
		flags := origFlags
		if !last {
			flags &^= 0x09
		}
		seg[ipHdrLen+13] = flags
		// TCP checksum (zero before computing)
		seg[ipHdrLen+16] = 0
		seg[ipHdrLen+17] = 0
		tcpCsum := tcp4Checksum(srcIP, dstIP, seg[ipHdrLen:])
		binary.BigEndian.PutUint16(seg[ipHdrLen+16:ipHdrLen+18], tcpCsum)

		out[n] = seg
		n++
	}
	return n, nil
}
