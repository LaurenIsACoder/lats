#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

import pathlib,sys
source,elf,target=map(pathlib.Path,sys.argv[1:])
data=source.read_bytes();replacement=elf.read_bytes();output=bytearray();offset=0;replaced=False
while offset+110<=len(data):
    header=data[offset:offset+110];assert header[:6]==b'070701'
    fields=[int(header[6+8*i:14+8*i],16) for i in range(13)]
    name=data[offset+110:offset+110+fields[11]]
    start=(offset+110+fields[11]+3)&~3
    payload=data[start:start+fields[6]]
    offset=(start+fields[6]+3)&~3
    if name.rstrip(b'\0').lstrip(b'./')==b'init':
        payload=replacement;fields[6]=len(payload);replaced=True
    output+=b'070701'+b''.join(f'{v:08x}'.encode() for v in fields)+name
    output+=b'\0'*((-len(output))%4)
    output+=payload;output+=b'\0'*((-len(output))%4)
    if name.rstrip(b'\0')==b'TRAILER!!!':break
assert replaced
output+=b'\0'*((-len(output))%512)
target.write_bytes(output)
