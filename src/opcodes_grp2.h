#ifndef OPCODES_GRP2_H_
#define OPCODES_GRP2_H_

#include <stdint.h>

typedef struct Thread Thread_t;

uint32_t Opcode_Grp2_Unknown_0(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_1(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_16(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_17(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_24(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_25(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_28(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_29(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_30(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_31(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_136(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_137(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_138(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_140(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_141(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_142(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_144(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_145(Thread_t* thread);
uint32_t Opcode_Grp2_Unknown_156(Thread_t* thread);

extern OpcodePtr_t OpcodesGrp2[256];
extern char* OpcodesGrp2Mnemonics[256];

#endif