#ifndef OPCODES_H_
#define OPCODES_H_

#include <stdint.h>
#include "engine.h"

typedef struct Thread Thread_t;

typedef uint32_t (*OpcodePtr_t)(Thread_t*);

uint32_t Opcode_Push8(Thread_t* thread);
uint32_t Opcode_Push16(Thread_t* thread);
uint32_t Opcode_Push32(Thread_t* thread);
uint32_t Opcode_CodeAddr(Thread_t* thread);
uint32_t Opcode_CodeOffset(Thread_t* thread);
uint32_t Opcode_PushBasePointer(Thread_t* thread);
uint32_t Opcode_PopBasePointer(Thread_t* thread);
uint32_t Opcode_Jump(Thread_t* thread);
uint32_t Opcode_CondJump(Thread_t* thread);
uint32_t Opcode_Add(Thread_t* thread);
uint32_t Opcode_Sys0(Thread_t* thread);
uint32_t Opcode_Grp0(Thread_t* thread);
uint32_t Opcode_Grp1(Thread_t* thread);
uint32_t Opcode_Grp2(Thread_t* thread);
uint32_t Opcode_Snd0(Thread_t* thread);
uint32_t Opcode_Ext0(Thread_t* thread);
uint32_t Opcode_Ext1(Thread_t* thread);
uint32_t Opcode_BaseOffset(Thread_t* thread);
uint32_t Opcode_ReadMem(Thread_t* thread);
uint32_t Opcode_WriteCopy(Thread_t* thread);
uint32_t Opcode_WriteMem(Thread_t* thread);
uint32_t Opcode_CopyStack(Thread_t* thread);
uint32_t Opcode_Ternary(Thread_t* thread);
uint32_t Opcode_Call(Thread_t* thread);
uint32_t Opcode_Ret(Thread_t* thread);
uint32_t Opcode_SHL(Thread_t* thread);
uint32_t Opcode_Mul(Thread_t* thread);
uint32_t Opcode_Div(Thread_t* thread);
uint32_t Opcode_Sub(Thread_t* thread);
uint32_t Opcode_SAR(Thread_t* thread);
uint32_t Opcode_Eq(Thread_t* thread);
uint32_t Opcode_Neq(Thread_t* thread);
uint32_t Opcode_Leq(Thread_t* thread);
uint32_t Opcode_Geq(Thread_t* thread);
uint32_t Opcode_Ge(Thread_t* thread);
uint32_t Opcode_Le(Thread_t* thread);
uint32_t Opcode_SHR(Thread_t* thread);
uint32_t Opcode_Mod(Thread_t* thread);
uint32_t Opcode_And(Thread_t* thread);
uint32_t Opcode_Or(Thread_t* thread);
uint32_t Opcode_Xor(Thread_t* thread);
uint32_t Opcode_Not(Thread_t* thread);
uint32_t Opcode_Sprintf(Thread_t* thread);
uint32_t Opcode_Memcpy(Thread_t* thread);
uint32_t Opcode_Memclr(Thread_t* thread);
uint32_t Opcode_DoubleNotZero(Thread_t* thread);
uint32_t Opcode_DoubleAnyNotZero(Thread_t* thread);
uint32_t Opcode_IsZero(Thread_t* thread);
uint32_t Opcode_Strcpy(Thread_t* thread);
uint32_t Opcode_StrToLower(Thread_t* thread);
uint32_t Opcode_Strlen(Thread_t* thread);
uint32_t Opcode_Streq(Thread_t* thread);

extern OpcodePtr_t Opcodes[256];
extern char* OpcodesMnemonics[256];

#endif