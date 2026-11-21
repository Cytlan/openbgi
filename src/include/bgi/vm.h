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
 * while it is blocked, a wait object.  The scheduler (src/vm/sched.c) gives
 * every thread of the list a turn per pass, one pass per frame
 * (OpenBGI_RunFrame, openbgi.h); each instruction handler
 * is a VmHandler_t that works on the thread and returns a scheduler code.
 * The handler tables are filled at start-up for the engine version in use
 * (Vm_TablesInit), so a script only sees the instructions its build had.
 *
 * Implemented in src/vm/thread.c (threads, heap), sched.c (the main loop),
 * ops_*.c (the handlers and their tables), trace.c (the development aids),
 * checks.c (operand validators) and userop.c (the "FF xx" instructions).
 * docs/dev/vm.md describes the machinery; the handlers' signature comments in
 * ops_*.c are the instruction-by-instruction reference.
 */
#ifndef BGI_VM_H_
#define BGI_VM_H_

#include "bgi/common.h"
#include "openbgi.h" // OpenBGI_Frame_t

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
struct Thread;

typedef struct AreaLoan // a part of a thread's code or data area lent to another thread ("81 44")
{
	struct Thread* holder; // the thread that runs in it
	uint32_t off;          // its first byte in the area
	uint32_t size;         // bytes
	struct AreaLoan* next; // the loan below it: the list runs from the top of the area down
} AreaLoan_t;

typedef struct Thread // 0x64 bytes in the original (0x90 from 1.599)
{
	int32_t valid;          // 1 when the buffers below were allocated (0 for the root thread)
	uint32_t id;            // serial number; the handle scripts see
	struct Thread* next;    // singly linked list, in creation order
	uint32_t flags;         // THR_FLAG_*
	uint32_t sp;            // evaluation stack index of the next push; wraps at stackEntries
	uint32_t opIP;          // offset of the instruction being executed
	uint32_t opSP;          // sp when it started (not in the original: an instruction started over, Vm_Frame)
	uint32_t ip;            // read cursor: the next operand byte
	uint32_t fp;            // frame pointer: byte offset into the data area
	uint32_t stackEntries;  // capacity of `stack`, entries
	PixBuf_t* stackBuf;     // owns `stack`
	uint32_t* stack;        // the evaluation stack
	uint8_t* stackMark;     // beside every value of it: 1 when it comes from the engine's own program file (follow.h)
	uint32_t codeSize;      // the end of the part of the code area this thread may use, bytes from `code`
	PixBuf_t* codeBuf;      // owns `code`
	uint8_t* code;          // the code area: the loaded modules, back to back
	ModuleNode_t* modules;  // most recently loaded first
	uint32_t moduleCount;   // length of `modules`
	uint32_t codeUsed;      // bytes of the code area the modules occupy
	uint32_t dataSize;      // the end of the part of the data area this thread may use, bytes from `data`
	PixBuf_t* dataBuf;      // owns `data`
	uint8_t* data;          // call stack and locals
	Heap_t* heap;           // local heap, 0x8000 bytes initially
	struct Wait* wait;      // owned; polled while THR_FLAG_WAITING is set
	uint32_t timerDeadline; // the thread timer ("80 58" .. "80 5A"), in GetTicks milliseconds
	ThreadMsg_t* msgHead;   // the message queue, oldest first (the original keeps a list pseudo-node in front of it)
	// a thread of "81 44" (1.599 on) runs in parts of another thread's areas:
	struct Thread* lender; // the thread whose code area, data area and heap it uses; NULL when it has its own
	uint32_t codeBase;     // the start of its part of the code area, where its own modules go (0 with its own area)
	uint32_t dataBase;     // the start of its part of the data area, where its frame stack begins
	// and a thread with areas of its own lends them out from the top down:
	uint32_t codeCap;      // the size of its code area
	uint32_t dataCap;      // the size of its data area
	AreaLoan_t* codeLoans; // the parts of the code area lent out, highest first
	AreaLoan_t* dataLoans; // the parts of the data area lent out, highest first
	uint32_t loanCount;    // the threads holding them
} Thread_t;

#define THR_FLAG_WAITING 1u // blocked on `wait`: the scheduler polls it instead of running the thread

/* Allocate a thread with a stack of stackEntries values, a code area of
 * codeSize bytes and a data area of dataSize bytes; with any of the three
 * 0 the result is a buffer-less list head (the root thread).  The thread
 * is not linked into any list. */
Thread_t* Thread_New(OpenBGI_t* o, uint32_t stackEntries, uint32_t codeSize, uint32_t dataSize);
/* release the buffers, modules, wait object and messages, then the thread
 * itself (not unlinked); a thread in borrowed areas gives them back */
void Thread_Delete(OpenBGI_t* o, Thread_t* t);
// Thread_New appended to the end of the list that starts at `root`
Thread_t* Thread_SpawnChild(OpenBGI_t* o, Thread_t* root, uint32_t stackEntries, uint32_t codeSize, uint32_t dataSize);
/* "81 44" (1.599 on): a thread with a stack of stackEntries values of its
 * own that runs in codeSize and dataSize bytes cut from the tops of the
 * code and data areas of t's lender (t itself when t has areas of its
 * own) and uses the lender's heap; it starts at code offset ip with its
 * frame stack at the start of its part of the data area, and is appended
 * at the end of t's list.  0 with the thread in *out; 2 when the code
 * area has no room for the part above the modules loaded in it, 3 when
 * the data area has none (nothing is made then). */
int Thread_SpawnBorrower(OpenBGI_t* o, Thread_t* t, uint32_t stackEntries, uint32_t codeSize, uint32_t dataSize, uint32_t ip, Thread_t** out);
// unlink and delete `t` from root's list, the threads that run in areas it lent first; 1 when it was found
int Thread_KillChild(OpenBGI_t* o, Thread_t* root, Thread_t* t);
// delete every thread after `root`, the last one first
void Thread_KillAllChildren(OpenBGI_t* o, Thread_t* root);
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
uint32_t Thread_LoadModule(OpenBGI_t* o, Thread_t* t, const void* file, const char* name);
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
	t->opSP = t->sp;
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
	t->stackMark[t->sp] = 0;
	t->sp = (t->sp + 1 < t->stackEntries) ? t->sp + 1 : 0;
}

// the mark of the value the next Thread_Pop returns (follow.h)
static inline uint8_t Thread_TopMark(Thread_t* t)
{
	return t->stackMark[(t->sp ? t->sp : t->stackEntries) - 1];
}

// Thread_Push of a value with its mark
static inline void Thread_PushMarked(Thread_t* t, uint32_t v, uint8_t mark)
{
	uint32_t i = t->sp;
	Thread_Push(t, v);
	t->stackMark[i] = mark;
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

/* wait objects (src/include/bgi/wait.h): a handler that has to block hands the
 * thread a wait object and returns scheduler code 2.  The thread owns the
 * object and releases it when the wait ends. */
void Thread_SetWait(OpenBGI_t* o, Thread_t* t, struct Wait* w);         // install `w` (releasing a previous one) and set THR_FLAG_WAITING
int Thread_PollWait(OpenBGI_t* o, Thread_t* t);                         // poll it: 0 still waiting, 1 done, -1 failed (1 and -1 release it and clear the flag)
void Thread_DropWait(OpenBGI_t* o, Thread_t* t);                        // release the wait unfinished and clear the flag (nothing when the thread does not wait)
int Thread_WaitNotify(Thread_t* t, uint32_t a, uint32_t b, uint32_t c); // "80 4C": queue (a, b, c) on the wait object; 1 when the thread had one, 0 otherwise

// per-thread timer ("80 58" .. "80 5A"), in milliseconds
uint32_t Thread_TimerRemaining(OpenBGI_t* o, Thread_t* t);    // milliseconds until the deadline, 0 once it has passed
void Thread_TimerSet(OpenBGI_t* o, Thread_t* t, uint32_t ms); // the deadline is `ms` from now
static inline void Thread_TimerAdd(Thread_t* t, uint32_t ms)  // move the deadline `ms` later
{
	t->timerDeadline += ms;
}

// per-thread message queue ("80 48" .. "80 4B")
void Thread_ListAppend(Thread_t* t, uint32_t v);   // queue v behind the other messages
int Thread_ListPopFront(Thread_t* t, uint32_t* v); // the oldest message into *v; 0 when the queue is empty

// -------------------------------------------------------------------------
// Instruction handlers and the scheduler
// -------------------------------------------------------------------------

/* Every handler returns a scheduler code (docs/dev/vm.md):
 *  0 next instruction, 1/2 end of turn (2: the thread was put into a
 *  wait), 3 switch to the thread in o->vm.switchThreadId, 4 end thread,
 *  5 reboot, 6 quit. */
typedef int (*VmHandler_t)(OpenBGI_t* o, Thread_t* t);

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
void Vm_FillTable(OpenBGI_t* o, VmHandler_t* table, OpFamily_t fam, const OpEntry_t* list, size_t n);
int Opcode_NotImplemented(OpenBGI_t* o, Thread_t* t); // the stub for defined-but-unimplemented opcodes: a script error naming the opcode

void Vm_TablesInit(OpenBGI_t* o);             // fill the handler tables for the selected engine profile; once, before the first instruction
void Vm_ListMissing(OpenBGI_t* o, FILE* out); // --list-missing: the instructions of the profile without a handler

/* The development tracing aids of src/vm/trace.c (BGI_TRACE, BGI_TRACE_OPS,
 * BGI_TRACE_LAST, BGI_OPSTAT in the environment; not part of the original). */
void Vm_TraceInit(OpenBGI_t* o);                            // read the environment, once before the first instruction
void Vm_TraceBefore(OpenBGI_t* o, Thread_t* t, uint8_t op); // before thread t executes instruction op
void Vm_TraceAfter(OpenBGI_t* o, Thread_t* t);              // after it ran
void Vm_TraceDump(OpenBGI_t* o);                            // BGI_TRACE_LAST: print the ring of the last instructions; MsgBox calls it before any message box
void Vm_TraceShutdown(OpenBGI_t* o);                        // BGI_OPSTAT: write the counts, when the machine stops

/* Handlers that older builds had in another family (the window operations
 * of 1.58 / 1.64 lived in 90 8x and 91 8x before the 92 family took them
 * over, docs/dev/opcodes.md); shared between the
 * ops_gfx*.c tables. */
int Opcode_Gfx0_WindowSetPunch(OpenBGI_t* o, Thread_t* t);    // 90 87, 1.58 .. 1.64: 90 8B
int Opcode_Gfx1_WindowSetFont(OpenBGI_t* o, Thread_t* t);     // 91 88, 1.58 .. 1.64: 90 87
int Opcode_Gfx1_WindowSetSpacing(OpenBGI_t* o, Thread_t* t);  // 91 89, 1.58 .. 1.64: 90 8A
int Opcode_Gfx1_WindowGetCursor(OpenBGI_t* o, Thread_t* t);   // 91 8D, 1.58 .. 1.64: 91 8F
int Opcode_Gfx2_WindowShowFrame(OpenBGI_t* o, Thread_t* t);   // 92 88, 1.58 .. 1.64: 90 8C
int Opcode_Gfx2_WindowDrawToFrame(OpenBGI_t* o, Thread_t* t); // 92 89, 1.58 .. 1.64: 90 8D
int Opcode_Gfx2_WindowShowText(OpenBGI_t* o, Thread_t* t);    // 92 8C, 1.58 .. 1.64: 91 8C
int Opcode_Gfx2_WindowDrawToText(OpenBGI_t* o, Thread_t* t);  // 92 8D, 1.58 .. 1.64: 91 8D
int Opcode_Gfx2_WindowClear(OpenBGI_t* o, Thread_t* t);       // 92 8E, 1.58 .. 1.64: 91 8E

/* the original's main loop, driven by the host (OpenBGI_RunFrame in
 * openbgi.h runs a frame): Vm_Start creates the root thread and boots;
 * Vm_Frame runs a pass and its services, or the rest of a pass that
 * waited, and fills *f; 0 once the run is over; Vm_Stop deletes the root
 * thread */
void Vm_Start(OpenBGI_t* o);
int Vm_Frame(OpenBGI_t* o, OpenBGI_Frame_t* f);
void Vm_Stop(OpenBGI_t* o);
/* Memory an instruction holds across a load that may ask for the disc
 * (RetryPrompt, bgi/file.h): such an instruction is started over once the
 * disc is there, and what it held is freed then, as after any error that
 * leaves the pass.  Vm_TempFree frees a block, Vm_TempKeep keeps it past
 * the instruction (it is the caller's then); both take untracked blocks
 * as well. */
void* Vm_TempAlloc(OpenBGI_t* o, size_t size);
void Vm_TempFree(OpenBGI_t* o, void* p);
void Vm_TempKeep(OpenBGI_t* o, void* p);
/* load program `name` of archive `arc` into a new thread appended to the
 * list that `root` is part of ("80 44", the boot thread); the sizes are
 * those of Thread_New.  Returns the thread's id; a missing program or one
 * that does not fit the code area is a script error raised on `root`. */
uint32_t SpawnThread(OpenBGI_t* o, const char* arc, const char* name, uint32_t stackEntries,
	uint32_t codeSize, uint32_t dataSize, Thread_t* root);
// make `t` the only thread the scheduler runs ("80 5D"); NULL lifts the restriction (`on` is ignored)
void SetExclusiveThread(OpenBGI_t* o, Thread_t* t, int on);

// script error for sizes above the profile's heap limit (32 MB in 1.69; "70", "80 20")
void CheckAllocSize(OpenBGI_t* o, uint32_t size, Thread_t* t);

/* operand validators of the graphics / sound families (src/vm/checks.c);
 * each raises a script error and never returns when the value is out of
 * range */
void CheckSeSlot(OpenBGI_t* o, uint32_t slot, Thread_t* t);        // 0.. 63
void CheckPan(OpenBGI_t* o, int32_t pan, Thread_t* t);             // 0.. 128
void CheckVolume(OpenBGI_t* o, int32_t volume, Thread_t* t);       // 0.. 128
void CheckBgmChannel(OpenBGI_t* o, uint32_t channel, Thread_t* t); // 0.. 15
void CheckDivCount(OpenBGI_t* o, int32_t n, Thread_t* t);          // 1.. 256
void CheckFontNo(OpenBGI_t* o, int32_t no, Thread_t* t);           // a number FontNameByNo knows
// the bitmap table: 4096 entries, 16384 from 1.69 build 472 on (the manager always has 16384 here)
#define BMP_NO_LIMIT(o) (o->engine.profile->gen >= GEN_1_69_472 ? 0x4000 : 0x1000)
void CheckBitmapNo(OpenBGI_t* o, int32_t no, Thread_t* t);          // 0.. BMP_NO_LIMIT - 1
void CheckPriority(OpenBGI_t* o, uint32_t prio, Thread_t* t);       // 0.. 4095; 0 .. 65535 from 1.529 on
void ErrBitmapNotRegistered(OpenBGI_t* o, int32_t no, Thread_t* t); // always raises
void CheckEffectMode(OpenBGI_t* o, uint32_t mode, Thread_t* t);     // one of the blitters' effect modes (checks.c lists them)
void CheckAlpha(OpenBGI_t* o, uint32_t level, Thread_t* t);         // 0.. 256
void CheckMixRatio(OpenBGI_t* o, uint32_t ratio, Thread_t* t);      // 0.. 256
void CheckTransparency(OpenBGI_t* o, uint32_t level, Thread_t* t);  // 0.. 256

// table initialisers, one per family (static data in the original); Vm_TablesInit calls them
void Vm_OptableMainInit(OpenBGI_t* o);
void Vm_Optable80Init(OpenBGI_t* o);
void Vm_Optable81Init(OpenBGI_t* o); // ops_sys81.c (1.69/472 on)
/* A loader instruction read a range of a file into buf with the result
 * `code` (0 done): when the file is the engine's own program file those
 * bytes are followed (follow.h).  off = size = 0 is all of it. */
void Vm_FollowRange(OpenBGI_t* o, void* buf, const char* arc, const char* name, uint32_t off, uint32_t size, int code);
void Vm_Optable90Init(OpenBGI_t* o);
void Vm_Optable91Init(OpenBGI_t* o);
void Vm_Optable92Init(OpenBGI_t* o);
void Vm_OptableA0Init(OpenBGI_t* o);
void Vm_OptableB0Init(OpenBGI_t* o);
void Vm_OptableC0Init(OpenBGI_t* o);
void Vm_OptableEFInit(OpenBGI_t* o);             // ops_obgi.c: the OpenBGI extensions
int Opcode_Sys_Yield(OpenBGI_t* o, Thread_t* t); // "80 5F": end the turn (scheduler code 1); in the original also the base wait class's poll

// user-defined instructions "FF xx" (userop.c)
#define USEROP_SLOTS 0xf0        // the user opcodes' slots 00 .. EF; F0 .. FF are the management sub-opcodes
void UserOp_Init(OpenBGI_t* o);  // clear the slot table, at start-up
void UserOp_Clear(OpenBGI_t* o); // unregister every slot (part of the machine's reset)

// memory watch points ("74" switches the write checks, "75" registers a region; ops_base.c)
void Watch_Clear(OpenBGI_t* o);             // drop every registered region
int Opcode_User(OpenBGI_t* o, Thread_t* t); // "FF xx": dispatch a user-defined instruction (userop.c)

/* the scheduler code of a turn cut short (not in the original): an
 * instruction asked the host or holds the engine (Vm_Frame), and the turn
 * goes on in a later frame */
#define SCHED_CUT   7
#define VM_TEMP_MAX 8 // the blocks of Vm_TempAlloc tracked at once

// the scheduler's flags: locals of the original's main loop that its "catch" blocks modify (o->vm.sched)
typedef struct SchedFlags
{
	int restart;   // boot again when the list empties (scheduler code 5)
	int running;   // threads may execute instructions; 0 ends the machine once the counted waits are done
	Thread_t* cur; // thread whose turn it is (NULL between passes)
	int bootOk;    // the boot thread was created
} SchedFlags_t;

#endif // BGI_VM_H_
