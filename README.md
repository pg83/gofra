# gofra

Multipath UDP encapsulator. No crypto, no peer discovery, no firewall —
just a TUN device + N underlay sockets that stripe inner packets across
every (src, dst) pair of every peer.

Targeted at trusted LANs where each host has 4 NICs on the same subnet
and you want all four to carry traffic for one logical link without
involving the kernel routing table or LACP.

## What it does

* opens a TUN device, assigns a VIP, brings it up
* binds N UDP sockets, one per local underlay IP, each pinned to its NIC
  via `SO_BINDTODEVICE`
* dispatches inner packets by destination IP: `peers[dst_vip]` →
  round-robin over (srcs × dsts), one UDP `sendto` per packet
* receives on all N sockets in parallel, dumps the payload straight to
  the TUN

That's the whole feature set. No replay window, no MAC, no
authentication, no roaming. If you want any of that, run nebula.

## Config

```json
{
  "log_level": "info",
  "listen_port": 8048,
  "me": {
    "underlay": ["10.0.0.64", "10.0.0.65", "10.0.0.66", "10.0.0.67"],
    "tun": {
      "dev": "gofra0",
      "mtu": 1400,
      "vip": "192.168.102.16/24"
    }
  },
  "peers": {
    "192.168.102.17": ["10.0.0.68", "10.0.0.69", "10.0.0.70", "10.0.0.71"],
    "192.168.102.18": ["10.0.0.72", "10.0.0.73", "10.0.0.74", "10.0.0.75"]
  }
}
```

* `me.underlay` — local IPs to bind on. Each must already be
  configured on a NIC.
* `me.tun.vip` — the overlay address (with prefix); the daemon
  assigns it to the TUN and brings the link up.
* `peers.<vip>` — list of underlay endpoints that reach this peer.
  `listen_port` applies to every entry.

## Run

```sh
gofra --config /etc/gofra/config.json
```

Needs `CAP_NET_ADMIN` (TUN + netlink) and `CAP_NET_RAW`
(`SO_BINDTODEVICE`). In practice: run as root.

## Status

Day 1: raw stripe, no GSO, no batching. Expected throughput on a
4×1Gbps lab: ~1 Gbps single-stream, ~3-4 Gbps with `iperf -P 8`.
Day 2: GSO on TUN, sendmmsg/recvmmsg.
