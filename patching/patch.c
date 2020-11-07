//
// Code that is called into from patches in the game and
// code that pokes around at the internals of the game.
//

#include "patch.h"

int(**opcodeJumptable)(VMThread_t* vmThread) = (int(**)(VMThread_t*))0x00488420;

VMThread_t** gVMThread = (VMThread_t**)0x0049d2f8;
VMThread_t* gLastExecutedVMThread = NULL;
int* bgiThreadCount = (int*)0x004994f4;

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

void printThreadInfo(char* out, VMThread_t* thread)
{
	sprintf(out,
		"programId          %.8LX\n"
		"threadId           %.8LX\n"
		"nextThread         %.8LX\n"
		"flags              %.8LX\n"
		"stackPointer       %.8LX\n"
		"instructionPointer %.8LX\n"
		"programCounter     %.8LX\n"
		"basePointer        %.8LX\n"
		"stackSize          %.8LX\n"
		"stackMemConfig     %.8LX\n"
		"stack              %.8LX\n"
		"codeSpaceSize      %.8LX\n"
		"codeSpaceMemConfig %.8LX\n"
		"codeSpace          %.8LX\n"
		"programList        %.8LX\n"
		"programCount       %.8LX\n"
		"codeSpaceTop       %.8LX\n"
		"localMemSize       %.8LX\n"
		"localMemConfig     %.8LX\n"
		"localMem           %.8LX\n"
		"unknownStruct      %.8LX\n"
		"field_0x54         %.8LX\n"
		"field_0x58         %.8LX\n"
		"field_0x5c         %.8LX\n"
		"field_0x60         %.8LX\n",
		(uint32_t)thread->programId,
		(uint32_t)thread->threadId,
		(uint32_t)thread->nextThread,
		(uint32_t)thread->flags,
		(uint32_t)thread->stackPointer,
		(uint32_t)thread->instructionPointer,
		(uint32_t)thread->programCounter,
		(uint32_t)thread->basePointer,
		(uint32_t)thread->stackSize,
		(uint32_t)thread->stackMemConfig,
		(uint32_t)thread->stack,
		(uint32_t)thread->codeSpaceSize,
		(uint32_t)thread->codeSpaceMemConfig,
		(uint32_t)thread->codeSpace,
		(uint32_t)thread->programList,
		(uint32_t)thread->programCount,
		(uint32_t)thread->codeSpaceTop,
		(uint32_t)thread->localMemSize,
		(uint32_t)thread->localMemConfig,
		(uint32_t)thread->localMem,
		(uint32_t)thread->unknownStruct,
		(uint32_t)thread->field_0x54,
		(uint32_t)thread->field_0x58,
		(uint32_t)thread->field_0x5c,
		(uint32_t)thread->field_0x60
	);
}

uint32_t getThreadIP(int threadId)
{
	if(!gVMThread)
		return 0;
	int count = 0;
	VMThread_t* t = *gVMThread;
	while(t)
	{
		if(count == threadId)
			return t->instructionPointer;
		count++;
		t = t->nextThread;
	}
	return 0;
}

int countThreads()
{
	if(!gVMThread)
		return 0;
	int count = 0;
	VMThread_t* t = *gVMThread;
	while(t)
	{
		count++;
		t = t->nextThread;
	}
	return count;
}

// This should be called as soon as possible in WinMain
void init()
{
	logFile = fopen("execution.log", "wb");
	if(logFile == NULL)
	{
		MessageBoxA(NULL, "Failed to create execution log file", "Error", 0);
		ExitProcess(1);
	}
}

// Called whenever the VM tries to execute an instruction
int executeOpcode(int opcode, VMThread_t* vmThread)
{
	gLastExecutedVMThread = vmThread;

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
	//int size = buildBuffer();
	//fwrite(&buf[0], 1, size, logFile);

	return res;
}

// Read 1 immediate byte from the code
__thiscall char BGI_ReadCode8(VMThread_t* vmThread)
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
__thiscall short BGI_ReadCode16(VMThread_t* vmThread)
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
__thiscall int BGI_ReadCode32(VMThread_t* vmThread)
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

__thiscall int BGI_PopStack(VMThread_t *vmThread)
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

__thiscall void BGI_PushStack(VMThread_t* vmThread, int data)
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
