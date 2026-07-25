#ifndef OPCODES_SND0_H_
#define OPCODES_SND0_H_

#include <stdint.h>

typedef struct Thread Thread_t;

uint32_t Opcode_Snd0_Unknown_0(Thread_t* thread);
uint32_t Opcode_Snd0_SetChannelVolume(Thread_t* thread);
uint32_t Opcode_Snd0_SetEffectVolume(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_16(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_17(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_18(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_20(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_21(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_22(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_23(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_24(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_25(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_32(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_33(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_34(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_36(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_37(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_38(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_128(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_129(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_132(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_133(Thread_t* thread);
uint32_t Opcode_Snd0_Unknown_134(Thread_t* thread);
uint32_t Opcode_Snd0_PlaySound(Thread_t* thread);

extern OpcodePtr_t OpcodesSnd0[256];
extern char* OpcodesSnd0Mnemonics[256];

#endif