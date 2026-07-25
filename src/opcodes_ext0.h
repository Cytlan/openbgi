#ifndef OPCODES_EXT0_H_
#define OPCODES_EXT0_H_

#include <stdint.h>

typedef struct Thread Thread_t;

uint32_t Opcode_Ext0_Unknown_0(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_4(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_5(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_8(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_16(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_17(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_20(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_21(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_22(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_23(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_24(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_25(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_26(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_32(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_36(Thread_t* thread);
uint32_t Opcode_Ext0_SetTextColor(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_38(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_39(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_128(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_129(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_130(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_132(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_133(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_143(Thread_t* thread);
uint32_t Opcode_Ext0_Unknown_240(Thread_t* thread);

extern OpcodePtr_t OpcodesExt0[256];
extern char* OpcodesExt0Mnemonics[256];

#endif