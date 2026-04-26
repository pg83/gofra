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
	configPath := flag.String("config", "/etc/gofra/config.json", "path to JSON config")
	flag.Parse()

	cfg, err := loadConfig(*configPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	logger := newLogger(cfg.LogLevel)
	logger.Info("gofra starting",
		"listen_port", cfg.ListenPort,
		"underlay", cfg.Underlay,
		"vip", cfg.TunVIP,
		"peers", len(cfg.PeerByVIP),
	)

	g, err := newGofra(cfg, logger)
	if err != nil {
		logger.Error("init failed", "err", err)
		os.Exit(1)
	}

	go func() {
		ch := make(chan os.Signal, 1)
		signal.Notify(ch, syscall.SIGINT, syscall.SIGTERM)
		sig := <-ch
		logger.Info("signal received, shutting down", "sig", sig)
		os.Exit(0)
	}()

	if err := g.Run(); err != nil {
		logger.Error("data plane stopped", "err", err)
		os.Exit(1)
	}
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
