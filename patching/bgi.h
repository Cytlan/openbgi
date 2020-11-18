//
//
//

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifndef _BGI_H_
#define _BGI_H_

#define BGI_MEMORY_OFFSET 0x12000000

//
// Forward declarations - Structs
//
typedef struct VMMemory VMMemory_t;
typedef struct VMThread VMThread_t;
typedef struct VMUnknownStruct VMUnknownStruct_t;
typedef struct VMProgramList VMProgramList_t;

//
// Forward declarations - Functions
//
// Note: Must be __thiscall, as it's expected when calling from assembly
char  __thiscall BGI_ReadCode8(VMThread_t* vmThread);
short __thiscall BGI_ReadCode16(VMThread_t* vmThread);
int   __thiscall BGI_ReadCode32(VMThread_t* vmThread);
int   __thiscall BGI_PopStack(VMThread_t* vmThread);
void  __thiscall BGI_PushStack(VMThread_t* vmThread, int data);

// Note: Must be __cdecl
int __cdecl op_push8(VMThread_t* thread);  // 0x00
int __cdecl op_push16(VMThread_t* thread); // 0x01
int __cdecl op_push32(VMThread_t* thread); // 0x02
int __cdecl op_memptr(VMThread_t* thread); // 0x04

//
// Allocated memory
//
struct VMMemory
{
	int isAllocated;
	uint8_t* mem;
	int size;
};

//
// Thread state
//
struct VMThread
{
	uint32_t programId;
	uint32_t threadId;
	VMThread_t* nextThread;
	uint32_t flags;
	uint32_t stackPointer;
	uint32_t instructionPointer;
	uint32_t programCounter;
	uint32_t memPtr;
	uint32_t stackSize;
	VMMemory_t* stackMemConfig;
	uint32_t* stack;
	uint32_t codeSpaceSize;
	VMMemory_t* codeSpaceMemConfig;
	uint8_t* codeSpace;
	VMProgramList_t* programList;
	uint32_t programCount;
	uint32_t codeSpaceTop;
	uint32_t localMemSize;
	VMMemory_t* localMemConfig;
	uint8_t* localMem;
	VMUnknownStruct_t* unknownStruct;
	int (*field_0x54)(int);
	uint32_t field_0x58;
	uint32_t field_0x5c;
	uint32_t field_0x60;
};

//
// ????
//
struct VMUnknownStruct
{
	int field_0x0;
	uint8_t* buf;
	int field_0x8;
	int field_0xc;
	VMMemory_t* mem;
	int field_0x14;
	int field_0x18;
	int field_0x1c;
};

//
// List of loaded programs - Unique per thread
//
struct VMProgramList
{
	char* filename;
	int size;
	int location;
	VMProgramList_t* prevProgram;
};

extern int(**opcodeJumptable)(VMThread_t* thread);
extern VMThread_t** gVMThread;
extern VMThread_t* gLastExecutedVMThread;
extern int* bgiThreadCount;

#endif
