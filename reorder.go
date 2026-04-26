package main

import (
	"log/slog"
	"sort"
	"time"
)

// reorderPipe is the per-peer pipeline that turns the unordered
// stripe-output coming from N udpReaders back into something kernel
// TCP can chew on without spurious retransmits, while keeping
// throughput high.
//
// Shape:
//
//   N udpReader goroutines  (one per src socket)
//      │   batch up to batchSize packets, ship to peer's `in`
//      ▼   (idle batches flushed by their owning udpReader)
//   1 reorder goroutine per peer
//      │   accumulate batches until `window` items or `timeout`,
//      │   THEN bucket items by seq % numWriters into M sub-slices
//      │   (no sort here — that's the writers' job),
//      │   ship each sub-slice as one channel send.
//      ▼
//   M writer goroutines, one per TUN queue
//      │   sort own sub-slice (~window/M items, O(n log n) on a
//      │   tiny n), tun.Write each in seq order.
//
// Why distribute-then-sort instead of sort-then-distribute:
// parallelises the sort over M cores. Each writer sorts only its
// share. The whole pipeline is lock-free outside channel ops.
//
// Cross-queue ordering at kernel softirq is best-effort, but each
// queue's input is monotone by seq (after writer sort), and writers
// progress roughly in lock-step, so the kernel-side reorder
// distance is microseconds — well within tcp_reordering's
// tolerance.
// tunWriter is the minimal surface reorderPipe needs from a TUN
// device. Real *TUN satisfies it; tests pass a fake that just
// counts writes.
type tunWriter interface {
	Write(b []byte) (int, error)
}

type reorderPipe struct {
	in   chan *batch
	outs []chan []rxItem
	stop chan struct{}

	window  int
	timeout time.Duration
	logger  *slog.Logger
}

const (
	batchSize = 64
	// batchFlushPeriod bounds how long a partly-filled batch sits
	// in a udpReader before being shipped to the reorder
	// goroutine. Short enough to not noticeably add to the
	// user-visible reorder latency budget.
	batchFlushPeriod = 1 * time.Millisecond
)

// rxItem carries one received packet from udpReader → reorder →
// writer. payload is owned by the item (a caller-allocated copy);
// recvmmsg buffers are reused so we can't keep references to them.
//
// payload layout: [10 zero virtio_net_hdr bytes][inner ip packet],
// pre-formatted so the writer can tun.Write(payload) directly.
type rxItem struct {
	seq     uint32
	payload []byte
}

// batch groups up to batchSize rxItems for one channel send.
type batch struct {
	items []rxItem
}

func newReorderPipe(window int, timeout time.Duration, tuns []tunWriter, logger *slog.Logger) *reorderPipe {
	p := &reorderPipe{
		in:      make(chan *batch, 64),
		outs:    make([]chan []rxItem, len(tuns)),
		stop:    make(chan struct{}),
		window:  window,
		timeout: timeout,
		logger:  logger,
	}

	for i := range p.outs {
		p.outs[i] = make(chan []rxItem, 8)
	}

	go p.reorderLoop()

	for i, t := range tuns {
		go p.writerLoop(p.outs[i], t)
	}

	return p
}

// reorderLoop collects incoming batches by reference, then on
// either window (batches) or timeout walks the queue once,
// distributes items into M sub-slices keyed by seq % M, ships
// each to the matching writer. Writers sort their own bucket.
//
// Window is counted in BATCHES, not items — simpler accounting
// and matches the natural granularity of pipe.in. The hot path
// (per-batch receive) is just one append; the flatten-and-bucket
// pass happens only at flush.
func (p *reorderPipe) reorderLoop() {
	batches := make([]*batch, 0, p.window)

	timer := time.NewTimer(p.timeout)
	defer timer.Stop()

	m := len(p.outs)

	flush := func() {
		if len(batches) == 0 {
			return
		}

		// Estimate items so each bucket allocates close to its
		// final size — saves the grow-and-copy churn on the
		// last bucket-append step.
		total := 0

		for _, b := range batches {
			total += len(b.items)
		}

		buckets := make([][]rxItem, m)

		for i := range buckets {
			buckets[i] = make([]rxItem, 0, total/m+1)
		}

		for _, b := range batches {
			for _, item := range b.items {
				idx := int(item.seq % uint32(m))
				buckets[idx] = append(buckets[idx], item)
			}
		}

		for i, b := range buckets {
			if len(b) > 0 {
				p.outs[i] <- b
			}
		}

		batches = batches[:0]
	}

	resetTimer := func() {
		if !timer.Stop() {
			select {
			case <-timer.C:
			default:
			}
		}

		timer.Reset(p.timeout)
	}

	for {
		select {
		case <-p.stop:
			flush()

			for _, ch := range p.outs {
				close(ch)
			}

			return

		case b, ok := <-p.in:
			if !ok {
				return
			}

			batches = append(batches, b)

			if len(batches) >= p.window {
				flush()
				resetTimer()
			}

		case <-timer.C:
			flush()
			timer.Reset(p.timeout)
		}
	}
}

// writerLoop receives one sub-slice per dispatch, sorts it by seq
// (O(n log n) on n ≈ window/M, trivial), then tun.Write's each item
// in order. Each writer owns one TUN queue; in-queue order is
// preserved by kernel softirq.
func (p *reorderPipe) writerLoop(in <-chan []rxItem, tun tunWriter) {
	for sub := range in {
		sort.Slice(sub, func(i, j int) bool {
			return sub[i].seq < sub[j].seq
		})

		for _, item := range sub {
			if _, err := tun.Write(item.payload); err != nil {
				p.logger.Warn("reorder writer: tun.Write failed", "err", err)
			}
		}
	}
}

func (p *reorderPipe) close() {
	close(p.stop)
}
