//
//
//

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifndef _BGI_H_
#define _BGI_H_

#define BGI_MEMORY_OFFSET 0x12000000
#define BGI_CODE_OFFSET   0x10000000

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
// Note: These must be __thiscall, as it's expected when calling from assembly
char  __thiscall BGI_ReadCode8(VMThread_t* vmThread);
short __thiscall BGI_ReadCode16(VMThread_t* vmThread);
int   __thiscall BGI_ReadCode32(VMThread_t* vmThread);
int   __thiscall BGI_PopStack(VMThread_t* vmThread);
void  __thiscall BGI_PushStack(VMThread_t* vmThread, int data);

uint8_t* BGI_PopAndResolveAddress(VMThread_t* thread);
uint8_t* BGI_ResolveAddr(uint32_t address, VMThread_t* thread);
uint32_t BGI_GetMemPointer(VMThread_t* thread);
void BGI_SetMemPointer(VMThread_t* thread, uint32_t newPtr);
uint32_t BGI_GetLocalMemSize(VMThread_t* thread);
uint32_t BGI_GetInstructionPointer(VMThread_t* thread);
void BGI_SetInstructionPointer(VMThread_t* thread, uint32_t address);
uint8_t* BGI_GetCodeSpace(VMThread_t* thread);
uint32_t BGI_GetCodeSpaceSize(VMThread_t* thread);
uint8_t* BGI_GetLocalMem(VMThread_t* thread);
uint8_t* BGI_GetUnknownStuctMem(VMThread_t* thread, uint32_t address);
uint8_t* BGI_GetAuxMemory(int slot);
void BGI_WriteIntToMemory(uint8_t* ptr, int intSize, uint32_t data);

// Note: Must be __cdecl
int __cdecl op_push8(VMThread_t* thread);       // 0x00
int __cdecl op_push16(VMThread_t* thread);      // 0x01
int __cdecl op_push32(VMThread_t* thread);      // 0x02
int __cdecl op_memptr(VMThread_t* thread);      // 0x04
int __cdecl op_codeptr(VMThread_t* thread);     // 0x05
int __cdecl op_codeoffset(VMThread_t* thread);  // 0x06
int __cdecl op_readmem(VMThread_t* thread);     // 0x08
int __cdecl op_writecopy(VMThread_t* thread);   // 0x09
int __cdecl op_write(VMThread_t* thread);       // 0x0A
int __cdecl op_copystack(VMThread_t* thread);   // 0x0C
int __cdecl op_memptr2(VMThread_t* thread);     // 0x10
int __cdecl op_storememptr(VMThread_t* thread); // 0x11
int __cdecl op_jmp(VMThread_t* thread);         // 0x14
int __cdecl op_cjmp(VMThread_t* thread);        // 0x15
int __cdecl op_add(VMThread_t* thread);         // 0x20
int __cdecl op_sys0(VMThread_t* thread);        // 0x80

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

extern int(*sys0Jumptable[0x100])(VMThread_t* thread);

#define gAuxMemory ((uint8_t**)0x004beac0)    // Pointer to array of uint8_t* pointers (uint8_t* gAuxMemory[])
#define gGlobalMem (*((uint8_t**)0x004bd43c)) // Pointer to uint8_t* array (uint8_t* gGlobalMem)

#endif
