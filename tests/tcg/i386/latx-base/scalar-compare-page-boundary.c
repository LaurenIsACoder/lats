/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef SCALAR_COMPARE_DLL
typedef __SIZE_TYPE__ size_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __UINT64_TYPE__ uint64_t;
#define UINT64_C(n) n##ULL
#else
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#endif

/* Exercise both compare/SETcc and compare/Jcc with an exact-width operand at
 * the end of a readable page. A following inaccessible page detects overreads. */
#define DEFINE_COMPARE(name, type, load, cmp)                                  \
static __attribute__((noinline)) int name(const void *p, int branch)            \
{                                                                              \
    type lhs = 1.0;                                                            \
    unsigned char result;                                                      \
    if (branch) {                                                              \
        __asm__ volatile(load " %1, %%xmm0\n\t" cmp " (%2), %%xmm0\n\t"          \
                         "ja 1f\n\tmovb $0, %0\n\tjmp 2f\n"                    \
                         "1: movb $1, %0\n2:"                                 \
                         : "=q" (result) : "m" (lhs), "r" (p)                 \
                         : "xmm0", "cc", "memory");                           \
    } else {                                                                   \
        __asm__ volatile(load " %1, %%xmm0\n\t" cmp " (%2), %%xmm0\n\t"          \
                         "seta %0"                                             \
                         : "=q" (result) : "m" (lhs), "r" (p)                 \
                         : "xmm0", "cc", "memory");                           \
    }                                                                          \
    return result;                                                             \
}
DEFINE_COMPARE(compare_ss, float, "movss", "comiss")
DEFINE_COMPARE(compare_uss, float, "movss", "ucomiss")
DEFINE_COMPARE(compare_sd, double, "movsd", "comisd")
DEFINE_COMPARE(compare_usd, double, "movsd", "ucomisd")

int scalar_compare_test(unsigned char *p, size_t page)
{
    unsigned checked = 0;
    const uint32_t single[] = {0xbf800000, 0, 0x80000000, 0x3f800000,
                               0x40000000, 0x7fc00000};
    const uint64_t dual[] = {UINT64_C(0xbff0000000000000), 0,
        UINT64_C(0x8000000000000000), UINT64_C(0x3ff0000000000000),
        UINT64_C(0x4000000000000000), UINT64_C(0x7ff8000000000000)};
    const int expected[] = {1, 1, 1, 0, 0, 0};
    int (*const compare[])(const void *, int) = {
        compare_ss, compare_uss, compare_sd, compare_usd
    };
    for (unsigned op = 0; op < 4; op++) {
        size_t width = op < 2 ? 4 : 8;
        size_t offsets[] = {0, 1, page - width, page - 1, 2 * page - width};
        for (unsigned pos = 0; pos < 5; pos++) {
            for (unsigned value = 0; value < 6; value++) {
                const unsigned char *data = op < 2 ?
                    (const unsigned char *)&single[value] :
                    (const unsigned char *)&dual[value];
                for (size_t byte = 0; byte < width; byte++) {
                    p[offsets[pos] + byte] = data[byte];
                }
                for (int branch = 0; branch < 2; branch++) {
                    if (compare[op](p + offsets[pos], branch) != expected[value]) {
                        return -1;
                    }
                    checked++;
                }
            }
        }
    }
    return checked;
}

#ifndef SCALAR_COMPARE_DLL
int main(void)
{
    size_t page;
    unsigned char *p;
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    page = si.dwPageSize;
    p = VirtualAlloc(NULL, page * 3, MEM_RESERVE, PAGE_NOACCESS);
    if (!p || VirtualAlloc(p, page * 2, MEM_COMMIT, PAGE_READWRITE) != p) return 2;
#else
    page = sysconf(_SC_PAGESIZE);
    p = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED || mprotect(p, page * 2, PROT_READ | PROT_WRITE)) return 2;
#endif
    int checked = scalar_compare_test(p, page);
    if (checked != 240) {
        fprintf(stderr, "FAIL scalar comparison boundaries\n");
        return 1;
    }
    printf("PASS scalar comparison boundaries: %d cases\n", checked);
    return 0;
}
#endif
