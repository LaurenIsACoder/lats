//===-- llvm/MC/MCInst.h - MCInst class -------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the declaration of the MCInst and MCOperand classes, which
// is the basic representation used to represent low-level machine code
// instructions.
//
//===----------------------------------------------------------------------===//

/* Capstone Disassembly Engine */
/* By Nguyen Anh Quynh <aquynh@gmail.com>, 2013-2015 */

#ifndef CS_MCINST_H
#define CS_MCINST_H

#include "include/capstone/capstone.h"

typedef struct MCInst MCInst;
typedef struct cs_struct cs_struct;
typedef struct MCOperand MCOperand;

/// MCOperand - Instances of this class represent operands of the MCInst class.
/// This is a simple discriminated union.
struct MCOperand {
	enum {
		kInvalid = 0,                 ///< Uninitialized.
		kRegister,                ///< Register operand.
		kImmediate,               ///< Immediate operand.
		kFPImmediate,             ///< Floating-point immediate operand.
	} MachineOperandType;
	unsigned char Kind;

	union {
		unsigned RegVal;
		int64_t ImmVal;
		double FPImmVal;
	};
};

/// MCInst - Instances of this class represent a single low-level machine
/// instruction.
struct MCInst {
	unsigned OpcodePub;
	uint8_t size;	// number of operands
	bool has_imm;	// indicate this instruction has an X86_OP_IMM operand - used for ATT syntax
	uint8_t op1_size; // size of 1st operand - for X86 Intel syntax
	unsigned Opcode;
	MCOperand Operands[48];
	cs_insn *flat_insn;	// insn to be exposed to public
	uint64_t address;	// address of this insn
	cs_struct *csh;	// save the main csh
	uint8_t x86opsize;	// opsize for [mem] operand

	// (Optional) instruction prefix, which can be up to 4 bytes.
	// A prefix byte gets value 0 when irrelevant.
	// This is copied from cs_x86 struct
	uint8_t x86_prefix[4];
	uint8_t imm_size;	// immediate size for X86_OP_IMM operand
	bool writeback;	// writeback for ARM
	// operand access index for list of registers sharing the same access right (for ARM)
	uint8_t ac_idx;
	uint8_t popcode_adjust;   // Pseudo X86 instruction adjust
	char assembly[8];	// for special instruction, so that we dont need printer
	unsigned char evm_data[32];	// for EVM PUSH operand
};

#define MCINST_CACHE (ARRAY_SIZE(mcInst->Operands) - 1)

inline __attribute__ ((always_inline))
void MCInst_Init(MCInst *inst)
{
	unsigned int i;

	for (i = 0; i < 48; i++) {
		inst->Operands[i].Kind = kInvalid;
	}

	inst->Opcode = 0;
	inst->OpcodePub = 0;
	inst->size = 0;
	inst->has_imm = false;
	inst->op1_size = 0;
	inst->writeback = false;
	inst->ac_idx = 0;
	inst->popcode_adjust = 0;
	inst->assembly[0] = '\0';
}

inline __attribute__ ((always_inline))
void MCInst_clear(MCInst *inst)
{
	inst->size = 0;
}

// do not free @Op
inline __attribute__ ((always_inline))
void MCInst_insert0(MCInst *inst, int index, MCOperand *Op)
{
	int i;

	for(i = inst->size; i > index; i--)
		//memcpy(&(inst->Operands[i]), &(inst->Operands[i-1]), sizeof(MCOperand));
		inst->Operands[i] = inst->Operands[i-1];

	inst->Operands[index] = *Op;
	inst->size++;
}

inline __attribute__ ((always_inline))
void MCInst_setOpcode(MCInst *inst, unsigned Op)
{
	inst->Opcode = Op;
}

inline __attribute__ ((always_inline))
void MCInst_setOpcodePub(MCInst *inst, unsigned Op)
{
	inst->OpcodePub = Op;
}

inline __attribute__ ((always_inline))
unsigned MCInst_getOpcode(const MCInst *inst)
{
	return inst->Opcode;
}

inline __attribute__ ((always_inline))
unsigned MCInst_getOpcodePub(const MCInst *inst)
{
	return inst->OpcodePub;
}

inline __attribute__ ((always_inline))
MCOperand *MCInst_getOperand(MCInst *inst, unsigned i)
{
	return &inst->Operands[i];
}

inline __attribute__ ((always_inline))
unsigned MCInst_getNumOperands(const MCInst *inst)
{
	return inst->size;
}

// This addOperand2 function doesnt free Op
inline __attribute__ ((always_inline))
void MCInst_addOperand2(MCInst *inst, MCOperand *Op)
{
	inst->Operands[inst->size] = *Op;

	inst->size++;
}

inline __attribute__ ((always_inline))
bool MCOperand_isValid(const MCOperand *op)
{
	return op->Kind != kInvalid;
}

inline __attribute__ ((always_inline))
bool MCOperand_isReg(const MCOperand *op)
{
	return op->Kind == kRegister;
}

inline __attribute__ ((always_inline))
bool MCOperand_isImm(const MCOperand *op)
{
	return op->Kind == kImmediate;
}

inline __attribute__ ((always_inline))
bool MCOperand_isFPImm(const MCOperand *op)
{
	return op->Kind == kFPImmediate;
}

/// getReg - Returns the register number.
inline __attribute__ ((always_inline))
unsigned MCOperand_getReg(const MCOperand *op)
{
	return op->RegVal;
}

/// setReg - Set the register number.
inline __attribute__ ((always_inline))
void MCOperand_setReg(MCOperand *op, unsigned Reg)
{
	op->RegVal = Reg;
}

inline __attribute__ ((always_inline))
int64_t MCOperand_getImm(MCOperand *op)
{
	return op->ImmVal;
}

inline __attribute__ ((always_inline))
void MCOperand_setImm(MCOperand *op, int64_t Val)
{
	op->ImmVal = Val;
}

inline __attribute__ ((always_inline))
double MCOperand_getFPImm(const MCOperand *op)
{
	return op->FPImmVal;
}

inline __attribute__ ((always_inline))
void MCOperand_setFPImm(MCOperand *op, double Val)
{
	op->FPImmVal = Val;
}

inline __attribute__ ((always_inline))
MCOperand *MCOperand_CreateReg1(MCInst *mcInst, unsigned Reg)
{
	MCOperand *op = &(mcInst->Operands[MCINST_CACHE]);

	op->Kind = kRegister;
	op->RegVal = Reg;

	return op;
}

inline __attribute__ ((always_inline))
void MCOperand_CreateReg0(MCInst *mcInst, unsigned Reg)
{
	MCOperand *op = &(mcInst->Operands[mcInst->size]);
	mcInst->size++;

	op->Kind = kRegister;
	op->RegVal = Reg;
}

inline __attribute__ ((always_inline))
MCOperand *MCOperand_CreateImm1(MCInst *mcInst, int64_t Val)
{
	MCOperand *op = &(mcInst->Operands[MCINST_CACHE]);

	op->Kind = kImmediate;
	op->ImmVal = Val;

	return op;
}

inline __attribute__ ((always_inline))
void MCOperand_CreateImm0(MCInst *mcInst, int64_t Val)
{
	MCOperand *op = &(mcInst->Operands[mcInst->size]);
	mcInst->size++;

	op->Kind = kImmediate;
	op->ImmVal = Val;
}
#endif
