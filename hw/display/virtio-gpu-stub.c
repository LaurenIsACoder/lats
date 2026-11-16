/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/iov.h"
#include "sysemu/cpus.h"
#include "ui/console.h"
#include "ui/rect.h"
#include "trace.h"
#include "sysemu/dma.h"
#include "sysemu/sysemu.h"
#include "hw/virtio/virtio.h"
#include "migration/qemu-file-types.h"
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-bswap.h"
#include "hw/virtio/virtio-gpu-pixman.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
