#ifndef THREAD_H_
#define THREAD_H_

#include <stdint.h>

typedef struct Engine Engine_t;

// Program structure:
// 4 bytes - Code offset
// 4 bytes - Program size
typedef struct Program Program_t;
struct Program
{
	char* filename;
	uint32_t size;
	uint32_t location;
	Program_t* previousProgram;
};

typedef struct Memory Memory_t;
struct Memory
{
	int isAllocated;
	uint8_t* mem;
	uint32_t size;
};


typedef struct Thread Thread_t;
struct Thread
{
	uint32_t programId;
	uint32_t threadId;
	Thread_t* previousThread;
	uint32_t flags;
	uint32_t stackPointer;
	uint32_t instructionPointer;
	uint32_t nextInstructionPointer;
	uint32_t basePointer;
	uint32_t stackSize;
	Memory_t stackMemoryConfig;
	uint32_t* stack;
	uint32_t codeSize;
	Memory_t codeMemoryConfig;
	uint8_t* code;
	Program_t* programs;
	uint32_t programCount;
	uint32_t codeSpaceUsed;
	uint32_t localMemSize;
	Memory_t localMemConfig;
	uint8_t* localMem;
	void* unknownStruct;
	void* unknownFuncPointer;
	uint32_t unknownTimestamp;
	uint32_t unknownField2;
	uint32_t unknownField3;

	int level;
	int running;
	int ticks;
	int error;
	Engine_t* engine;
	uint16_t opcode;
	int waitTicks;
	int queuePush;
	int queuePushQueue[10];

	int inBasicOpcode;
	int silenceBasicOpcodeLog;
	int silenceYield;
	int silenceGlobalList;
};

extern char* TLevel[4];

uint32_t Thread_LoadCode(Thread_t* thread, uint8_t* code, const char* filename);
uint32_t Thread_DeleteProgram(Thread_t* thread);
void Thread_PushStack(Thread_t* thread, uint32_t data);
uint32_t Thread_PopStack(Thread_t* thread);
uint8_t Thread_ReadCode8(Thread_t* thread);
uint16_t Thread_ReadCode16(Thread_t* thread);
uint32_t Thread_ReadCode32(Thread_t* thread);
uint8_t Thread_ReadImm8(Thread_t* thread);
uint32_t Thread_Execute(Thread_t* thread);
uint32_t Thread_GetBasePointer(Thread_t* thread);
void Thread_SetBasePointer(Thread_t* thread, uint32_t value);
uint32_t Thread_GetInstructionPointer(Thread_t* thread);
void Thread_SetInstructionPointer(Thread_t* thread, uint32_t value);
void Thread_SetUnknownTimestamp(Thread_t* thread, uint32_t value);
uint8_t* Thread_PopAndResolveAddress(Thread_t* thread);
uint8_t* Thread_ResolveAddr(Thread_t* thread, uint32_t address);
uint32_t Thread_WriteIntToMemory(Thread_t* thread, uint8_t* ptr, uint8_t size, uint32_t value);
void Thread_WriteReturnAddr(Thread_t* thread, uint32_t addr);
uint32_t Thread_ReadReturnAddr(Thread_t* thread);
uint32_t Thread_GetLocalMemSize(Thread_t* thread);
uint32_t Thread_GetThreadID(Thread_t* thread);
void Thread_Sprintf(Thread_t* thread, char* dst, const char* fmt);
void Thread_SchedulePush(Thread_t* thread, uint32_t data);

#endif