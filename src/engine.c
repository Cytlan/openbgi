#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include "engine.h"
#include "renderer.h"
#include "golden_log.h"
#include "os.h"

void Engine_Init(Engine_t* engine)
{
	engine->threadCounter = 2;
	engine->programCounter = 0;
	engine->threads = NULL;
	engine->programs = NULL;
	engine->memory = NULL;
	for(int i = 0; i < 48; i++)
		engine->auxMemory[i] = NULL;
	engine->globalBufferSize = 0;
	engine->globalMem = NULL;
	engine->windowObjectHandle = 0xC0000000;
	engine->filterObjectHandle = 0x90000000;
	engine->spriteObjectHandle = 0x80000000;
	engine->knobObjectHandle = 0xE0000000;
	engine->nextThreadRequest = 0;
	engine->renderer = Renderer_Init(engine);
	engine->window = NULL;
	printf("[Engine]: Engine initialised\n");
}

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
	thread->stackMemoryConfig.mem = (uint8_t*)malloc(stackSize * sizeof(uint32_t));
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
	thread->ticks = 0;
	thread->error = 0;
	thread->engine = engine;
	thread->opcode = 0;
	thread->waitTicks = 0;
	thread->queuePush = 0;

	thread->silenceBasicOpcodeLog = 1;
	thread->silenceYield = 1;

	printf("[Engine]: Created thread %d (stack: 0x%.8X, code: 0x%.8X, memory: 0x%.8X)\n", thread->threadId, thread->stackSize, thread->codeSize, thread->localMemSize);

	return thread;
}

char* Engine_SearchForArchive(const char* archive)
{
	char arcName[256];
	strcpy(arcName, archive);
	for(int i = 0; arcName[i] != '\0'; i++)
		arcName[i] = tolower(arcName[i]);
	char path[256];
    DIR *dir;
    struct dirent *entry;

    dir = opendir(".");
    if(dir == NULL)
    {
        perror("[Engine]");
        return NULL;
    }

    int found = 0;
    while((entry = readdir(dir)) != NULL)
    {
        char* name = entry->d_name;
        int i;
		for(i = 0; name[i] != '\0'; i++)
			path[i] = tolower(name[i]);
		path[i] = 0;
		if(strcmp(path, arcName) == 0)
		{
			strcpy(path, name);
			found = 1;
			break;
		}
		int pi = i;
		path[i++] = '.';
		path[i++] = 'a';
		path[i++] = 'r';
		path[i++] = 'c';
		path[i++] = 0;
		if(strcmp(path, arcName) == 0)
		{
			path[pi] = 0;
			strcpy(path, name);
			found = 1;
			break;
		}
    }
    closedir(dir);
    if(found)
    {
    	char* pathname = (char*)malloc(strlen(path) + 1);
    	strcpy(pathname, path);
    	return pathname;
    }
    return NULL;
}

char* Engine_SearchForFile(const char* archive, const char* filename)
{
	char* arcPath = Engine_SearchForArchive(archive);
	if(arcPath == NULL)
		return NULL;

    DIR *dir;
    struct dirent *entry;

    dir = opendir(arcPath);
    if(dir == NULL)
    {
        perror("[Engine]");
        free(arcPath);
        return NULL;
    }

    char fileName[256];
	strcpy(fileName, filename);
	for(int i = 0; fileName[i] != '\0'; i++)
		fileName[i] = tolower(fileName[i]);

	char path[256];
	int found = 0;
    while((entry = readdir(dir)) != NULL)
    {
        char* name = entry->d_name;
        int i;
		for(i = 0; name[i] != '\0'; i++)
			path[i] = tolower(name[i]);
		path[i] = 0;

		if(strcmp(path, fileName) == 0)
		{
			strcpy(path, name);
			found = 1;
			break;
		}
    }
    closedir(dir);
    if(found)
    {
		char fullpath[256];
		int cx = snprintf(fullpath, 256, "%s/%s", arcPath, path);
		free(arcPath);
		if(cx < 0 || cx > 256)
		{
			// TODO: Error
			return NULL;
		}

		char* finalpath = (char*)malloc(strlen(fullpath) + 1);
		strcpy(finalpath, fullpath);
		return finalpath;
	}
	else
	{
		free(arcPath);
		return NULL;
	}
}

uint8_t* Engine_ReadFile(Engine_t* engine, const char* archive, const char* filename, size_t* outSize)
{
	printf("[Engine]: Attempting to read file \"%s\" from archive \"%s\"\n", filename, archive);
	char* path = Engine_SearchForFile(archive, filename);
	if(!path)
	{
		printf("[Engine]: Failed to find file \"%s\" from archive \"%s\"\n", filename, archive);
		return 0;
	}
	printf("[Engine]: Found file at \"%s\"\n", path);

	FILE* f = fopen(path, "rb");
	if(f == NULL)
	{
		// TODO: Error
		perror("Error");
		free(path);
		return NULL;
	}
	if(fseek(f, 0, SEEK_END) != 0)
	{
		// TODO: Error
		perror("Error");
		free(path);
		fclose(f);
		return NULL;
	}
	size_t size = ftell(f);
	if(fseek(f, 0, SEEK_SET) != 0)
	{
		// TODO: Error
		perror("Error");
		free(path);
		fclose(f);
		return NULL;
	}
	uint8_t* file = (uint8_t*)malloc(size);
	if(file == NULL)
	{
		// TODO: Error
		fclose(f);
		free(path);
		return NULL;
	}
	if(fread(file, 1, size, f) != size)
	{
		// TODO: Error
		perror("Error");
		free(file);
		free(path);
		fclose(f);
		return NULL;
	}
	fclose(f);
	printf("[Engine]: Read file \"%s\"\n", path);
	free(path);
	if(outSize != NULL)
		*outSize = size;
	return file;
}

uint32_t Engine_LoadProgram(Engine_t* engine, const char* archive, const char* filename, uint32_t stackSize, uint32_t codeSize, uint32_t memorySize)
{
	printf("[Engine]: Loading program \"%s\" from archive \"%s\"\n", filename, archive);
	Thread_t* thread = Engine_CreateThread(engine, stackSize, codeSize, memorySize);
	size_t fileSize;
	uint8_t* code = Engine_ReadFile(engine, archive, filename, &fileSize);
	if(code == NULL)
		return 1;
	Thread_LoadCode(thread, code, filename);
	free(code);

	return thread->threadId;
}

uint32_t Engine_ReadFileToMemory(Engine_t* engine, const char* archive, const char* filename, uint8_t* buffer)
{
	printf("[Engine]: Attempting to read file \"%s\" from archive \"%s\" into memory\n", filename, archive);
	
	char* path = Engine_SearchForFile(archive, filename);
	if(!path)
	{
		printf("[Engine]: Failed to find file \"%s\" from archive \"%s\"\n", filename, archive);
		return 0;
	}
	printf("[Engine]: Found file at \"%s\"\n", path);

	FILE* f = fopen(path, "rb");
	if(f == NULL)
	{
		// TODO: Error
		free(path);
		perror("[Engine]");
		return 0;
	}
	if(fseek(f, 0, SEEK_END) != 0)
	{
		// TODO: Error
		free(path);
		perror("[Engine]");
		fclose(f);
		return 0;
	}
	size_t size = ftell(f);
	if(fseek(f, 0, SEEK_SET) != 0)
	{
		// TODO: Error
		free(path);
		perror("[Engine]");
		fclose(f);
		return 0;
	}
	if(fread(buffer, 1, size, f) != size)
	{
		// TODO: Error
		free(path);
		perror("[Engine]");
		fclose(f);
		return 0;
	}
	fclose(f);
	printf("[Engine]: Read file \"%s\" into memory\n", path);
	free(path);
	return size;
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

void Engine_Sleep(int microseconds)
{
	struct timespec ts;
    ts.tv_sec = microseconds / 1000000;
    ts.tv_nsec = (microseconds % 1000000) * 1000;
    nanosleep(&ts, NULL);
}

int totalTicks = 0;
void Engine_Execute(Engine_t* engine)
{
	engine->isRunning = 1;

	Thread_t* thread = engine->threads;
	Thread_t* nextThread;
	int lastSleep = 0;
	while(engine->isRunning)
	{
		OS_Poll();

		if(GoldenLogTotal)
		{
			if(lastSleep == 0)
				lastSleep = GoldenLog[GoldenLogIndex].time;
			else
			{
				int delta = GoldenLog[GoldenLogIndex].time - lastSleep;
				if(delta > 20000)
				{
					Renderer_DrawScreen(engine->renderer);
					lastSleep = GoldenLog[GoldenLogIndex].time;
					Engine_Sleep(delta);
				}
			}
		}

		if(thread->waitTicks == 0)
			Engine_ExecuteThread(engine, thread->threadId, 1);
		else
			thread->waitTicks--;
		if(engine->nextThreadRequest)
		{
			nextThread = Engine_GetThreadById(engine, engine->nextThreadRequest);
			engine->nextThreadRequest = 0;
		}
		else
			nextThread = Engine_GetThreadById(engine, thread->threadId + 1);
		if(nextThread == NULL)
			nextThread = Engine_GetThreadById(engine, 2);
		thread = nextThread;
	}
	printf("[Engine]: Engine stopped. Executed %d ticks...\n", totalTicks);
}

void Engine_ExecuteThread(Engine_t* engine, uint32_t threadId, int ticks)
{
	Thread_t* thread = Engine_GetThreadById(engine, threadId);
	int runSteps = ticks; //81944;
	int steps = runSteps;
	thread->running = 1;


	//printf("[Engine]: Running %d instructions...\n", steps);
	while(steps && thread->running)
	{
		// Delayed push from async execution
		while(thread->queuePush)
		{
			thread->queuePush--;
			Thread_PushStack(thread, thread->queuePushQueue[thread->queuePush]);
		}

		uint32_t res = Thread_Execute(thread);
		if(res == 0xFFFFFFFF)
		{
			printf("[Engine]: Stub opcode encountered. Stopping.\n");
			engine->isRunning = 0;
			break;
		}
		if(res == 0xFFFFFFFE)
		{
			printf("[Engine]: Golden log mismatch encountered. Stopping.\n");
			engine->isRunning = 0;
			break;
		}
		if(res == 0xFFFFFFFD)
		{
			printf("[Engine]: Unknown opcode encountered. Stopping.\n");
			engine->isRunning = 0;
			break;
		}
		if(res == 0xFFFFFFFC)
		{
			printf("[Engine]: Error encountered. Stopping.\n");
			engine->isRunning = 0;
			break;
		}
		if(res != 0 && res != 1 && res != 2 && res != 3)
		{
			printf("[Engine]: Non-zero result from opcode. Stopping.\n");
			engine->isRunning = 0;
			break;
		}
		if(res == 2)
		{
			// Simulate async if we have golden log
			if(GoldenLogTotal)
			{
				int idx = GoldenLogIndex;
				int repeatThread = GoldenLog[GoldenLogIndex].thread;
				int ticks = 0;
				idx++;
				while(idx < GoldenLogTotal)
				{
					if(GoldenLog[idx].type != LOG_TYPE_EXEC)
					{
						idx++;
						continue;
					}
					int cThread = GoldenLog[idx].thread;
					if(cThread == repeatThread)
						ticks++;
					if(cThread == threadId)
						break;
					idx++;
				}
				thread->waitTicks = ticks - 1;
				printf("[Engine]: Sleeping thread for %d ticks.\n", ticks);
			}
		}
		if(res == 1 || res == 3)
		{
			break;
		}
		steps--;
		totalTicks++;
	}
	//printf("[Engine]: Ran %d instructions; Yielding at tick %d...\n", runSteps - steps, thread->ticks);
}

uint32_t gUnknownVal001 = 0;
void SetGlobalUnknownVal001(uint32_t value)
{
	gUnknownVal001 = value;
}

int Engine_InitGlobalMemory(Engine_t* engine, uint32_t level)
{
	if(level < 0 || level >= 13)
	{
		printf("[Engine]: Error: Attempted to initialise global memory with an invalid level of %d\n", level);
		return 0;
	}

	uint32_t bufferSize = 0x1000 << level; // 4096 * (1 << level)

	engine->globalBufferSize = bufferSize;

	if(engine->globalMem != NULL)
	{
		printf("[Engine]: Freeing previous global memory\n");
		free(engine->globalMem);
	}

	engine->globalMem = (uint8_t*)calloc(bufferSize, sizeof(uint8_t));

	printf("[Engine]: Initialised global memory with size 0x%.8X\n", bufferSize);

	return 1;
}

uint32_t Engine_AllocAuxMemory(Engine_t* engine, uint32_t size)
{
	if(size > 0x2000000)
	{
		printf("[Engine]: Attempting to allocate too much aux memory\n");
		engine->isRunning = 0;
		return 0;
	}
	for(int i = 0; i < 48; i++)
	{
		if(engine->auxMemory[i] != NULL)
			continue;
		engine->auxMemory[i] = (uint8_t*)malloc(size);
		printf("[Engine]: Initialised aux memory in slot %d with size 0x%.8X\n", i, size);
		return (i + 32) * 0x2000000;
	}
	return 0;
}

uint8_t* Engine_GetAuxMemory(Engine_t* engine, uint8_t slot)
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
	//ListNode_t* node = (ListNode_t*)malloc(sizeof(ListNode_t));
	//node->data1 = value1;
	//node->data2 = value2;
	//node->data3 = value3;
	//node->next = NULL;
	//if(gLinkedListTailNext)
	//	gLinkedListTailNext->next = node;
	//gLinkedListTailNext = node;

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

bool Str_IsDoubleByteSJIS(char c)
{
	if((c < 0x80 || c > 0x9F) && c < 0xE0)
		return false;
	return true;
}

void Str_StrToLowerCase(char* ptr)
{
	char c = *ptr;
	while(c)
	{
		if(Str_IsDoubleByteSJIS(c))
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

	if(engine->globalMem != NULL)
	{
		free(engine->globalMem);
		engine->globalMem = NULL;
	}

	for(int i = 0; i < 48; i++)
	{
		if(engine->auxMemory[i])
		{
			free(engine->auxMemory[i]);
			engine->auxMemory[i] = NULL;
		}
	}

	Renderer_Free(engine->renderer);

	while(gSearchPaths)
	{
		SearchPathNode_t* next = gSearchPaths->next;
		free(gSearchPaths->path);
		free(gSearchPaths);
		gSearchPaths = next;
	}
}
