#!/bin/bash

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

# Bind only inside a private mount namespace; other LAT/LATS sessions keep their cache.
set -euo pipefail
cache=$(realpath "${1:?absolute task cache directory}")
shift
mkdir -p "$cache/sys"
as_root=0
if [[ "${1:-}" == --as-root ]]; then
    as_root=1
    shift
fi
if (( EUID != 0 )); then
    if (( as_root )); then
        exec sudo -n --preserve-env=HOME unshare -m --propagation private bash "$0" "$cache" --as-root "$@"
    fi
    exec sudo -n --preserve-env=HOME unshare -m --propagation private bash "$0" "$cache" "$@"
fi
cache_target="${LATS_CACHE_HOME:-$HOME/.cache/latx}"
mkdir -p "$cache_target"
mount --bind "$cache" "$cache_target"
mkdir -p /tmp/latx-sys-aot
mount --bind "$cache/sys" /tmp/latx-sys-aot
if (( as_root )); then
    exec "$@"
fi
exec runuser -u "${SUDO_USER:-$(id -un)}" -- "$@"
