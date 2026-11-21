/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * loader.c - the background loader and its file cache (src/include/bgi/file.h)
 *
 * The original runs a second thread (priority THREAD_PRIORITY_HIGHEST)
 * that drains two job queues: whole-file loads (LoadFile into a buffer the
 * script owns) and sound-effect loads (into a sound slot).  A job carries
 * a pointer to a status word the script polls through the wait objects of
 * "90 .." / "A0 ..": 0 while a file job is pending, the loaded size (or
 * -1) when done; -1 while a sound job is pending, then 0 or an error code.
 *
 * This implementation performs every job immediately when it is queued.
 * Nothing in the engine can observe the difference: the status word is
 * only read through the wait objects, which run after the queuing
 * instruction has returned, and the original gives no ordering guarantee
 * between the loader and the script other than the status word itself.
 * The one job that cannot be done at once is a file that is missing from
 * a disc (RetryPrompt): the original's loader thread asks for the disc and
 * waits while the scripts run on.  Here the job stays pending (its status
 * 0) and waits in a queue with the file jobs queued after it, the boxes go
 * to the host, and every frame (Loader_Service) takes their answers and
 * tries again; the boxes hold the engine as every request does, the pause
 * after them does not.
 */
#include "bgi/file.h"
#include "bgi/sound.h"
#include "bgi/error.h"
#include "bgi/sys.h"
#include "bgi/request.h"
#include "bgi/core.h"

#include <string.h>

// a file job waiting for the disc, or behind one that does
typedef struct LoadJob
{
	uint32_t* status;
	void* buf;
	const char* arc; // NULL for a loose file; the strings are the waiting wait object's
	const char* name;
	struct LoadJob* next;
} LoadJob_t;

/* LoadFile for a file job: 1 and *status, or 0 when the disc is asked for
 * (the job is tried again after the boxes).  A load that threw writes -1:
 * the original catches the C++ exception on the loader thread, so a
 * missing file does not stop the machine. */
static int Loader_TryFile(OpenBGI_t* o, uint32_t* status, void* buf, const char* arc, const char* name)
{
	BgiTryFrame_t frame;
	int done = 1;
	o->files.inLoad++;
	if(BGI_TRY(o, frame))
	{
		uint32_t r = LoadFile(o, buf, arc, name);
		BGI_END_TRY(o, frame);
		*status = r;
	}
	else if(frame.value == BGI_RETRY_VALUE)
		done = 0;
	else
		*status = 0xffffffffu;
	o->files.inLoad--;
	return done;
}

static void Loader_Append(OpenBGI_t* o, uint32_t* status, void* buf, const char* arc, const char* name)
{
	LoadJob_t* j = (LoadJob_t*)BGI_Calloc(sizeof *j);
	LoadJob_t** p = &o->files.loadJobs;
	j->status = status;
	j->buf = buf;
	j->arc = arc;
	j->name = name;
	while(*p)
		p = &(*p)->next;
	*p = j;
}

static void Loader_PopJob(OpenBGI_t* o)
{
	LoadJob_t* j = o->files.loadJobs;
	o->files.loadJobs = j->next;
	BGI_Free(j);
}

/* A file job ("90 10", "90 F4"): *status = LoadFile(buf, arc, name), or
 * -1 when the load threw.  `arc` and `name` must last until the status is
 * written (Loader_Cancel otherwise). */
void Loader_QueueFile(OpenBGI_t* o, uint32_t* status, void* buf, const char* arc, const char* name)
{
	*status = 0;
	if(o->files.loadJobs)
	{
		Loader_Append(o, status, buf, arc, name); // behind the job that waits for its disc
		return;
	}
	if(!Loader_TryFile(o, status, buf, arc, name))
	{
		Loader_Append(o, status, buf, arc, name);
		Retry_Begin(o, &o->files.loadRetry);
	}
}

void Loader_Service(OpenBGI_t* o)
{
	while(o->files.loadJobs)
	{
		LoadJob_t* j = o->files.loadJobs;
		if(o->files.loadRetry.active)
		{
			int a = Retry_Step(o, &o->files.loadRetry);
			if(a == ASK_WAIT)
				return;
			if(a == ASK_ABORT)
			{
				*j->status = 0xffffffffu; // the error the loader catches
				Loader_PopJob(o);
				continue;
			}
		}
		if(!Loader_TryFile(o, j->status, j->buf, j->arc, j->name))
		{
			Retry_Begin(o, &o->files.loadRetry);
			return;
		}
		Loader_PopJob(o);
	}
}

void Loader_Cancel(OpenBGI_t* o, const uint32_t* status)
{
	LoadJob_t** p = &o->files.loadJobs;
	while(*p)
	{
		if((*p)->status == status)
		{
			LoadJob_t* j = *p;
			int first = j == o->files.loadJobs;
			*p = j->next;
			BGI_Free(j);
			if(first && o->files.loadRetry.active)
			{ // its prompt goes with it (a box still up is answered for nobody); the next job is tried by Loader_Service
				Request_Forget(o, o->files.loadRetry.ask.req);
				memset(&o->files.loadRetry, 0, sizeof o->files.loadRetry);
			}
			return;
		}
		p = &(*p)->next;
	}
}

/* A sound job ("A0 20"): load the BW file `desc` into sound-effect slot
 * `slot` with Sound_LoadSe (with the fade-in, gain and playback rate)
 * and translate the library's result into the
 * status word: 0 and 20 (the library is not running) give 0; 14 (not a
 * BW file) gives 0x80000001; 18 gives 0x80000002; any other code, and a
 * thrown error, give 0x8fffffff. */
void Loader_QueueSe(OpenBGI_t* o, uint32_t* status, int slot, const void* desc, int fadeInMs, double gain, double rate)
{
	BgiTryFrame_t frame;
	*status = 0xffffffffu;
	if(BGI_TRY(o, frame))
	{
		uint32_t r = Sound_LoadSe(o, slot, desc, fadeInMs, gain, rate);
		BGI_END_TRY(o, frame);
		switch(r)
		{
			case 0:
			case 20: *status = 0; break;
			case 14: *status = 0x80000001u; break;
			case 18: *status = 0x80000002u; break;
			default: *status = 0x8fffffffu; break;
		}
	}
	else
	{
		*status = 0x8fffffffu;
	}
}

// part of Engine_Init: the original starts the loader thread here
void Loader_Start(OpenBGI_t* o)
{
	o->files.loaderRun = 1;
}

// part of Engine_Shutdown: the original stops the thread and waits for it
void Loader_Stop(OpenBGI_t* o)
{
	o->files.loaderRun = 0;
	while(o->files.loadJobs)
		Loader_PopJob(o);
	memset(&o->files.loadRetry, 0, sizeof o->files.loadRetry);
}

//  --- the file cache of the loader ("90 03") --------------------

// drop the cache (also part of Gfx_Destroy)
void Loader_FreeCache(OpenBGI_t* o)
{
	if(o->files.fileCache)
		FileCache_Delete(o->files.fileCache);
	o->files.fileCache = NULL;
}

// "90 03": replace the cache by one of `capacity` bytes, or by none for 0
void Loader_SetCacheSize(OpenBGI_t* o, uint32_t capacity)
{
	Loader_FreeCache(o);
	if(capacity > 0)
		o->files.fileCache = FileCache_New(capacity);
}

// FileCache_Add on the loader's cache; 0 when there is none
int Loader_CacheAdd(OpenBGI_t* o, const char* arc, const char* name, const void* data, uint32_t size)
{
	if(!o->files.fileCache)
		return 0;
	return FileCache_Add(o, o->files.fileCache, arc, name, data, size);
}

// FileCache_Lookup on the loader's cache; 0 when there is none
int Loader_CacheLookup(OpenBGI_t* o, void* dst, uint32_t* size, const char* arc, const char* name)
{
	if(!o->files.fileCache)
		return 0;
	return FileCache_Lookup(o, o->files.fileCache, dst, size, arc, name);
}
