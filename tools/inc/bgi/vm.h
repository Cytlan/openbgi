/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * vm.h - the script virtual machine: threads, the scheduler and the
 *        instruction tables
 *
 * A script thread (Thread_t) owns an evaluation stack, a code area that
 * holds the program and the modules appended to it, a data area for the
 * call frames and locals, a local heap, a timer and a message queue, and,
 * while it is blocked, a wait object.  The scheduler (VmMain, src/vm/sched.c)
 * gives every thread of the list a turn per pass; each instruction handler
 * is a VmHandler_t that works on the thread and returns a scheduler code.
 * The handler tables are filled at start-up for the engine version in use
 * (Vm_TablesInit), so a script only sees the instructions its build had.
 *
 * Implemented in src/vm/thread.c (threads, heap), sched.c (the main loop),
 * ops_*.c (the handlers and their tables), trace.c (the development aids),
 * checks.c (operand validators) and userop.c (the "FF xx" instructions).
 * docs/vm.md describes the machinery; the handlers' signature comments in
 * ops_*.c are the instruction-by-instruction reference.
 */
#ifndef BGI_VM_H_
#define BGI_VM_H_

#include "bgi/common.h"

// -------------------------------------------------------------------------
// Support objects used by a thread
// -------------------------------------------------------------------------

#include "bgi/pixbuf.h" // PixBuf_t: the stack, code and data buffers of a thread

/* The thread-local heap ("70 lalloc" / "71 lfree"): a growable arena with a
 * free list and a used list of {offset, size} records.  Blocks are
 * addressed by their offset into the arena; the arena doubles (and moves)
 * when a request does not fit, so pointers into it are only valid until
 * the next allocation. */
typedef struct HeapNode
{
	uint32_t off;          // offset of the block into the arena, bytes
	uint32_t size;         // bytes
	struct HeapNode* next; // next record of the same list
} HeapNode_t;
typedef struct Heap
{
	uint32_t size;        // arena size, bytes
	uint8_t* base;        // arena
	HeapNode_t* freeHead; // free records, ascending offset
	HeapNode_t* usedHead; // used records, most recent first
} Heap_t;
Heap_t* Heap_Create(uint32_t size);         // an arena of `size` bytes, all free
void Heap_Destroy(Heap_t* h);               // the arena, the records and `h` itself
uint32_t Heap_Alloc(Heap_t* h, uint32_t n); // first fit; grows the arena when nothing fits; returns the block's offset
int Heap_Free(Heap_t* h, uint32_t off);     // 1 when `off` started an allocated block, 0 when it did not (nothing happens)
uint8_t* Heap_Ptr(Heap_t* h, uint32_t off); // the address of offset `off` (valid until the next Heap_Alloc)

// -------------------------------------------------------------------------
// Script threads
// -------------------------------------------------------------------------

typedef struct ModuleNode // 16 bytes: one program loaded into a code area
{
	char* name;              // heap copy of the file name
	uint32_t size;           // code bytes
	uint32_t base;           // offset of the first byte in the code area
	struct ModuleNode* next; // previously loaded module
} ModuleNode_t;

typedef struct ThreadMsg // 8 bytes: inter-thread message ("80 48" .. "80 4B")
{
	uint32_t value;         // the 32-bit value posted
	struct ThreadMsg* next; // the message posted after this one
} ThreadMsg_t;

typedef struct ProgramHeader // start of a "._bp" file
{
	uint32_t codeOffset; // from the start of the file to the first code byte
	uint32_t codeSize;   // code bytes that follow it
} ProgramHeader_t;

struct Wait;

typedef struct Thread // 0x64 bytes in the original
{
	int32_t valid;          // 1 when the buffers below were allocated (0 for the root thread)
	uint32_t id;            // serial number; the handle scripts see
	struct Thread* next;    // singly linked list, in creation order
	uint32_t flags;         // THR_FLAG_*
	uint32_t sp;            // evaluation stack index of the next push; wraps at stackEntries
	uint32_t opIP;          // offset of the instruction being executed
	uint32_t ip;            // read cursor: the next operand byte
	uint32_t fp;            // frame pointer: byte offset into the data area
	uint32_t stackEntries;  // capacity of `stack`, entries
	PixBuf_t* stackBuf;     // owns `stack`
	uint32_t* stack;        // the evaluation stack
	uint32_t codeSize;      // capacity of the code area, bytes
	PixBuf_t* codeBuf;      // owns `code`
	uint8_t* code;          // the code area: the loaded modules, back to back
	ModuleNode_t* modules;  // most recently loaded first
	uint32_t moduleCount;   // length of `modules`
	uint32_t codeUsed;      // bytes of the code area the modules occupy
	uint32_t dataSize;      // capacity of the data area, bytes
	PixBuf_t* dataBuf;      // owns `data`
	uint8_t* data;          // call stack and locals
	Heap_t* heap;           // local heap, 0x8000 bytes initially
	struct Wait* wait;      // owned; polled while THR_FLAG_WAITING is set
	uint32_t timerDeadline; // the thread timer ("80 58" .. "80 5A"), in GetTicks milliseconds
	ThreadMsg_t* msgHead;   // the message queue, oldest first (the original keeps a list pseudo-node in front of it)
} Thread_t;

#define THR_FLAG_WAITING 1u // blocked on `wait`: the scheduler polls it instead of running the thread

extern Thread_t* gRootThread;  // list head, never runs; its id is 0
extern uint32_t gThreadSerial; // the id the next thread gets

/* Allocate a thread with a stack of stackEntries values, a code area of
 * codeSize bytes and a data area of dataSize bytes; with any of the three
 * 0 the result is a buffer-less list head (the root thread).  The thread
 * is not linked into any list. */
Thread_t* Thread_New(uint32_t stackEntries, uint32_t codeSize, uint32_t dataSize);
// release the buffers, modules, wait object and messages, then the thread itself (not unlinked)
void Thread_Delete(Thread_t* t);
// Thread_New appended to the end of the list that starts at `root`
Thread_t* Thread_SpawnChild(Thread_t* root, uint32_t stackEntries, uint32_t codeSize, uint32_t dataSize);
// unlink and delete `t` from root's list; 1 when it was found
int Thread_KillChild(Thread_t* root, Thread_t* t);
// delete every thread after `root`, the last one first
void Thread_KillAllChildren(Thread_t* root);
// the thread with that id in root's list, NULL when there is none (id 0, the root, is never found)
Thread_t* Thread_FindById(Thread_t* root, uint32_t id);
static inline Thread_t* Thread_Next(Thread_t* t)
{
	return t->next;
}

static inline uint32_t Thread_GetId(Thread_t* t)
{
	return t->id;
}

// drop every module record; the code area counts as empty afterwards
void Thread_FreeModules(Thread_t* t);
/* append the program `file` (a "._bp" image with a ProgramHeader_t) to the
 * code area under `name` and return the offset of its first byte, or
 * 0x80000000 when the code area has no room for it ("80 40", "80 44") */
uint32_t Thread_LoadModule(Thread_t* t, const void* file, const char* name);
// drop the module loaded last ("80 41"); the number left, or 0x80000001 when there was none
uint32_t Thread_UnloadLastModule(Thread_t* t);
/* the module records as an array, most recently loaded first, into *out
 * (BGI_Alloc, the caller frees it; NULL when there is none); returns the
 * count */
uint32_t Thread_ExportModules(Thread_t* t, ModuleNode_t** out);

static inline uint32_t Thread_GetFlags(Thread_t* t)
{
	return t->flags;
}

static inline void Thread_ClrFlags(Thread_t* t, uint32_t f)
{
	t->flags &= ~f;
}

static inline void Thread_SetFlags(Thread_t* t, uint32_t f)
{
	t->flags |= f;
}

// continue at code offset `ip` (a jump: both the read cursor and the instruction start)
static inline void Thread_SetIP(Thread_t* t, uint32_t ip)
{
	t->opIP = ip;
	t->ip = ip;
}

static inline uint32_t Thread_GetFP(Thread_t* t)
{
	return t->fp;
}

static inline void Thread_SetFP(Thread_t* t, uint32_t fp)
{
	t->fp = fp;
}

// the offset of the instruction being executed (for error messages and relative operands)
static inline uint32_t Thread_CurOpIP(Thread_t* t)
{
	return t->opIP;
}

// instruction stream: the opcode byte, then its operands with the Rd* readers
static inline uint8_t Thread_FetchOp(Thread_t* t)
{
	t->opIP = t->ip;
	t->ip++;
	return t->code[t->opIP];
}

static inline uint8_t RdU8(Thread_t* t)
{
	return t->code[t->ip++];
}

static inline uint16_t RdU16(Thread_t* t)
{
	uint16_t v;
	memcpy(&v, t->code + t->ip, 2);
	t->ip += 2;
	return v;
}

static inline uint32_t RdU32(Thread_t* t)
{
	uint32_t v;
	memcpy(&v, t->code + t->ip, 4);
	t->ip += 4;
	return v;
}

int RdBytes(Thread_t* t, void* dst, uint32_t n); // n operand bytes into dst; 0 (nothing read) if they reach past the code area

// evaluation stack (circular, no overflow check - as in the original)
static inline uint32_t Thread_Pop(Thread_t* t)
{
	uint32_t i = t->sp ? t->sp : t->stackEntries;
	i--;
	t->sp = i;
	return t->stack[i];
}

static inline void Thread_Push(Thread_t* t, uint32_t v)
{
	t->stack[t->sp] = v;
	t->sp = (t->sp + 1 < t->stackEntries) ? t->sp + 1 : 0;
}

// call-stack frames in the data area: 32-bit values at the frame pointer, which grows upwards
static inline uint32_t FramePop(Thread_t* t)
{
	uint32_t v;
	t->fp -= 4;
	memcpy(&v, t->data + t->fp, 4);
	return v;
}

static inline void FramePush(Thread_t* t, uint32_t v)
{
	memcpy(t->data + t->fp, &v, 4);
	t->fp += 4;
}

// local heap ("70" / "71"): the Heap_* operations on the thread's heap
static inline uint32_t Thread_HeapAlloc(Thread_t* t, uint32_t n)
{
	return Heap_Alloc(t->heap, n);
}

static inline int Thread_HeapFree(Thread_t* t, uint32_t off)
{
	return Heap_Free(t->heap, off);
}

static inline uint8_t* Thread_HeapPtr(Thread_t* t, uint32_t off)
{
	return Heap_Ptr(t->heap, off);
}

/* wait objects (inc/bgi/wait.h): a handler that has to block hands the
 * thread a wait object and returns scheduler code 2.  The thread owns the
 * object and releases it when the wait ends. */
void Thread_SetWait(Thread_t* t, struct Wait* w);                       // install `w` (releasing a previous one) and set THR_FLAG_WAITING
int Thread_PollWait(Thread_t* t);                                       // poll it: 0 still waiting, 1 done, -1 failed (1 and -1 release it and clear the flag)
int Thread_WaitNotify(Thread_t* t, uint32_t a, uint32_t b, uint32_t c); // "80 4C": queue (a, b, c) on the wait object; 1 when the thread had one, 0 otherwise

// per-thread timer ("80 58" .. "80 5A"), in milliseconds
uint32_t Thread_TimerRemaining(Thread_t* t);                 // milliseconds until the deadline, 0 once it has passed
void Thread_TimerSet(Thread_t* t, uint32_t ms);              // the deadline is `ms` from now
static inline void Thread_TimerAdd(Thread_t* t, uint32_t ms) // move the deadline `ms` later
{
	t->timerDeadline += ms;
}

// per-thread message queue ("80 48" .. "80 4B")
void Thread_ListAppend(Thread_t* t, uint32_t v);   // queue v behind the other messages
int Thread_ListPopFront(Thread_t* t, uint32_t* v); // the oldest message into *v; 0 when the queue is empty

// -------------------------------------------------------------------------
// Instruction handlers and the scheduler
// -------------------------------------------------------------------------

/* Every handler returns a scheduler code (docs/vm.md):
 *  0 next instruction, 1/2 end of turn (2: the thread was put into a
 *  wait), 3 switch to the thread in gSwitchThreadId, 4 end thread,
 *  5 reboot, 6 quit. */
typedef int (*VmHandler_t)(Thread_t* t);

#include "bgi/version.h"

/* a handler registration: the opcode, the handler and the engine
 * generations it is valid for (GEN_FIRST .. GEN_LAST for one that never
 * changed; a version-specific variant names its own range and wins over
 * the wider one).  The tables are filled from these at start-up for the
 * selected engine profile (Vm_FillTable). */
typedef struct OpEntry
{
	uint8_t op;     // the opcode (the second byte for a family instruction)
	VmHandler_t fn; // its handler
	uint8_t from;   // first generation it is valid for (EngineGen_t)
	uint8_t to;     // last one, inclusive
} OpEntry_t;
/* fill one family's table from the `n` registrations of `list` for the
 * selected engine version: an opcode the version does not define stays
 * NULL (the "undefined instruction" error), one it defines but no
 * registration covers gets Opcode_NotImplemented */
void Vm_FillTable(VmHandler_t* table, OpFamily_t fam, const OpEntry_t* list, size_t n);
int Opcode_NotImplemented(Thread_t* t); // the stub for defined-but-unimplemented opcodes: a script error naming the opcode

extern VmHandler_t vm_optable_main[256]; // the base instructions and the family dispatchers
extern VmHandler_t vm_optable_80[256];   // system control
extern VmHandler_t vm_optable_90[256];   // graphics 0
extern VmHandler_t vm_optable_91[256];   // graphics 1
extern VmHandler_t vm_optable_92[256];   // graphics 2
extern VmHandler_t vm_optable_A0[256];   // sound
extern VmHandler_t vm_optable_B0[256];   // extension 0
extern VmHandler_t vm_optable_C0[256];   // extension 1
extern VmHandler_t vm_optable_7F[256];   // the families of later builds (docs/versions.md): "7F xx", 1.667 on
extern VmHandler_t vm_optable_81[256];   // "81 xx", 1.69 build 472 on (ops_sys81.c)
extern VmHandler_t vm_optable_D0[256];   // "D0 xx", 1.535 on
extern VmHandler_t vm_optable_E0[256];   // "E0 xx", 1.588 on
extern VmHandler_t vm_optable_EF[256];   // "EF xx", the OpenBGI extensions (ops_obgi.c): every generation

void Vm_TablesInit(void);       // fill the handler tables for the selected engine profile; once, before the first instruction
void Vm_ListMissing(FILE* out); // --list-missing: the instructions of the profile without a handler

/* The development tracing aids of src/vm/trace.c (BGI_TRACE, BGI_TRACE_OPS,
 * BGI_TRACE_LAST, BGI_OPSTAT in the environment; not part of the original). */
extern int gVmTraceOn;                        // any of them is on: the scheduler calls the hooks
void Vm_TraceInit(void);                      // read the environment, once before the first instruction
void Vm_TraceBefore(Thread_t* t, uint8_t op); // before thread t executes instruction op
void Vm_TraceAfter(Thread_t* t);              // after it ran
void Vm_TraceDump(void);                      // BGI_TRACE_LAST: print the ring of the last instructions; MsgBox calls it before any message box
void Vm_TraceShutdown(void);                  // BGI_OPSTAT: write the counts, when the machine stops

/* Handlers that older builds had in another family (the window operations
 * of 1.58 / 1.64 lived in 90 8x and 91 8x before the 92 family took them
 * over, docs/versions.md "Base-opcode readings"); shared between the
 * ops_gfx*.c tables. */
int Opcode_Gfx0_WindowSetPunch(Thread_t* t);    // 90 87, 1.58 .. 1.64: 90 8B
int Opcode_Gfx1_WindowSetFont(Thread_t* t);     // 91 88, 1.58 .. 1.64: 90 87
int Opcode_Gfx1_WindowSetSpacing(Thread_t* t);  // 91 89, 1.58 .. 1.64: 90 8A
int Opcode_Gfx1_WindowGetCursor(Thread_t* t);   // 91 8D, 1.58 .. 1.64: 91 8F
int Opcode_Gfx2_WindowShowFrame(Thread_t* t);   // 92 88, 1.58 .. 1.64: 90 8C
int Opcode_Gfx2_WindowDrawToFrame(Thread_t* t); // 92 89, 1.58 .. 1.64: 90 8D
int Opcode_Gfx2_WindowShowText(Thread_t* t);    // 92 8C, 1.58 .. 1.64: 91 8C
int Opcode_Gfx2_WindowDrawToText(Thread_t* t);  // 92 8D, 1.58 .. 1.64: 91 8D
int Opcode_Gfx2_WindowClear(Thread_t* t);       // 92 8E, 1.58 .. 1.64: 91 8E

extern uint32_t gSwitchThreadId;   // target of scheduler code 3 ("80 5E")
extern int gExclusiveOn;           // 1 while only gExclusiveThread gets turns ("80 5D")
extern Thread_t* gExclusiveThread; // that thread

// the main loop: boot the first thread and run the scheduler until the thread list empties; returns 0
int VmMain(void);
/* load program `name` of archive `arc` into a new thread appended to the
 * list that `root` is part of ("80 44", the boot thread); the sizes are
 * those of Thread_New.  Returns the thread's id; a missing program or one
 * that does not fit the code area is a script error raised on `root`. */
uint32_t SpawnThread(const char* arc, const char* name, uint32_t stackEntries,
	uint32_t codeSize, uint32_t dataSize, Thread_t* root);
// make `t` the only thread the scheduler runs ("80 5D"); NULL lifts the restriction (`on` is ignored)
void SetExclusiveThread(Thread_t* t, int on);

// the per-pass services (particles, panels, input, the display, the message pump), exported for the OS back ends / tests
void Vm_FrameServices(void);

// script error for sizes above the profile's heap limit (32 MB in 1.69; "70", "80 20")
void CheckAllocSize(uint32_t size, Thread_t* t);

/* operand validators of the graphics / sound families (src/vm/checks.c);
 * each raises a script error and never returns when the value is out of
 * range */
void CheckSeSlot(uint32_t slot, Thread_t* t);        // 0.. 63
void CheckPan(int32_t pan, Thread_t* t);             // 0.. 128
void CheckVolume(int32_t volume, Thread_t* t);       // 0.. 128
void CheckBgmChannel(uint32_t channel, Thread_t* t); // 0.. 15
void CheckDivCount(int32_t n, Thread_t* t);          // 1.. 256
void CheckFontNo(int32_t no, Thread_t* t);           // a number FontNameByNo knows
void CheckVirtualKey(int32_t vk);                    // 0.. 255, no thread context
// the bitmap table: 4096 entries, 16384 from 1.69 build 472 on (the manager always has 16384 here)
#define BMP_NO_LIMIT (gEngine->gen >= GEN_1_69_472 ? 0x4000 : 0x1000)
void CheckBitmapNo(int32_t no, Thread_t* t);          // 0.. BMP_NO_LIMIT - 1
void CheckPriority(uint32_t prio, Thread_t* t);       // 0.. 4095
void ErrBitmapNotRegistered(int32_t no, Thread_t* t); // always raises
void CheckEffectMode(uint32_t mode, Thread_t* t);     // one of the blitters' effect modes (checks.c lists them)
void CheckAlpha(uint32_t level, Thread_t* t);         // 0.. 256
void CheckMixRatio(uint32_t ratio, Thread_t* t);      // 0.. 256
void CheckTransparency(uint32_t level, Thread_t* t);  // 0.. 256

// table initialisers, one per family (static data in the original); Vm_TablesInit calls them
void Vm_OptableMainInit(void);
void Vm_Optable80Init(void);
void Vm_Optable81Init(void); // ops_sys81.c (1.69/472 on)
void Vm_Optable90Init(void);
void Vm_Optable91Init(void);
void Vm_Optable92Init(void);
void Vm_OptableA0Init(void);
void Vm_OptableB0Init(void);
void Vm_OptableC0Init(void);
void Vm_OptableEFInit(void);       // ops_obgi.c: the OpenBGI extensions
int Opcode_Sys_Yield(Thread_t* t); // "80 5F": end the turn (scheduler code 1); in the original also the base wait class's poll

// user-defined instructions "FF xx" (userop.c)
void UserOp_Init(void);  // clear the slot table, at start-up
void UserOp_Clear(void); // unregister every slot (part of the machine's reset)

// memory watch points ("74" switches the write checks, "75" registers a region; ops_base.c)
void Watch_Clear(void);       // drop every registered region
int Opcode_User(Thread_t* t); // "FF xx": dispatch a user-defined instruction (userop.c)

#endif // BGI_VM_H_
