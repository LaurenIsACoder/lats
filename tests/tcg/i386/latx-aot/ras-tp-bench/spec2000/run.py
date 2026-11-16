#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Run complete CPU2000 integer inputs inside SYS/HAMT and validate outputs."""
from contextlib import contextmanager
import argparse
import fcntl
import hashlib
import importlib.util
import json
import math
import os
import queue
import signal
import shutil
import socket
import statistics
import subprocess
import sys
import threading
import time
import zlib
from pathlib import Path

def sha(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def parameter(value, size, file):
    if not isinstance(value, dict):
        return value
    selected = value.get(size)
    if isinstance(selected, dict):
        if file in selected: return selected[file]
        if 'default' in selected: return selected['default']
        default = value.get('default')
        if isinstance(default, dict) and file in default: return default[file]
    if size in value and not isinstance(selected, dict): return selected
    if file in value and not isinstance(value[file], dict): return value[file]
    return value.get('default')

def close_vms(vms, original_error=None):
    failures = []
    for vm in vms:
        try: vm.close()
        except BaseException as error: failures.append(error)
    if failures:
        primary = original_error if original_error is not None else failures[0]
        for error in failures:
            if error is not primary: primary.add_note('VM cleanup failed: '+repr(error))
        if original_error is None: raise primary

class VM:
    save_timeout = 1800
    exit_timeout = 10
    qmp_timeout = 10

    def __init__(self, command, directory, cpu):
        try:
            self.start(command, directory, cpu)
        except BaseException as error:
            close_vms([self], error)
            raise

    def start(self, command, directory, cpu):
        self.lines = queue.Queue()
        self.log = (directory / 'console.log').open('w')
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1, start_new_session=True)
        self.pgid = self.proc.pid
        def reader():
            for line in self.proc.stdout:
                # Output bytes have their own files; keep the readable protocol log.
                if not line.startswith('SPEC_HEX '):
                    self.log.write(line); self.log.flush()
                self.lines.put((time.monotonic_ns(), line.rstrip('\r\n')))
            self.lines.put((time.monotonic_ns(), None))
        self.reader = threading.Thread(target=reader, daemon=True)
        self.reader.start()
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        deadline = time.monotonic() + 30
        while True:
            try:
                self.socket.connect(str(directory / 'qmp.sock')); break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() > deadline: raise RuntimeError('QMP startup timeout')
                time.sleep(.1)
        self.io = self.socket.makefile('rwb', buffering=0)
        self.socket.settimeout(self.qmp_timeout)
        json.loads(self.io.readline()); self.qmp('qmp_capabilities')
        self.expect('SPEC_READY', 180)
        self.qmp('stop')
        cpus = self.qmp('query-cpus-fast')
        assert len(cpus) == 1, cpus
        self.tid = cpus[0]['thread-id']
        for task in Path(f'/proc/{self.proc.pid}/task').iterdir():
            os.sched_setaffinity(int(task.name), {cpu if int(task.name) == self.tid else 2})

    def qmp(self, command):
        deadline = time.monotonic() + self.qmp_timeout
        self.socket.settimeout(self.qmp_timeout)
        self.io.write((json.dumps({'execute': command}) + '\n').encode())
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0: raise TimeoutError('QMP request timeout: '+command)
            self.socket.settimeout(remaining)
            line = self.io.readline()
            if not line: raise RuntimeError('QMP connection closed: '+command)
            reply = json.loads(line)
            if 'error' in reply: raise RuntimeError(reply)
            if 'return' in reply: return reply['return']

    def line(self, deadline):
        try: timestamp, line = self.lines.get(timeout=max(.01, deadline - time.monotonic()))
        except queue.Empty: raise TimeoutError('guest protocol timeout')
        if line is None: raise RuntimeError(f'LATS exited: {self.proc.poll()}')
        return timestamp, line

    def expect(self, expected, timeout):
        deadline = time.monotonic() + timeout
        while self.line(deadline)[1] != expected: pass

    def run(self, item, directory, timeout):
        if directory.exists(): shutil.rmtree(directory)
        directory.mkdir(parents=True)
        self.qmp('cont')
        self.proc.stdin.write(f"RUN {item['index']}\n"); self.proc.stdin.flush()
        deadline = time.monotonic() + timeout
        start = result = file = None
        remaining = 0
        decompressor = digest = expected_hash = None
        raw_remaining = 0
        try:
            while True:
                timestamp, line = self.line(deadline)
                if line.startswith('SPEC_BEGIN '): start = timestamp
                elif line.startswith('SPEC_END '):
                    result = json.loads(line[9:]); result['host_ns'] = timestamp - start
                elif line.startswith('SPEC_GZIP '):
                    if file is not None: raise RuntimeError('Nested output header')
                    name, length, raw_length, expected_hash = line[10:].rsplit(' ', 3)
                    allowed = set(item['expected']) | {c['error'] for c in item['commands']}
                    if name not in allowed: raise RuntimeError(f'Unexpected output: {name}')
                    path = directory / name; path.parent.mkdir(parents=True, exist_ok=True)
                    file = path.open('wb'); remaining = int(length)
                    raw_remaining = int(raw_length)
                    decompressor = zlib.decompressobj(16 + zlib.MAX_WBITS)
                    digest = hashlib.sha256()
                elif line.startswith('SPEC_HEX '):
                    data = bytes.fromhex(line[9:])
                    if file is None or len(data) > remaining: raise RuntimeError('Invalid output stream')
                    remaining -= len(data)
                    raw = decompressor.decompress(data)
                    if len(raw) > raw_remaining: raise RuntimeError('Oversized decompressed output')
                    file.write(raw); digest.update(raw); raw_remaining -= len(raw)
                elif line == 'SPEC_FILE_END':
                    if file is None or remaining: raise RuntimeError('Truncated output')
                    raw = decompressor.flush()
                    file.write(raw); digest.update(raw); raw_remaining -= len(raw)
                    if raw_remaining or not decompressor.eof or decompressor.unused_data or digest.hexdigest() != expected_hash:
                        raise RuntimeError('Output length, gzip stream or guest SHA-256 mismatch')
                    file.close(); file = None
                elif line.startswith('SPEC_ERROR '): raise RuntimeError(line)
                elif line == 'SPEC_DONE':
                    if file is not None: raise RuntimeError('Incomplete output at DONE')
                    break
            if not result or result['key'] != item['bench'] + '/' + item['size']:
                raise RuntimeError('Missing benchmark result')
            return result
        finally:
            if file: file.close()
            original = sys.exception()
            try: self.qmp('stop')
            except BaseException as error:
                self._force_close = True
                close_vms([self], original if original is not None else error)
                if original is None: raise
                original.add_note('QMP stop failed: '+repr(error))

    def close(self):
        if getattr(self, '_closed', False): return
        self._closed = True
        def kill_group():
            try: os.killpg(self.pgid, signal.SIGKILL)
            except ProcessLookupError: pass
        try:
            if hasattr(self, 'proc'):
                if getattr(self, '_force_close', False): kill_group()
                else: self.proc.terminate()
                try: self.proc.wait(timeout=self.exit_timeout)
                except subprocess.TimeoutExpired:
                    kill_group(); self.proc.wait(timeout=self.exit_timeout)
            if hasattr(self, 'reader'):
                # Let normal AOT workers commit before terminating their group.
                self.reader.join(timeout=self.save_timeout)
                if self.reader.is_alive():
                    kill_group(); self.reader.join(timeout=self.exit_timeout)
                    raise TimeoutError('AOT save worker did not close its output')
        except BaseException:
            if hasattr(self, 'pgid'): kill_group()
            if hasattr(self, 'proc'): self.proc.wait(timeout=self.exit_timeout)
            if hasattr(self, 'reader'): self.reader.join(timeout=self.exit_timeout)
            raise
        finally:
            handles = [getattr(self, name, None) for name in ('io','socket','log')]
            if hasattr(self, 'proc'):
                handles.append(self.proc.stdin)
                if not hasattr(self, 'reader') or not self.reader.is_alive():
                    handles.append(self.proc.stdout)
            failures = []
            for handle in handles:
                if handle is not None:
                    try: handle.close()
                    except BaseException as error: failures.append(error)
            if failures:
                original = sys.exception()
                if original is None: raise failures[0]
                for error in failures: original.add_note('Handle cleanup failed: '+repr(error))

def validate(item, directory, spec):
    checks = []
    for name in item['expected']:
        actual = directory / name
        if not actual.exists():
            checks.append(dict(file=name, passed=False, reason='missing')); continue
        # Preload the host XS module before the legacy suite adds its Perl paths.
        command = ['perl', '-MCompress::Zlib', str(spec / 'bin/specdiff'), '-m', '-l', '10']
        for key, flag in [('reltol', '-r'), ('abstol', '-a')]:
            value = parameter(item.get(key), item['size'], Path(name).name)
            if value is not None: command += [flag, str(value)]
        for key, flag in [('obiwan', '-o'), ('compress_whitespace', '-c')]:
            if parameter(item.get(key), item['size'], Path(name).name): command += [flag]
        reference = spec / 'benchspec/CINT2000' / item['bench'] / 'data' / item['size'] / 'output' / name
        command += [str(reference), name]
        env = dict(os.environ, SPEC=str(spec))
        checked = subprocess.run(command, cwd=directory, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (directory / (name + '.compare.log')).write_text(checked.stdout)
        checks.append(dict(file=name, passed=checked.returncode == 0, returncode=checked.returncode,
            sha256=sha(actual), command=command))
    return checks

def measurement_plan(items, sizes, rounds, benches):
    if len(sizes.split(',')) != len(set(sizes.split(','))):
        raise ValueError('Duplicate sizes are not allowed')
    plan = []
    for size in sizes.split(','):
        selected = [item for item in items if item['size'] == size and
                    (not benches or item['bench'] in benches.split(','))]
        for round in range(1 if size == 'test' else rounds):
            for index, item in enumerate(selected):
                modes = ['legacy','tp'] if (round+index)%2 == 0 else ['tp','legacy']
                plan.extend((size, round, item['bench'], mode) for mode in modes)
    if len(plan) != len(set(plan)):
        raise ValueError("Duplicate benchmark inputs are not allowed")
    return plan

def atomic_text(path, text):
    temporary = path.with_name(path.name + '.new')
    with temporary.open('w') as output:
        output.write(text); output.flush(); os.fsync(output.fileno())
    temporary.replace(path)

def execution_files(root, spec, items, plan):
    files = [root/'scripts/cache-namespace.sh', spec/'bin/specdiff']
    files.extend(f for f in (root/'runtime/pc-bios').rglob('*') if f.is_file())
    selected = {(size,bench) for size,_,bench,_ in plan}
    for item in items:
        if (item['size'],item['bench']) in selected:
            files.extend(spec/'benchspec/CINT2000'/item['bench']/'data'/item['size']/'output'/name
                         for name in item.get('expected', []))
    return {str(f):sha(f) for f in sorted(set(files))}

def prepare_run(directory, metadata, plan, resume):
    """Resume a verified prefix; retain any interrupted pair before rerunning it."""
    raw = directory / 'raw.jsonl'
    meta = directory / 'metadata.json'
    identity = [key for key in metadata if key.endswith('_sha256')] + ['args']
    rows = []
    if resume:
        old = json.loads(meta.read_text())
        if any(old.get(key) != metadata[key] for key in identity):
            raise RuntimeError('Resume binary, inputs, driver or measurement settings changed')
        lines = raw.read_text().splitlines(keepends=True)
        for index, line in enumerate(lines):
            try: rows.append(json.loads(line))
            except json.JSONDecodeError:
                if index == len(lines)-1 and not line.endswith('\n'): break
                raise RuntimeError('Damaged saved measurement before the interrupted tail')
        if len(rows) > len(plan): raise RuntimeError('Too many saved measurements')
        for row, expected in zip(rows, plan):
            if (row['size'],row['round'],row['bench'],row['mode']) != expected:
                raise RuntimeError('Saved measurements do not follow the fixed order')
            if not row['passed'] or row['failed'] or not row['checks'] or not all(c['passed'] for c in row['checks']):
                raise RuntimeError('Saved output validation failed')
            if metadata['args']['guard_host_load']:
                report = row['host_load_guard']
                if report['status'] != 'valid' or report.get('observer_error') or report.get('background_fraction') != metadata['args']['background_budget_percent']/100:
                    raise RuntimeError('Saved background observation does not match this run')
        metadata['invocations'] = old['invocations']
        number = len(metadata['invocations'])
        while any(path.exists() for path in (directory/f'raw-before-resume-{number}.jsonl',
                  directory/f'metadata-before-resume-{number}.json',directory/f'run{number}')):
            number += 1
        # Preserve the entire previous file, including an unpaired accepted row.
        shutil.copyfile(raw, directory / f'raw-before-resume-{number}.jsonl')
        shutil.copyfile(meta, directory / f'metadata-before-resume-{number}.json')
        if len(rows) % 2: rows.pop()
    else:
        if raw.exists() or meta.exists():
            raise RuntimeError('Existing measurement directory; use --resume or a new --out')
        metadata['invocations'] = []
        number = 0
    metadata['invocations'].append(metadata['started'])
    atomic_text(raw, ''.join(json.dumps(row)+'\n' for row in rows))
    (directory / 'summary.json').unlink(missing_ok=True)
    atomic_text(meta, json.dumps(metadata, indent=2))
    return rows, number

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--root', type=Path, required=True, help='Existing microbenchmark runtime directory')
    p.add_argument('--spec', type=Path, required=True)
    p.add_argument('--fixture', type=Path, required=True)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--sizes', default='test,ref')
    p.add_argument('--rounds', type=int, default=3)
    p.add_argument('--tumode', choices=('jit', 'off'), default='jit',
                   help='Use JIT TU by default; off isolates ordinary JIT for diagnosis')
    p.add_argument('--vm-lifecycle', choices=('resident','fresh'), default='resident',
                   help='fresh creates and closes one VM per benchmark/mode/attempt')
    p.add_argument('--aot', choices=('off','on'), default='off')
    p.add_argument('--cpu', type=int, default=6)
    p.add_argument('--timeout', type=int, default=7200)
    p.add_argument('--benches', help='Optional subset for diagnosis; no total score for subsets')
    p.add_argument('--extra-latx', default='',
                   help='Additional translator options for separate compatibility tests')
    p.add_argument('--cache-root', type=Path,
                   help='Reuse private per-mode caches for AOT cold/hot correctness runs')
    p.add_argument('--guard-host-load', action='store_true',
                   help='Reject timings with competing host threads on the measurement core')
    p.add_argument('--background-budget-percent', type=float, default=.1,
                   help='Host background CPU budget, 0 to 0.5 percent; not an error bound')
    p.add_argument('--resume', action='store_true',
                   help='Resume completed pairs with exactly the same fresh JIT settings')
    a = p.parse_args()
    for key in ('root', 'spec', 'fixture', 'binary', 'out'):
        setattr(a, key, getattr(a, key).resolve())
    if a.cache_root:
        a.cache_root = a.cache_root.resolve()
    guard_path = a.root / 'scripts/host-load-guard.py'
    if a.guard_host_load:
        guard_spec = importlib.util.spec_from_file_location('host_guard', guard_path)
        host_guard = importlib.util.module_from_spec(guard_spec)
        guard_spec.loader.exec_module(host_guard)
    if a.rounds < 1: p.error('--rounds must be positive')
    if not 0 <= a.background_budget_percent <= .5:
        p.error('--background-budget-percent must be between 0 and 0.5')
    if a.resume and (a.vm_lifecycle != 'fresh' or a.aot != 'off' or a.cache_root):
        p.error('--resume requires fresh VMs, AOT off and no reused cache')
    if os.geteuid() != 0: p.error('Run with sudo for private cache mount namespace')
    a.out.mkdir(parents=True, exist_ok=True)
    with (a.out/'measurement.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        items = json.loads((a.fixture / 'manifest.json').read_text())
        metadata = dict(binary_sha256=sha(a.binary), initrd_sha256=sha(a.fixture / 'spec.cpio'),
            driver_sha256=sha(Path(__file__)), host_guard_sha256=sha(guard_path) if a.guard_host_load else None,
            kernel_sha256=sha(a.root / 'runtime/vmlinuz-4.4.0-186-generic-x64'),
            manifest_sha256=sha(a.fixture / 'manifest.json'), started=time.strftime('%Y-%m-%dT%H:%M:%S%z'),
            args={k:str(v) if isinstance(v, Path) else v for k,v in vars(a).items() if k != 'resume'},
            commands={}, output_protocol='gzip -1 / HEX, original length and guest SHA-256 checked',
            timing='guest CLOCK_MONOTONIC, sum of complete invoke commands; reset and export excluded')
        plan = measurement_plan(items, a.sizes, a.rounds, a.benches)
        if not plan: p.error('No selected benchmark inputs')
        metadata['measurement_order'] = plan
        metadata['execution_files_sha256'] = execution_files(a.root,a.spec,items,plan)
        rows, invocation = prepare_run(a.out, metadata, plan, a.resume)
        completed = {(r['size'],r['round'],r['bench']) for r in rows}
        vms = {}
        try:
            options = {'legacy':'ras=on,rastoptp=off', 'tp':'ras=on,rastoptp=on'}
            def command_for(mode, directory, cache):
                cache.mkdir(parents=True, exist_ok=True)
                return ['unshare','-m','--propagation','private','bash',str(a.root / 'scripts/cache-namespace.sh'),
                    str(cache),'--as-root','taskset','-c',str(a.cpu),str(a.binary),'-M','pc','-m','1024','-smp','1',
                    '-cpu','qemu64','-kernel',str(a.root / 'runtime/vmlinuz-4.4.0-186-generic-x64'),
                    '-initrd',str(a.fixture / 'spec.cpio'),'-append','console=ttyS0 rdinit=/init quiet clocksource=acpi_pm',
                    '-nographic','-no-reboot','-monitor','none','-qmp','unix:'+str(directory / 'qmp.sock')+',server=on,wait=off',
                    '-net','none','-L',str(a.root / 'runtime/pc-bios'),'-accel','tcg,mode=lat,thread=single','-latx',
                    'select,hamt=1,aotsys='+('1' if a.aot=='on' else '0')+
                    ',tumode='+('aot' if a.aot=='on' else a.tumode)+','+options[mode]+
                    (','+a.extra_latx if a.extra_latx else '')]
            for mode in options:
                directory=a.out/mode;directory.mkdir(exist_ok=True)
                cache=a.cache_root/mode if a.cache_root else directory/'cache'
                cache.mkdir(parents=True,exist_ok=True)
                metadata.setdefault('cache_input',{})[mode]={
                    str(f.relative_to(cache)):dict(bytes=f.stat().st_size,sha256=sha(f))
                    for f in cache.rglob('*') if f.is_file() and not f.name.endswith('.lock')}
                command=command_for(mode,directory,cache)
                metadata['commands'][mode]=command
                if a.vm_lifecycle=='resident':vms[mode]=VM(command,directory,a.cpu)
            metadata['vcpu_tids']={k:v.tid for k,v in vms.items()}
            atomic_text(a.out/'metadata.json',json.dumps(metadata,indent=2))
            with (a.out / 'raw.jsonl').open('a') as output:
                for size in a.sizes.split(','):
                    selected_items = [item for item in items if item['size'] == size and
                        (not a.benches or item['bench'] in a.benches.split(','))]
                    for round in range(1 if size == 'test' else a.rounds):
                        for item_index,item in enumerate(selected_items):
                            if (size,round,item['bench']) in completed: continue
                            for mode in (['legacy','tp'] if (round+item_index)%2==0 else ['tp','legacy']):
                                directory = a.out / f'run{invocation}' / mode / size / str(round) / item['bench']
                                active={}
                                @contextmanager
                                def fresh(attempt):
                                    vm_dir=attempt/'vm';vm_dir.mkdir(parents=True,exist_ok=True)
                                    cache=a.cache_root/mode if a.cache_root else vm_dir/'cache'
                                    command=command_for(mode,vm_dir,cache)
                                    vm=VM(command,vm_dir,a.cpu)
                                    active['vm']=vm
                                    active['identity']=dict(pid=vm.proc.pid,tid=vm.tid,command=command)
                                    try:yield [vm.tid]
                                    finally:close_vms([vm], sys.exception())
                                def execute(attempt):
                                    vm=active['vm'] if a.vm_lifecycle=='fresh' else vms[mode]
                                    data=vm.run(item,attempt/'work',a.timeout)
                                    verified=validate(item,attempt/'work',a.spec)
                                    if data['failed'] or not verified or not all(c['passed'] for c in verified):
                                        raise RuntimeError(f"Validation failed: {mode} {item['bench']} {size}")
                                    if a.vm_lifecycle=='fresh':data['vm']=active['identity']
                                    return data,verified
                                if a.guard_host_load:
                                    (result,checks),report=host_guard.guarded_measure(
                                        a.cpu, [] if a.vm_lifecycle=='fresh' else [vms[mode].tid],
                                        directory,execute,
                                        attempt_context=fresh if a.vm_lifecycle=='fresh' else None,
                                        background_fraction=a.background_budget_percent/100)
                                    result['host_load_guard']=report
                                elif a.vm_lifecycle=='fresh':
                                    attempt=directory/'attempt-01'
                                    with fresh(attempt):result,checks=execute(attempt)
                                else:
                                    result=vms[mode].run(item,directory,a.timeout)
                                    checks=validate(item,directory,a.spec)
                                passed = not result['failed'] and checks and all(c['passed'] for c in checks)
                                row = dict(mode=mode, round=round, bench=item['bench'], size=size,
                                    reference_seconds=item['reference_seconds'], passed=bool(passed), checks=checks, **result)
                                output.write(json.dumps(row)+'\n'); output.flush(); os.fsync(output.fileno()); rows.append(row)
                                print(mode, size, round, item['bench'], result['ns']/1e9, bool(passed), flush=True)
                                if not passed: raise RuntimeError(f"Validation failed: {mode} {item['bench']} {size}")
            summary = []
            for bench in sorted({r['bench'] for r in rows if r['size'] == 'ref'}):
                result = dict(bench=bench)
                for mode in options:
                    selected = [r for r in rows if r['bench']==bench and r['size']=='ref' and r['mode']==mode]
                    seconds = statistics.median(r['ns']/1e9 for r in selected)
                    ratio = selected[0]['reference_seconds']/seconds
                    result[mode] = dict(seconds=seconds, ratio=ratio, score=100*ratio, rounds=len(selected))
                result['speedup_percent'] = 100*(result['legacy']['seconds']/result['tp']['seconds']-1)
                summary.append(result)
            totals = {mode:math.exp(statistics.mean(math.log(r[mode]['score']) for r in summary)) for mode in options} if len(summary)==12 else None
            (a.out / 'summary.json').write_text(json.dumps(dict(benchmarks=summary, internal_score=totals,
                vm_lifecycle=a.vm_lifecycle,
                label='Internal CPU2000 ref measurement using custom SYS runner; not a reportable SPEC score'), indent=2))
        finally:
            close_vms(vms.values(), sys.exception())

if __name__ == '__main__': main()
