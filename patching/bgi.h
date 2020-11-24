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
void BGI_WriteReturnAddr(VMThread_t* thread, uint32_t addr);
uint32_t BGI_ReadReturnAddr(VMThread_t* thread);
void BGI_Sprintf(char* dst, char* fmt, VMThread_t* thread);

// Note: Must be __cdecl
int __cdecl op_push8(VMThread_t* thread);          // 0x00
int __cdecl op_push16(VMThread_t* thread);         // 0x01
int __cdecl op_push32(VMThread_t* thread);         // 0x02
int __cdecl op_memptr(VMThread_t* thread);         // 0x04
int __cdecl op_codeptr(VMThread_t* thread);        // 0x05
int __cdecl op_codeoffset(VMThread_t* thread);     // 0x06
int __cdecl op_readmem(VMThread_t* thread);        // 0x08
int __cdecl op_writecopy(VMThread_t* thread);      // 0x09
int __cdecl op_write(VMThread_t* thread);          // 0x0A
int __cdecl op_copystack(VMThread_t* thread);      // 0x0C
int __cdecl op_memptr2(VMThread_t* thread);        // 0x10
int __cdecl op_storememptr(VMThread_t* thread);    // 0x11
int __cdecl op_jmp(VMThread_t* thread);            // 0x14
int __cdecl op_cjmp(VMThread_t* thread);           // 0x15
int __cdecl op_call(VMThread_t* thread);           // 0x16
int __cdecl op_ret(VMThread_t* thread);            // 0x17
int __cdecl op_add(VMThread_t* thread);            // 0x20
int __cdecl op_sub(VMThread_t* thread);            // 0x21
int __cdecl op_mul(VMThread_t* thread);            // 0x22
int __cdecl op_div(VMThread_t* thread);            // 0x23
int __cdecl op_mod(VMThread_t* thread);            // 0x24
int __cdecl op_and(VMThread_t* thread);            // 0x25
int __cdecl op_or(VMThread_t* thread);             // 0x26
int __cdecl op_xor(VMThread_t* thread);            // 0x27
int __cdecl op_not(VMThread_t* thread);            // 0x28
int __cdecl op_shl(VMThread_t* thread);            // 0x29
int __cdecl op_shr(VMThread_t* thread);            // 0x2A
int __cdecl op_sar(VMThread_t* thread);            // 0x2B
int __cdecl op_eq(VMThread_t* thread);             // 0x30
int __cdecl op_neq(VMThread_t* thread);            // 0x31
int __cdecl op_leq(VMThread_t* thread);            // 0x32
int __cdecl op_geq(VMThread_t* thread);            // 0x33
int __cdecl op_le(VMThread_t* thread);             // 0x34
int __cdecl op_ge(VMThread_t* thread);             // 0x35
int __cdecl op_dnotzero(VMThread_t* thread);       // 0x38
int __cdecl op_dnotzero2(VMThread_t* thread);      // 0x39
int __cdecl op_iszero(VMThread_t* thread);         // 0x3A
int __cdecl op_ternary(VMThread_t* thread);        // 0x40
int __cdecl op_muldiv(VMThread_t* thread);         // 0x42
int __cdecl op_sin(VMThread_t* thread);            // 0x48
int __cdecl op_cos(VMThread_t* thread);            // 0x49
int __cdecl op_memcpy(VMThread_t* thread);         // 0x60
int __cdecl op_memclr(VMThread_t* thread);         // 0x61
int __cdecl op_memset(VMThread_t* thread);         // 0x62
int __cdecl op_memcmp(VMThread_t* thread);         // 0x63
int __cdecl op_strreplace(VMThread_t* thread);     // 0x67
int __cdecl op_strlen(VMThread_t* thread);         // 0x68
int __cdecl op_streq(VMThread_t* thread);          // 0x69
int __cdecl op_strcpy(VMThread_t* thread);         // 0x6A
int __cdecl op_strconcat(VMThread_t* thread);      // 0x6B
int __cdecl op_getchar(VMThread_t* thread);        // 0x6C
int __cdecl op_sprintf(VMThread_t* thread);        // 0x6F
int __cdecl op_addmemboundary(VMThread_t* thread); // 0x75
int __cdecl op_sys0(VMThread_t* thread);           // 0x80

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
