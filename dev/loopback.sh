#!/bin/sh
# Smoke test: build gofra2 locally, ship it to lab1 via ssh, run
# one instance on each side, exercise the overlay with iperf3, and
# leave both gofra2 instances running with their logs streaming to
# stdout until Ctrl-C / subreaper-driven teardown.
#
# No netns, no veth — the underlay is just nebula between the dev
# machine and lab1. No manual cleanup either: subreaper kills our
# children on exit, ssh disconnect SIGHUPs the remote gofra2, and
# the TUN device is non-persistent so it disappears with the
# process. Lab1 side files land in $HOME/gofra2-smoke/ (no /tmp on
# stalix).
#
# Layout:
#   local: gofra2 bound to $LOCAL_UNDERLAY:$PORT, vip 192.168.110.1/24
#   lab1:  gofra2 bound to $LAB1_UNDERLAY:$PORT,  vip 192.168.110.2/24
#
# Custom port and TUN name so we don't stomp on the deployed
# cluster gofra2 (port 8050, dev gofra20).
#
# Usage:
#   sudo subreaper sh dev/loopback.sh             # 10s TCP
#   sudo subreaper sh dev/loopback.sh tcp 30
#   sudo subreaper sh dev/loopback.sh udp 10 1G

set -xu

export PATH=/ix/realm/llm/bin:$PATH

GOFRA2=${GOFRA2:-./gofra2}
LAB1_HOST=lab1.nebula
LAB1_UNDERLAY=192.168.100.16
LOCAL_UNDERLAY=192.168.100.64    # this dev machine's nebula IP — edit if it moves
PORT=8060
TUN=gofra2_smoke
LOCAL_VIP=192.168.110.1
LAB1_VIP=192.168.110.2

# Local scratch (real /tmp here). Lab side lands in $HOME (no /tmp
# on stalix); SSH_DIR is referenced as a path RELATIVE to whatever
# pwd ssh lands in.
TMP=/tmp/gofra2-smoke
SSH_DIR=gofra2-smoke

mode=${1:-tcp}
secs=${2:-10}
udp_b=${3:-1G}

[ "$(id -u)" -eq 0 ] || { echo "need root (sudo)" >&2; exit 1; }

mkdir -p "$TMP"

cat > "$TMP/local.ini" <<EOF
[me]
vip     = $LOCAL_VIP/24
tun_dev = $TUN
tun_mtu = 1280

[peers]
$LOCAL_VIP = $LOCAL_UNDERLAY:$PORT
$LAB1_VIP  = $LAB1_UNDERLAY:$PORT

[udp]
recv_buf = 16777216
send_buf = 16777216
EOF

cat > "$TMP/lab1.ini" <<EOF
[me]
vip     = $LAB1_VIP/24
tun_dev = $TUN
tun_mtu = 1280

[peers]
$LOCAL_VIP = $LOCAL_UNDERLAY:$PORT
$LAB1_VIP  = $LAB1_UNDERLAY:$PORT

[udp]
recv_buf = 16777216
send_buf = 16777216
EOF

SSH_OPTS="-o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

echo "=== underlay: local=$LOCAL_UNDERLAY  lab1=$LAB1_UNDERLAY ==="

ssh $SSH_OPTS "root@$LAB1_HOST" "mkdir -p $SSH_DIR"
ssh $SSH_OPTS "root@$LAB1_HOST" "cat > $SSH_DIR/gofra2 && chmod +x $SSH_DIR/gofra2" < "$GOFRA2"
ssh $SSH_OPTS "root@$LAB1_HOST" "cat > $SSH_DIR/lab1.ini" < "$TMP/lab1.ini"

# Lab1 gofra2 over a held-open ssh; channel close → SIGHUP → exit.
# `-T` (no pty) + line-buffered redirect via `stdbuf` keep startup
# log lines from sitting in a pipe block buffer; otherwise the
# initial "gofra2: tun=..." line takes ages to surface and looks
# like a hang.
ssh $SSH_OPTS -T "root@$LAB1_HOST" \
    "exec $SSH_DIR/gofra2 --config $SSH_DIR/lab1.ini 2>&1" \
    | sed -u 's/^/[lab1] /' &

# Local gofra2.
"$GOFRA2" --config "$TMP/local.ini" &

sleep 1

# ssh runs commands with stalix's busybox-only PATH; iproute2's
# full `ip` lives at the realm path (see feedback memory).
LAB1_IP=/ix/realm/ip/bin/ip

echo "=== route + iface state ==="
echo "--- local: route to $LAB1_VIP ---"
ip route get "$LAB1_VIP" || :
echo "--- local: ip rule (policy routing) ---"
ip rule show || :
echo "--- local: rp_filter / accept_local on $TUN ---"
for k in rp_filter accept_local forwarding; do
    f=/proc/sys/net/ipv4/conf/$TUN/$k
    [ -e "$f" ] && printf '  %s = %s\n' "$k" "$(cat "$f")"
done
echo "--- local: nft ruleset ---"
nft list ruleset 2>&1 | head -20 || :
echo "--- local: $TUN ---"
ip -s -s link show "$TUN" || :
echo "--- lab1: route to $LOCAL_VIP ---"
ssh $SSH_OPTS "root@$LAB1_HOST" "$LAB1_IP route get $LOCAL_VIP" || :
echo "--- lab1: $TUN ---"
ssh $SSH_OPTS "root@$LAB1_HOST" "$LAB1_IP -s -s link show $TUN" || :

echo "=== ping smoke ==="
ping -c 5 -W 2 "$LAB1_VIP" || echo "(ping failed)"

echo "=== iface state after ping ==="
echo "--- local: $TUN ---"
ip -s -s link show "$TUN" || :
echo "--- lab1: $TUN ---"
ssh $SSH_OPTS "root@$LAB1_HOST" "$LAB1_IP -s -s link show $TUN" || :

echo "=== iperf3 server on lab1 ==="
ssh $SSH_OPTS -T "root@$LAB1_HOST" \
    "exec iperf3 -s -B $LAB1_VIP -p 5201 2>&1" \
    | sed -u "s/^/[iperf3 lab1] /" &
sleep 0.5

echo "=== iperf3 client (mode=$mode duration=${secs}s) ==="
case "$mode" in
    tcp) iperf3 -c "$LAB1_VIP" -B "$LOCAL_VIP" -p 5201 -t "$secs" || true ;;
    udp) iperf3 -c "$LAB1_VIP" -B "$LOCAL_VIP" -p 5201 -t "$secs" -u -b "$udp_b" || true ;;
    *)   echo "mode must be tcp|udp" >&2; exit 1 ;;
esac

echo
echo "=== gofra2 still running. Ctrl-C to stop. ==="
wait
