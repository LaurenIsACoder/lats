#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""hang-probe.py — reproduce the guest stall and capture thread/monitor state.

Boots lats with the given initrd, watches the serial log; when output stalls
without the DONE marker, captures:
  - per-thread host state (running vs futex sleep) via /proc
  - per-CPU guest registers + LAPIC via the QEMU monitor
Then quits the VM.
"""

import argparse
import os
import socket
import subprocess
import time
from pathlib import Path

MON = "/tmp/hp-mon.sock"
LOG = Path("/tmp/hp-guest.log")
PIDF = "/tmp/hp.pid"


def monitor(cmds, timeout=20):
    deadline = time.monotonic() + timeout
    while not os.path.exists(MON):
        if time.monotonic() > deadline:
            return "NO SOCKET"
        time.sleep(0.1)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(MON)
    s.settimeout(3)
    out = ""

    def drain():
        nonlocal out
        while True:
            try:
                c = s.recv(65536)
            except socket.timeout:
                break
            if not c:
                break
            out += c.decode(errors="replace")
            if out.rstrip().endswith("(qemu)"):
                break

    drain()
    for c in cmds:
        s.sendall((c + "\n").encode())
        time.sleep(0.4)
        drain()
    s.close()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", required=True)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--initrd", required=True)
    ap.add_argument("--stall-sec", type=float, default=25,
                    help="declare hang when log unchanged for this long")
    ap.add_argument("--max-wait", type=float, default=400)
    ap.add_argument("--latx-opt", default="")
    args = ap.parse_args()

    for f in (MON, LOG, PIDF):
        try:
            os.unlink(f)
        except FileNotFoundError:
            pass

    cmd = [
        args.qemu, "-M", "pc", "-m", "2048M", "-smp", "4",
        "-nographic", "-display", "none",
        "-serial", f"file:{LOG}", "-monitor", f"unix:{MON},server,nowait",
        "-no-reboot", "-enable-sync-profile", "-device", "edu",
        "-optbql", "waitn=1",
    ]
    if args.latx_opt:
        cmd += ["-latx", args.latx_opt]
    cmd += [
        "-kernel", args.kernel, "-initrd", args.initrd,
        "-append", "console=ttyS0 rdinit=/sbin/bql-init panic=-1",
        "-pidfile", PIDF,
    ]
    env = os.environ.copy()
    env["LATX_AOT"] = "0"
    proc = subprocess.Popen(cmd, env=env, stdout=open("/tmp/hp-qemu.out", "w"),
                            stderr=subprocess.STDOUT)
    print("qemu pid", proc.pid, flush=True)

    last_size, last_change, start = -1, time.monotonic(), time.monotonic()
    hung = False
    while time.monotonic() - start < args.max_wait:
        time.sleep(2)
        if proc.poll() is not None:
            break
        text = LOG.read_text(errors="replace") if LOG.exists() else ""
        if "[BQL-STRESS] DONE" in text:
            import re
            us = re.search(r"UPTIME_START=([0-9.]+)", text)
            ue = re.search(r"UPTIME_END=([0-9.]+)", text)
            el = (float(ue.group(1)) - float(us.group(1))) if us and ue else -1
            print("completed normally guest_elapsed=%.1f" % el, flush=True)
            break
        size = len(text)
        if size != last_size:
            last_size, last_change = size, time.monotonic()
        elif time.monotonic() - last_change > args.stall_sec and size > 0:
            hung = True
            break

    if hung:
        print("HANG detected, probing...", flush=True)
        pid = open(PIDF).read().strip()
        subprocess.run(
            f"ps -L -o tid,stat,%cpu,wchan:40,comm -p {pid} > /tmp/hp-threads.txt",
            shell=True)
        dump = monitor(
            sum(([f"cpu {i}", "info registers", "info lapic"] for i in range(4)),
                []) + ["info cpus", "info latx-irq"])
        Path("/tmp/hp-mon.txt").write_text(dump)
        print("probe saved: /tmp/hp-threads.txt /tmp/hp-mon.txt", flush=True)
    if proc.poll() is None:
        monitor(["quit"], timeout=10)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
    print("hung =", hung, flush=True)


if __name__ == "__main__":
    main()
