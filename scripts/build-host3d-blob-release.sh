#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage: scripts/build-host3d-blob-release.sh RENDERER_PREFIX DEPENDENCY_PREFIX [BUILD_DIR]

RENDERER_PREFIX must contain the matching virglrenderer with map_fixed support.
DEPENDENCY_PREFIX contains any non-system renderer dependencies such as
spirv-cross-c-shared. BUILD_DIR defaults to build-host3d-blob-release.
EOF
    exit 2
}

[[ $# -ge 2 && $# -le 3 ]] || usage

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
renderer_prefix=$(cd -- "$1" && pwd)
dependency_prefix=$(cd -- "$2" && pwd)
build_dir=${3:-$source_dir/build-host3d-blob-release}
if [[ $build_dir != /* ]]; then
    build_dir=$PWD/$build_dir
fi
mkdir -p "$build_dir"

pkg_paths=(
    "$renderer_prefix/lib/pkgconfig"
    "$renderer_prefix/lib64/pkgconfig"
    "$dependency_prefix/lib/pkgconfig"
    "$dependency_prefix/lib64/pkgconfig"
)
pkg_config_path=$(IFS=:; echo "${pkg_paths[*]}")
export PKG_CONFIG_PATH="$pkg_config_path${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

(
    cd "$build_dir"
    "$source_dir/configure" \
        --target-list=x86_64-softmmu \
        --enable-spice --enable-sdl --enable-slirp --enable-virglrenderer \
        --enable-latx-ndebug --disable-werror --disable-docs \
        --disable-plugins --disable-tools \
        --enable-latx-opt --enable-latx-opt-nofp \
        --enable-latx-opt-eflags --enable-latx-opt-tu \
        --enable-latx-opt-bql --enable-latx-opt-mmio-lockless \
        --enable-latx-hack-mem --enable-atomic \
        --extra-ldflags="-Wl,-rpath-link,$dependency_prefix/lib" \
        -Dbuildtype=release -Db_ndebug=true
    ninja lats-x86_64
    meson configure | grep -E \
        '^  (buildtype|optimization|debug|b_ndebug)[[:space:]]'
)
