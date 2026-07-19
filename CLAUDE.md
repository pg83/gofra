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

## Formatting

### Blank lines around control blocks

Before `if`, `for`, `while`, `switch`, `return` — add a blank line.
Exception: no blank line if the statement is the first or last
inside `{}`.

Same rule applies after a control block: a blank line before the
next statement, unless the block is the last thing in `{}`.

```cpp
size_t n = conns->srcCount();

if (!n) {
    return;                        // first stmt, no blank before
}

Vector<int> tunFds;                // blank line before tunFds…
                                   // (printed by the formatter)
for (size_t i = 0; i < n; ++i) {
    tunFds.pushBack(openTun(...));
}

configureTun(...);                 // blank before next stmt after for
```

### Logical grouping

Consecutive one-liners that form a single logical operation stay
together without blank lines (mem-init lists, paired `setsockopt`
calls, a chain of attribute fills before one `run()`). Between
distinct operations — add a blank line.

```cpp
ifr.ifr_family = AF_UNSPEC;        // one operation: fill ifr fields
ifr.ifr_index  = idx;
ifr.ifr_flags  = 0;

mnl_attr_put_u32(nh, IFLA_MTU, mtu);   // distinct: queue an attr

run(nh, "RTM_NEWLINK MTU");        // distinct: send the request
```

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
                         │     split N segs into 8 parts; for each part:
                         │       slot = conn->next() (atomic)
                         │       sendmsg(slot.srcFd, UDP_SEGMENT, gso_size)
                         │       → kernel/NIC re-segments to MTU
                         │
[N UDP sockets] ── recvmmsg(64) ── plane.cpp: udpReader (per socket)
                                    write [10 zero hdr | payload] → tuns[i]
```

* TX path: N OS threads, one per TUN queue. Each blocks on
  `read(tunFd)`; on GSO_TCPV4 the super-packet is split in user-space
  via `gsoSplit` (vendored from wireguard-go). The N segments are
  partitioned into 8 chunks; each chunk is sent as one
  `sendmsg(UDP_SEGMENT)` cmsg call through its own stripe slot, so
  segments of one super-packet land on multiple (src,dst) pairs and
  the kernel/NIC handles the actual MTU-sized re-segmentation.
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
  rotates dst slow). The 8-way USO split rotates the counter 8 times
  per super-packet, hitting all 4 srcs × 2 dsts.

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
  (gofra doesn't add them).
* Switch must forward L2 between every peer's underlay IPs in a
  single broadcast domain (no routed underlay).

## Dev workflow

```sh
./build gofra          # builds gofra against ../std/std/libstd.a
sudo subreaper sh dev/loopback.sh tcp 30   # 30 s TCP smoke over the local→lab2 stripe
sudo sh dev/perf.sh 30                     # perf record on the deployed cluster gofra
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

* `UDP_GRO` on RX — kernel coalesces incoming UDP packets into a
  super-buffer; we'd then write that as one GSO_TCPV4 frame to TUN,
  cutting receiver-side TCP stack walks per byte. Symmetric to USO
  on TX and the obvious next big lever.
* Optional 4-byte header with seq/path-id for downstream dedup or
  reorder-buffer features. Capability-bit so old peers degrade.
