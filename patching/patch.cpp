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

bool inInstruction = false;
int numOperations = 0;
LogOperation_t operation;
FILE* logFile;

uint8_t buf[4096];

int buildBuffer()
{
	int index = 0;

	buf[index++] = operation.opcode;
	buf[index++] = operation.res;
	buf[index++] = operation.thread;

	*(uint16_t*)(&buf[index]) = operation.numStackIn; index += 2;
	*(uint16_t*)(&buf[index]) = operation.numStackOut; index += 2;
	*(uint16_t*)(&buf[index]) = operation.numBytesIn; index += 2;

	*(uint32_t*)(&buf[index]) = operation.pc; index += 4;

	for(int i = 0; i < operation.numStackIn; i++)
	{
		*(uint32_t*)(&buf[index]) = operation.stackIn[i]; index += 4;
	}
	for(int i = 0; i < operation.numStackOut; i++)
	{
		*(uint32_t*)(&buf[index]) = operation.stackOut[i]; index += 4;
	}
	for(int i = 0; i < operation.numBytesIn; i++)
	{
		buf[index++] = operation.bytesIn[i];
	}
	return index;
}

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
	operation.opcode = opcode;
	operation.pc = vmThread->instructionPointer;
	operation.thread = vmThread->threadId;
	operation.numStackIn = 0;
	operation.numStackOut = 0;
	operation.numBytesIn = 0;

	// Execute opcode
	int res = opcodeJumptable[opcode](vmThread);

	operation.res = res;

	inInstruction = false;

	// Write log file
	int size = buildBuffer();
	fwrite(&buf[0], 1, size, logFile);

	return res;
}

// Read 1 immediate byte from the code
extern "C" __thiscall char BGI_ReadCode8(VMThread_t* vmThread)
{
	// Read data from code space
	char data = vmThread->codeSpace[vmThread->programCounter];
	vmThread->programCounter++;

	// Log data
	if(inInstruction)
	{
		operation.bytesIn[operation.numBytesIn++] = 1;
		operation.bytesIn[operation.numBytesIn++] = data;
	}

	return data;
}

// Read 2 immediate bytes from the code
extern "C" __thiscall short BGI_ReadCode16(VMThread_t* vmThread)
{
	// Read data from code space
	short data = *(short*)(vmThread->codeSpace + vmThread->programCounter);
	vmThread->programCounter += 2;

	if(inInstruction)
	{
		operation.bytesIn[operation.numBytesIn++] = 2;
		*(uint16_t*)(&operation.bytesIn[operation.numBytesIn]) = data;
		operation.numBytesIn += 2;
	}

	return data;
}

// Read 4 immediate bytes from the code
extern "C" __thiscall int BGI_ReadCode32(VMThread_t* vmThread)
{
	// Read data from code space
	int data = *(int*)(vmThread->codeSpace + vmThread->programCounter);
	vmThread->programCounter += 4;

	// Log data
	if(inInstruction)
	{
		operation.bytesIn[operation.numBytesIn++] = 4;
		*(uint32_t*)(&operation.bytesIn[operation.numBytesIn]) = data;
		operation.numBytesIn += 4;
	}

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
	if(inInstruction)
	{
		operation.stackIn[operation.numStackIn++] = data;
	}

	return data;
}

extern "C" __thiscall void BGI_PushStack(VMThread* vmThread, int data)
{
	// Write data to stack
	vmThread->stack[vmThread->stackPointer++] = data;

	// Log data
	if(inInstruction)
	{
		operation.stackOut[operation.numStackOut++] = data;
	}

	// Stack overflow
	if(vmThread->stackPointer >= vmThread->stackSize)
		vmThread->stackPointer = 0;
}

