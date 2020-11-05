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

typedef struct VMMemory {
    int isAllocated;
    uint8_t* mem;
    int size;
} VMMemory_t;

typedef struct VMThread {
    uint32_t programId;
    uint32_t threadId;
    struct VMThread* prevVMState;
    uint32_t flags;
    uint32_t stackSize_;
    uint32_t instructionPointer;
    uint32_t programCounter;
    uint32_t basePointer;
    uint32_t stackSize;
    struct VMMemory* stackMemConfig;
    uint32_t* stack;
    uint32_t codeSpaceSize;
    struct VMMemory* codeSpaceMemConfig;
    uint8_t* codeSpace;
    struct VMProgramList * programList;
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

typedef struct VMUnknownStruct {
    int field_0x0;
    uint8_t * buf;
    int field_0x8;
    int field_0xc;
    struct VMMemory * mem;
    int field_0x14;
    int field_0x18;
    int field_0x1c;
} VMUnknownStruct_t;

typedef struct VMProgramList {
    char* filename;
    int size;
    int location;
    struct VMProgramList* prevProgram;
} VMProgramList_t;

int(**opcodeJumptable)(VMThread_t* vmThread) = (int(**)(VMThread_t*))0x00488420;

// This should be called as soon as possible in WinMain
extern "C" __declspec(dllexport) void init()
{
	
}

extern "C" __declspec(dllexport) int executeOpcode(int opcode, VMThread_t* vmThread)
{
	printf("Opcode: %.2x\n", opcode);

	int res = opcodeJumptable[opcode](vmThread);

	return res;
}
