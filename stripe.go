package main

import (
	"errors"
	"log/slog"
	"net"
	"net/netip"
	"sync"
	"sync/atomic"

	"golang.org/x/sys/unix"
)

// peer is the dispatch entry for one remote VIP — N underlay AddrPort
// endpoints to stripe over, plus a per-peer round-robin counter that
// walks the full (srcs × dsts) matrix.
type peer struct {
	dsts []netip.AddrPort
	rr   atomic.Uint64
}

// Gofra is the data plane: N src sockets + peer table + N TUN queues.
// One tunReader goroutine per TUN queue pumps inner packets out via
// the stripe; one udpReader goroutine per UDP socket pumps inbound
// packets straight into a TUN queue.
type Gofra struct {
	socks  []*net.UDPConn
	peers  map[netip.Addr]*peer
	tuns   []*TUN
	logger *slog.Logger
}

func newGofra(cfg *parsedConfig, logger *slog.Logger) *Gofra {
	queues := len(cfg.Underlay)
	tuns := make([]*TUN, 0, queues)

	tuns = append(tuns, OpenTUN(cfg.TunDev, cfg.TunMTU, cfg.TunVIP.String()))
	logger.Info("tun ready", "dev", cfg.TunDev, "vip", cfg.TunVIP, "mtu", cfg.TunMTU, "queues", queues)

	for i := 1; i < queues; i++ {
		tuns = append(tuns, AttachTUN(cfg.TunDev, cfg.TunMTU))
	}

	socks := make([]*net.UDPConn, 0, len(cfg.Underlay))

	for _, src := range cfg.Underlay {
		ifname := ifaceForAddr(src)
		uc := openUDPSocket(src, int(cfg.ListenPort), ifname)

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
		tuns:   tuns,
		logger: logger,
	}
}

func closeAllSocks(socks []*net.UDPConn) {
	for _, s := range socks {
		_ = s.Close()
	}
}

func closeAllTUNs(tuns []*TUN) {
	for _, t := range tuns {
		_ = t.Close()
	}
}

// Run launches the data-plane goroutines and blocks until any of
// them returns a fatal error. Returns the first such error, or nil
// on clean shutdown.
func (g *Gofra) Run() error {
	var (
		mu       sync.Mutex
		firstErr *Exception
	)

	signal := func(e *Exception) {
		if e == nil {
			return
		}

		mu.Lock()
		defer mu.Unlock()

		if firstErr == nil {
			firstErr = e
			closeAllTUNs(g.tuns)
			closeAllSocks(g.socks)
		}
	}

	var wg sync.WaitGroup

	for i, t := range g.tuns {
		wg.Add(1)

		go func(idx int, tun *TUN) {
			defer wg.Done()

			signal(g.tunReader(idx, tun))
		}(i, t)
	}

	for i, sock := range g.socks {
		wg.Add(1)

		go func(idx int, s *net.UDPConn) {
			defer wg.Done()

			signal(g.udpReader(idx, s))
		}(i, sock)
	}

	wg.Wait()

	return firstErr.AsError()
}

// tunReader pumps inner packets from one TUN queue out via the
// stripe. Multi-queue TUN means the kernel hashes by 5-tuple into
// one of N queues, so multiple TCP flows can be processed in
// parallel by N independent goroutines.
//
// The TUN was opened with IFF_VNET_HDR + TUN_F_CSUM | TUN_F_TSO4
// so the kernel hands us up to ~64 KB super-segments with a 10-byte
// virtio_net_hdr prefix. For GSO_TCPV4 we segment into MTU-sized
// real packets in user-space; non-GSO packets pass through as-is
// (just stripped of their virtio_net_hdr prefix).
func (g *Gofra) tunReader(idx int, t *TUN) *Exception {
	return Try(func() {
		const maxGSOSegs = 64

		mtu := t.MTU()

		// 64 KB pseudo-segment + virtio_net_hdr + slack.
		buf := make([]byte, virtioNetHdrLen+65535+128)

		segBufs := make([][]byte, maxGSOSegs)
		segSizes := make([]int, maxGSOSegs)

		for i := range segBufs {
			segBufs[i] = make([]byte, mtu+128)
		}

		send := g.makeSender()

		for {
			n := Throw2(t.Read(buf))

			if n < virtioNetHdrLen {
				continue
			}

			var hdr virtioNetHdr
			Throw(hdr.decode(buf[:virtioNetHdrLen]))

			pkt := buf[virtioNetHdrLen:n]

			switch hdr.gsoType {
			case unix.VIRTIO_NET_HDR_GSO_NONE:
				if hdr.flags&unix.VIRTIO_NET_HDR_F_NEEDS_CSUM != 0 {
					if err := gsoNoneChecksum(pkt, hdr.csumStart, hdr.csumOffset); err != nil {
						g.logger.Warn("gsoNoneChecksum failed", "err", err)

						continue
					}
				}

				send(pkt)

			case unix.VIRTIO_NET_HDR_GSO_TCPV4:
				segs, err := gsoSplit(pkt, hdr, segBufs, segSizes, 0, false)
				if err != nil {
					g.logger.Warn("gsoSplit TCPv4 failed", "err", err)

					continue
				}

				for i := 0; i < segs; i++ {
					send(segBufs[i][:segSizes[i]])
				}

			default:
				g.logger.Debug("tun: unsupported gso_type", "type", hdr.gsoType)
			}
		}
	})
}

// makeSender returns the per-tunReader stripe-and-send closure.
// Filter-style: a single bad packet logs a warning and is skipped,
// not propagated.
func (g *Gofra) makeSender() func([]byte) {
	return func(payload []byte) {
		if len(payload) < 20 {
			return
		}

		dst, ok := dstFromIPv4(payload)
		if !ok {
			return
		}

		p, ok := g.peers[dst]
		if !ok {
			return
		}

		c := p.rr.Add(1) - 1
		srcIdx := int(c % uint64(len(g.socks)))
		dstIdx := int((c / uint64(len(g.socks))) % uint64(len(p.dsts)))

		if _, err := g.socks[srcIdx].WriteToUDPAddrPort(payload, p.dsts[dstIdx]); err != nil {
			if !errors.Is(err, net.ErrClosed) {
				g.logger.Warn("udp write failed", "src", srcIdx, "dst", p.dsts[dstIdx], "err", err)
			}
		}
	}
}

// udpReader pumps inbound UDP packets straight into a TUN queue.
// Uses recvmmsg(batch=64) — one syscall per batch instead of one
// per packet. Each udpReader writes to its corresponding TUN queue
// (round-robined by udpReader index); the kernel doesn't care which
// queue we use for the inbound write.
func (g *Gofra) udpReader(idx int, s *net.UDPConn) *Exception {
	return Try(func() {
		rc := Throw2(s.SyscallConn())

		// Reserve virtio_net_hdr at the start of each buffer so we can
		// tun.Write(buffer[:virtioNetHdrLen+pktLen]) without an extra
		// copy. The header bytes are zero-initialised at allocation —
		// no GSO, no checksum-offload hints.
		msgs, buffers := prepareRawMessages(udpRecvBatch, virtioNetHdrLen)
		tun := g.tuns[idx%len(g.tuns)]

		var (
			n    int
			done bool
		)

		reader := func(fd uintptr) bool {
			n, done = recvmmsg(fd, msgs)

			return done
		}

		for {
			Throw(rc.Read(reader))

			if !done {
				continue
			}

			for i := 0; i < n; i++ {
				pktLen := int(msgs[i].Len)

				if _, err := tun.Write(buffers[i][:virtioNetHdrLen+pktLen]); err != nil {
					if errors.Is(err, net.ErrClosed) {
						Throw(err)
					}

					g.logger.Warn("tun write failed", "src", idx, "err", err)
				}
			}
		}
	})
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
