/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LATX_NATIVE_ATOMIC_H
#define LATX_NATIVE_ATOMIC_H

#include <stdbool.h>
#include <stdint.h>

/* Address and value are written by LLVM before its terminal helper call. */
typedef struct LatxNativeAtomicRequest {
    uint64_t address;
    uint64_t value;
    uint32_t descriptor;
    uint64_t completed;
} LatxNativeAtomicRequest;

struct CPUArchState;
void latx_helper_aot_atomic_request(struct CPUArchState *env, unsigned descriptor);
void latx_helper_aot_atomic(struct CPUArchState *env, unsigned descriptor);
bool latx_aot_atomic_complete(struct CPUArchState *env);

#endif
