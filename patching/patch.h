#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifndef _PATCH_H_
#define _PATCH_H_

#include "bgi.h"

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
extern VMThread_t* curThreadPtr;

// Execution control
extern int gHaltExecution;
extern int gStepExecution;
extern int gStepThread;

// Disassembler
extern BGIOpcode_t basicInstructions[256];
extern BGIOpcode_t sys0Instructions[256];

// -----------------------------------------------------------------------------
//
// VM funcs
//
// -----------------------------------------------------------------------------

typedef VMThread_t* (__thiscall *BGI_AllocateVMThread_)(VMThread_t* parent, int stackSize, unsigned int codeSpaceSize, unsigned int localMemSize);
#define BGI_AllocateVMThread ((BGI_AllocateVMThread_)0x0042e030)

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

// Switch opcodes to our re-implementations
void overrideVMOpcodes();

// Windows stuff
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved);
LRESULT WINAPI DLLWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);



#endif
