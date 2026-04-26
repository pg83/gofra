package main

import (
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"
)

func main() {
	if len(os.Args) >= 2 && os.Args[1] == "perf" {
		runPerf(os.Args[2:])

		return
	}

	configPath := flag.String("config", "/etc/gofra/config.json", "path to JSON config")
	flag.Parse()

	Try(func() {
		cfg := loadConfig(*configPath)
		logger := newLogger(cfg.LogLevel)

		logger.Info("gofra starting",
			"listen_port", cfg.ListenPort,
			"underlay", cfg.Underlay,
			"vip", cfg.TunVIP,
			"peers", len(cfg.PeerByVIP),
		)

		g := newGofra(cfg, logger)

		go signalLoop(logger)

		Throw(g.Run())
	}).Catch(func(e *Exception) {
		fmt.Fprintln(os.Stderr, e)
		os.Exit(1)
	})
}

func signalLoop(logger *slog.Logger) {
	ch := make(chan os.Signal, 1)
	signal.Notify(ch, syscall.SIGINT, syscall.SIGTERM)

	sig := <-ch

	logger.Info("signal received, shutting down", "sig", sig)
	os.Exit(0)
}

func newLogger(level string) *slog.Logger {
	var lvl slog.Level

	switch level {
	case "debug":
		lvl = slog.LevelDebug
	case "info", "":
		lvl = slog.LevelInfo
	case "warn":
		lvl = slog.LevelWarn
	case "error":
		lvl = slog.LevelError
	default:
		lvl = slog.LevelInfo
	}

	return slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: lvl}))
}
