#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>

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
#define POP_ADDRESS            0x0041FC1E // pop stack (before ECX gets overwritten)
#define PUSH_ADDRESS           0x0041FC51 // push stack (end of function)
#define RESOLVE_ADDR           0x0043b560 // resolve addr

#define PATCH_ADDRESS          0x0043AA8E // address where basic opcodes are called
#define JUMPTABLE_ADDRESS      0x0046C2A4 // basic opcodes
#define RETURN_AFTER_CALL_ADDR 0x0043AA95 // addr to jump to after opcode call
#endif


int threadTicks[100] = { 0 };
int threadSpacial[100] = { 0 };
FILE* threadLogs[100] = { NULL };


typedef struct _VMThread_t {
    uint32_t programId;
    uint32_t threadId;
    struct _VMThread_t *prevVMState;
    uint32_t flags;
    uint32_t stackPointer;
    uint32_t instructionPointer;
    uint32_t programCounter;
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

static FILE* g_logFile = NULL;

static void Log(const char* fmt, ...)
{
    if(!g_logFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);
    fflush(g_logFile);
}

static void Log2(FILE* file, const char* fmt, ...)
{
    if(!file) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    //fflush(file);
}

// ==============================
//
// Pop tracing
//
// ==============================

uint32_t __cdecl TracePop(VMThread_t* thread)
{
    uint32_t data = thread->stack[thread->stackPointer];
    //Log(" <  [POP]   thread=%d | data: 0x%08X\n", thread->threadId, data);
    if(threadSpacial[thread->threadId] == 1 || threadSpacial[thread->threadId] == 2)
    {
        char* str = (char*)Thread_ResolveAddr(data, thread);
        Log2(threadLogs[thread->threadId], "pop:  [%d, %d, %d, 0x%.8X, \"%s\"]\n", thread->threadId, threadTicks[thread->threadId], 1, data, str);
        threadSpacial[thread->threadId]++;
    }
    else
        Log2(threadLogs[thread->threadId], "pop:  [%d, %d, %d, 0x%.8X]\n", thread->threadId, threadTicks[thread->threadId], 1, data);
    return data;
}

__declspec(naked) void PopHookStub(void) {
    __asm__ (
        ".intel_syntax noprefix\n\t"
        //"push eax\n\t"
        //"pushad\n\t"
        "push ecx\n\t"
        "call _TracePop\n\t"
        "add esp, 4\n\t"
        //"popad\n\t"
        "ret\n\t"
        ".att_syntax prefix\n\t"
    );
    __builtin_unreachable();
}

// ==============================
//
// Push tracing
//
// ==============================

void __cdecl TracePush(VMThread_t* thread, uint32_t data)
{
    //Log(" >  [PUSH]  thread=%d | data: 0x%08X\n", thread->threadId, data);
    Log2(threadLogs[thread->threadId], "push: [%d, %d, %d, 0x%.8X]\n", thread->threadId, threadTicks[thread->threadId], 2, data);
}

__declspec(naked) void PushHookStub(void)
{
    __asm__ (
        ".intel_syntax noprefix\n\t"
        "push dword ptr [esp + 4]\n\t"
        "push ecx\n\t"
        "call _TracePush\n\t"
        "add esp, 8\n\t"
        "ret 4\n\t"
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
    Log2(threadLogs[thread->threadId], "exec: [%d, %d, %d, 0x%.4X, %d, 0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X,0x%.8X]\n",
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
        thread->programCounter,        // 0x00000B36
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

InstructionHandler_t* jumpTable = (InstructionHandler_t*)JUMPTABLE_ADDRESS;
uint32_t __cdecl TraceInstruction(uint32_t opcode, VMThread_t* thread)
{
    if(threadLogs[thread->threadId] == NULL)
    {
        char filename[255];
        sprintf(&filename[0], "vm_trace_thread_%d.log", thread->threadId);
        threadLogs[thread->threadId] = fopen(filename, "w");
    }

    uint32_t startCounter = thread->programCounter;
    int opcodeSize = 1;
    uint16_t opcode2 = opcode;
    if(opcode >= 128)
    {
        uint8_t subOpcode = thread->codeSpace[thread->programCounter];
        uint16_t extOp = opcode << 8 | subOpcode;
        opcode2 = extOp;
        opcodeSize = 2;
        if(opcode == 0x90 && subOpcode == 0x10)
            threadSpacial[thread->threadId] = 1;
        if(opcode == 0x80 && subOpcode == 0x40)
            threadSpacial[thread->threadId] = 1;
        if(opcode == 0x80 && subOpcode == 0x44)
        {
            Log2(threadLogs[thread->threadId], "CreateThread(\"%s\", \"%s\", %.8X, %.8X, %.8X)\n",
                (char*)Thread_ResolveAddr(thread->stack[thread->stackPointer - 5], thread),
                (char*)Thread_ResolveAddr(thread->stack[thread->stackPointer - 4], thread),
                thread->stack[thread->stackPointer - 3],
                thread->stack[thread->stackPointer - 2],
                thread->stack[thread->stackPointer - 1]
            );
        }
        // No effect
        else if(opcode == 0x90 && subOpcode == 0x88)
        {
            thread->programCounter++;
            thread->stackPointer -= 5;
            return 0;
        }
        // No effect
        else if(opcode == 0x90 && subOpcode == 0x87)
        {
            thread->programCounter++;
            thread->stackPointer -= 7;
            return 0;
        }
        else if(opcode == 0x90 && subOpcode == 0x85)
        {
            thread->programCounter++;
            thread->stackPointer -= 7;
            return 0;
        }
        // Background doesn't show up if removed
        else if(opcode == 0x90 && subOpcode == 0x86)
        {
            thread->programCounter++;
            thread->stackPointer -= 4;
            return 0;
        }
        // Crash if removed
        //else if(opcode == 0x90 && subOpcode == 0x80)
        //{
        //    thread->programCounter++;
        //    thread->stackPointer -= 3;
        //    return 0;
        //}
        // No effect
        else if(opcode == 0x90 && subOpcode == 0x13)
        {
            thread->programCounter++;
            thread->stackPointer -= 2;
            return 0;
        }
        // No effect
        else if(opcode == 0x90 && subOpcode == 0x94)
        {
            thread->programCounter++;
            thread->stackPointer -= 1;
            return 0;
        }
        // No effect
        else if(opcode == 0x91 && subOpcode == 0x98)
        {
            thread->programCounter++;
            thread->stackPointer -= 5;
            return 0;
        }
        // No effect
        else if(opcode == 0x90 && subOpcode == 0x96)
        {
            thread->programCounter++;
            thread->stackPointer -= 2;
            return 0;
        }
        // No effect
        else if(opcode == 0x90 && subOpcode == 0x95)
        {
            thread->programCounter++;
            thread->stackPointer -= 2;
            return 0;
        }
        // No effect
        else if(opcode == 0x90 && subOpcode == 0x97)
        {
            thread->programCounter++;
            thread->stackPointer -= 2;
            return 0;
        }
        // Causes meny not to appear - Close dialog still appears
        //else if(opcode == 0x90 && subOpcode == 0x20)
        //{
        //    thread->programCounter++;
        //    thread->stackPointer -= 6;
        //    return 0;
        //}
        else if(opcode == 0x90 && subOpcode == 0x20)
        {
            //thread->programCounter++;
            //thread->stackPointer -= 6;
            thread->stack[thread->stackPointer - 1] = 0x00; // 0x00
            thread->stack[thread->stackPointer - 2] = 0x00; // 0x00
            thread->stack[thread->stackPointer - 3] = 0x01; // 0x78
            thread->stack[thread->stackPointer - 4] = 0x01; // 0x96
            thread->stack[thread->stackPointer - 5] = 0x01; // 0x00
            thread->stack[thread->stackPointer - 6] = 0x00; // 0x00
            //return 0;
        }


        //Log(">>> [TRACE] thread=%d | START opcode: 0x%04X, tick: %d\n", thread->threadId, extOp, threadTicks[thread->threadId]);
    }
    //else
    //    Log(">>> [TRACE] thread=%d | START opcode: 0x%02X, tick: %d\n", thread->threadId, opcode, threadTicks[thread->threadId]);
    print_state(thread, 0, opcode2, opcodeSize);
    uint32_t res = jumpTable[opcode](thread);
    print_state(thread, 3, opcode2, opcodeSize);
    //uint32_t size = 1 + thread->programCounter - startCounter;
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
        ".att_syntax prefix\n\t"
    );
    asm volatile(
        "jmp *%0" 
        : 
        : "r"(RETURN_AFTER_CALL_ADDR)
    );
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
    g_logFile = fopen("vm_trace.log", "w");
    if(g_logFile)
    {
        Log("========================================\n");
        Log("  VM Instruction Tracer DLL loaded\n");
        Log("========================================\n");
    }

    PatchInJump((uint32_t)HookStub, PATCH_ADDRESS);
    PatchInJump((uint32_t)PopHookStub, POP_ADDRESS);
    PatchInJump((uint32_t)PushHookStub, PUSH_ADDRESS);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch(fdwReason)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
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

