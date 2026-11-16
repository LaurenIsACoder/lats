/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __LISA_ASSEMBLE_H_
#define __LISA_ASSEMBLE_H_

#include "la-ir2.h"
typedef enum {
    OPD_INVALID = 0,
    OPD_CA,
    OPD_CD,
    OPD_CJ,
    OPD_CODE,
    OPD_CONDF,
    OPD_CONDH,
    OPD_CONDL,
    OPD_CSR,
    OPD_FA,
    OPD_FCSRH,
    OPD_FCSRL,
    OPD_FD,
    OPD_FJ,
    OPD_FK,
    OPD_HINTL,
    OPD_HINTS,
    OPD_I13,
    OPD_IDXS,
    OPD_IDXM,
    OPD_IDXL,
    OPD_IDXLL,
    OPD_LEVEL,
    OPD_LSBD,
    OPD_LSBW,
    OPD_MODE,
    OPD_MSBD,
    OPD_MSBW,
    OPD_OFFS,
    OPD_OFFL,
    OPD_OFFLL,
    OPD_OPCACHE,
    OPD_OPX86,
    OPD_PTR,
    OPD_RD,
    OPD_RJ,
    OPD_RK,
    OPD_SA2,
    OPD_SA3,
    OPD_SD,
    OPD_SEQ,
    OPD_SI10,
    OPD_SI11,
    OPD_SI12,
    OPD_SI14,
    OPD_SI16,
    OPD_SI20,
    OPD_SI5,
    OPD_SI8,
    OPD_SI9,
    OPD_SJ,
    OPD_UI1,
    OPD_UI12,
    OPD_UI2,
    OPD_UI3,
    OPD_UI4,
    OPD_UI5H,
    OPD_UI5L,
    OPD_UI6,
    OPD_UI7,
    OPD_UI8,
    OPD_UI20,
    OPD_VA,
    OPD_VD,
    OPD_VJ,
    OPD_VK,
    OPD_XA,
    OPD_XD,
    OPD_XJ,
    OPD_XK,
    OPD_END,
} GM_OPERAND_TYPE;

typedef struct pair {
    int start;
    union {
        int end;
        int len;
    };
} pair;

typedef struct {
    GM_OPERAND_TYPE type;
    pair bit_range_0;
    pair bit_range_1; /* some branch offset is splited into 2 parts */
} GM_OPERAND_PLACE_RELATION;

typedef struct {
    IR2_OPCODE type;
    uint32_t opcode;
    GM_OPERAND_TYPE opnd[4];
} GM_LA_OPCODE_FORMAT;

typedef struct {
    GM_OPERAND_TYPE place;
    IR2_OPND_TYPE type;
} GM_LA_OPERAND_PLACE_TYPE;

GM_OPERAND_PLACE_RELATION bit_field_table[] = {
    {OPD_INVALID,   {-1,{-1}}, {-1,{-1}}},
    {OPD_CA,        {15,{17}}, {-1,{-1}}},
    {OPD_CD,        { 0, {2}}, {-1,{-1}}},
    {OPD_CJ,        { 5, {7}}, {-1,{-1}}},
    {OPD_CODE,      { 0,{14}}, {-1,{-1}}},
    {OPD_CONDF,     {15,{19}}, {-1,{-1}}},
    {OPD_CONDH,     {10,{13}}, {-1,{-1}}},
    {OPD_CONDL,     { 0,{ 3}}, {-1,{-1}}},
    {OPD_CSR,       {10,{23}}, {-1,{-1}}},
    {OPD_FA,        {15,{19}}, {-1,{-1}}},
    {OPD_FCSRH,     { 5,{ 9}}, {-1,{-1}}},
    {OPD_FCSRL,     { 0,{ 4}}, {-1,{-1}}},
    {OPD_FD,        { 0,{ 4}}, {-1,{-1}}},
    {OPD_FJ,        { 5,{ 9}}, {-1,{-1}}},
    {OPD_FK,        {10,{14}}, {-1,{-1}}},
    {OPD_HINTL,     { 0,{14}}, {-1,{-1}}},
    {OPD_HINTS,     { 0,{ 4}}, {-1,{-1}}},
    {OPD_I13,       { 5,{17}}, {-1,{-1}}},
    {OPD_IDXS,      {18,{18}}, {-1,{-1}}},
    {OPD_IDXM,      {18,{19}}, {-1,{-1}}},
    {OPD_IDXL,      {18,{20}}, {-1,{-1}}},
    {OPD_IDXLL,     {18,{21}}, {-1,{-1}}},
    {OPD_LEVEL,     {10,{17}}, {-1,{-1}}},
    {OPD_LSBD,      {10,{15}}, {-1,{-1}}},
    {OPD_LSBW,      {10,{14}}, {-1,{-1}}},
    {OPD_MODE,      { 5,{ 9}}, {-1,{-1}}},
    {OPD_MSBD,      {16,{21}}, {-1,{-1}}},
    {OPD_MSBW,      {16,{20}}, {-1,{-1}}},
    {OPD_OFFS,      {10,{25}}, {-1,{-1}}},
    {OPD_OFFL,      {10,{25}}, { 0,{ 4}}},
    {OPD_OFFLL,     {10,{25}}, { 0,{ 9}}},
    {OPD_OPCACHE,   { 0,{ 4}}, {-1,{-1}}},
    {OPD_OPX86,     { 5,{ 9}}, {-1,{-1}}},
    {OPD_PTR,       { 5,{ 7}}, {-1,{-1}}},
    {OPD_RD,        { 0,{ 4}}, {-1,{-1}}},
    {OPD_RJ,        { 5,{ 9}}, {-1,{-1}}},
    {OPD_RK,        {10,{14}}, {-1,{-1}}},
    {OPD_SA2,       {15,{16}}, {-1,{-1}}},
    {OPD_SA3,       {15,{17}}, {-1,{-1}}},
    {OPD_SD,        { 0,{ 1}}, {-1,{-1}}},
    {OPD_SEQ,       {10,{17}}, {-1,{-1}}},
    {OPD_SI10,      {10,{19}}, {-1,{-1}}},
    {OPD_SI11,      {10,{20}}, {-1,{-1}}},
    {OPD_SI12,      {10,{21}}, {-1,{-1}}},
    {OPD_SI14,      {10,{23}}, {-1,{-1}}},
    {OPD_SI16,      {10,{25}}, {-1,{-1}}},
    {OPD_SI20,      { 5,{24}}, {-1,{-1}}},
    {OPD_SI5,       {10,{14}}, {-1,{-1}}},
    {OPD_SI8,       {10,{17}}, {-1,{-1}}},
    {OPD_SI9,       {10,{18}}, {-1,{-1}}},
    {OPD_SJ,        { 5,{ 6}}, {-1,{-1}}},
    {OPD_UI1,       {10,{10}}, {-1,{-1}}},
    {OPD_UI12,      {10,{21}}, {-1,{-1}}},
    {OPD_UI2,       {10,{11}}, {-1,{-1}}},
    {OPD_UI3,       {10,{12}}, {-1,{-1}}},
    {OPD_UI4,       {10,{13}}, {-1,{-1}}},
    {OPD_UI5H,      {15,{19}}, {-1,{-1}}},
    {OPD_UI5L,      {10,{14}}, {-1,{-1}}},
    {OPD_UI6,       {10,{15}}, {-1,{-1}}},
    {OPD_UI7,       {10,{16}}, {-1,{-1}}},
    {OPD_UI8,       {10,{17}}, {-1,{-1}}},
    {OPD_UI20,      { 5,{24}}, {-1,{-1}}},
    {OPD_VA,        {15,{19}}, {-1,{-1}}},
    {OPD_VD,        { 0,{ 4}}, {-1,{-1}}},
    {OPD_VJ,        { 5,{ 9}}, {-1,{-1}}},
    {OPD_VK,        {10,{14}}, {-1,{-1}}},
    {OPD_XA,        {15,{19}}, {-1,{-1}}},
    {OPD_XD,        { 0,{ 4}}, {-1,{-1}}},
    {OPD_XJ,        { 5,{ 9}}, {-1,{-1}}},
    {OPD_XK,        {10,{14}}, {-1,{-1}}},
};

GM_OPERAND_PLACE_RELATION bit_field_table_new[] = {
    {OPD_INVALID, { 0,{ 0}}, {0,{ 0}} },     /**/
    {OPD_CA,      {15,{ 3}}, {0,{ 0}} },     /*15, 17*/
    {OPD_CD,      { 0,{ 3}}, {0,{ 0}} },     /*0, 2*/
    {OPD_CJ,      { 5,{ 3}}, {0,{ 0}} },     /*5, 7*/
    {OPD_CODE,    { 0,{15}}, {0,{ 0}} },     /*0, 14*/
    {OPD_CONDF,   {15,{ 5}}, {0,{ 0}} },     /*15, 19*/
    {OPD_CONDH,   {10,{ 4}}, {0,{ 0}} },     /*10, 13*/
    {OPD_CONDL,   { 0,{ 4}}, {0,{ 0}} },     /*0, 3*/
    {OPD_CSR,     {10,{14}}, {0,{ 0}} },     /*10, 23*/
    {OPD_FA,      {15,{ 5}}, {0,{ 0}} },     /*15, 19*/
    {OPD_FCSRH,   { 5,{ 5}}, {0,{ 0}} },     /*5, 9*/
    {OPD_FCSRL,   { 0,{ 5}}, {0,{ 0}} },     /*0, 4*/
    {OPD_FD,      { 0,{ 5}}, {0,{ 0}} },     /*0, 4*/
    {OPD_FJ,      { 5,{ 5}}, {0,{ 0}} },     /*5, 9*/
    {OPD_FK,      {10,{ 5}}, {0,{ 0}} },     /*10, 14*/
    {OPD_HINTL,   { 0,{15}}, {0,{ 0}} },     /*0, 14*/
    {OPD_HINTS,   { 0,{ 5}}, {0,{ 0}} },     /*0, 4*/
    {OPD_I13,     { 5,{13}}, {0,{ 0}} },     /*5, 17*/
    {OPD_IDXS,    {18,{ 1}}, {0,{ 0}} },     /*18, 18*/
    {OPD_IDXM,    {18,{ 2}}, {0,{ 0}} },     /*18, 19*/
    {OPD_IDXL,    {18,{ 3}}, {0,{ 0}} },     /*18, 20*/
    {OPD_IDXLL,   {18,{ 4}}, {0,{ 0}} },     /*18, 21*/
    {OPD_LEVEL,   {10,{ 8}}, {0,{ 0}} },     /*10, 17*/
    {OPD_LSBD,    {10,{ 6}}, {0,{ 0}} },     /*10, 15*/
    {OPD_LSBW,    {10,{ 5}}, {0,{ 0}} },     /*10, 14*/
    {OPD_MODE,    { 5,{ 5}}, {0,{ 0}} },     /*5, 9*/
    {OPD_MSBD,    {16,{ 6}}, {0,{ 0}} },     /*16, 21*/
    {OPD_MSBW,    {16,{ 5}}, {0,{ 0}} },     /*16, 20*/
    {OPD_OFFS,    {10,{16}}, {0,{ 0}} },     /*10, 25*/
    {OPD_OFFL,    {10,{16}}, {0,{ 5}} },     /*10, 25*/ /*0, 4*/
    {OPD_OFFLL,   {10,{16}}, {0,{10}} },     /*10, 25*/ /*0, 9*/
    {OPD_OPCACHE, { 0,{ 5}}, {0,{ 0}} },     /*0, 4*/
    {OPD_OPX86,   { 5,{ 5}}, {0,{ 0}} },     /*5, 9*/
    {OPD_PTR,     { 5,{ 3}}, {0,{ 0}} },     /*5, 7*/
    {OPD_RD,      { 0,{ 5}}, {0,{ 0}} },     /*0, 4*/
    {OPD_RJ,      { 5,{ 5}}, {0,{ 0}} },     /*5, 9*/
    {OPD_RK,      {10,{ 5}}, {0,{ 0}} },     /*10, 14*/
    {OPD_SA2,     {15,{ 2}}, {0,{ 0}} },     /*15, 16*/
    {OPD_SA3,     {15,{ 3}}, {0,{ 0}} },     /*15, 17*/
    {OPD_SD,      { 0,{ 2}}, {0,{ 0}} },     /*0, 1*/
    {OPD_SEQ,     {10,{ 8}}, {0,{ 0}} },     /*10, 17*/
    {OPD_SI10,    {10,{10}}, {0,{ 0}} },     /*10, 19*/
    {OPD_SI11,    {10,{11}}, {0,{ 0}} },     /*10, 20*/
    {OPD_SI12,    {10,{12}}, {0,{ 0}} },     /*10, 21*/
    {OPD_SI14,    {10,{14}}, {0,{ 0}} },     /*10, 23*/
    {OPD_SI16,    {10,{16}}, {0,{ 0}} },     /*10, 25*/
    {OPD_SI20,    { 5,{20}}, {0,{ 0}} },     /*5, 24*/
    {OPD_SI5,     {10,{ 5}}, {0,{ 0}} },     /*10, 14*/
    {OPD_SI8,     {10,{ 8}}, {0,{ 0}} },     /*10, 17*/
    {OPD_SI9,     {10,{ 9}}, {0,{ 0}} },     /*10, 18*/
    {OPD_SJ,      { 5,{ 2}}, {0,{ 0}} },     /*5, 6*/
    {OPD_UI1,     {10,{ 1}}, {0,{ 0}} },     /*10, 10*/
    {OPD_UI12,    {10,{12}}, {0,{ 0}} },     /*10, 21*/
    {OPD_UI2,     {10,{ 2}}, {0,{ 0}} },     /*10, 11*/
    {OPD_UI3,     {10,{ 3}}, {0,{ 0}} },     /*10, 12*/
    {OPD_UI4,     {10,{ 4}}, {0,{ 0}} },     /*10, 13*/
    {OPD_UI5H,    {15,{ 5}}, {0,{ 0}} },     /*15, 19*/
    {OPD_UI5L,    {10,{ 5}}, {0,{ 0}} },     /*10, 14*/
    {OPD_UI6,     {10,{ 6}}, {0,{ 0}} },     /*10, 15*/
    {OPD_UI7,     {10,{ 7}}, {0,{ 0}} },     /*10, 16*/
    {OPD_UI8,     {10,{ 8}}, {0,{ 0}} },     /*10, 17*/
    {OPD_UI20,    { 5,{20}}, {0,{ 0}} },     /*5, 24*/
    {OPD_VA,      {15,{ 5}}, {0,{ 0}} },     /*15, 19*/
    {OPD_VD,      { 0,{ 5}}, {0,{ 0}} },     /*0, 4*/
    {OPD_VJ,      { 5,{ 5}}, {0,{ 0}} },     /*5, 9*/
    {OPD_VK,      {10,{ 5}}, {0,{ 0}} },     /*10, 14*/
    {OPD_XA,      {15,{ 5}}, {0,{ 0}} },     /*15, 19*/
    {OPD_XD,      { 0,{ 5}}, {0,{ 0}} },     /*0, 4*/
    {OPD_XJ,      { 5,{ 5}}, {0,{ 0}} },     /*5, 9*/
    {OPD_XK,      {10,{ 5}}, {0,{ 0}} },     /*10, 14*/
};

/* there are some errors in vector instruction */
GM_LA_OPCODE_FORMAT lisa_format_table[] = {
#include "ir2-format.h"
};

GM_LA_OPERAND_PLACE_TYPE ir2_type_check_table[] = {
    { OPD_INVALID , IR2_OPND_INV  },
    { OPD_CA      , IR2_OPND_CC   },
    { OPD_CD      , IR2_OPND_CC   },
    { OPD_CJ      , IR2_OPND_CC   },
    { OPD_CODE    , IR2_OPND_CODE },
    { OPD_CONDF   , IR2_OPND_IMMH },
    { OPD_CONDH   , IR2_OPND_IMMH },
    { OPD_CONDL   , IR2_OPND_IMMH },
    { OPD_CSR     , IR2_OPND_CSR  },
    { OPD_FA      , IR2_OPND_FPR  },
    { OPD_FCSRH   , IR2_OPND_FCSR },
    { OPD_FCSRL   , IR2_OPND_FCSR },
    { OPD_FD      , IR2_OPND_FPR  },
    { OPD_FJ      , IR2_OPND_FPR  },
    { OPD_FK      , IR2_OPND_FPR  },
    { OPD_HINTL   , IR2_OPND_IMMH },
    { OPD_HINTS   , IR2_OPND_IMMH },
    { OPD_I13     , IR2_OPND_IMMH },
    { OPD_IDXS    , IR2_OPND_IMMH },
    { OPD_IDXM    , IR2_OPND_INV  },
    { OPD_IDXL    , IR2_OPND_INV  },
    { OPD_IDXLL   , IR2_OPND_INV  },
    { OPD_LEVEL   , IR2_OPND_INV  },
    { OPD_LSBD    , IR2_OPND_IMMH },
    { OPD_LSBW    , IR2_OPND_IMMH },
    { OPD_MODE    , IR2_OPND_IMMH },
    { OPD_MSBD    , IR2_OPND_IMMH },
    { OPD_MSBW    , IR2_OPND_IMMH },
    { OPD_OFFS    , IR2_OPND_IMMH },
    { OPD_OFFL    , IR2_OPND_IMMH },
    { OPD_OFFLL   , IR2_OPND_IMMH },
    { OPD_OPCACHE , IR2_OPND_INV  },
    { OPD_OPX86   , IR2_OPND_IMMH },
    { OPD_PTR     , IR2_OPND_IMMH },
    { OPD_RD      , IR2_OPND_GPR  },
    { OPD_RJ      , IR2_OPND_GPR  },
    { OPD_RK      , IR2_OPND_GPR  },
    { OPD_SA2     , IR2_OPND_IMMH },
    { OPD_SA3     , IR2_OPND_IMMH },
    { OPD_SD      , IR2_OPND_SCR  },
    { OPD_SEQ     , IR2_OPND_INV  },
    { OPD_SI10    , IR2_OPND_IMMH },
    { OPD_SI11    , IR2_OPND_IMMH },
    { OPD_SI12    , IR2_OPND_IMMH },
    { OPD_SI14    , IR2_OPND_IMMH },
    { OPD_SI16    , IR2_OPND_IMMH },
    { OPD_SI20    , IR2_OPND_IMMH },
    { OPD_SI5     , IR2_OPND_IMMH },
    { OPD_SI8     , IR2_OPND_IMMH },
    { OPD_SI9     , IR2_OPND_IMMH },
    { OPD_SJ      , IR2_OPND_SCR  },
    { OPD_UI1     , IR2_OPND_IMMH },
    { OPD_UI12    , IR2_OPND_IMMH },
    { OPD_UI2     , IR2_OPND_IMMH },
    { OPD_UI3     , IR2_OPND_IMMH },
    { OPD_UI4     , IR2_OPND_IMMH },
    { OPD_UI5H    , IR2_OPND_IMMH },
    { OPD_UI5L    , IR2_OPND_IMMH },
    { OPD_UI6     , IR2_OPND_IMMH },
    { OPD_UI7     , IR2_OPND_IMMH },
    { OPD_UI8     , IR2_OPND_IMMH },
    { OPD_UI20    , IR2_OPND_IMMH },
    { OPD_VA      , IR2_OPND_FPR  },
    { OPD_VD      , IR2_OPND_FPR  },
    { OPD_VJ      , IR2_OPND_FPR  },
    { OPD_VK      , IR2_OPND_FPR  },
    { OPD_XA      , IR2_OPND_FPR  },
    { OPD_XD      , IR2_OPND_FPR  },
    { OPD_XJ      , IR2_OPND_FPR  },
    { OPD_XK      , IR2_OPND_FPR  },
};

#endif

