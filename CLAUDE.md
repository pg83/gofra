# Notes for working in this repo

## Coding conventions

- Git author: `claude <claude@users.noreply.github.com>`. Commit messages in English.
- Types: `CamelCase`. Variables/functions: `camelCase`. Private members: trailing `_` (e.g. `len_`, `ptr_`). Macros: `STD_*` prefix.
- CamelCase for test names in `STD_TEST`.
- Namespace `stl` for public API. Free classes and functions in `.cpp` go into anonymous namespace.
- Methods longer than 1 line must be defined out of line and outside any namespace.
- Each `xxx.h` must have a corresponding `xxx.cpp`.
- No single-line braced blocks: `{ ... }` must span multiple lines.
- Avoid includes in headers; prefer forward declarations. Only include in `.cpp` files.
- Type aliases: `i8`/`u8`/`i16`/`u16`/`i32`/`u32`/`i64`/`u64` from `std/sys/types.h`.

## Mental model

C++ daemon on top of `std/`. `main.cpp` parses flags, loads the INI
config, opens N TUN queues + N UDP sockets, spawns 2*N OS threads
via `stl::Thread`, runs the data plane.

```
[kernel TCP/IP]
    │  (kernel routing — overlay IPs go via gofra20)
    ▼
[TUN gofra20] ── read ──┐
                         ├── plane.cpp: tunReader (per queue)
                         │     decode 10-byte virtio_net_hdr
                         │     gsoSplit on GSO_TCPV4 → N MTU segments
                         │     for each: conn = conns->lookup(dst)
                         │              slot = conn->next() (atomic)
                         │              sendto(slot.srcFd, slot.dstAddr)
                         │
[N UDP sockets] ── recvmmsg(64) ── plane.cpp: udpReader (per socket)
                                    write [10 zero hdr | payload] → tuns[i]
```

* TX path: N OS threads, one per TUN queue. Each blocks on
  `read(tunFd)`; on GSO_TCPV4 the super-packet is split in user-space
  via `gsoSplit` (vendored from wireguard-go), each MTU segment
  handed to `Conn::next()` which atomically picks the next slot in
  the per-peer N*M (srcFd × dstAddr) stripe array.
* RX path: N OS threads, one per UDP socket. Each blocks on
  `recvmmsg(MSG_WAITFORONE)` to drain up to 64 packets per syscall,
  then writes each to the paired TUN queue with a zeroed 10-byte
  `virtio_net_hdr` prefix (kernel reads as gso_type=NONE).
* TUN open: `IFF_TUN | IFF_NO_PI | IFF_MULTI_QUEUE | IFF_VNET_HDR`,
  `TUNSETVNETHDRSZ=10`, `TUNSETOFFLOAD(TUN_F_CSUM | TUN_F_TSO4)`.
* iface mtu/addr/up via rtnetlink (`libmnl`). The legacy SIOCSIF*
  ioctl path silently drops egress on a /24 P2P TUN.
* Stripe: per-`Conn` `atomic_uint64` rr counter walks the pre-built
  N*M slot array (laid out so `rr % N` rotates src fast, `rr / N`
  rotates dst slow — matches gofra-go's stripe formula).

There's no header on the wire. The UDP payload IS the inner IP
packet; receiver hands it to TUN with the zero virtio_net_hdr
prefix and the kernel sees a normal packet.

## What lives where

| file | purpose |
|------|---------|
| `main.cpp` | flags, signal handling, thread spawn |
| `config.h/.cpp` | typed `Config` + `loadConfig` over `ini::parseConfig` |
| `ini.h/.cpp` | INI tokenizer → `SymbolMap<Section>` (section name → keys + map) |
| `addr.h/.cpp` | `parseIPv4` / `parseCIDR` / `parseSockAddr` / `forEachItem` |
| `peer.h/.cpp` | `Peer` and `PeerTable` interfaces (cluster vip → endpoints) |
| `conn.h/.cpp` | `Conn` and `ConnTable` interfaces — per-peer N*M stripe slots, owns the N UDP sockets |
| `tun.h/.cpp` | `openTun` + `configureTun` (rtnetlink via libmnl) |
| `socks.h/.cpp` | `openUdpSocket` (SO_BINDTODEVICE, RCVBUFFORCE, bind) |
| `csum.h/.cpp` | 16-bit one's-complement TCP/IP checksum primitives (vendored) |
| `gso.h/.cpp` | `VirtioNetHdr` decode, `gsoSplit` (TCPV4 super-packet → N MTU segments) |
| `plane.h/.cpp` | `tunReader` / `udpReader` thread entry points + scratch buffers |

## Running locally

* Linux only (TUN, SO_BINDTODEVICE, rtnetlink, recvmmsg).
* Needs root or `CAP_NET_ADMIN + CAP_NET_RAW`.
* Underlay IPs in `[peers][my_vip]` must already exist on local NICs
  (gofra2 doesn't add them).
* Switch must forward L2 between every peer's underlay IPs in a
  single broadcast domain (no routed underlay).

## Dev workflow

```sh
make -j               # builds gofra2 against ../std/std/libstd.a
sudo subreaper sh dev/loopback.sh tcp 30   # 30 s TCP smoke over the local→lab2 stripe
sudo sh dev/perf.sh 30                     # perf record on the deployed cluster gofra2
```

## What's deliberately out of scope

* Cryptography (Noise, AES-GCM, anything). Plaintext on the wire.
* Authentication of peers. Anyone on the underlay who can guess
  `listen_port` and a peer's underlay IP can inject inner packets.
* Lighthouse / roaming / NAT punching. Peers are static, on-LAN.
* Firewall, conntrack, replay window.
* Per-path liveness. If a NIC dies, you'll see TCP retransmits.

If any of these matter for your environment, use nebula or wireguard,
not gofra.

## Future hooks (not built)

* `UDP_SEGMENT` (USO) on TX — one `sendmsg` per super-packet with
  cmsg, kernel/NIC segments to MTU. Drops `gsoSplit` from user-space
  and amortizes `udp_sendmsg` / qdisc lock through the wireguard-go
  PR #75 path. Closes the remaining gap to NIC saturation on
  4×1G underlays.
* `UDP_GRO` on RX — kernel coalesces incoming UDP packets into
  super-buffers before handing to user-space; pairs with USO.
* Optional 4-byte header with seq/path-id for downstream dedup or
  reorder-buffer features. Capability-bit so old peers degrade.
