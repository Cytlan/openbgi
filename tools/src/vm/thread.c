/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * thread.c - script threads (Thread_t), the buffers they are made of
 *            (PixBuf_t, inc/bgi/pixbuf.h) and the thread-local heap
 *            (Heap_t); the interface is inc/bgi/vm.h
 *
 * A thread owns three PixBuf-backed areas (evaluation stack, code, data),
 * a heap for "70" / "71", a list of the modules loaded into its code area,
 * a timer, a message queue and an optional wait object.  Threads form a
 * singly linked list behind gRootThread, a buffer-less head that never
 * runs; the scheduler (sched.c) walks that list.  Nothing here executes
 * instructions: the fetch, stack and frame primitives are the inline
 * functions of vm.h.
 */
#include "bgi/vm.h"
#include "bgi/dbg.h"
#include "bgi/wait.h"
#include "bgi/sys.h"

// -------------------------------------------------------------------------
// PixBuf
// -------------------------------------------------------------------------

void PixBuf_Ctor(PixBuf_t* b)
{
	b->allocated = 0;
	b->data = NULL;
	b->size = 0;
}

// release the allocation, if any; returns whether there was one
int PixBuf_Free(PixBuf_t* b)
{
	int was = b->allocated;
	if(was)
	{
		BGI_Free(b->data);
		b->data = NULL;
		b->size = 0;
		b->allocated = 0;
	}
	return was;
}

void PixBuf_Dtor(PixBuf_t* b)
{
	PixBuf_Free(b);
}

// n uninitialised bytes; NULL (nothing changes) while a previous allocation is still held
uint8_t* PixBuf_Alloc(PixBuf_t* b, uint32_t n)
{
	if(b->allocated)
		return NULL;
	b->data = (uint8_t*)BGI_Alloc(n);
	b->size = n;
	b->allocated = 1;
	return b->data;
}

// -------------------------------------------------------------------------
// Heap: growable arena with free / used record lists
// -------------------------------------------------------------------------

// an arena of `size` bytes with one free record covering all of it
Heap_t* Heap_Create(uint32_t size)
{
	Heap_t* h = (Heap_t*)BGI_Calloc(sizeof(Heap_t));
	HeapNode_t* n = (HeapNode_t*)BGI_Alloc(sizeof(HeapNode_t));
	h->size = size;
	h->base = (uint8_t*)BGI_Alloc(size);
	n->off = 0;
	n->size = size;
	n->next = NULL;
	h->freeHead = n;
	h->usedHead = NULL;
	return h;
}

// free both record lists, the arena and the heap object
void Heap_Destroy(Heap_t* h)
{
	HeapNode_t *n, *nx;
	for(n = h->freeHead; n; n = nx)
	{
		nx = n->next;
		BGI_Free(n);
	}
	for(n = h->usedHead; n; n = nx)
	{
		nx = n->next;
		BGI_Free(n);
	}
	BGI_Free(h->base);
	BGI_Free(h);
}

// merge free records that are adjacent in the arena
static void Heap_Coalesce(Heap_t* h)
{
	HeapNode_t *n = h->freeHead, *nx;
	if(!n)
		return;
	nx = n->next;
	while(nx)
	{
		if(n->off + n->size == nx->off)
		{
			n->size += nx->size;
			n->next = nx->next;
			BGI_Free(nx);
			nx = n->next;
		}
		else
		{
			n = nx;
			nx = nx->next;
		}
	}
}

/*
 * Allocate n bytes and return the offset of the block.  First fit over the
 * free list; when nothing fits the arena is doubled (repeatedly, until the
 * new tail can hold n) and moved, which invalidates every pointer obtained
 * from Heap_Ptr before.  The arena is never shrunk.  A block of 0 bytes is
 * a valid request: it takes the offset of the first free record without
 * consuming anything.
 */
uint32_t Heap_Alloc(Heap_t* h, uint32_t n)
{
	for(;;)
	{
		HeapNode_t** link = &h->freeHead;
		HeapNode_t* f = h->freeHead;
		// first fit, in ascending offset order
		while(f && n > f->size)
		{
			link = &f->next;
			f = f->next;
		}
		if(f)
		{
			HeapNode_t* u = (HeapNode_t*)BGI_Alloc(sizeof(HeapNode_t));
			u->off = f->off;
			u->size = n;
			u->next = h->usedHead;
			h->usedHead = u;
			if(n < f->size)
			{ // shrink the free record
				f->size -= n;
				f->off += n;
			}
			else
			{ // exact fit: remove it
				*link = f->next;
				BGI_Free(f);
			}
			return u->off;
		}
		// nothing fits: double the arena until the new tail can hold n
		{
			uint32_t oldSize = h->size, newSize = oldSize * 2;
			uint8_t* nb;
			HeapNode_t* tail;
			while(newSize - oldSize < n)
				newSize *= 2;
			nb = (uint8_t*)BGI_Alloc(newSize);
			memcpy(nb, h->base, oldSize);
			BGI_Free(h->base);
			tail = (HeapNode_t*)BGI_Alloc(sizeof(HeapNode_t));
			tail->off = oldSize;
			tail->size = newSize - oldSize;
			tail->next = NULL;
			*link = tail; // link == end of the free list
			h->size = newSize;
			h->base = nb;
			Heap_Coalesce(h);
		}
		// the original retries only when the list was non-empty before
		// growing; with an empty list the new tail record satisfies the
		// request on the next iteration in both cases
	}
}

/*
 * Free the block that starts at offset `off`.  Returns 1 when such a block
 * was allocated; 0 when no used record starts there, in which case nothing
 * changes ("71" turns that into a script error).  The record moves to the
 * free list, which is kept sorted by offset so neighbours can be merged.
 */
int Heap_Free(Heap_t* h, uint32_t off)
{
	HeapNode_t **link = &h->usedHead, *u = h->usedHead, **flink, *f;
	while(u && u->off != off)
	{
		link = &u->next;
		u = u->next;
	}
	if(!u)
		return 0;
	*link = u->next; // unlink from used
	// insert into the free list, sorted by offset
	flink = &h->freeHead;
	f = h->freeHead;
	while(f && f->off <= u->off)
	{
		flink = &f->next;
		f = f->next;
	}
	u->next = f;
	*flink = u;
	Heap_Coalesce(h);
	return 1;
}

// the address of offset `off`; not checked against the arena size
uint8_t* Heap_Ptr(Heap_t* h, uint32_t off)
{
	return h->base + off;
}

// -------------------------------------------------------------------------
// Thread
// -------------------------------------------------------------------------

Thread_t* gRootThread;  // the list head (id 0, no buffers); created by VmMain
uint32_t gThreadSerial; // the id the next Thread_New hands out

/*
 * Allocate a thread.  With all three sizes non-zero it gets an evaluation
 * stack of stackEntries values, a code area of codeSize bytes, a data area
 * of dataSize bytes and a local heap of 0x8000 bytes (which grows on
 * demand), and `valid` is 1.  With any size 0 the result is the buffer-less
 * root thread (`valid` 0), only good as a list head.  Either way the thread
 * takes the next serial number as its id and is not linked anywhere.
 */
Thread_t* Thread_New(uint32_t stackEntries, uint32_t codeSize, uint32_t dataSize)
{
	Thread_t* t = (Thread_t*)BGI_Calloc(sizeof(Thread_t));

	if(stackEntries && codeSize && dataSize)
	{
		t->stackEntries = stackEntries;
		t->stackBuf = (PixBuf_t*)BGI_Alloc(sizeof(PixBuf_t));
		PixBuf_Ctor(t->stackBuf);
		t->stack = (uint32_t*)PixBuf_Alloc(t->stackBuf, stackEntries * 4);
		t->codeSize = codeSize;
		t->codeBuf = (PixBuf_t*)BGI_Alloc(sizeof(PixBuf_t));
		PixBuf_Ctor(t->codeBuf);
		t->code = PixBuf_Alloc(t->codeBuf, codeSize);
		t->dataSize = dataSize;
		t->dataBuf = (PixBuf_t*)BGI_Alloc(sizeof(PixBuf_t));
		PixBuf_Ctor(t->dataBuf);
		t->data = PixBuf_Alloc(t->dataBuf, dataSize);
		t->heap = Heap_Create(0x8000);
		t->valid = 1;
	}
	else
	{
		// the root thread: list head only, no buffers
		t->valid = 0;
	}
	t->id = gThreadSerial++;
	t->next = NULL;
	t->modules = NULL;
	Thread_FreeModules(t);
	t->flags = 0;
	t->sp = 0;
	t->opIP = 0;
	t->ip = 0;
	t->fp = 0;
	t->wait = NULL;
	t->msgHead = NULL;
	return t;
}

/*
 * Destroy a thread: its three areas and heap (when it has them), the module
 * records, the wait object it may be blocked in and the unread messages of
 * its queue, then the thread itself.  The caller unlinks it from the list
 * first (Thread_KillChild does both).
 */
void Thread_Delete(Thread_t* t)
{
	uint32_t dummy;
	if(t->valid)
	{
		if(t->stackBuf)
		{
			PixBuf_Dtor(t->stackBuf);
			BGI_Free(t->stackBuf);
		}
		if(t->codeBuf)
		{
			PixBuf_Dtor(t->codeBuf);
			BGI_Free(t->codeBuf);
		}
		if(t->dataBuf)
		{
			PixBuf_Dtor(t->dataBuf);
			BGI_Free(t->dataBuf);
		}
		if(t->heap)
		{
			Heap_Destroy(t->heap);
		}
	}
	Thread_FreeModules(t);
	if(t->wait)
		Wait_Release(t->wait);
	while(Thread_ListPopFront(t, &dummy))
		;
	BGI_Free(t);
}

// a new thread (Thread_New) appended at the end of the list `root` heads; returns it
Thread_t* Thread_SpawnChild(Thread_t* root, uint32_t stackEntries, uint32_t codeSize, uint32_t dataSize)
{
	// append at the end of the list (recursive in the original)
	Thread_t* t = root;
	while(t->next)
		t = t->next;
	t->next = Thread_New(stackEntries, codeSize, dataSize);
	return t->next;
}

// unlink `victim` from the list behind `root` and delete it; 0 when it is not in that list
int Thread_KillChild(Thread_t* root, Thread_t* victim)
{
	Thread_t *prev = root, *t = root->next;
	while(t && t != victim)
	{
		prev = t;
		t = t->next;
	}
	if(!t)
		return 0;
	prev->next = Thread_Next(victim);
	Thread_Delete(victim);
	return 1;
}

// delete every thread behind `root`, leaving `root` alone in the list
void Thread_KillAllChildren(Thread_t* root)
{
	// recursive in the original: the last thread is destroyed first
	if(root->next)
	{
		Thread_KillAllChildren(root->next);
		Thread_Delete(root->next);
		root->next = NULL;
	}
}

/* the thread with that id in the list `root` heads, or NULL.  Id 0 is the
 * root thread's own, and asking for it yields NULL: scripts never get a
 * handle to the root ("80 47" reports it as not existing). */
Thread_t* Thread_FindById(Thread_t* root, uint32_t id)
{
	Thread_t* t = root;
	for(;;)
	{
		if(Thread_GetId(t) == id)
			return id != 0 ? t : NULL; // id 0 (the root) is never found
		if(!t->next)
			return NULL;
		t = t->next;
	}
}

// drop every module record (names included); the code area counts as empty afterwards
void Thread_FreeModules(Thread_t* t)
{
	ModuleNode_t *m = t->modules, *nx;
	while(m)
	{
		nx = m->next;
		BGI_Free(m->name);
		BGI_Free(m);
		m = nx;
	}
	t->modules = NULL;
	t->moduleCount = 0;
	t->codeUsed = 0;
}

/*
 * Append a program to the code area ("80 40", the first program of a
 * thread, the user-defined instructions of userop.c).  `file` is the image
 * of a "._bp" file: its ProgramHeader_t says where the code starts and how
 * long it is; `name` is kept (copied) for error messages and the trace.
 * Returns the offset of the module's first byte in the code area, or
 * 0x80000000 when the room left in the area is too small for it.
 */
uint32_t Thread_LoadModule(Thread_t* t, const void* file, const char* name)
{
	const ProgramHeader_t* h = (const ProgramHeader_t*)file;
	ModuleNode_t* m;
	if(t->codeUsed + h->codeSize > t->codeSize)
		return 0x80000000u;
	m = (ModuleNode_t*)BGI_Alloc(sizeof(ModuleNode_t));
	m->name = BGI_Strdup(name);
	m->size = h->codeSize;
	m->base = t->codeUsed;
	m->next = t->modules;
	t->modules = m;
	memcpy(t->code + m->base, (const uint8_t*)file + h->codeOffset, h->codeSize);
	t->codeUsed += h->codeSize;
	t->moduleCount++;
	Dbg_Log("thread %u: module %s at %x (%u bytes)", (unsigned)t->id, name, (unsigned)m->base, (unsigned)m->size);
	return m->base;
}

/* drop the module loaded last and give its bytes back to the code area
 * ("80 41"); returns the number of modules left, or 0x80000001 when there
 * was none to drop */
uint32_t Thread_UnloadLastModule(Thread_t* t)
{
	ModuleNode_t* m = t->modules;
	if(!m)
		return 0x80000001u;
	t->modules = m->next;
	t->codeUsed -= m->size;
	t->moduleCount--;
	BGI_Free(m->name);
	BGI_Free(m);
	return t->moduleCount;
}

/*
 * Copy the module records into a fresh array, most recently loaded first,
 * with their `next` links cleared; *out gets the array (the caller frees it
 * with BGI_Free) or NULL when the thread has no module.  Returns the number
 * of entries.  The names in the copies still belong to the thread.
 */
uint32_t Thread_ExportModules(Thread_t* t, ModuleNode_t** out)
{
	ModuleNode_t *m, *dst;
	uint32_t i = 0;
	if(t->moduleCount == 0)
	{
		*out = NULL;
		return 0;
	}
	dst = (ModuleNode_t*)BGI_Alloc(sizeof(ModuleNode_t) * t->moduleCount);
	*out = dst;
	for(m = t->modules; m; m = m->next)
	{
		dst[i] = *m;
		dst[i].next = NULL;
		i++;
	}
	return t->moduleCount;
}

// read n operand bytes from the code stream into dst; 0 (and nothing read) when they would reach past the code area
int RdBytes(Thread_t* t, void* dst, uint32_t n)
{
	if(t->ip + n > t->codeSize)
		return 0;
	memcpy(dst, t->code + t->ip, n);
	t->ip += n;
	return 1;
}

// install the wait object `w` (the thread owns it from now on; one still installed is released first) and mark the thread blocked
void Thread_SetWait(Thread_t* t, struct Wait* w)
{
	if(t->wait)
		Wait_Release(t->wait);
	t->wait = w;
	Thread_SetFlags(t, THR_FLAG_WAITING);
}

/*
 * Poll the wait object the thread is blocked in: 0 while it keeps waiting,
 * 1 when it finished, -1 when it failed (also -1 when there is no wait
 * object).  On 1 and -1 the object is released and the thread unblocked;
 * the object's poll may have pushed results on the thread's stack.
 */
int Thread_PollWait(Thread_t* t)
{
	int r;
	if(!t->wait)
		return -1;
	r = t->wait->vt->poll(t->wait);
	if(r == 1 || r == -1)
	{
		if(t->wait)
			Wait_Release(t->wait);
		t->wait = NULL;
		Thread_ClrFlags(t, THR_FLAG_WAITING);
	}
	return r;
}

// queue the notification (a, b, c) on the thread's wait object ("80 4C"); 1 when it had one, 0 when the thread was not waiting
int Thread_WaitNotify(Thread_t* t, uint32_t a, uint32_t b, uint32_t c)
{
	if(t->wait)
		Wait_Notify(t->wait, a, b, c);
	return t->wait != NULL;
}

// milliseconds until the thread timer expires, 0 once it has
uint32_t Thread_TimerRemaining(Thread_t* t)
{
	int32_t rem = (int32_t)(t->timerDeadline - GetTicks());
	return rem > 0 ? (uint32_t)rem : 0;
}

// the thread timer expires `ms` milliseconds from now ("80 58")
void Thread_TimerSet(Thread_t* t, uint32_t ms)
{
	t->timerDeadline = GetTicks() + ms;
}

// append v to the end of the thread's message queue ("80 48", "80 4A")
void Thread_ListAppend(Thread_t* t, uint32_t v)
{
	ThreadMsg_t** link = &t->msgHead;
	ThreadMsg_t* m;
	while(*link)
		link = &(*link)->next;
	m = (ThreadMsg_t*)BGI_Alloc(sizeof(ThreadMsg_t));
	m->value = v;
	m->next = NULL;
	*link = m;
}

// take the oldest message of the queue into *v ("80 49", "80 4B"); 0 when the queue is empty
int Thread_ListPopFront(Thread_t* t, uint32_t* v)
{
	ThreadMsg_t* m = t->msgHead;
	if(!m)
		return 0;
	*v = m->value;
	t->msgHead = m->next;
	BGI_Free(m);
	return 1;
}
