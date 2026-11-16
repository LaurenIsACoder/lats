#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Reject damaged guest output before accepting a benchmark result."""
import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import types
import sys
import subprocess
import socket
import threading
import time
import zlib

spec = importlib.util.spec_from_file_location('spec_runner', Path(__file__).with_name('run.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
item = dict(index=0, bench='fixture', size='test', expected=['out'], commands=[])

def stream(raw, damaged=None):
    compressed = gzip.compress(raw)
    digest = hashlib.sha256(raw).hexdigest()
    if damaged == 'corrupt':
        compressed = compressed[:-8] + bytes([compressed[-8] ^ 1]) + compressed[-7:]
    if damaged == 'truncated': compressed = compressed[:-1]
    payload = compressed[:-1] if damaged == 'compressed_length' else compressed
    if damaged == 'trailing':
        payload += b'extra'; compressed = payload
    if damaged == 'hash': digest = '0' * 64
    raw_length = len(raw) + (1 if damaged == 'raw_length' else 0)
    lines = ['SPEC_BEGIN fixture/test', 'SPEC_END ' + json.dumps(dict(key='fixture/test',ns=1,failed=0)),
             f'SPEC_GZIP out {len(compressed)} {raw_length} {digest}']
    if damaged == 'nested': lines.append(lines[-1])
    lines += ['SPEC_HEX ' + payload[i:i+256].hex() for i in range(0,len(payload),256)]
    if damaged != 'early_done': lines.append('SPEC_FILE_END')
    lines.append('SPEC_DONE')
    return iter(enumerate(lines))

def receive(raw, directory, damaged=None):
    vm = runner.VM.__new__(runner.VM)
    vm.qmp = lambda command: None
    vm.proc = types.SimpleNamespace(stdin=io.StringIO())
    lines = stream(raw, damaged)
    vm.line = lambda deadline: next(lines)
    return vm.run(item, directory, 10)

with tempfile.TemporaryDirectory() as temp:
    directory = Path(temp) / 'result'
    for raw in [b'', bytes(range(256)) * 40000]:
        receive(raw, directory)
        assert (directory / 'out').read_bytes() == raw
    for damage in ['corrupt','truncated','compressed_length','raw_length',
                   'trailing','hash','nested','early_done']:
        try:
            receive(b'repeated output\n' * 1000, directory, damage)
        except (RuntimeError, zlib.error):
            pass
        else:
            raise AssertionError('Accepted ' + damage)
print('PASS: empty/large output; CRC, length, SHA, trailing data and protocol guards')

# Exercise the production fresh lifecycle without a licensed suite or native VM.
with tempfile.TemporaryDirectory() as temp:
    base=Path(temp);root=base/'root';fixture=base/'fixture';suite=base/'suite'
    (root/'runtime').mkdir(parents=True);fixture.mkdir();suite.mkdir()
    (root/'scripts').mkdir();(root/'scripts/cache-namespace.sh').write_text('cache namespace')
    (suite/'bin').mkdir();(suite/'bin/specdiff').write_text('output comparator')
    (root/'runtime/vmlinuz-4.4.0-186-generic-x64').write_bytes(b'kernel')
    (fixture/'spec.cpio').write_bytes(b'initrd');binary=base/'lats';binary.write_bytes(b'binary')
    items=[dict(index=2*i+j,bench='case'+str(i),size=size,reference_seconds=1,expected=['out'])
           for i in range(2) for j,size in enumerate(('test','ref'))]
    for entry in items:
        reference=suite/'benchspec/CINT2000'/entry['bench']/'data'/entry['size']/'output/out'
        reference.parent.mkdir(parents=True);reference.write_text('reference output')
    (fixture/'manifest.json').write_text(json.dumps(items))
    events=[];alive=[]
    class FakeVM:
        failure=False
        runs=0
        fail_after=None
        def __init__(self,command,directory,cpu):
            assert not alive,'two resident VMs in fresh mode'
            self.command=command;self.tid=100+len(events);self.proc=types.SimpleNamespace(pid=self.tid+1000)
            events.append(('open',self.tid));alive.append(self)
        def run(self,item,directory,timeout):
            assert alive==[self]
            self.__class__.runs+=1
            if self.fail_after is not None and self.runs > self.fail_after:
                raise RuntimeError('interrupted pair')
            if self.failure:raise RuntimeError('injected protocol failure')
            return dict(key=item['bench']+'/ref',ns=100,failed=0)
        def close(self):
            assert alive==[self];alive.clear();events.append(('close',self.tid))
    originals=(runner.VM,runner.validate,runner.os.geteuid,sys.argv)
    runner.VM=FakeVM;runner.validate=lambda *args:[dict(passed=True)];runner.os.geteuid=lambda:0
    try:
        sys.argv=['run.py','--root',str(root),'--spec',str(suite),'--fixture',str(fixture),'--binary',str(binary),'--out',str(base/'out'),'--sizes','ref','--rounds','3','--vm-lifecycle','fresh']
        runner.main()
        rows=[json.loads(line) for line in (base/'out/raw.jsonl').read_text().splitlines()]
        assert len(rows)==12 and not alive and len(events)==24
        assert all(events[i][0]=='open' and events[i+1]==('close',events[i][1]) for i in range(0,24,2))
        assert len({row['vm']['tid'] for row in rows})==12
        assert [row['mode'] for row in rows[:4]]==['legacy','tp','tp','legacy']
        assert [row['mode'] for row in rows[4:8]]==['tp','legacy','legacy','tp']
        assert all('aotsys=0,tumode=jit' in row['vm']['command'][-1] for row in rows)
        # Stop after one complete pair and one accepted half-pair. Resume must
        # preserve the old half-pair, rerun both modes and retain the fixed order.
        sys.argv[sys.argv.index('--out')+1]=str(base/'interrupted')
        FakeVM.runs=0;FakeVM.fail_after=3
        try:runner.main()
        except RuntimeError as error:assert str(error)=='interrupted pair'
        else:raise AssertionError('did not interrupt the pair')
        saved=[json.loads(line) for line in (base/'interrupted/raw.jsonl').read_text().splitlines()]
        assert len(saved)==3 and not alive
        FakeVM.fail_after=None
        sys.argv.append('--resume');runner.main()
        resumed=[json.loads(line) for line in (base/'interrupted/raw.jsonl').read_text().splitlines()]
        assert len(resumed)==12 and resumed[:2]==saved[:2]
        assert resumed[2]['vm']['pid']!=saved[2]['vm']['pid']
        backup=[json.loads(line) for line in (base/'interrupted/raw-before-resume-1.jsonl').read_text().splitlines()]
        assert backup==saved
        assert [r['mode'] for r in resumed]==[r['mode'] for r in rows]
        before=(runner.sha(base/'interrupted/raw.jsonl'),runner.sha(base/'interrupted/metadata.json'),len(events))
        sys.argv+=['--cpu','4']
        try:runner.main()
        except RuntimeError as error:assert 'settings changed' in str(error)
        else:raise AssertionError('resumed changed measurement identity')
        assert before==(runner.sha(base/'interrupted/raw.jsonl'),runner.sha(base/'interrupted/metadata.json'),len(events))
        sys.argv=sys.argv[:-2]
        for path in [root/'scripts/cache-namespace.sh',suite/'bin/specdiff',
                     suite/'benchspec/CINT2000/case0/data/ref/output/out']:
            original=path.read_bytes();path.write_bytes(original+b' changed')
            try:runner.main()
            except RuntimeError as error:assert 'settings changed' in str(error)
            else:raise AssertionError('resumed changed execution or verification files')
            assert before==(runner.sha(base/'interrupted/raw.jsonl'),runner.sha(base/'interrupted/metadata.json'),len(events))
            path.write_bytes(original)
        sys.argv.remove('--resume')
        FakeVM.failure=True;sys.argv[sys.argv.index('--out')+1]=str(base/'failed')
        try:runner.main()
        except RuntimeError as error:assert str(error)=='injected protocol failure'
        else:raise AssertionError('ignored protocol failure')
        assert not alive and events[-1][0]=='close'
    finally:
        runner.VM,runner.validate,runner.os.geteuid,sys.argv=originals
print('PASS: fresh measurements close each VM before the next, balance order and clean up failures')

# Model interrupted writes and a kill between canonical-file and metadata updates.
with tempfile.TemporaryDirectory() as temp:
    import copy
    base=Path(temp)
    plan=[('ref',0,'fixture','legacy'),('ref',0,'fixture','tp')]
    metadata=dict(binary_sha256='binary',args=dict(guard_host_load=False),started='first')
    row=dict(size='ref',round=0,bench='fixture',mode='legacy',passed=True,failed=0,checks=[dict(passed=True)])
    runner.prepare_run(base,copy.deepcopy(metadata),plan,False)
    original_meta=(base/'metadata.json').read_bytes()
    interrupted=json.dumps(row)+'\n'+ '{"mode":"tp"'
    (base/'raw.jsonl').write_text(interrupted)
    rows,number=runner.prepare_run(base,copy.deepcopy(metadata),plan,True)
    assert rows==[] and number==1
    assert (base/'raw-before-resume-1.jsonl').read_text()==interrupted
    # Simulate a kill before publishing the invocation increment.
    (base/'metadata.json').write_bytes(original_meta)
    rows,number=runner.prepare_run(base,copy.deepcopy(metadata),plan,True)
    assert rows==[] and number==2
    assert (base/'raw-before-resume-1.jsonl').read_text()==interrupted
    damaged='{bad}\n'+json.dumps(row)+'\n'
    (base/'raw.jsonl').write_text(damaged)
    try:runner.prepare_run(base,copy.deepcopy(metadata),plan,True)
    except RuntimeError as error:assert 'Damaged saved measurement' in str(error)
    else:raise AssertionError('accepted damaged non-tail record')
    assert (base/'raw.jsonl').read_text()==damaged
    try:runner.measurement_plan([dict(size='ref',bench='fixture')],'ref,ref',1,None)
    except ValueError as error:assert 'Duplicate sizes' in str(error)
    else:raise AssertionError('accepted duplicate size and reused output paths')
print('PASS: interrupted tail recovery, immutable backups, external identities and unique measurement plan')

# Use actual forked pipe writers to check AOT completion and timeout cleanup.
with tempfile.TemporaryDirectory() as temp:
    base=Path(temp)
    control=subprocess.Popen([sys.executable,'-c','import time;time.sleep(30)'],start_new_session=True)
    worker_code="""
import os,sys,time
from pathlib import Path
child=os.fork()
if child==0:
    Path(sys.argv[2]).write_text(str(os.getpid()))
    time.sleep(float(sys.argv[3]))
    Path(sys.argv[1]).write_text('committed')
    os._exit(0)
time.sleep(30)
"""
    try:
        for hanging in (False,True):
            directory=base/str(hanging);directory.mkdir()
            marker=directory/'committed';pidfile=directory/'worker.pid'
            vm=runner.VM.__new__(runner.VM)
            vm.log=(directory/'log').open('w')
            vm.proc=subprocess.Popen([sys.executable,'-u','-c',worker_code,str(marker),str(pidfile),'30' if hanging else '.2'],
                stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,start_new_session=True)
            vm.pgid=vm.proc.pid;vm.save_timeout=.02 if hanging else 2;vm.exit_timeout=2
            vm.reader=threading.Thread(target=lambda:list(vm.proc.stdout),daemon=True);vm.reader.start()
            deadline=time.monotonic()+2
            while not pidfile.exists():
                assert time.monotonic()<deadline;time.sleep(.01)
            worker=int(pidfile.read_text())
            try:vm.close()
            except TimeoutError:assert hanging
            else:assert not hanging and marker.read_text()=='committed'
            assert vm.proc.poll() is not None and not vm.reader.is_alive() and vm.log.closed
            assert vm.proc.stdin.closed and vm.proc.stdout.closed
            if hanging:
                status=Path('/proc')/str(worker)/'stat'
                try:state=status.read_text().split(') ',1)[1][0]
                except FileNotFoundError:state='gone'
                assert state in ('gone','Z')
            assert control.poll() is None
            vm.close() # repeated cleanup does not signal a reused PID
        partial=runner.VM.__new__(runner.VM)
        try:runner.VM.__init__(partial,['/nonexistent-lats-test-command'],base,0)
        except FileNotFoundError:pass
        else:raise AssertionError('Popen startup unexpectedly succeeded')
        assert partial.log.closed
    finally:
        control.terminate();control.wait(timeout=2)
closed=[]
class BadClose:
    def close(self):closed.append('bad');raise RuntimeError('cleanup failure')
class GoodClose:
    def close(self):closed.append('good')
try:runner.close_vms([BadClose(),GoodClose()])
except RuntimeError as error:assert str(error)=='cleanup failure'
else:raise AssertionError('cleanup error hidden')
assert closed==['bad','good']
original=RuntimeError('original benchmark failure')
runner.close_vms([BadClose(),GoodClose()],original)
assert str(original)=='original benchmark failure' and 'cleanup failure' in original.__notes__[0]
print('PASS: AOT workers finish normally, timeouts kill only their group, all VMs clean up and original errors survive')

# A dead QMP peer must not trap protocol timeout cleanup in readline().
vm=runner.VM.__new__(runner.VM)
vm.socket,peer=socket.socketpair();vm.io=vm.socket.makefile('rwb',buffering=0);vm.qmp_timeout=.02
started=time.monotonic()
try:
    try:vm.qmp('stop')
    except TimeoutError:pass
    else:raise AssertionError('unresponsive QMP accepted')
    assert time.monotonic()-started<1
finally:vm.io.close();vm.socket.close();peer.close()
with tempfile.TemporaryDirectory() as temp:
    vm=runner.VM.__new__(runner.VM)
    vm.proc=types.SimpleNamespace(stdin=io.StringIO())
    vm.line=lambda deadline: (_ for _ in ()).throw(TimeoutError('guest protocol timeout'))
    def qmp(command):
        if command=='stop':raise RuntimeError('QMP EOF')
    vm.qmp=qmp;closed=[];vm.close=lambda:closed.append(vm._force_close)
    try:vm.run(item,Path(temp)/'result',.02)
    except TimeoutError as error:
        assert str(error)=='guest protocol timeout' and 'QMP EOF' in error.__notes__[0]
    else:raise AssertionError('original protocol timeout lost')
    assert closed==[True]
print('PASS: unresponsive QMP has a bounded timeout and stop failure forces cleanup without replacing the original error')
