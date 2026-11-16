#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Save paused warm TB metadata and code; this does not count execution."""
import argparse,hashlib,json,os,pathlib,re,struct,subprocess
p=argparse.ArgumentParser()
p.add_argument('binary',type=pathlib.Path);p.add_argument('pid',type=int);p.add_argument('out',type=pathlib.Path)
p.add_argument('--system',action='store_true');p.add_argument('--start',type=lambda x:int(x,0),required=True);p.add_argument('--end',type=lambda x:int(x,0),required=True)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
fields=['pc','tc.ptr','ir2_num' if a.system else 'tc.size','cflags']
if a.system:fields+=['next_pc[0]','next_pc[1]','jmp_dest[0]','jmp_dest[1]','jmp_insn_offset[0]','jmp_insn_offset[1]','tc.offset_in_tu','bool_flags','inst_ptr_offset']
args=['gdb','-nx','-q','-batch',str(a.binary)]
for f in fields:args+=['-ex','p (unsigned long)&((TranslationBlock *)0)->'+f,'-ex','p sizeof(((TranslationBlock *)0)->'+f+')']
text=subprocess.check_output(args,text=True,stderr=subprocess.STDOUT)
values=[int(x,0) for x in re.findall(r'\$\d+ = (0x[0-9a-f]+|\d+)',text)];assert len(values)==2*len(fields),text
layout={f:(values[2*i],values[2*i+1]) for i,f in enumerate(fields)}
# LATS separates kernel/user code into contexts 0/1 when MTCC is enabled.
ctx='tcg_ctxs[mtcc_enabled ? 1 : 0]' if a.system else 'tcg_ctx'
args=['gdb','-nx','-q','-batch',str(a.binary),'-ex','attach '+str(a.pid),'-ex','p/x '+ctx+'->code_gen_buffer','-ex','p/x '+ctx+'->code_gen_ptr','-ex','detach']
text=subprocess.check_output(args,text=True,stderr=subprocess.STDOUT);(a.out/'context.log').write_text(text)
values=[int(x,0) for x in re.findall(r'\$\d+ = (0x[0-9a-f]+|\d+)',text)];assert len(values)==2,text
begin,end=values;assert 0<end-begin<128*1024*1024,(hex(begin),hex(end))
fd=os.open('/proc/%d/mem'%a.pid,os.O_RDONLY);records=[]
def field(tb,f):
 off,size=layout[f];raw=os.pread(fd,size,tb+off)
 if len(raw)!=size:raise ValueError('unreadable metadata')
 return int.from_bytes(raw,'little')
def capture(tb,kind='tb',**identity):
 values={f:field(tb,f) for f in fields}
 if values['cflags']&0x40000:return
 host=values['tc.ptr'];size=values['ir2_num']*4 if a.system else values['tc.size']
 if not begin<=host<end or not 0<size<=end-host:return
 if (tb,host) in seen:return
 seen.add((tb,host))
 code=os.pread(fd,size,host);assert len(code)==size
 name=f'block-{values["pc"]:x}-{host:x}.bin';(a.out/name).write_bytes(code)
 records.append(dict(pc=hex(values['pc']),host=hex(host),tb=hex(tb),size=size,phase=host&63,metadata=values,code_file=name,sha256=hashlib.sha256(code).hexdigest(),kind=kind,**identity))
seen=set()
try:
 raw=os.pread(fd,end-begin,begin);assert len(raw)==end-begin
 for i in range(0,len(raw)-8,4):
  tag=struct.unpack_from('<Q',raw,i)[0]
  if tag>>48!=0xbeef:continue
  low=tag&((1<<48)-1)
  try:
   if low>1000:
    if a.start<=field(low,'pc')<a.end:capture(low)
   elif a.system and low and (begin+i)%32==0:
    # The TU header stores N, followed by N (size16, TBpointer48) entries.
    count=low;table_end=i+8*(count+1)
    if table_end>len(raw):continue
    base=begin+table_end;offset=0;members=[]
    for j in range(count):
     entry=struct.unpack_from('<Q',raw,i+8*(j+1))[0]
     tb=entry&((1<<48)-1);size=entry>>48
     if (size%4 or size!=field(tb,'ir2_num')*4 or
         field(tb,'tc.ptr')!=base+offset or
         field(tb,'tc.offset_in_tu')!=offset or
         not field(tb,'bool_flags')&0x40 or
         field(tb,'cflags')&0x40000):break
     members.append((tb,size));offset+=size
    else:
     if base+offset>end:continue
     if not any(a.start<=field(tb,'pc')<a.end for tb,size in members):continue
     # Keep the full TU, including the last member's out-of-line repairs.
     for j,(tb,size) in enumerate(members):
      capture(tb,'tu_member',tu_header=hex(begin+i),tu_base=hex(base),tu_member_count=count,tu_member_index=j,tbmini_size=size)
  except (OSError,ValueError,OverflowError):continue
finally:os.close(fd)
assert records,'no matching warm blocks'
(a.out/'snapshot.json').write_text(json.dumps(dict(binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),pid=a.pid,buffer=hex(begin),end=hex(end),layout=layout,records=sorted(records,key=lambda r:(r['pc'],r['host'])),limitation='paused metadata and complete bytes; not execution counts; single context JIT/TU only; requires non-region-tree TBMini headers'),indent=2)+'\n')
print('Captured',len(records),'warm blocks')
