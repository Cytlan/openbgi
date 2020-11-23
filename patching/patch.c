//
// Code that is called into from patches in the game and
// code that pokes around at the internals of the game.
//

#include "patch.h"

int gHaltExecution = 1;
int gStepExecution = 0;
int gStepThread = 0;

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
		"memPtr             %.8LX\n"
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
		(uint32_t)thread->memPtr,
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
	//logFile = fopen("execution.log", "wb");
	//if(logFile == NULL)
	//{
	//	MessageBoxA(NULL, "Failed to create execution log file", "Error", 0);
	//	ExitProcess(1);
	//}

	// Unicode, please
	SetConsoleOutputCP(CP_UTF8);
	_setmode(_fileno(stdout), _O_U8TEXT);
	_setmode(_fileno(stderr), _O_U8TEXT);

	overrideVMOpcodes();
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

	bool doRun = true;
	int res;

	if(gHaltExecution)
	{
		if(gStepExecution && vmThread->threadId == gStepThread)
		{
			doRun = true;
			gStepExecution = 0;
			gStepThread = 0;
		}
		else
		{
			// Pretend the current thread yielded
			doRun = false;
			res = 1;
			vmThread->instructionPointer--; // Correct the counters, so that we don't skip the real
			vmThread->programCounter--;     // instruction when we begin execution again.
		}
	}

	if(doRun)
	{
		// Execute opcode
		res = opcodeJumptable[opcode](vmThread);
	}

	operation.res = res;

	inInstruction = false;

	// Write log file
	//int size = buildBuffer();
	//fwrite(&buf[0], 1, size, logFile);

	return res;
}

void haltExecution(VMThread_t* thread)
{
	curThreadPtr = thread;
	doHaltExecution(1);
}

int __cdecl op_unknown(VMThread_t* thread)
{
	uint8_t opcode = thread->codeSpace[thread->instructionPointer];
	printf("Undefined opcode: 0x%.2X (%d) at address %.8LX\n", opcode, opcode, thread->instructionPointer);
	thread->instructionPointer--;
	thread->programCounter--;
	haltExecution(thread);
}

// Switch opcodes to our re-implementations
void overrideVMOpcodes()
{
	//for(int i = 0; i < 0x100; i++)
	//	opcodeJumptable[i] = op_unknown;

	opcodeJumptable[0x00] = op_push8;
	opcodeJumptable[0x01] = op_push16;
	opcodeJumptable[0x02] = op_push32;
	opcodeJumptable[0x04] = op_memptr;
	opcodeJumptable[0x05] = op_codeptr;
	opcodeJumptable[0x06] = op_codeoffset;
	opcodeJumptable[0x08] = op_readmem;
	opcodeJumptable[0x09] = op_writecopy;
	opcodeJumptable[0x0A] = op_write;
	opcodeJumptable[0x0B] = op_unknown; // TODO: op_copycode
	opcodeJumptable[0x0C] = op_copystack;
	opcodeJumptable[0x10] = op_memptr2;
	opcodeJumptable[0x11] = op_storememptr;
	opcodeJumptable[0x14] = op_jmp;
	opcodeJumptable[0x15] = op_cjmp;
	opcodeJumptable[0x20] = op_add;
	opcodeJumptable[0x21] = op_sub;
	opcodeJumptable[0x22] = op_mul;
	opcodeJumptable[0x23] = op_div;
	opcodeJumptable[0x24] = op_mod;
	opcodeJumptable[0x25] = op_and;
	opcodeJumptable[0x26] = op_or;
	opcodeJumptable[0x27] = op_xor;
	opcodeJumptable[0x28] = op_not;
	opcodeJumptable[0x29] = op_shl;
	opcodeJumptable[0x2A] = op_shr;
	opcodeJumptable[0x2B] = op_sar;
	opcodeJumptable[0x30] = op_eq;
	opcodeJumptable[0x31] = op_neq;
	opcodeJumptable[0x32] = op_leq;
	opcodeJumptable[0x33] = op_geq;
	opcodeJumptable[0x34] = op_le;
	opcodeJumptable[0x35] = op_ge;
	opcodeJumptable[0x38] = op_dnotzero;
	opcodeJumptable[0x39] = op_dnotzero2;
	opcodeJumptable[0x3A] = op_iszero;
	opcodeJumptable[0x40] = op_ternary;
	opcodeJumptable[0x42] = op_muldiv;
	opcodeJumptable[0x48] = op_sin;
	opcodeJumptable[0x49] = op_cos;
	opcodeJumptable[0x60] = op_memcpy;
	opcodeJumptable[0x61] = op_memclr;
	opcodeJumptable[0x62] = op_memset;
	opcodeJumptable[0x63] = op_memcmp;
	opcodeJumptable[0x67] = op_strreplace;
	opcodeJumptable[0x68] = op_strlen;
	opcodeJumptable[0x69] = op_streq;
	opcodeJumptable[0x6A] = op_strcpy;
	//opcodeJumptable[0x80] = op_sys0;
}
