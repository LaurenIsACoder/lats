#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Read paused RAS state across syscall and 32 RET hits; no hot counters."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

spec = importlib.util.spec_from_file_location(
    "suite", Path(__file__).with_name("run-suite.py"))
suite = importlib.util.module_from_spec(spec)
spec.loader.exec_module(suite)

parser = argparse.ArgumentParser()
parser.add_argument("--root", type=Path, required=True)
parser.add_argument("--binary", type=Path, required=True)
parser.add_argument("--guest", type=Path, required=True)
parser.add_argument("--initrd", type=Path, required=True)
parser.add_argument("--out", type=Path, required=True)
parser.add_argument("--invalidate", choices=("lookup", "kernel", "user"),
                    help="Diagnostic: request a real cache operation at PRE")
args = parser.parse_args()
if os.geteuid() != 0:
    raise SystemExit("Run with sudo -n --preserve-env=HOME")
args.out.mkdir(parents=True, exist_ok=True)
symbols = subprocess.check_output(["nm", "-n", str(args.guest)], text=True)
begin, end = [int(re.search(r"^([0-9a-f]+) T " + name + r"$", symbols,
                            re.M)[1], 16)
              for name in ("ras_probe_begin", "ras_probe_end")]
metadata = {"binary_sha256": suite.sha(args.binary),
            "guest_sha256": suite.sha(args.guest),
            "initrd_sha256": suite.sha(args.initrd),
            "driver_sha256": suite.sha(Path(__file__)),
            "invalidate": args.invalidate,
            "guest_range": [begin, end], "commands": {}}


class QMP:
    def __init__(self, path, timeout=10):
        self.timeout = timeout
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.io = None
        self.sock.settimeout(timeout)
        try:
            self.sock.connect(str(path))
            self.io = self.sock.makefile("rwb", buffering=0)
            self._reply(time.monotonic() + timeout, "greeting")
            self.command("qmp_capabilities")
        except BaseException:
            self.close()
            raise

    def _reply(self, deadline, request):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("QMP request timeout: " + request)
        self.sock.settimeout(remaining)
        raw = self.io.readline()
        if not raw:
            raise RuntimeError("QMP disconnected: " + request)
        return json.loads(raw)

    def command(self, name):
        deadline = time.monotonic() + self.timeout
        self.sock.settimeout(self.timeout)
        self.io.write((json.dumps({"execute": name}) + "\n").encode())
        while True:
            reply = self._reply(deadline, name)
            if "error" in reply:
                raise RuntimeError(reply)
            if "return" in reply:
                return reply["return"]

    def close(self):
        try:
            if self.io is not None:
                self.io.close()
        finally:
            self.sock.close()


def snapshot(server, directory, phase):
    script = directory / (phase + ".gdb")
    script.write_text("set pagination off\nattach " + str(server.proc.pid) + r"""
python
import gdb,json
cpu=gdb.parse_and_eval('cpus.tqh_first')
env=cpu['env_ptr'].cast(gdb.lookup_type('CPUX86State').pointer()).dereference()
ras=env['ras_tp'];top=int(ras['top'])
assert 0<=top<=512
result=dict(top=top,pending=int(ras['reset_pending']),mtcc=int(ras['mtcc_id']),
 opt_syscall=int(gdb.parse_and_eval('option_opt_syscall')),
 opt_sysret=int(gdb.parse_and_eval('option_opt_sysret')),
 full_flushes=int(gdb.parse_and_eval('tb_ctx.tb_flush_count')),
 user_flushes=int(gdb.parse_and_eval('tb_ctx.tb_flush_count_mtcc[1]')),
 kernel_flushes=int(gdb.parse_and_eval('tb_ctx.tb_flush_count_mtcc[0]')),
 mtcc_enabled=bool(gdb.parse_and_eval('mtcc_enabled')),
 invalid_entries=int(env['hamt_invalid_tb_entries']),
 physical_invalidations=int(gdb.parse_and_eval('tb_ctx.tb_phys_invalidate_count')),
 entries=[dict(pc=int(ras['stack'][i]['pc']),ptr=int(ras['stack'][i]['ptr'])) for i in range(top)])
print('RAS_STATE='+json.dumps(result))
end
detach
""")
    output = subprocess.check_output(
        ["gdb", "-nx", "-q", "-batch", str(args.binary), "-x", str(script)],
        text=True, stderr=subprocess.STDOUT)
    (directory / (phase + "-gdb.log")).write_text(output)
    match = re.search(r"RAS_STATE=(.*)", output)
    if not match:
        raise RuntimeError(output)
    state = json.loads(match[1])
    subprocess.run(["python3", str(args.root / "scripts/snapshot-direct-warm-code.py"),
                    str(args.binary), str(server.proc.pid),
                    str(directory / (phase + "-code")), "--system",
                    "--start", hex(begin), "--end", hex(end)], check=True)
    return state


results = {}
for mode, options in (("fast", ""),
                      ("helper", ",opt_syscall=off,opt_sysret=off")):
    directory = args.out / mode
    directory.mkdir(exist_ok=True)
    cache = args.out / ("cache-" + mode)
    cmd = ["unshare", "-m", "--propagation", "private", "bash",
           str(args.root / "scripts/cache-namespace.sh"), str(cache), "--as-root",
           "taskset", "-c", "6", str(args.binary), "-M", "pc", "-m", "256",
           "-cpu", "qemu64", "-kernel", str(args.root / "runtime/vmlinuz-4.4.0-186-generic-x64"),
           "-initrd", str(args.initrd), "-append",
           "console=ttyS0 rdinit=/init quiet clocksource=acpi_pm", "-nographic",
           "-no-reboot", "-monitor", "none", "-qmp",
           "unix:" + str(directory / "qmp.sock") + ",server=on,wait=off",
           "-net", "none", "-L", str(args.root / "runtime/pc-bios"),
           "-accel", "tcg,mode=lat,thread=single", "-latx",
           "select,hamt=1,aotsys=0,tumode=jit,ras=on,rastoptp=on" + options]
    metadata["commands"][mode] = cmd
    (args.out / "metadata.json").write_text(json.dumps(metadata, indent=2))
    server = suite.Server(cmd, directory / "lats.log")
    qmp = None
    try:
        qmp = QMP(directory / "qmp.sock")
        cpus = qmp.command("query-cpus-fast")
        tid = cpus[0]["thread-id"]
        for thread in Path("/proc/%d/task" % server.proc.pid).iterdir():
            os.sched_setaffinity(int(thread.name), {6 if int(thread.name) == tid else 2})
        server.proc.stdin.write(b"G\n")
        server.proc.stdin.flush()
        phases = {}
        for phase in ("RAS_PRE", "RAS_POST", "RAS_OUTER"):
            _, line = server.until(phase, 180)
            assert line == phase, line
            qmp.command("stop")
            phases[phase] = snapshot(server, directory, phase)
            (directory / "states.json").write_text(json.dumps(phases, indent=2))
            if phase == "RAS_PRE" and args.invalidate:
                if args.invalidate in ("kernel", "user"):
                    assert phases[phase]["mtcc_enabled"] and phases[phase]["mtcc"] == 1
                function = ("tcg_flush_jmp_cache(cpus.tqh_first)" if args.invalidate == "lookup"
                            else "tb_flush_mtcc(cpus.tqh_first,%d)" % (args.invalidate == "user"))
                capture = subprocess.check_output(
                    ["gdb", "-nx", "-q", "-batch", str(args.binary),
                     "-ex", "attach " + str(server.proc.pid), "-ex", "call " + function,
                     "-ex", "detach"], text=True, stderr=subprocess.STDOUT)
                (directory / "invalidate-gdb.log").write_text(capture)
            qmp.command("cont")
            server.proc.stdin.write(b"G\n")
            server.proc.stdin.flush()
        _, line = server.until("RAS_PASS", 180)
        assert line == "RAS_PASS 570", line
        pre, post, outer = [phases[x] for x in ("RAS_PRE", "RAS_POST", "RAS_OUTER")]
        assert (pre["opt_syscall"], pre["opt_sysret"]) == (
            (2, 1) if mode == "fast" else (0, 0)), phases
        assert pre["top"] > 32, phases
        assert pre["pending"] == post["pending"] == outer["pending"] == 0, phases
        changed_counter = {"kernel": "kernel_flushes", "user": "user_flushes"}.get(args.invalidate)
        if args.invalidate == "kernel":
            assert post["physical_invalidations"] > pre["physical_invalidations"], phases
        for key in ("full_flushes", "user_flushes", "kernel_flushes", "invalid_entries", "mtcc"):
            expected = pre[key] + (1 if key == changed_counter else 0)
            assert post[key] == outer[key] == expected, (key, phases)
        if args.invalidate == "user":
            assert post["top"] == outer["top"] == 0, phases
        else:
            assert pre["top"] == post["top"] and pre["entries"] == post["entries"], phases
            assert outer["top"] == pre["top"] - 32 > 0, phases
            assert outer["entries"] == pre["entries"][:outer["top"]], phases
        returning = pre["entries"][-32:]
        assert all(begin <= e["pc"] < end for e in returning), returning
        results[mode] = {"states": phases, "ret_hits_proven": 0 if args.invalidate == "user" else 32,
                         "checksum": 570,
                         "basis": "user flush clears top" if args.invalidate == "user" else
                         "32 RETs and no CALL between POST and OUTER; any miss clears top"}
        (args.out / "results.json").write_text(json.dumps(results, indent=2))
    finally:
        if qmp:
            qmp.close()
        server.close()
print("PASS: default fast and helper syscalls: " + (
    "user flush clears top and execution falls back correctly" if args.invalidate == "user"
    else "top survives and all 32 returns hit"))
