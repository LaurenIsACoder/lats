#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""run-bql-bench.py — host-side driver for LATS BQL contention benchmarks.

Per run:
  1. boot lats-x86_64 with -device edu, -enable-sync-profile and a
     prepared initrd (see prepare-initrd.sh)
  2. wait for the "[BQL-STRESS] DONE" marker on the serial log
  3. dump `info sync-profile` through the QEMU monitor (QSP report,
     includes per-callsite "BQL mutex" wait/hold times)
  4. quit QEMU and collect guest throughput + QSP counters

Aggregates runs into summary.json and prints a compact table.

Example:
  python3 run-bql-bench.py \
      --qemu /path/to/build/lats-x86_64 \
      --kernel /path/to/bzImage \
      --initrd /path/to/bql-initrd.cpio \
      --smp 4 --runs 5 --out-dir results/baseline
"""

import argparse
import json
import os
import re
import socket
import statistics
import subprocess
import sys
import time
from pathlib import Path

DONE_MARKER = "[BQL-STRESS] DONE"


def qemu_command(args, run_dir):
    command = [
        str(args.qemu),
        "-M", "pc",
        "-m", args.memory,
        "-smp", str(args.smp),
        "-nographic",
        "-display", "none",
        "-serial", f"file:{run_dir / 'guest.log'}",
        "-monitor", f"unix:{run_dir / 'mon.sock'},server,nowait",
        "-no-reboot",
        "-device", "edu",
        "-kernel", str(args.kernel),
        "-initrd", str(args.initrd),
        "-append",
        "console=ttyS0 rdinit=/sbin/bql-init panic=-1 " + args.kernel_append,
        *args.extra_qemu_arg,
    ]
    if args.sync_profile:
        command.insert(command.index("-device"), "-enable-sync-profile")
    return command


def monitor_session(sock_path, commands, timeout=30):
    """Send HMP commands over the monitor unix socket, return transcript."""
    sock_path = str(sock_path)
    deadline = time.monotonic() + timeout
    while not os.path.exists(sock_path):
        if time.monotonic() > deadline:
            raise TimeoutError("monitor socket did not appear")
        time.sleep(0.1)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(sock_path)
    sock.settimeout(5)
    transcript = ""

    def drain():
        nonlocal transcript
        while True:
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            transcript += chunk.decode(errors="replace")
            if transcript.rstrip().endswith("(qemu)"):
                break

    drain()
    for cmd in commands:
        sock.sendall((cmd + "\n").encode())
        time.sleep(0.5)
        drain()
    sock.close()
    return transcript


def wait_for_marker(log_path, process, marker, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if log_path.exists() and marker in log_path.read_text(errors="replace"):
            return True
        if process.poll() is not None:
            return False
        time.sleep(1)
    return False


def wait_for_done(log_path, process, timeout):
    return wait_for_marker(log_path, process, DONE_MARKER, timeout)


GUEST_LINE_RE = re.compile(r"^\[BQL-STRESS\] (.*)$", re.M)
GUEST_KV_RE = re.compile(r"(\w+)=([^\s]+)")


def parse_guest(log_text):
    out = {}
    for m in GUEST_LINE_RE.finditer(log_text):
        for k, v in GUEST_KV_RE.findall(m.group(1)):
            out[k] = v.rstrip("\r")
    return out


ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]|\x1b.")
QSP_BQL_RE = re.compile(
    r"^BQL mutex\s+\S+\s+(?P<site>\S+)\s+"
    r"(?P<wait_s>[0-9.]+)\s+(?P<count>\d+)\s+(?P<avg_us>[0-9.]+)\s*$",
    re.M)
QSP_MMIO_RE = re.compile(
    r"^(?P<type>MMIO (?:read|write))\s+\S+\s+(?P<site>\S+)\s+"
    r"(?P<wait_s>[0-9.]+)\s+(?P<count>\d+)\s+(?P<avg_us>[0-9.]+)\s*$",
    re.M)


def parse_qsp(text):
    """Extract BQL mutex rows from an `info sync-profile` transcript."""
    text = ANSI_RE.sub("", text)
    rows = []
    for m in QSP_BQL_RE.finditer(text):
        rows.append({
            "site": m.group("site"),
            "count": int(m.group("count")),
            "wait_s": float(m.group("wait_s")),
            "avg_us": float(m.group("avg_us")),
        })
    return rows


def parse_qsp_mmio(text):
    """Extract per-MemoryRegion MMIO BQL rows from a QSP report."""
    text = ANSI_RE.sub("", text)
    rows = []
    for match in QSP_MMIO_RE.finditer(text):
        rows.append({
            "type": match.group("type"),
            "site": match.group("site"),
            "count": int(match.group("count")),
            "wait_s": float(match.group("wait_s")),
            "avg_us": float(match.group("avg_us")),
        })
    return rows


def run_once(args, index):
    run_dir = args.out_dir / f"run-{index}"
    run_dir.mkdir(parents=True, exist_ok=True)
    for stale in ("guest.log", "mon.sock"):
        try:
            (run_dir / stale).unlink()
        except FileNotFoundError:
            pass
    command = qemu_command(args, run_dir)
    (run_dir / "qemu-cmd.json").write_text(json.dumps(command, indent=2))

    env = os.environ.copy()
    env["LATX_AOT"] = "0"
    start = time.monotonic()
    process = subprocess.Popen(
        command, env=env,
        stdout=(run_dir / "qemu.out").open("w"),
        stderr=subprocess.STDOUT,
    )
    try:
        ready = True
        if args.qsp_reset_marker:
            ready = wait_for_marker(
                run_dir / "guest.log", process, args.qsp_reset_marker,
                args.boot_timeout,
            )
            if ready:
                try:
                    (run_dir / "qsp-reset.txt").write_text(
                        monitor_session(run_dir / "mon.sock",
                                        ["sync-profile reset"]))
                except (OSError, TimeoutError) as exc:
                    ready = False
                    (run_dir / "qsp-reset.txt").write_text(
                        f"MONITOR ERROR: {exc}")
        done = ready and wait_for_done(
            run_dir / "guest.log", process, args.duration_margin,
        )
        qsp_text = ""
        if done:
            try:
                qsp_text = monitor_session(
                    run_dir / "mon.sock", ["info sync-profile"])
            except (OSError, TimeoutError) as exc:
                qsp_text = f"MONITOR ERROR: {exc}"
        (run_dir / "qsp.txt").write_text(qsp_text)
        if process.poll() is None:
            try:
                monitor_session(run_dir / "mon.sock", ["quit"], timeout=10)
            except (OSError, TimeoutError):
                pass
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    wall = round(time.monotonic() - start, 3)

    guest_log = (run_dir / "guest.log").read_text(errors="replace") \
        if (run_dir / "guest.log").exists() else ""
    result = {
        "run": index,
        "done": done,
        "wall_seconds": wall,
        "guest": parse_guest(guest_log),
        "qsp_bql": parse_qsp(qsp_text),
        "qsp_mmio": parse_qsp_mmio(qsp_text),
    }
    (run_dir / "result.json").write_text(json.dumps(result, indent=2))
    return result


def summarize(results):
    mmio, sd, wait, count = [], [], [], []
    mmio_regions = {}
    for r in results:
        if not r["done"]:
            continue
        g = r["guest"]
        if "mmio_ops_per_sec" in g:
            mmio.append(float(g["mmio_ops_per_sec"]))
        if "shootdown_ops_per_sec" in g:
            sd.append(float(g["shootdown_ops_per_sec"]))
        wait.append(sum(x["wait_s"] for x in r["qsp_bql"]))
        count.append(sum(x["count"] for x in r["qsp_bql"]))
        for row in r["qsp_mmio"]:
            key = (row["type"], row["site"])
            aggregate = mmio_regions.setdefault(
                key, {"type": row["type"], "site": row["site"],
                      "count": 0, "wait_s": 0.0})
            aggregate["count"] += row["count"]
            aggregate["wait_s"] += row["wait_s"]

    def stats(values):
        if not values:
            return None
        mean = statistics.mean(values)
        return {
            "n": len(values),
            "mean": mean,
            "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
            "cv_pct": (statistics.stdev(values) / mean * 100)
            if len(values) > 1 and mean else 0.0,
            "min": min(values),
            "max": max(values),
        }

    mmio_summary = sorted(mmio_regions.values(),
                          key=lambda row: row["wait_s"], reverse=True)
    for row in mmio_summary:
        row["avg_us"] = (row["wait_s"] * 1e6 / row["count"]
                         if row["count"] else 0.0)

    return {
        "mmio_ops_per_sec": stats(mmio),
        "shootdown_ops_per_sec": stats(sd),
        "qsp_bql_wait_s_total": stats(wait),
        "qsp_bql_lock_count": stats(count),
        "qsp_mmio": mmio_summary,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--initrd", type=Path, required=True,
                        help="prepared initrd from prepare-initrd.sh")
    parser.add_argument("--smp", type=int, default=4)
    parser.add_argument("--memory", default="2048M")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--boot-timeout", type=int, default=180)
    parser.add_argument("--duration-margin", type=int, default=90,
                        help="extra seconds after boot for the workload")
    parser.add_argument("--kernel-append", default="")
    parser.add_argument("--extra-qemu-arg", action="append", default=[])
    parser.add_argument("--no-sync-profile", dest="sync_profile",
                        action="store_false",
                        help="run without QSP instrumentation for overhead "
                             "checks")
    parser.add_argument("--qsp-reset-marker", default="",
                        help="reset QSP after this guest log marker appears")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--label", default="")
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    print(f"[bench] {args.runs} runs, smp={args.smp}, out={args.out_dir}",
          flush=True)
    results = []
    for i in range(args.runs):
        r = run_once(args, i)
        results.append(r)
        g = r["guest"]
        print(f"[bench] run-{i} done={r['done']} "
              f"mmio/s={g.get('mmio_ops_per_sec', '-')} "
              f"sd/s={g.get('shootdown_ops_per_sec', '-')} "
              f"bql_rows={len(r['qsp_bql'])} "
              f"mmio_rows={len(r['qsp_mmio'])}", flush=True)

    summary = summarize(results)
    report = {
        "label": args.label,
        "qemu": str(args.qemu),
        "smp": args.smp,
        "runs": args.runs,
        "summary": summary,
        "results": results,
    }
    (args.out_dir / "summary.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(summary, indent=2))
    ok = all(r["done"] for r in results)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
