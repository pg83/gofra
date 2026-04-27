#!/bin/sh
# Strace-diff: run gofra1 (Go) and gofra2 (C++) with the same
# minimal config and capture syscall traces. Each runs under
# `timeout 2`, so the trace covers init only — TUN open, ioctl
# setup, AF_INET ctl-socket dance, UDP socket bind, and the
# first few reads/writes after the data plane goroutines spawn.
#
# Diff the two traces to spot what gofra1 does and gofra2
# doesn't (or vice versa) — typically the differences are in
# the address-setup path (netlink in Go vs ioctl in C++).
#
# Output: /tmp/gofra-strace/{g1,g2}.strace
#
# Usage: sudo subreaper sh dev/strace-diff.sh

set -xeu

TMP=/tmp/gofra-strace
[ "$(id -u)" -eq 0 ] || { echo "need root (sudo)" >&2; exit 1; }

# Both binaries are expected in PWD (caller pre-builds: `go build`
# for gofra1, `make` for gofra2). We don't rebuild here.
[ -x ./gofra  ] || { echo "./gofra not found — run 'go build' first" >&2; exit 1; }
[ -x ./gofra2 ] || { echo "./gofra2 not found — run 'make' first"   >&2; exit 1; }

mkdir -p "$TMP"

cat > "$TMP/g1.json" <<'EOF'
{
  "log_level": "info",
  "listen_port": 9060,
  "me": {
    "underlay": ["127.0.0.1"],
    "tun": {"dev": "g_diff", "mtu": 1400, "vip": "10.20.0.1/24"}
  },
  "peers": {"10.20.0.2": ["127.0.0.2"]}
}
EOF

cat > "$TMP/g2.ini" <<'EOF'
listen_port = 9060

[me]
underlay = 127.0.0.1
tun_dev  = g_diff
tun_mtu  = 1400
tun_vip  = 10.20.0.1/24

[peer]
10.20.0.2 = 127.0.0.2
EOF

# Same syscall filter for both — focus on what touches the kernel
# TUN device, the AF_INET ctl-socket setup, and UDP I/O. Verbose
# but bounded.
FILTER=network,ioctl,openat,close,read,write,sendto,recvfrom,sendmsg,recvmsg

echo "=== running gofra1 (Go) ==="
strace -f -o "$TMP/g1.strace" -e trace=$FILTER \
    timeout 2 ./gofra --config "$TMP/g1.json" 2>&1 | sed 's/^/[g1] /' || :

echo "=== running gofra2 (C++) ==="
strace -f -o "$TMP/g2.strace" -e trace=$FILTER \
    timeout 2 ./gofra2 --config "$TMP/g2.ini" 2>&1 | sed 's/^/[g2] /' || :

echo
echo "=== TUN-related syscalls — gofra1 ==="
grep -E '/dev/net/tun|TUN|SIOCS|SIOCG|sendto|recvfrom' "$TMP/g1.strace" | head -50

echo
echo "=== TUN-related syscalls — gofra2 ==="
grep -E '/dev/net/tun|TUN|SIOCS|SIOCG|sendto|recvfrom' "$TMP/g2.strace" | head -50

echo
echo "=== Full traces saved at $TMP/{g1,g2}.strace ==="
echo "=== diff $TMP/g1.strace $TMP/g2.strace | less ==="
