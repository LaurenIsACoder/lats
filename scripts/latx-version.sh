#!/bin/sh

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only


set -eu

version=$(git describe --match 'v*' --always 2>/dev/null || echo "nogit")

cat <<EOF
#ifndef LATX_VERSION_H_
#define LATX_VERSION_H_
#define LATX_VERSION "$version"
#endif /* LATX_VERSION_H_ */
EOF
