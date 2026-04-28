#!/bin/sh
# Smoke test over Ethernet (no nebula). Local has one NIC at
# 10.0.0.163; lab2 has four NICs at 10.0.0.68..71. peers[lab2.vip]
# lists all four lab2 underlays, so the stripe is 1×4 from local
# (one local src) and 4×1 from lab2 (four local srcs, one remote).
#
# No netns, no veth — direct UDP over Ethernet. ssh stays over
# nebula; only the gofra underlay traffic uses eth. No manual
# cleanup either: subreaper kills our children on exit, ssh
# disconnect SIGHUPs the remote gofra, and the TUN device is
# non-persistent so it disappears with the process. Lab2 side
# files land in $HOME/gofra-smoke/ (no /tmp on stalix).
#
# Layout:
#   local: vip 192.168.110.1/24, underlay 10.0.0.163:$PORT
#   lab2:  vip 192.168.110.2/24, underlays 10.0.0.{68,69,70,71}:$PORT
#
# Custom port and TUN name so we don't stomp on the deployed
# cluster gofra.
#
# Usage:
#   sudo subreaper sh dev/loopback.sh             # 10s TCP
#   sudo subreaper sh dev/loopback.sh tcp 30
#   sudo subreaper sh dev/loopback.sh udp 10 1G

set -xu

export PATH=/ix/realm/llm/bin:$PATH

GOFRA=${GOFRA:-./gofra}
LAB_SSH=lab2.nebula              # ssh over nebula; underlay traffic uses eth
LOCAL_UNDERLAY=10.0.0.163        # this dev machine's eth1 IP
PORT=8060
TUN=gofra_smoke
LOCAL_VIP=192.168.110.1
LAB_VIP=192.168.110.2

# Local scratch (real /tmp here). Lab side lands in $HOME (no /tmp
# on stalix); SSH_DIR is referenced as a path RELATIVE to whatever
# pwd ssh lands in.
TMP=/tmp/gofra-smoke
SSH_DIR=gofra-smoke

mode=${1:-tcp}
secs=${2:-10}
udp_b=${3:-1G}

[ "$(id -u)" -eq 0 ] || { echo "need root (sudo)" >&2; exit 1; }

mkdir -p "$TMP"

# Both sides see the same [peers] table — only [me].vip differs.
PEERS_BLOCK=$(cat <<EOF
[peers]
$LOCAL_VIP = 10.0.0.163:$PORT
$LAB_VIP   = 10.0.0.68:$PORT, 10.0.0.69:$PORT, 10.0.0.70:$PORT, 10.0.0.71:$PORT
EOF
)

cat > "$TMP/local.ini" <<EOF
[me]
vip     = $LOCAL_VIP/24
tun_dev = $TUN
tun_mtu = 1280

$PEERS_BLOCK

[udp]
recv_buf = 16777216
send_buf = 16777216
EOF

cat > "$TMP/lab.ini" <<EOF
[me]
vip     = $LAB_VIP/24
tun_dev = $TUN
tun_mtu = 1280

$PEERS_BLOCK

[udp]
recv_buf = 16777216
send_buf = 16777216
EOF

SSH_OPTS="-o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

echo "=== underlay: local=$LOCAL_UNDERLAY  lab=10.0.0.{68,69,70,71} ==="

ssh $SSH_OPTS "root@$LAB_SSH" "mkdir -p $SSH_DIR"
ssh $SSH_OPTS "root@$LAB_SSH" "cat > $SSH_DIR/gofra && chmod +x $SSH_DIR/gofra" < "$GOFRA"
ssh $SSH_OPTS "root@$LAB_SSH" "cat > $SSH_DIR/lab.ini" < "$TMP/lab.ini"

# Lab gofra over a held-open ssh; channel close → SIGHUP → exit.
# `-T` (no pty) keeps startup log lines from sitting in a pipe
# block buffer; otherwise the initial "gofra: tun=..." line takes
# ages to surface and looks like a hang.
ssh $SSH_OPTS -T "root@$LAB_SSH" \
    "exec $SSH_DIR/gofra --config $SSH_DIR/lab.ini 2>&1" \
    | sed -u 's/^/[lab] /' &

# Local gofra.
"$GOFRA" --config "$TMP/local.ini" &

sleep 1

# ssh runs commands with stalix's busybox-only PATH; iproute2's
# full `ip` lives at the realm path (see feedback memory).
LAB_IP=/ix/realm/ip/bin/ip

echo "=== route + iface state ==="
echo "--- local: route to $LAB_VIP ---"
ip route get "$LAB_VIP" || :
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
echo "--- lab: route to $LOCAL_VIP ---"
ssh $SSH_OPTS "root@$LAB_SSH" "$LAB_IP route get $LOCAL_VIP" || :
echo "--- lab: $TUN ---"
ssh $SSH_OPTS "root@$LAB_SSH" "$LAB_IP -s -s link show $TUN" || :

echo "=== ping smoke ==="
ping -c 5 -W 2 "$LAB_VIP" || echo "(ping failed)"

echo "=== iface state after ping ==="
echo "--- local: $TUN ---"
ip -s -s link show "$TUN" || :
echo "--- lab: $TUN ---"
ssh $SSH_OPTS "root@$LAB_SSH" "$LAB_IP -s -s link show $TUN" || :

echo "=== iperf3 server on lab ==="
ssh $SSH_OPTS -T "root@$LAB_SSH" \
    "exec iperf3 -s -B $LAB_VIP -p 5201 2>&1" \
    | sed -u "s/^/[iperf3 lab] /" &
sleep 0.5

echo "=== iperf3 client (mode=$mode duration=${secs}s) ==="
case "$mode" in
    tcp) iperf3 -c "$LAB_VIP" -B "$LOCAL_VIP" -p 5201 -t "$secs" || true ;;
    udp) iperf3 -c "$LAB_VIP" -B "$LOCAL_VIP" -p 5201 -t "$secs" -u -b "$udp_b" || true ;;
    *)   echo "mode must be tcp|udp" >&2; exit 1 ;;
esac

echo
echo "=== gofra still running. Ctrl-C to stop. ==="
wait
