package main

import (
	"sync"
	"time"
)

// reorderRing absorbs out-of-order delivery from the (srcs × dsts)
// stripe so kernel TCP on the receiver never sees it. One ring per
// peer. Each wire packet carries a 4-byte monotonic seq prepended
// by the sender; the ring delivers packets to the TUN strictly in
// seq order, holding gaps for up to `timeout` before skipping
// past them (and letting the inner-protocol retransmit machinery
// — if any — recover the lost data the slow way).
//
// Sliding window: `base` is the next seq we expect. Slot
// (seq - base) % len(slots) holds the corresponding packet. New
// arrivals advance base as far as consecutive valid slots allow;
// per-arrival timeout-check skips lone gaps when their oldest
// pending neighbour has been waiting too long. A periodic ticker
// makes timeout-flushes happen even without new arrivals.
//
// Concurrency: 4 udpReader goroutines call put() from different
// sockets but the same peer may be served by any of them. One
// mutex protects the ring; the critical section is small (a copy
// + a few atomic field updates).
type reorderRing struct {
	mu     sync.Mutex
	base   uint32
	slots  []reorderSlot
	tun    *TUN
	logger interface {
		Debug(string, ...any)
		Warn(string, ...any)
	}

	timeout time.Duration

	// gapStartedAt timestamps the first non-base insertion that
	// extended the in-window range. Cleared when base advances
	// past every pending slot. Used to age out stuck gaps.
	gapStartedAt time.Time

	// stop signals the timeout-flush goroutine to exit.
	stop chan struct{}
}

type reorderSlot struct {
	// payload holds [virtio_net_hdr 10 zero bytes][inner ip pkt],
	// already laid out for tun.Write(payload). Nil/zero-length
	// when the slot is unused.
	payload []byte
	valid   bool
	arrived time.Time
}

func newReorderRing(window int, timeout time.Duration, tun *TUN, logger interface {
	Debug(string, ...any)
	Warn(string, ...any)
}) *reorderRing {
	r := &reorderRing{
		slots:   make([]reorderSlot, window),
		tun:     tun,
		logger:  logger,
		timeout: timeout,
		stop:    make(chan struct{}),
	}

	go r.timeoutLoop()

	return r
}

// put inserts the packet at its seq position and advances the base
// as far as ready / aged-out slots allow.
func (r *reorderRing) put(seq uint32, innerPkt []byte) {
	r.mu.Lock()
	defer r.mu.Unlock()

	now := time.Now()
	winLen := uint32(len(r.slots))

	// First-ever arrival: align base on this seq so we don't wait
	// forever for "missing" packets that never existed.
	if r.base == 0 && !r.anyValidLocked() {
		r.base = seq
	}

	diff := seq - r.base

	if diff >= winLen {
		// Either a stale duplicate from way back (diff is huge
		// because of unsigned wrap) or a jump too far ahead. We
		// distinguish via the high bit: if diff is closer to 2^32
		// than to 0, it's a late packet — drop. Otherwise force
		// the window forward to fit the new seq.
		if diff > (1<<31) {
			return
		}

		// Slide the window so the new seq falls at the very edge.
		skip := diff - winLen + 1

		for i := uint32(0); i < skip; i++ {
			r.advanceLocked()
		}

		diff = seq - r.base
	}

	idx := (r.base + diff) % winLen

	if r.slots[idx].valid {
		// Duplicate (or wrap collision after force-advance). Drop.
		return
	}

	hdrLen := virtioNetHdrLen
	need := hdrLen + len(innerPkt)

	if cap(r.slots[idx].payload) < need {
		r.slots[idx].payload = make([]byte, need)
	} else {
		r.slots[idx].payload = r.slots[idx].payload[:need]

		for i := 0; i < hdrLen; i++ {
			r.slots[idx].payload[i] = 0
		}
	}

	copy(r.slots[idx].payload[hdrLen:], innerPkt)

	r.slots[idx].valid = true
	r.slots[idx].arrived = now

	if diff > 0 && r.gapStartedAt.IsZero() {
		r.gapStartedAt = now
	}

	r.tryAdvanceLocked(now)
}

// tryAdvanceLocked walks the ring forward as long as either:
//   - the slot at base is valid, or
//   - there's a pending packet behind a gap whose age exceeds
//     the configured timeout.
//
// When the second condition fires we skip the gap (advance past
// it) and try again. Caller must hold r.mu.
func (r *reorderRing) tryAdvanceLocked(now time.Time) {
	winLen := uint32(len(r.slots))

	for {
		idx := r.base % winLen

		if r.slots[idx].valid {
			r.flushSlotLocked(idx)
			r.base++

			if !r.anyValidLocked() {
				r.gapStartedAt = time.Time{}
			}

			continue
		}

		if r.gapStartedAt.IsZero() || now.Sub(r.gapStartedAt) < r.timeout {
			return
		}

		// Gap aged out — skip it.
		r.base++

		// New gapStartedAt = oldest still-pending arrival, or
		// zero if nothing pending now.
		r.gapStartedAt = r.oldestPendingLocked()
	}
}

// advanceLocked unconditionally moves base by one, flushing the
// slot at base if it happens to be valid.
func (r *reorderRing) advanceLocked() {
	idx := r.base % uint32(len(r.slots))

	if r.slots[idx].valid {
		r.flushSlotLocked(idx)
	}

	r.base++
}

// flushSlotLocked writes the slot's payload to the TUN and marks
// it free.
func (r *reorderRing) flushSlotLocked(idx uint32) {
	s := &r.slots[idx]

	if _, err := r.tun.Write(s.payload); err != nil {
		r.logger.Warn("reorder: tun.Write failed", "err", err)
	}

	s.valid = false
	s.payload = s.payload[:0]
}

func (r *reorderRing) anyValidLocked() bool {
	for i := range r.slots {
		if r.slots[i].valid {
			return true
		}
	}

	return false
}

func (r *reorderRing) oldestPendingLocked() time.Time {
	var oldest time.Time

	for i := range r.slots {
		if !r.slots[i].valid {
			continue
		}

		if oldest.IsZero() || r.slots[i].arrived.Before(oldest) {
			oldest = r.slots[i].arrived
		}
	}

	return oldest
}

// timeoutLoop force-advances the ring on a tick so timeouts fire
// even when no fresh packets are arriving. Tick at timeout/4 so we
// stay close to the configured upper bound on hold time.
func (r *reorderRing) timeoutLoop() {
	period := r.timeout / 4
	if period <= 0 {
		period = time.Millisecond
	}

	t := time.NewTicker(period)
	defer t.Stop()

	for {
		select {
		case <-r.stop:
			return

		case now := <-t.C:
			r.mu.Lock()
			r.tryAdvanceLocked(now)
			r.mu.Unlock()
		}
	}
}

func (r *reorderRing) close() {
	close(r.stop)
}
