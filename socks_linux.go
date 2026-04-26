//go:build linux

package main

import (
	"context"
	"fmt"
	"net"
	"net/netip"

	"golang.org/x/sys/unix"
)

// openUDPSocket binds a UDP socket on (src, port) and forces every
// packet to egress via the named iface (SO_BINDTODEVICE). Without
// the latter, multiple sockets in the same /24 unify on the
// default-route NIC and stripe never reaches hardware.
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
		setErr = unix.SetsockoptString(int(fd), unix.SOL_SOCKET, unix.SO_BINDTODEVICE, ifname)
	}); err != nil {
		_ = uc.Close()
		return nil, fmt.Errorf("rawconn control %s: %w", listenAddr, err)
	}
	if setErr != nil {
		_ = uc.Close()
		return nil, fmt.Errorf("SO_BINDTODEVICE %s on %s: %w", ifname, listenAddr, setErr)
	}
	return uc, nil
}

// ifaceForAddr returns the interface name that owns `addr`. Mirrors
// the helper in our nebula fork — single source of truth for src→NIC
// mapping based on net.Interfaces() (i.e. configured IPs).
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
