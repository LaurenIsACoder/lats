#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise real pinned, paused, waiting, and newly active child threads."""
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

path = Path(__file__).with_name("ras-tp-bench") / "scripts/host-load-guard.py"
spec = importlib.util.spec_from_file_location("guard", path)
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)
available = sorted(os.sched_getaffinity(0))
cpu = available[-1]
control = next((c for c in available if c not in guard.siblings(cpu)), None)
if control is None:
    raise SystemExit("Need two physical cores for the host-load guard test")
original_affinity = os.sched_getaffinity(0)
os.sched_setaffinity(0, {control})
visible = {os.getpid()}
original_snapshot = guard.snapshot
def test_snapshot(cpus):
    result = original_snapshot(cpus)
    # Isolate test fixtures from unrelated jobs on the developer's machine.
    result["tasks"] = {k:v for k,v in result["tasks"].items() if v["pid"] in visible}
    return result
guard.snapshot = test_snapshot
real_accounting = guard.accounting
def fixture_accounting(cpus, targets):
    result = real_accounting(cpus, targets)
    # /proc/stat cannot isolate fixtures from developer jobs. Thread tests
    # disable that independent detector; its real short-lived case is below.
    result["busy_ticks"] = {}
    return result
guard.accounting = fixture_accounting

child_code = r'''
import ctypes,os,signal,sys,time
os.sched_setaffinity(0,{int(sys.argv[1])})
ctypes.CDLL(None).prctl(15,sys.argv[2].encode(),0,0,0)
busy=sys.argv[3]=="busy"
def wake(*args):
    global busy
    busy=True
signal.signal(signal.SIGUSR1,wake)
print("READY",flush=True)
if sys.argv[3]=="short":
    end=time.monotonic()+.16
    while time.monotonic()<end:pass
    sys.exit(0)
if sys.argv[3]=="stdin":sys.stdin.readline()
else:
    while True:
        if busy:
            for i in range(10000):pass
        else:signal.pause()
'''
children = []
def start(name, mode):
    process = subprocess.Popen([sys.executable,"-c",child_code,str(cpu),name,mode],
        stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    children.append(process)
    assert process.stdout.readline().strip()=="READY"
    visible.add(process.pid)
    return process
def finish(process):
    process.kill()
    process.wait()
    visible.remove(process.pid)
    children.remove(process)

try:
    with tempfile.TemporaryDirectory(prefix="host-load-guard-") as directory:
        base = Path(directory)
        def run(name, target=(), action=None):
            monitor=guard.HostCoreGuard(cpu,target,base/(name+".json"),interval=.05)
            with monitor:
                if action:action()
                time.sleep(.16)
            assert monitor.summary["status"]=="valid"
            assert monitor.output.stat().st_mode & 0o777 == 0o600
            return monitor
        def rejected(name, target=(), action=None):
            try:run(name,target,action)
            except RuntimeError:
                report=json.loads((base/(name+".json")).read_text())
                assert report["summary"]["status"]=="invalid"
                assert report["summary"]["competing_threads"]
            else:raise AssertionError("accepted a competing busy thread")
        busy=start("lats-x86_64","busy")
        rejected("external")
        # Selection is per-thread identity, not a blanket process-family exemption.
        run("selected",(busy.pid,))
        # Existing permissive files must be hardened before writing new evidence.
        (base/"selected.json").chmod(0o644)
        run("selected",(busy.pid,))
        os.kill(busy.pid,signal.SIGSTOP)
        os.waitpid(busy.pid,os.WUNTRACED)
        run("paused-vm")
        finish(busy)
        waiting=start("bench-la","stdin")
        run("waiting-native")
        finish(waiting)
        writer=start("lats-x86_64","sleep")
        run("waiting-aot")
        rejected("active-aot",action=lambda:os.kill(writer.pid,signal.SIGUSR1))
        finish(writer)
        # The short-lived child is absent from every thread scan; core ticks
        # must still reject its runtime, including the SMT sibling separately.
        guard.accounting=real_accounting
        monitor=guard.HostCoreGuard(cpu,(),base/"short-lived.json",interval=1)
        try:
            with monitor:
                short=start("short-competitor","short")
                short.wait()
                visible.remove(short.pid)
                children.remove(short)
        except guard.HostContention:
            assert monitor.summary["unexplained_core_activity"]
            assert not monitor.summary["competing_threads"]
        else:raise AssertionError("missed a competitor between scans")
        guard.accounting=fixture_accounting
        # Sub-millisecond pieces must accumulate by thread identity.
        totals={}
        task=dict(tid=123,pid=123,start_ticks=100,runtime_ns=0,cpu=cpu,
                  affinity=[cpu],name="pieces")
        previous={"tasks":{123:dict(task)}}
        for n in range(1,4):
            task["runtime_ns"]=n*400_000
            current={"tasks":{123:dict(task)}}
            conflicts=guard.competitors(previous,current,{}, {cpu},totals,0,{})
            assert bool(conflicts)==(n==3)
            previous=current
        def account(ticks, runtime=0, elapsed=0):
            selected={1:dict(cpu=cpu,start_ticks=1,runtime_ns=runtime)}
            return dict(before=selected,after=selected,span_ns=0,busy_ticks=ticks,
                        at_ns=elapsed)
        # A target-accounting surplus on one core cannot cancel sibling work.
        sibling=cpu+1
        assert guard.unexplained(account({cpu:0,sibling:0}),
            account({cpu:0,sibling:20},runtime=300_000_000,elapsed=1_000_000_000),100)[0]["cpu"]==sibling
        # Dispersed 90ms over 116s fits the stated background budget; a ~100ms
        # burst in a 1s window does not. No CPU6 surplus can offset CPU7.
        assert not guard.unexplained(account({cpu:0}),
            account({cpu:9},elapsed=116_000_000_000),100)
        assert guard.unexplained(account({cpu:0}),
            account({cpu:10},elapsed=1_000_000_000),100)
        generic={(123,100):dict(task)}
        totals={(123,100):20_000_000}
        assert guard.generic_excess(generic,totals,1_000_000_000)
        assert not guard.generic_excess(generic,totals,116_000_000_000)
        # A prospective 0.5% policy accepts dispersed 0.3% background, but
        # still rejects 1% activity and preserves the fixed-thread rule above.
        totals[(123,100)]=300_000_000
        assert guard.generic_excess(generic,totals,100_000_000_000)
        assert not guard.generic_excess(generic,totals,100_000_000_000,.005)
        assert guard.unexplained(account({cpu:0}),
            account({cpu:30},elapsed=100_000_000_000),100)
        assert not guard.unexplained(account({cpu:0}),
            account({cpu:30},elapsed=100_000_000_000),100,.005)
        assert guard.unexplained(account({cpu:0}),
            account({cpu:100},elapsed=100_000_000_000),100,.005)
        for value in (-.001,.006,float('nan')):
            try:guard.HostCoreGuard(cpu,(),base/'bad-policy.json',background_fraction=value)
            except ValueError:pass
            else:raise AssertionError('accepted invalid background budget')
        # An endpoint identity/affinity error must overwrite valid preflight
        # evidence and abort as an observer error, without contention retries.
        real_budget = guard.core_budget
        fail_end = False
        def failed_budget(*args):
            if fail_end: raise RuntimeError("Measurement thread identity changed")
            return real_budget(*args)
        guard.core_budget = failed_budget
        try:
            monitor = guard.HostCoreGuard(cpu,(),base/"observer-failure.json")
            try:
                with monitor:
                    assert json.loads(monitor.output.read_text())["summary"]["status"]=="valid"
                    fail_end = True
            except RuntimeError as error:
                assert not isinstance(error,guard.HostContention)
                report=json.loads(monitor.output.read_text())["summary"]
                assert report["status"]!="valid" and report["observer_error"]
            else:raise AssertionError("accepted observer failure")
            # The same failure during preflight must also remain nonretryable.
            monitor = guard.HostCoreGuard(cpu,(),base/"preflight-failure.json")
            try:
                with monitor: raise AssertionError("entered after observer failure")
            except RuntimeError as error:
                assert not isinstance(error,guard.HostContention)
                report=json.loads(monitor.output.read_text())["summary"]
                assert report["status"]!="valid" and report["observer_error"]
        finally:
            guard.core_budget = real_budget
        # Retry only contention, preserve rejected work, and bound total attempts.
        from contextlib import contextmanager
        real_monitor = guard.HostCoreGuard
        class InjectedMonitor:
            outcomes = []
            def __init__(self, cpu, targets, output, **kwargs):
                self.targets = tuple(targets)
                self.output = Path(output)
                self.outcome = self.outcomes.pop(0)
                self.summary = dict(status="valid")
            def __enter__(self):
                if self.outcome == "fixed":
                    raise guard.HostContention(dict(status="invalid"), preflight=True)
                return self
            def __exit__(self, kind, value, traceback):
                guard.private_json(self.output, dict(outcome=self.outcome))
                if kind is None and self.outcome == "contention":
                    raise guard.HostContention(dict(status="uncertain"))
        guard.HostCoreGuard = InjectedMonitor
        try:
            calls = []
            def operation(attempt):
                calls.append(attempt)
                attempt.mkdir(parents=True, exist_ok=True)
                (attempt / "work.txt").write_text("retained")
                return len(calls)
            InjectedMonitor.outcomes = ["contention", "valid"]
            value, report = guard.guarded_measure(cpu, (), base / "retry", operation)
            assert value == 2 and report["accepted_attempt"] == 2
            assert len(report["rejected_attempts"]) == 1
            assert all((p / "work.txt").read_text() == "retained" for p in calls)
            assert [json.loads((p / "result.json").read_text()) for p in calls] == [1, 2]
            assert all((p / "result.json").stat().st_mode & 0o777 == 0o600 for p in calls)
            assert (base / "retry/rejected.json").stat().st_mode & 0o777 == 0o600
            for outcome, limit in (("contention", 3), ("fixed", 1)):
                calls.clear()
                InjectedMonitor.outcomes = [outcome] * limit
                try: guard.guarded_measure(cpu, (), base / outcome, operation)
                except guard.HostContention: pass
                else: raise AssertionError("accepted repeated contention")
                assert not InjectedMonitor.outcomes
                assert len(calls) == (0 if outcome == "fixed" else 3)
                assert all((p / "result.json").exists() for p in calls)
            InjectedMonitor.outcomes = ["contention", "valid"]
            calls.clear()
            def broken(attempt):
                calls.append(attempt)
                raise RuntimeError("checksum mismatch")
            try: guard.guarded_measure(cpu, (), base / "failure", broken)
            except RuntimeError as error: assert str(error) == "checksum mismatch"
            else: raise AssertionError("ignored operation failure")
            assert len(calls) == 1 and InjectedMonitor.outcomes == ["valid"]
            events=[]
            @contextmanager
            def fresh(attempt):
                identity=int(attempt.name[-2:])
                events.append(('open',identity))
                try: yield (identity,)
                finally: events.append(('close',identity))
            InjectedMonitor.outcomes=['contention','valid'];calls.clear()
            value,report=guard.guarded_measure(cpu,(),base/'fresh-retry',operation,attempt_context=fresh)
            assert value==2 and report['accepted_attempt']==2
            assert events==[('open',1),('close',1),('open',2),('close',2)]
            for outcome,op in [('fixed',operation),('valid',broken)]:
                events.clear();InjectedMonitor.outcomes=[outcome]
                try:guard.guarded_measure(cpu,(),base/('fresh-'+outcome),op,attempt_context=fresh)
                except RuntimeError:pass
                else:raise AssertionError('accepted invalid fresh attempt')
                assert events==[('open',1),('close',1)]
        finally:
            guard.HostCoreGuard = real_monitor
finally:
    for child in children:
        child.kill();child.wait()
    os.sched_setaffinity(0,original_affinity)
print("PASS: contention detection, private evidence, bounded retries and error propagation")
