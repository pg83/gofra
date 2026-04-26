//go:build linux

package main

import (
	"context"
	"encoding/binary"
	"net"
	"net/netip"
	"syscall"
	"unsafe"

	"golang.org/x/sys/unix"
)

const (
	udpMTU = 65536
)

// openUDPSocket binds a UDP socket on (src, port), pins egress to
// the named iface via SO_BINDTODEVICE, and bumps SO_RCVBUF/SO_SNDBUF
// to the configured size. SO_*BUFFORCE is used so the kernel ignores
// rmem_max/wmem_max if they're set lower.
func openUDPSocket(src netip.Addr, port int, ifname string, rcvBuf, sndBuf int) *net.UDPConn {
	listenAddr := netip.AddrPortFrom(src, uint16(port))

	var lc net.ListenConfig
	pc := Throw2(lc.ListenPacket(context.Background(), "udp", listenAddr.String()))

	uc, ok := pc.(*net.UDPConn)
	if !ok {
		_ = pc.Close()
		ThrowFmt("ListenPacket returned %T, want *net.UDPConn", pc)
	}

	rawConn := Throw2(uc.SyscallConn())

	var setErr error
	Throw(rawConn.Control(func(fd uintptr) {
		if err := unix.SetsockoptString(int(fd), unix.SOL_SOCKET, unix.SO_BINDTODEVICE, ifname); err != nil {
			setErr = Fmt("SO_BINDTODEVICE %s: %v", ifname, err).AsError()

			return
		}

		if err := unix.SetsockoptInt(int(fd), unix.SOL_SOCKET, unix.SO_RCVBUFFORCE, rcvBuf); err != nil {
			setErr = Fmt("SO_RCVBUFFORCE: %v", err).AsError()

			return
		}

		if err := unix.SetsockoptInt(int(fd), unix.SOL_SOCKET, unix.SO_SNDBUFFORCE, sndBuf); err != nil {
			setErr = Fmt("SO_SNDBUFFORCE: %v", err).AsError()

			return
		}
	}))

	if setErr != nil {
		_ = uc.Close()
		Throw(setErr)
	}

	return uc
}

// ifaceForAddr returns the interface name that owns `addr`. Loopback
// (127.0.0.0/8 / ::1) short-circuits to "lo".
func ifaceForAddr(addr netip.Addr) string {
	if addr.IsLoopback() {
		return "lo"
	}

	ifs := Throw2(net.Interfaces())

	for _, ifc := range ifs {
		addrs, err := ifc.Addrs()
		if err != nil {
			continue
		}

		for _, a := range addrs {
			ipnet, ok := a.(*net.IPNet)
			if !ok {
				continue
			}

			ip, ok := netip.AddrFromSlice(ipnet.IP)
			if !ok {
				continue
			}

			if ip.Unmap() == addr {
				return ifc.Name
			}
		}
	}

	ThrowFmt("no iface owns address %v", addr)

	return ""
}

// rawMessage / iovec / msghdr layout for SYS_RECVMMSG. amd64 only —
// gofra is server-side, no 32-bit support intended.
type iovec struct {
	Base *byte
	Len  uint64
}

type msghdr struct {
	Name       *byte
	Namelen    uint32
	pad0       uint32
	Iov        *iovec
	Iovlen     uint64
	Control    *byte
	Controllen uint64
	Flags      int32
	pad1       int32
}

type rawMessage struct {
	Hdr msghdr
	Len uint32
	pad uint32
}

// prepareRawMessages allocates the buffers + sockaddr_in storage
// for a recvmmsg batch of size n. We need the source address per
// packet so the receiver can dispatch into the right peer's reorder
// ring; sockaddr_in is 16 bytes, sockaddr_in6 is 28 — pin to 28 so
// either fits.
func prepareRawMessages(n int) (msgs []rawMessage, buffers, names [][]byte) {
	msgs = make([]rawMessage, n)
	buffers = make([][]byte, n)
	names = make([][]byte, n)
	iovs := make([]iovec, n)

	for i := range msgs {
		buffers[i] = make([]byte, udpMTU)
		names[i] = make([]byte, 28)

		iovs[i].Base = &buffers[i][0]
		iovs[i].Len = uint64(udpMTU)

		msgs[i].Hdr.Iov = &iovs[i]
		msgs[i].Hdr.Iovlen = 1
		msgs[i].Hdr.Name = &names[i][0]
		msgs[i].Hdr.Namelen = 28
	}

	return msgs, buffers, names
}

// recvmmsg blocks (via the rawConn) until the kernel hands us 1+
// packets. Returns (count, done) where done=false means EAGAIN —
// no data yet, the rawConn machinery should re-arm and retry. Real
// errors panic via Throw.
func recvmmsg(fd uintptr, msgs []rawMessage) (int, bool) {
	n, _, errno := unix.Syscall6(
		unix.SYS_RECVMMSG,
		fd,
		uintptr(unsafe.Pointer(&msgs[0])),
		uintptr(len(msgs)),
		unix.MSG_WAITFORONE,
		0,
		0,
	)

	if errno == syscall.EAGAIN || errno == syscall.EWOULDBLOCK {
		return int(n), false
	}

	if errno != 0 {
		ThrowFmt("recvmmsg: %v", errno)
	}

	return int(n), true
}

// fromV4 extracts (ip, port) from a sockaddr_in laid out at
// names[i][0:16]. Bytes 0..1 = sa_family, 2..4 = port (big-endian),
// 4..8 = ipv4 addr.
func fromV4(name []byte) netip.AddrPort {
	port := binary.BigEndian.Uint16(name[2:4])
	var ip [4]byte
	copy(ip[:], name[4:8])

	return netip.AddrPortFrom(netip.AddrFrom4(ip), port)
}
