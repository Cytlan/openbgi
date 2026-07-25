#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"

Thread_t* Engine_CreateThread(Engine_t* engine, uint32_t stackSize, uint32_t codeSize, uint32_t memorySize)
{
	Thread_t* thread = (Thread_t*)malloc(sizeof(Thread_t));
	thread->previousThread = engine->threads;
	engine->threads = thread;

	thread->programId = 0;
	thread->threadId = engine->threadCounter++;
	thread->flags = 0;
	thread->stackPointer = 0;
	thread->instructionPointer = 0;
	thread->nextInstructionPointer = 0;
	thread->basePointer = 0;
	thread->stackSize = stackSize;
	thread->stackMemoryConfig.isAllocated = 1;
	thread->stackMemoryConfig.mem = (uint8_t*)malloc(stackSize);
	thread->stackMemoryConfig.size = stackSize;
	thread->stack = (uint32_t*)thread->stackMemoryConfig.mem;
	thread->codeSize = codeSize;
	thread->codeMemoryConfig.isAllocated = 1;
	thread->codeMemoryConfig.mem = (uint8_t*)malloc(codeSize);
	thread->codeMemoryConfig.size = codeSize;
	thread->code = thread->codeMemoryConfig.mem;
	thread->programs = NULL;
	thread->programCount = 0;
	thread->codeSpaceUsed = 0;
	thread->localMemSize = memorySize;
	thread->localMemConfig.isAllocated = 1;
	thread->localMemConfig.mem = (uint8_t*)malloc(memorySize);
	thread->localMemConfig.size = memorySize;
	thread->localMem = thread->localMemConfig.mem;

	thread->level = 0;
	thread->running = 0;

	printf("[Engine]: Created thread %d (stack: 0x%.8X, code: 0x%.8X, memory: 0x%.8X)\n", thread->threadId, thread->stackSize, thread->codeSize, thread->localMemSize);

	return thread;
}

uint8_t* Engine_ReadFile(Engine_t* engine, const char* archive, const char* filename)
{
	printf("[Engine]: Attempting to read file \"%s\" from archive \"%s\"\n", filename, archive);
	char path[256];
	int cx = snprintf(path, 256, "%s/%s", archive, filename);
	if(cx < 0 || cx > 256)
	{
		// TODO: Error
		return NULL;
	}
	FILE* f = fopen(path, "rb");
	if(f == NULL)
	{
		// TODO: Error
		perror("Error");
		return NULL;
	}
	if(fseek(f, 0, SEEK_END) != 0)
	{
		// TODO: Error
		perror("Error");
		fclose(f);
		return NULL;
	}
	size_t size = ftell(f);
	if(fseek(f, 0, SEEK_SET) != 0)
	{
		// TODO: Error
		perror("Error");
		fclose(f);
		return NULL;
	}
	uint8_t* file = (uint8_t*)malloc(size);
	if(file == NULL)
	{
		// TODO: Error
		fclose(f);
		return NULL;
	}
	if(fread(file, 1, size, f) != size)
	{
		// TODO: Error
		perror("Error");
		free(file);
		fclose(f);
		return NULL;
	}
	fclose(f);
	printf("[Engine]: Read file \"%s\"\n", path);
	return file;
}

uint32_t Engine_LoadProgram(Engine_t* engine, const char* archive, const char* filename, uint32_t stackSize, uint32_t codeSize, uint32_t memorySize)
{
	printf("[Engine]: Loading program \"%s\" from archive \"%s\"\n", filename, archive);
	Thread_t* thread = Engine_CreateThread(engine, stackSize, codeSize, memorySize);
	uint8_t* code = Engine_ReadFile(engine, archive, filename);
	if(code == NULL)
		return 1;
	Thread_LoadCode(thread, code, filename);
	free(code);

	return thread->threadId;
}

Thread_t* Engine_GetThreadById(Engine_t* engine, uint32_t threadId)
{
	Thread_t* thread = engine->threads;
	while(thread)
	{
		if(thread->threadId == threadId)
			return thread;
		thread = thread->previousThread;
	}
	return NULL;
}

void Engine_ExecuteThread(Engine_t* engine, uint32_t threadId)
{
	Thread_t* thread = Engine_GetThreadById(engine, threadId);
	int runSteps = 3000;
	int steps = runSteps;
	thread->running = 1;
	printf("[Engine]: Running %d instructions...\n", steps);
	while(steps && thread->running)
	{
		uint32_t res = Thread_Execute(thread);
		if(res == 0xFFFFFFFF)
		{
			printf("[Engine]: Stub opcode encountered. Stopping.\n");
			break;
		}
		if(res != 0)
		{
			printf("[Engine]: Non-zero result from opcode. Stopping.\n");
			break;
		}
		steps--;
	}
	printf("[Engine]: Ran %d instructions; Exiting...\n", runSteps - steps);
}

void Engine_Free(Engine_t* engine)
{
	printf("[Engine]: Freeing memory\n");
	Thread_t* thread = engine->threads;
	while(thread)
	{
		Thread_t* nextThread = thread->previousThread;
		if(thread->stackMemoryConfig.isAllocated)
			free(thread->stackMemoryConfig.mem);
		if(thread->codeMemoryConfig.isAllocated)
			free(thread->codeMemoryConfig.mem);
		if(thread->localMemConfig.isAllocated)
			free(thread->localMemConfig.mem);
		Program_t* program = thread->programs;
		while(program)
		{
			Program_t* nextProgram = program->previousProgram;
			free(program->filename);
			free(program);
			program = nextProgram;
		}
		free(thread);
		thread = nextThread;
	}
	engine->threads = NULL;

	if(gGlobalMem != NULL)
		free(gGlobalMem);
	gGlobalMem = NULL;

	while(gSearchPaths)
	{
		SearchPathNode_t* next = gSearchPaths->next;
		free(gSearchPaths->path);
		free(gSearchPaths);
		gSearchPaths = next;
	}
}

void Engine_Init(Engine_t* engine)
{
	engine->threadCounter = 0;
	engine->programCounter = 0;
	engine->threads = NULL;
	engine->programs = NULL;
	engine->memory = NULL;
	printf("[Engine]: Engine initialised\n");
}

uint32_t gUnknownVal001 = 0;
void SetGlobalUnknownVal001(uint32_t value)
{
	gUnknownVal001 = value;
}

uint32_t gGlobalBufferSize = 0;
uint8_t* gGlobalMem = NULL;
int InitGlobalMemory(uint32_t level)
{
	if(level < 0 || level >= 13)
	{
		printf("[Engine]: Error: Attempted to initialise global memory with an invalid level of %d\n", level);
		return 0;
	}

	uint32_t bufferSize = 0x1000 << level; // 4096 * (1 << level)

	gGlobalBufferSize = bufferSize;

	if(gGlobalMem != NULL)
	{
		printf("[Engine]: Freeing previous global memory\n");
		free(gGlobalMem);
	}

	gGlobalMem = (uint8_t*)calloc(bufferSize, sizeof(uint8_t));

	printf("[Engine]: Initialised global memory with size 0x%.8X\n", bufferSize);

	return 1;
}

uint8_t* Engine_GetAuxMemory(uint8_t slot)
{
	if(slot >= 48)
		return NULL;
	return NULL;
}

uint32_t gFrameTimeMs = 0;
uint32_t gFrameTimer = 0;
void Engine_SetFramerateTime(uint32_t fps)
{
	if(fps == 0)
		gFrameTimeMs = 1;
	else
	{
		gFrameTimeMs = 1000U / fps;

		if(gFrameTimeMs == 0)
			gFrameTimeMs = 1;
	}

	printf("[Engine]: Set framerate to %d (%d ms)\n", fps, gFrameTimeMs);

	gFrameTimer = 0;
}

int gAntiAliasing1 = 0;
int gAntiAliasing2 = 0;
int gAntiAliasing3 = 0;
void Engine_SetAntialiasingLevel(int level)
{
	if(level > 4)
		return;

	switch(level)
	{
		case 0:
			gAntiAliasing1 = 1;
			gAntiAliasing2 = 2;
			gAntiAliasing3 = 2;
			return;
		case 1:
			gAntiAliasing1 = 2;
			gAntiAliasing2 = 4;
			gAntiAliasing3 = 4;
			return;
		case 2:
			gAntiAliasing2 = 6;
			gAntiAliasing3 = 8;
			gAntiAliasing1 = 3;
			return;
		case 3:
			gAntiAliasing2 = 8;
			gAntiAliasing3 = 0x10;
			gAntiAliasing1 = 4;
			return;
		default:
			gAntiAliasing2 = 0;
			gAntiAliasing3 = 1;
			gAntiAliasing1 = 0;
	}

	printf("[Engine]: Set antialiasing level to %d\n", level);

	return;
}

int gCursorShape = 0;
int gFlagUnknown10 = 0;
void Engine_SetFlagUnknown10(int value)
{
	gFlagUnknown10 = value;
	printf("[Engine]: Set FlagUnknown10 to %d\n", value);
}

int gEnableSearchPaths = 0;
void Engine_SetEnableSearchPaths(int value)
{
	gEnableSearchPaths = value;
	printf("[Engine]: Set EnableSearchPaths to %d\n", value);
}

int gFlagUnknown2 = 0;
int gFlagUnknown3 = 0;
int gFlagUnknown4 = 0;
void Engine_SetFlagUnknown1to4(int value)
{
	gFlagUnknown2 = value;
	gFlagUnknown3 = value;
	gFlagUnknown4 = value;
	printf("[Engine]: Set FlagUnknown2 to %d\n", value);
	printf("[Engine]: Set FlagUnknown3 to %d\n", value);
	printf("[Engine]: Set FlagUnknown4 to %d\n", value);
}

SearchPathNode_t* gSearchPaths = NULL;
void Engine_AddSearchPath(char* path)
{
	if(path == NULL)
		return;

	SearchPathNode_t* node = (SearchPathNode_t*)malloc(sizeof(SearchPathNode_t));

	size_t len = strlen(path) + 1;
	char* copy = (char*)malloc(len);

	node->path = copy;
	node->next = gSearchPaths;

	memcpy(copy, path, len);

	gSearchPaths = node;

	printf("[Engine]: Added search path \"%s\"\n", copy);
}

int gFlagUnknown20 = 0;
void Engine_SetFlagUnknown20(int value)
{
	gFlagUnknown20 = value;
}

int gUnknownGrp0Val1 = 0;
int gUnknownGrp0Val2 = 0;
void Engine_SetUnknownGrp0Val1and2(int value1, int value2)
{
	gUnknownGrp0Val1 = value1;
	gUnknownGrp0Val2 = value2;
}

int gFlagUnknown21 = 0;
void Engine_SetFlagUnknown21(int value)
{
	gFlagUnknown21 = value;
}

ListNode_t* gLinkedListHead;
ListNode_t* gLinkedListTailNext;
ListNode_t  gLinkedListSentinel = {
	.data1 = 0,
    .data2 = 0,
    .data3 = 0,
    .next = NULL
};
void Engine_PushGlobalList(uint32_t value1, uint32_t value2, uint32_t value3)
{
	ListNode_t* node = (ListNode_t*)malloc(sizeof(ListNode_t));
	node->data1 = value1;
	node->data2 = value2;
	node->data3 = value3;
	node->next = NULL;
	gLinkedListTailNext->next = node;
	gLinkedListTailNext = node;

}
int Engine_PopGlobalList(uint32_t* output)
{
    ListNode_t* node = gLinkedListHead;
    if(node == NULL)
        return 0;

    if(node->next == NULL)
        gLinkedListTailNext = (ListNode_t*)&gLinkedListSentinel;

    output[0] = node->data1;
    output[1] = node->data2;
    output[2] = node->data3;

    gLinkedListHead = node->next;
    free(node);

    return 1;
}

int gSomethingToDoWithKeylots = 0;
int gKeySlots[16] = { 0 };
int Engine_SetKeySlots(int value, int* keys)
{
    gSomethingToDoWithKeylots = value;

    if (value != 0) {
        if (keys == NULL)
        {
            gKeySlots[0] = 0;
        }
        else
        {
            // Count elements until the terminating 0
            int numSlots = 0;
            while(keys[numSlots] != 0)
                numSlots++;

            if(numSlots > 15)
                return 0;

            // Copy slots + terminating sentinel
            for(int i = 0; i <= numSlots; i++)
                gKeySlots[i] = keys[i];
            return 1;
        }
    }
    return 1;
}

ThreadNode_t* gThreadList = NULL;
int gThreadListCount = 0;
uint32_t Engine_AddThreadToList(Thread_t* thread)
{
	//ThreadNode_t* node = operator_new(0xc);
	//uint32_t threadId = Thread_GetThreadID(thread);
	//node->threadId = threadId;
	//node->thread = thread;
	//node->next = gThreadList;
	//gThreadList = node;
	//gThreadListCount++;
	//return node->threadId;
	return 0;
}
Thread_t* Engine_GetThreadFromListById(uint32_t threadId)
{
	ThreadNode_t* node = gThreadList;
	while(node)
	{
		if(node->threadId == threadId)
			return node->thread;
		node = node->next;
	}
	return NULL;
}

int Engine_PlaySound(char* path)
{
	printf("Play sound: %s\n", path);
	//return PlaySoundA(path, gHinstance, SND_ASYNC | SND_NODEFAULT | SND_NOWAIT | SND_FILENAME)
	return 1;
}
