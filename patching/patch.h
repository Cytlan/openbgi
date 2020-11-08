#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifndef _PATCH_H_
#define _PATCH_H_

struct VMMemory;
struct VMThread;
struct VMUnknownStruct;
struct VMProgramList;

typedef struct VMMemory
{
	int isAllocated;
	uint8_t* mem;
	int size;
} VMMemory_t;

typedef struct VMThread
{
	uint32_t programId;
	uint32_t threadId;
	struct VMThread* nextThread;
	uint32_t flags;
	uint32_t stackPointer;
	uint32_t instructionPointer;
	uint32_t programCounter;
	uint32_t basePointer;
	uint32_t stackSize;
	struct VMMemory* stackMemConfig;
	uint32_t* stack;
	uint32_t codeSpaceSize;
	struct VMMemory* codeSpaceMemConfig;
	uint8_t* codeSpace;
	struct VMProgramList* programList;
	uint32_t programCount;
	uint32_t codeSpaceTop;
	uint32_t localMemSize;
	struct VMMemory* localMemConfig;
	uint8_t* localMem;
	struct VMUnknownStruct* unknownStruct;
	int (*field_0x54)(int);
	uint32_t field_0x58;
	uint32_t field_0x5c;
	uint32_t field_0x60;
} VMThread_t;

typedef struct VMUnknownStruct
{
	int field_0x0;
	uint8_t * buf;
	int field_0x8;
	int field_0xc;
	struct VMMemory * mem;
	int field_0x14;
	int field_0x18;
	int field_0x1c;
} VMUnknownStruct_t;

typedef struct VMProgramList
{
	char* filename;
	int size;
	int location;
	struct VMProgramList* prevProgram;
} VMProgramList_t;

typedef struct LogOperation
{
	uint8_t opcode;
	uint8_t res;
	uint8_t thread;
	uint16_t numStackIn;
	uint16_t numStackOut;
	uint16_t numBytesIn;
	uint32_t pc;
	uint32_t stackIn[512];
	uint32_t stackOut[512];
	uint8_t bytesIn[1024];
} __attribute__((__packed__)) LogOperation_t;

struct DisasmLine;
typedef struct DisasmLine
{
	char* str;
	uint32_t location;
	int isLabel;
	struct DisasmLine* next;
} DisasmLine_t;

struct Disasm;
typedef struct Disasm
{
	VMThread_t* thread;
	VMProgramList_t* program;
	char* filename;
	uint32_t location;
	uint32_t size;
	DisasmLine_t* disasm;
	struct Disasm* next;
} Disasm_t;

extern VMThread_t** gVMThread;
extern VMThread_t* gLastExecutedVMThread;
extern HMODULE gDllHModule;

extern int gHaltExecution;
extern int gStepExecution;
extern int gStepThread;

int buildBuffer();
int countThreads();
uint32_t getThreadIP(int threadId);
void updateVMInfo(VMThread_t* thread);

// Called from assembly
void init();
int executeOpcode(int opcode, VMThread_t* vmThread);
__thiscall char BGI_ReadCode8(VMThread_t* vmThread);
__thiscall short BGI_ReadCode16(VMThread_t* vmThread);
__thiscall int BGI_ReadCode32(VMThread_t* vmThread);
__thiscall int BGI_PopStack(VMThread_t* vmThread);
__thiscall void BGI_PushStack(VMThread_t* vmThread, int data);

// Misc
void shutdownDebugger();
void fatalError(char* error);

// Debugging
bool createDebugWindow();
Disasm_t* disassembleProgram(VMThread_t* thread, VMProgramList_t* program);
void freeDisassemblies(Disasm_t* disasm);


// Windows stuff
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved);
LRESULT WINAPI DLLWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

#endif
