//
//
//

#include "bgi.h"
#include "patch.h"

int(**opcodeJumptable)(VMThread_t* thread) = (int(**)(VMThread_t*))0x00488420;

VMThread_t** gVMThread = (VMThread_t**)0x0049d2f8;

int* bgiThreadCount = (int*)0x004994f4;
//extern uint8_t* gAuxMemory[]; // = (uint8_t**)0x004beac0;
//uint8_t* gAuxMemory = (uint8_t**)0x004beac0;

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

uint8_t* BGI_ResolveAddr(uint32_t address, VMThread_t* thread)
{
	if(address == 0)
		return NULL;

	int addrUpper = address >> 24;

	switch(addrUpper)
	{
		case 0x00:
		case 0x01:
			return gGlobalMem + (address & 0x1ffffff);
		case 0x10:
		case 0x11:
			return BGI_GetCodeSpace(thread) + (address & 0x1ffffff);
		case 0x12:
		case 0x13:
			return BGI_GetLocalMem(thread) + (address & 0x1ffffff);
		case 0x14:
		case 0x15:
			return BGI_GetUnknownStuctMem(thread, address & 0x1ffffff);
		default:
			if(addrUpper < 0x20)
			{
				// Crash - Undefined area between 0x00xxxxxx and 0x20xxxxxx
				MessageBox(NULL, "Undefined memory area", "Memory error", 0);
				return NULL;
			}

			uint8_t* auxMem = BGI_GetAuxMemory((addrUpper >> 1) - 0x10);
			if(auxMem == NULL)
			{
				// Crash - Aux mem out of bounds
				MessageBox(NULL, "Undefined aux memory area", "Memory error", 0);
				return NULL;
			}
			return auxMem + (address & 0x1ffffff);
	}
}

uint8_t* BGI_PopAndResolveAddress(VMThread_t* thread)
{
	uint32_t address = BGI_PopStack(thread);
	uint8_t* ptr = BGI_ResolveAddr(address, thread);
    return ptr;
}

uint32_t BGI_GetMemPointer(VMThread_t* thread)
{
	return thread->memPtr;
}

uint32_t BGI_GetInstructionPointer(VMThread_t* thread)
{
	return thread->instructionPointer;
}

uint8_t* BGI_GetCodeSpace(VMThread_t* thread)
{
	return thread->codeSpace;
}

uint8_t* BGI_GetLocalMem(VMThread_t* thread)
{
	return thread->localMem;
}

uint8_t* BGI_GetUnknownStuctMem(VMThread_t* thread, uint32_t address)
{
	return thread->unknownStruct->buf + address;
}

uint8_t* BGI_GetAuxMemory(int slot)
{
	if(slot > 0x30)
		return NULL;
	return gAuxMemory[slot];
}

void BGI_WriteIntToMemory(uint8_t* ptr, int intSize, uint32_t data)
{
	if(intSize == 0)
	{
		*ptr = (uint8_t)data;
	}
	else if(intSize == 1)
	{
		*(uint16_t*)ptr = (uint16_t)data;
	}
	else if(intSize == 2)
	{
		*(uint32_t*)ptr = (uint32_t)data;
	}
}
