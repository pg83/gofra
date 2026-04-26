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
	TimeoutMs int `json:"timeout_ms"`
}

type Config struct {
	LogLevel   string              `json:"log_level"`
	ListenPort int                 `json:"listen_port"`
	Me         meConfig            `json:"me"`
	Peers      map[string][]string `json:"peers"`
	Udp        udpConfig           `json:"udp"`
	Reorder    reorderConfig       `json:"reorder"`
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

	// Reorder.Window is in BATCHES (not packets) — simpler
	// accounting in the reorder goroutine and matches the
	// natural granularity of pipe.in. 1024 batches × batchSize=64
	// = up to ~64k packets of headroom; in practice the timeout
	// fires first long before that fills, the window is just a
	// safety bound for traffic spikes.
	reorderWindow := raw.Reorder.Window
	if reorderWindow <= 0 {
		reorderWindow = 1024
	}

	reorderTimeoutMs := raw.Reorder.TimeoutMs
	if reorderTimeoutMs <= 0 {
		reorderTimeoutMs = 10
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
		ReorderTimeout: time.Duration(reorderTimeoutMs) * time.Millisecond,
	}
}
