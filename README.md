# gofra

Multipath UDP encapsulator. No crypto, no peer discovery, no firewall —
just a TUN device + N underlay sockets that stripe inner packets across
every (src, dst) pair of every peer.

Targeted at trusted LANs where each host has 4 NICs on the same subnet
and you want all four to carry traffic for one logical link without
involving the kernel routing table or LACP.

C++ on top of [std/](https://github.com/pg83/std) (ObjPool, threads,
intrusive containers). Multi-queue TUN with `IFF_VNET_HDR` + GSO/TSO,
blocking-thread data path, `recvmmsg(64)` on RX, `sendmsg(UDP_SEGMENT)`
on TX with the super-packet split across 8 stripe slots. Single-stream
TCP hits ~3.5 Gbit/s on a 4×1Gbps lab stripe.

## What it does

* opens N TUN queues with `IFF_MULTI_QUEUE | IFF_VNET_HDR`, assigns
  the overlay VIP, brings the link up via rtnetlink.
* binds N UDP sockets, one per local underlay IP, each pinned to its
  NIC via `SO_BINDTODEVICE`.
* per remote peer, pre-builds an N*M (srcFd × dstAddr) slot array;
  an atomic counter walks the array so traffic fans out across every
  underlay pair.
* TX: read the GSO super-packet from TUN, run wireguard-go's
  `gsoSplit` (vendored), then `sendmsg(UDP_SEGMENT)` over 8 stripe
  slots — kernel/NIC re-segments each part to MTU. RX-parallel by
  construction.
* RX: drain each UDP socket with `recvmmsg(64)` and write each
  datagram (with a zeroed 10-byte virtio_net_hdr prefix) to the
  paired TUN queue.

That's the whole feature set. No replay window, no MAC, no
authentication, no roaming. If you want any of that, run nebula.

## Config

INI. `[me]` identifies us; `[peers]` is the full cluster table with
every member (us included) mapping VIP → comma-separated `ip:port`
list of underlay endpoints. The binary picks our row via
`peers->lookup([me].vip)` and binds N UDP sockets there.

```ini
[me]
vip     = 192.168.110.1/24
tun_dev = gofra20
tun_mtu = 1280

[peers]
192.168.110.1 = 10.0.0.64:8050, 10.0.0.65:8050, 10.0.0.66:8050, 10.0.0.67:8050
192.168.110.2 = 10.0.0.68:8050, 10.0.0.69:8050, 10.0.0.70:8050, 10.0.0.71:8050
192.168.110.3 = 10.0.0.72:8050, 10.0.0.73:8050, 10.0.0.74:8050, 10.0.0.75:8050

[udp]
recv_buf = 16777216
send_buf = 16777216
```

* `[me].vip` — the overlay address (with prefix); the daemon assigns
  it to the TUN and brings the link up.
* `[peers].<vip>` — comma-separated `ip:port` endpoints; the entry
  whose VIP equals `[me].vip` is treated as self.

## Run

```sh
gofra2 --config /etc/gofra2/config.ini
```

Needs `CAP_NET_ADMIN` (TUN + rtnetlink) and `CAP_NET_RAW`
(`SO_BINDTODEVICE`). In practice: run as root.

## Status

~3.5 Gbit/s single-stream TCP over a 4×1Gbps lab stripe. Next lever
is `UDP_GRO` on RX so the kernel hands us a coalesced super-buffer
to write to TUN as one GSO_TCPV4 frame, cutting receiver-side TCP
stack walks per byte.
