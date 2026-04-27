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

Single-file daemon. `main.go` parses flags, loads JSON config, builds a
`Gofra` (TUN + N UDP sockets + peer table), runs the data plane.

```
[kernel TCP/IP]
    │  (kernel routing — overlay IPs go via gofra0)
    ▼
[TUN gofra0] ── tun.Read ──┐
                            ├── stripe.go: tunReader
                            │     dst := dstFromIPv4(buf)
                            │     p   := peers[dst]
                            │     n   := p.rr++
                            │     socks[n%N].WriteToUDPAddrPort(buf, p.dsts[(n/N)%M])
                            │
[UDP src×N sockets] ── udpReader ── tun.Write ──> [kernel]
```

* TX path: 1 goroutine reading TUN, dispatching to one of N sockets.
* RX path: N goroutines (one per socket) writing to the TUN directly.
* Peer dispatch: `map[netip.Addr]*peer` keyed by inner dst IP.
* Stripe: per-peer `atomic.Uint64` counter walks the full (srcs×dsts)
  matrix with `srcIdx = c % N`, `dstIdx = (c / N) % M`.

There's no header on the wire. The UDP payload IS the inner IP
packet; receiver hands it to TUN and the kernel sees a normal packet.

## What lives where

| file | purpose |
|------|---------|
| `main.go` | flags, signal handling, log init |
| `config.go` | JSON schema + parse + validate |
| `tun_linux.go` | `/dev/net/tun` open + IP/MTU/up via netlink |
| `socks_linux.go` | UDP bind + SO_BINDTODEVICE + iface lookup |
| `stripe.go` | peer table, RR dispatch, the two reader loops |

## Running locally

* Linux only (TUN, SO_BINDTODEVICE, netlink).
* Needs root or `CAP_NET_ADMIN + CAP_NET_RAW`.
* Underlay IPs in `me.underlay` must already exist on NICs (gofra
  doesn't add them).
* Switch must forward L2 between the underlay IPs and the peer's
  underlay IPs (single broadcast domain in the lab case).

## Dev workflow

```sh
go build ./...            # one binary, no codegen, no vendor
go vet ./...
go test ./...             # only when there are tests, currently none
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

* `IFF_VNET_HDR` on TUN + `virtio_net_hdr` parsing → kernel TSO/GSO
  pushes 64K segments, gofra splits to MTU-sized UDP frames before
  send. Big single-stream win.
* `recvmmsg` + `sendmmsg` to amortise syscall cost on the data path.
* Optional 4-byte header with seq/path-id for downstream dedup or
  reorder-buffer features. Capability-bit so old peers degrade.
