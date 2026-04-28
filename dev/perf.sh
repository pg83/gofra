#!/bin/sh
# Profile the deployed gofra on lab1 / lab2 with perf record.
#
# 1. Ship the locally-built ./gofra to root@$h:/bin/gofra on each
#    host (chmod +x). This lets perf resolve symbols against the
#    same binary the cluster's runsrv supervisor will pick up after
#    we kill the live process — provided /bin is ahead of /ix/store
#    in the service's PATH. If it isn't, perf still works; symbols
#    just resolve against whichever binary was respawned.
#
# 2. pkill -x gofra — runsrv will bring it back up within a second
#    or two with the same INI from cg.py.
#
# 3. perf record -F 99 -g -p PID -o $HOME/gofra.perf for $DURATION
#    seconds on each host, in parallel.
#
# 4. scp the per-host perf.data back to ./perf-{lab1,lab2}.data.
#
# Lab side files land in $HOME (no /tmp on stalix) and are reaped
# with the host on next boot; we don't bother cleaning up.
#
# Assumes load is constant during the window — caller is expected
# to have iperf3 (or similar) running while this script does its
# thing. Defaults to a 30 s capture; pass an int to override.

set -xu

GOFRA=${GOFRA:-./gofra}
HOSTS="${HOSTS:-lab1 lab2}"
DURATION=${1:-30}

SSH_OPTS="-o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

[ -x "$GOFRA" ] || { echo "no $GOFRA — build it first" >&2; exit 1; }

# 1) push binary, 2) kill old instance.
#
# Can't write directly to /bin/gofra — kernel refuses (ETXTBSY)
# while it's running. Write to a sibling, chmod +x, then mv atomically
# replaces the inode (the old open process keeps its mapping until
# we kill it). Only after mv is done do we kill, so the respawn
# picks up the new binary.
for h in $HOSTS; do
    ssh $SSH_OPTS "root@$h.nebula" \
        "cat > /bin/gofra.new && chmod +x /bin/gofra.new && mv /bin/gofra.new /bin/gofra" \
        < "$GOFRA"
    ssh $SSH_OPTS "root@$h.nebula" "pkill -x gofra || :"
done

# Give runsrv a moment to respawn.
sleep 3

# 3) profile in parallel, 4) pull perf.data back via ssh-cat (no
#    scp — stalix has no sftp-server). `./perf` is expected to live
#    in $HOME on each lab host (where ssh lands by default).
# 5) also pull /proc/kallsyms for each host so kernel-side callgraph
#    resolves (kptr_restrict means perf-on-host already saw real
#    addresses; we just need the same name table on the analysis
#    machine). pass to perf report via --kallsyms=./kallsyms-<host>.
for h in $HOSTS; do
    (
        ssh $SSH_OPTS "root@$h.nebula" "
            pid=\$(pgrep -x gofra | head -1)
            [ -n \"\$pid\" ] || { echo '$h: no gofra pid after respawn' >&2; exit 1; }
            echo '$h: profiling pid '\$pid' for $DURATION s'
            ./perf record -F 999 -g -p \$pid -o ./gofra.perf -- timeout ${DURATION}s tail -f /dev/null
        "
        ssh $SSH_OPTS "root@$h.nebula" "cat ./gofra.perf"   > "./perf-$h.data"
        ssh $SSH_OPTS "root@$h.nebula" "cat /proc/kallsyms" > "./kallsyms-$h"
        echo "$h: ./perf-$h.data + ./kallsyms-$h ready"
    ) &
done

wait

echo
echo "captures:"
ls -lh ./perf-*.data ./kallsyms-*
echo
echo "open with: perf report -i ./perf-<host>.data --kallsyms=./kallsyms-<host>"
