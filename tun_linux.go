//go:build linux

package main

import (
	"fmt"
	"os"
	"unsafe"

	"github.com/vishvananda/netlink"
	"golang.org/x/sys/unix"
)

const (
	cIFF_TUN         = 0x0001
	cIFF_NO_PI       = 0x1000
	cIFNAMSIZ        = 16
	cTUNSETIFF       = 0x400454ca
)

type ifReq struct {
	Name  [cIFNAMSIZ]byte
	Flags uint16
	pad   [22]byte
}

// TUN encapsulates a /dev/net/tun device plus the netlink-managed
// interface state (link up + IP/route). MVP: single-queue, no GSO.
type TUN struct {
	dev *os.File
	mtu int
}

func newTUN(dev string, mtu int) (*os.File, error) {
	fd, err := unix.Open("/dev/net/tun", unix.O_RDWR|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, fmt.Errorf("open /dev/net/tun: %w", err)
	}
	var req ifReq
	req.Flags = cIFF_TUN | cIFF_NO_PI
	copy(req.Name[:], dev)
	if _, _, errno := unix.Syscall(unix.SYS_IOCTL, uintptr(fd), uintptr(cTUNSETIFF), uintptr(unsafe.Pointer(&req))); errno != 0 {
		_ = unix.Close(fd)
		return nil, fmt.Errorf("TUNSETIFF %s: %w", dev, errno)
	}
	return os.NewFile(uintptr(fd), "/dev/net/tun"), nil
}

// OpenTUN opens the TUN device, sets MTU, assigns the VIP, brings the
// link up. The VIP prefix length doubles as the on-link route.
func OpenTUN(dev string, mtu int, vip string) (*TUN, error) {
	parsedAddr, err := netlink.ParseAddr(vip)
	if err != nil {
		return nil, fmt.Errorf("parse vip %q: %w", vip, err)
	}
	f, err := newTUN(dev, mtu)
	if err != nil {
		return nil, err
	}
	link, err := netlink.LinkByName(dev)
	if err != nil {
		_ = f.Close()
		return nil, fmt.Errorf("LinkByName %s: %w", dev, err)
	}
	if err := netlink.LinkSetMTU(link, mtu); err != nil {
		_ = f.Close()
		return nil, fmt.Errorf("LinkSetMTU %s %d: %w", dev, mtu, err)
	}
	if err := netlink.AddrAdd(link, parsedAddr); err != nil {
		_ = f.Close()
		return nil, fmt.Errorf("AddrAdd %s %s: %w", dev, vip, err)
	}
	if err := netlink.LinkSetUp(link); err != nil {
		_ = f.Close()
		return nil, fmt.Errorf("LinkSetUp %s: %w", dev, err)
	}
	return &TUN{dev: f, mtu: mtu}, nil
}

func (t *TUN) Read(b []byte) (int, error)  { return t.dev.Read(b) }
func (t *TUN) Write(b []byte) (int, error) { return t.dev.Write(b) }
func (t *TUN) Close() error                { return t.dev.Close() }
func (t *TUN) MTU() int                    { return t.mtu }
