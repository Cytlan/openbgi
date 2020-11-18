//
//
//

#include "bgi.h"
#include "patch.h"

int(**opcodeJumptable)(VMThread_t* thread) = (int(**)(VMThread_t*))0x00488420;

VMThread_t** gVMThread = (VMThread_t**)0x0049d2f8;

int* bgiThreadCount = (int*)0x004994f4;

VMThread_t* gLastExecutedVMThread = NULL;

// Read 1 immediate byte from the code
char __thiscall BGI_ReadCode8(VMThread_t* vmThread)
{
	// Read data from code space
	char data = vmThread->codeSpace[vmThread->programCounter];
	vmThread->programCounter++;

	return data;
}

// Read 2 immediate bytes from the code
short __thiscall BGI_ReadCode16(VMThread_t* vmThread)
{
	// Read data from code space
	short data = *(short*)(vmThread->codeSpace + vmThread->programCounter);
	vmThread->programCounter += 2;

	return data;
}

// Read 4 immediate bytes from the code
int __thiscall BGI_ReadCode32(VMThread_t* vmThread)
{
	// Read data from code space
	int data = *(int*)(vmThread->codeSpace + vmThread->programCounter);
	vmThread->programCounter += 4;

	return data;
}

int __thiscall BGI_PopStack(VMThread_t *vmThread)
{
	// Stack underflow
	if(vmThread->stackPointer == 0)
		vmThread->stackPointer = vmThread->stackSize;
	vmThread->stackPointer--;

	// Read data from stack
	int data = vmThread->stack[vmThread->stackPointer];

	return data;
}

void __thiscall BGI_PushStack(VMThread_t* vmThread, int data)
{
	// Write data to stack
	vmThread->stack[vmThread->stackPointer++] = data;

	// Stack overflow
	if(vmThread->stackPointer >= vmThread->stackSize)
		vmThread->stackPointer = 0;
}

uint32_t BGI_GetMemPointer(VMThread_t* thread)
{
	return thread->memPtr;
}
