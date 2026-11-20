#!/bin/bash

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

set -e

# Release builds use the shared dependency preflight in build.sh.
exec ./latxbuild/build.sh -c rel "$@"
