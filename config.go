package main

import (
	"encoding/json"
	"fmt"
	"net/netip"
	"os"
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

type Config struct {
	LogLevel   string              `json:"log_level"`
	ListenPort int                 `json:"listen_port"`
	Me         meConfig            `json:"me"`
	Peers      map[string][]string `json:"peers"`
}

type parsedConfig struct {
	LogLevel    string
	ListenPort  uint16
	Underlay    []netip.Addr
	TunDev      string
	TunMTU      int
	TunVIP      netip.Prefix
	PeerByVIP   map[netip.Addr][]netip.Addr
}

func loadConfig(path string) (*parsedConfig, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read config %s: %w", path, err)
	}
	var raw Config
	if err := json.Unmarshal(data, &raw); err != nil {
		return nil, fmt.Errorf("parse config %s: %w", path, err)
	}
	if raw.ListenPort <= 0 || raw.ListenPort > 65535 {
		return nil, fmt.Errorf("listen_port out of range: %d", raw.ListenPort)
	}
	if len(raw.Me.Underlay) == 0 {
		return nil, fmt.Errorf("me.underlay is empty")
	}
	if raw.Me.Tun.Dev == "" {
		return nil, fmt.Errorf("me.tun.dev is empty")
	}
	if raw.Me.Tun.MTU < 576 || raw.Me.Tun.MTU > 65535 {
		return nil, fmt.Errorf("me.tun.mtu out of range: %d", raw.Me.Tun.MTU)
	}
	if raw.Me.Tun.VIP == "" {
		return nil, fmt.Errorf("me.tun.vip is empty")
	}
	vip, err := netip.ParsePrefix(raw.Me.Tun.VIP)
	if err != nil {
		return nil, fmt.Errorf("parse me.tun.vip %q: %w", raw.Me.Tun.VIP, err)
	}

	underlay := make([]netip.Addr, len(raw.Me.Underlay))
	for i, s := range raw.Me.Underlay {
		ip, err := netip.ParseAddr(s)
		if err != nil {
			return nil, fmt.Errorf("parse me.underlay[%d] %q: %w", i, s, err)
		}
		underlay[i] = ip.Unmap()
	}

	peers := make(map[netip.Addr][]netip.Addr, len(raw.Peers))
	for vipStr, ips := range raw.Peers {
		pvip, err := netip.ParseAddr(vipStr)
		if err != nil {
			return nil, fmt.Errorf("parse peer key %q: %w", vipStr, err)
		}
		if pvip == vip.Addr() {
			return nil, fmt.Errorf("peer key %q matches self vip", vipStr)
		}
		if len(ips) == 0 {
			return nil, fmt.Errorf("peer %q has no underlay endpoints", vipStr)
		}
		parsed := make([]netip.Addr, len(ips))
		for i, s := range ips {
			ip, err := netip.ParseAddr(s)
			if err != nil {
				return nil, fmt.Errorf("parse peer %q underlay[%d] %q: %w", vipStr, i, s, err)
			}
			parsed[i] = ip.Unmap()
		}
		peers[pvip.Unmap()] = parsed
	}
	if len(peers) == 0 {
		return nil, fmt.Errorf("peers is empty")
	}

	logLevel := raw.LogLevel
	if logLevel == "" {
		logLevel = "info"
	}

	return &parsedConfig{
		LogLevel:   logLevel,
		ListenPort: uint16(raw.ListenPort),
		Underlay:   underlay,
		TunDev:     raw.Me.Tun.Dev,
		TunMTU:     raw.Me.Tun.MTU,
		TunVIP:     vip,
		PeerByVIP:  peers,
	}, nil
}
