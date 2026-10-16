/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * wait_load.c - the background file loads
 *
 * Interface: waitobj.h (the constructors).
 *
 *   WaitLoader     base: a file read by the loader thread into a 32 MB buffer
 *   WaitBmpLoad    "90 10": the file becomes a bitmap slot; pushes nothing
 *   WaitSeLoad     "A0 20" / "A0 21": the file becomes a sound effect slot;
 *                  pushes nothing
 *   WaitSeqLoad    "90 F4" (1.69/472 on): the file becomes a BF_Movie
 *                  sequence record; pushes 0 ok / 1 / 2 not a movie
 *   WaitSeqDecode  "90 F6" after "80 53": the frame is decoded on the next
 *                  pass instead of at once; pushes 0 / 3 / 5 / 8
 *
 * The constructor lower-cases both names, consults the file cache when the
 * caller allows it and queues the read; poll() waits for the loader's status
 * word and then runs the subclass's finish (a vtable slot behind the base
 * table), which turns the data into the slot and raises the script errors.
 * An archive name of "0" stands for "no archive" (a loose file).  These are
 * counted waits: the machine does not shut down while one is pending.
 */
#include "bgi/wait.h"
#include "bgi/waitobj.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/file.h"
#include "bgi/strutil.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/gfx/bmseq.h"

typedef struct WaitLoader // 0x238 bytes
{
	Wait_t w;         // (WaitCounted)
	uint8_t* buf;     // 32 MB file buffer
	char arc[0x104];  // lower-cased archive name ("" for none)
	char name[0x104]; // lower-cased file name
	uint32_t status;  // loader status: 0 pending, -1 failed, else the size in bytes
	int useCache;     // the file cache is consulted / fed
	int cacheHit;     // the lookup succeeded (nothing was queued)
} WaitLoader_t;

typedef struct WaitBmpLoad // 0x23c bytes
{
	WaitLoader_t l;
	int bmpNo; // the destination bitmap slot
} WaitBmpLoad_t;

typedef struct WaitSeLoad // 0x250 bytes
{
	WaitLoader_t l;
	int slot;         // the destination sound effect slot
	int fadeInMs;     // a fade-in burnt into the samples ("A0 21")
	double gain;      // sample multiplier (1.0 for "A0 20")
	int queued;       // the data has been handed to the sound loader
	uint32_t status2; // the sound loader's status: -1 pending, 0 ok, 0x8xxxxxxx error
} WaitSeLoad_t;

typedef struct LoaderVtbl
{
	WaitVtbl_t base;
	int (*finish)(WaitLoader_t* w); // the loaded data is in buf; 1 when the wait is over
} LoaderVtbl_t;

// the archive argument of the file functions: "0" means a loose file
#define ARC_ARG(l) ((l)->arc[0] != '0' ? (l)->arc : NULL)

/* the shared start: copy the names lower-cased, allocate the buffer and
 * either satisfy the read from the file cache (useCache, and the cache
 * exists) or queue it with the loader thread.  A NULL name is a script
 * error, which does not return. */
static void WaitLoader_Ctor(WaitLoader_t* w, Thread_t* t, const char* arc, const char* name, int useCache)
{
	WaitCounted_Ctor(&w->w, t);
	if(!name)
	{
		w->buf = NULL;
		ScriptError(MSG_LOAD_NULL_NAME, t);
	}
	WaitCount_Inc();
	if(arc)
	{
		strcpy(w->arc, arc);
		SjisStrLwr(w->arc);
	}
	else
		w->arc[0] = 0;
	strcpy(w->name, name);
	SjisStrLwr(w->name);
	w->useCache = useCache && gFileCache != NULL;
	w->buf = (uint8_t*)BGI_Alloc(0x2000000);
	if(w->useCache)
	{
		w->cacheHit = FileCache_Lookup(gFileCache, w->buf, &w->status, ARC_ARG(w), w->name) != 0;
		if(w->cacheHit)
			return;
	}
	Loader_QueueFile(&w->status, w->buf, ARC_ARG(w), w->name);
}

static void WaitLoader_Dtor(WaitLoader_t* w)
{
	BGI_Free(w->buf);
	WaitCount_Dec();
	WaitCounted_Dtor(&w->w);
}

/* the shared poll: 0 while the loader has not written the status, -1 (the
 * machine stops) when the read failed, else the file goes into the cache
 * (when it was not from there) and the subclass's finish decides whether
 * the wait is over */
static int WaitLoader_Poll(Wait_t* base)
{
	WaitLoader_t* w = (WaitLoader_t*)base;
	if(!w->buf)
		return -1;
	Wait_Tick(base);
	if(w->status == 0)
		return 0;
	if(w->status == 0xffffffffu)
		return -1;
	if(w->useCache && !w->cacheHit)
		FileCache_Add(gFileCache, ARC_ARG(w), w->name, w->buf, w->status);
	return ((const LoaderVtbl_t*)base->vt)->finish(w) != 0;
}

// ---- bitmaps ---------------------------------------------------------------

static void WaitBmpLoad_Destroy(Wait_t* base)
{
	WaitLoader_Dtor((WaitLoader_t*)base);
	BGI_Free(base);
}

/* decode a Windows BMP into the slot; a file that is not one goes through
 * the engine's own image format instead.  Every decoder error but "the
 * manager refused" is a script error naming the file.  Always finishes. */
static int WaitBmpLoad_Finish(WaitLoader_t* l)
{
	WaitBmpLoad_t* w = (WaitBmpLoad_t*)l;
	Thread_t* t = l->w.thread;
	char msg[0x100];
	switch(BmpOp_DecodeBmp(w->bmpNo, l->buf))
	{
		case(int)0x80000001: // not a BMP
			switch(BmpOp_PutRaw(w->bmpNo, l->buf))
			{
				case 1: // not the engine's format either
					sprintf(msg, MSG_BMP_NOT_BG, l->arc, l->name);
					ScriptError(msg, t);
					break;
				case 2: ScriptError(MSG_BMP_NO_MEMORY, t); break;
				default: break;
			}
			break;
		case(int)0x80000002:
			sprintf(msg, MSG_BMP_NOT_WINDOWS, l->arc, l->name);
			ScriptError(msg, t);
			break;
		case(int)0x80000003:
			sprintf(msg, MSG_BMP_BAD_PLANES, l->arc, l->name);
			ScriptError(msg, t);
			break;
		case(int)0x80000004:
			sprintf(msg, MSG_BMP_BAD_BITCOUNT2, l->arc, l->name);
			ScriptError(msg, t);
			break;
		case(int)0x80000005:
			sprintf(msg, MSG_BMP_COMPRESSED, l->arc, l->name);
			ScriptError(msg, t);
			break;
		case(int)0x80000006:
			sprintf(msg, MSG_BMP_BAD_SIZE, l->arc, l->name);
			ScriptError(msg, t);
			break;
		default: break; // 0 and 0x80000007 (the manager refused) pass silently
	}
	return 1;
}

static const LoaderVtbl_t WaitBmpLoad_Vtbl = {
	{WaitBmpLoad_Destroy, WaitLoader_Poll, Wait_SetTimer, Wait_IsDirtyBase, Wait_OnMessage}, WaitBmpLoad_Finish};

// load file `name` of archive `arc` ("0" = loose file) into bitmap slot bmpNo ("90 10"); uses the file cache
Wait_t* WaitBmpLoad_New(Thread_t* t, int bmpNo, const char* arc, const char* name)
{
	WaitBmpLoad_t* w = (WaitBmpLoad_t*)BGI_Alloc(sizeof *w);
	WaitLoader_Ctor(&w->l, t, arc, name, 1);
	w->l.w.vt = &WaitBmpLoad_Vtbl.base;
	w->bmpNo = bmpNo;
	return &w->l.w;
}

// ---- sound effects ---------------------------------------------------------

static void WaitSeLoad_Destroy(Wait_t* base)
{
	WaitLoader_Dtor((WaitLoader_t*)base);
	BGI_Free(base);
}

/* the first call queues the data with the sound loader; the next ones wait
 * for its status (-1 pending) and report its errors as script errors (not
 * a BW file, not mono, the registration failed) */
static int WaitSeLoad_Finish(WaitLoader_t* l)
{
	WaitSeLoad_t* w = (WaitSeLoad_t*)l;
	Thread_t* t = l->w.thread;
	char msg[0x100];
	if(!w->queued)
	{
		Loader_QueueSe(&w->status2, w->slot, l->buf, w->fadeInMs, w->gain);
		w->queued = 1;
		return 0;
	}
	switch(w->status2)
	{
		case 0xffffffffu: return 0;
		case 0x80000001u:
			sprintf(msg, MSG_SE_NOT_BW, l->arc, l->name);
			ScriptError(msg, t);
			break;
		case 0x80000002u:
			sprintf(msg, MSG_SE_NOT_MONO, l->arc, l->name);
			ScriptError(msg, t);
			break;
		case 0x8fffffffu:
			sprintf(msg, MSG_SE_REGISTER_FATAL, l->arc, l->name);
			ScriptError(msg, t);
			break;
		default: break;
	}
	return 1;
}

static const LoaderVtbl_t WaitSeLoad_Vtbl = {
	{WaitSeLoad_Destroy, WaitLoader_Poll, Wait_SetTimer, Wait_IsDirtyBase, Wait_OnMessage}, WaitSeLoad_Finish};

/* load the sound effect `name` of archive `arc` into `slot` with a fade-in
 * of fadeInMs ms and a sample gain ("A0 20": 0 and 1.0; "A0 21": as given).
 * The file cache is not used for sounds. */
Wait_t* WaitSeLoad_New(Thread_t* t, int slot, const char* arc, const char* name, int fadeInMs, double gain)
{
	WaitSeLoad_t* w = (WaitSeLoad_t*)BGI_Alloc(sizeof *w);
	WaitLoader_Ctor(&w->l, t, arc, name, 0);
	w->l.w.vt = &WaitSeLoad_Vtbl.base;
	w->slot = slot;
	w->fadeInMs = fadeInMs;
	w->gain = gain;
	w->queued = 0;
	return &w->l.w;
}

// ---- sound effects from memory ("A0 28", 1.547 on) --------------------------

typedef struct WaitSeMem
{
	Wait_t w;         // (WaitCounted)
	int slot;         // the destination sound effect slot
	const void* data; // the BW file in the script's memory
	double gain;      // sample multiplier
	int queued;       // the data has been handed to the sound loader
	uint32_t status;  // the sound loader's status: -1 pending, 0 ok, 0x8xxxxxxx error
} WaitSeMem_t;

static void WaitSeMem_Destroy(Wait_t* base)
{
	WaitCount_Dec();
	WaitCounted_Dtor(base);
	BGI_Free(base);
}

/* the first poll queues the data with the sound loader; the next ones wait
 * for its status and report its errors as script errors (the messages
 * name no file: the data came from memory) */
static int WaitSeMem_Poll(Wait_t* base)
{
	WaitSeMem_t* w = (WaitSeMem_t*)base;
	Thread_t* t = base->thread;
	Wait_Tick(base);
	if(!w->queued)
	{
		Loader_QueueSe(&w->status, w->slot, w->data, 0, w->gain);
		w->queued = 1;
		return 0;
	}
	switch(w->status)
	{
		case 0xffffffffu: return 0;
		case 0x80000001u: ScriptError(MSG_SE_MEM_NOT_BW, t); break;
		case 0x80000002u: ScriptError(MSG_SE_MEM_NOT_MONO, t); break;
		case 0x8fffffffu: ScriptError(MSG_SE_MEM_REGISTER_FATAL, t); break;
		default: break;
	}
	return 1;
}

static const WaitVtbl_t WaitSeMem_Vtbl = {WaitSeMem_Destroy, WaitSeMem_Poll, Wait_SetTimer, Wait_IsDirtyBase,
	Wait_OnMessage};

/* load the BW file at `data` (in the script's memory, decoded whole before
 * the wait ends) into effect slot `slot` with a sample gain ("A0 28" of
 * 1.547 on) */
Wait_t* WaitSeMem_New(Thread_t* t, int slot, const void* data, double gain)
{
	WaitSeMem_t* w = (WaitSeMem_t*)BGI_Alloc(sizeof *w);
	WaitCounted_Ctor(&w->w, t);
	WaitCount_Inc();
	w->w.vt = &WaitSeMem_Vtbl;
	w->slot = slot;
	w->data = data;
	w->gain = gain;
	w->queued = 0;
	w->status = 0xffffffffu;
	return &w->w;
}

// ---- BF_Movie sequences (1.69/472 on) ---------------------------------------------

typedef struct WaitSeqLoad // 0x244 bytes
{
	WaitLoader_t l;
	int32_t* outNo;  // receives the sequence number
	int32_t* info;   // five words: width, height, mode, rate, frames
	uint32_t result; // pushed when the wait ends (0 ok, 1, 2 not a movie, else raw)
} WaitSeqLoad_t;

// the result is pushed by the destructor, so a failed read pushes the initial 1 as well
static void WaitSeqLoad_Destroy(Wait_t* base)
{
	WaitSeqLoad_t* w = (WaitSeqLoad_t*)base;
	Thread_Push(base->thread, w->result);
	WaitLoader_Dtor(&w->l);
	BGI_Free(base);
}

// register the file as a sequence; the registry's 0x8000000n codes become 1, 2 (others 0x80000002)
static int WaitSeqLoad_Finish(WaitLoader_t* l)
{
	WaitSeqLoad_t* w = (WaitSeqLoad_t*)l;
	switch(BmSeq_Register(l->buf, l->status, w->outNo, w->info))
	{
		case 0: w->result = 0; break;
		case 0x80000001u: w->result = 1; break;
		case 0x80000002u: w->result = 2; break;
		default: w->result = 0x80000002u; break;
	}
	return 1;
}

static const LoaderVtbl_t WaitSeqLoad_Vtbl = {
	{WaitSeqLoad_Destroy, WaitLoader_Poll, Wait_SetTimer, Wait_IsDirtyBase, Wait_OnMessage}, WaitSeqLoad_Finish};

/* load the BF_Movie file `name` of archive `arc` and register it ("90 F4");
 * *outNo receives the sequence number, info its five description words.
 * Uses the file cache. */
Wait_t* WaitSeqLoad_New(Thread_t* t, const char* arc, const char* name, int32_t* outNo, int32_t* info)
{
	WaitSeqLoad_t* w = (WaitSeqLoad_t*)BGI_Alloc(sizeof *w);
	WaitLoader_Ctor(&w->l, t, arc, name, 1);
	w->l.w.vt = &WaitSeqLoad_Vtbl.base;
	w->outNo = outNo;
	w->info = info;
	w->result = 1;
	return &w->l.w;
}

/* The deferred decode: the original hands the frame to its decoder pool
 * and polls the job; here the frame is decoded on the first poll.  The
 * result codes are those "90 F6" pushes (0 ok, 3 no movie, 5 slot
 * mismatch, 8 the decode failed). */
typedef struct WaitSeqDecode // 0x34 bytes
{
	Wait_t w;        // (WaitCounted)
	uint32_t result; // the code to push; 0x7fffffff until decoded
	int bmpNo, no;   // destination bitmap slot, sequence number
	uint32_t frame;  // frame index
} WaitSeqDecode_t;

static void WaitSeqDecode_Destroy(Wait_t* base)
{
	WaitCount_Dec();
	WaitCounted_Dtor(base);
	BGI_Free(base);
}

// decode the frame and push the result; always finishes
static int WaitSeqDecode_Poll(Wait_t* base)
{
	WaitSeqDecode_t* w = (WaitSeqDecode_t*)base;
	switch(BmSeq_Decode(w->bmpNo, w->no, w->frame))
	{
		case 0: w->result = 0; break;
		case 0x80000003u: w->result = 3; break;
		case 0x80000005u: w->result = 5; break;
		default: w->result = 8; break;
	}
	Thread_Push(base->thread, w->result);
	return 1;
}

static const WaitVtbl_t WaitSeqDecode_Vtbl = {
	WaitSeqDecode_Destroy, WaitSeqDecode_Poll, Wait_SetTimer, Wait_IsDirtyBase, Wait_OnMessage};

// decode frame `frame` of sequence `no` into bitmap bmpNo on the next pass ("90 F6" after "80 53")
Wait_t* WaitSeqDecode_New(Thread_t* t, int bmpNo, int no, uint32_t frame)
{
	WaitSeqDecode_t* w = (WaitSeqDecode_t*)BGI_Alloc(sizeof *w);
	WaitCounted_Ctor(&w->w, t);
	WaitCount_Inc();
	w->w.vt = &WaitSeqDecode_Vtbl;
	w->result = 0x7fffffffu;
	w->bmpNo = bmpNo;
	w->no = no;
	w->frame = frame;
	return &w->w;
}

// the name of a wait class of this file, for the debugger (NULL: not one of these)
const char* WaitLoad_ClassName(const WaitVtbl_t* vt)
{
	if(vt == &WaitBmpLoad_Vtbl.base)
		return "WaitBmpLoad";
	if(vt == &WaitSeLoad_Vtbl.base)
		return "WaitSeLoad";
	if(vt == &WaitSeqLoad_Vtbl.base)
		return "WaitSeqLoad";
	if(vt == &WaitSeqDecode_Vtbl)
		return "WaitSeqDecode";
	return NULL;
}
