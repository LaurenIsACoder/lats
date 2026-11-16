/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LATS_AOT_RECOVER_H
#define LATS_AOT_RECOVER_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define NATIVE_RECOVER_MAGIC UINT64_C(0x5245564f4345524c)
#define NATIVE_RECOVER_FOOTER UINT64_C(0x444e45524345524c)
#define NATIVE_XR_OFFSET 1024
#define NATIVE_FCSR_OFFSET 2048
#define NATIVE_FLAGS_OFFSET 2056
#define NATIVE_RECOVER_MAX_OSR 16

typedef struct __attribute__((packed)) NativeRecoverHeader {
    uint64_t magic;
    uint32_t bytes;
    uint32_t code_bytes;
    uint16_t points;
    uint16_t state_bytes; /* zero: direct GPR locations, no State frame */
    uint32_t frame_bytes;
    uint32_t saved_xr_mask;
    union {
        uint32_t reserved;
        uint32_t osr_count;
    };
    union {
        uint16_t saved_xr_offsets[32];
        int32_t osr_guest_offsets[NATIVE_RECOVER_MAX_OSR];
    };
} NativeRecoverHeader;

typedef struct __attribute__((packed)) NativeRecoverPoint {
    uint32_t native_offset;
    int32_t guest_offset;
    uint16_t locations;
    uint8_t flags_mask; /* CF/ZF/SF/OF/PF/AF: bits 0/1/2/3/4/5. */
    uint8_t memory_bytes;
    uint32_t instruction;
} NativeRecoverPoint;

typedef struct __attribute__((packed)) NativeRecoverLocation {
    uint8_t field; /* GPR0..15, XMM16..31, result/SF/OF/CF32..35,
                    * State36, effective address37, store value38,
                    * dynamic flag definition mask39, PF40, AF41,
                    * parity source low byte42. */
    uint8_t kind;  /* GPR0, XR1, constant2, fault-SP stack3. */
    uint8_t reg;
    uint8_t bytes;
    int32_t offset;
    uint64_t value;
} NativeRecoverLocation;

typedef struct __attribute__((packed)) NativeRecoverFooter {
    uint64_t magic;
    uint32_t offset;
    uint32_t bytes;
} NativeRecoverFooter;

/* Stack storage follows the proved MIR form, not the logical i1/i32 width. */
static inline unsigned native_recover_stack_storage(const NativeRecoverLocation *loc)
{
    return loc->bytes == 16 ? 16 : 8;
}

static inline bool native_recover_stack_location_valid(const NativeRecoverLocation *loc)
{
    unsigned storage = native_recover_stack_storage(loc);
    return loc->kind == 3 && loc->reg == 3 && loc->offset >= 0 &&
        (loc->bytes == 1 || loc->bytes == 2 || loc->bytes == 4 ||
         loc->bytes == 8 || loc->bytes == 16) &&
        (!(loc->field >= 16 && loc->field < 32) || loc->bytes == 16) &&
        (!(loc->field < 16 || (loc->field >= 32 && loc->field <= 37)) || loc->bytes <= 8) &&
        loc->value && loc->value <= INT32_MAX && !(loc->value & 15) &&
        !((uint32_t)loc->offset % storage) &&
        (uint32_t)loc->offset <= loc->value &&
        storage <= loc->value - (uint32_t)loc->offset;
}

/* The wrapper uses the exact 0x900 DMW domain; C uses its low address alias.
 * Validate the entire range before forming either pointer. */
static inline bool native_recover_private_range(uint64_t base, uint64_t bytes,
        uintptr_t *normal)
{
    const uint64_t domain = UINT64_C(0x9000000000000000);
    const uint64_t limit = UINT64_C(0x0010000000000000);
    if ((base >> 52) != 0x900 || (base & 15) || !bytes) {
        return false;
    }
    uint64_t low = base - domain;
    if (bytes > limit - low || low > UINTPTR_MAX || bytes - 1 > UINTPTR_MAX - low) {
        return false;
    }
    *normal = (uintptr_t)low;
    return true;
}

static inline bool native_recover_stack_address(const NativeRecoverLocation *loc,
        uint64_t fault_sp, uintptr_t *normal)
{
    uintptr_t base;
    if (!native_recover_stack_location_valid(loc) ||
        !native_recover_private_range(fault_sp, loc->value, &base)) {
        return false;
    }
    *normal = base + (uint32_t)loc->offset;
    return true;
}

/* Called only after the complete table has passed native_recover_parse. */
static inline bool native_recover_read_location(const NativeRecoverLocation *loc,
        const uint64_t *frame, uint8_t out[16], uint64_t *scalar)
{
    uint64_t value = 0;
    memset(out, 0, 16);
    switch (loc->kind) {
    case 0:
        value = frame[loc->reg];
        memcpy(out, &value, loc->bytes);
        break;
    case 1:
        memcpy(out, (const uint8_t *)frame + NATIVE_XR_OFFSET + loc->reg * 32, loc->bytes);
        memcpy(&value, out, loc->bytes < 8 ? loc->bytes : 8);
        break;
    case 2:
        value = loc->value;
        memcpy(out, &value, loc->bytes);
        break;
    case 3: {
        uintptr_t address;
        if (!native_recover_stack_address(loc, frame[3], &address)) {
            return false;
        }
        memcpy(out, (const void *)address, loc->bytes);
        memcpy(&value, out, loc->bytes < 8 ? loc->bytes : 8);
        break;
    }
    default:
        return false;
    }
    *scalar = value;
    return true;
}

/* Merge only fields present in this point, including a dynamic mask. */
static inline bool native_recover_flags(uint64_t baseline, uint64_t mask,
        uint64_t fields, const uint64_t values[43], uint64_t *result)
{
    static const unsigned required[] = {35, 32, 33, 34, 40, 41};
    static const unsigned bits[] = {0, 6, 7, 11, 2, 4};
    if (mask & ~UINT64_C(63)) {
        return false;
    }
    uint64_t flags = baseline;
    for (unsigned f = 0; f < 6; ++f) {
        if (!(mask & (UINT64_C(1) << f))) {
            continue;
        }
        bool parity_source = f == 4 && (fields & (UINT64_C(1) << 42));
        if (!parity_source && !(fields & (UINT64_C(1) << required[f]))) {
            return false;
        }
        bool value = parity_source ? !__builtin_parity((unsigned)values[42] & 255) :
                     f == 1 ? !values[32] : values[required[f]] != 0;
        uint64_t bit = UINT64_C(1) << bits[f];
        flags = (flags & ~bit) | (value ? bit : 0);
    }
    *result = flags;
    return true;
}

static inline const NativeRecoverHeader *native_recover_parse(
        const void *entry, uint32_t bytes, bool *recognized)
{
    *recognized = false;
    if (bytes < sizeof(NativeRecoverFooter)) {
        return NULL;
    }
    NativeRecoverFooter footer;
    memcpy(&footer, (const uint8_t *)entry + bytes - sizeof(footer), sizeof(footer));
    if (footer.magic != NATIVE_RECOVER_FOOTER) {
        return NULL;
    }
    *recognized = true;
    if (footer.offset > bytes - sizeof(footer) ||
        footer.bytes != bytes - sizeof(footer) - footer.offset ||
        footer.bytes < sizeof(NativeRecoverHeader)) {
        return NULL;
    }
    const NativeRecoverHeader *h = (const NativeRecoverHeader *)((const uint8_t *)entry + footer.offset);
    bool direct = h->state_bytes == 0;
    if (h->magic != NATIVE_RECOVER_MAGIC || h->bytes != footer.bytes ||
        (!direct && h->reserved) || h->code_bytes < 4 || h->code_bytes > footer.offset ||
        (!direct && h->state_bytes != 152 && h->state_bytes != 672) ||
        h->frame_bytes < h->state_bytes ||
        (direct && (h->saved_xr_mask || h->osr_count > NATIVE_RECOVER_MAX_OSR ||
                    (h->osr_count && h->points) ||
                    h->frame_bytes > INT32_MAX || (h->frame_bytes & 15)))) {
        return NULL;
    }
    if (direct) {
        bool any_osr = false;
        for (unsigned k = 0; k < NATIVE_RECOVER_MAX_OSR; ++k) {
            int32_t off = h->osr_guest_offsets[k];
            if (k >= h->osr_count) {
                if (off) {
                    return NULL;
                }
                continue;
            }
            if (!off) {
                continue;
            }
            any_osr = true;
            for (unsigned j = 0; j < k; ++j) {
                if (h->osr_guest_offsets[j] == off) {
                    return NULL;
                }
            }
        }
        if (h->osr_count && !any_osr) {
            return NULL;
        }
    } else {
        for (unsigned i = 0; i < 32; ++i) {
            if ((h->saved_xr_mask & (1u << i)) &&
                (h->saved_xr_offsets[i] > h->frame_bytes ||
                 h->frame_bytes - h->saved_xr_offsets[i] < 32)) {
                return NULL;
            }
        }
    }
    const uint8_t *pos = (const uint8_t *)(h + 1);
    const uint8_t *end = (const uint8_t *)h + h->bytes;
    uint32_t previous = 0;
    uint64_t core_frame_bytes = 0;
    for (unsigned i = 0; i < h->points; ++i) {
        if ((size_t)(end - pos) < sizeof(NativeRecoverPoint)) {
            return NULL;
        }
        const NativeRecoverPoint *point = (const NativeRecoverPoint *)pos;
        if ((point->native_offset & 3) || point->native_offset >= h->code_bytes ||
            h->code_bytes - point->native_offset < 4 ||
            (i && point->native_offset <= previous) ||
            (point->flags_mask & ~63) || !point->locations ||
            (point->memory_bytes != 1 && point->memory_bytes != 2 &&
             point->memory_bytes != 4 && point->memory_bytes != 8 &&
             point->memory_bytes != 16)) {
            return NULL;
        }
        previous = point->native_offset;
        pos += sizeof(*point);
        if (point->locations > (size_t)(end - pos) / sizeof(NativeRecoverLocation)) {
            return NULL;
        }
        uint64_t fields = 0;
        for (unsigned j = 0; j < point->locations; ++j) {
            const NativeRecoverLocation *loc = (const NativeRecoverLocation *)pos;
            if (loc->field > 42 || (fields & (UINT64_C(1) << loc->field)) ||
                loc->kind > 3 || (loc->kind != 3 && loc->offset) ||
                (loc->bytes != 1 && loc->bytes != 2 && loc->bytes != 4 &&
                 loc->bytes != 8 && loc->bytes != 16) ||
                (loc->kind < 2 && loc->reg >= 32) ||
                (loc->kind == 0 && loc->bytes > 8) ||
                (loc->kind == 2 && loc->bytes > 8) ||
                (loc->field == 36 && ((loc->kind != 0 && loc->kind != 3) || loc->bytes != 8)) ||
                (loc->field == 39 && (loc->kind == 1 || loc->bytes != 8)) ||
                (loc->field >= 40 && (loc->kind == 1 || loc->bytes != 1)) ||
                (direct && ((loc->field < 16 &&
                             (loc->bytes != 8 || loc->kind == 1)) ||
                            (loc->field >= 16 && loc->field < 32 &&
                             (loc->bytes != 16 || (loc->kind != 1 && loc->kind != 3))) ||
                            (loc->field == 37 &&
                             (loc->bytes != 8 || loc->kind == 1))))) {
                return NULL;
            }
            if (loc->kind == 3) {
                if (direct && loc->value != h->frame_bytes) {
                    return NULL;
                }
                if (!native_recover_stack_location_valid(loc) ||
                    (core_frame_bytes && core_frame_bytes != loc->value)) {
                    return NULL;
                }
                core_frame_bytes = loc->value;
            }
            fields |= UINT64_C(1) << loc->field;
            pos += sizeof(*loc);
        }
        if (!(fields & (UINT64_C(1) << 37)) ||
            (!direct && !(fields & (UINT64_C(1) << 36)))) {
            return NULL;
        }
        /* Without State there is no fallback for an omitted GPR. A zero flag
         * mask uses the LBT flags captured at the fault; the direct producer
         * must prove that its preceding host instructions preserved them. */
        if (direct && ((fields & UINT64_C(0xffff)) != UINT64_C(0xffff) ||
                       (fields & (UINT64_C(1) << 36)) ||
                       (fields & (UINT64_C(1) << 39)) ||
                       (point->flags_mask != 0 && point->flags_mask != 63))) {
            return NULL;
        }
        if ((fields & (UINT64_C(1) << 39)) &&
            (fields & (UINT64_C(15) << 32)) != (UINT64_C(15) << 32)) {
            return NULL;
        }
        if ((fields & (UINT64_C(1) << 40)) && (fields & (UINT64_C(1) << 42))) {
            return NULL;
        }
        for (unsigned f = 0; f < 6; ++f) {
            static const unsigned required[] = {35, 32, 33, 34, 40, 41};
            uint64_t alternatives = UINT64_C(1) << required[f];
            if (f == 4) {
                alternatives |= UINT64_C(1) << 42;
            }
            if ((point->flags_mask & (1u << f)) && !(fields & alternatives)) {
                return NULL;
            }
        }
    }
    return pos == end ? h : NULL;
}
#endif
