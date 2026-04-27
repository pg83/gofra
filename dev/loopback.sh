#!/bin/sh
# Local two-instance gofra2 smoke test using network namespaces.
#
# Spawns two netns ("a" and "b") connected by a veth pair (carries
# the underlay traffic) and runs one gofra2 inside each. Each netns
# has its own TUN with the overlay vip, so the gofra2 instances can
# stripe packets through the veth without colliding on a shared
# routing table.
#
# After setup, runs `iperf3 -s` in netns A and `iperf3 -c` in netns
# B targeting A's overlay IP. Logs go to /tmp/gofra2-{a,b}.log.
#
# Requires root (mkns + tun + ip link).
#
# Usage:
#   sudo sh dev/loopback.sh             # 10s iperf3 default
#   sudo sh dev/loopback.sh tcp 30      # 30s TCP
#   sudo sh dev/loopback.sh udp 10 3G   # UDP -b 3G for 10s
#   sudo sh dev/loopback.sh stop        # tear everything down
#
# Layout:
#   netns A:  tun_a 10.20.0.1/24    veth-a 192.168.99.1/24
#   netns B:  tun_b 10.20.0.2/24    veth-b 192.168.99.2/24
#   peer A: 10.20.0.2 -> 192.168.99.2:8050
#   peer B: 10.20.0.1 -> 192.168.99.1:8050

set -eu

GOFRA2=${GOFRA2:-./gofra2}
NS_A=gofra2_a
NS_B=gofra2_b
TMP=/tmp/gofra2-loop
LOG_A=/tmp/gofra2-a.log
LOG_B=/tmp/gofra2-b.log

[ "$(id -u)" -eq 0 ] || { echo "need root (sudo)"; exit 1; }

teardown() {
    pkill -f "$GOFRA2.*$TMP/a.ini" 2>/dev/null || :
    pkill -f "$GOFRA2.*$TMP/b.ini" 2>/dev/null || :
    pkill -f "ip netns exec $NS_A iperf3" 2>/dev/null || :
    pkill -f "ip netns exec $NS_B iperf3" 2>/dev/null || :
    sleep 0.3

    ip netns del "$NS_A" 2>/dev/null || :
    ip netns del "$NS_B" 2>/dev/null || :
    rm -rf "$TMP"
}

if [ "${1:-}" = "stop" ]; then
    teardown
    echo "torn down"
    exit 0
fi

mode=${1:-tcp}
secs=${2:-10}
udp_b=${3:-1G}

trap 'teardown' EXIT INT

# Clean previous run.
teardown
mkdir -p "$TMP"

ip netns add "$NS_A"
ip netns add "$NS_B"

# Underlay link (veth).
ip link add veth-a type veth peer name veth-b
ip link set veth-a netns "$NS_A"
ip link set veth-b netns "$NS_B"

ip netns exec "$NS_A" ip link set lo up
ip netns exec "$NS_A" ip addr add 192.168.99.1/24 dev veth-a
ip netns exec "$NS_A" ip link set veth-a up

ip netns exec "$NS_B" ip link set lo up
ip netns exec "$NS_B" ip addr add 192.168.99.2/24 dev veth-b
ip netns exec "$NS_B" ip link set veth-b up

# INI configs.
cat > "$TMP/a.ini" <<EOF
listen_port = 8050

[me]
underlay = 192.168.99.1
tun_dev  = tun_a
tun_mtu  = 1400
tun_vip  = 10.20.0.1/24

[peer]
10.20.0.2 = 192.168.99.2

[udp]
recv_buf = 16777216
send_buf = 16777216
EOF

cat > "$TMP/b.ini" <<EOF
listen_port = 8050

[me]
underlay = 192.168.99.2
tun_dev  = tun_b
tun_mtu  = 1400
tun_vip  = 10.20.0.2/24

[peer]
10.20.0.1 = 192.168.99.1

[udp]
recv_buf = 16777216
send_buf = 16777216
EOF

ip netns exec "$NS_A" "$GOFRA2" --config "$TMP/a.ini" >"$LOG_A" 2>&1 &
ip netns exec "$NS_B" "$GOFRA2" --config "$TMP/b.ini" >"$LOG_B" 2>&1 &

# Give them a moment to open the TUN devices and reactor.
sleep 0.5

# Quick sanity: ping across the overlay.
echo "=== ping smoke ==="
ip netns exec "$NS_B" ping -c 2 -W 2 10.20.0.1 || echo "(ping failed)"

# iperf3 across the overlay.
echo "=== iperf3 ==="
ip netns exec "$NS_A" iperf3 -s -1 -p 5201 >/dev/null 2>&1 &
sleep 0.3

case "$mode" in
    tcp) ip netns exec "$NS_B" iperf3 -c 10.20.0.1 -p 5201 -t "$secs" ;;
    udp) ip netns exec "$NS_B" iperf3 -c 10.20.0.1 -p 5201 -t "$secs" -u -b "$udp_b" ;;
    *) echo "mode must be tcp|udp"; exit 1 ;;
esac

echo
echo "=== gofra2 logs ==="
echo "--- A ($LOG_A) ---"
cat "$LOG_A"
echo "--- B ($LOG_B) ---"
cat "$LOG_B"
