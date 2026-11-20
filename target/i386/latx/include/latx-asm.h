/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_ASM_H_
#define _LATX_ASM_H_

#define _IFC_REG(n)             \
   ".ifc \\f, $r" #n "\n\t"     \
   "\\var = " #n "\n\t"         \
   ".endif \n\t"

__asm__(".macro parse_f var f\n\t"
   "\\var = -1\n\t"
   _IFC_REG(0)  _IFC_REG(1)  _IFC_REG(2)  _IFC_REG(3)
   _IFC_REG(4)  _IFC_REG(5)  _IFC_REG(6)  _IFC_REG(7)
   _IFC_REG(8)  _IFC_REG(9)  _IFC_REG(10) _IFC_REG(11)
   _IFC_REG(12) _IFC_REG(13) _IFC_REG(14) _IFC_REG(15)
   _IFC_REG(16) _IFC_REG(17) _IFC_REG(18) _IFC_REG(19)
   _IFC_REG(20) _IFC_REG(21) _IFC_REG(22) _IFC_REG(23)
   _IFC_REG(24) _IFC_REG(25) _IFC_REG(26) _IFC_REG(27)
   _IFC_REG(28) _IFC_REG(29) _IFC_REG(30) _IFC_REG(31)
   ".iflt \\var \n\t"
   ".error \"unable to parse register name\\r\"\n\t"
   ".endif\n\t"
   ".endm");
#define P(_val, _reg) "parse_f "#_val","#_reg" \n\t"

#define MOVGR_2_SCR0(_rj) P(rj, _rj) ".word 0x00000800 | (rj << 5) | 0\n\t"
#define MOVGR_2_SCR1(_rj) P(rj, _rj) ".word 0x00000800 | (rj << 5) | 1\n\t"
#define MOVGR_2_SCR2(_rj) P(rj, _rj) ".word 0x00000800 | (rj << 5) | 2\n\t"
#define MOVGR_2_SCR3(_rj) P(rj, _rj) ".word 0x00000800 | (rj << 5) | 3\n\t"
#define MOVSCR0_2_GR(_rd) P(rd, _rd) ".word 0x00000C00 | (rd) | (0 << 5) \n\t"
#define MOVSCR1_2_GR(_rd) P(rd, _rd) ".word 0x00000C00 | (rd) | (1 << 5) \n\t"
#define MOVSCR2_2_GR(_rd) P(rd, _rd) ".word 0x00000C00 | (rd) | (2 << 5) \n\t"
#define MOVSCR3_2_GR(_rd) P(rd, _rd) ".word 0x00000C00 | (rd) | (3 << 5) \n\t"

#define MOVGR_2_FCSR0(_rj) P(rj, _rj) ".word 0x0114C000 | (rj << 5) | 0\n\t"
#define MOVGR_2_FCSR1(_rj) P(rj, _rj) ".word 0x0114C000 | (rj << 5) | 1\n\t"
#define MOVGR_2_FCSR2(_rj) P(rj, _rj) ".word 0x0114C000 | (rj << 5) | 2\n\t"
#define MOVGR_2_FCSR3(_rj) P(rj, _rj) ".word 0x0114C000 | (rj << 5) | 3\n\t"
#define MOVFCSR0_2_GR(_rd) P(rd, _rd) ".word 0x0114C800 | (rd) | (0 << 5)\n\t"
#define MOVFCSR1_2_GR(_rd) P(rd, _rd) ".word 0x0114C800 | (rd) | (1 << 5)\n\t"
#define MOVFCSR2_2_GR(_rd) P(rd, _rd) ".word 0x0114C800 | (rd) | (2 << 5)\n\t"
#define MOVFCSR3_2_GR(_rd) P(rd, _rd) ".word 0x0114C800 | (rd) | (3 << 5)\n\t"

#define X86MFFLAG(_rd) P(rd, _rd) ".word 0x005C0000 | (63<<10) | rd \n\t"
#define X86MTFLAG(_rd) P(rd, _rd) ".word 0x005C0020 | (63<<10) | rd \n\t"
#define __X86MFFLAG(rd) ".word 0x005C0000 | (63<<10) | rd \n\t"
#define __X86MTFLAG(rd) ".word 0x005C0020 | (63<<10) | rd \n\t"

#define X86SETTM ".word 0x00008008\n\t"
#define X86CLRTM ".word 0x00008028\n\t"

#define X86MTTOP0 ".word (0x00007000 | (0 << 5))\n\r"
#define X86MTTOP1 ".word (0x00007000 | (1 << 5))\n\r"
#define X86MTTOP2 ".word (0x00007000 | (2 << 5))\n\r"
#define X86MTTOP3 ".word (0x00007000 | (3 << 5))\n\r"
#define X86MTTOP4 ".word (0x00007000 | (4 << 5))\n\r"
#define X86MTTOP5 ".word (0x00007000 | (5 << 5))\n\r"
#define X86MTTOP6 ".word (0x00007000 | (6 << 5))\n\r"
#define X86MTTOP7 ".word (0x00007000 | (7 << 5))\n\r"
#define X86MFTOP(_rd) P(rd, _rd) ".word 0x00007400 | rd\n\r"
#define __X86MFTOP(rd) ".word 0x00007400 | rd\n\r"

#endif
