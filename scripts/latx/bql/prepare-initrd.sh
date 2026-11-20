#!/bin/sh

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

# prepare-initrd.sh — build a prepared initrd for BQL benchmarking.
#
# Usage:
#   prepare-initrd.sh <base-rootfs.cpio> <bql-stress-binary> <out.cpio> \
#                     <duration_sec> <mmio_threads> <shootdown_threads>
#
# Runs on the 3A6000 host. Reuses the buildroot rootfs and injects:
#   /opt/bql/bql-stress   — guest stressor (statically linked x86_64)
#   /sbin/bql-init        — rdinit entry, runs the stressor then idles
#
# After the stressor finishes it prints "[BQL-STRESS] DONE"; the host
# driver then dumps `info sync-profile` via the QEMU monitor and quits.
set -e

BASE=$(readlink -f "$1")
BIN=$(readlink -f "$2")
OUT=$(readlink -f "$3")
DUR="${4:-30}"
NMMIO="${5:-2}"
NSD="${6:-2}"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/root"
cd "$TMP/root"
if [ "$(od -An -tx1 -N2 "$BASE" | tr -d ' ')" = "1f8b" ]; then
    gzip -dc "$BASE" | cpio -idm --quiet
    COMPRESS=1
else
    cpio -idm --quiet < "$BASE"
    COMPRESS=0
fi

mkdir -p opt/bql sbin
cp "$BIN" opt/bql/bql-stress
chmod 755 opt/bql/bql-stress

cat > sbin/bql-init <<EOF
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev 2>/dev/null
echo '[BQL-STRESS] UPTIME_START='\$(cut -d' ' -f1 /proc/uptime)
/opt/bql/bql-stress $DUR $NMMIO $NSD
echo '[BQL-STRESS] RC='\$?
echo '[BQL-STRESS] DONE'
echo '[BQL-STRESS] UPTIME_END='\$(cut -d' ' -f1 /proc/uptime)
sync
while true; do sleep 3600; done
EOF
chmod 755 sbin/bql-init

if [ "$COMPRESS" = 1 ]; then
    find . | cpio -o -H newc --quiet | gzip > "$OUT"
else
    find . | cpio -o -H newc --quiet > "$OUT"
fi
echo "prepared initrd: $OUT (duration=$DUR mmio=$NMMIO sd=$NSD)"
