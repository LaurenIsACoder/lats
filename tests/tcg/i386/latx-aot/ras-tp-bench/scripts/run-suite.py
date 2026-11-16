#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Run native, LAT and an isolated LATS Linux guest, serially on one core."""
import argparse, hashlib, json, math, os, pathlib, queue, statistics, subprocess, threading, time, socket, ctypes, struct, fcntl
import importlib.util
guard_spec = importlib.util.spec_from_file_location('host_guard', pathlib.Path(__file__).with_name('host-load-guard.py'))
host_guard = importlib.util.module_from_spec(guard_spec)
guard_spec.loader.exec_module(host_guard)
HostCoreGuard = host_guard.HostCoreGuard
CASES=['loop_dep','loop_ilp','branch_pred','branch_random','call_direct','call_indirect1','call_indirect4','chase_l1','chase_large']
class Counter:
    def __init__(self,tid,config):
        attr=bytearray(128)
        struct.pack_into('IIQQQQQ',attr,0,0,128,config,0,0,3,1)
        libc=ctypes.CDLL(None,use_errno=True)
        self.fd=libc.syscall(241,ctypes.c_char_p(bytes(attr)),tid,-1,-1,8)
        if self.fd<0:raise OSError(ctypes.get_errno(),'perf_event_open')
        event_id=bytearray(8)
        try:fcntl.ioctl(self.fd,0x80082407,event_id,True)
        except BaseException:os.close(self.fd);raise
        self.spec=dict(type=0,config=config,attr_flags=1,read_format=3,
                       tid=tid,cpu=-1,group_fd=-1,syscall_flags=8,
                       event_id=struct.unpack('Q',event_id)[0])
    def start(self):fcntl.ioctl(self.fd,0x2403,0);fcntl.ioctl(self.fd,0x2400,0)
    def finish(self):
        fcntl.ioctl(self.fd,0x2401,0)
        value,enabled,running=struct.unpack('QQQ',os.read(self.fd,24))
        return {'value':value,'enabled_ns':enabled,'running_ns':running}
    def close(self):os.close(self.fd)
class DeltaCounter(Counter):
    """perf RESET clears counts, but enabled/running times remain cumulative."""
    def start(self):
        _,self.enabled_base,self.running_base=struct.unpack('QQQ',os.read(self.fd,24))
        fcntl.ioctl(self.fd,0x2403,0)
        fcntl.ioctl(self.fd,0x2400,0)
    def finish(self):
        result=super().finish()
        result['enabled_ns']-=self.enabled_base
        result['running_ns']-=self.running_base
        if result['enabled_ns']<=0 or not 0<=result['running_ns']<=result['enabled_ns']:
            raise RuntimeError('invalid per-row counter times: '+str(result))
        if result['running_ns']!=result['enabled_ns']:
            raise RuntimeError('multiplexed counter rejected: '+str(result))
        return result
class Server:
    def __init__(self,cmd,log,env=None,handshake=False):
        self.handshake=handshake
        self.counters=[];self.cmd=cmd;self.log=open(log,'w');self.events=queue.Queue()
        self.proc=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=env,start_new_session=True)
        self.thread=threading.Thread(target=self.read,daemon=True);self.thread.start()
        try:
            _,ready=self.until('BENCH_READY',180)
            expected='BENCH_READY GO/STOP/NEXT' if handshake else 'BENCH_READY'
            if ready!=expected:raise RuntimeError('protocol mismatch: '+ready)
        except BaseException:self.close();raise
    def read(self):
        for b in self.proc.stdout:
            stamp=time.monotonic_ns();line=b.decode(errors='replace').strip();self.log.write(line+'\n');self.log.flush();self.events.put((stamp,line))
        self.events.put((time.monotonic_ns(),'PROCESS_EXIT'))
    def until(self,prefix,timeout):
        deadline=time.monotonic()+timeout
        while True:
            stamp,line=self.events.get(timeout=max(0.001,deadline-time.monotonic()))
            if "Parameter '" in line and 'expects' in line:raise RuntimeError('rejected translator option: '+line)
            if 'Kernel panic - not syncing' in line:raise RuntimeError('guest boot failed: '+line)
            if line.startswith(prefix):return stamp,line
            if line=='PROCESS_EXIT':raise RuntimeError('process exited: '+str(self.cmd))
            if time.monotonic()>deadline:raise TimeoutError(prefix)
    def run(self,name,n,timeout=120):
        self.proc.stdin.write(('%s %d\n'%(name,n)).encode());self.proc.stdin.flush()
        begin,line=self.until('BENCH_BEGIN ',timeout)
        if line!='BENCH_BEGIN %s %d'%(name,n):raise RuntimeError('begin mismatch: '+line)
        enable_begin=time.monotonic_ns()
        for c in self.counters:c.start()
        counter_begin=time.monotonic_ns()
        if self.handshake:
            self.proc.stdin.write(b'GO\n');self.proc.stdin.flush()
            stop,line=self.until('BENCH_STOP '+name,timeout)
            if line!='BENCH_STOP '+name:raise RuntimeError('stop mismatch: '+line)
            disable_begin=time.monotonic_ns()
            counts=[c.finish() for c in self.counters]
            disable_end=time.monotonic_ns()
            counter_elapsed=disable_end-enable_begin
            self.proc.stdin.write(b'NEXT\n');self.proc.stdin.flush()
            end,line=self.until('BENCH_END ',timeout)
            result=json.loads(line[len('BENCH_END '):])
            result.update(host_ns=stop-counter_begin,protocol='GO/STOP/NEXT',begin_to_end_ns=end-begin)
        else:
            end,line=self.until('BENCH_END ',timeout)
            counts=[c.finish() for c in self.counters]
            counter_elapsed=time.monotonic_ns()-counter_begin
            result=json.loads(line[len('BENCH_END '):]);result['host_ns']=end-begin
        if counts:
            result['pmu']={'cycles':counts[0],'instructions':counts[1],'window_ns':counter_elapsed,'protocol':result.get('protocol','asynchronous BEGIN/END')}
            if self.handshake:
                result['pmu'].update(window_ratio=counter_elapsed/result['host_ns'],enable_overhead_ns=counter_begin-enable_begin,stop_to_disable_ns=disable_begin-stop,disable_overhead_ns=disable_end-disable_begin)
            else:result['pmu']['coverage']=counter_elapsed/result['host_ns']
        if len(counts)>2:result['pmu']['branch_misses']=counts[2]
        if result['case']!=name or result['iterations']!=n:raise RuntimeError('response mismatch')
        if result['ns']<=0 or result['host_ns']<=0:raise RuntimeError('invalid timing')
        return result
    def close(self,graceful=False):
        if self.proc.poll() is None:
            if graceful:
                self.proc.stdin.close()
                try:self.proc.wait(timeout=180)
                except subprocess.TimeoutExpired:pass
        if self.proc.poll() is None:
            import signal
            try:os.killpg(self.proc.pid,signal.SIGTERM)
            except ProcessLookupError:pass
            try:self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:os.killpg(self.proc.pid,signal.SIGKILL);self.proc.wait()
        # An AOT save child retains this pipe after its VM parent exits.
        self.thread.join(timeout=1800)
        if self.thread.is_alive():raise TimeoutError('AOT save worker did not close its output')
        self.log.close()
        for c in self.counters:c.close()
def sha(path):return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('--root',default=str(pathlib.Path(__file__).resolve().parent.parent));p.add_argument('--out',default='results/main');p.add_argument('--rounds',type=int,default=7);p.add_argument('--target-seconds',type=float,default=.3);p.add_argument('--cpu',type=int,default=6);p.add_argument('--io-cpu',type=int,default=2);p.add_argument('--kernel',required=True);p.add_argument('--bios',required=True);p.add_argument('--latx-options',default='select,hamt=1,aotsys=0');p.add_argument('--cases',default=','.join(CASES));p.add_argument('--iterations-file');p.add_argument('--lat-env',action='append',default=[]);p.add_argument('--lats-user',action='store_true');p.add_argument('--pmu',action='store_true');p.add_argument('--aot-trace-inspect',action='store_true');p.add_argument('--native-binary');p.add_argument('--lat-binary');p.add_argument('--lats-binary');p.add_argument('--lats-extra',action='append',default=[]);p.add_argument('--lat-extra',action='append',default=[]);a=p.parse_args()
    root=pathlib.Path(a.root);out=root/a.out;out.mkdir(parents=True,exist_ok=True);cases=a.cases.split(',')
    env=dict(os.environ,LATX_AOT='0',LATX_KZT='0',XDG_CACHE_HOME=str(root/'build/cache'))
    for setting in a.lat_env:key,value=setting.split('=',1);env[key]=value
    native=a.native_binary or str(root/'build/bench-la');x86=str(root/'bench-x86');lat=a.lat_binary or str(root/'build/lat/latx-x86_64');lats=a.lats_binary or str(root/'build/lats/lats-x86_64')
    commands={
      'native':['taskset','-c',str(a.cpu),native,'--server'],
      'lat':['taskset','-c',str(a.cpu),lat,x86,'--server'],
      'lats':['taskset','-c',str(a.cpu),lats,'-M','pc','-m','256','-smp','1','-cpu','qemu64','-kernel',a.kernel,'-initrd',str(root/'bench.cpio'),'-append','console=ttyS0 rdinit=/init quiet clocksource=acpi_pm','-nographic','-no-reboot','-monitor','none','-qmp','unix:'+str(out/'qmp.sock')+',server=on,wait=off','-net','none','-L',a.bios,'-accel','tcg,mode=lat,thread=single','-latx',a.latx_options]
    }
    commands['lat'][4:4]=a.lat_extra
    commands['lats']+=a.lats_extra
    if os.environ.get('BENCH_AOT_TRAIN_GDB')=='1':
        training_gdb=os.environ.get('BENCH_AOT_TRAIN_GDB_FILE',str(root/'scripts/aot-train-lats.gdb'))
        commands['lats'][3:3]=['gdb','-q','-batch','-x',training_gdb,'--args']
    if os.environ.get('BENCH_AOT_GDB')=='lat':
        for mode in ['lat']:
            commands[mode][3:3]=['gdb','-q','-batch','-x',str(root/'scripts'/('aot-'+mode+'.gdb')),'--args']
    if a.lats_user:commands['lats']=['taskset','-c',str(a.cpu),str(root/'build/lats-user/lats-user-x86_64'),'-latx',a.latx_options,x86,'--server']
    metadata={'commands':commands,'env':{k:env[k] for k in env if k.startswith(('LATX_','LATS_AOT_','BENCH_AOT_')) or k=='XDG_CACHE_HOME'},'args':vars(a),'binary_sha256':{k:sha(v) for k,v in [('native',native),('x86',x86),('lat',lat),('lats',str(root/'build/lats-user/lats-user-x86_64') if a.lats_user else lats)]},'started':time.strftime('%Y-%m-%dT%H:%M:%S%z')}
    metadata['controller_affinity']=sorted(os.sched_getaffinity(0))
    metadata['start_loadavg']=os.getloadavg()
    (out/'metadata.json').write_text(json.dumps(metadata,indent=2))
    servers={};qmp=None
    try:
        for mode in commands:
            print('starting '+mode,flush=True);servers[mode]=Server(commands[mode],out/(mode+'.log'),env)
        if not a.lats_user:
            qmp_socket=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);qmp_socket.connect(str(out/'qmp.sock'))
            qmp=qmp_socket.makefile('rwb',buffering=0);json.loads(qmp.readline())
            def qmp_command(command):
                qmp.write((json.dumps({'execute':command})+'\n').encode())
                while True:
                    response=json.loads(qmp.readline())
                    if 'error' in response:raise RuntimeError(response)
                    if 'return' in response:return response['return']
            qmp_command('qmp_capabilities')
            cpus=qmp_command('query-cpus-fast');pinned=[int(cpu['thread-id']) for cpu in cpus]
            if len(pinned)!=1:raise RuntimeError('expected one vCPU: '+str(cpus))
            tasks=pathlib.Path('/proc/%d/task'%pinned[0])
            for t in tasks.iterdir():os.sched_setaffinity(int(t.name),{a.cpu if int(t.name) in pinned else a.io_cpu})
            metadata['vcpu_tids']=pinned
        def measure(mode,case,n):
            if qmp:qmp_command('cont' if mode=='lats' else 'stop')
            return servers[mode].run(case,n)
        if a.pmu:
            for mode,server in servers.items():
                tid=metadata['vcpu_tids'][0] if mode=='lats' and not a.lats_user else server.proc.pid
                server.counters=[Counter(tid,0),Counter(tid,1)]
        correctness=[]
        for case in cases:
            for n in [1,17,10000]:
                values={mode:measure(mode,case,n) for mode in servers}
                if len({v['checksum'] for v in values.values()})!=1:raise RuntimeError('checksum mismatch '+str(values))
                correctness.append({'case':case,'iterations':n,'values':values})
        (out/'correctness.json').write_text(json.dumps(correctness,indent=2))
        iterations=json.loads(pathlib.Path(a.iterations_file).read_text()) if a.iterations_file else {}
        for case in cases:
            if case in iterations:continue
            n=100000
            for trial in range(4):
                result=measure('native',case,n)
                if result['ns']>=a.target_seconds*1e9*.8:break
                n=max(n+1,min(int(n*a.target_seconds*1e9/max(result['ns'],1)),n*100))
            iterations[case]=n
        (out/'iterations.json').write_text(json.dumps(iterations,indent=2))
        (out/'metadata.json').write_text(json.dumps(metadata,indent=2))
        rows=[]
        with open(out/'raw.jsonl','w') as f:
            for repeat in range(a.rounds):
                for ci,case in enumerate(cases):
                    order=['native','lat','lats'];shift=(repeat+ci)%3;order=order[shift:]+order[:shift]
                    checks=set()
                    for mode in order:
                        row=measure(mode,case,iterations[case]);row.update(mode=mode,repeat=repeat);rows.append(row);checks.add(row['checksum']);f.write(json.dumps(row)+'\n');f.flush()
                        print('%d %s %s %.3fs guest/host=%.4f'%(repeat,case,mode,row['host_ns']/1e9,row['ns']/row['host_ns']),flush=True)
                    if len(checks)!=1:raise RuntimeError('measured checksum mismatch')
        summary=[]
        for case in cases:
            native_med=statistics.median([r['host_ns'] for r in rows if r['case']==case and r['mode']=='native'])
            for mode in commands:
                selected=[r for r in rows if r['case']==case and r['mode']==mode];times=[r['host_ns'] for r in selected];med=statistics.median(times)
                summary.append({'case':case,'mode':mode,'median_ns':med,'ns_per_op':med/iterations[case],'efficiency_pct':100*native_med/med,'cv_pct':100*statistics.stdev(times)/statistics.mean(times) if len(times)>1 else None,'min_ns':min(times),'max_ns':max(times),'guest_host_ratio':statistics.median([r['ns']/r['host_ns'] for r in selected])})
        if a.aot_trace_inspect:
            qmp_command('stop');time.sleep(.5)
            subprocess.run(['python3',str(root/'scripts/inspect-aot-traces.py'),lats,str(servers['lats'].proc.pid),str(out/'lats-exec.log'),str(out/'lats-trace-evidence.log')],check=True)
        (out/'summary.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2),flush=True)
    finally:
        if qmp:
            try:qmp_command('cont');qmp_command('quit')
            except (OSError,ValueError,RuntimeError):pass
        for mode,s in servers.items():s.close(graceful=mode=='lat' or (mode=='lats' and qmp is not None))
        if qmp:qmp.close();qmp_socket.close()
if __name__=='__main__':main()
