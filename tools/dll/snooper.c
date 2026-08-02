#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>

#define G_NR

#ifdef G_TAYUTAMA
#define POP_ADDRESS            0x0042e4ee // pop stack (before ECX gets overwritten)
#define PUSH_ADDRESS           0x0042e521 // push stack (end of function)
#define RESOLVE_ADDR           0x00452530 // resolve addr

#define PATCH_ADDRESS          0x004519cd // address where basic opcodes are called
#define JUMPTABLE_ADDRESS      0x00488420 // basic opcodes
#define RETURN_AFTER_CALL_ADDR 0x004519d4 // addr to jump to after opcode call
#endif

#ifdef G_NR
#define POP_ADDRESS            0x0041fc10 // 0x0041FC1E // pop stack (before ECX gets overwritten)
#define PUSH_ADDRESS           0x0041fc30 // 0x0041FC51 // push stack (end of function)
#define RESOLVE_ADDR           0x0043b560 // resolve addr

#define PATCH_ADDRESS          0x0043AA8E // address where basic opcodes are called
#define JUMPTABLE_ADDRESS      0x0046C2A4 // basic opcodes
#define RETURN_AFTER_CALL_ADDR 0x0043AA95 // addr to jump to after opcode call


#define VM_READ_PC_8           0x0041fb60 // VMThread_ReadPC8

#define VM_READ_CODE_8         0x0041fb80 // VMThread_ReadCode8
#define VM_READ_CODE_16        0x0041fb90 // VMThread_ReadCode16
#define VM_READ_CODE_32        0x0041fbb0 // VMThread_ReadCode32

#define VM_GLOBAL_MEM          0x0048e3ec
#define VM_GLOBAL_MEM_SIZE     0x0048e3f0

#endif

#define STRINGIFY(x) #x
#define TOSTRING(x)  STRINGIFY(x)


int threadTicks[20] = { 0 };
int threadSpacial[20] = { 0 };
int threadMemHooked[20] = { 0 };
void* memHooksStart[20] = { 0 };
size_t memHooksSize[20] = { 0 };
void* memHooksStartReal[20] = { 0 };
size_t memHooksSizeReal[20] = { 0 };
int memHooksType[20] = { 0 };
int memHooks = 0;
FILE* threadLogs[100] = { NULL };
FILE* memoryLogFile;
LARGE_INTEGER countFrequency;
LARGE_INTEGER countStart;

uint32_t** gGlobalMemory = (uint32_t**)VM_GLOBAL_MEM;
uint32_t* gGlobalMemorySize = (uint32_t*)VM_GLOBAL_MEM_SIZE;

/* ------------------------------------------------------------------ */
/* Global region                                                      */
void*  g_region      = NULL;
size_t g_region_size = 0;
void*  g_actual_region      = NULL;
size_t g_actual_region_size = 0;
DWORD  g_old_protect = 0;

/* Serialises VirtualProtect + logging                                */
CRITICAL_SECTION g_cs;

/* Per-thread state – GCC/MinGW TLS                                   */
typedef struct {
    void*  fault_addr;
    void*  instr_addr;
    int    was_write;   /* 0 = read, 1 = write */
    DWORD  value;       /* only used for reads */
    int    active;      /* 1 while waiting for the matching single-step */
    int    ignore;
    void*  addr;
    void*  real;
    int    type;
    int    size;
} ThreadAccessState;

__thread ThreadAccessState t_state;   /* zero-initialised by the loader */

void InitMemoryMonitor();
void AddMemoryMonitor(void* region, size_t size, int type);
LONG CALLBACK VehHandler(PEXCEPTION_POINTERS ep);

/* Keep the handle so we can remove + re-add */
PVOID g_veh_handle = NULL;



static FILE* g_logFile = NULL;

static void Log(const char* fmt, ...)
{
	if(!g_logFile) return;
	va_list args;
	va_start(args, fmt);
	vfprintf(g_logFile, fmt, args);
	vprintf(fmt, args);
	va_end(args);
	fflush(g_logFile);
}

static void Log2(uint32_t threadId, const char* fmt, ...)
{
	//if(threadLogs[threadId] == NULL)
	//{
	//	char filename[255];
	//	sprintf(&filename[0], "vm_trace_thread_%d.log", threadId);
	//	threadLogs[threadId] = fopen(filename, "w");
	//}
	FILE* file = g_logFile; //threadLogs[threadId];
	va_list args;
	va_start(args, fmt);
	vfprintf(file, fmt, args);
	vprintf(fmt, args);
	va_end(args);
	//fflush(file);
}

static void Log3(FILE* file, const char* fmt, ...)
{
	file = g_logFile;
	va_list args;
	va_start(args, fmt);
	vfprintf(file, fmt, args);
	vprintf(fmt, args);
	va_end(args);
	//fflush(file);
}

void ForceVehFirst(void)
{
    if (g_veh_handle)
        RemoveVectoredExceptionHandler(g_veh_handle);

    DWORD old;
    g_veh_handle = AddVectoredExceptionHandler(1, VehHandler);  /* 1 = first */
    //VirtualProtect(g_region, g_region_size, PAGE_NOACCESS, &old);
}

typedef struct _VMThread_t {
	uint32_t programId;
	uint32_t threadId;
	struct _VMThread_t *prevVMState;
	uint32_t flags;
	uint32_t stackPointer;
	uint32_t instructionPointer;
	uint32_t nextInstructionPointer;
	uint32_t basePointer;
	uint32_t stackSize;
	void *stackMemConfig;
	uint32_t *stack;
	uint32_t codeSpaceSize;
	void *codeSpaceMemConfig;
	uint8_t *codeSpace;
	void *programList;
	uint32_t programCount;
	uint32_t codeSpaceTop;
	uint32_t localMemSize;
	void *localMemConfig;
	uint8_t *localMem;
} VMThread_t;

typedef uint8_t* (__cdecl *Thread_ResolveAddr_t)(uint32_t address, VMThread_t* thread);
Thread_ResolveAddr_t Thread_ResolveAddr = (Thread_ResolveAddr_t)RESOLVE_ADDR;

typedef uint32_t (__thiscall *InstructionHandler_t)(VMThread_t* thread);



// --------------------------------------------------------------------
uint8_t __thiscall VMThread_ReadPC8(VMThread_t* this)
{
	this->instructionPointer = this->nextInstructionPointer;
	uint8_t data = this->codeSpace[this->instructionPointer];
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	uint64_t usPassed = (now.QuadPart - countStart.QuadPart) * 1000000 / countFrequency.QuadPart;
	Log2(this->threadId, "r8:  [%lld, %d, %d, %d, 0x%.8X, 0x%.8X]\n", usPassed, this->threadId, threadTicks[this->threadId], 3, data, this->instructionPointer);
	this->nextInstructionPointer = this->nextInstructionPointer + 1;
	return this->codeSpace[this->instructionPointer];
}

// --------------------------------------------------------------------
uint8_t __thiscall VMThread_ReadCode8(VMThread_t* this)
{
	uint8_t data;

	data = this->codeSpace[this->nextInstructionPointer];
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	uint64_t usPassed = (now.QuadPart - countStart.QuadPart) * 1000000 / countFrequency.QuadPart;
	Log2(this->threadId, "rc8:  [%lld, %d, %d, %d, 0x%.8X, 0x%.8X]\n", usPassed, this->threadId, threadTicks[this->threadId], 4, data, this->instructionPointer);
	this->nextInstructionPointer = this->nextInstructionPointer + 1;
	return data;
}

// --------------------------------------------------------------------
uint16_t __thiscall VMThread_ReadCode16(VMThread_t* this)
{
	uint16_t data;

	data = *(uint16_t *)(this->codeSpace + this->nextInstructionPointer);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	uint64_t usPassed = (now.QuadPart - countStart.QuadPart) * 1000000 / countFrequency.QuadPart;
	Log2(this->threadId, "rc16:  [%lld, %d, %d, %d, 0x%.8X, 0x%.8X]\n", usPassed, this->threadId, threadTicks[this->threadId], 5, data, this->instructionPointer);
	this->nextInstructionPointer = this->nextInstructionPointer + 2;
	return data;
}

// --------------------------------------------------------------------
uint32_t __thiscall VMThread_ReadCode32(VMThread_t *this)
{
	uint32_t data;

	data = *(uint32_t *)(this->codeSpace + this->nextInstructionPointer);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	uint64_t usPassed = (now.QuadPart - countStart.QuadPart) * 1000000 / countFrequency.QuadPart;
	Log2(this->threadId, "rc32:  [%lld, %d, %d, %d, 0x%.8X, 0x%.8X]\n", usPassed, this->threadId, threadTicks[this->threadId], 6, data, this->instructionPointer);
	this->nextInstructionPointer = this->nextInstructionPointer + 4;
	return data;
}

// --------------------------------------------------------------------
int __thiscall VMThread_PopStack(VMThread_t *this)
{
	uint32_t uVar1;

	uVar1 = this->stackPointer;
	if (uVar1 == 0) {
		uVar1 = this->stackSize;
	}
	this->stackPointer = uVar1 - 1;

	uint32_t data = this->stack[uVar1 - 1];

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	uint64_t usPassed = (now.QuadPart - countStart.QuadPart) * 1000000 / countFrequency.QuadPart;
	Log2(this->threadId, "pop:  [%lld, %d, %d, %d, 0x%.8X]\n", usPassed, this->threadId, threadTicks[this->threadId], 1, data);
	return data;
}

// --------------------------------------------------------------------
void __thiscall VMThread_PushStack(VMThread_t* this,uint32_t data)
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	uint64_t usPassed = (now.QuadPart - countStart.QuadPart) * 1000000 / countFrequency.QuadPart;
	Log2(this->threadId, "push: [%lld, %d, %d, %d, 0x%.8X]\n", usPassed, this->threadId, threadTicks[this->threadId], 2, data);
	uint32_t uVar1;

	this->stack[this->stackPointer] = data;
	uVar1 = this->stackPointer + 1;
	this->stackPointer = -(uint32_t)(uVar1 < this->stackSize) & uVar1;
	return;
}


// --------------------------------------------------------------------
// --------------------------------------------------------------------

// ==============================
//
// Pop tracing
//
// ==============================

__declspec(naked) void PopHookStub(void) {
	__asm__ (
		".intel_syntax noprefix\n\t"
		//"push ecx\n\t"
		//"call _TracePop\n\t"
		//"add esp, 4\n\t"
		//"ret\n\t"
		"jmp _VMThread_PopStack\n\t"
		".att_syntax prefix\n\t"
	);
	__builtin_unreachable();
}

// ==============================
//
// Push tracing
//
// ==============================

__declspec(naked) void PushHookStub(void)
{
	__asm__ (
		".intel_syntax noprefix\n\t"
		//"push dword ptr [esp + 4]\n\t"
		//"push ecx\n\t"
		//"call _TracePush\n\t"
		//"add esp, 8\n\t"
		//"ret 4\n\t"
		"jmp _VMThread_PushStack\n\t"
		".att_syntax prefix\n\t"
	);
	__builtin_unreachable();
}

__declspec(naked) void ReadPC8Stub(void)
{
	__asm__ (
		".intel_syntax noprefix\n\t"
		//"push dword ptr [esp + 4]\n\t"
		//"push ecx\n\t"
		//"call _TracePush\n\t"
		//"add esp, 8\n\t"
		//"ret 4\n\t"
		"jmp _VMThread_ReadPC8\n\t"
		".att_syntax prefix\n\t"
	);
	__builtin_unreachable();
}

__declspec(naked) void ReadCode8Stub(void)
{
	__asm__ (
		".intel_syntax noprefix\n\t"
		//"push dword ptr [esp + 4]\n\t"
		//"push ecx\n\t"
		//"call _TracePush\n\t"
		//"add esp, 8\n\t"
		//"ret 4\n\t"
		"jmp _VMThread_ReadCode8\n\t"
		".att_syntax prefix\n\t"
	);
	__builtin_unreachable();
}

__declspec(naked) void ReadCode16Stub(void)
{
	__asm__ (
		".intel_syntax noprefix\n\t"
		//"push dword ptr [esp + 4]\n\t"
		//"push ecx\n\t"
		//"call _TracePush\n\t"
		//"add esp, 8\n\t"
		//"ret 4\n\t"
		"jmp _VMThread_ReadCode16\n\t"
		".att_syntax prefix\n\t"
	);
	__builtin_unreachable();
}

__declspec(naked) void ReadCode32Stub(void)
{
	__asm__ (
		".intel_syntax noprefix\n\t"
		//"push dword ptr [esp + 4]\n\t"
		//"push ecx\n\t"
		//"call _TracePush\n\t"
		//"add esp, 8\n\t"
		//"ret 4\n\t"
		"jmp _VMThread_ReadCode32\n\t"
		".att_syntax prefix\n\t"
	);
	__builtin_unreachable();
}

// ==============================
//
// Opcode tracing
//
// ==============================

void print_state(VMThread_t* thread, int state, uint16_t opcode, int opcodeSize)
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	uint64_t usPassed = (now.QuadPart - countStart.QuadPart) * 1000000 / countFrequency.QuadPart;
	Log2(thread->threadId, "exec: [%lld, %d, %d, %d, 0x%.4X, %d, 0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X]\n",
		usPassed,
		thread->threadId,              // 2
		threadTicks[thread->threadId], // 18869
		state,                         // 3
		opcode,                        // 0x0004
		opcodeSize,                    // 1
		thread->programId,             // 0x00000001
		thread->threadId,              // 0x00000002
		thread->prevVMState,           // 0x044A7848
		thread->flags,                 // 0x00000000
		thread->stackPointer,          // 0x00000343
		thread->instructionPointer,    // 0x00000B33
		thread->nextInstructionPointer,        // 0x00000B36
		thread->basePointer,           // 0x00000434
		thread->stackSize,             // 0x00001000
		thread->stackMemConfig,        // 0x01804900
		thread->stack,                 // 0x0180A0E8
		thread->codeSpaceSize,         // 0x00020000
		thread->codeSpaceMemConfig,    // 0x01804920
		thread->codeSpace,             // 0x018C0048
		thread->programList,           // 0x042EE748
		thread->programCount,          // 0x00000002
		thread->codeSpaceTop,          // 0x00001630
		thread->localMemSize,          // 0x00020000
		thread->localMemConfig,        // 0x01804940
		thread->localMem               // 0x018E0050
	);
}

int memHooked = 0;
InstructionHandler_t* jumpTable = (InstructionHandler_t*)JUMPTABLE_ADDRESS;
uint32_t __cdecl TraceInstruction(uint32_t opcode, VMThread_t* thread)
{
	if(threadLogs[thread->threadId] == NULL)
	{
		char filename[255];
		sprintf(&filename[0], "vm_trace_thread_%d.log", thread->threadId);
		threadLogs[thread->threadId] = fopen(filename, "w");
	}

	if(!threadMemHooked[thread->threadId])
	{
		threadMemHooked[thread->threadId] = 1;
		AddMemoryMonitor(thread->localMem, thread->localMemSize, 1);
	}

	uint32_t startCounter = thread->nextInstructionPointer;
	int opcodeSize = 1;
	int dumpMem = 0;
	uint16_t opcode2 = opcode;
	uint8_t subOpcode = 0;

	if(opcode >= 128)
	{
		subOpcode = thread->codeSpace[thread->nextInstructionPointer];
	}

	print_state(thread, 0, opcode2, opcodeSize);
	uint32_t res = 0;

	if(opcode == 0x90 && subOpcode == 0x80)
	{
		thread->stack[thread->stackPointer - 1] = 600;
		thread->stack[thread->stackPointer - 2] = 800;
		//VMThread_PopStack(thread);
		//VMThread_PopStack(thread);
		//VMThread_PopStack(thread);
		//VMThread_PopStack(thread);
		//VMThread_PopStack(thread);
	}
	else
	{
	}
		res = jumpTable[opcode](thread);
	print_state(thread, 3, opcode2, opcodeSize);
	Log2(thread->threadId, "\n");
	if(dumpMem)
	{
		dumpMem = 0;
	}

	if(!memHooked && opcode == 0x80)
	{
		if(subOpcode == 0x70)
		{
			AddMemoryMonitor(*gGlobalMemory, *gGlobalMemorySize, 0);
			memHooked = 1;
		}
	}
	else if(memHooked)
	{
		ForceVehFirst();
	}

	//uint32_t size = 1 + thread->nextInstructionPointer - startCounter;
	//Log("<<< [TRACE] thread=%d | END result: 0x%08X, size: %d\n", thread->threadId, res, size);
	//if(threadTicks[thread->threadId] == 23822)
	//{
	//    FILE* memDump = fopen("memdump.bin", "w");
	//    fwrite(thread->localMem, 1, thread->localMemSize, memDump);
	//    fclose(memDump);
	//}



	threadTicks[thread->threadId]++;
	threadSpacial[thread->threadId] = 0;

	//if(res == 0)
	//    res = 4;

	return res;
}

__declspec(naked) void HookStub(void) {
	__asm__ (
		".intel_syntax noprefix\n\t"
		"push esi\n\t"
		"call _TraceInstruction\n\t"
		"add esp, 4\n\t"
		"mov eax, " TOSTRING(RETURN_AFTER_CALL_ADDR) "\n\t"
		"jmp eax\n\t"
		".att_syntax prefix\n\t"
	);
	//asm volatile(
	//    "jmp *%0" 
	//    : 
	//    : "r"(RETURN_AFTER_CALL_ADDR)
	//);
	__builtin_unreachable();
}

// ==============================
//
// Hooking
//
// ==============================

void PatchInJump(uint32_t jumpTargetAddr, uint32_t patchAddress)
{
	DWORD oldProtect = 0;

	// Make the target memory writable
	LPVOID patchAddr = (LPVOID)patchAddress;
	if (!VirtualProtect(patchAddr, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
		if (g_logFile) Log("!!! VirtualProtect(READWRITE) FAILED (err=%lu)\n", GetLastError());
		return;
	}

	// Build the JMP instruction: E9 <rel32>
	uint32_t instrAfterJmp = patchAddress + 5;
	int32_t  relOffset = (int32_t)(jumpTargetAddr - instrAfterJmp);
	uint8_t* p = (uint8_t*)patchAddress;
	p[0] = 0xE9; // JMP rel32 opcode
	p[1] = (uint8_t)(relOffset & 0xFF);
	p[2] = (uint8_t)((relOffset >> 8) & 0xFF);
	p[3] = (uint8_t)((relOffset >> 16) & 0xFF);
	p[4] = (uint8_t)((relOffset >> 24) & 0xFF);

	// Restore original protection
	VirtualProtect(patchAddr, 8, oldProtect, &oldProtect);
}

static void InstallHook(void)
{
	QueryPerformanceFrequency(&countFrequency);
	QueryPerformanceCounter(&countStart);

	memoryLogFile = fopen("memory.log", "w");
	g_logFile = fopen("vm_trace.log", "w");
	if(g_logFile)
	{
		Log("========================================\n");
		Log("  VM Instruction Tracer DLL loaded\n");
		Log("========================================\n");
	}

	InitMemoryMonitor();

	PatchInJump((uint32_t)HookStub, PATCH_ADDRESS);
	PatchInJump((uint32_t)PopHookStub, POP_ADDRESS);
	PatchInJump((uint32_t)PushHookStub, PUSH_ADDRESS);
	PatchInJump((uint32_t)ReadPC8Stub, VM_READ_PC_8);
	PatchInJump((uint32_t)ReadCode8Stub, VM_READ_CODE_8);
	PatchInJump((uint32_t)ReadCode16Stub, VM_READ_CODE_16);
	PatchInJump((uint32_t)ReadCode32Stub, VM_READ_CODE_32);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	switch(fdwReason)
	{
		case DLL_PROCESS_ATTACH:
			DisableThreadLibraryCalls(hinstDLL);

			// Prefer attaching to the injector’s console so output appears in the same window
	        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
	            freopen("CONOUT$", "w", stdout);
	            freopen("CONOUT$", "w", stderr);
	            freopen("CONIN$",  "r", stdin);
	            printf("[DLL] Attached to parent console successfully.\n");
	        }
	        else {
	            // Fallback: create a brand-new console window
	            AllocConsole();
	            freopen("CONOUT$", "w", stdout);
	            freopen("CONOUT$", "w", stderr);
	            freopen("CONIN$",  "r", stdin);
	            printf("[DLL] Created new console (AttachConsole failed, error %lu).\n", GetLastError());
	        }

	        // Your real DLL code / hooks go here
	        printf("[DLL] Hello from the injected DLL!\n");

			InstallHook();
			break;

		case DLL_PROCESS_DETACH:
			for(int i = 0; i < 100; i++)
			{
				if(threadLogs[i] != NULL)
					fclose(threadLogs[i]);
			}
			if(g_logFile)
			{
				Log("\n========================================\n");
				Log("  VM Tracer DLL unloaded\n");
				Log("========================================\n");
				fclose(g_logFile);
				g_logFile = NULL;
			}
			break;
	}
	return TRUE;
}


/*

0x00000343


[4, 485, 0, 0x9086, 2, 0x00000001,0x00000004,0x00000000,0x00000000,0x0000002F,0x000003FA,0x000003FB,0x0000018C,0x00000100,0x04667DD0,0x04667F90,0x00001000,0x04667DF0,0x046C5070,0x04667E90,0x00000002,0x000008F0,0x00001000,0x04667E10,0x046C6078]
[4, 485, 1, 0x00000343] back
[4, 485, 1, 0xFFFFFFFF] frame
[4, 485, 1, 0xFFFFFFFF] decoration
[4, 485, 1, 0xC0000001] uVar1
[4, 485, 3, 0x9086, 2, 0x00000001,0x00000004,0x00000000,0x00000000,0x0000002B,0x000003FA,0x000003FC,0x0000018C,0x00000100,0x04667DD0,0x04667F90,0x00001000,0x04667DF0,0x046C5070,0x04667E90,0x00000002,0x000008F0,0x00001000,0x04667E10,0x046C6078]


tit_101 - Normal      - slot: 0x00000801
tit_102 - Highlighted - slot: 0x00000802

If this is removed, the exit confirmation dialog appears in the top-left corner as opposed to centered
exec: [2, 16703, 0, 0x9085, 2, 0x00000001,0x00000002,0x044A7B28,0x00000000,0x00000230,0x00000AA3,0x00000AA4,0x00000434,0x00001000,0x01804910,0x0180A0D8,0x00020000,0x01804930,0x018C0048,0x042EE748,0x00000002,0x00001630,0x00020000,0x01804950,0x018E0050]
pop:  [2, 16703, 1, 0x00000075]
pop:  [2, 16703, 1, 0x00000000]
pop:  [2, 16703, 1, 0x00000100]
pop:  [2, 16703, 1, 0x00000001]
pop:  [2, 16703, 1, 0x00000000]
pop:  [2, 16703, 1, 0x00000000]
pop:  [2, 16703, 1, 0xC0000000]
exec: [2, 16703, 3, 0x9085, 2, 0x00000001,0x00000002,0x044A7B28,0x00000000,0x00000229,0x00000AA3,0x00000AA5,0x00000434,0x00001000,0x01804910,0x0180A0D8,0x00020000,0x01804930,0x018C0048,0x042EE748,0x00000002,0x00001630,0x00020000,0x01804950,0x018E0050]

If this is removed, the menu and background doesn't show up
exec: [2, 16790, 0, 0x9020, 2, 0x00000001,0x00000002,0x044A7B28,0x00000000,0x00000232,0x00000B0F,0x00000B10,0x00000434,0x00001000,0x01804910,0x0180A0D8,0x00020000,0x01804930,0x018C0048,0x042EE748,0x00000002,0x00001630,0x00020000,0x01804950,0x018E0050]
pop:  [2, 16790, 1, 0x00000000]
pop:  [2, 16790, 1, 0x00000000]
pop:  [2, 16790, 1, 0x00000078]
pop:  [2, 16790, 1, 0x00000096]
pop:  [2, 16790, 1, 0x00000000]
pop:  [2, 16790, 1, 0xC0000000]
exec: [2, 16790, 3, 0x9020, 2, 0x00000001,0x00000002,0x044A7B28,0x00000001,0x0000022C,0x00000B0F,0x00000B11,0x00000434,0x00001000,0x01804910,0x0180A0D8,0x00020000,0x01804930,0x018C0048,0x042EE748,0x00000002,0x00001630,0x00020000,0x01804950,0x018E0050]

*/

/* ------------------------------------------------------------------ */
LONG g_unlock_count = 0;          /* how many threads currently have the page unlocked */
DWORD g_oldProtect;
/* ------------------------------------------------------------------ */
static void UnlockRegion(void)
{
    if (InterlockedIncrement(&g_unlock_count) == 1) {
        DWORD old;
        VirtualProtect(g_region, g_region_size, g_oldProtect, &old);
    }
}

static void LockRegion(void)
{
    if (InterlockedDecrement(&g_unlock_count) == 0) {
        DWORD old;
        VirtualProtect(g_region, g_region_size, PAGE_NOACCESS, &old);
    }
}

LONG CALLBACK VehHandler(PEXCEPTION_POINTERS ep)
{
	EnterCriticalSection(&g_cs);
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    PCONTEXT ctx = ep->ContextRecord;
    ULONG_PTR type = ep->ExceptionRecord->ExceptionInformation[0]; /* 0=read, 1=write */
    void* addr     = (void*)ep->ExceptionRecord->ExceptionInformation[1];
    void* instr    = ep->ExceptionRecord->ExceptionAddress;

    /*
    printf("\n========== VEH EXCEPTION ==========\n");
    printf("Exception code   : 0x%08X\n", code);
    printf("Instruction addr : %p\n", instr);
    printf("Access addr      : %p\n", addr);
    printf("Hooked addr      : %p - %p\n", g_region, g_region + g_region_size);
    printf("Lock count       : %lu\n", g_unlock_count);
    printf("Thread           : %lu\n", GetCurrentThreadId());

    // optional: decode a few common codes
    switch (code) {
		// Memory / access
	    case EXCEPTION_ACCESS_VIOLATION:          printf("Type: ACCESS_VIOLATION\n");          break;
	    case EXCEPTION_IN_PAGE_ERROR:             printf("Type: IN_PAGE_ERROR\n");             break;
	    case EXCEPTION_GUARD_PAGE:                printf("Type: GUARD_PAGE\n");                break;
	    case EXCEPTION_STACK_OVERFLOW:            printf("Type: STACK_OVERFLOW\n");            break;

	    // Integer
	    case EXCEPTION_INT_DIVIDE_BY_ZERO:        printf("Type: INT_DIVIDE_BY_ZERO\n");        break;
	    case EXCEPTION_INT_OVERFLOW:              printf("Type: INT_OVERFLOW\n");              break;

	    // Floating-point
	    case EXCEPTION_FLT_DIVIDE_BY_ZERO:        printf("Type: FLT_DIVIDE_BY_ZERO\n");        break;
	    case EXCEPTION_FLT_OVERFLOW:              printf("Type: FLT_OVERFLOW\n");              break;
	    case EXCEPTION_FLT_UNDERFLOW:             printf("Type: FLT_UNDERFLOW\n");             break;
	    case EXCEPTION_FLT_INVALID_OPERATION:     printf("Type: FLT_INVALID_OPERATION\n");     break;
	    case EXCEPTION_FLT_INEXACT_RESULT:        printf("Type: FLT_INEXACT_RESULT\n");        break;
	    case EXCEPTION_FLT_DENORMAL_OPERAND:      printf("Type: FLT_DENORMAL_OPERAND\n");      break;
	    case EXCEPTION_FLT_STACK_CHECK:           printf("Type: FLT_STACK_CHECK\n");           break;

	    // Instruction / CPU
	    case EXCEPTION_ILLEGAL_INSTRUCTION:       printf("Type: ILLEGAL_INSTRUCTION\n");       break;
	    case EXCEPTION_PRIV_INSTRUCTION:          printf("Type: PRIV_INSTRUCTION\n");          break;
	    case EXCEPTION_DATATYPE_MISALIGNMENT:     printf("Type: DATATYPE_MISALIGNMENT\n");     break;
	    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:     printf("Type: ARRAY_BOUNDS_EXCEEDED\n");     break;

	    // Debugging / control
	    case EXCEPTION_BREAKPOINT:                printf("Type: BREAKPOINT\n");                break;
	    case EXCEPTION_SINGLE_STEP:               printf("Type: SINGLE_STEP\n");               break;

	    // Other
	    case EXCEPTION_NONCONTINUABLE_EXCEPTION:  printf("Type: NONCONTINUABLE_EXCEPTION\n");  break;
	    case EXCEPTION_INVALID_DISPOSITION:       printf("Type: INVALID_DISPOSITION\n");       break;
	    case EXCEPTION_INVALID_HANDLE:            printf("Type: INVALID_HANDLE\n");            break;

	    default:
	        printf("Type: unknown (0x%08X)\n", code);
	        break;
    }
    */

    // ---- print call stack (raw addresses) ----
    /*
    printf("Call stack:\n");

    void* stack[64];
    USHORT frames = CaptureStackBackTrace(0, 64, stack, NULL);

    for (USHORT i = 0; i < frames; ++i) {
        printf("  [%02u] %p\n", i, stack[i]);
    }
    */
    DWORD old;
    if(code == EXCEPTION_ACCESS_VIOLATION)
    {

        t_state.fault_addr = addr;
        t_state.instr_addr = instr;
        t_state.was_write  = (type == 1) ? 1 : 0;
        t_state.active     = 1;

        t_state.ignore = 1;
        for(int i = 0; i < memHooks; i++)
        {
        	if(addr >= memHooksStart[i] && addr < (void*)(memHooksStart[i] + memHooksSize[i]))
        	{
        		t_state.addr = memHooksStart[i];
        		t_state.size = memHooksSize[i];
        		t_state.type = memHooksType[i];
        		t_state.real = memHooksStartReal[i];
				VirtualProtect(memHooksStart[i], memHooksSize[i], PAGE_READWRITE, &old);
	        	if(addr >= memHooksStartReal[i] && addr < (void*)(memHooksStartReal[i] + memHooksSizeReal[i]))
	        		t_state.ignore = 0;
        	}
        }

        if (!t_state.was_write)
        {
            /* Capture value that is about to be read */
            t_state.value = *(DWORD*)addr;
        }

        ctx->EFlags |= 0x100;          /* TF = 1 */
        //printf("Return           : EXCEPTION_CONTINUE_EXECUTION\n");
        LeaveCriticalSection(&g_cs);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    else if (code == EXCEPTION_SINGLE_STEP)
    {
        if(!t_state.active)
        {
        	//printf("Return           : EXCEPTION_CONTINUE_SEARCH\n");
        	LeaveCriticalSection(&g_cs);
        	return EXCEPTION_CONTINUE_SEARCH;
        }

        DWORD value;
        if (t_state.was_write)
        {
            /* Write has completed */
            value = *(DWORD*)t_state.fault_addr;
        }
        else
        {
            value = t_state.value;
        }

        if(!t_state.ignore)
        {
        	//Log2(this->threadId, "push: [%lld, %d, %d, %d, 0x%.8X]\n", usPassed, this->threadId, threadTicks[this->threadId], 2, data);
	        LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			uint64_t usPassed = (now.QuadPart - countStart.QuadPart) * 1000000 / countFrequency.QuadPart;
	        Log3(memoryLogFile, "mem:  [%llu, %s, %s, 0x%08X, %08X, %p, %p, %lu]\n",
	        		usPassed,
	               t_state.was_write ? "WRITE" : "READ",
	               t_state.type ? "Local" : "Global",
	               value,
	               t_state.fault_addr - t_state.real,
	               t_state.fault_addr,
	               t_state.instr_addr,
	               GetCurrentThreadId());
	    }

        VirtualProtect(t_state.addr, t_state.size, PAGE_NOACCESS, &old);
        ctx->EFlags &= ~0x100;         /* clear TF */

        t_state.active = 0;
        //printf("Return           : EXCEPTION_CONTINUE_EXECUTION\n");
        LeaveCriticalSection(&g_cs);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    else
    {
    	//printf("Return           : EXCEPTION_CONTINUE_SEARCH\n");
    	LeaveCriticalSection(&g_cs);
    	return EXCEPTION_CONTINUE_SEARCH;
    }

}

/* ------------------------------------------------------------------ */
/* Call once at start-up                                              */

#define PAGE_SIZE 0x1000

void* AlignDown(void* p)
{
    return (void*)((ULONG_PTR)p & ~(PAGE_SIZE - 1));
}

size_t AlignUp(size_t s)
{
    return (s + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

void InitMemoryMonitor()
{
    InitializeCriticalSection(&g_cs);
    ForceVehFirst();
}

void AddMemoryMonitor(void* region, size_t size, int type)
{
    memHooksStart[memHooks]     = AlignDown(region);
	memHooksSize[memHooks]      = AlignUp((char*)region + size - (char*)memHooksStart[memHooks]);
	memHooksStartReal[memHooks] = region;
	memHooksSizeReal[memHooks]  = size;
	memHooksType[memHooks]      = type;

    VirtualProtect(memHooksStart[memHooks], memHooksSize[memHooks], PAGE_NOACCESS, &g_oldProtect);

    memHooks++;

    ForceVehFirst();          /* install as first */
}