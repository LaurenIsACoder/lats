#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Compare native, NJC, memory RAS and r2 RAS using the same JIT binary."""
import argparse,importlib.util,json,os,pathlib,re,socket,statistics,subprocess,time
spec=importlib.util.spec_from_file_location('suite',pathlib.Path(__file__).with_name('run-suite.py'))
suite=importlib.util.module_from_spec(spec);spec.loader.exec_module(suite)
p=argparse.ArgumentParser();p.add_argument('--root',default=str(pathlib.Path(__file__).resolve().parent.parent))
p.add_argument('--out',required=True);p.add_argument('--rounds',type=int,default=3)
p.add_argument('--lengths',default='100000000,400000000',help='Alternate lengths within each persistent VM')
p.add_argument('--cases',default=','.join(suite.CASES))
p.add_argument('--guest',default='bench-x86');p.add_argument('--initrd',default='bench.cpio');p.add_argument('--native-binary',default='build/bench-la')
p.add_argument('--trace-start',type=lambda v:int(v,0),default=0x403340);p.add_argument('--trace-end',type=lambda v:int(v,0),default=0x4037e0)
p.add_argument('--binary',default='build/lats/lats-x86_64')
p.add_argument('--pmu',action='store_true')
p.add_argument('--background-budget-percent',type=float,default=.1,help='Host background CPU budget, 0 to 0.5 percent; not an error bound')
p.add_argument('--branch-misses',action='store_true',help='Also count generic branch misses (requires --pmu)')
p.add_argument('--warm-code-snapshots',action='store_true',help='Read paused hot code before and after the measured batch')
p.add_argument('--interrupt-log',action='store_true',help='Independent diagnostic: count accepted interrupts around complete commands, including warmup')
p.add_argument('--counter-handshake',action='store_true',help='Wait for GO before execution and STOP/NEXT before result formatting')
p.add_argument('--guest-env',action='append',default=[],help='NAME=VALUE passed to native, LAT and the VM init environment')
p.add_argument('--cold-state-snapshots',action='store_true',help='Read per-vCPU cold-link and invalid-entry counters; separate from timing')
p.add_argument('--flush-after-warmup',choices=['full','mtcc'],help='Request a real safe code-cache flush after correctness warmup')
p.add_argument('--extra-latx',default='',help='Additional options applied to all VM modes, for configuration regressions')
p.add_argument('--inject-invalid-entry',type=lambda x:int(x,0),help='Diagnostic only: leave an old CALL target cached and replace its entry with the invalid sentinel')
p.add_argument('--compare-tu',action='store_true',help='Diagnostic only: interleave NJC/r2 with TU off/jit using identical defaults')
p.add_argument('--poc-binary',help='Frozen 7/6 PoC binary for same-batch comparison')
p.add_argument('--smp',type=int,default=1)
p.add_argument('--vcpu-host-cpus',default='6,4,0,2')
p.add_argument('--modes',help='Comma-separated VM modes, native is always included')
a=p.parse_args()
if not 0 <= a.background_budget_percent <= .5:p.error('--background-budget-percent must be between 0 and 0.5')
lengths=[int(n) for n in a.lengths.split(',')];assert len(lengths)==2 and len(set(lengths))==2 and min(lengths)>0
if a.compare_tu and (a.poc_binary or 'tumode=' in a.extra_latx):p.error('--compare-tu requires one binary and controls tumode per group')
if a.counter_handshake:
    if any(x.startswith('BENCH_COUNTER_HANDSHAKE=') for x in a.guest_env):raise SystemExit('handshake environment is set automatically')
    a.guest_env.append('BENCH_COUNTER_HANDSHAKE=1')
root=pathlib.Path(a.root).resolve();out=root/a.out;out.mkdir(parents=True,exist_ok=True)
if os.geteuid()!=0:raise SystemExit('Run with sudo -n --preserve-env=HOME; each VM uses a private mount namespace')
if not 0 <= a.trace_start < a.trace_end:raise SystemExit('Invalid guest trace range')
if a.interrupt_log and a.pmu:raise SystemExit('interrupt logging must run separately')
if a.cold_state_snapshots and a.pmu:raise SystemExit('Cold state diagnostics and PMU timing must be separate')
if a.flush_after_warmup and (a.pmu or not a.cold_state_snapshots):raise SystemExit('Flush regression requires cold diagnostics and no PMU')
if a.branch_misses and not a.pmu:raise SystemExit('--branch-misses requires --pmu')
if a.pmu and not a.counter_handshake:raise SystemExit('PMU measurements require GO/STOP/NEXT counter handshake')
if a.inject_invalid_entry and (a.modes not in ('tp','legacy','disabled') or a.pmu or a.warm_code_snapshots):raise SystemExit('Entry injection requires one VM mode and no performance/code-identity checks')
if any(not re.fullmatch(r'[A-Z_][A-Z0-9_]*=[A-Za-z0-9_.-]+',x) for x in a.guest_env):raise SystemExit('Invalid --guest-env assignment')
binary=root/a.binary;cases=a.cases.split(',')
host_cpus=[int(x) for x in a.vcpu_host_cpus.split(',')];assert 1 <= a.smp <= len(host_cpus)
if a.pmu and a.smp != 1:raise SystemExit('Multi-vCPU regression has no PMU timing')
inject_mode=a.modes
servers={};connections={};mode_binaries={};metadata=dict(binary_sha256=suite.sha(binary),x86_sha256=suite.sha(root/a.guest),native_sha256=suite.sha(root/a.native_binary),initrd_sha256=suite.sha(root/a.initrd),aslr=pathlib.Path('/proc/sys/kernel/randomize_va_space').read_text().strip(),commands={},cache_input={},started=time.strftime('%Y-%m-%dT%H:%M:%S%z'),controller_affinity=sorted(os.sched_getaffinity(0)),counter_protocol='GO/STOP/NEXT with per-row counter times' if a.counter_handshake else 'asynchronous BEGIN/END')
metadata['args']=vars(a)
metadata['driver_sha256']={name:suite.sha(root/'scripts'/name) for name in ['run-tp-ras-compare.py','run-suite.py','host-load-guard.py','snapshot-direct-warm-code.py','cache-namespace.sh']}
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

try:
    cmd=(['env']+a.guest_env if a.guest_env else [])+['taskset','-c',str(host_cpus[0]),str(root/a.native_binary),'--server'];metadata['lengths']=lengths;metadata['commands']['native']=cmd
    servers['native']=suite.Server(cmd,out/'native.log',handshake=a.counter_handshake)
    mode_options = {
        'disabled': 'ras=off',
        'legacy': 'ras=on',
        'tp': 'ras=on,rastoptp=on',
        **({'poc':'ras=on,rastoptp=on'} if a.poc_binary else {}),
    }
    if a.compare_tu:
        mode_options={'disabled_off':'ras=off','disabled_jit':'ras=off','tp_off':'ras=on,rastoptp=on','tp_jit':'ras=on,rastoptp=on'}
    if a.modes:
        selected=a.modes.split(','); assert set(selected) <= set(mode_options)
        mode_options={k:v for k,v in mode_options.items() if k in selected}
    for mode,extra in mode_options.items():
        mode_binary=mode_binaries[mode]=root/a.poc_binary if mode=='poc' else binary
        metadata.setdefault('mode_binary_sha256',{})[mode]=suite.sha(mode_binary)
        cache='cache-'+mode
        d=out/mode;d.mkdir(exist_ok=True);cache=root/cache
        if a.cold_state_snapshots:
            (d/'context-state.jsonl').write_text('')
            (d/'cold-state.log').write_text('')
        metadata['cache_input'][mode]={str(f.relative_to(cache)):dict(bytes=f.stat().st_size,sha256=suite.sha(f)) for f in cache.rglob('*') if f.is_file()}
        boot_affinity=','.join(str(cpu) for cpu in host_cpus[:a.smp])
        tumode=('off' if mode.endswith('_off') else 'jit') if a.compare_tu else 'jit'
        options='select,hamt=1,aotsys=0,tumode='+tumode+','+extra+(','+a.extra_latx if a.extra_latx else '')
        cmd=['unshare','-m','--propagation','private','bash',str(root/'scripts/cache-namespace.sh'),str(cache),'--as-root','taskset','-c',boot_affinity,str(mode_binary),'-M','pc','-m','256','-smp',str(a.smp),'-cpu','qemu64','-kernel',str(root/'runtime/vmlinuz-4.4.0-186-generic-x64'),'-initrd',str(root/a.initrd),'-append','console=ttyS0 rdinit=/init quiet clocksource=acpi_pm'+''.join(' '+x for x in a.guest_env),'-nographic','-no-reboot','-monitor','none','-qmp','unix:'+str(d/'qmp.sock')+',server=on,wait=off','-net','none','-L',str(root/'runtime/pc-bios'),'-accel','tcg,mode=lat,thread='+('multi' if a.smp>1 else 'single'),'-latx',options]
        if a.interrupt_log:cmd+=['-d','int','-D',str(d/'interrupt.log')]
        metadata['commands'][mode]=cmd;servers[mode]=suite.Server(cmd,d/'lats.log',handshake=a.counter_handshake)
        q=connections[mode]=QMP(d/'qmp.sock');q.command('stop')
        cpus=q.command('query-cpus-fast');assert len(cpus)==a.smp
        tids={c['thread-id']:host_cpus[c['cpu-index']] for c in cpus};tid=cpus[0]['thread-id']
        assert len(tids)==a.smp,('vCPUs must use distinct host threads',cpus)
        for t in pathlib.Path('/proc/%d/task'%servers[mode].proc.pid).iterdir():os.sched_setaffinity(int(t.name),{tids.get(int(t.name),2)})
        metadata.setdefault('vcpu_tids',{})[mode]=list(tids)
        if a.pmu:servers[mode].counters=[suite.DeltaCounter(tid,event) for event in ([0,1,5] if a.branch_misses else [0,1])]
    if a.pmu:servers['native'].counters=[suite.DeltaCounter(servers['native'].proc.pid,event) for event in ([0,1,5] if a.branch_misses else [0,1])]
    (out/'metadata.json').write_text(json.dumps(metadata,indent=2))
    def cold_state(mode):
        connections[mode].command('stop')
        script=out/mode/'cold-state.gdb'
        script.write_text('set pagination off\nattach '+str(servers[mode].proc.pid)+r"""
python
import gdb,json,hashlib
states=[]
cpu=gdb.parse_and_eval('cpus.tqh_first')
while int(cpu):
    env=cpu['env_ptr'].cast(gdb.lookup_type('CPUX86State').pointer()).dereference()
    states.append(dict(cpu=int(cpu['cpu_index']),top=int(env['ras_tp']['top']),reset_pending=int(env['ras_tp']['reset_pending']),invalid_entries=int(env['hamt_invalid_tb_entries']),full_flushes=int(gdb.parse_and_eval('tb_ctx.tb_flush_count')),user_flushes=int(gdb.parse_and_eval('tb_ctx.tb_flush_count_mtcc[1]')),**{k:int(env['ras_tp'][k]) for k in ('initial_links','queued_links','repaired_links','expired_links')}))
    cpu=cpu['node']['tqe_next']
print('COLD_STATE='+json.dumps(states))
contexts=[]
for i in range(int(gdb.parse_and_eval('tcg_cur_ctxs'))):
    ctx=gdb.parse_and_eval('tcg_ctxs[%d]'%i)
    contexts.append(dict(index=i,mtcc=int(ctx['mtcc_id']),begin=int(ctx['code_gen_buffer']),end=int(ctx['code_gen_highwater']),current=int(ctx['code_gen_ptr'])))
prefixes=[]
for i in range(int(gdb.parse_and_eval('region.n_mtcc'))):
    base=int(gdb.parse_and_eval('region.start_aligned'))+i*int(gdb.parse_and_eval('region.mtcc_n'))*int(gdb.parse_and_eval('region.stride'))
    after=int(gdb.parse_and_eval('region.after_prologue[%d]'%i))
    size=after if int(gdb.parse_and_eval('option_region_prologue')) else after-base
    assert 0<size<1048576
    raw=gdb.selected_inferior().read_memory(base,size).tobytes()
    prefixes.append(dict(mtcc=i,begin=base,size=size,sha256=hashlib.sha256(raw).hexdigest()))
print('CONTEXT_STATE='+json.dumps(dict(contexts=contexts,prefixes=prefixes)))
end
detach
""")
        capture=subprocess.check_output(['gdb','-nx','-q','-batch',str(mode_binaries[mode]),'-x',str(script)],text=True,stderr=subprocess.STDOUT)
        with (out/mode/'cold-state.log').open('a') as log:log.write(capture)
        match=re.search(r'COLD_STATE=(.*)',capture)
        assert match,capture
        context_match=re.search(r'CONTEXT_STATE=(.*)',capture)
        assert context_match,capture
        context_state=json.loads(context_match[1])
        contexts=context_state['contexts']
        for i,left in enumerate(contexts):
            for right in contexts[i+1:]:assert left['end']<=right['begin'] or right['end']<=left['begin'],context_state
        with (out/mode/'context-state.jsonl').open('a') as log:log.write(json.dumps(context_state)+'\n')
        return json.loads(match[1])
    guard_index=0
    def measure(mode,case,n,guarded=False):
        global guard_index
        for m,q in connections.items():q.command('cont' if m==mode and not a.interrupt_log else 'stop')
        interrupt_path=out/mode/'interrupt.log'
        position=interrupt_path.stat().st_size if a.interrupt_log and mode in connections else None
        if position is not None:connections[mode].command('cont')
        if guarded:
            tids=metadata['vcpu_tids'][mode] if mode in connections else [servers[mode].proc.pid]
            def execute(attempt):
                result=servers[mode].run(case,n)
                if result['checksum']!=expected_checksums[case,n]:
                    raise RuntimeError('checksum mismatch: '+str(result))
                return result
            row,report=suite.host_guard.guarded_measure(host_cpus[0],tids,out/'host-load'/('%05d'%guard_index),execute,background_fraction=a.background_budget_percent/100)
            guard_index+=1
            row['host_load_guard']=report
        else:row=servers[mode].run(case,n)
        if position is not None:
            connections[mode].command('stop')
            with interrupt_path.open('rb') as f:f.seek(position);raw=f.read()
            row['interrupt_command_window']={'byte_start':position,'byte_end':position+len(raw),'accepted_hardware':len(re.findall(rb'^Servicing hardware INT=',raw,re.M)),'accepted_virtual':len(re.findall(rb'^Servicing virtual hardware INT=',raw,re.M)),'vectors':re.findall(r'^Servicing (?:virtual )?hardware INT=(0x[0-9a-f]+)',raw.decode(errors='replace'),re.M),'includes':'paused endpoints bracket resume, warmup, protocol, result and pause acknowledgement; diagnostic only'}
        if a.cold_state_snapshots and mode in connections:row['cold_state_after_command']=cold_state(mode)
        return row
    if a.cold_state_snapshots:
        metadata['cold_state_ready']={mode:cold_state(mode) for mode in connections}
        (out/'metadata.json').write_text(json.dumps(metadata,indent=2))
    correctness=[]
    for case in cases:
        for n in [1,17,10000]:
            values={mode:measure(mode,case,n) for mode in servers}
            assert len({r['checksum'] for r in values.values()})==1,(case,n,values)
            correctness.append(dict(case=case,iterations=n,values=values))
    (out/'correctness.json').write_text(json.dumps(correctness,indent=2))
    def snapshot(stage):
        for q in connections.values():q.command('stop')
        for mode in connections:
            cmd=['python3',str(root/'scripts/snapshot-direct-warm-code.py'),str(mode_binaries[mode]),str(servers[mode].proc.pid),str(out/'warm-code'/stage/mode),'--start',hex(a.trace_start),'--end',hex(a.trace_end),'--system']
            subprocess.run(cmd,check=True)
    def read_invalid_entries():
        q=connections[inject_mode];q.command('stop')
        cmd=['gdb','-nx','-q','-batch',str(binary),'-ex','attach '+str(servers[inject_mode].proc.pid),'-ex','p/x ((CPUX86State *)cpus.tqh_first->env_ptr)->hamt_invalid_tb_entries','-ex','detach']
        capture=subprocess.check_output(cmd,text=True,stderr=subprocess.STDOUT)
        values=re.findall(r'\$\d+ = (0x[0-9a-f]+|\d+)',capture)
        assert len(values)==1,capture
        return int(values[0],0)
    if a.inject_invalid_entry:
        snapshot('inject-before')
        snap=json.loads((out/'warm-code/inject-before'/inject_mode/'snapshot.json').read_text())
        matches=[r for r in snap['records'] if int(r['pc'],0)==a.inject_invalid_entry]
        assert len(matches)==1,matches
        injected=matches[0]
        invalid_before=read_invalid_entries()
        tb=injected['tb']
        cmd=['gdb','-nx','-q','-batch',str(binary),'-ex','attach '+str(servers[inject_mode].proc.pid),'-ex',f'set ((TranslationBlock *){tb})->cflags = ((TranslationBlock *){tb})->cflags | 0x40000','-ex',f'call hamt_poison_tb_entry((TranslationBlock *){tb})','-ex',f'x/wx {injected["host"]}','-ex','detach']
        capture=subprocess.check_output(cmd,text=True,stderr=subprocess.STDOUT)
        (out/'injection-gdb.log').write_text(capture)
        assert re.search(r':\s+0x00000001',capture),capture
        metadata['injected_entry']={**injected,'invalid_entries_before':invalid_before,'method':'Mark the destination invalid and install its real sentinel; retain the published CALL payload to model a stale RAS target. No GDB feature activation.'}
        (out/'metadata.json').write_text(json.dumps(metadata,indent=2))
    if a.flush_after_warmup:
        metadata['flush_before']={mode:cold_state(mode) for mode in connections}
        for mode in connections:
            function='tb_flush(cpus.tqh_first)' if a.flush_after_warmup=='full' else 'tb_flush_mtcc(cpus.tqh_first,1)'
            cmd=['gdb','-nx','-q','-batch',str(mode_binaries[mode]),'-ex','attach '+str(servers[mode].proc.pid),'-ex','call '+function,'-ex','detach']
            capture=subprocess.check_output(cmd,text=True,stderr=subprocess.STDOUT)
            (out/mode/'flush-request-gdb.log').write_text(capture)
        (out/'metadata.json').write_text(json.dumps(metadata,indent=2))
    if a.warm_code_snapshots:snapshot('before')
    expected_checksums={}
    if a.pmu:
        for q in connections.values():q.command('stop')
        for case in cases:
            for n in lengths:expected_checksums[case,n]=servers['native'].run(case,n)['checksum']
    rows=[]
    with (out/'raw.jsonl').open('w') as f:
        for repeat in range(a.rounds):
            for ci,case in enumerate(cases):
                for n in (lengths if repeat%2==0 else list(reversed(lengths))):
                    order=list(servers);shift=(repeat+ci)%len(order);order=order[shift:]+order[:shift];checks=set()
                    for mode in order:
                        row=measure(mode,case,n,guarded=a.pmu);row.update(mode=mode,repeat=repeat,order=order,measured_at_ns=time.monotonic_ns())
                        rows.append(row);checks.add(row['checksum']);f.write(json.dumps(row)+'\n');f.flush()
                        print(repeat,case,n,mode,round(row['host_ns']/1e9,3),flush=True)
                    assert len(checks)==1,(case,repeat,n)
    if a.inject_invalid_entry:
        invalid_after=read_invalid_entries()
        assert invalid_after>invalid_before,(invalid_before,invalid_after)
        (out/'entry-exception.json').write_text(json.dumps(dict(before=invalid_before,after=invalid_after,exceptions=invalid_after-invalid_before),indent=2))
        snapshot('inject-after')
    if a.warm_code_snapshots:
        snapshot('after')
        identities={}
        for mode in list(connections):
            if mode not in servers:continue
            left=json.loads((out/'warm-code/before'/mode/'snapshot.json').read_text());right=json.loads((out/'warm-code/after'/mode/'snapshot.json').read_text())
            assert left['records']==right['records'],('warm code changed during length comparison',mode)
            identities[mode]=len(left['records'])
        (out/'warm-code/identity.json').write_text(json.dumps(identities,indent=2))
    if a.flush_after_warmup:
        after={mode:cold_state(mode) for mode in connections}
        key='full_flushes' if a.flush_after_warmup=='full' else 'user_flushes'
        for mode,states in after.items():assert states[0][key]>metadata['flush_before'][mode][0][key],(mode,metadata['flush_before'],after)
        (out/'flush-after.json').write_text(json.dumps(after,indent=2))
        for mode in connections:
            records=[json.loads(line) for line in (out/mode/'context-state.jsonl').read_text().splitlines()]
            assert all(record['prefixes']==records[0]['prefixes'] for record in records),('static prefix changed',mode)

    summary=[]
    for case in cases:
        for mode in servers:
            slopes=[];guest_slopes=[];intercepts=[];pmu_slopes={key:[] for key in (('cycles','instructions','branch_misses') if a.branch_misses else ('cycles','instructions'))}
            for repeat in range(a.rounds):
                selected={r['iterations']:r for r in rows if r['case']==case and r['mode']==mode and r['repeat']==repeat}
                lo,hi=sorted(lengths);a0,b0=selected[lo],selected[hi]
                calls=2 if case.endswith('_two') else 1
                slope=(b0['host_ns']-a0['host_ns'])/((hi-lo)*calls)
                if a.pmu:
                    for key in pmu_slopes:pmu_slopes[key].append((b0['pmu'][key]['value']-a0['pmu'][key]['value'])/((hi-lo)*calls))
                slopes.append(slope);guest_slopes.append((b0['ns']-a0['ns'])/((hi-lo)*calls));intercepts.append(a0['host_ns']-slope*lo*calls)
            summary.append(dict(case=case,mode=mode,host_ns_per_call=statistics.median(slopes),guest_ns_per_call=statistics.median(guest_slopes),host_fixed_ns=statistics.median(intercepts),host_slopes=slopes,guest_slopes=guest_slopes,host_intercepts=intercepts,pmu_slopes=pmu_slopes if a.pmu else None,pmu_medians={key:statistics.median(values) for key,values in pmu_slopes.items()} if a.pmu else None))
    (out/'summary.json').write_text(json.dumps(summary,indent=2))
    print(json.dumps(summary,indent=2),flush=True)
finally:
    for q in connections.values():
        try:q.command('cont');q.command('quit')
        except (OSError,RuntimeError):pass
    for mode,s in servers.items():s.close(graceful=mode in connections)
    for q in connections.values():q.close()
