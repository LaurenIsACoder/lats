/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <stdio.h>
#include <string.h>
#include <capstone/capstone.h>

/*
 * Check architectural operand widths. A widened read can fault when the
 * scalar operand ends at a page boundary.
 */
int main(void)
{
    unsigned failures = 0, checked = 0;
    const cs_mode modes[] = { CS_MODE_32, CS_MODE_64 };
    const int syntaxes[] = { CS_OPT_SYNTAX_INTEL, CS_OPT_SYNTAX_ATT };
    /* Capstone diet only supports Intel syntax. */
    unsigned syntax_count = cs_support(CS_SUPPORT_DIET) ? 1 : ARRAY_SIZE(syntaxes);
    const struct {
        unsigned char code[8];
        unsigned len, width;
    } cases[] = {
        {{0x0f, 0x2f, 0x00}, 3, 4},
        {{0x0f, 0x2e, 0x00}, 3, 4},
        {{0x66, 0x0f, 0x2f, 0x00}, 4, 8},
        {{0x66, 0x0f, 0x2e, 0x00}, 4, 8},
        {{0xc5, 0xf8, 0x2f, 0x00}, 4, 4},
        {{0xc5, 0xf8, 0x2e, 0x00}, 4, 4},
        {{0xc5, 0xf9, 0x2f, 0x00}, 4, 8},
        {{0xc5, 0xf9, 0x2e, 0x00}, 4, 8},
        /* Packed conversions also use the 128-bit memory printer. */
        {{0x0f, 0x5b, 0x00}, 3, 16},
        {{0x66, 0x0f, 0x5a, 0x00}, 4, 16},
    };
    for (unsigned m = 0; m < ARRAY_SIZE(modes); m++) {
        csh h;
        if (cs_open(CS_ARCH_X86, modes[m], &h) != CS_ERR_OK) {
            fprintf(stderr, "Failed to open Capstone\n");
            return 2;
        }
        if (cs_option(h, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) {
            fprintf(stderr, "Failed to enable Capstone operand details\n");
            cs_close(&h);
            return 2;
        }
        for (unsigned s = 0; s < syntax_count; s++) {
            if (cs_option(h, CS_OPT_SYNTAX, syntaxes[s]) != CS_ERR_OK) {
                fprintf(stderr, "Failed to set Capstone syntax %d\n", syntaxes[s]);
                cs_close(&h);
                return 2;
            }
            for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
                for (unsigned reg = 0; reg < 2; reg++) {
                    union {
                        uint64_t align;
                        unsigned char bytes[2 * IR1_INST_SIZE];
                    } buf = {0};
                    unsigned char code[8];
                    cs_insn *i = NULL;
                    unsigned mem = 0;
                    memcpy(code, cases[k].code, sizeof(code));
                    if (reg) {
                        code[cases[k].len - 1] = 0xc1;
                    }
                    checked++;
                    if (cs_disasm(h, code, cases[k].len, 0, 1, &i, 1,
                                  buf.bytes) != 1) {
                        fprintf(stderr, "case=%u could not be decoded\n", k);
                        failures++;
                        continue;
                    }
                    if (i->detail->x86.op_count != 2) {
                        fprintf(stderr, "case=%u expected two operands\n", k);
                        failures++;
                        continue;
                    }
                    for (unsigned n = 0; n < i->detail->x86.op_count; n++) {
                        cs_x86_op *op = &i->detail->x86.operands[n];
                        if (op->type == X86_OP_REG && op->size != 16) {
                            fprintf(stderr, "Unexpected XMM width: %u\n",
                                    op->size);
                            failures++;
                        }
                        if (op->type != X86_OP_MEM) {
                            continue;
                        }
                        mem++;
                        if (op->size != cases[k].width) {
                            fprintf(stderr, "%u-bit %s case=%u: "
                                    "memory width=%u expected=%u\n",
                                    m ? 64 : 32, s ? "AT&T" : "Intel", k,
                                    op->size, cases[k].width);
                            failures++;
                        }
                    }
                    if (mem != !reg) {
                        fprintf(stderr, "case=%u unexpected memory operand count %u\n",
                                k, mem);
                        failures++;
                    }
                }
            }
        }
        cs_close(&h);
    }
    printf("scalar comparison decode: %u cases, %u failures\n", checked, failures);
    return failures != 0;
}
