/* SPDX-License-Identifier: MIT
 *
 * Adapted from wireguard-go (Copyright (C) 2017-2025 WireGuard LLC.
 * All Rights Reserved.) — file tun/offload_linux.go.
 *
 * Trimmed to TX-only:
 *   * virtioNetHdr + decode/encode
 *   * gsoSplit, gsoNoneChecksum
 *   * the constants they depend on (ipv4SrcAddrOffset,
 *     ipv6SrcAddrOffset, tcpFlag*).
 *
 * GRO machinery (tcpGROTable / udpGROTable / coalesce / handleGRO)
 * is dropped — gofra writes one received UDP packet straight to TUN
 * without coalescing.
 */

//go:build linux

package main

import (
	"encoding/binary"
	"errors"
	"io"
	"unsafe"

	"golang.org/x/sys/unix"
)

const tcpFlagsOffset = 13

const (
	tcpFlagFIN uint8 = 0x01
	tcpFlagPSH uint8 = 0x08
)

const (
	ipv4SrcAddrOffset = 12
	ipv6SrcAddrOffset = 8
)

var ErrTooManySegments = errors.New("too many segments")

type virtioNetHdr struct {
	flags      uint8
	gsoType    uint8
	hdrLen     uint16
	gsoSize    uint16
	csumStart  uint16
	csumOffset uint16
}

func (v *virtioNetHdr) decode(b []byte) error {
	if len(b) < virtioNetHdrLen {
		return io.ErrShortBuffer
	}
	copy(unsafe.Slice((*byte)(unsafe.Pointer(v)), virtioNetHdrLen), b[:virtioNetHdrLen])
	return nil
}

func (v *virtioNetHdr) encode(b []byte) error {
	if len(b) < virtioNetHdrLen {
		return io.ErrShortBuffer
	}
	copy(b[:virtioNetHdrLen], unsafe.Slice((*byte)(unsafe.Pointer(v)), virtioNetHdrLen))
	return nil
}

const (
	// virtioNetHdrLen is the length in bytes of virtioNetHdr. This matches the
	// shape of the C ABI for its kernel counterpart -- sizeof(virtio_net_hdr).
	virtioNetHdrLen = int(unsafe.Sizeof(virtioNetHdr{}))
)

func gsoSplit(in []byte, hdr virtioNetHdr, outBuffs [][]byte, sizes []int, outOffset int, isV6 bool) (int, error) {
	iphLen := int(hdr.csumStart)
	srcAddrOffset := ipv6SrcAddrOffset
	addrLen := 16
	if !isV6 {
		in[10], in[11] = 0, 0 // clear ipv4 header checksum
		srcAddrOffset = ipv4SrcAddrOffset
		addrLen = 4
	}
	transportCsumAt := int(hdr.csumStart + hdr.csumOffset)
	in[transportCsumAt], in[transportCsumAt+1] = 0, 0 // clear tcp/udp checksum
	var firstTCPSeqNum uint32
	var protocol uint8
	if hdr.gsoType == unix.VIRTIO_NET_HDR_GSO_TCPV4 || hdr.gsoType == unix.VIRTIO_NET_HDR_GSO_TCPV6 {
		protocol = unix.IPPROTO_TCP
		firstTCPSeqNum = binary.BigEndian.Uint32(in[hdr.csumStart+4:])
	} else {
		protocol = unix.IPPROTO_UDP
	}
	nextSegmentDataAt := int(hdr.hdrLen)
	i := 0
	for ; nextSegmentDataAt < len(in); i++ {
		if i == len(outBuffs) {
			return i - 1, ErrTooManySegments
		}
		nextSegmentEnd := nextSegmentDataAt + int(hdr.gsoSize)
		if nextSegmentEnd > len(in) {
			nextSegmentEnd = len(in)
		}
		segmentDataLen := nextSegmentEnd - nextSegmentDataAt
		totalLen := int(hdr.hdrLen) + segmentDataLen
		sizes[i] = totalLen
		out := outBuffs[i][outOffset:]

		copy(out, in[:iphLen])
		if !isV6 {
			// For IPv4 we are responsible for incrementing the ID field,
			// updating the total len field, and recalculating the header
			// checksum.
			if i > 0 {
				id := binary.BigEndian.Uint16(out[4:])
				id += uint16(i)
				binary.BigEndian.PutUint16(out[4:], id)
			}
			binary.BigEndian.PutUint16(out[2:], uint16(totalLen))
			ipv4CSum := ^checksum(out[:iphLen], 0)
			binary.BigEndian.PutUint16(out[10:], ipv4CSum)
		} else {
			// For IPv6 we are responsible for updating the payload length field.
			binary.BigEndian.PutUint16(out[4:], uint16(totalLen-iphLen))
		}

		// copy transport header
		copy(out[hdr.csumStart:hdr.hdrLen], in[hdr.csumStart:hdr.hdrLen])

		if protocol == unix.IPPROTO_TCP {
			// set TCP seq and adjust TCP flags
			tcpSeq := firstTCPSeqNum + uint32(hdr.gsoSize*uint16(i))
			binary.BigEndian.PutUint32(out[hdr.csumStart+4:], tcpSeq)
			if nextSegmentEnd != len(in) {
				// FIN and PSH should only be set on last segment
				clearFlags := tcpFlagFIN | tcpFlagPSH
				out[hdr.csumStart+tcpFlagsOffset] &^= clearFlags
			}
		} else {
			// set UDP header len
			binary.BigEndian.PutUint16(out[hdr.csumStart+4:], uint16(segmentDataLen)+(hdr.hdrLen-hdr.csumStart))
		}

		// payload
		copy(out[hdr.hdrLen:], in[nextSegmentDataAt:nextSegmentEnd])

		// transport checksum
		transportHeaderLen := int(hdr.hdrLen - hdr.csumStart)
		lenForPseudo := uint16(transportHeaderLen + segmentDataLen)
		transportCSumNoFold := pseudoHeaderChecksumNoFold(protocol, in[srcAddrOffset:srcAddrOffset+addrLen], in[srcAddrOffset+addrLen:srcAddrOffset+addrLen*2], lenForPseudo)
		transportCSum := ^checksum(out[hdr.csumStart:totalLen], transportCSumNoFold)
		binary.BigEndian.PutUint16(out[hdr.csumStart+hdr.csumOffset:], transportCSum)

		nextSegmentDataAt += int(hdr.gsoSize)
	}
	return i, nil
}

func gsoNoneChecksum(in []byte, cSumStart, cSumOffset uint16) error {
	cSumAt := cSumStart + cSumOffset
	// The initial value at the checksum offset should be summed with the
	// checksum we compute. This is typically the pseudo-header checksum.
	initial := binary.BigEndian.Uint16(in[cSumAt:])
	in[cSumAt], in[cSumAt+1] = 0, 0
	binary.BigEndian.PutUint16(in[cSumAt:], ^checksum(in[cSumStart:], uint64(initial)))
	return nil
}
