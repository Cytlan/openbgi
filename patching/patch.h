#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifndef _PATCH_H_
#define _PATCH_H_

// -----------------------------------------------------------------------------
//
// BGI Structs (Exported from Ghidra)
//
// -----------------------------------------------------------------------------
typedef struct VMMemory VMMemory_t;
typedef struct VMThread VMThread_t;
typedef struct VMUnknownStruct VMUnknownStruct_t;
typedef struct VMProgramList VMProgramList_t;

struct VMMemory
{
	int isAllocated;
	uint8_t* mem;
	int size;
};

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

struct VMProgramList
{
	char* filename;
	int size;
	int location;
	VMProgramList_t* prevProgram;
};

// -----------------------------------------------------------------------------
//
// Debugger structs
//
// -----------------------------------------------------------------------------
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

typedef struct Breakpoint Breakpoint_t;
struct Breakpoint
{
	bool inUse;
	int type;
	bool enabled;
	char name[64];
	uint32_t value;
};
extern Breakpoint_t gBreakpoints[32];

// -----------------------------------------------------------------------------
//
// Disassembler
//
// -----------------------------------------------------------------------------
typedef struct BGIOpcode
{
	char* mnemonic;
	int numOperands;
	char operands[4];
} BGIOpcode_t;

typedef struct DisasmLine DisasmLine_t;
struct DisasmLine
{
	char* str;
	uint32_t location;
	int isLabel;
	DisasmLine_t* next;
};

typedef struct Disasm Disasm_t;
struct Disasm
{
	VMThread_t* thread;
	VMProgramList_t* program;
	char* filename;
	uint32_t location;
	uint32_t size;
	DisasmLine_t* disasm;
	Disasm_t* next;
};

// -----------------------------------------------------------------------------
//
// Debugger globals
//
// -----------------------------------------------------------------------------
// Windows
extern HMODULE gDllHModule;
extern HWND gDebuggerWindow;
extern HFONT gFont;

// Debugger
extern VMThread_t** gVMThread;
extern VMThread_t* gLastExecutedVMThread;

// Execution control
extern int gHaltExecution;
extern int gStepExecution;
extern int gStepThread;

// Disassembler
extern BGIOpcode_t basicInstructions[256];
extern BGIOpcode_t sys0Instructions[256];

// -----------------------------------------------------------------------------
//
// Debugger funcs
//
// -----------------------------------------------------------------------------
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
HWND makeButton(char* title, HWND wnd, int x, int y, int width, int height, int id);

// Debugging
bool createDebugWindow();
Disasm_t* disassembleProgram(VMThread_t* thread, VMProgramList_t* program);
void freeDisassemblies(Disasm_t* disasm);

// Breakpoints
bool createBreakpointsWindow();
void disableBreakpointsInputs();
void showBreakpointsWindow();
void hideBreakpointsWindow();
void populateBreakpointList();
void setBreakpointsWindow(Breakpoint_t* breakpoint);
Breakpoint_t* getFreeBreakpoint();

// Windows stuff
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved);
LRESULT WINAPI DLLWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

#endif
