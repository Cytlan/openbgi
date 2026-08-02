#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "thread.h"
#include "engine.h"
#include "opcodes.h"
#include "golden_log.h"

char* TLevel[4] = {
	"",
	"    ",
	"        ",
	"            "
};

uint32_t Thread_LoadCode(Thread_t* thread, uint8_t* code, const char* filename)
{
	Program_t* program = (Program_t*)malloc(sizeof(Program_t));
	size_t nameLen = strlen(filename);
	program->filename = (char*)malloc(nameLen + 1);
	strcpy(program->filename, filename);
	uint32_t programSize = *(uint32_t*)(code + 4);
	uint32_t programOffset = *(uint32_t*)(code);
	program->size = programSize;
	program->location = thread->codeSpaceUsed;
	program->previousProgram = thread->programs;
	thread->programs = program;
	memcpy(thread->code + thread->codeSpaceUsed, code + programOffset, programSize);
	thread->codeSpaceUsed += programSize;
	thread->programCount++;

	printf("[Thread %d]: %sLoaded code (0x%.8X) from \"%s\" into 0x%.8X\n", thread->threadId, TLevel[thread->level], program->size, program->filename, program->location);

	return program->location;
}

uint32_t Thread_DeleteProgram(Thread_t* thread)
{
	Program_t* program;
	uint32_t res = 0x80000001;
	program = thread->programs;
	if(program != NULL)
	{
		thread->programs = program->previousProgram;
		thread->codeSpaceUsed -= program->size;
		thread->programCount--;
		free(program->filename);
		free(program);
		res = thread->programCount;
	}
	return res;
}

uint8_t Thread_ReadImm8(Thread_t* thread)
{
	thread->instructionPointer = thread->nextInstructionPointer;

	uint8_t data = thread->code[thread->instructionPointer];

	if(GoldenLogTotal)
	{
		if(GoldenLog[GoldenLogIndex].type != LOG_TYPE_R8)
		{
			printf("[Thread %d]: (%u) Golden log R8 mismatch! Type isn't LOG_TYPE_R8\n", thread->threadId, GoldenLog[GoldenLogIndex].time);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].thread != thread->threadId)
		{
			printf("[Thread %d]: (%u) Golden log R8 mismatch! Thread is not %d, but %d\n", thread->threadId, GoldenLog[GoldenLogIndex].time, thread->threadId, GoldenLog[GoldenLogIndex].thread);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].tick != thread->ticks)
		{
			printf("[Thread %d]: (%u) Golden log R8 mismatch! Tick is not %d, but %d\n", thread->threadId, GoldenLog[GoldenLogIndex].time, thread->ticks, GoldenLog[GoldenLogIndex].tick);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].value != data)
		{
			printf("[Thread %d]: (%u) Golden log R8 mismatch! Value is not 0x%.8X, but 0x%.8X\n", thread->threadId, GoldenLog[GoldenLogIndex].time, data, GoldenLog[GoldenLogIndex].value);
			thread->running = 0;
			thread->error = 2;
		}
		GoldenLogIndex++;
	}

	thread->nextInstructionPointer = thread->nextInstructionPointer + 1;
	return data;
}

uint8_t Thread_ReadCode8(Thread_t* thread)
{
	uint8_t data = thread->code[thread->nextInstructionPointer];
	if(!thread->inBasicOpcode || !thread->silenceBasicOpcodeLog)
		printf("[Thread %d]: %sReadCode8() @ 0x%.8X = 0x%.2X\n", thread->threadId, TLevel[thread->level], thread->nextInstructionPointer, data);
	thread->nextInstructionPointer = thread->nextInstructionPointer + 1;

	if(GoldenLogTotal)
	{
		if(GoldenLog[GoldenLogIndex].type != LOG_TYPE_RC8)
		{
			printf("[Thread %d]: (%u) Golden log RC8 mismatch! Type isn't LOG_TYPE_RC8\n", thread->threadId, GoldenLog[GoldenLogIndex].time);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].thread != thread->threadId)
		{
			printf("[Thread %d]: (%u) Golden log RC8 mismatch! Thread is not %d, but %d\n", thread->threadId, GoldenLog[GoldenLogIndex].time, thread->threadId, GoldenLog[GoldenLogIndex].thread);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].tick != thread->ticks)
		{
			printf("[Thread %d]: (%u) Golden log RC8 mismatch! Tick is not %d, but %d\n", thread->threadId, GoldenLog[GoldenLogIndex].time, thread->ticks, GoldenLog[GoldenLogIndex].tick);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].value != data)
		{
			printf("[Thread %d]: (%u) Golden log RC8 mismatch! Value is not 0x%.8X, but 0x%.8X\n", thread->threadId, GoldenLog[GoldenLogIndex].time, data, GoldenLog[GoldenLogIndex].value);
			thread->running = 0;
			thread->error = 2;
		}
		GoldenLogIndex++;
	}
	return data;
}

uint16_t Thread_ReadCode16(Thread_t* thread)
{
	uint16_t data = thread->code[thread->nextInstructionPointer] | (thread->code[thread->nextInstructionPointer + 1] << 8);
	if(!thread->inBasicOpcode || !thread->silenceBasicOpcodeLog)
		printf("[Thread %d]: %sReadCode16() @ 0x%.8X = 0x%.4X\n", thread->threadId, TLevel[thread->level], thread->nextInstructionPointer, data);
	thread->nextInstructionPointer = thread->nextInstructionPointer + 2;

	if(GoldenLogTotal)
	{
		if(GoldenLog[GoldenLogIndex].type != LOG_TYPE_RC16)
		{
			printf("[Thread %d]: (%u) Golden log RC16 mismatch! Type isn't LOG_TYPE_RC16\n", thread->threadId, GoldenLog[GoldenLogIndex].time);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].thread != thread->threadId)
		{
			printf("[Thread %d]: (%u) Golden log RC16 mismatch! Thread is not %d, but %d\n", thread->threadId, GoldenLog[GoldenLogIndex].time, thread->threadId, GoldenLog[GoldenLogIndex].thread);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].tick != thread->ticks)
		{
			printf("[Thread %d]: (%u) Golden log RC16 mismatch! Tick is not %d, but %d\n", thread->threadId, GoldenLog[GoldenLogIndex].time, thread->ticks, GoldenLog[GoldenLogIndex].tick);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].value != data)
		{
			printf("[Thread %d]: (%u) Golden log RC16 mismatch! Value is not 0x%.8X, but 0x%.8X\n", thread->threadId, GoldenLog[GoldenLogIndex].time, data, GoldenLog[GoldenLogIndex].value);
			thread->running = 0;
			thread->error = 2;
		}
		GoldenLogIndex++;
	}
	return data;
}

uint32_t Thread_ReadCode32(Thread_t* thread)
{
	uint32_t data = 
		thread->code[thread->nextInstructionPointer] | 
		(thread->code[thread->nextInstructionPointer + 1] << 8) |
		(thread->code[thread->nextInstructionPointer + 2] << 16) |
		(thread->code[thread->nextInstructionPointer + 3] << 24);
	if(!thread->inBasicOpcode || !thread->silenceBasicOpcodeLog)
		printf("[Thread %d]: %sReadCode32() @ 0x%.8X = 0x%.8X\n", thread->threadId, TLevel[thread->level], thread->nextInstructionPointer, data);
	thread->nextInstructionPointer = thread->nextInstructionPointer + 4;

	if(GoldenLogTotal)
	{
		if(GoldenLog[GoldenLogIndex].type != LOG_TYPE_RC32)
		{
			printf("[Thread %d]: (%u) Golden log RC32 mismatch! Type isn't LOG_TYPE_RC32\n", thread->threadId, GoldenLog[GoldenLogIndex].time);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].thread != thread->threadId)
		{
			printf("[Thread %d]: (%u) Golden log RC32 mismatch! Thread is not %d, but %d\n", thread->threadId, GoldenLog[GoldenLogIndex].time, thread->threadId, GoldenLog[GoldenLogIndex].thread);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].tick != thread->ticks)
		{
			printf("[Thread %d]: (%u) Golden log RC32 mismatch! Tick is not %d, but %d\n", thread->threadId, GoldenLog[GoldenLogIndex].time, thread->ticks, GoldenLog[GoldenLogIndex].tick);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].value != data)
		{
			printf("[Thread %d]: (%u) Golden log RC32 mismatch! Value is not 0x%.8X, but 0x%.8X\n", thread->threadId, GoldenLog[GoldenLogIndex].time, data, GoldenLog[GoldenLogIndex].value);
			thread->running = 0;
			thread->error = 2;
		}
		GoldenLogIndex++;
	}

	return data;
}

void Thread_PushStack(Thread_t* thread, uint32_t data)
{
	if(GoldenLogTotal)
	{
		// Get Sys0.GetSysTime value from the log
		if(GoldenLog[GoldenLogIndex].value != data && thread->opcode == 0x8004)
		{
			printf("[Thread %d]: (%u) Golden log PUSH overwrite, from 0x%.8X, but 0x%.8X\n", thread->threadId, GoldenLog[GoldenLogIndex].time, data, GoldenLog[GoldenLogIndex].value);
			data = GoldenLog[GoldenLogIndex].value;
		}
		// Get Sys0.PopGlobalList value from the log
		if(GoldenLog[GoldenLogIndex].value != data && thread->opcode == 0x80A0)
		{
			printf("[Thread %d]: (%u) Golden log PUSH overwrite, from 0x%.8X, but 0x%.8X\n", thread->threadId, GoldenLog[GoldenLogIndex].time, data, GoldenLog[GoldenLogIndex].value);
			data = GoldenLog[GoldenLogIndex].value;
		}

		if(GoldenLog[GoldenLogIndex].type != LOG_TYPE_PUSH)
		{
			printf("[Thread %d]: (%u) Golden log PUSH mismatch! Type isn't LOG_TYPE_PUSH\n", thread->threadId, GoldenLog[GoldenLogIndex].time);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].thread != thread->threadId)
		{
			printf("[Thread %d]: (%u) Golden log PUSH mismatch! Thread is not %d, but %d\n", thread->threadId, GoldenLog[GoldenLogIndex].time, thread->threadId, GoldenLog[GoldenLogIndex].thread);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].tick != thread->ticks)
		{
			printf("[Thread %d]: (%u) Golden log PUSH mismatch! Tick is not %d, but %d\n", thread->threadId, GoldenLog[GoldenLogIndex].time, thread->ticks, GoldenLog[GoldenLogIndex].tick);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].value != data)
		{
			printf("[Thread %d]: (%u) Golden log PUSH mismatch! Value is not 0x%.8X, but 0x%.8X\n", thread->threadId, GoldenLog[GoldenLogIndex].time, data, GoldenLog[GoldenLogIndex].value);
			thread->running = 0;
			thread->error = 2;
		}
		GoldenLogIndex++;
	}

	if(!thread->inBasicOpcode || !thread->silenceBasicOpcodeLog)
		printf("[Thread %d]: %sPushStack(0x%.8X) @ 0x%.8X\n", thread->threadId, TLevel[thread->level], data, thread->stackPointer);
	if(thread->stackPointer == thread->stackSize)
	{
		//sp = thread->stackSize;
		if(!thread->inBasicOpcode || !thread->silenceBasicOpcodeLog)
			printf("[Thread %d]: %sStack overflow!\n", thread->threadId, TLevel[thread->level]);
		//thread->running = 0;
		//thread->error = 1;
		//return;
		thread->stackPointer = 0;
	}
	thread->stack[thread->stackPointer] = data;
	thread->stackPointer++;
}

uint32_t Thread_PopStack(Thread_t* thread)
{
	uint32_t sp = thread->stackPointer;
	if(sp == 0)
	{
		//sp = thread->stackSize;
		if(!thread->inBasicOpcode || !thread->silenceBasicOpcodeLog)
			printf("[Thread %d]: %sStack underflow!\n", thread->threadId, TLevel[thread->level]);
		//thread->running = 0;
		//thread->error = 1;
		//return 0;
		thread->stackPointer = thread->stackSize;
		sp = thread->stackPointer;
	}
	sp--;
	thread->stackPointer = sp;
	uint32_t data = thread->stack[sp];
	if(!thread->inBasicOpcode || !thread->silenceBasicOpcodeLog)
		printf("[Thread %d]: %sPopStack() @ 0x%.8X = 0x%.8X\n", thread->threadId, TLevel[thread->level], thread->stackPointer, data);

	if(GoldenLogTotal)
	{
		if(GoldenLog[GoldenLogIndex].type != LOG_TYPE_POP)
		{
			printf("[Thread %d]: (%u) Golden log POP mismatch! Type isn't LOG_TYPE_POP\n", thread->threadId, GoldenLog[GoldenLogIndex].time);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].thread != thread->threadId)
		{
			printf("[Thread %d]: (%u) Golden log POP mismatch! Thread is not %d, but %d\n", thread->threadId, GoldenLog[GoldenLogIndex].time, thread->threadId, GoldenLog[GoldenLogIndex].thread);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].tick != thread->ticks)
		{
			printf("[Thread %d]: (%u) Golden log POP mismatch! Tick is not %d, but %d\n", thread->threadId, GoldenLog[GoldenLogIndex].time, thread->ticks, GoldenLog[GoldenLogIndex].tick);
			thread->running = 0;
			thread->error = 2;
		}
		if(GoldenLog[GoldenLogIndex].value != data)
		{
			printf("[Thread %d]: (%u) Golden log POP mismatch! Value is not 0x%.8X, but 0x%.8X\n", thread->threadId, GoldenLog[GoldenLogIndex].time, data, GoldenLog[GoldenLogIndex].value);
			thread->running = 0;
			thread->error = 2;
		}
		GoldenLogIndex++;
	}
	return data;
}

void Thread_SchedulePush(Thread_t* thread, uint32_t data)
{
	thread->queuePushQueue[thread->queuePush] = data;
	thread->queuePush++;
}

uint32_t Thread_GetInstructionPointer(Thread_t* thread)
{
	return thread->instructionPointer;
}

void Thread_SetInstructionPointer(Thread_t* thread, uint32_t value)
{
	if(!thread->silenceBasicOpcodeLog)
		printf("[Thread %d]: %sSet instruction pointer to 0x%.8X\n", thread->threadId, TLevel[thread->level], value);
	thread->nextInstructionPointer = value;
}

uint32_t Thread_GetBasePointer(Thread_t* thread)
{
	return thread->basePointer;
}

void Thread_SetBasePointer(Thread_t* thread, uint32_t value)
{
	if(!thread->silenceBasicOpcodeLog)
		printf("[Thread %d]: %sSet base pointer to 0x%.8X\n", thread->threadId, TLevel[thread->level], value);
	thread->basePointer = value;
}

void Thread_SetUnknownTimestamp(Thread_t* thread, uint32_t value)
{
	// Should use GetTickCount() or similar instead of 0
	uint32_t timestamp = 0 + value;
	printf("[Thread %d]: %sSet unknown timestamp to 0x%.8X\n", thread->threadId, TLevel[thread->level], timestamp);
	thread->unknownTimestamp = timestamp;
}

uint32_t Thread_Execute(Thread_t* thread)
{
	uint8_t opcode = Thread_ReadImm8(thread);
	thread->inBasicOpcode = 1;
	if(!thread->silenceBasicOpcodeLog)
		printf("[Thread %d]: %s[%.8X] Executing opcode %s (0x%.2X / %d)\n", thread->threadId, TLevel[thread->level], thread->instructionPointer, OpcodesMnemonics[opcode], opcode, opcode);
	thread->level++;
	thread->opcode = opcode;

	if(GoldenLogTotal)
	{
		if(GoldenLog[GoldenLogIndex].type != LOG_TYPE_EXEC)
		{
			printf("[Thread %d]: (%u) Golden log EXEC mismatch! Type isn't LOG_TYPE_EXEC\n", thread->threadId, GoldenLog[GoldenLogIndex].time);
			thread->running = 0;
			thread->error = 2;
			return 0xFFFFFFFE;
		}
		if(GoldenLog[GoldenLogIndex].thread != thread->threadId)
		{
			printf("[Thread %d]: (%u) Golden log EXEC mismatch! Thread is not %d, but %d\n", thread->threadId, GoldenLog[GoldenLogIndex].time, thread->threadId, GoldenLog[GoldenLogIndex].thread);
			thread->running = 0;
			thread->error = 2;
			return 0xFFFFFFFE;
		}
		if(GoldenLog[GoldenLogIndex].tick != thread->ticks)
		{
			printf("[Thread %d]: (%u) Golden log EXEC mismatch! Tick is not %d, but %d\n", thread->threadId, GoldenLog[GoldenLogIndex].time, thread->ticks, GoldenLog[GoldenLogIndex].tick);
			thread->running = 0;
			thread->error = 2;
			return 0xFFFFFFFE;
		}
		if(GoldenLog[GoldenLogIndex].value != opcode)
		{
			printf("[Thread %d]: (%u) Golden log EXEC mismatch! Value is not 0x%.8X, but 0x%.8X\n", thread->threadId, GoldenLog[GoldenLogIndex].time, opcode, GoldenLog[GoldenLogIndex].value);
			thread->running = 0;
			thread->error = 2;
			return 0xFFFFFFFE;
		}
		GoldenLogIndex++;
	}

	if(Opcodes[opcode] == NULL)
	{
		thread->level--;
		printf("[Thread %d]: %sError: opcode 0x%.2X (%d) not implemented\n", thread->threadId, TLevel[thread->level], opcode, opcode);
		return 0xFFFFFFFD;
	}
	uint32_t res = Opcodes[opcode](thread);
	thread->level--;

	if(GoldenLog[GoldenLogIndex].thread == thread->threadId && GoldenLog[GoldenLogIndex].tick == thread->ticks)
	{
		int idx = GoldenLogIndex;
		while(GoldenLog[idx].tick == thread->ticks)
		{
			switch(GoldenLog[idx].type)
			{
				case LOG_TYPE_EXEC:
					printf("[Thread %d]: (%u) Golden log missing LOG_TYPE_EXEC\n", thread->threadId, GoldenLog[GoldenLogIndex].time);
				case LOG_TYPE_R8:
					printf("[Thread %d]: (%u) Golden log missing LOG_TYPE_R8\n", thread->threadId, GoldenLog[GoldenLogIndex].time);
				case LOG_TYPE_RC8:
					printf("[Thread %d]: (%u) Golden log missing LOG_TYPE_RC8\n", thread->threadId, GoldenLog[GoldenLogIndex].time);
				case LOG_TYPE_RC16:
					printf("[Thread %d]: (%u) Golden log missing LOG_TYPE_RC16\n", thread->threadId, GoldenLog[GoldenLogIndex].time);
				case LOG_TYPE_RC32:
					printf("[Thread %d]: (%u) Golden log missing LOG_TYPE_RC32\n", thread->threadId, GoldenLog[GoldenLogIndex].time);
				case LOG_TYPE_PUSH:
					printf("[Thread %d]: (%u) Golden log missing LOG_TYPE_PUSH\n", thread->threadId, GoldenLog[GoldenLogIndex].time);
				case LOG_TYPE_POP:
					printf("[Thread %d]: (%u) Golden log missing LOG_TYPE_POP\n", thread->threadId, GoldenLog[GoldenLogIndex].time);

			}
			idx++;
		}
		thread->running = 0;
		thread->error = 2;
	}
	if(thread->error == 1)
		return 0xFFFFFFFC;
	if(thread->error == 2)
		return 0xFFFFFFFE;
	thread->ticks++;
	return res;
}

uint8_t* Thread_ResolveAddr(Thread_t* thread, uint32_t address)
{
	if(address == 0)
		return NULL;

	int tag = address >> 24;
	uint32_t offset = address & 0x00ffffff;

	// TODO: Bounds checking
	switch(tag)
	{
		case 0:
			if(!thread->inBasicOpcode || !thread->silenceBasicOpcodeLog)
				printf("[Thread %d]: %sResolved address: GlobalMem Offset 0x%.8X\n", thread->threadId, TLevel[thread->level], offset);
			return thread->engine->globalMem + offset;
		case 0x10:
			if(!thread->inBasicOpcode || !thread->silenceBasicOpcodeLog)
				printf("[Thread %d]: %sResolved address: LocalMem Offset 0x%.8X\n", thread->threadId, TLevel[thread->level], offset);
			return thread->localMem + offset;
		case 0x11:
			if(!thread->inBasicOpcode || !thread->silenceBasicOpcodeLog)
				printf("[Thread %d]: %sResolved address: CodeMem Offset 0x%.8X\n", thread->threadId, TLevel[thread->level], offset);
			return thread->code + offset;
		case 0x12:
			printf("[Thread %d]: %sError: Struct Memory area %.2X is not implemented\n", thread->threadId, TLevel[thread->level], tag);
			//return BGI_GetUnknownStuctMem(thread, offset);
			return NULL;
		default:
			if(tag < 0x40)
			{
				printf("[Thread %d]: %sError: %.2X is an invalid memory area\n", thread->threadId, TLevel[thread->level], tag);
				return NULL;
			}
			int slot = (tag >> 1) - 32;
			if(thread->engine->auxMemory[slot] == NULL)
			{
				printf("[Thread %d]: %sError: %.2X is an uninitialised aux memory area\n", thread->threadId, TLevel[thread->level], tag);
				thread->running = 0;
				thread->error = 1;
				return NULL;
			}

			if(!thread->inBasicOpcode || !thread->silenceBasicOpcodeLog)
				printf("[Thread %d]: %sResolved address: AuxMem Offset 0x%.8X\n", thread->threadId, TLevel[thread->level], offset);
			return thread->engine->auxMemory[slot] + offset;

			//printf("[Thread %d]: %sError: Aux Memory area %.2X is not implemented\n", thread->threadId, TLevel[thread->level], tag);
			////return BGI_GetUnknownStuctMem(thread, offset);
			//thread->running = 0;
			//thread->error = 1;
			//return NULL;
			/*
			if(tag < 16)
			{
				printf("[Thread %d]: %sError: %.2X is an invalid memory area\n", thread->threadId, TLevel[thread->level], tag);
				return NULL;
			}

			uint8_t* auxMem = Engine_GetAuxMemory(tag - 16);
			if(auxMem == NULL)
			{
				printf("[Thread %d]: %sError: %.2X is an uninitialised aux memory area\n", thread->threadId, TLevel[thread->level], tag);
				return NULL;
			}
			return auxMem + offset;
			*/
	}
}

uint8_t* Thread_PopAndResolveAddress(Thread_t* thread)
{
	uint32_t address = Thread_PopStack(thread);
	uint8_t* ptr = Thread_ResolveAddr(thread, address);
    return ptr;
}

uint32_t Thread_WriteIntToMemory(Thread_t* thread, uint8_t* ptr, uint8_t size, uint32_t value)
{
	switch(size)
	{
		case 0:
			if(!thread->inBasicOpcode || !thread->silenceBasicOpcodeLog)
				printf("[Thread %d]: %sWrite 0x%.2X to memory\n", thread->threadId, TLevel[thread->level], value);
			*(uint8_t*)ptr = (uint8_t)value;
			break;
		case 1:
			if(!thread->inBasicOpcode || !thread->silenceBasicOpcodeLog)
				printf("[Thread %d]: %sWrite 0x%.4X to memory\n", thread->threadId, TLevel[thread->level], value);
			*(uint16_t*)ptr = (uint16_t)value;
			break;
		case 2:
			if(!thread->inBasicOpcode || !thread->silenceBasicOpcodeLog)
				printf("[Thread %d]: %sWrite 0x%.8X to memory\n", thread->threadId, TLevel[thread->level], value);
			*(uint32_t*)ptr = (uint32_t)value;
			break;
		default:
			printf("[Thread %d]: %sInvalid size %d of value 0x%.8X when writing to memory\n", thread->threadId, TLevel[thread->level], size, value);
			return 1;
	}
	return 0;
}

void Thread_WriteReturnAddr(Thread_t* thread, uint32_t addr)
{
    *(uint32_t*)(thread->localMem + thread->basePointer) = addr;
    thread->basePointer += 4;
}

uint32_t Thread_GetLocalMemSize(Thread_t* thread)
{
	return thread->localMemSize;
}

uint32_t Thread_ReadReturnAddr(Thread_t* thread)
{
	thread->basePointer -= 4;
	return *(uint32_t *)(thread->localMem + thread->basePointer);
}

uint32_t Thread_GetThreadID(Thread_t* thread)
{
	return thread->threadId;
}

void Thread_Sprintf(Thread_t* thread, char* dst, const char* fmt)
{
	size_t args[16] = {0};
	int argCount = 0;
	const char* p = fmt;

	while(*p != '\0')
	{
		if(*p != '%')
		{
			++p;
			continue;
		}

		++p;  // skip '%'
		if(*p == '\0')
		{
			//crash("BGI_Sprintf: format string ends with '%'", (char *)thread);
			return;
		}

		// Skip optional flags (-,0, ,.,+,#) and width/precision digits
		while(*p == ' ' || *p == '0' || *p == '-' || *p == '.' || *p == '+' || *p == '#')
		{
			++p;
		}
		while(*p >= '0' && *p <= '9')
		{
			++p;
		}

		if(*p == '\0')
		{
			//crash("BGI_Sprintf: incomplete format specifier", (char *)thread);
			return;
		}

		char conv = *p;

		if(conv == '%')
		{
			++p;  // literal %%
			continue;
		}

		if(conv == 'd' || conv == 'c' || conv == 'x' || conv == 'X')
		{
			if(argCount >= 16)
			{
				//crash("BGI_Sprintf: too many arguments (maximum 16)", (char *)thread);
				return;
			}
			args[argCount++] = Thread_PopStack(thread);
		}
		else if(conv == 's')
		{
			if(argCount >= 16)
			{
				//crash("BGI_Sprintf: too many arguments (maximum 16)", (char *)thread);
				return;
			}
			uint8_t* str_ptr = Thread_PopAndResolveAddress(thread);
			args[argCount++] = (size_t)str_ptr;
		}
		else
		{
			//char err[256];
			//snprintf(err, sizeof(err), "BGI_Sprintf: unknown format specifier '%%%c'", conv);
			//crash(err, (char *)thread);
			return;
		}

		++p;  // skip conversion character
	}

	// Dispatch to sprintf with the exact number of arguments popped from the VM
	if(!thread->inBasicOpcode || !thread->silenceBasicOpcodeLog)
		printf("sprintf with %d args to %.16lX\n", argCount, (size_t)dst);
	switch(argCount)
	{
		case 0:  strcpy(dst, fmt); break;
		case 1:  sprintf(dst, fmt, args[0]); break;
		case 2:  sprintf(dst, fmt, args[0], args[1]); break;
		case 3:  sprintf(dst, fmt, args[0], args[1], args[2]); break;
		case 4:  sprintf(dst, fmt, args[0], args[1], args[2], args[3]); break;
		case 5:  sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4]); break;
		case 6:  sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5]); break;
		case 7:  sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6]); break;
		case 8:  sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]); break;
		case 9:  sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]); break;
		case 10: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9]); break;
		case 11: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10]); break;
		case 12: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11]); break;
		case 13: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12]); break;
		case 14: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13]); break;
		case 15: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14]); break;
		case 16: sprintf(dst, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15]); break;
		default: strcpy(dst, fmt); break;
	}
	if(!thread->inBasicOpcode || !thread->silenceBasicOpcodeLog)
		printf("[Thread %d]: %sSprintf dst: %s, fmt: %s, Number of arguments: %d\n", thread->threadId, TLevel[thread->level], dst, fmt, argCount);
}