package main

import (
	"encoding/json"
	"net/netip"
	"os"
	"time"
)

type tunConfig struct {
	Dev string `json:"dev"`
	MTU int    `json:"mtu"`
	VIP string `json:"vip"`
}

type meConfig struct {
	Underlay []string  `json:"underlay"`
	Tun      tunConfig `json:"tun"`
}

type udpConfig struct {
	RecvBatch int `json:"recv_batch"`
	RecvBuf   int `json:"recv_buf"`
	SendBuf   int `json:"send_buf"`
}

type reorderConfig struct {
	Window    int `json:"window"`
	TimeoutUs int `json:"timeout_us"`
}

type writerConfig struct {
	Bucket    int `json:"bucket"`
	TimeoutUs int `json:"timeout_us"`
}

type Config struct {
	LogLevel   string              `json:"log_level"`
	ListenPort int                 `json:"listen_port"`
	Me         meConfig            `json:"me"`
	Peers      map[string][]string `json:"peers"`
	Udp        udpConfig           `json:"udp"`
	Reorder    reorderConfig       `json:"reorder"`
	Writer     writerConfig        `json:"writer"`
}

type parsedConfig struct {
	LogLevel       string
	ListenPort     uint16
	Underlay       []netip.Addr
	TunDev         string
	TunMTU         int
	TunVIP         netip.Prefix
	PeerByVIP      map[netip.Addr][]netip.Addr
	UdpRecvBatch   int
	UdpRecvBuf     int
	UdpSendBuf     int
	ReorderWindow  int
	ReorderTimeout time.Duration
	WriterBucket   int
	WriterTimeout  time.Duration
}

func loadConfig(path string) *parsedConfig {
	data := Throw2(os.ReadFile(path))

	var raw Config
	Throw(json.Unmarshal(data, &raw))

	if raw.ListenPort <= 0 || raw.ListenPort > 65535 {
		ThrowFmt("listen_port out of range: %d", raw.ListenPort)
	}

	if len(raw.Me.Underlay) == 0 {
		ThrowFmt("me.underlay is empty")
	}

	if raw.Me.Tun.Dev == "" {
		ThrowFmt("me.tun.dev is empty")
	}

	if raw.Me.Tun.MTU < 576 || raw.Me.Tun.MTU > 65535 {
		ThrowFmt("me.tun.mtu out of range: %d", raw.Me.Tun.MTU)
	}

	if raw.Me.Tun.VIP == "" {
		ThrowFmt("me.tun.vip is empty")
	}

	vip := Throw2(netip.ParsePrefix(raw.Me.Tun.VIP))

	underlay := make([]netip.Addr, len(raw.Me.Underlay))

	for i, s := range raw.Me.Underlay {
		underlay[i] = Throw2(netip.ParseAddr(s)).Unmap()
	}

	peers := make(map[netip.Addr][]netip.Addr, len(raw.Peers))

	for vipStr, ips := range raw.Peers {
		pvip := Throw2(netip.ParseAddr(vipStr)).Unmap()

		if pvip == vip.Addr() {
			ThrowFmt("peer key %q matches self vip", vipStr)
		}

		if len(ips) == 0 {
			ThrowFmt("peer %q has no underlay endpoints", vipStr)
		}

		parsed := make([]netip.Addr, len(ips))

		for i, s := range ips {
			parsed[i] = Throw2(netip.ParseAddr(s)).Unmap()
		}

		peers[pvip] = parsed
	}

	if len(peers) == 0 {
		ThrowFmt("peers is empty")
	}

	logLevel := raw.LogLevel
	if logLevel == "" {
		logLevel = "info"
	}

	udpRecvBatch := raw.Udp.RecvBatch
	if udpRecvBatch <= 0 {
		udpRecvBatch = 64
	}

	udpRecvBuf := raw.Udp.RecvBuf
	if udpRecvBuf <= 0 {
		udpRecvBuf = 16 << 20
	}

	udpSendBuf := raw.Udp.SendBuf
	if udpSendBuf <= 0 {
		udpSendBuf = 16 << 20
	}

	// Reorder.Window is in BATCHES (the reorder goroutine
	// counts incoming pipe.in receives, not items). Real LAN
	// reorder distance is microseconds, so the timeout below
	// usually fires first; window is the safety bound on
	// burst-driven accumulator size.
	reorderWindow := raw.Reorder.Window
	if reorderWindow <= 0 {
		reorderWindow = 16
	}

	// timeout in microseconds for finer tuning. Default 1 ms.
	reorderTimeoutUs := raw.Reorder.TimeoutUs
	if reorderTimeoutUs <= 0 {
		reorderTimeoutUs = 1000
	}

	// Writer.Bucket counts how many sub-slices the writer
	// accumulates from the reorder goroutine before sorting +
	// flushing to TUN. Bigger bucket = bigger sort batch
	// (longer monotonic run on TUN), smaller bucket = lower
	// per-packet latency.
	writerBucket := raw.Writer.Bucket
	if writerBucket <= 0 {
		writerBucket = 16
	}

	writerTimeoutUs := raw.Writer.TimeoutUs
	if writerTimeoutUs <= 0 {
		writerTimeoutUs = 1000
	}

	return &parsedConfig{
		LogLevel:       logLevel,
		ListenPort:     uint16(raw.ListenPort),
		Underlay:       underlay,
		TunDev:         raw.Me.Tun.Dev,
		TunMTU:         raw.Me.Tun.MTU,
		TunVIP:         vip,
		PeerByVIP:      peers,
		UdpRecvBatch:   udpRecvBatch,
		UdpRecvBuf:     udpRecvBuf,
		UdpSendBuf:     udpSendBuf,
		ReorderWindow:  reorderWindow,
		ReorderTimeout: time.Duration(reorderTimeoutUs) * time.Microsecond,
		WriterBucket:   writerBucket,
		WriterTimeout:  time.Duration(writerTimeoutUs) * time.Microsecond,
	}
}
