package main

import (
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/netip"
	"sync"
	"sync/atomic"
)

// peer is the dispatch entry for one remote VIP — N underlay AddrPort
// endpoints to stripe over, plus a per-peer round-robin counter that
// walks the full (srcs × dsts) matrix.
type peer struct {
	dsts []netip.AddrPort
	rr   atomic.Uint64
}

// Gofra is the data plane: 4 src sockets + peer table + TUN device.
// One TUN-reader goroutine pumps inner packets; per-socket reader
// goroutines pump UDP receive into the TUN.
type Gofra struct {
	socks  []*net.UDPConn
	peers  map[netip.Addr]*peer
	tun    *TUN
	logger *slog.Logger
}

func newGofra(cfg *parsedConfig, logger *slog.Logger) (*Gofra, error) {
	tun, err := OpenTUN(cfg.TunDev, cfg.TunMTU, cfg.TunVIP.String())
	if err != nil {
		return nil, err
	}
	logger.Info("tun ready", "dev", cfg.TunDev, "vip", cfg.TunVIP, "mtu", cfg.TunMTU)

	socks := make([]*net.UDPConn, 0, len(cfg.Underlay))
	for _, src := range cfg.Underlay {
		ifname, err := ifaceForAddr(src)
		if err != nil {
			closeAll(socks)
			_ = tun.Close()
			return nil, fmt.Errorf("src %s: %w", src, err)
		}
		uc, err := openUDPSocket(src, int(cfg.ListenPort), ifname)
		if err != nil {
			closeAll(socks)
			_ = tun.Close()
			return nil, err
		}
		logger.Info("udp ready", "src", src, "iface", ifname, "port", cfg.ListenPort)
		socks = append(socks, uc)
	}

	peers := make(map[netip.Addr]*peer, len(cfg.PeerByVIP))
	for vip, ips := range cfg.PeerByVIP {
		dsts := make([]netip.AddrPort, len(ips))
		for i, ip := range ips {
			dsts[i] = netip.AddrPortFrom(ip, cfg.ListenPort)
		}
		peers[vip] = &peer{dsts: dsts}
		logger.Info("peer registered", "vip", vip, "dsts", dsts)
	}

	return &Gofra{
		socks:  socks,
		peers:  peers,
		tun:    tun,
		logger: logger,
	}, nil
}

func closeAll(socks []*net.UDPConn) {
	for _, s := range socks {
		_ = s.Close()
	}
}

// Run launches the data-plane goroutines and blocks until any of
// them returns a fatal error.
func (g *Gofra) Run() error {
	var (
		mu       sync.Mutex
		fatalErr error
	)
	signal := func(err error) {
		if err == nil {
			return
		}
		mu.Lock()
		if fatalErr == nil {
			fatalErr = err
			_ = g.tun.Close()
			closeAll(g.socks)
		}
		mu.Unlock()
	}

	var wg sync.WaitGroup

	wg.Add(1)
	go func() {
		defer wg.Done()
		signal(g.tunReader())
	}()

	for i, sock := range g.socks {
		wg.Add(1)
		go func(idx int, s *net.UDPConn) {
			defer wg.Done()
			signal(g.udpReader(idx, s))
		}(i, sock)
	}

	wg.Wait()
	return fatalErr
}

// tunReader pumps inner packets from TUN out via the stripe. One
// goroutine — TUN reads aren't safe to parallelise from a single
// non-multiqueue file descriptor.
func (g *Gofra) tunReader() error {
	mtu := g.tun.MTU()
	buf := make([]byte, mtu+128)
	for {
		n, err := g.tun.Read(buf)
		if err != nil {
			return fmt.Errorf("tun read: %w", err)
		}
		if n < 20 {
			g.logger.Debug("tun: short packet", "n", n)
			continue
		}
		dst, ok := dstFromIPv4(buf[:n])
		if !ok {
			g.logger.Debug("tun: not ipv4", "first_byte", buf[0])
			continue
		}
		p, ok := g.peers[dst]
		if !ok {
			g.logger.Debug("tun: no peer for dst", "dst", dst)
			continue
		}
		c := p.rr.Add(1) - 1
		srcIdx := int(c % uint64(len(g.socks)))
		dstIdx := int((c / uint64(len(g.socks))) % uint64(len(p.dsts)))
		if _, err := g.socks[srcIdx].WriteToUDPAddrPort(buf[:n], p.dsts[dstIdx]); err != nil {
			if errors.Is(err, net.ErrClosed) {
				return err
			}
			g.logger.Warn("udp write failed", "src", srcIdx, "dst", p.dsts[dstIdx], "err", err)
		}
	}
}

// udpReader pumps inbound UDP packets directly into the TUN. The
// inner IP packet is self-describing — kernel routes by inner dst
// once tun.Write delivers it.
func (g *Gofra) udpReader(idx int, s *net.UDPConn) error {
	buf := make([]byte, 65536)
	for {
		n, _, err := s.ReadFromUDPAddrPort(buf)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return err
			}
			return fmt.Errorf("udp read src[%d]: %w", idx, err)
		}
		if _, err := g.tun.Write(buf[:n]); err != nil {
			if errors.Is(err, net.ErrClosed) {
				return err
			}
			g.logger.Warn("tun write failed", "src", idx, "err", err)
		}
	}
}

// dstFromIPv4 pulls bytes 16..19 (dst addr) out of an IPv4 header.
// Refuses non-v4 first nibble.
func dstFromIPv4(p []byte) (netip.Addr, bool) {
	if len(p) < 20 {
		return netip.Addr{}, false
	}
	if (p[0] >> 4) != 4 {
		return netip.Addr{}, false
	}
	var raw [4]byte
	copy(raw[:], p[16:20])
	return netip.AddrFrom4(raw), true
}
