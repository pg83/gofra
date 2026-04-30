#!/usr/bin/env python3
import argparse
import os
import sys
import signal
import socket
import subprocess
import threading
import time
from pathlib import Path

LAB_SSH = "lab2.nebula"
PORT = 8060
TUN = "gofra_smoke"
LOCAL_VIP = "192.168.110.1"
LAB_VIP = "192.168.110.2"
LOCAL_UNDERLAY = "10.0.0.163"
LAB_UNDERLAYS = ["10.0.0.68", "10.0.0.69", "10.0.0.70", "10.0.0.71"]
LOCAL_TMP = Path("/tmp/gofra-smoke")
SSH_DIR = "gofra-smoke"
SSH_OPTS = ["-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
            "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR"]
PRINT_LOCK = threading.Lock()

os.environ["PATH"] = "/ix/realm/llm/bin:/ix/realm/ip/bin:" + os.environ.get("PATH", "/usr/bin:/bin")


def ini(vip):
    peers = f"{LOCAL_VIP} = {LOCAL_UNDERLAY}:{PORT}\n"
    peers += f"{LAB_VIP} = " + ", ".join(f"{u}:{PORT}" for u in LAB_UNDERLAYS) + "\n"
    return (f"[me]\nvip = {vip}/24\ntun_dev = {TUN}\ntun_mtu = 1280\nuser = nobody\n\n"
            f"[peers]\n{peers}\n[udp]\nrecv_buf = 16777216\nsend_buf = 16777216\n")


def echo(prefix, text):
    with PRINT_LOCK:
        for line in text.splitlines() if text else [""]:
            sys.stdout.write(f"{prefix} {line}\n")
        sys.stdout.flush()


def stream(proc, prefix):
    for line in iter(proc.stdout.readline, b""):
        echo(prefix, line.decode("utf-8", errors="replace").rstrip("\n"))


def launch(cmd, prefix):
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         start_new_session=True)
    threading.Thread(target=stream, args=(p, prefix), daemon=True).start()
    return p


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def find_ip_bin():
    for p in ("/ix/realm/ip/bin/ip", "/sbin/ip", "/bin/ip", "/usr/bin/ip"):
        if Path(p).exists():
            return p
    return "ip"


def collect_threads(get_pid):
    pid = get_pid()
    if not pid:
        return "(no PID)"
    task_dir = Path(f"/proc/{pid}/task")
    if not task_dir.exists():
        return f"(pid {pid} gone)"
    out = ["TID  CPU  STATE  UTIME  STIME  WCHAN  COMM"]
    for tid_dir in sorted(task_dir.iterdir(), key=lambda p: int(p.name)):
        tid = tid_dir.name
        try:
            stat = (tid_dir / "stat").read_text().split()
            wchan = (tid_dir / "wchan").read_text().strip() or "-"
            comm = (tid_dir / "comm").read_text().strip()
            state = stat[2]
            utime = stat[13]
            stime = stat[14]
            cpu = stat[38] if len(stat) > 38 else "?"
            out.append(f"{tid:>7} {cpu:>4} {state:>4}  {utime:>7} {stime:>6}  {wchan:<24} {comm}")
        except (FileNotFoundError, IndexError):
            pass
    return "\n".join(out)


def collect_softirqs():
    return Path("/proc/softirqs").read_text().rstrip()


def collect_kstacks(get_pid):
    pid = get_pid()
    if not pid:
        return "(no PID)"
    task_dir = Path(f"/proc/{pid}/task")
    if not task_dir.exists():
        return f"(pid {pid} gone)"
    out = []
    for tid_dir in sorted(task_dir.iterdir(), key=lambda p: int(p.name)):
        tid = tid_dir.name
        try:
            stat = (tid_dir / "stat").read_text().split()
            state = stat[2]
            wchan = (tid_dir / "wchan").read_text().strip() or "-"
            out.append(f"=== tid={tid} state={state} wchan={wchan} ===")
            stack = (tid_dir / "stack").read_text().rstrip()
            if stack:
                for line in stack.splitlines():
                    out.append(line)
        except (FileNotFoundError, PermissionError):
            pass
    return "\n".join(out)


def collect_d_state():
    out = []
    for pid_dir in Path("/proc").iterdir():
        if not pid_dir.name.isdigit():
            continue
        task_dir = pid_dir / "task"
        if not task_dir.exists():
            continue
        for tid_dir in task_dir.iterdir():
            try:
                stat = (tid_dir / "stat").read_text()
                fields = stat[stat.rfind(")") + 2:].split()
                state = fields[0]
                if state != "D":
                    continue
                comm = stat[stat.find("(") + 1: stat.rfind(")")]
                wchan = (tid_dir / "wchan").read_text().strip() or "-"
                out.append(f"=== pid={pid_dir.name} tid={tid_dir.name} comm={comm} wchan={wchan} ===")
                stack = (tid_dir / "stack").read_text().rstrip()
                if stack:
                    for line in stack.splitlines():
                        out.append(line)
            except (FileNotFoundError, PermissionError, IndexError):
                pass
    if not out:
        return "(no D-state threads)"
    return "\n".join(out)


def collect_net(ifaces):
    out = []
    for line in Path("/proc/net/snmp").read_text().splitlines():
        if line.startswith(("Udp:", "Ip:", "Tcp:")):
            out.append(line)
    out.append("softnet: " + Path("/proc/net/softnet_stat").read_text().replace("\n", " | "))
    ipbin = find_ip_bin()
    for d in ifaces:
        r = run([ipbin, "-s", "link", "show", d], timeout=2)
        for line in r.stdout.splitlines()[:6]:
            out.append(line)
    return "\n".join(out)


def collect_top(get_pid):
    out = []
    pid = get_pid()
    if pid:
        task_dir = Path(f"/proc/{pid}/task")
        if task_dir.exists():
            out.append("TID STATE UTIME STIME CPU POL")
            for tid_dir in sorted(task_dir.iterdir(), key=lambda p: int(p.name)):
                try:
                    stat = (tid_dir / "stat").read_text().split()
                    out.append(f"{tid_dir.name} {stat[2]} {stat[13]} {stat[14]} {stat[38] if len(stat) > 38 else '?'} {stat[39] if len(stat) > 39 else '?'}")
                except (FileNotFoundError, IndexError):
                    pass
    out.append("--- /proc/stat all cpus ---")
    try:
        for line in Path("/proc/stat").read_text().splitlines():
            if not line.startswith("cpu"):
                break
            out.append(line)
    except FileNotFoundError:
        pass
    out.append("--- /proc top by total cpu time (utime+stime) ---")
    rows = []
    try:
        for pid_dir in Path("/proc").iterdir():
            if not pid_dir.name.isdigit():
                continue
            try:
                stat = (pid_dir / "stat").read_text()
                fields = stat[stat.rfind(")") + 2:].split()
                state = fields[0]
                utime = int(fields[11])
                stime = int(fields[12])
                cpu = fields[36] if len(fields) > 36 else "?"
                comm = stat[stat.find("(") + 1: stat.rfind(")")]
                rows.append((utime + stime, pid_dir.name, cpu, state, comm))
            except (FileNotFoundError, IndexError, ValueError):
                pass
        rows.sort(reverse=True)
        out.append("TIME PID CPU STATE COMM")
        for r in rows[:20]:
            out.append(" ".join(map(str, r)))
    except Exception as e:
        out.append(f"top error: {e}")
    return "\n".join(out)


def stat_loop(prefix, period, fn, stop):
    while not stop.is_set():
        ts = time.time()
        try:
            txt = fn()
        except Exception as e:
            txt = f"ERROR: {e}"
        with PRINT_LOCK:
            for line in txt.splitlines():
                sys.stdout.write(f"{prefix} T={ts:.3f} {line}\n")
            sys.stdout.flush()
        stop.wait(period)


def start_stat_threads(side, get_pid, ifaces, stop):
    threads = [
        threading.Thread(target=stat_loop, args=(f"[stats:{side}:threads]", 1.0,
                                                  lambda: collect_threads(get_pid), stop)),
        threading.Thread(target=stat_loop, args=(f"[stats:{side}:softirqs]", 1.0,
                                                  collect_softirqs, stop)),
        threading.Thread(target=stat_loop, args=(f"[stats:{side}:net]", 1.0,
                                                  lambda: collect_net(ifaces), stop)),
        threading.Thread(target=stat_loop, args=(f"[stats:{side}:top]", 2.0,
                                                  lambda: collect_top(get_pid), stop)),
        threading.Thread(target=stat_loop, args=(f"[stats:{side}:kstack]", 2.0,
                                                  lambda: collect_kstacks(get_pid), stop)),
        threading.Thread(target=stat_loop, args=(f"[stats:{side}:dstate]", 2.0,
                                                  collect_d_state, stop)),
    ]
    for t in threads:
        t.daemon = True
        t.start()
    return threads


def cmd_remote(args):
    here = Path(__file__).parent
    cfg = here / "lab.ini"
    binary = here / "gofra-smoke"
    stop = threading.Event()
    procs = []

    gofra_proc = launch(["chrt", "-f", "10", str(binary), "--config", str(cfg)],
                        "[gofra:lab2]")
    procs.append(gofra_proc)

    start_stat_threads("lab2", lambda: gofra_proc.pid, ["eth0", "eth1", "eth2", "eth3", TUN], stop)

    def cleanup(*_):
        stop.set()
        for p in procs:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        time.sleep(0.5)
        sys.exit(0)

    signal.signal(signal.SIGTERM, cleanup)
    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGHUP, cleanup)
    procs[0].wait()
    cleanup()


def push(content, remote_path, mode=None):
    cmd = ["ssh", *SSH_OPTS, f"root@{LAB_SSH}",
           f"cat > {remote_path}" + (f" && chmod {mode} {remote_path}" if mode else "")]
    p = subprocess.run(cmd, input=content)
    if p.returncode:
        raise RuntimeError(f"push to {remote_path} failed")


def cmd_local(args):
    if os.geteuid() != 0:
        sys.exit("need root (sudo)")

    LOCAL_TMP.mkdir(exist_ok=True)
    (LOCAL_TMP / "local.ini").write_text(ini(LOCAL_VIP))

    me = Path(__file__).read_bytes()
    binary = Path(args.gofra).read_bytes()
    lab_cfg = ini(LAB_VIP).encode()

    echo("[setup]", "mkdir + push to lab2")
    subprocess.run(["ssh", *SSH_OPTS, f"root@{LAB_SSH}", f"mkdir -p {SSH_DIR}"], check=True)
    push(me, f"{SSH_DIR}/loopback.py", mode="755")
    push(binary, f"{SSH_DIR}/gofra-smoke", mode="755")
    push(lab_cfg, f"{SSH_DIR}/lab.ini")

    procs = []
    stop = threading.Event()

    gofra_proc = launch(["chrt", "-f", "10", args.gofra, "--config", str(LOCAL_TMP / "local.ini")],
                       "[gofra:lab1]")
    procs.append(gofra_proc)

    start_stat_threads("lab1", lambda: gofra_proc.pid, ["eth1", TUN], stop)

    procs.append(launch(["ssh", *SSH_OPTS, "-T", f"root@{LAB_SSH}",
                         f"python3 {SSH_DIR}/loopback.py remote"],
                        "[lab2]"))

    time.sleep(2)

    echo("[diag]", "ip route get " + LAB_VIP)
    r = run(["ip", "route", "get", LAB_VIP])
    echo("[diag]", r.stdout.rstrip())

    procs.append(launch(["ssh", *SSH_OPTS, "-T", f"root@{LAB_SSH}",
                         f"iperf3 -s -B {LAB_VIP} -p 5201"],
                        "[iperf-srv]"))
    time.sleep(0.5)

    echo("[setup]", f"iperf3 -c {LAB_VIP} -t {args.duration}")
    cli_args = ["iperf3", "-c", LAB_VIP, "-B", LOCAL_VIP, "-p", "5201", "-t", str(args.duration)]
    if args.mode == "udp":
        cli_args += ["-u", "-b", args.bw]

    cli = launch(cli_args, "[iperf-cli]")
    procs.append(cli)

    def cleanup(*_):
        stop.set()
        for p in procs:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        time.sleep(0.5)
        sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    cli.wait()
    echo("[setup]", "iperf done; gofra still running, Ctrl-C to stop")
    while True:
        time.sleep(60)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd")
    p_remote = sub.add_parser("remote")
    p_local = sub.add_parser("local")
    p_local.add_argument("--gofra", default="./gofra")
    p_local.add_argument("--mode", choices=["tcp", "udp"], default="tcp")
    p_local.add_argument("--duration", type=int, default=100)
    p_local.add_argument("--bw", default="1G")
    args = ap.parse_args()
    if args.cmd == "remote":
        cmd_remote(args)
    else:
        if not args.cmd:
            args.gofra = "./gofra"
            args.mode = "tcp"
            args.duration = 100
            args.bw = "1G"
        cmd_local(args)


if __name__ == "__main__":
    main()
