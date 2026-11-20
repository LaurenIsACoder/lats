#!/bin/bash

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only


set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QEMU="${LATS_X86_64:-$ROOT/build/lats-x86_64}"
SIZE=2097152

tmpdir="$(mktemp -d /tmp/lats-ivshmem-file.XXXXXX)"
memfile="$tmpdir/mem"
qmp="$tmpdir/qmp"
log="$tmpdir/qemu.log"
pid=""

cleanup()
{
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$tmpdir"
}
trap cleanup EXIT

[[ -x "$QEMU" ]] || { echo "missing QEMU: $QEMU" >&2; exit 1; }

"$QEMU" \
    -nodefaults \
    -machine pc,accel=tcg \
    -m 64M \
    -display none \
    -S \
    -object memory-backend-file,id=ivshmem-file,size=2M,share=on,mem-path="$memfile" \
    -device ivshmem-plain,id=ivshmem-test,memdev=ivshmem-file \
    -qmp unix:"$qmp",server=on,wait=off \
    >"$log" 2>&1 &
pid=$!

for _ in $(seq 1 100); do
    if [[ -S "$qmp" ]]; then
        break
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
        cat "$log" >&2
        exit 1
    fi
    sleep 0.05
done
[[ -S "$qmp" ]] || { echo "QMP socket did not become ready" >&2; exit 1; }

{
    printf '%s\n' '{"execute":"qmp_capabilities"}'
    printf '%s\n' '{"execute":"qom-get","arguments":{"path":"/objects/ivshmem-file","property":"size"}}'
    printf '%s\n' '{"execute":"quit"}'
    sleep 0.2
} | timeout 4 socat - UNIX-CONNECT:"$qmp" >"$tmpdir/qmp.out" 2>/dev/null || true

grep -Fq '"return": 2097152' "$tmpdir/qmp.out"
[[ "$(stat -c %s "$memfile")" -eq "$SIZE" ]]
wait "$pid" || true
pid=""

echo "file-backed ivshmem size: ok"
