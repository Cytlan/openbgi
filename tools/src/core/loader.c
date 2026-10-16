/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * loader.c - the background loader and its file cache (inc/bgi/file.h)
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
 */
#include "bgi/file.h"
#include "bgi/sound.h"
#include "bgi/error.h"
#include "bgi/sys.h"

static int gLoaderRun; // between Loader_Start and Loader_Stop

/* A file job ("90 10", "90 F4"): *status = LoadFile(buf, arc, name), or
 * -1 when the load threw (the original catches the C++ exception on the
 * loader thread, so a missing file does not stop the machine). */
void Loader_QueueFile(uint32_t* status, void* buf, const char* arc, const char* name)
{
	BgiTryFrame_t frame;
	*status = 0;
	if(BGI_TRY(frame))
	{
		uint32_t r = LoadFile(buf, arc, name);
		BGI_END_TRY(frame);
		*status = r;
	}
	else
	{
		*status = 0xffffffffu;
	}
}

/* A sound job ("A0 20"): load the BW file `desc` into sound-effect slot
 * `slot` with Sound_LoadSe and translate the library's result into the
 * status word: 0 and 20 (the library is not running) give 0; 14 (not a
 * BW file) gives 0x80000001; 18 gives 0x80000002; any other code, and a
 * thrown error, give 0x8fffffff. */
void Loader_QueueSe(uint32_t* status, int slot, const void* desc, int fadeInMs, double gain)
{
	BgiTryFrame_t frame;
	*status = 0xffffffffu;
	if(BGI_TRY(frame))
	{
		uint32_t r = Sound_LoadSe(slot, desc, fadeInMs, gain);
		BGI_END_TRY(frame);
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
void Loader_Start(void)
{
	gLoaderRun = 1;
}

// part of Engine_Shutdown: the original stops the thread and waits for it
void Loader_Stop(void)
{
	gLoaderRun = 0;
}

int Loader_IsRunning(void)
{
	return gLoaderRun;
}

//  --- the file cache of the loader ("90 03") --------------------

// drop the cache (also part of Gfx_Destroy)
void Loader_FreeCache(void)
{
	if(gFileCache)
		FileCache_Delete(gFileCache);
	gFileCache = NULL;
}

// "90 03": replace the cache by one of `capacity` bytes, or by none for 0
void Loader_SetCacheSize(uint32_t capacity)
{
	Loader_FreeCache();
	if(capacity > 0)
		gFileCache = FileCache_New(capacity);
}

// FileCache_Add on the loader's cache; 0 when there is none
int Loader_CacheAdd(const char* arc, const char* name, const void* data, uint32_t size)
{
	if(!gFileCache)
		return 0;
	return FileCache_Add(gFileCache, arc, name, data, size);
}

// FileCache_Lookup on the loader's cache; 0 when there is none
int Loader_CacheLookup(void* dst, uint32_t* size, const char* arc, const char* name)
{
	if(!gFileCache)
		return 0;
	return FileCache_Lookup(gFileCache, dst, size, arc, name);
}
