#!/bin/sh
# Local two-instance gofra2 smoke test using network namespaces.
#
# Spawns two netns ("a" and "b") connected by a veth pair (carries
# the underlay traffic) and runs one gofra2 inside each. Each netns
# has its own TUN with the overlay vip, so the gofra2 instances can
# stripe packets through the veth without colliding on a shared
# routing table.
#
# We bypass `ip netns add` because the stalix-built iproute2 has a
# read-only state path baked in. Instead: parallel `unshare --net`
# of a sleep-forever holder per netns, then move veths and run gofra2
# inside via `nsenter --target $PID --net`.
#
# After setup, runs `iperf3 -s` in netns A and `iperf3 -c` in netns B
# targeting A's overlay IP. Logs go to /tmp/gofra2-{a,b}.log.
#
# Usage:
#   sudo sh dev/loopback.sh             # 10s TCP iperf3 default
#   sudo sh dev/loopback.sh tcp 30      # 30s TCP
#   sudo sh dev/loopback.sh udp 10 3G   # UDP -b 3G for 10s
#   sudo sh dev/loopback.sh stop        # tear everything down
#
# Layout:
#   netns A:  tun_a 10.20.0.1/24    veth-a 192.168.99.1/24
#   netns B:  tun_b 10.20.0.2/24    veth-b 192.168.99.2/24
#   peer A: 10.20.0.2 -> 192.168.99.2:8050
#   peer B: 10.20.0.1 -> 192.168.99.1:8050

set -xeu

GOFRA2=${GOFRA2:-./gofra2}
TMP=/tmp/gofra2-loop
LOG_A=/tmp/gofra2-a.log
LOG_B=/tmp/gofra2-b.log
PID_A_FILE=$TMP/holder_a.pid
PID_B_FILE=$TMP/holder_b.pid

[ "$(id -u)" -eq 0 ] || { echo "need root (sudo)"; exit 1; }

teardown() {
    [ -f "$PID_A_FILE" ] && kill "$(cat "$PID_A_FILE")" 2>/dev/null || :
    [ -f "$PID_B_FILE" ] && kill "$(cat "$PID_B_FILE")" 2>/dev/null || :

    pkill -f "$GOFRA2.*$TMP/a.ini" 2>/dev/null || :
    pkill -f "$GOFRA2.*$TMP/b.ini" 2>/dev/null || :
    pkill -f "iperf3.*$TMP" 2>/dev/null || :

    sleep 0.3
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

# Spawn a sleep-forever holder inside each new netns. The holder's
# PID is what we use as the netns handle for `ip link set ... netns`
# and `nsenter --target`. Killing the holder destroys the netns.
unshare --net sleep infinity &
PID_A=$!
echo "$PID_A" > "$PID_A_FILE"

unshare --net sleep infinity &
PID_B=$!
echo "$PID_B" > "$PID_B_FILE"

# Wait for the holders to actually be in their new netns (unshare
# is fast but not synchronous — give it a tick).
sleep 0.1

# Underlay veth, created in the main netns then moved.
ip link add veth-a type veth peer name veth-b
ip link set veth-a netns "$PID_A"
ip link set veth-b netns "$PID_B"

nsenter --target "$PID_A" --net ip link set lo up
nsenter --target "$PID_A" --net ip addr add 192.168.99.1/24 dev veth-a
nsenter --target "$PID_A" --net ip link set veth-a up

nsenter --target "$PID_B" --net ip link set lo up
nsenter --target "$PID_B" --net ip addr add 192.168.99.2/24 dev veth-b
nsenter --target "$PID_B" --net ip link set veth-b up

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

nsenter --target "$PID_A" --net "$GOFRA2" --config "$TMP/a.ini" >"$LOG_A" 2>&1 &
nsenter --target "$PID_B" --net "$GOFRA2" --config "$TMP/b.ini" >"$LOG_B" 2>&1 &

# Give them a moment to open the TUN devices and wire up the reactor.
sleep 0.5

echo "=== ping smoke ==="
nsenter --target "$PID_B" --net ping -c 2 -W 2 10.20.0.1 || echo "(ping failed)"

echo "=== iperf3 ==="
nsenter --target "$PID_A" --net iperf3 -s -1 -p 5201 >/dev/null 2>&1 &
sleep 0.3

case "$mode" in
    tcp) nsenter --target "$PID_B" --net iperf3 -c 10.20.0.1 -p 5201 -t "$secs" ;;
    udp) nsenter --target "$PID_B" --net iperf3 -c 10.20.0.1 -p 5201 -t "$secs" -u -b "$udp_b" ;;
    *) echo "mode must be tcp|udp"; exit 1 ;;
esac

echo
echo "=== gofra2 logs ==="
echo "--- A ($LOG_A) ---"
cat "$LOG_A"
echo "--- B ($LOG_B) ---"
cat "$LOG_B"
