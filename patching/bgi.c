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

void BGI_SetMemPointer(VMThread_t* thread, uint32_t newPtr)
{
	thread->memPtr = newPtr;
}

uint32_t BGI_GetLocalMemSize(VMThread_t* thread)
{
	return thread->localMemSize;
}

uint32_t BGI_GetInstructionPointer(VMThread_t* thread)
{
	return thread->instructionPointer;
}

void BGI_SetInstructionPointer(VMThread_t* thread, uint32_t address)
{
	thread->instructionPointer = address;
	thread->programCounter = address;
}

uint8_t* BGI_GetCodeSpace(VMThread_t* thread)
{
	return thread->codeSpace;
}

uint32_t BGI_GetCodeSpaceSize(VMThread_t* thread)
{
	return thread->codeSpaceSize;
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

void BGI_WriteReturnAddr(VMThread_t* thread, uint32_t addr)
{
    *(uint32_t*)(thread->localMem + thread->memPtr) = addr;
    thread->memPtr += 4;
}

uint32_t BGI_ReadReturnAddr(VMThread_t* thread)
{
	thread->memPtr -= 4;
	return *(uint32_t *)(thread->localMem + thread->memPtr);
}

void BGI_Sprintf(char* dst, char* fmt, VMThread_t* thread)
{
	uint32_t args[16];
	int argCount = 0;
	char* ptr = fmt;
	char c = *ptr;
	while(c)
	{
		c = *ptr++;

		if(c != '%')
			continue;

		c = *ptr++;
		if(!c) break; // Check for string end
		
		// Flags
		while(c == '-' || c == '+' || c == ' ' || c == '#' || c == '0')
			c = *ptr++;
		if(!c) break; // Check for string end

		// Width
		if(c == '*')
		{
			// Variable width
			args[argCount++] = BGI_PopStack(thread);
			c = *ptr++;
		}
		else
		{
			while(c >= '0' && c <= '9')
				c = *ptr++;
		}
		if(!c) break; // Check for string end

		// Precision
		if(c == '.')
		{
			c = *ptr++;
			if(c == '*')
			{
				// Variable precision
				args[argCount++] = BGI_PopStack(thread);
				c = *ptr++;
			}
			else
			{
				while(c >= '0' && c <= '9')
					c = *ptr++;
			}
		}
		if(!c) break; // Check for string end

		// Length
		while(c == 'h' || c == 'l' || c == 'j' || c == 'z' || c == 't' || c == 'L')
			c = *ptr++;
		if(!c) break; // Check for string end

		// Specifier
		if(c == 's')
			args[argCount++] = (uint32_t)BGI_PopAndResolveAddress(thread);
		else
			args[argCount++] = BGI_PopStack(thread);
	}
	switch(argCount)
	{
		case 0: sprintf(dst, fmt); break;
		case 1: sprintf(dst, fmt, args[0]); break;
		case 2: sprintf(dst, fmt, args[0], args[1]); break;
		case 3: sprintf(dst, fmt, args[0], args[1], args[2]); break;
		case 4: sprintf(dst, fmt, args[0], args[1], args[2], args[3]); break;
		case 5: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4]); break;
		case 6: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5]); break;
		case 7: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6]); break;
		case 8: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]); break;
		case 9: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]); break;
		case 10: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9]); break;
		case 11: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10]); break;
		case 12: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11]); break;
		case 13: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12]); break;
		case 14: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13]); break;
		case 15: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14]); break;
		case 16: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15]); break;
	}
	printf("dst: %s, fmt: %s, Number of arguments: %d\n", dst, fmt, argCount);
}

bool BGI_IsDoubleByteSJIS(char c)
{
	if((c < 0x80 || c > 0x9F) && c < 0xE0)
		return false;
	return true;
}

void BGI_StrToLowerCase(char* ptr)
{
	char c = *ptr;
	while(c)
	{
		if(BGI_IsDoubleByteSJIS(c))
			ptr++;
		else
		{
			if(c >= 'A' && c <= 'Z')
				*ptr |= 0x20;
		}
		ptr++;
		c = *ptr;
	}
}

uint16_t delimiters[36] = {
	0x002C, // ,
	0x002E, // .
	0x00A4, // ､
	0x00A1, // ｡
	0x003A, // :
	0x003B, // ;
	0x003F, // ?
	0x0021, // !
	0x00DE, // ﾞ
	0x00DF, // ﾟ
	0x00A5, // ･
	0x8141, // 、
	0x8142, // 。
	0x8143, // ，
	0x8144, // ．
	0x8146, // ：
	0x8147, // ；
	0x8148, // ？
	0x8149, // ！
	0x814A, // ゛
	0x814B, // ゜
	0x815D, // ‐
	0x005D, // ]
	0x007D, // }
	0x0029, // )
	0x816A, // ）
	0x816C, // 〕
	0x816E, // ］
	0x8170, // ｝
	0x8172, // 〉
	0x8174, // 》
	0x8176, // 」
	0x8178, // 』
	0x817A, // 】
	0x8165, // ‘
	0x8167  // “
};

bool BGI_IsDelimiter(uint16_t c)
{
	for(int i = 0; i < 36; i++)
	{
		if(c == delimiters[i])
			return true;
	}
	return false;
}
