/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LATX_PTW_ADDRESS_H
#define LATX_PTW_ADDRESS_H

/* Include after ir2.h and latx-options.h. */

/* base and index are scratch; addr may reuse either one. */
static inline void latxs_ptw_address(IR2_OPND *addr, IR2_OPND *base,
                                    IR2_OPND *index, IR2_OPND *va,
                                    unsigned shift)
{
    if (hamt_enabled() && option_table_addr) {
        la_out2ii(LISA_BSTRPICK_D, index, va, shift + 8, shift);
        la_out2ii(LISA_BSTRPICK_D, base, base, 51, 12);
        la_out2i(LISA_SLLI_D, base, base, 12);
        la_out3i(LISA_ALSL_D, addr, index, base, 2);
        return;
    }
    la_out2ii(LISA_BSTRPICK_D, base, base, 51, 12);
    la_out2i(LISA_SLLI_D, base, base, 12);
    la_out2ii(LISA_BSTRPICK_D, index, va, shift + 8, shift);
    la_out2i(LISA_SLLI_D, index, index, 3);
    la_out3(LISA_ADD_D, addr, base, index);
}

static inline void latxs_ptw_pair_address(IR2_OPND *addr0, IR2_OPND *addr1,
                                         IR2_OPND *base, IR2_OPND *even,
                                         IR2_OPND *odd, IR2_OPND *va)
{
    if (hamt_enabled() && option_table_addr) {
        /* Each even/odd pair occupies 16 bytes within one page table. */
        la_out2ii(LISA_BSTRPICK_D, odd, va, 20, 13);
        la_out2ii(LISA_BSTRPICK_D, base, base, 51, 12);
        la_out2i(LISA_SLLI_D, base, base, 12);
        la_out3i(LISA_ALSL_D, addr0, odd, base, 3);
        la_out2i(LISA_ADDI_D, addr1, addr0, 8);
        return;
    }
    la_out2ii(LISA_BSTRPICK_D, odd, va, 20, 12);
    la_out2i(LISA_ORI, odd, odd, 1);
    la_out2i(LISA_ADDI_D, even, odd, -1);
    la_out2i(LISA_SLLI_D, even, even, 3);
    la_out2i(LISA_SLLI_D, odd, odd, 3);
    la_out2ii(LISA_BSTRPICK_D, base, base, 51, 12);
    la_out2i(LISA_SLLI_D, base, base, 12);
    la_out3(LISA_ADD_D, addr0, even, base);
    la_out3(LISA_ADD_D, addr1, odd, base);
}

#endif
