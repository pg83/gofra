package main

import (
	"encoding/binary"
	"errors"
	"log/slog"
	"net"
	"net/netip"
	"sync"
	"sync/atomic"
	"syscall"

	"golang.org/x/sys/unix"
)

const (
	// wireSeqLen is the 4-byte monotonic seq prepended to every
	// outgoing UDP datagram. Receiver strips it before delivery.
	wireSeqLen = 4
)

// peer is the dispatch entry for one remote VIP. Holds the underlay
// dst endpoints to stripe over, the per-peer RR counter, the
// per-peer outbound seq counter, and the per-peer reorder pipeline
// used on the inbound side.
//
// dstSockAddrs is the parallel pre-encoded sockaddr_in form of
// dsts (16 bytes each) so the TX hot path can copy a fixed
// 16-byte blob into the sendmmsg msghdr name slot without
// re-deriving AF_INET / port bytes per packet.
type peer struct {
	dsts         []netip.AddrPort
	dstSockAddrs [][16]byte
	rr           atomic.Uint64
	txSeq        atomic.Uint32
	rx           *reorderPipe
}

// Gofra is the data plane: N src sockets + peer table + N TUN queues.
// One tunReader goroutine per TUN queue pumps inner packets out via
// the stripe; one udpReader goroutine per UDP socket pumps inbound
// packets into the matching peer's reorder pipeline.
type Gofra struct {
	socks  []*net.UDPConn
	peers  map[netip.Addr]*peer
	// peerByDst lets udpReader resolve a from-addr (one of any
	// peer's dsts) to the owning peer in O(1).
	peerByDst map[netip.AddrPort]*peer
	tuns      []*TUN
	cfg       *parsedConfig
	logger    *slog.Logger
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
		uc := openUDPSocket(src, int(cfg.ListenPort), ifname, cfg.UdpRecvBuf, cfg.UdpSendBuf)

		logger.Info("udp ready", "src", src, "iface", ifname, "port", cfg.ListenPort, "rcvbuf", cfg.UdpRecvBuf, "sndbuf", cfg.UdpSendBuf)

		socks = append(socks, uc)
	}

	peers := make(map[netip.Addr]*peer, len(cfg.PeerByVIP))
	peerByDst := make(map[netip.AddrPort]*peer)

	for vip, ips := range cfg.PeerByVIP {
		dsts := make([]netip.AddrPort, len(ips))

		for i, ip := range ips {
			dsts[i] = netip.AddrPortFrom(ip, cfg.ListenPort)
		}

		writers := make([]tunWriter, len(tuns))

		for i, t := range tuns {
			writers[i] = t
		}

		dstSockAddrs := make([][16]byte, len(dsts))

		for i, d := range dsts {
			fillSockaddrV4(dstSockAddrs[i][:], d)
		}

		p := &peer{
			dsts:         dsts,
			dstSockAddrs: dstSockAddrs,
			rx:           newReorderPipe(cfg.Timeout, writers, logger),
		}
		peers[vip] = p

		for _, d := range dsts {
			peerByDst[d] = p
		}

		logger.Info("peer registered",
			"vip", vip,
			"dsts", dsts,
			"timeout", cfg.Timeout,
		)
	}

	return &Gofra{
		socks:     socks,
		peers:     peers,
		peerByDst: peerByDst,
		tuns:      tuns,
		cfg:       cfg,
		logger:    logger,
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

			for _, p := range g.peers {
				p.rx.close()
			}

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
// Every wire packet gets a 4-byte monotonic seq prepended for the
// receiver's reorder pipeline.
func (g *Gofra) tunReader(idx int, t *TUN) *Exception {
	return Try(func() {
		const maxGSOSegs = 64

		mtu := t.MTU()

		buf := make([]byte, virtioNetHdrLen+65535+128)

		segBufs := make([][]byte, maxGSOSegs)
		segSizes := make([]int, maxGSOSegs)

		for i := range segBufs {
			segBufs[i] = make([]byte, mtu+128)
		}

		// One sendQueue per src socket. Worst case all maxGSOSegs
		// segments from one superframe hash to the same src, so
		// each queue is sized for that.
		queues := make([]*sendQueue, len(g.socks))
		rcs := make([]syscall.RawConn, len(g.socks))

		for i, s := range g.socks {
			queues[i] = newSendQueue(maxGSOSegs, mtu)
			rcs[i] = Throw2(s.SyscallConn())
		}

		queue := func(payload []byte) {
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

			seq := p.txSeq.Add(1) - 1

			queues[srcIdx].push(p.dstSockAddrs[dstIdx], seq, payload)
		}

		flush := func() {
			for srcIdx, q := range queues {
				if q.n == 0 {
					continue
				}

				wantN := q.n

				err := rcs[srcIdx].Write(func(fd uintptr) bool {
					sent, done := sendmmsg(fd, q.msgs[:q.n])

					if !done {
						return false
					}

					if sent < q.n {
						g.logger.Warn("sendmmsg partial", "src", srcIdx, "sent", sent, "total", q.n)
					}

					return true
				})

				if err != nil && !errors.Is(err, net.ErrClosed) {
					g.logger.Warn("sendmmsg control failed", "src", srcIdx, "want", wantN, "err", err)
				}

				q.n = 0
			}
		}

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

				queue(pkt)

			case unix.VIRTIO_NET_HDR_GSO_TCPV4:
				segs, err := gsoSplit(pkt, hdr, segBufs, segSizes, 0, false)
				if err != nil {
					g.logger.Warn("gsoSplit TCPv4 failed", "err", err)

					continue
				}

				for i := 0; i < segs; i++ {
					queue(segBufs[i][:segSizes[i]])
				}

			default:
				g.logger.Debug("tun: unsupported gso_type", "type", hdr.gsoType)
			}

			// One sendmmsg per used src socket per superframe.
			// All segments queued from this superframe ship in
			// at most len(g.socks) syscalls instead of one per
			// segment.
			flush()
		}
	})
}

// sendQueue is a per-(tunReader, src socket) batched-send buffer.
// Each slot owns its own scratch payload, iovec, and sockaddr_in
// storage; the parallel msgs[] array is laid out as the kernel
// expects for sendmmsg(2). slot wiring (msg.Hdr.Iov, msg.Hdr.Name)
// is set up once at construction; per-push only updates iov.Len,
// the address bytes, and the payload contents.
type sendQueue struct {
	msgs  []rawMessage
	iovs  []iovec
	addrs [][16]byte
	bufs  [][]byte
	n     int
}

func newSendQueue(maxN, mtu int) *sendQueue {
	q := &sendQueue{
		msgs:  make([]rawMessage, maxN),
		iovs:  make([]iovec, maxN),
		addrs: make([][16]byte, maxN),
		bufs:  make([][]byte, maxN),
	}

	for i := 0; i < maxN; i++ {
		q.bufs[i] = make([]byte, wireSeqLen+mtu+128)

		q.iovs[i].Base = &q.bufs[i][0]
		q.msgs[i].Hdr.Iov = &q.iovs[i]
		q.msgs[i].Hdr.Iovlen = 1
		q.msgs[i].Hdr.Name = &q.addrs[i][0]
		q.msgs[i].Hdr.Namelen = 16
	}

	return q
}

// push fills the next slot with [seq 4B][payload] and the dst
// sockaddr_in. Caller is responsible for not exceeding the queue's
// preallocated size — sized for maxGSOSegs at construction.
func (q *sendQueue) push(dst [16]byte, seq uint32, payload []byte) {
	slot := q.n

	binary.BigEndian.PutUint32(q.bufs[slot][:wireSeqLen], seq)
	copy(q.bufs[slot][wireSeqLen:], payload)

	q.iovs[slot].Len = uint64(wireSeqLen + len(payload))
	q.addrs[slot] = dst

	q.n++
}

// udpReader pumps inbound UDP packets through to the reorder
// pipeline. recvmmsg drains the kernel — every syscall returns
// 1+ packets in one go, which we group by peer and ship as one
// batch per peer per recvmmsg call. No userspace accumulator,
// no timer: the reorder goroutine downstream is the sole hold
// point in the pipeline.
func (g *Gofra) udpReader(idx int, s *net.UDPConn) *Exception {
	return Try(func() {
		rc := Throw2(s.SyscallConn())

		msgs, buffers, names := prepareRawMessages(g.cfg.UdpRecvBatch)

		// Per-recvmmsg grouping. Reused across iterations.
		grouped := make(map[*peer][]rxItem)

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
				wireLen := int(msgs[i].Len)

				if wireLen < wireSeqLen+20 {
					continue
				}

				seq := binary.BigEndian.Uint32(buffers[i][:wireSeqLen])
				inner := buffers[i][wireSeqLen:wireLen]

				from := fromV4(names[i])

				p, ok := g.peerByDst[from]
				if !ok {
					g.logger.Debug("udp: drop packet from unknown peer src", "from", from)

					continue
				}

				// Allocate item with the virtio prefix already
				// in place (zero) and the inner payload copied
				// in. The writer can tun.Write(item.payload)
				// directly — no further copies on the hot path.
				payload := make([]byte, virtioNetHdrLen+len(inner))
				copy(payload[virtioNetHdrLen:], inner)

				grouped[p] = append(grouped[p], rxItem{seq: seq, payload: payload})
			}

			for p, items := range grouped {
				p.rx.in <- &batch{items: items}
				delete(grouped, p)
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
