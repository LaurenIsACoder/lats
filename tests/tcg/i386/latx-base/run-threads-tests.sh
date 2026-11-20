#!/bin/sh

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

set -eu

# Focused tests for LATX user thread creation and fault recovery.
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mode=${1:-}
output=${2:-"$source_dir/build"}
tests='user-thread-create user-thread-fault-exit'

case "$mode" in
build)
    mkdir -p -- "$output"
    for name in $tests; do
        "${CC:-gcc}" -O2 -static -pthread -mno-avx \
            -Wall -Wextra -Werror "$source_dir/$name.c" -o "$output/$name"
    done
    ;;
run)
    for name in $tests; do
        "$output/$name"
    done
    ;;
*)
    echo "usage: $0 build OUTPUT_DIR | run BINARY_DIR" >&2
    exit 2
    ;;
esac
