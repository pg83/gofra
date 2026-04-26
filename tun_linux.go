//go:build linux

package main

import (
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

// TUN encapsulates one /dev/net/tun queue. The first queue owns the
// netlink config (MTU, IP, link-up); additional queues just attach
// more fds to the same dev via IFF_MULTI_QUEUE.
type TUN struct {
	dev *os.File
	mtu int
}

func openTUNFD(dev string, multi bool) *os.File {
	fd := Throw2(unix.Open("/dev/net/tun", unix.O_RDWR|unix.O_CLOEXEC, 0))

	var req ifReq
	req.Flags = cIFF_TUN | cIFF_NO_PI | cIFF_VNET_HDR

	if multi {
		req.Flags |= cIFF_MULTI_QUEUE
	}

	copy(req.Name[:], dev)

	if _, _, errno := unix.Syscall(unix.SYS_IOCTL, uintptr(fd), uintptr(cTUNSETIFF), uintptr(unsafe.Pointer(&req))); errno != 0 {
		_ = unix.Close(fd)
		ThrowFmt("TUNSETIFF %s: %v", dev, errno)
	}

	// Pin the virtio_net_hdr length to 10 bytes (struct virtio_net_hdr).
	// Newer kernels advertise virtio_net_hdr_v1 (12 bytes); the
	// vendored gsoSplit / virtioNetHdr layout assumes the legacy 10
	// bytes.
	hdrSize := uintptr(virtioNetHdrLen)

	if _, _, errno := unix.Syscall(unix.SYS_IOCTL, uintptr(fd), uintptr(cTUNSETVNETHDRSZ), uintptr(unsafe.Pointer(&hdrSize))); errno != 0 {
		_ = unix.Close(fd)
		ThrowFmt("TUNSETVNETHDRSZ %s: %v", dev, errno)
	}

	// Tell kernel we'll handle TCPv4 GSO + checksum offload. Without
	// this, IFF_VNET_HDR-mode reads still work but the header always
	// reports gso_type=NONE and we lose the win.
	off := uintptr(cTUN_F_CSUM | cTUN_F_TSO4)

	if _, _, errno := unix.Syscall(unix.SYS_IOCTL, uintptr(fd), uintptr(cTUNSETOFFLOAD), off); errno != 0 {
		_ = unix.Close(fd)
		ThrowFmt("TUNSETOFFLOAD %s: %v", dev, errno)
	}

	return os.NewFile(uintptr(fd), "/dev/net/tun")
}

// OpenTUN opens the primary TUN fd, sets MTU, assigns the VIP, brings
// the link up. Always opens with IFF_MULTI_QUEUE so additional queues
// can attach later via AttachTUN.
func OpenTUN(dev string, mtu int, vip string) *TUN {
	parsedAddr := Throw2(netlink.ParseAddr(vip))

	f := openTUNFD(dev, true)

	link := Throw2(netlink.LinkByName(dev))
	Throw(netlink.LinkSetMTU(link, mtu))
	Throw(netlink.AddrAdd(link, parsedAddr))
	Throw(netlink.LinkSetUp(link))

	return &TUN{dev: f, mtu: mtu}
}

// AttachTUN opens an additional queue on an already-up TUN device.
// No netlink touches — just another fd attached to the same dev with
// IFF_MULTI_QUEUE set.
func AttachTUN(dev string, mtu int) *TUN {
	return &TUN{dev: openTUNFD(dev, true), mtu: mtu}
}

// Read / Write keep error returns to satisfy io.Reader / io.Writer.
func (t *TUN) Read(b []byte) (int, error)  { return t.dev.Read(b) }
func (t *TUN) Write(b []byte) (int, error) { return t.dev.Write(b) }
func (t *TUN) Close() error                { return t.dev.Close() }
func (t *TUN) MTU() int                    { return t.mtu }
