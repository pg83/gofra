//go:build linux

package main

import (
	"context"
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

// prepareRawMessagesOffset allocates buffers for a recvmmsg batch
// of size n. recvmmsg writes payload starting at buffers[i][offset]
// — so the first `offset` bytes stay untouched and zero, ready as
// a virtio_net_hdr prefix the udpReader can hand to tun.Write
// without an extra copy. No sockaddr_in storage: the receive side
// no longer needs to dispatch by src.
func prepareRawMessagesOffset(n, offset int) (msgs []rawMessage, buffers [][]byte) {
	msgs = make([]rawMessage, n)
	buffers = make([][]byte, n)
	iovs := make([]iovec, n)

	for i := range msgs {
		buffers[i] = make([]byte, offset+udpMTU)

		iovs[i].Base = &buffers[i][offset]
		iovs[i].Len = uint64(udpMTU)

		msgs[i].Hdr.Iov = &iovs[i]
		msgs[i].Hdr.Iovlen = 1
	}

	return msgs, buffers
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

