#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage: LATS_DISK_IMAGE=/path/to/windows.qcow2 [options] scripts/run-windows-vgpu.sh

Environment options:
  LATS_BIN               LATS executable (default: build/lats-x86_64)
  LATS_BLOB_MODE         off, standard, or external (default: off)
  LATS_BLOB_HOSTMEM_MB   host-visible BAR size (default: 256)
  LATS_RENDERER_PREFIX   matching virglrenderer installation prefix
  LATS_RENDERER_DEP_PREFIX optional prefix for non-system renderer dependencies
  LATS_MEMORY_MB         guest memory (default: 8192)
  LATS_VCPUS             guest vCPUs (default: 4)
  LATS_TCG_TB_MB         optional TCG code-cache size in MiB
  LATS_DISK_FORMAT       qcow2 or raw (default: qcow2)
  LATS_SNAPSHOT          0 or 1 (default: 0)
  LATS_DISPLAY           display backend (default: gtk,gl=on)
  LATS_VNC_ADDRESS       optional VNC address, for example :5
  LATS_QMP_SOCKET        optional QMP Unix socket
  LATS_QGA_SOCKET        optional QEMU guest-agent Unix socket
  LATS_TRACE_FILE        optional HOST3D BLOB trace output file
  LATS_LATX_OPTIONS      translator options
EOF
    exit 2
}

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
lats_bin=${LATS_BIN:-$repo_dir/build/lats-x86_64}
disk_image=${LATS_DISK_IMAGE:-}
blob_mode=${LATS_BLOB_MODE:-off}
hostmem_mb=${LATS_BLOB_HOSTMEM_MB:-256}
memory_mb=${LATS_MEMORY_MB:-8192}
vcpus=${LATS_VCPUS:-4}
tcg_tb_mb=${LATS_TCG_TB_MB:-}
disk_format=${LATS_DISK_FORMAT:-qcow2}
snapshot=${LATS_SNAPSHOT:-0}
display=${LATS_DISPLAY:-gtk,gl=on}
vnc_address=${LATS_VNC_ADDRESS:-}
qmp_socket=${LATS_QMP_SOCKET:-}
qga_socket=${LATS_QGA_SOCKET:-}
trace_file=${LATS_TRACE_FILE:-}
latx_options=${LATS_LATX_OPTIONS:-select,verbose=on,aotsys=0,hamt=1,hamtmode=1,hamtsubmode=0,hamtconfig=0x33e9}

[[ -n "$disk_image" ]] || usage
[[ -x "$lats_bin" ]] || { echo "LATS binary is not executable: $lats_bin" >&2; exit 1; }
[[ -r "$disk_image" ]] || { echo "Disk image is not readable: $disk_image" >&2; exit 1; }
[[ "$blob_mode" =~ ^(off|standard|external)$ ]] || usage
[[ "$disk_format" =~ ^(qcow2|raw)$ ]] || usage
[[ "$snapshot" =~ ^[01]$ ]] || usage
[[ "$hostmem_mb" =~ ^[0-9]+$ ]] || usage
[[ -z "$tcg_tb_mb" || "$tcg_tb_mb" =~ ^[0-9]+$ ]] || usage
(( hostmem_mb >= 64 && (hostmem_mb & (hostmem_mb - 1)) == 0 )) || {
    echo "LATS_BLOB_HOSTMEM_MB must be a power of two of at least 64" >&2
    exit 1
}

if [[ -n "${LATS_RENDERER_PREFIX:-}" ]]; then
    renderer_lib="$LATS_RENDERER_PREFIX/lib"
    renderer_arch_lib="$LATS_RENDERER_PREFIX/lib/loongarch64-linux-gnu"
    export LD_LIBRARY_PATH="$renderer_lib:$renderer_arch_lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
if [[ -n "${LATS_RENDERER_DEP_PREFIX:-}" ]]; then
    renderer_dep_lib="$LATS_RENDERER_DEP_PREFIX/lib"
    renderer_dep_arch_lib="$LATS_RENDERER_DEP_PREFIX/lib/loongarch64-linux-gnu"
    # Dependencies can contain an older libvirglrenderer.  Append them so the
    # explicitly selected renderer prefix above always wins library lookup.
    export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+$LD_LIBRARY_PATH:}$renderer_dep_lib:$renderer_dep_arch_lib"
fi

vgpu_device=virtio-vga-gl
case "$blob_mode" in
    off)
        ;;
    standard)
        vgpu_device+=",blob=on,hostmem=${hostmem_mb}M"
        ;;
    external)
        vgpu_device+=",blob=on,hostmem=${hostmem_mb}M,external-blob=on"
        ;;
esac

extra_args=()
[[ "$snapshot" == 1 ]] && extra_args+=(-snapshot)
[[ -n "$tcg_tb_mb" ]] && extra_args+=(-accel "tcg,tb-size=$tcg_tb_mb")
[[ -n "$vnc_address" ]] && extra_args+=(-vnc "$vnc_address")
if [[ -n "$qmp_socket" ]]; then
    [[ ! -e "$qmp_socket" ]] || {
        echo "QMP socket path already exists: $qmp_socket" >&2
        exit 1
    }
    extra_args+=(-qmp "unix:$qmp_socket,server=on,wait=off")
fi
if [[ -n "$qga_socket" ]]; then
    [[ ! -e "$qga_socket" ]] || {
        echo "QGA socket path already exists: $qga_socket" >&2
        exit 1
    }
    extra_args+=(-device virtio-serial-pci)
    extra_args+=(-chardev "socket,path=$qga_socket,server=on,wait=off,id=qga0")
    extra_args+=(-device virtserialport,chardev=qga0,name=org.qemu.guest_agent.0)
fi
if [[ -n "$trace_file" ]]; then
    extra_args+=(-trace "events=$repo_dir/scripts/host3d-blob-trace-events,file=$trace_file")
fi

echo "LATS binary:    $lats_bin"
echo "Disk image:     $disk_image ($disk_format)"
echo "Memory/vCPUs:   $memory_mb MiB / $vcpus"
echo "TCG TB size:    ${tcg_tb_mb:-default}"
echo "BLOB mode:      $blob_mode"
echo "vGPU device:    $vgpu_device"
echo "Renderer prefix:${LATS_RENDERER_PREFIX:-system default}"
echo "Snapshot writes:$snapshot"
echo "Display/VNC:    $display / ${vnc_address:-disabled}"

exec "$lats_bin" \
    -L "$repo_dir/pc-bios" \
    -m "$memory_mb" \
    -smp "$vcpus" \
    -boot menu=on \
    -drive "file=$disk_image,format=$disk_format,if=ide,index=0,media=disk" \
    -display "$display" \
    -device "$vgpu_device" \
    -usb -device usb-tablet \
    -netdev user,id=n0 -device e1000e,netdev=n0 \
    -name "guest=windows-vgpu-$blob_mode,process=latxsys-windows-vgpu-$blob_mode,debug-threads=on" \
    "${extra_args[@]}" \
    -latx "$latx_options"
