package main

import (
	"io"
	"log/slog"
	"math/rand"
	"sync/atomic"
	"testing"
	"time"
)

// fakeTun is a tunWriter that just counts writes — no syscall, no
// actual I/O. Drives reorder benchmarks without touching the
// kernel.
type fakeTun struct {
	count atomic.Uint64
	bytes atomic.Uint64
}

func (f *fakeTun) Write(b []byte) (int, error) {
	f.count.Add(1)
	f.bytes.Add(uint64(len(b)))

	return len(b), nil
}

// silentLogger discards everything — benchmarks shouldn't print.
func silentLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, &slog.HandlerOptions{Level: slog.LevelError + 1}))
}

// makeFakeWriters returns m fakeTuns plus the matching tunWriter
// slice for newReorderPipe.
func makeFakeWriters(m int) ([]*fakeTun, []tunWriter) {
	fakes := make([]*fakeTun, m)
	writers := make([]tunWriter, m)

	for i := range fakes {
		fakes[i] = &fakeTun{}
		writers[i] = fakes[i]
	}

	return fakes, writers
}

func totalCount(fakes []*fakeTun) uint64 {
	var t uint64

	for _, f := range fakes {
		t += f.count.Load()
	}

	return t
}

// shuffledBatch builds a batch of `size` items with sequence numbers
// in [base, base+size) shuffled to look like stripe-output (i.e. the
// reorder pipeline's actual input shape). Each item carries a
// pre-allocated payload of payloadLen bytes (just enough to look
// like a real packet for cache/alloc cost).
func shuffledBatch(rng *rand.Rand, base uint32, size, payloadLen int) *batch {
	items := make([]rxItem, size)
	seqs := make([]uint32, size)

	for i := range seqs {
		seqs[i] = base + uint32(i)
	}

	rng.Shuffle(len(seqs), func(i, j int) { seqs[i], seqs[j] = seqs[j], seqs[i] })

	// Single shared payload — fakeTun just counts, no point copying.
	pay := make([]byte, payloadLen)

	for i := range items {
		items[i] = rxItem{seq: seqs[i], payload: pay}
	}

	return &batch{items: items}
}

// drainTo waits until the fakes' total count reaches `expected` or
// `deadline` passes. Returns true if drained on time.
func drainTo(fakes []*fakeTun, expected uint64, deadline time.Duration) bool {
	end := time.Now().Add(deadline)

	for time.Now().Before(end) {
		if totalCount(fakes) >= expected {
			return true
		}

		time.Sleep(100 * time.Microsecond)
	}

	return false
}

// BenchmarkReorderPipeline measures end-to-end pps from `pipe.in`
// → fakeTun.Write through the reorder + writer fan-out. Each b.N
// iteration ships one batch (batchSize items), so packets/sec =
// batchSize × benchmark rate.
func BenchmarkReorderPipeline(b *testing.B) {
	const payloadLen = virtioNetHdrLen + 1400

	fakes, writers := makeFakeWriters(4)

	pipe := newReorderPipe(1024, 10*time.Millisecond, writers, silentLogger())
	defer pipe.close()

	rng := rand.New(rand.NewSource(1))

	// Pre-build all batches up front so the timer measures pipe
	// throughput, not allocation.
	batches := make([]*batch, b.N)

	for i := range batches {
		batches[i] = shuffledBatch(rng, uint32(i*batchSize), batchSize, payloadLen)
	}

	expected := uint64(b.N) * uint64(batchSize)

	b.ResetTimer()

	for i := 0; i < b.N; i++ {
		pipe.in <- batches[i]
	}

	if !drainTo(fakes, expected, 30*time.Second) {
		b.Fatalf("drain timed out: got %d / %d", totalCount(fakes), expected)
	}

	b.StopTimer()

	pps := float64(expected) / b.Elapsed().Seconds()
	bps := pps * float64(payloadLen) * 8

	b.ReportMetric(pps/1e6, "Mpps")
	b.ReportMetric(bps/1e9, "Gbps")
}

// BenchmarkReorderPipelineSorted feeds packets already in seq
// order — measures the steady-state path with no actual reorder
// work. Lower bound on pipeline overhead.
func BenchmarkReorderPipelineSorted(b *testing.B) {
	const payloadLen = virtioNetHdrLen + 1400

	fakes, writers := makeFakeWriters(4)

	pipe := newReorderPipe(1024, 10*time.Millisecond, writers, silentLogger())
	defer pipe.close()

	pay := make([]byte, payloadLen)
	batches := make([]*batch, b.N)

	for i := range batches {
		items := make([]rxItem, batchSize)
		base := uint32(i * batchSize)

		for j := range items {
			items[j] = rxItem{seq: base + uint32(j), payload: pay}
		}

		batches[i] = &batch{items: items}
	}

	expected := uint64(b.N) * uint64(batchSize)

	b.ResetTimer()

	for i := 0; i < b.N; i++ {
		pipe.in <- batches[i]
	}

	if !drainTo(fakes, expected, 30*time.Second) {
		b.Fatalf("drain timed out: got %d / %d", totalCount(fakes), expected)
	}

	b.StopTimer()

	pps := float64(expected) / b.Elapsed().Seconds()
	bps := pps * float64(payloadLen) * 8

	b.ReportMetric(pps/1e6, "Mpps")
	b.ReportMetric(bps/1e9, "Gbps")
}

// TestReorderPipelineDelivers sanity-checks that packets fed into
// the pipeline reach the fake writers (combined count matches what
// went in). Doesn't verify ordering — the writer-side sort happens
// per-bucket and we don't observe per-writer order at the bench
// layer.
func TestReorderPipelineDelivers(t *testing.T) {
	const payloadLen = virtioNetHdrLen + 1400

	fakes, writers := makeFakeWriters(4)

	pipe := newReorderPipe(256, 5*time.Millisecond, writers, silentLogger())
	defer pipe.close()

	rng := rand.New(rand.NewSource(42))

	const totalItems = 4096
	const sentBatches = totalItems / batchSize

	for i := 0; i < sentBatches; i++ {
		pipe.in <- shuffledBatch(rng, uint32(i*batchSize), batchSize, payloadLen)
	}

	if !drainTo(fakes, totalItems, 2*time.Second) {
		t.Fatalf("drain timed out: got %d / %d", totalCount(fakes), totalItems)
	}

	got := totalCount(fakes)

	if got != totalItems {
		t.Fatalf("count mismatch: got %d, want %d", got, totalItems)
	}
}
