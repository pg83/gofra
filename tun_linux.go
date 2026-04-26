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
	cIFF_MULTI_QUEUE = 0x0100
	cIFF_VNET_HDR    = 0x4000
	cIFNAMSIZ        = 16
	cTUNSETIFF       = 0x400454ca
	cTUNSETOFFLOAD   = 0x400454d0
	cTUNSETVNETHDRSZ = 0x400454d8

	// Linux uapi/linux/if_tun.h:
	cTUN_F_CSUM    = 0x01
	cTUN_F_TSO4    = 0x02
	cTUN_F_TSO6    = 0x04
	cTUN_F_TSO_ECN = 0x08
	cTUN_F_UFO     = 0x10
)

type ifReq struct {
	Name  [cIFNAMSIZ]byte
	Flags uint16
	pad   [22]byte
}

// TUN encapsulates one /dev/net/tun queue + the netlink-managed
// interface state (link up + IP/route). For multi-queue, the first
// TUN owns the netlink config; subsequent queues just attach more
// fds to the same dev.
type TUN struct {
	dev *os.File
	mtu int
}

func openTUNFD(dev string, multi bool) (*os.File, error) {
	fd, err := unix.Open("/dev/net/tun", unix.O_RDWR|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, fmt.Errorf("open /dev/net/tun: %w", err)
	}
	var req ifReq
	req.Flags = cIFF_TUN | cIFF_NO_PI | cIFF_VNET_HDR
	if multi {
		req.Flags |= cIFF_MULTI_QUEUE
	}
	copy(req.Name[:], dev)
	if _, _, errno := unix.Syscall(unix.SYS_IOCTL, uintptr(fd), uintptr(cTUNSETIFF), uintptr(unsafe.Pointer(&req))); errno != 0 {
		_ = unix.Close(fd)
		return nil, fmt.Errorf("TUNSETIFF %s: %w", dev, errno)
	}
	// Pin the virtio_net_hdr length to 10 bytes (struct virtio_net_hdr).
	// Newer kernels advertise virtio_net_hdr_v1 (12 bytes); we only
	// support the legacy layout in segmentTCPv4 / parseVirtioNetHdr.
	hdrSize := uintptr(virtioNetHdrLen)
	if _, _, errno := unix.Syscall(unix.SYS_IOCTL, uintptr(fd), uintptr(cTUNSETVNETHDRSZ), uintptr(unsafe.Pointer(&hdrSize))); errno != 0 {
		_ = unix.Close(fd)
		return nil, fmt.Errorf("TUNSETVNETHDRSZ %s: %w", dev, errno)
	}
	// Tell kernel we'll handle TCPv4 GSO + checksum offload. Without
	// this, IFF_VNET_HDR-mode reads still work but the header always
	// reports gso_type=NONE and we lose the win.
	off := uintptr(cTUN_F_CSUM | cTUN_F_TSO4)
	if _, _, errno := unix.Syscall(unix.SYS_IOCTL, uintptr(fd), uintptr(cTUNSETOFFLOAD), off); errno != 0 {
		_ = unix.Close(fd)
		return nil, fmt.Errorf("TUNSETOFFLOAD %s: %w", dev, errno)
	}
	return os.NewFile(uintptr(fd), "/dev/net/tun"), nil
}

// OpenTUN opens the primary TUN fd, sets MTU, assigns the VIP, brings
// the link up. The VIP prefix length doubles as the on-link route.
// Always opens with IFF_MULTI_QUEUE so additional queues can attach
// later via AttachTUN.
func OpenTUN(dev string, mtu int, vip string) (*TUN, error) {
	parsedAddr, err := netlink.ParseAddr(vip)
	if err != nil {
		return nil, fmt.Errorf("parse vip %q: %w", vip, err)
	}
	f, err := openTUNFD(dev, true)
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

// AttachTUN opens an additional queue on an already-up TUN device.
// No netlink touches — just another fd attached to the same dev with
// IFF_MULTI_QUEUE set.
func AttachTUN(dev string, mtu int) (*TUN, error) {
	f, err := openTUNFD(dev, true)
	if err != nil {
		return nil, err
	}
	return &TUN{dev: f, mtu: mtu}, nil
}

func (t *TUN) Read(b []byte) (int, error)  { return t.dev.Read(b) }
func (t *TUN) Write(b []byte) (int, error) { return t.dev.Write(b) }
func (t *TUN) Close() error                { return t.dev.Close() }
func (t *TUN) MTU() int                    { return t.mtu }
