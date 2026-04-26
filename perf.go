package main

import (
	"flag"
	"fmt"
	"io"
	"log/slog"
	"math/rand"
	"os"
	"os/signal"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

// fakeTun is a tunWriter that just counts writes — no syscall, no
// actual I/O. Lets us measure reorderPipe throughput in isolation.
type fakeTun struct {
	count atomic.Uint64
	bytes atomic.Uint64
}

func (f *fakeTun) Write(b []byte) (int, error) {
	f.count.Add(1)
	f.bytes.Add(uint64(len(b)))

	return len(b), nil
}

func makeFakeWriters(m int) ([]*fakeTun, []tunWriter) {
	fakes := make([]*fakeTun, m)
	writers := make([]tunWriter, m)

	for i := range fakes {
		fakes[i] = &fakeTun{}
		writers[i] = fakes[i]
	}

	return fakes, writers
}

func sumCount(fakes []*fakeTun) (uint64, uint64) {
	var c, b uint64

	for _, f := range fakes {
		c += f.count.Load()
		b += f.bytes.Load()
	}

	return c, b
}

func silentLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, &slog.HandlerOptions{Level: slog.LevelError + 1}))
}

// runPerf is `gofra perf` — a self-contained throughput benchmark
// for the reorder pipeline. Spawns the pipe against fake writers,
// floods it with shuffled-seq batches as fast as it'll accept, and
// prints a one-line summary every second.
//
// Useful for sanity-checking the upper bound of in-process
// reorder/sort throughput on a host before wondering whether real
// network performance is limited by us or by tun.Write/kernel TCP
// downstream.
func runPerf(args []string) {
	fs := flag.NewFlagSet("perf", flag.ExitOnError)
	writers := fs.Int("writers", 4, "number of TUN-queue writer goroutines")
	window := fs.Int("window", 16, "reorder window (batches)")
	timeoutUs := fs.Int("timeout-us", 1000, "reorder timeout in microseconds")
	wBucket := fs.Int("writer-bucket", 16, "writer bucket size (sub-slices)")
	wTimeoutUs := fs.Int("writer-timeout-us", 1000, "writer timeout in microseconds")
	payload := fs.Int("payload", 1400, "inner payload bytes per packet")
	duration := fs.Duration("duration", 30*time.Second, "auto-stop after this; 0 = run until SIGINT")
	_ = fs.Parse(args)

	fakes, ws := makeFakeWriters(*writers)

	pipe := newReorderPipe(
		*window,
		time.Duration(*timeoutUs)*time.Microsecond,
		*wBucket,
		time.Duration(*wTimeoutUs)*time.Microsecond,
		ws,
		silentLogger(),
	)
	defer pipe.close()

	stop := make(chan struct{})

	var stopOnce sync.Once
	closeStop := func() { stopOnce.Do(func() { close(stop) }) }

	go func() {
		ch := make(chan os.Signal, 1)
		signal.Notify(ch, syscall.SIGINT, syscall.SIGTERM)
		<-ch

		closeStop()
	}()

	if *duration > 0 {
		go func() {
			time.Sleep(*duration)

			closeStop()
		}()
	}

	go feeder(pipe, *payload, stop)

	reportLoop(pipe, fakes, *payload, stop)
}

// feeder pumps shuffled-seq batches into pipe.in as fast as the
// channel will accept them. Pre-allocates a single shared payload
// buffer (fakeTun doesn't read it; in real life every item gets a
// fresh allocation because recvmmsg buffers are reused).
func feeder(pipe *reorderPipe, payloadLen int, stop <-chan struct{}) {
	rng := rand.New(rand.NewSource(time.Now().UnixNano()))
	pay := make([]byte, virtioNetHdrLen+payloadLen)

	var seq uint32

	for {
		select {
		case <-stop:
			return

		default:
		}

		items := make([]rxItem, batchSize)

		for j := range items {
			items[j] = rxItem{seq: seq, payload: pay}
			seq++
		}

		rng.Shuffle(len(items), func(i, j int) {
			items[i], items[j] = items[j], items[i]
		})

		select {
		case <-stop:
			return

		case pipe.in <- &batch{items: items}:
		}
	}
}

// reportLoop prints one stats line per second until stop fires.
func reportLoop(pipe *reorderPipe, fakes []*fakeTun, payloadLen int, stop <-chan struct{}) {
	t := time.NewTicker(time.Second)
	defer t.Stop()

	start := time.Now()

	var (
		prevCount uint64
		prevBytes uint64
	)

	fmt.Fprintln(os.Stderr, "  t      pkts/s         Mpps     Gbps    in-q")

	for {
		select {
		case <-stop:
			cnt, byt := sumCount(fakes)
			elapsed := time.Since(start).Seconds()

			fmt.Fprintf(os.Stderr, "\nTOTAL  %ds  pkts=%d  bytes=%d  avg %.2f Mpps  %.2f Gbps\n",
				int(elapsed), cnt, byt,
				float64(cnt)/elapsed/1e6,
				float64(byt*8)/elapsed/1e9,
			)

			return

		case now := <-t.C:
			cnt, byt := sumCount(fakes)

			dCnt := cnt - prevCount
			dByt := byt - prevBytes

			prevCount = cnt
			prevBytes = byt

			elapsed := now.Sub(start).Seconds()

			fmt.Fprintf(os.Stderr, "%5.0fs  %11d   %6.2f  %7.2f   %4d\n",
				elapsed, dCnt,
				float64(dCnt)/1e6,
				float64(dByt*8)/1e9,
				len(pipe.in),
			)
		}
	}
}
