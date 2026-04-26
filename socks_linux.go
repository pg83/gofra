//go:build linux

package main

import (
	"context"
	"fmt"
	"net"
	"net/netip"
	"syscall"
	"unsafe"

	"golang.org/x/sys/unix"
)

const (
	udpRecvBatch = 64
	udpRecvBuf   = 8 << 20
	udpSendBuf   = 8 << 20
	udpMTU       = 65536
)

// openUDPSocket binds a UDP socket on (src, port), pins egress to
// the named iface via SO_BINDTODEVICE, and bumps SO_RCVBUF/SO_SNDBUF
// to 8 MB. SO_*BUFFORCE is used so the kernel ignores
// rmem_max/wmem_max if they're set lower.
func openUDPSocket(src netip.Addr, port int, ifname string) (*net.UDPConn, error) {
	listenAddr := netip.AddrPortFrom(src, uint16(port))
	var lc net.ListenConfig
	pc, err := lc.ListenPacket(context.Background(), "udp", listenAddr.String())
	if err != nil {
		return nil, fmt.Errorf("bind udp %s: %w", listenAddr, err)
	}
	uc, ok := pc.(*net.UDPConn)
	if !ok {
		_ = pc.Close()
		return nil, fmt.Errorf("ListenPacket returned %T, want *net.UDPConn", pc)
	}
	rawConn, err := uc.SyscallConn()
	if err != nil {
		_ = uc.Close()
		return nil, fmt.Errorf("SyscallConn %s: %w", listenAddr, err)
	}
	var setErr error
	if err := rawConn.Control(func(fd uintptr) {
		if err := unix.SetsockoptString(int(fd), unix.SOL_SOCKET, unix.SO_BINDTODEVICE, ifname); err != nil {
			setErr = fmt.Errorf("SO_BINDTODEVICE %s: %w", ifname, err)
			return
		}
		if err := unix.SetsockoptInt(int(fd), unix.SOL_SOCKET, unix.SO_RCVBUFFORCE, udpRecvBuf); err != nil {
			setErr = fmt.Errorf("SO_RCVBUFFORCE: %w", err)
			return
		}
		if err := unix.SetsockoptInt(int(fd), unix.SOL_SOCKET, unix.SO_SNDBUFFORCE, udpSendBuf); err != nil {
			setErr = fmt.Errorf("SO_SNDBUFFORCE: %w", err)
			return
		}
	}); err != nil {
		_ = uc.Close()
		return nil, fmt.Errorf("rawconn control %s: %w", listenAddr, err)
	}
	if setErr != nil {
		_ = uc.Close()
		return nil, fmt.Errorf("setsockopts on %s: %w", listenAddr, setErr)
	}
	return uc, nil
}

// ifaceForAddr returns the interface name that owns `addr`. Loopback
// (127.0.0.0/8 / ::1) short-circuits to "lo".
func ifaceForAddr(addr netip.Addr) (string, error) {
	if addr.IsLoopback() {
		return "lo", nil
	}
	ifs, err := net.Interfaces()
	if err != nil {
		return "", fmt.Errorf("net.Interfaces: %w", err)
	}
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
				return ifc.Name, nil
			}
		}
	}
	return "", fmt.Errorf("no iface owns address %v", addr)
}

// rawConnFD pulls the raw fd out of a *net.UDPConn for use with
// SYS_RECVMMSG. The returned RawConn must outlive the syscall — we
// hold a reference for the lifetime of the udpReader.
func rawConnFD(uc *net.UDPConn) (syscall.RawConn, error) {
	rc, err := uc.SyscallConn()
	if err != nil {
		return nil, err
	}
	return rc, nil
}

// rawMessage / iovec / msghdr layout for SYS_RECVMMSG. Mirrors what
// nebula's udp_linux_64.go does on amd64; we don't try to be portable
// to 32-bit since gofra is server-side only.
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

// prepareRawMessages allocates the buffers / msghdr scaffolding for
// a recvmmsg batch of size n. We don't need sockaddr_in (the inner
// IP packet self-describes the source), so msghdr.Name is left nil.
func prepareRawMessages(n int) ([]rawMessage, [][]byte) {
	msgs := make([]rawMessage, n)
	buffers := make([][]byte, n)
	iovs := make([]iovec, n)

	for i := range msgs {
		buffers[i] = make([]byte, udpMTU)
		iovs[i].Base = &buffers[i][0]
		iovs[i].Len = uint64(len(buffers[i]))

		msgs[i].Hdr.Iov = &iovs[i]
		msgs[i].Hdr.Iovlen = 1
	}
	return msgs, buffers
}

// recvmmsg blocks (via the rawConn) until the kernel hands us 1+
// packets. Returns the count actually received. EAGAIN is treated as
// "try again, no fatal error".
func recvmmsg(fd uintptr, msgs []rawMessage) (int, bool, error) {
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
		return int(n), false, nil
	}
	if errno != 0 {
		return int(n), true, &net.OpError{Op: "recvmmsg", Err: errno}
	}
	return int(n), true, nil
}

