# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Reject benchmark intervals with competing userspace threads on the core."""
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import threading
import time


def private_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "w") as stream:
        os.fchmod(stream.fileno(), 0o600)
        json.dump(data, stream, indent=2)
        stream.write("\n")


class HostContention(RuntimeError):
    def __init__(self, report, preflight=False):
        self.report = report
        self.preflight = preflight
        super().__init__("Host contention: "+str(report))


def cpu_list(value):
    result = set()
    for part in value.strip().split(","):
        start, _, end = part.partition("-")
        result.update(range(int(start), int(end or start) + 1))
    return result


def siblings(cpu):
    path = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list")
    return cpu_list(path.read_text()) if path.exists() else {cpu}


def snapshot(cpus):
    ticks = {}
    for line in Path("/proc/stat").read_text().splitlines():
        name, *values = line.split()
        if name.startswith("cpu") and name[3:].isdigit() and int(name[3:]) in cpus:
            ticks[name] = [int(x) for x in values[:8]]
    tasks = {}
    for process in Path("/proc").iterdir():
        if not process.name.isdigit():
            continue
        try:
            # Per-CPU kernel workers and interrupts are not orphan translators.
            with (process / "cmdline").open("rb") as command:
                if not command.read(1):
                    continue
            process_name = (process / "comm").read_text().strip()
            for task in (process / "task").iterdir():
                try:
                    tid = int(task.name)
                    affinity = os.sched_getaffinity(tid)
                    if not cpus.intersection(affinity):
                        continue
                    fields = (task / "stat").read_text().rsplit(") ", 1)[1].split()
                    runtime, delay, _ = map(int, (task / "schedstat").read_text().split())
                    tasks[tid] = dict(pid=int(process.name), tid=tid,
                        start_ticks=int(fields[19]), parent=int(fields[1]),
                        name=process_name, state=fields[0], cpu=int(fields[36]),
                        affinity=sorted(affinity), runtime_ns=runtime, delay_ns=delay)
                except (OSError, ValueError):
                    continue
        except OSError:
            continue
    return dict(at_ns=time.monotonic_ns(), ticks=ticks, tasks=tasks)


def competitors(before, after, allowed, cpus, totals, start_ticks, generic):
    found = []
    for tid, task in after["tasks"].items():
        if allowed.get(tid) == task["start_ticks"]:
            continue
        old = before["tasks"].get(tid)
        if old and old["start_ticks"] != task["start_ticks"]:
            old = None
        baseline = old["runtime_ns"] if old else (
            0 if task["start_ticks"] > start_ticks else task["runtime_ns"])
        delta = max(0, task["runtime_ns"] - baseline)
        pinned = set(task["affinity"]).issubset(cpus)
        translator = task["name"].startswith(("lats", "latx", "qemu"))
        # A broad mask and the last CPU alone cannot prove where it ran.
        recently_here = old and old["cpu"] in cpus and task["cpu"] in cpus
        if pinned or translator or recently_here:
            identity = (tid, task["start_ticks"])
            total = totals.get(identity, 0) + delta
            totals[identity] = total
            if not pinned and not translator:
                generic[identity] = dict(task)
            elif total >= 1_000_000:
                found.append(dict(**task, runtime_delta_ns=delta,
                                  runtime_total_ns=total,
                                  status="invalid" if pinned else "uncertain"))
    return found


def accounting(cpus, targets):
    """Bracket core ticks with selected runtime reads, before the slow scan."""
    def selected():
        values = {}
        for tid in targets:
            task = Path(f"/proc/{tid}")
            fields = (task / "stat").read_text().rsplit(") ", 1)[1].split()
            affinity = os.sched_getaffinity(tid)
            if len(affinity) != 1 or not affinity.issubset(cpus):
                raise RuntimeError("Measurement thread must remain pinned to one core")
            values[tid] = dict(start_ticks=int(fields[19]), cpu=next(iter(affinity)),
                              runtime_ns=int((task / "schedstat").read_text().split()[0]))
        return values
    start = time.monotonic_ns()
    before = selected()
    ticks = {}
    for line in Path("/proc/stat").read_text().splitlines():
        name, *values = line.split()
        if name.startswith("cpu") and name[3:].isdigit() and int(name[3:]) in cpus:
            ticks[int(name[3:])] = sum(map(int, values[:3]))
    after = selected()
    return dict(at_ns=time.monotonic_ns(), span_ns=time.monotonic_ns()-start,
                before=before, after=after, busy_ticks=ticks)


def core_budget(first, last, hz, background_fraction=.001):
    found = []
    for cpu, ticks in last["busy_ticks"].items():
        upper = 0
        for tid, end in last["after"].items():
            start = first["before"][tid]
            if (end["start_ticks"], end["cpu"]) != (start["start_ticks"], start["cpu"]):
                raise RuntimeError("Measurement thread identity or affinity changed")
            if end["cpu"] == cpu:
                upper += end["runtime_ns"] - start["runtime_ns"]
        busy = (ticks - first["busy_ticks"][cpu]) * 1e9 / hz
        residual = busy - upper
        # Engineering tolerance for tick quantization and scheduler accounting.
        elapsed = last["at_ns"] - first["at_ns"]
        tolerance = 6e9 / hz + first["span_ns"] + last["span_ns"] + background_fraction * elapsed
        found.append(dict(cpu=cpu, status="uncertain", busy_ns=busy,
                          target_upper_ns=upper, unexplained_ns=residual,
                          background_fraction=residual/elapsed if elapsed else 0,
                          tolerance_ns=tolerance, elapsed_ns=elapsed))
    return found


def unexplained(first, last, hz, background_fraction=.001):
    return [x for x in core_budget(first, last, hz, background_fraction)
            if x["unexplained_ns"] > x["tolerance_ns"]]


def generic_excess(generic, totals, elapsed, background_fraction=.001):
    budget = max(10_000_000, background_fraction * elapsed)
    return [dict(**task, runtime_total_ns=totals[identity], tolerance_ns=budget,
                 status="uncertain") for identity, task in generic.items()
            if totals[identity] >= budget]


class HostCoreGuard:
    """Samples on the controller CPU, including the measurement CPU's sibling."""
    def __init__(self, cpu, target_tids, output, interval=.25, background_fraction=.001):
        if not 0 <= background_fraction <= .005:
            raise ValueError("Background budget must be between 0 and 0.5 percent")
        self.background_fraction = background_fraction
        self.cpus = siblings(cpu)
        self.targets = set(target_tids)
        self.output = Path(output)
        self.interval = interval
        self.samples = []
        self.conflicts = []
        self.error = None
        self.stop = threading.Event()
        self.thread = None
        self.totals = {}
        self.generic = {}
        self.accounts = []
        self.core_conflicts = []
        self.hz = os.sysconf("SC_CLK_TCK")
        self.start_ticks = int(time.clock_gettime(time.CLOCK_BOOTTIME) * self.hz)

    def take(self):
        current_account = accounting(self.cpus, self.targets)
        self.accounts.append(current_account)
        # Rolling violations persist; the growing whole-window budget is only
        # judged at preflight/end, so early background activity can amortize.
        if len(self.accounts) > 1:
            begins = []
            for seconds in (1, 10):
                eligible = [x for x in self.accounts[:-1]
                            if x["at_ns"] <= current_account["at_ns"] - seconds*1e9]
                if eligible: begins.append(eligible[-1])
            for begin in begins:
                self.core_conflicts.extend(unexplained(begin, current_account, self.hz, self.background_fraction))
        current = snapshot(self.cpus)
        if self.samples:
            self.conflicts.extend(competitors(self.samples[-1], current,
                                               self.allowed, self.cpus, self.totals,
                                               self.start_ticks, self.generic))
        self.samples.append(current)

    def write(self):
        first, last = self.samples[0], self.samples[-1]
        account_first, account_last = self.accounts[0], self.accounts[-1]
        try:
            whole = core_budget(account_first, account_last, self.hz, self.background_fraction)
        except Exception as error:
            self.error = repr(error)
            whole = []
        core_conflicts = self.core_conflicts + [x for x in whole
                         if x["unexplained_ns"] > x["tolerance_ns"]]
        generic = generic_excess(self.generic, self.totals,
                                 account_last["at_ns"]-account_first["at_ns"], self.background_fraction)
        threads = self.conflicts + generic
        status = ("invalid" if any(x["status"] == "invalid" for x in self.conflicts)
                  else "uncertain" if threads or core_conflicts or self.error else "valid")
        target_delta = {}
        for tid, identity in self.allowed.items():
            a, b = first["tasks"].get(tid), last["tasks"].get(tid)
            if not a or not b or b["start_ticks"] != identity:
                status = "invalid"
                continue
            target_delta[tid] = dict(runtime_ns=b["runtime_ns"]-a["runtime_ns"],
                                    wait_ns=b["delay_ns"]-a["delay_ns"])
        core_delta = {name: [b-a for a, b in zip(first["ticks"][name], values)]
                      for name, values in last["ticks"].items()}
        self.summary = dict(status=status, logical_cpus=sorted(self.cpus),
            measurement_tids=sorted(self.targets), interval_seconds=self.interval,
            sample_count=len(self.samples), core_ticks_delta=core_delta,
            target_sched_delta=target_delta, competing_threads=threads,
            unexplained_core_activity=core_conflicts, core_background_budget=whole,
            tick_hz=self.hz, rule_version="background-budget-v2",
            background_fraction=self.background_fraction,
            accounting_tolerance=f"6 ticks + endpoint read spans + {100*self.background_fraction:g}% elapsed window; fixed competitors >=1ms rejected",
            observer_error=self.error)
        private_json(self.output, dict(summary=self.summary, samples=self.samples,
                                       accounting=self.accounts))
        self.summary.update(evidence_path=str(self.output),
            evidence_sha256=hashlib.sha256(self.output.read_bytes()).hexdigest())

    def __enter__(self):
        if self.cpus.intersection(os.sched_getaffinity(0)):
            raise RuntimeError("Pin the controller outside measurement CPUs "+
                               str(sorted(self.cpus)))
        self.accounts.append(accounting(self.cpus, self.targets))
        initial = snapshot(self.cpus)
        self.allowed = {tid: initial["tasks"][tid]["start_ticks"] for tid in self.targets
                        if tid in initial["tasks"]}
        if set(self.allowed) != self.targets:
            raise RuntimeError("Measurement thread missing or not pinned to its core")
        self.samples.append(initial)
        time.sleep(.1)
        self.take()
        self.write()
        if self.error or len(self.summary["target_sched_delta"]) != len(self.targets):
            raise RuntimeError("Failed to observe the measurement thread: "+str(self.summary))
        if self.summary["status"] != "valid":
            raise HostContention(self.summary, preflight=True)
        def observe():
            try:
                while not self.stop.wait(self.interval):
                    self.take()
            except BaseException as error:
                self.error = repr(error)
        self.thread = threading.Thread(target=observe, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, kind, value, traceback):
        self.stop.set()
        self.thread.join()
        try:
            self.take()
        except BaseException as error:
            self.error = repr(error)
        self.write()
        if kind is None:
            if self.error or len(self.summary["target_sched_delta"]) != len(self.targets):
                raise RuntimeError("Failed to observe the measurement thread: "+str(self.summary))
            if self.summary["status"] != "valid":
                raise HostContention(self.summary)


def guarded_measure(cpu, target_tids, directory, operation, *, attempt_context=None, background_fraction=.001):
    """At most three complete attempts, only after a contention rejection."""
    directory = Path(directory)
    rejected = []
    for number in range(1, 4):
        attempt = directory / ("attempt-%02d" % number)
        # A fresh VM can return a new measurement TID for every retry.
        resource = attempt_context(attempt) if attempt_context else nullcontext(target_tids)
        with resource as tids:
            monitor = HostCoreGuard(cpu, tids, attempt / "host-load.json",
                                    background_fraction=background_fraction)
            try:
                with monitor:
                    value = operation(attempt)
                    # Keep completed results even when the exit check rejects them.
                    private_json(attempt / "result.json", value)
            except HostContention as error:
                rejected.append(dict(attempt=number, preflight=error.preflight,
                                     report=error.report))
                private_json(directory / "rejected.json", rejected)
                # A persistent fixed competitor must be cleaned by its owner.
                if number == 3 or (error.preflight and error.report["status"] == "invalid"):
                    raise
                continue
            return value, dict(monitor.summary, accepted_attempt=number,
                               rejected_attempts=rejected)
