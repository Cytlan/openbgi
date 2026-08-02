#ifndef ENGINE_H_
#define ENGINE_H_

#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "thread.h"

typedef struct Renderer Renderer_t;
typedef struct Engine Engine_t;
struct Engine
{
	uint32_t threadCounter;
	uint32_t programCounter;
	Thread_t* threads;
	Program_t* programs;
	Memory_t* memory;
    uint8_t* auxMemory[48];
    uint32_t globalBufferSize;
    uint8_t* globalMem;
    int isRunning;

    uint32_t windowObjectHandle;
    uint32_t filterObjectHandle;
    uint32_t spriteObjectHandle;
    uint32_t knobObjectHandle;

    uint32_t nextThreadRequest;

    Renderer_t* renderer;
    SDL_Window* window;
};

extern Engine_t* gEngine;

uint32_t Engine_LoadProgram(Engine_t* engine, const char* archive, const char* filename, uint32_t stackSize, uint32_t codeSize, uint32_t memorySize);
Thread_t* Engine_CreateThread(Engine_t* engine, uint32_t stackSize, uint32_t codeSize, uint32_t memorySize);
uint8_t* Engine_ReadFile(Engine_t* engine, const char* archive, const char* filename, size_t* outSize);
uint32_t Engine_ReadFileToMemory(Engine_t* engine, const char* archive, const char* filename, uint8_t* buffer);
void Engine_Execute(Engine_t* engine);
void Engine_ExecuteThread(Engine_t* engine, uint32_t threadId, int ticks);
Thread_t* Engine_GetThreadById(Engine_t* engine, uint32_t threadId);
void Engine_Free(Engine_t* engine);
void Engine_Init(Engine_t* engine);

extern uint32_t gUnknownVal001;
void SetGlobalUnknownVal001(uint32_t value);

int Engine_InitGlobalMemory(Engine_t* engine, uint32_t level);

uint32_t Engine_AllocAuxMemory(Engine_t* engine, uint32_t size);
uint8_t* Engine_GetAuxMemory(Engine_t* engine, uint8_t slot);

extern uint32_t gFrameTimeMs;
extern uint32_t gFrameTimer;
void Engine_SetFramerateTime(uint32_t fps);

extern int gAntiAliasing1;
extern int gAntiAliasing2;
extern int gAntiAliasing3;
void Engine_SetAntialiasingLevel(int level);

extern int gCursorShape;
extern int gFlagUnknown10;
void Engine_SetFlagUnknown10(int value);

typedef struct SearchPathNode SearchPathNode_t;
struct SearchPathNode
{
    char*             path;
    SearchPathNode_t* next;
};
extern SearchPathNode_t* gSearchPaths;
void Engine_AddSearchPath(char* path);

extern int gEnableSearchPaths;
void Engine_SetEnableSearchPaths(int value);

extern int gFlagUnknown2;
extern int gFlagUnknown3;
extern int gFlagUnknown4;
void Engine_SetFlagUnknown1to4(int value);

extern int gFlagUnknown20;
void Engine_SetFlagUnknown20(int value);

extern int gUnknownGrp0Val1;
extern int gUnknownGrp0Val2;
void Engine_SetUnknownGrp0Val1and2(int value1, int value2);

extern int gFlagUnknown21;
void Engine_SetFlagUnknown21(int value);

typedef struct ListNode ListNode_t;
struct ListNode
{
    uint32_t data1;
    uint32_t data2;
    uint32_t data3;
    ListNode_t* next;
};

extern ListNode_t* gLinkedListHead;
extern ListNode_t* gLinkedListTailNext;
extern ListNode_t  gLinkedListSentinel;
void Engine_PushGlobalList(uint32_t value1, uint32_t value2, uint32_t value3);
int Engine_PopGlobalList(uint32_t* output);

extern int gSomethingToDoWithKeylots;
extern int gKeySlots[16];
int Engine_SetKeySlots(int value, int* keys);

typedef struct ThreadNode ThreadNode_t;
struct ThreadNode
{
    uint32_t threadId;
    Thread_t* thread;
    ThreadNode_t* next;
};
extern ThreadNode_t* gThreadList;
extern int gThreadListCount;
uint32_t Engine_AddThreadToList(Thread_t* thread);
Thread_t* Engine_GetThreadFromListById(uint32_t threadId);

int Engine_PlaySound(char* path);

bool Str_IsDoubleByteSJIS(char c);
void Str_StrToLowerCase(char* ptr);

#endif