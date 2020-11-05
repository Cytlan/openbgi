//
// DLL to be called into from the patch assembly
//
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

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
    struct VMThread* prevVMState;
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

int(**opcodeJumptable)(VMThread_t* vmThread) = (int(**)(VMThread_t*))0x00488420;

typedef enum LogType
{
	TYPE_NONE = 0,
	TYPE_OPCODE,     // 1
	TYPE_OPCODE_RES, // 2
	TYPE_READ8,      // 3
	TYPE_READ16,     // 4
	TYPE_READ32,     // 5
	TYPE_POP,        // 6
	TYPE_PUSH        // 7
} LOG_TYPE;

typedef struct LogOperations
{
	LOG_TYPE type;
	uint32_t data;
} LogOperations_t;

bool inInstruction = false;
int numOperations = 0;
LogOperations_t operations[1024];
FILE* logFile;

// This should be called as soon as possible in WinMain
extern "C" void init()
{
	logFile = fopen("execution.log", "wb");
	if(logFile == NULL)
	{
		MessageBoxA(NULL, "Failed to create execution log file", "Error", 0);
		ExitProcess(1);
	}
}

// Called whenever the VM tries to execute an instruction
extern "C" int executeOpcode(int opcode, VMThread_t* vmThread)
{
	inInstruction = true;
	numOperations = 0;

	// Log opcode
	operations[numOperations].type = TYPE_OPCODE;
	operations[numOperations].data = opcode;
	numOperations++;

	// Execute opcode
	int res = opcodeJumptable[opcode](vmThread);

	// Log result
	operations[numOperations].type = TYPE_OPCODE_RES;
	operations[numOperations].data = res;
	numOperations++;

	inInstruction = false;

	// Write log file
	fwrite(&operations[0], sizeof(LogOperations_t), numOperations, logFile);

	return res;
}

// Read 1 immediate byte from the code
extern "C" __thiscall char BGI_ReadCode8(VMThread_t* vmThread)
{
	// Read data from code space
	char data = vmThread->codeSpace[vmThread->programCounter];
	vmThread->programCounter++;

	// Log data
	operations[numOperations].type = TYPE_READ8;
	operations[numOperations].data = data;
	numOperations++;

	return data;
}

// Read 2 immediate bytes from the code
extern "C" __thiscall short BGI_ReadCode16(VMThread_t* vmThread)
{
	// Read data from code space
	short data = *(short*)(vmThread->codeSpace + vmThread->programCounter);
	vmThread->programCounter += 2;

	operations[numOperations].type = TYPE_READ16;
	operations[numOperations].data = data;
	numOperations++;

	return data;
}

// Read 4 immediate bytes from the code
extern "C" __thiscall int BGI_ReadCode32(VMThread_t* vmThread)
{
	// Read data from code space
	int data = *(int*)(vmThread->codeSpace + vmThread->programCounter);
	vmThread->programCounter += 4;

	// Log data
	operations[numOperations].type = TYPE_READ32;
	operations[numOperations].data = data;
	numOperations++;

	return data;
}

extern "C" __thiscall int BGI_PopStack(VMThread *vmThread)
{
	// Stack underflow
	if(vmThread->stackPointer == 0)
		vmThread->stackPointer = vmThread->stackSize;
	vmThread->stackPointer--;

	// Read data from stack
	int data = vmThread->stack[vmThread->stackPointer];

	// Log data
	operations[numOperations].type = TYPE_POP;
	operations[numOperations].data = data;
	numOperations++;

	return data;
}

extern "C" __thiscall void BGI_PushStack(VMThread* vmThread, int data)
{
	// Write data to stack
	vmThread->stack[vmThread->stackPointer++] = data;

	// Log data
	operations[numOperations].type = TYPE_PUSH;
	operations[numOperations].data = data;
	numOperations++;

	// Stack overflow
	if(vmThread->stackPointer >= vmThread->stackSize)
		vmThread->stackPointer = 0;
}

