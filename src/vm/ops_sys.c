/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ops_sys.c - the "80 xx" system-control instruction family: random
 * numbers and time, input, dynamic global memory, files and paths,
 * programs, threads and waiting, the window, save files, the global
 * database, the message history, rings, locks, the SDC / DCFS codecs,
 * dictionaries, string tables, external programs and the installer.  The
 * second system family, "81 xx", is in ops_sys81.c.  The interface
 * (vm_optable_80, Vm_Optable80Init, Opcode_Sys_Yield) is declared in vm.h.
 *
 * Every handler pops its operands in the order the original does, calls
 * the engine service and pushes the result.  The trailing comment on a
 * handler gives the stack effect "inputs → outputs": the inputs in the
 * order the script pushed them (the last one is on top of the stack and is
 * popped first), the outputs as they are pushed; see docs/vm.md.  A service's result code is pushed as it is
 * unless the comment says otherwise; the conditions the original turned
 * into script errors stay script errors (ScriptError never returns).
 *
 * Handlers that install a wait object return scheduler code 2 (end of turn,
 * thread blocked until the object reports completion); the object itself
 * pushes the eventual result when it finishes - see src/wait/.  The other
 * codes used here: 0 next instruction, 1 end of turn, 3 switch to the
 * thread in gSwitchThreadId, 4 end the thread, 5 reboot, 6 quit (vm.h).
 *
 * The table at the end of the file (Vm_Optable80Init) maps the opcodes to
 * the handlers per engine generation; an opcode whose meaning changed
 * between builds has one handler per variant.
 */
#include "bgi/vm.h"
#include "bgi/wait.h"
#include "bgi/waitobj.h"
#include "bgi/error.h"
#include "bgi/file.h"
#include "bgi/sys.h"
#include "bgi/msg.h"
#include "bgi/engine.h"
#include "bgi/input.h"
#include "bgi/display.h"
#include "bgi/sysobj.h"
#include "bgi/panel.h"
#include "bgi/gfx.h"
#include "bgi/gfx/bmseq.h"
#include "bgi/sound.h"
#include "bgi/install.h"
#include "bgi/strutil.h"
#include "bgi/codec.h"
#include "bgi/os.h"

VmHandler_t vm_optable_80[256]; // the "80 xx" dispatch table, filled by Vm_Optable80Init

/* raise a script error with a printf-style message (formatted into a local
 * buffer as the original does; ScriptError never returns) */
#define SYS_ERROR(t, ...)           \
	do                              \
	{                               \
		char msg_[0x104];           \
		sprintf(msg_, __VA_ARGS__); \
		ScriptError(msg_, (t));     \
	} while(0)

// -------------------------------------------------------------------------
// 80 00 .. 80 0F : random numbers, time, machine information
// -------------------------------------------------------------------------

static int Opcode_Sys_Srand(Thread_t* t) // 80 00: seed →
{
	BGI_Srand(Thread_Pop(t));
	return 0;
}

static int Opcode_Sys_Rand(Thread_t* t) // 80 01: → v (0..0x7fff)
{
	Thread_Push(t, (uint32_t)BGI_Rand());
	return 0;
}

// a random value in 0 .. n-1; 0 when n is not positive
static int Opcode_Sys_Random(Thread_t* t) // 80 02: n → v
{
	int32_t n = (int32_t)Thread_Pop(t);
	int32_t v = 0;
	if(n > 0)
	{
		// three 15-bit rand() results combined into a 31-bit value, then modulo
		int32_t r = BGI_Rand();
		r = (r << 8) ^ BGI_Rand();
		r = (r << 8) ^ BGI_Rand();
		v = r % n; // signed remainder, as in the original
	}
	Thread_Push(t, (uint32_t)v);
	return 0;
}

static int Opcode_Sys_GetTicks(Thread_t* t) // 80 04: → ms (since boot)
{
	Thread_Push(t, GetTicks());
	return 0;
}

// the cursor in back-buffer coordinates (0, 0 without a window)
static int Opcode_Sys_GetMousePos(Thread_t* t) // 80 08: → x, y
{
	int xy[2];
	GetMouseClientPos(xy);
	Thread_Push(t, (uint32_t)xy[0]);
	Thread_Push(t, (uint32_t)xy[1]);
	return 0;
}

// the 0x40-byte processor description (SysInfo_t, sys.h) into *dst
static int Opcode_Sys_GetSysinfo(Thread_t* t) // 80 0A: dst →
{
	void* dst = PopPtr(t);
	memcpy(dst, &gSysInfo, 0x40); // the whole SysInfo_t block
	return 0;
}

// the compositor's band size
static int Opcode_Sys_GetGfxProp(Thread_t* t) // 80 0B: → v
{
	Thread_Push(t, Sys_GetGfxProp());
	return 0;
}

// the local time as a 16-byte SYSTEMTIME into *dst
static int Opcode_Sys_GetLocaltime(Thread_t* t) // 80 0C: dst →
{
	uint16_t* dst = (uint16_t*)PopPtr(t);
	OsLocalTime_t lt;
	OS_LocalTime(&lt);
	// SYSTEMTIME layout: eight 16-bit fields, year first
	dst[0] = lt.year;
	dst[1] = lt.month;
	dst[2] = lt.dayOfWeek;
	dst[3] = lt.day;
	dst[4] = lt.hour;
	dst[5] = lt.minute;
	dst[6] = lt.second;
	dst[7] = lt.millis;
	return 0;
}

// the physical memory in bytes, total and available, clamped to 0x7fffffff
static int Opcode_Sys_GetMemstatus(Thread_t* t) // 80 0D: → total, avail
{
	OsMemStatus_t ms;
	OS_MemoryStatus(&ms);
	Thread_Push(t, ms.totalPhys >= 0x80000000u ? 0x7fffffffu : ms.totalPhys);
	Thread_Push(t, ms.availPhys >= 0x80000000u ? 0x7fffffffu : ms.availPhys);
	return 0;
}

// whether "80 65" minimised the window and it has not been activated since
static int Opcode_Sys_GetMinimized(Thread_t* t) // 80 0E: → f
{
	Thread_Push(t, (uint32_t)Window_MinimizedByScript());
	return 0;
}

// whether the main window has the focus
static int Opcode_Sys_GetActive(Thread_t* t) // 80 0F: → f
{
	Thread_Push(t, (uint32_t)Window_IsActive());
	return 0;
}

// -------------------------------------------------------------------------
// 80 10 .. 80 1F : input
// -------------------------------------------------------------------------

// switch input on or off; the key states are forgotten either way
static int Opcode_Sys_InputEnable(Thread_t* t) // 80 10: f →
{
	Set_InputEnable((int)Thread_Pop(t));
	Input_ClearStates();
	return 0;
}

// 1 while virtual key vk is held (the mouse buttons as swapped by "80 1E")
static int Opcode_Sys_KeyDown(Thread_t* t) // 80 11: vk → f
{
	uint32_t vk = Thread_Pop(t);
	Thread_Push(t, ((uint32_t)GetKeyStateEx((int)vk) >> 15) & 1);
	return 0;
}

// the presses counted for the virtual keys of a 0-terminated list, summed
static int Opcode_Sys_KeyCounterSum(Thread_t* t) // 80 12: list → n
{
	const uint32_t* list = (const uint32_t*)PopPtr(t);
	uint32_t sum = 0;
	for(; *list; list++)
		sum += (uint32_t)GetKeyCounter((int)*list);
	Thread_Push(t, sum);
	return 0;
}

// a counter bumped on every input message (a cheap "anything happened" test)
static int Opcode_Sys_InputSerial(Thread_t* t) // 80 13: → v
{
	Thread_Push(t, InputSerial_Get());
	return 0;
}

// whether the skip keys work at all ("80 17")
static int Opcode_Sys_SetSkipEnable(Thread_t* t) // 80 14: f →
{
	Set_SkipKeyEnable((int)Thread_Pop(t));
	return 0;
}

// hold skipping on (f 1) without a key being pressed
static int Opcode_Sys_SetSkipLatch(Thread_t* t) // 80 15: f →
{
	Set_SkipLatch((int)Thread_Pop(t));
	return 0;
}

/* forget the pending state of the skip keys: a key already held does not
 * count until it is released and pressed again */
static int Opcode_Sys_InputFlush(Thread_t* t) // 80 16
{
	BGI_UNUSED(t);
	Input_Flush();
	return 0;
}

/* whether skipping is active: a skip key held while the window is active,
 * or the latch of "80 15"; requires the switch of "80 14" */
static int Opcode_Sys_CheckSkip(Thread_t* t) // 80 17: → f
{
	Thread_Push(t, (uint32_t)Input_CheckSkip());
	return 0;
}

/* Input layers (input.h): a layer is identified by the key
 * (priority << 20) | 0xfffff; only the top-most key layer and the mouse
 * layer under the cursor receive input.  "80 18" opens a layer of priority
 * prio on both lists and swallows the input pending for it. */
static int Opcode_Sys_InputLayerPush(Thread_t* t) // 80 18: prio →
{
	uint32_t key = (Thread_Pop(t) << 20) | 0xfffff;
	KeyLayer_Push(key);
	MouseLayer_Push(key);
	Input_Poll(key, key);
	return 0;
}

// swallow the input pending for the layer of priority prio and close it
static int Opcode_Sys_InputLayerPop(Thread_t* t) // 80 19: prio →
{
	uint32_t key = (Thread_Pop(t) << 20) | 0xfffff;
	Input_Poll(key, key);
	KeyLayer_Pop(key);
	MouseLayer_Pop(key);
	return 0;
}

/* the input pending for the layer of priority prio, as a mask: bit 31
 * skip, bits 6 .. 30 the standard-key groups, bits 0, 1, 2, 4, 5 the mouse
 * buttons when the mouse layer is hit (input.h); the pending input is
 * consumed */
static int Opcode_Sys_InputPoll(Thread_t* t) // 80 1A: prio → mask
{
	uint32_t key = (Thread_Pop(t) << 20) | 0xfffff;
	Thread_Push(t, Input_Poll(key, key));
	return 0;
}

/* assign up to 15 virtual keys (0-terminated) to the standard-key group
 * `group` (a bit of the "80 1A" mask, 0x40 .. 0x8000); an unknown group or
 * a longer list is a script error */
static int Opcode_Sys_SetStandardKey(Thread_t* t) // 80 1B: group, keys →
{
	const uint32_t* keys = (const uint32_t*)PopPtr(t);
	uint32_t group = Thread_Pop(t);
	uint32_t r = SetStandardKey(group, keys);
	if(r == 0x80000001u)
		SYS_ERROR(t, MSG_BAD_STANDARD_KEY, group);
	if(r == 0x80000002u)
		SYS_ERROR(t, MSG_KEY_LIST_TOO_LONG, 15);
	return 0;
}

// the presses counted for every virtual key of the standard-key groups in mask
static int Opcode_Sys_CountKeys(Thread_t* t) // 80 1C: mask → n
{
	uint32_t mask = Thread_Pop(t);
	Thread_Push(t, (uint32_t)CountKeysByMask(mask));
	return 0;
}

/* the pending presses of virtual key vk (bit 31: first report while it is
 * held), when the layer of priority prio is the one receiving input; 0
 * otherwise.  The presses are consumed. */
static int Opcode_Sys_KeyTrigger(Thread_t* t) // 80 1D: prio, vk → v
{
	uint32_t vk = Thread_Pop(t);
	uint32_t key = (Thread_Pop(t) << 20) | 0xfffff;
	uint32_t v = 0;
	int active;
	// mouse buttons are governed by the mouse layer, keys by the key layer
	if(vk == 1 || vk == 2)
		active = MouseLayer_Hit(key);
	else
		active = KeyLayer_IsTop(key);
	if(active)
		v = (uint32_t)GetKeyTrigger((int)vk);
	Thread_Push(t, v);
	return 0;
}

// exchange the left and right mouse buttons (v 1) or not (0); ok 0 for any other v
static int Opcode_Sys_SwapMouseButtons(Thread_t* t) // 80 1E: v → ok
{
	uint32_t v = Thread_Pop(t);
	Thread_Push(t, (uint32_t)Input_SetSwapButtons(v));
	return 0;
}

/* when the panels are polled: mode 0 before the input of a pass, 1 after
 * the pass; ok 0 for any other value */
static int Opcode_Sys_SetPanelPollMode(Thread_t* t) // 80 AF (1.573 on): mode → ok
{
	Thread_Push(t, (uint32_t)Panel_SetPollMode((int)Thread_Pop(t)));
	return 0;
}

/* glide the cursor to back-buffer position (x, y): a3 is the curve (1
 * eases in and out, anything else is linear), a4 the duration in ms, a5
 * the steps per second and a6 non-zero stops the glide when the user moves
 * the mouse.  Nothing happens while the window is minimised. */
static int Opcode_Sys_MouseMove(Thread_t* t) // 80 1F: x, y, a3, a4, a5, a6 →
{
	uint32_t a6 = Thread_Pop(t), a5 = Thread_Pop(t), a4 = Thread_Pop(t), a3 = Thread_Pop(t);
	uint32_t y = Thread_Pop(t), x = Thread_Pop(t);
	MouseMove_Start((int)x, (int)y, (int)a3, (int)a4, (int)a5, (int)a6);
	return 0;
}

// -------------------------------------------------------------------------
// 80 20 .. 80 3F : dynamic global memory, files and paths
// -------------------------------------------------------------------------

/* a dynamic global block of `size` bytes, as a tagged pointer; a size above
 * 32 MB or no room left is a script error */
static int Opcode_Sys_Galloc(Thread_t* t) // 80 20: size → p
{
	uint32_t size = Thread_Pop(t);
	uint32_t p;
	CheckAllocSize(size, t);
	p = GAlloc(size);
	if(p == 0)
		ScriptError(MSG_GLOBAL_FULL, t);
	Thread_Push(t, p);
	return 0;
}

// free a block of "80 20"; a pointer that is not one is a script error
static int Opcode_Sys_Gfree(Thread_t* t) // 80 21: p → ok
{
	uint32_t p = Thread_Pop(t);
	int ok = GFree(p);
	if(!ok)
		SYS_ERROR(t, MSG_GFREE_BAD_ADDRESS, p);
	Thread_Push(t, (uint32_t)ok);
	return 0;
}

// the number of files matching `pattern`, sub-directories entered when `recurse` is set
static int Opcode_Sys_CountFiles(Thread_t* t) // 80 24: pattern, recurse → n
{
	uint32_t recurse = Thread_Pop(t);
	const char* pattern = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)FindFiles(NULL, pattern, (int)recurse, gEmptyPath, -1));
	return 0;
}

/*
 * The names of the files matching `pattern` (sub-directories entered when
 * `recurse` is set, their name prefixed): up to `max` names are enumerated
 * into 0x104-byte slots, then packed into `buf` (bufSize bytes) as
 * consecutive NUL-terminated strings while they fit.  Pushes the number of
 * names found, or -1 when the buffer was too small for all of them.
 */
static int Opcode_Sys_ListFiles(Thread_t* t) // 80 25: buf, bufSize, pattern, recurse, max → n
{
	uint32_t max = Thread_Pop(t);
	uint32_t recurse = Thread_Pop(t);
	const char* pattern = (const char*)PopPtr(t);
	uint32_t bufSize = Thread_Pop(t);
	char* buf = (char*)PopPtr(t);
	char** names = (char**)BGI_Alloc(max * sizeof(char*));
	uint32_t i, found;
	int fits = 1;

	for(i = 0; i < max; i++)
		names[i] = (char*)BGI_Alloc(0x104);
	found = (uint32_t)FindFiles(names, pattern, (int)recurse, NULL, (int)max);

	for(i = 0; i < max; i++)
	{
		if(i < found && fits)
		{
			uint32_t len = (uint32_t)strlen(names[i]) + 1;
			if(len <= bufSize)
			{
				memcpy(buf, names[i], len);
				buf += len;
				bufSize -= len;
			}
			else
			{
				fits = 0;
			}
		}
		BGI_Free(names[i]);
	}
	BGI_Free(names);
	Thread_Push(t, fits ? found : 0xffffffffu);
	return 0;
}

static int Opcode_Sys_Mkdir(Thread_t* t) // 80 28: path → ok
{
	const char* path = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)OS_DirCreate(path));
	return 0;
}

static int Opcode_Sys_Rmdir(Thread_t* t) // 80 29: path → ok
{
	const char* path = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)OS_DirRemove(path));
	return 0;
}

// whether `path` names an existing directory
static int Opcode_Sys_IsDir(Thread_t* t) // 80 2A: path → f
{
	const char* path = (const char*)PopPtr(t);
	uint32_t a = OS_FileAttrs(path);
	Thread_Push(t, a == OS_INVALID_ATTRS ? 0 : (a >> 4) & 1); // bit 4: FILE_ATTRIBUTE_DIRECTORY
	return 0;
}

// the Win32 file attributes of `path` (OS_ATTR_*), -1 when it does not exist
static int Opcode_Sys_GetAttr(Thread_t* t) // 80 2C: path → attr
{
	const char* path = (const char*)PopPtr(t);
	Thread_Push(t, OS_FileAttrs(path));
	return 0;
}

static int Opcode_Sys_SetAttr(Thread_t* t) // 80 2D: path, attr → ok
{
	uint32_t attr = Thread_Pop(t);
	const char* path = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)OS_FileSetAttrs(path, attr));
	return 0;
}

// copy `src` over `dst`, clearing dst's read-only attribute first so that the copy may overwrite it
static int Opcode_Sys_CopyFile(Thread_t* t) // 80 2F: dst, src → ok
{
	const char* src = (const char*)PopPtr(t);
	const char* dst = (const char*)PopPtr(t);
	uint32_t a = OS_FileAttrs(dst);
	if(a != OS_INVALID_ATTRS)
		OS_FileSetAttrs(dst, a & ~OS_ATTR_READONLY);
	Thread_Push(t, (uint32_t)OS_FileCopy(src, dst, 0));
	return 0;
}

/* split `path` into drive, directory, file name and extension as
 * _splitpath does (any destination may be NULL); ok 0 and nothing written
 * when path is NULL */
static int Opcode_Sys_SplitPath(Thread_t* t) // 80 2B (1.69 build 472 on): drive, dir, fname, ext, path → ok
{
	const char* path = (const char*)PopPtr(t);
	char* ext = (char*)PopPtr(t);
	char* fname = (char*)PopPtr(t);
	char* dir = (char*)PopPtr(t);
	char* drive = (char*)PopPtr(t);
	if(path)
		SplitPath(path, drive, dir, fname, ext);
	Thread_Push(t, path != NULL);
	return 0;
}

/* the whole file `name` of archive `arc` (or the loose file) into buf,
 * decompressed when it is DSC / CompressedBG; pushes the size in buf.  A
 * file that stays missing after the disc prompt is a script error. */
static int Opcode_Sys_LoadFile(Thread_t* t) // 80 30: buf, arc, name → size
{
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	void* buf = PopPtr(t);
	Thread_Push(t, LoadFile(buf, arc, name));
	return 0;
}

/* `size` bytes from offset `off` of the raw (stored) file into buf; 0 ok,
 * 1 not found, 2 / 3 range errors, -1 short read */
static int Opcode_Sys_LoadFileRange(Thread_t* t) // 80 31 (.. 1.69 build 444): buf, arc, name, off, size → code
{
	uint32_t size = Thread_Pop(t), off = Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	void* buf = PopPtr(t);
	Thread_Push(t, (uint32_t)LoadFileRange(buf, arc, name, off, size));
	return 0;
}

/* the same range of the decoded (DSC / CompressedBG) file; off = size = 0
 * is everything.  0 ok, 1 not found, 2 end beyond the data, 3 size 0 or
 * beyond, 5 decode failure, 6 larger than the limit (file.h).  The raw
 * range moved to "81 30" in these builds. */
static int Opcode_Sys_LoadFileRangeDecoded(Thread_t* t) // 80 31 (1.69 build 472 on): buf, arc, name, off, size → code
{
	uint32_t size = Thread_Pop(t), off = Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	void* buf = PopPtr(t);
	Thread_Push(t, (uint32_t)LoadFileRangeDecoded(buf, arc, name, off, size));
	return 0;
}

/* create or overwrite file `name` (an absolute name as it is, else in the
 * base directory) with `size` bytes; ok when every byte was written */
static int Opcode_Sys_WriteFile(Thread_t* t) // 80 32: name, data, size → ok
{
	uint32_t size = Thread_Pop(t);
	const void* data = PopPtr(t);
	const char* name = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)WriteTextFile(name, data, size) == size);
	return 0;
}

// delete "<dir>\<name>", or `name` in the base directory when dir is NULL
static int Opcode_Sys_DeleteFile(Thread_t* t) // 80 33: dir, name → ok
{
	const char* name = (const char*)PopPtr(t);
	const char* dir = (const char*)PopPtr(t);
	char path[0x104];
	if(dir)
		sprintf(path, "%s\\%s", dir, name);
	else
		sprintf(path, "%s%s", gBaseDir, name);
	Thread_Push(t, (uint32_t)OS_FileDelete(path));
	return 0;
}

// whether file `name` exists, loose or in archive `arc`
/* The disc marker: the discs carry a 16-byte file named exactly like the
 * product ("Tayutama", "HanairoHeptagram"), and every boot program quits
 * at once when "80 34" finds it next to the game - a copy of the disc
 * directory never starts.  Unless --disc-marker asks for the original's
 * behaviour, a loose file named like the product is reported missing
 * (said once on stderr); the name match is case-insensitive like the
 * file lookup. */
static int IsDiscMarker(const char* arc, const char* name)
{
	static int said;
	const char* product = Engine_ProductName();
	size_t i;
	if(gDiscMarker || arc || !name || !product[0])
		return 0;
	for(i = 0; name[i] && product[i]; i++)
	{
		int a = (uint8_t)name[i], b = (uint8_t)product[i];
		if(a >= 'A' && a <= 'Z')
			a += 32;
		if(b >= 'A' && b <= 'Z')
			b += 32;
		if(a != b)
			return 0;
	}
	if(name[i] || product[i])
		return 0;
	if(!said++)
		fprintf(stderr, "bgi: the disc marker file \"%s\" is hidden from the scripts (--disc-marker shows it)\n", name);
	return 1;
}

static int Opcode_Sys_FileExists(Thread_t* t) // 80 34: arc, name → f
{
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	Thread_Push(t, IsDiscMarker(arc, name) ? 0u : (uint32_t)FileExists(arc, name));
	return 0;
}

/* the size the file takes once loaded (the decoded size of a DSC stream),
 * found by reading it; 0 when it is not found.  "81 35" (1.588 on) answers
 * without reading. */
static int Opcode_Sys_FileSize(Thread_t* t) // 80 35: arc, name → size
{
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	Thread_Push(t, GetFileSize_(arc, name));
	return 0;
}

// whether the loader tries the sub-directories of "80 37"
static int Opcode_Sys_SetFileSearch(Thread_t* t) // 80 36: v →
{
	Set_FileSearch((int)Thread_Pop(t));
	return 0;
}

// a sub-directory the loader tries (newest first) before the directory itself
static int Opcode_Sys_PushSearchName(Thread_t* t) // 80 37: name →
{
	PushSearchName((const char*)PopPtr(t));
	return 0;
}

/* the number of entries of a 0-terminated array of tagged pointers,
 * terminator included, so that ResolvePtrArray turns it into a
 * NULL-terminated array of host pointers */
static int TaggedListLen(const uint32_t* list)
{
	int n = 0;
	while(list[n])
		n++;
	return n + 1;
}

/* the performance counter as a 64-bit nanosecond value (counter /
 * frequency * 1e9, truncated) into out[0] (low word) and out[1]; ok 0 and
 * nothing written when the machine has no counter */
static int Opcode_Sys_PerfCounter(Thread_t* t) // 80 05 (1.69 build 472 on): out → ok
{
	uint32_t* out = (uint32_t*)PopPtr(t);
	int64_t ns;
	int ok = OS_PerfCounterNs(&ns);
	if(ok)
	{
		out[0] = (uint32_t)ns;
		out[1] = (uint32_t)((uint64_t)ns >> 32);
	}
	Thread_Push(t, (uint32_t)ok);
	return 0;
}

/* name the alternative directory (in these builds the user's data folder,
 * where BGI.gdb and the saves go): an existing directory is stored with a
 * trailing backslash and ok is 1; otherwise 0 and no change */
static int Opcode_Sys_SetAltDir(Thread_t* t) // 80 39 (1.69 build 472 on): dir → ok
{
	const char* dir = (const char*)PopPtr(t);
	uint32_t attrs = OS_FileAttrs(dir);
	int ok = attrs != OS_INVALID_ATTRS && (attrs & OS_ATTR_DIRECTORY);
	if(ok)
		sprintf(gAltDir, "%s\\", dir);
	Thread_Push(t, (uint32_t)ok);
	return 0;
}

/* register the pseudo archive `arc` that stands for the 0-terminated list
 * of archive file names; ok 1 when registered, 0 for an empty name or
 * list or a name already registered */
static int Opcode_Sys_RegisterFilelist(Thread_t* t) // 80 38: arc, names → ok
{
	const uint32_t* names = (const uint32_t*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	int n = TaggedListLen(names);
	char** list = (char**)BGI_Alloc((size_t)n * sizeof(char*));
	int ok;
	ResolvePtrArray((void**)list, t, names, n);
	ok = Sys_RegisterFilelist(arc, list);
	Thread_Push(t, (uint32_t)ok);
	BGI_Free(list);
	return 0;
}

/* a system folder into buf: which 0 the Windows directory, 1 the desktop,
 * 2 the start menu's Programs folder, 3 My Documents; the result code of
 * the lookup is dropped */
static int Opcode_Sys_GetSpecialFolder(Thread_t* t) // 80 3A: buf, which →
{
	uint32_t which = Thread_Pop(t);
	char* buf = (char*)PopPtr(t);
	GetSpecialFolder(buf, (int)which);
	return 0;
}

/* the open (mode 0) or save (mode 1) file dialog with the filter
 * description p2, the extension p3, the title p4 and the initial directory
 * p5; r 0 when a file was chosen (its path in buf, 0x104 bytes), -1 when
 * the dialog was cancelled, 4 for another mode */
static int Opcode_Sys_FileDialog(Thread_t* t) // 80 3B: buf, p2, p3, p4, p5, mode → r
{
	uint32_t mode = Thread_Pop(t);
	const char* p5 = (const char*)PopPtr(t);
	const char* p4 = (const char*)PopPtr(t);
	const char* p3 = (const char*)PopPtr(t);
	const char* p2 = (const char*)PopPtr(t);
	char* buf = (char*)PopPtr(t);
	Thread_Push(t, (uint32_t)FileDialog(buf, p2, p3, p4, p5, (int)mode));
	return 0;
}

/* show `msg` (with `title`) until `file` exists in the base directory, as
 * the "insert the disc" prompt; ok 0 when the user chose to give up */
static int Opcode_Sys_PromptDisk(Thread_t* t) // 80 3C: file, title, msg → ok
{
	const char* msg = (const char*)PopPtr(t);
	const char* title = (const char*)PopPtr(t);
	const char* file = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)PromptForDisk(file, title, msg));
	return 0;
}

// the base directory (which 0) or the alternative one (1) into buf; ok 0 when there is none
static int Opcode_Sys_GetBasePath(Thread_t* t) // 80 3D: buf, which → ok
{
	uint32_t which = Thread_Pop(t);
	char* buf = (char*)PopPtr(t);
	Thread_Push(t, (uint32_t)GetBasePath(buf, (int)which));
	return 0;
}

// an existing directory becomes the base directory (stored with a trailing backslash); otherwise ok 0 and no change
static int Opcode_Sys_SetBasePath(Thread_t* t) // 80 3E: path → ok
{
	const char* path = (const char*)PopPtr(t);
	uint32_t a = OS_FileAttrs(path);
	int ok = 0;
	if(a != OS_INVALID_ATTRS && (a & OS_ATTR_DIRECTORY))
	{
		sprintf(gBaseDir, "%s\\", path);
		ok = 1;
	}
	Thread_Push(t, (uint32_t)ok);
	return 0;
}

/* make the disc holding the file p1 the alternative directory, prompting
 * with message p2 and title p3 while no drive has it (with a4 set, Cancel
 * offers to give up); r 1 when the disc was found */
static int Opcode_Sys_SetInstallPaths(Thread_t* t) // 80 3F: p1, p2, p3, a4 → r
{
	uint32_t a4 = Thread_Pop(t);
	const char* p3 = (const char*)PopPtr(t);
	const char* p2 = (const char*)PopPtr(t);
	const char* p1 = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)SetInstallPaths(p1, p2, p3, (int)a4));
	return 0;
}

// -------------------------------------------------------------------------
// 80 40 .. 80 5F : programs, threads, waiting
// -------------------------------------------------------------------------

/* append the program file `name` of archive `arc` to this thread's code
 * area as a module and push its base offset; a missing program or a full
 * code area is a script error */
static int Opcode_Sys_LoadProgram(Thread_t* t) // 80 40: arc, name → base
{
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	uint8_t* image = (uint8_t*)BGI_Alloc(0x20000);
	uint32_t n = LoadFile(image, arc, name);
	if(n != 0)
	{
		uint32_t base = Thread_LoadModule(t, image, name);
		if(base == 0x80000000u)
			SYS_ERROR(t, MSG_CODE_AREA_FULL, arc, name);
		Thread_Push(t, base);
	}
	else
	{
		SYS_ERROR(t, MSG_PROGRAM_NOT_FOUND, arc, name);
	}
	BGI_Free(image);
	return 0;
}

/* drop the module loaded last and push the number of modules left
 * (0x80000001 when there was none); unloading the main program itself,
 * which leaves 0, is a script error */
static int Opcode_Sys_UnloadProgram(Thread_t* t) // 80 41: → n
{
	uint32_t n = Thread_UnloadLastModule(t);
	if(n == 0)
		ScriptError(MSG_MAIN_PROGRAM_UNLOADED, t);
	Thread_Push(t, n);
	return 0;
}

/* run program `name` of archive `arc` in a new thread with a stack of
 * stackSz entries, codeSz bytes of code area and dataSz bytes of data
 * area; pushes the thread's id (a missing program is a script error) */
static int Opcode_Sys_SpawnThread(Thread_t* t) // 80 44: arc, name, stackSz, codeSz, dataSz → id
{
	uint32_t dataSz = Thread_Pop(t), codeSz = Thread_Pop(t), stackSz = Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	Thread_Push(t, SpawnThread(arc, name, stackSz, codeSz, dataSz, t));
	return 0;
}

// end this thread (scheduler code 4)
static int Opcode_Sys_ExitThread(Thread_t* t) // 80 45
{
	BGI_UNUSED(t);
	return 4;
}

static int Opcode_Sys_GetThreadId(Thread_t* t) // 80 46: → id
{
	Thread_Push(t, Thread_GetId(t));
	return 0;
}

// whether a thread with this id exists (id 0, the root thread, is never found)
static int Opcode_Sys_ThreadExists(Thread_t* t) // 80 47: id → f
{
	Thread_Push(t, Thread_FindById(gRootThread, Thread_Pop(t)) != NULL);
	return 0;
}

/* Thread messages: every thread has a queue of 32-bit values other threads
 * append to ("80 48" / "80 4A") and it reads in order ("80 49" / "80 4B").
 * An unknown thread id is a script error. */
static int Opcode_Sys_ThreadPost(Thread_t* t) // 80 48: id, msg →
{
	uint32_t msg = Thread_Pop(t);
	Thread_t* target = Thread_FindById(gRootThread, Thread_Pop(t));
	if(!target)
		ScriptError(MSG_BAD_THREAD_HANDLE, t);
	Thread_ListAppend(target, msg);
	return 0;
}

// the oldest message of this thread's queue into *out; f 1 when there was one
static int Opcode_Sys_ThreadRecv(Thread_t* t) // 80 49: out → f
{
	uint32_t* out = (uint32_t*)PopPtr(t);
	Thread_Push(t, (uint32_t)Thread_ListPopFront(t, out));
	return 0;
}

// append the n (at least 1, else a script error) values of arr to thread id's queue
static int Opcode_Sys_ThreadPostN(Thread_t* t) // 80 4A: id, n, arr →
{
	const uint32_t* arr = (const uint32_t*)PopPtr(t);
	int32_t n = (int32_t)Thread_Pop(t);
	Thread_t* target = Thread_FindById(gRootThread, Thread_Pop(t));
	int32_t i;
	if(!target)
		ScriptError(MSG_BAD_THREAD_HANDLE, t);
	if(n < 1)
		SYS_ERROR(t, MSG_BAD_THREAD_MSG_COUNT, n);
	for(i = 0; i < n; i++)
		Thread_ListAppend(target, arr[i]);
	return 0;
}

// up to n (at least 1, else a script error) messages of this thread's queue into arr; got = how many
static int Opcode_Sys_ThreadRecvN(Thread_t* t) // 80 4B: n, arr → got
{
	uint32_t* arr = (uint32_t*)PopPtr(t);
	int32_t n = (int32_t)Thread_Pop(t);
	int32_t i, got = 0;
	int more = 1;
	if(n < 1)
		SYS_ERROR(t, MSG_BAD_THREAD_MSG_COUNT, n);
	for(i = 0; i < n && more; i++)
	{
		more = Thread_ListPopFront(t, arr + i);
		got += more != 0;
	}
	Thread_Push(t, (uint32_t)got);
	return 0;
}

/* deliver the notification (a, b, c) to the wait object thread `id` is
 * blocked in (its onMessage: a = 1 cuts a "80 5C" wait short; the text,
 * selection and tween waits interpret their own codes); r 1 when the
 * thread was waiting, 0 when it was not or does not exist */
static int Opcode_Sys_ThreadNotify(Thread_t* t) // 80 4C (1.69 build 444 on): id, a, b, c → r
{
	uint32_t c = Thread_Pop(t), b = Thread_Pop(t), a = Thread_Pop(t);
	Thread_t* target = Thread_FindById(gRootThread, Thread_Pop(t));
	uint32_t r = 0;
	if(target)
		r = (uint32_t)Thread_WaitNotify(target, a, b, c);
	Thread_Push(t, r);
	return 0;
}

/* the same for 1.58 .. 1.66: an unknown thread is a script error instead
 * of a 0 result, and until 1.64 the thread lookup had no special case for
 * id 0, so 0 names the root thread (whose id it is) rather than nothing */
static int Opcode_Sys_ThreadNotify_Old(Thread_t* t) // 80 4C (1.58 .. 1.66): id, a, b, c → r
{
	uint32_t c = Thread_Pop(t), b = Thread_Pop(t), a = Thread_Pop(t);
	uint32_t id = Thread_Pop(t);
	Thread_t* target = Thread_FindById(gRootThread, id);
	if(!target && id == 0 && gEngine->gen <= GEN_1_64 && Thread_GetId(gRootThread) == 0)
		target = gRootThread;
	if(!target)
		ScriptError(MSG_BAD_THREAD_HANDLE, t);
	Thread_Push(t, (uint32_t)Thread_WaitNotify(target, a, b, c));
	return 0;
}

/* whether wait objects block (the default): with v 0 every wait ends at
 * its first poll, pushing its "abandoned" result (-1 where it has one).
 * Ends the turn (scheduler code 1). */
static int Opcode_Sys_WaitSetBlocking(Thread_t* t) // 80 50: v →; ends the turn
{
	Wait_SetBlocking((int)Thread_Pop(t));
	return 1;
}

/* block until the window receives message number `msg`; the wait pushes
 * the message's wParam and lParam, or -1, -1 in non-blocking mode */
static int Opcode_Sys_WaitWinmsg(Thread_t* t) // 80 54: msg → (pushes 2 values later)
{
	uint32_t msg = Thread_Pop(t);
	Thread_SetWait(t, WaitWinMsg_New(t, msg));
	return 2;
}

// the thread timer expires `ms` milliseconds from now
static int Opcode_Sys_TimerSet(Thread_t* t) // 80 58: ms →
{
	Thread_TimerSet(t, Thread_Pop(t));
	return 0;
}

// move the thread timer's deadline `ms` milliseconds later; f 1 when time is left after that
static int Opcode_Sys_TimerAdd(Thread_t* t) // 80 59: ms → f
{
	Thread_TimerAdd(t, Thread_Pop(t));
	Thread_Push(t, Thread_TimerRemaining(t) != 0);
	return 0;
}

/* block until the thread timer expires; f 1 when there was time left (and
 * the wait was installed), 0 when it had already expired.  The result is
 * pushed before the wait, which pushes nothing itself. */
static int Opcode_Sys_TimerWait(Thread_t* t) // 80 5A: → f
{
	uint32_t remaining = Thread_TimerRemaining(t);
	if(remaining != 0)
		Thread_SetWait(t, WaitTimer_New(t, remaining));
	Thread_Push(t, remaining != 0);
	return 2;
}

/* block for `ms` milliseconds; with f set, input on the layer of priority
 * prio (skip, the down key, decide, wheel down or the left button - see
 * WaitTimerKey_Poll) ends the wait early, as does a "80 4C" notification
 * with a = 1.  The wait pushes 1
 * when input ended it, 0 otherwise. */
static int Opcode_Sys_WaitTimeKey(Thread_t* t) // 80 5C: ms, f, prio → (pushes later)
{
	uint32_t prio = Thread_Pop(t), f = Thread_Pop(t), ms = Thread_Pop(t);
	Thread_SetWait(t, WaitTimerKey_New(t, ms, (int)f, prio));
	return 2;
}

// f 1 makes this the only thread the scheduler runs; 0 lifts the restriction
static int Opcode_Sys_SetExclusive(Thread_t* t) // 80 5D: f →
{
	int on = Thread_Pop(t) != 0;
	SetExclusiveThread(on ? t : NULL, on);
	return 0;
}

// give the turn to thread `id` (scheduler code 3)
static int Opcode_Sys_SwitchThread(Thread_t* t) // 80 5E: id →
{
	gSwitchThreadId = Thread_Pop(t);
	return 3;
}

/* end the turn (scheduler code 1).  Declared in vm.h rather than static
 * because in the original the same routine served as the base wait
 * class's poll (Wait_PollDone in wait_sys.c here). */
int Opcode_Sys_Yield(Thread_t* t) // 80 5F
{
	BGI_UNUSED(t);
	return 1;
}

// -------------------------------------------------------------------------
// 80 60 .. 80 6F : the window
// -------------------------------------------------------------------------

/* windowed (fullscreen 0) or full screen at the picture size of entry
 * sizeIdx of the size table and pixel mode 0 (16-bit) or 1 (32-bit); a
 * mode or index out of range is a script error */
static int Opcode_Sys_SetDisplayMode(Thread_t* t) // 80 60: sizeIdx, pixelMode, fullscreen →
{
	uint32_t fullscreen = Thread_Pop(t), pixelMode = Thread_Pop(t), sizeIdx = Thread_Pop(t);
	if(pixelMode >= 2)
		ScriptError(MSG_BAD_PIXEL_MODE, t);
	if(sizeIdx >= (gEngine->gen >= GEN_1_494 ? 8u : 4u)) // the size table has eight entries from 1.494 on, four before
		ScriptError(MSG_BAD_SCREEN_SIZE, t);
	SetDisplayMode((int)sizeIdx, (int)pixelMode, (int)fullscreen);
	return 0;
}

static int Opcode_Sys_IsFullscreen(Thread_t* t) // 80 61: → f
{
	Thread_Push(t, (uint32_t)gFullscreen);
	return 0;
}

/* the keys (at most 15, 0-terminated) that toggle between windowed and
 * full screen, and whether they do; a longer list is a script error */
static int Opcode_Sys_SetStyleKeys(Thread_t* t) // 80 62: enable, keys →
{
	const uint32_t* keys = (const uint32_t*)PopPtr(t);
	uint32_t enable = Thread_Pop(t);
	if(!SetStyleChangeKeys((int)enable, keys))
		ScriptError(MSG_STYLE_KEYS_TOO_LONG, t);
	return 0;
}

// whether the full-screen picture is letter-boxed on wide displays (Display_SetPosMode)
static int Opcode_Sys_SetWindowPosMode(Thread_t* t) // 80 63 (1.64 on): v →
{
	Display_SetPosMode((int)Thread_Pop(t));
	return 0;
}

// show (f 1) or hide the main window; the key states are forgotten
static int Opcode_Sys_ShowWindow(Thread_t* t) // 80 64 (1.64 on): f →
{
	ShowMainWindow((int)Thread_Pop(t));
	Input_ClearStates();
	return 0;
}

/* minimise the main window: leave the full-screen display mode, forget
 * the key states, mute every sound and note that the script did it, so
 * that the next activation resumes the sounds and the display mode and
 * "80 0E" reports it meanwhile */
static int Opcode_Sys_Minimize(Thread_t* t) // 80 65 (.. 1.69 build 444)
{
	BGI_UNUSED(t);
	if(gFullscreen)
		Display_RestoreMode(); // back to the desktop's display mode
	Window_Minimize();
	Input_ClearStates();
	Sound_StopAll();
	Window_SetMinimizedByScript(1);
	return 0;
}

/* the minimise of 1.58: no display mode to restore (the full-screen check
 * came with 1.64) and no "minimised by the script" note for the activation
 * handler */
static int Opcode_Sys_Minimize_158(Thread_t* t) // 80 63 (1.58)
{
	BGI_UNUSED(t);
	Window_Minimize();
	Input_ClearStates();
	Sound_StopAll();
	return 0;
}

// the minimise of 1.69 build 472 on: the sounds keep playing
static int Opcode_Sys_Minimize_472(Thread_t* t) // 80 65 (1.69 build 472 on)
{
	BGI_UNUSED(t);
	if(gFullscreen)
		Display_RestoreMode();
	Window_Minimize();
	Input_ClearStates();
	Window_SetMinimizedByScript(1);
	return 0;
}

static int Opcode_Sys_SetTitle(Thread_t* t) // 80 66 (1.58: 80 64): text →
{
	OS_WindowSetTitle((const char*)PopPtr(t));
	return 0;
}

// the mouse cursor shape, 0 .. 4; another value is a script error
static int Opcode_Sys_SetCursorShape(Thread_t* t) // 80 67 (1.58: 80 66): n →
{
	int32_t n = (int32_t)Thread_Pop(t);
	if(n < 0 || n > 4)
		SYS_ERROR(t, MSG_BAD_CURSOR_SHAPE, n);
	gCursorShape = n;
	// the window procedure applies it on WM_SETCURSOR: 0 the arrow,
	// 1 the resource cursor; 2 .. 4 have no cursor in the table and show the class arrow
	OS_CursorSetShape(n);
	return 0;
}

/* whether the original waited for the vertical blank before presenting
 * (polling the scan line with Sleep).  Presentation is the OS layer's
 * here, so the flag is only stored. */
static int Opcode_Sys_SetVsyncWait(Thread_t* t) // 80 6E (1.69 build 472 on): on →
{
	Display_SetVsyncWait((int)Thread_Pop(t));
	return 0;
}

// switch the render-time profiler on or off ("80 07" reads it)
static int Opcode_Sys_ProfileEnable(Thread_t* t) // 80 06 (1.529 on): on →
{
	Profile_Enable((int)Thread_Pop(t));
	return 0;
}

/* a profiler figure into *out: mode 0 the number of passes, 1 the mean
 * pass time in ms, 2 the frame budget over the mean pass time in % (100:
 * exactly one period per pass), 3 the squared share of passes over the
 * budget in %, anything else 0 (present.c) */
static int Opcode_Sys_ProfileQuery(Thread_t* t) // 80 07 (1.529 on): out, mode →
{
	int mode = (int)Thread_Pop(t);
	int32_t* out = (int32_t*)PopPtr(t);
	*out = Profile_Query(mode);
	return 0;
}

// the next "90 F6" decodes its frame inside a wait object instead of at once
static int Opcode_Sys_SeqDeferDecode(Thread_t* t) // 80 53 (1.494 on)
{
	BmSeq_SetDeferFlag(1);
	return 0;
}

// the sleep of a main-loop pass that presented nothing, in ms (0 restores the default wait)
static int Opcode_Sys_SetIdleSleep(Thread_t* t) // 80 52 (1.535 on): ms →
{
	Display_SetIdleSleep((int)Thread_Pop(t));
	return 0;
}

/* what closing the window does: v 1 destroys it (the engine quits), 0
 * posts event 2 to the script's event queue ("80 A0") and leaves the
 * decision to the script */
static int Opcode_Sys_SetCloseMode(Thread_t* t) // 80 68: v →
{
	Set_CloseMode((int)Thread_Pop(t));
	return 0;
}

// ask the window to close, as a WM_CLOSE would ("80 68" decides what happens then)
static int Opcode_Sys_PostClose(Thread_t* t) // 80 69
{
	BGI_UNUSED(t);
	OS_WindowClose();
	return 0;
}

// quit the engine (scheduler code 6)
static int Opcode_Sys_Quit(Thread_t* t) // 80 6A
{
	BGI_UNUSED(t);
	return 6;
}

// make program `name` under `path` the boot program and restart the VM (scheduler code 5)
static int Opcode_Sys_Reboot(Thread_t* t) // 80 6B: path, name →
{
	const char* name = (const char*)PopPtr(t);
	const char* path = (const char*)PopPtr(t);
	SetBootProgram(path, name);
	return 5;
}

// whether the window accepts dropped files ("80 6D")
static int Opcode_Sys_DragdropEnable(Thread_t* t) // 80 6C: f →
{
	EnableDragDrop((int)Thread_Pop(t));
	return 0;
}

// the path of the file dropped on the window into buf; f 1 when there was one
static int Opcode_Sys_GetDroppedFile(Thread_t* t) // 80 6D: buf → f
{
	char* buf = (char*)PopPtr(t);
	Thread_Push(t, (uint32_t)GetDroppedFile(buf));
	return 0;
}

// whether the screen can show the full-screen mode of the current picture size
static int Opcode_Sys_CanFullscreen(Thread_t* t) // 80 6F: → f
{
	Thread_Push(t, (uint32_t)CanFullscreen(gSizeIndex));
	return 0;
}

// -------------------------------------------------------------------------
// 80 70 .. 80 7F : global memory and save files
// -------------------------------------------------------------------------

// the global memory is 0x1000 << n bytes, n in 0 .. 12; another n is a script error
static int Opcode_Sys_SetGmemSize(Thread_t* t) // 80 70: n → ok
{
	int32_t n = (int32_t)Thread_Pop(t);
	int ok = SetGlobalMemSize(n);
	if(!ok)
		SYS_ERROR(t, MSG_BAD_SIZE_NUMBER, n);
	Thread_Push(t, (uint32_t)ok);
	return 0;
}

static int Opcode_Sys_ClearGmem(Thread_t* t) // 80 71
{
	BGI_UNUSED(t);
	ClearGlobalMem();
	return 0;
}

// whether the save files are written scrambled (and expected so when read)
static int Opcode_Sys_SetSaveScramble(Thread_t* t) // 80 74: f →
{
	Set_SaveEncode((int)Thread_Pop(t));
	return 0;
}

/* write the global memory to save slot `slot` with a comment of at most
 * 0x27 characters (a longer one is a script error); the write's result is
 * not reported */
static int Opcode_Sys_Save(Thread_t* t) // 80 78: slot, comment →
{
	const char* comment = (const char*)PopPtr(t);
	uint32_t slot = Thread_Pop(t);
	if(strlen(comment) >= 0x28)
		SYS_ERROR(t, MSG_SAVE_COMMENT_TOO_LONG, 0x27);
	SaveWrite((int)slot, comment);
	return 0;
}

/* load save slot `slot` into the global memory: 0 ok, 1 no file, 2 wrong
 * size, 3 the scramble check failed.  The first 0x400 bytes of global
 * memory survive the load: they hold the system variables a title keeps
 * across saves. */
static int Opcode_Sys_Load(Thread_t* t) // 80 79: slot → code
{
	uint8_t keep[0x400];
	uint32_t slot;
	memcpy(keep, gGlobalMem, sizeof keep);
	slot = Thread_Pop(t);
	Thread_Push(t, (uint32_t)SaveLoad((int)slot));
	memcpy(gGlobalMem, keep, sizeof keep);
	return 0;
}

// the 0x40-byte header of save slot `slot` into buf: 0 ok, 1 no file, 2 wrong size
static int Opcode_Sys_SaveHeader(Thread_t* t) // 80 7A: buf, slot → code
{
	uint32_t slot = Thread_Pop(t);
	void* buf = PopPtr(t);
	Thread_Push(t, (uint32_t)SaveReadHeader((int)slot, buf));
	return 0;
}

// check save slot `slot` without loading it; the codes of "80 79"
static int Opcode_Sys_SaveCheck(Thread_t* t) // 80 7B: slot → code
{
	Thread_Push(t, (uint32_t)SaveCheck((int)Thread_Pop(t)));
	return 0;
}

// -------------------------------------------------------------------------
// 80 80 .. 80 8F : the global database, system area, names and flags
// -------------------------------------------------------------------------

/* read the global database BGI.gdb into the live state (system area,
 * names, flags, the first 0x400 bytes of global memory): 0 ok, 1 no file;
 * a damaged file is a script error (the code 2 behind it is not reached) */
static int Opcode_Sys_GdbLoad(Thread_t* t) // 80 80 (.. 1.69 build 444): → code
{
	uint32_t r = GlobalDB_Load();
	if(r == 0x80000002u)
	{
		ScriptError(MSG_GDB_CORRUPT, t);
		r = 2;
	}
	else if(r == 0x80000001u)
	{
		r = 1;
	}
	Thread_Push(t, r);
	return 0;
}

/* the same from 1.69 build 472 on: a damaged database is the result 2
 * rather than a script error, and the load also yields the window position
 * stored in the database (when the window would still be on the screen),
 * else the default centred position */
static int Opcode_Sys_GdbLoad_472(Thread_t* t) // 80 80 (1.69 build 472 on): → x, y, code
{
	uint32_t r = GlobalDB_Load();
	int x, y;
	if(r == 0x80000002u)
		r = 2;
	else if(r == 0x80000001u)
		r = 1;
	if(!SavedWindowPos_Load(&x, &y))
		Display_DefaultWindowPos(&x, &y);
	Thread_Push(t, (uint32_t)x);
	Thread_Push(t, (uint32_t)y);
	Thread_Push(t, r);
	return 0;
}

// write the global database; ok 1 when the file was written completely
static int Opcode_Sys_GdbSave(Thread_t* t) // 80 81: → ok
{
	Thread_Push(t, (uint32_t)GlobalDB_Save());
	return 0;
}

/* the bounds check of a system-area access (the area is gSysAreaSize,
 * 0x40000 bytes): the offset must lie inside it and the size, at least 1,
 * must not reach past its end; a violation is a script error */
static void SysareaCheck(Thread_t* t, uint32_t off, uint32_t size)
{
	if(off >= gSysAreaSize)
		SYS_ERROR(t, MSG_BAD_OFFSET, off);
	if(off + size > gSysAreaSize || size == 0 || size > gSysAreaSize)
		SYS_ERROR(t, MSG_BAD_OFFSET_SIZE, off, size);
}

// `size` bytes of src to offset `offset` of the system area (which the global database saves)
static int Opcode_Sys_SysareaWrite(Thread_t* t) // 80 82: offset, src, size →
{
	uint32_t size = Thread_Pop(t);
	const void* src = PopPtr(t);
	uint32_t off = Thread_Pop(t);
	SysareaCheck(t, off, size);
	memcpy(gSysArea + off, src, size);
	return 0;
}

// `size` bytes from offset `offset` of the system area into dst
static int Opcode_Sys_SysareaRead(Thread_t* t) // 80 83: dst, offset, size →
{
	uint32_t size = Thread_Pop(t);
	uint32_t off = Thread_Pop(t);
	void* dst = PopPtr(t);
	SysareaCheck(t, off, size);
	memcpy(dst, gSysArea + off, size);
	return 0;
}

// register a global name (names.h); ok 1 when it is present or was added, 0 when the table is full
static int Opcode_Sys_GnameAdd(Thread_t* t) // 80 84: name → ok
{
	Thread_Push(t, (uint32_t)GName_Add((const char*)PopPtr(t)));
	return 0;
}

static int Opcode_Sys_GnameExists(Thread_t* t) // 80 85: name → f
{
	Thread_Push(t, (uint32_t)GName_Exists((const char*)PopPtr(t)));
	return 0;
}

/* register a group of `bits` cleared flags under `name`, or resize an
 * existing group of that name (flags.h); ok 1, 0 for any error (a bad
 * name, zero bits, no free group) */
static int Opcode_Sys_GflagRegister(Thread_t* t) // 80 88: name, bits → ok
{
	uint32_t bits = Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)GFlag_RegisterGroup(name, bits));
	return 0;
}

/* the result codes of the flag manager as the "80 89" .. "80 8B" scripts
 * see them: 0 ok, 0x8008 unknown group → 1, 0x8009 index out of range →
 * 2, 0x800a bad count → 3 ("80 8A" only); anything else passes through */
static uint32_t GflagCode(uint32_t r)
{
	switch(r)
	{
		case 0: return 0;
		case 0x8008: return 1;
		case 0x8009: return 2;
		case 0x800a: return 3;
		default: return r;
	}
}

/* set (val != 0) or clear flag `idx` of group `name`: 0 ok, 1 unknown
 * group, 2 index out of range.  Before 1.494 the two errors are script
 * errors instead of results. */
static int Opcode_Sys_GflagSet(Thread_t* t) // 80 89: name, idx, val → code
{
	uint32_t val = Thread_Pop(t), idx = Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	uint32_t code = GflagCode(GFlag_SetBit(name, idx, val));
	// 1.494 on: the codes are only returned, no error is raised
	if(gEngine->gen < GEN_1_494)
	{
		if(code == 1)
			SYS_ERROR(t, MSG_GFLAG_GROUP_UNKNOWN, name);
		if(code == 2)
			SYS_ERROR(t, MSG_GFLAG_BAD_INDEX, name, idx);
	}
	Thread_Push(t, code);
	return 0;
}

/* set or clear `count` flags from `idx` on (count 1 .. 0x10000, within
 * the group): the codes of "80 89" plus 3 for a bad count; script errors
 * before 1.494 */
static int Opcode_Sys_GflagSetN(Thread_t* t) // 80 8A: name, idx, val, count → code
{
	uint32_t count = Thread_Pop(t), val = Thread_Pop(t), idx = Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	uint32_t code = GflagCode(GFlag_SetBits(name, idx, val, count));
	if(gEngine->gen < GEN_1_494) // (see "80 89")
	{
		if(code == 1)
			SYS_ERROR(t, MSG_GFLAG_GROUP_UNKNOWN, name);
		if(code == 2)
			SYS_ERROR(t, MSG_GFLAG_BAD_INDEX, name, idx);
		if(code == 3)
			SYS_ERROR(t, MSG_GFLAG_BAD_COUNT, name, idx, count);
	}
	Thread_Push(t, code);
	return 0;
}

/* flag `idx` of group `name` into *out - non-zero when set, not 0 / 1
 * (the scripts test it for zero); the codes of "80 89", script errors
 * before 1.494 */
static int Opcode_Sys_GflagGet(Thread_t* t) // 80 8B: out, name, idx → code
{
	uint32_t idx = Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	uint32_t* out = (uint32_t*)PopPtr(t);
	uint32_t code = GflagCode(GFlag_GetBit(out, name, idx));
	if(gEngine->gen < GEN_1_494) // (see "80 89")
	{
		if(code == 1)
			SYS_ERROR(t, MSG_GFLAG_GROUP_UNKNOWN, name);
		if(code == 2)
			SYS_ERROR(t, MSG_GFLAG_BAD_INDEX, name, idx);
	}
	Thread_Push(t, code);
	return 0;
}

// -------------------------------------------------------------------------
// 80 90 .. 80 9F : message history and rings
// -------------------------------------------------------------------------

// drop the message back-log and allow `max` records from now on
static int Opcode_Sys_HistReset(Thread_t* t) // 80 90: max →
{
	History_Reset((int)Thread_Pop(t));
	return 0;
}

static int Opcode_Sys_HistCount(Thread_t* t) // 80 91: → n
{
	Thread_Push(t, (uint32_t)History_Count());
	return 0;
}

/* append a record to the message back-log (history.h): the nine integers
 * a1 .. a9 the script chooses (speaker, voice, flags ...), the archive,
 * file and name of the message and the message itself.  The oldest record
 * goes when the log is full.  A string that is too long - arc, file, name
 * above 0x1f characters, msg above 0xff - is a script error. */
static int Opcode_Sys_HistAdd(Thread_t* t) // 80 94: a1 .. a9, arc, file, name, msg →
{
	const char* msg = (const char*)PopPtr(t);
	const char* name = (const char*)PopPtr(t);
	const char* file = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	uint32_t a9 = Thread_Pop(t), a8 = Thread_Pop(t), a7 = Thread_Pop(t), a6 = Thread_Pop(t), a5 = Thread_Pop(t);
	uint32_t a4 = Thread_Pop(t), a3 = Thread_Pop(t), a2 = Thread_Pop(t), a1 = Thread_Pop(t);
	uint32_t r = History_Add(a1, a2, a3, a4, a5, a6, a7, a8, a9, arc, file, name, msg, NULL);
	switch(r)
	{
		case 0x80000001u: SYS_ERROR(t, MSG_HIST_ARC_TOO_LONG, 0x1f);
		case 0x80000002u: SYS_ERROR(t, MSG_HIST_FILE_TOO_LONG, 0x1f);
		case 0x80000003u: SYS_ERROR(t, MSG_HIST_NAME_TOO_LONG, 0x1f);
		case 0x80000004u: SYS_ERROR(t, MSG_HIST_MSG_TOO_LONG, 0xff);
		default: break;
	}
	return 0;
}

/* record `idx` of the back-log (0 is the newest) as a HistoryRec_t
 * without its reading list into buf; a bad index is a script error */
static int Opcode_Sys_HistGet(Thread_t* t) // 80 95: buf, idx → ok
{
	uint32_t idx = Thread_Pop(t);
	void* buf = PopPtr(t);
	int ok = History_Get(buf, (int)idx, 0);
	if(!ok)
		SYS_ERROR(t, MSG_BAD_HISTORY_INDEX, idx);
	Thread_Push(t, (uint32_t)ok);
	return 0;
}

/* append a record given as a HistoryRec_t, reading list included; the
 * length limits of "80 94" apply (the reading: 0x1ff characters) */
static int Opcode_Sys_HistAddStruct(Thread_t* t) // 80 96: rec →
{
	const void* rec = PopPtr(t);
	switch(History_AddStruct(rec))
	{
		case 0x80000001u: SYS_ERROR(t, MSG_HIST2_ARC_TOO_LONG, 0x1f);
		case 0x80000002u: SYS_ERROR(t, MSG_HIST2_FILE_TOO_LONG, 0x1f);
		case 0x80000003u: SYS_ERROR(t, MSG_HIST2_NAME_TOO_LONG, 0x1f);
		case 0x80000004u: SYS_ERROR(t, MSG_HIST2_MSG_TOO_LONG, 0xff);
		case 0x80000005u: SYS_ERROR(t, MSG_HIST2_READING_TOO_LONG, 0x1ff);
		default: break;
	}
	return 0;
}

// "80 95" with the reading list (the record is 0x400 bytes then)
static int Opcode_Sys_HistGetEx(Thread_t* t) // 80 97: buf, idx → ok
{
	uint32_t idx = Thread_Pop(t);
	void* buf = PopPtr(t);
	int ok = History_Get(buf, (int)idx, 1);
	if(!ok)
		SYS_ERROR(t, MSG_BAD_HISTORY_INDEX, idx);
	Thread_Push(t, (uint32_t)ok);
	return 0;
}

/* a ring (rings.h) keeping the last `maxCount` records of `elemSize`
 * bytes, its id in *outId; 0 ok, 2 for a zero count or size */
static int Opcode_Sys_RingCreate(Thread_t* t) // 80 98: outId, maxCount, elemSize → code
{
	uint32_t elemSize = Thread_Pop(t), maxCount = Thread_Pop(t);
	uint32_t* outId = (uint32_t*)PopPtr(t);
	Thread_Push(t, Ring_Create(outId, maxCount, elemSize));
	return 0;
}

// 0 ok, 1 unknown ring
static int Opcode_Sys_RingDelete(Thread_t* t) // 80 99: id → code
{
	Thread_Push(t, Ring_Delete(Thread_Pop(t)));
	return 0;
}

// the records ring `id` holds into *out; 0 ok, 1 unknown ring
static int Opcode_Sys_RingCount(Thread_t* t) // 80 9A: out, id → code
{
	uint32_t id = Thread_Pop(t);
	uint32_t* out = (uint32_t*)PopPtr(t);
	Thread_Push(t, Ring_Count(out, id));
	return 0;
}

// push one record (elemSize bytes) as the newest, dropping the oldest beyond maxCount; 0 ok, 1 unknown ring
static int Opcode_Sys_RingPush(Thread_t* t) // 80 9C: id, data → code
{
	const void* data = PopPtr(t);
	uint32_t id = Thread_Pop(t);
	Thread_Push(t, Ring_Push(id, data));
	return 0;
}

// record `idx` (0 is the newest) into buf; 0 ok, 1 unknown ring, 2 index out of range
static int Opcode_Sys_RingGet(Thread_t* t) // 80 9D: buf, id, idx → code
{
	uint32_t idx = Thread_Pop(t), id = Thread_Pop(t);
	void* buf = PopPtr(t);
	Thread_Push(t, Ring_Get(buf, id, idx));
	return 0;
}

// 1.616 on: remove `count` records from index `idx` (0 is the newest) on; 0 ok, 1 unknown ring, 2 index out of range
static int Opcode_Sys_RingRemove(Thread_t* t) // 80 9E (1.616 on): id, idx, count → code
{
	uint32_t count = Thread_Pop(t), idx = Thread_Pop(t), id = Thread_Pop(t);
	Thread_Push(t, Ring_Remove(id, idx, count));
	return 0;
}

// -------------------------------------------------------------------------
// 80 A0 .. 80 AF : the event queue and display-object priorities
// -------------------------------------------------------------------------

/* the oldest event of the engine's queue (events.h: three dwords - key
 * presses, mouse buttons, the close request, dropped files, the script's
 * own posts ...) into buf[0 .. 2]; f 1 when there was one */
static int Opcode_Sys_MsgqGet(Thread_t* t) // 80 A0: buf → f
{
	uint32_t* buf = (uint32_t*)PopPtr(t);
	Thread_Push(t, (uint32_t)MsgQueue_Get(buf));
	return 0;
}

// post the event (0, a, b) to the queue, kind 0 being the script's own
static int Opcode_Sys_MsgqPost(Thread_t* t) // 80 A1: a, b →
{
	uint32_t b = Thread_Pop(t), a = Thread_Pop(t);
	MsgQueue_Post(0, a, b);
	return 0;
}

/* Sub-objects are the controllers (ObjProc_t, panel.h) of the panels
 * created by "90 B8", addressed by their id.  "80 A8" enables or disables
 * one; f 0 for an unknown id. */
static int Opcode_Sys_SubobjSetEnabled(Thread_t* t) // 80 A8: id, on → f
{
	int on = (int)Thread_Pop(t);
	ObjProc_t* o = SubObj_Find(Thread_Pop(t));
	if(o)
		ObjProc_SetEnabled(o, on);
	Thread_Push(t, o != NULL);
	return 0;
}

// the enabled state of sub-object `id` into *out; f 0 for an unknown id
static int Opcode_Sys_SubobjGetEnabled(Thread_t* t) // 80 A9: id, out → f
{
	uint32_t* out = (uint32_t*)PopPtr(t);
	ObjProc_t* o = SubObj_Find(Thread_Pop(t));
	if(o)
		*out = (uint32_t)ObjProc_GetEnabled(o);
	Thread_Push(t, o != NULL);
	return 0;
}

// post a message of n dwords (1 .. 0x100) to sub-object `id`; f 0 for an unknown id
static int Opcode_Sys_SubobjPost(Thread_t* t) // 80 AC: id, n, data → f
{
	const uint32_t* data = (const uint32_t*)PopPtr(t);
	int n = (int)Thread_Pop(t);
	ObjProc_t* o = SubObj_Find(Thread_Pop(t));
	if(o)
		ObjProc_PostMessage(o, n, data);
	Thread_Push(t, o != NULL);
	return 0;
}

// -------------------------------------------------------------------------
// 80 B0 .. 80 BF : named locks
// -------------------------------------------------------------------------

// a named lock (locks.h) admitting `max` holders at once; ok 1 created, 0 the name exists
static int Opcode_Sys_SemCreate(Thread_t* t) // 80 B0: name, max → ok
{
	uint32_t max = Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)SemMgr_Create(gSemMgr, name, max));
	return 0;
}

// 0 ok, 0x80000001 unknown lock, 0x80000002 the lock is held or awaited
static int Opcode_Sys_SemDelete(Thread_t* t) // 80 B1: name → code
{
	Thread_Push(t, SemMgr_Delete(gSemMgr, (const char*)PopPtr(t)));
	return 0;
}

/* queue this thread for lock `name` behind the waiters of equal or higher
 * priority and block until it heads the queue and a place is free; the
 * wait pushes 0 when the lock was acquired, 0x80000001 for an unknown
 * lock, -1 when abandoned in non-blocking mode */
static int Opcode_Sys_SemWait(Thread_t* t) // 80 B4: name, prio → (pushes later)
{
	uint32_t prio = Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	Thread_SetWait(t, WaitLock_New(t, gSemMgr, name, prio));
	return 2;
}

// give this thread's place in lock `name` back: 0 ok, 0x80000001 unknown lock, 0x80000003 it holds no place
static int Opcode_Sys_SemRelease(Thread_t* t) // 80 B5: name → code
{
	const char* name = (const char*)PopPtr(t);
	Thread_Push(t, SemMgr_Release(gSemMgr, name, Thread_GetId(t)));
	return 0;
}

// would a request of priority `prio` be admitted now: 0 yes, 0x80000001 unknown lock, 0x80000004 no
static int Opcode_Sys_SemQuery(Thread_t* t) // 80 B6: name, prio → code
{
	uint32_t prio = Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	Thread_Push(t, SemMgr_Query(gSemMgr, name, prio));
	return 0;
}

// -------------------------------------------------------------------------
// 80 C0 .. 80 CF : SDC / DCFS coding
// -------------------------------------------------------------------------

/* SDC-encode `size` bytes of src into dst (codec.h; dst needs room for
 * size + size / 128 + 1 bytes plus the header).  The encoder ran on a
 * worker thread in the original, hence the wait: it pushes the encoded
 * size, 0 when the verification decode failed. */
static int Opcode_Sys_SdcEncodeAsync(Thread_t* t) // 80 C0: dst, src, size → (pushes later)
{
	uint32_t size = Thread_Pop(t);
	const void* src = PopPtr(t);
	void* dst = PopPtr(t);
	Thread_SetWait(t, WaitEncode_New(t, dst, src, size));
	return 2;
}

// decode the SDC file at src into dst; the decoded size, 0 when the magic or the checks fail
static int Opcode_Sys_SdcDecode(Thread_t* t) // 80 C1: dst, src → size
{
	const void* src = PopPtr(t);
	void* dst = PopPtr(t);
	Thread_Push(t, SdcDecode(dst, src));
	return 0;
}

/* difference-code a sequence of frames (DCFS) and SDC-encode the result
 * into dst.  The two integers reach the encoder as the frame size in bytes
 * (`w`) and the frame count (`h`).  The wait pushes the encoded size, 0 on
 * failure. */
static int Opcode_Sys_DcfsEncodeAsync(Thread_t* t) // 80 C4: dst, src, w, h → (pushes later)
{
	uint32_t h = Thread_Pop(t), w = Thread_Pop(t);
	const void* src = PopPtr(t);
	void* dst = PopPtr(t);
	Thread_SetWait(t, WaitEncode2_New(t, dst, src, w, h));
	return 2;
}

/* the inverse of "80 C4": SDC-decode src into a temporary, then
 * DCFS-decode that into dst.  Pushes the frame count of the DCFS header,
 * 0 when src is not an SDC file or the DCFS decode fails. */
static int Opcode_Sys_DcfsDecode(Thread_t* t) // 80 C5: dst, src → n
{
	const void* src = PopPtr(t);
	void* dst = PopPtr(t);
	uint32_t n = 0;
	if(SdcIsFormat(src))
	{
		uint8_t* tmp = (uint8_t*)BGI_Alloc(SdcOutSize(src));
		SdcDecode(tmp, src);
		if(DcfsDecode(dst, tmp) == 0)
			n = ((const DcfsHeader_t*)tmp)->frameCount;
		BGI_Free(tmp);
	}
	Thread_Push(t, n);
	return 0;
}

// -------------------------------------------------------------------------
// 80 D0 .. 80 DF : dictionaries and string tables
// -------------------------------------------------------------------------

/* a dictionary (dicts.h) of `elemSize`-byte records (at least 2) keyed
 * by strings, its id in *outId; 0 ok, 0x80000001 bad size */
static int Opcode_Sys_DictCreate(Thread_t* t) // 80 D0: outId, elemSize → code
{
	uint32_t elemSize = Thread_Pop(t);
	uint32_t* outId = (uint32_t*)PopPtr(t);
	Thread_Push(t, Dict_Create(outId, elemSize));
	return 0;
}

// 0 ok, 0x80000002 unknown dictionary
static int Opcode_Sys_DictDelete(Thread_t* t) // 80 D1: id → code
{
	Thread_Push(t, Dict_Delete(Thread_Pop(t)));
	return 0;
}

// store a copy of the record at data under `key`, replacing an existing one; 0 ok, 0x80000002 unknown dictionary
static int Opcode_Sys_DictSet(Thread_t* t) // 80 D2: id, key, data → code
{
	const void* data = PopPtr(t);
	const char* key = (const char*)PopPtr(t);
	uint32_t id = Thread_Pop(t);
	Thread_Push(t, Dict_Set(id, key, data));
	return 0;
}

// 0 ok, 0x80000002 unknown dictionary, 0x80000003 unknown key
static int Opcode_Sys_DictRemove(Thread_t* t) // 80 D3: id, key → code
{
	const char* key = (const char*)PopPtr(t);
	uint32_t id = Thread_Pop(t);
	Thread_Push(t, Dict_Remove(id, key));
	return 0;
}

/* the record of `key`, or the one at position `idx` (insertion order)
 * when key is NULL, into buf; 0 ok, 0x80000002 unknown dictionary,
 * 0x80000003 unknown key / position */
static int Opcode_Sys_DictGet(Thread_t* t) // 80 D4: buf, id, key, idx → code
{
	uint32_t idx = Thread_Pop(t);
	const char* key = (const char*)PopPtr(t);
	uint32_t id = Thread_Pop(t);
	void* buf = PopPtr(t);
	Thread_Push(t, Dict_Get(buf, id, key, idx));
	return 0;
}

// drop every string table (strtables.h) but, from 1.616 on, the one the global names live in
static int Opcode_Sys_StrtabClear(Thread_t* t) // 80 D8
{
	BGI_UNUSED(t);
	StrTab_Clear(1);
	return 0;
}

// the strings in table `id`, 0 for an unknown table
static int Opcode_Sys_StrtabCount(Thread_t* t) // 80 D9: id → n
{
	Thread_Push(t, (uint32_t)StrTab_Count(Thread_Pop(t)));
	return 0;
}

// append a copy of `str` to table `id`, creating the table when needed
static int Opcode_Sys_StrtabAdd(Thread_t* t) // 80 DC (.. 1.588): id, str →
{
	const char* str = (const char*)PopPtr(t);
	uint32_t id = Thread_Pop(t);
	StrTab_Add(id, str);
	return 0;
}

/* "80 DC" from 1.599 on: the table keeps each string once, and the
 * string's index (new or existing) is pushed */
static int Opcode_Sys_StrtabAddIdx(Thread_t* t) // 80 DC (1.599 on): id, str → idx
{
	const char* str = (const char*)PopPtr(t);
	uint32_t id = Thread_Pop(t);
	Thread_Push(t, (uint32_t)StrTab_Add(id, str));
	return 0;
}

// string `idx` of table `id` into buf; 0 ok, 0x80000001 unknown table, 0x80000002 bad index
static int Opcode_Sys_StrtabGet(Thread_t* t) // 80 DD: buf, id, idx → code
{
	uint32_t idx = Thread_Pop(t), id = Thread_Pop(t);
	char* buf = (char*)PopPtr(t);
	Thread_Push(t, (uint32_t)StrTab_Get(buf, id, idx));
	return 0;
}

/* replace table `id` by the `count` NUL-terminated strings following each
 * other at strs (count 0 drops the table); ok 1 done, 0 for a negative
 * count or no strings */
static int Opcode_Sys_StrtabSetAll(Thread_t* t) // 80 DA (1.599 on): id, count, strs → ok
{
	const char* strs = (const char*)PopPtr(t);
	int count = (int)Thread_Pop(t);
	uint32_t id = Thread_Pop(t);
	Thread_Push(t, (uint32_t)StrTab_SetAll(id, count, strs));
	return 0;
}

/* every string of table `id`, terminators included, one after the other
 * into buf (NULL: only measure); the bytes they take, 0 for an unknown
 * table */
static int Opcode_Sys_StrtabGather(Thread_t* t) // 80 DB (1.599 on): buf, id → bytes
{
	uint32_t id = Thread_Pop(t);
	char* buf = (char*)PopPtr(t);
	Thread_Push(t, (uint32_t)StrTab_Gather(buf, id));
	return 0;
}

// the length of string `idx` of table `id` into *outLen; the codes of "80 DD"
static int Opcode_Sys_StrtabLength(Thread_t* t) // 80 DE (1.599 on): outLen, id, idx → code
{
	uint32_t idx = Thread_Pop(t), id = Thread_Pop(t);
	int32_t* outLen = (int32_t*)PopPtr(t);
	Thread_Push(t, (uint32_t)StrTab_Length(outLen, id, idx));
	return 0;
}

// -------------------------------------------------------------------------
// 80 E0 .. 80 EF : external programs, registration
// -------------------------------------------------------------------------

/* start "<p1>\<p2>" (p2 in the base directory when p1 is NULL) as a new
 * process and wait for it to end, hiding the main window meanwhile when
 * `a` is set.  While the file's drive is not ready the message p3 is shown
 * with OK / Cancel, Cancel asking whether to give up (install.h).  r 1
 * when the process was started. */
static int Opcode_Sys_RunProgram(Thread_t* t) // 80 E0: p1, p2, p3, a → r
{
	uint32_t a = Thread_Pop(t);
	const char* p3 = (const char*)PopPtr(t);
	const char* p2 = (const char*)PopPtr(t);
	const char* p1 = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)RunProgram(p1, p2, p3, (int)a, 1, 1, 0));
	return 0;
}

/* the program to start once the engine has shut down: "<p1>\<file>" (the
 * base directory when p1 is NULL) with the "drive not ready" message p3.
 * Copies of the strings are kept for Engine_Shutdown; the window is
 * destroyed and the VM ends (scheduler code 6).  A NULL file is a script
 * error. */
static int Opcode_Sys_ExecOnExit(Thread_t* t) // 80 E1: p1, file, p3 →
{
	const char* p3 = (const char*)PopPtr(t);
	const char* file = (const char*)PopPtr(t);
	const char* p1 = (const char*)PopPtr(t);

	gExitArg0 = p1 ? BGI_Strdup(p1) : NULL;
	if(file)
		gExitFile = BGI_Strdup(file);
	else
		ScriptError(MSG_EXEC_FILE_NULL, t);
	gExitArg2 = p3 ? BGI_Strdup(p3) : NULL;
	OS_WindowDestroy();
	return 6;
}

/* "80 E0" for the uninstaller: the main window is hidden, the message p3
 * is informational (no retry), and the engine waits for the process and
 * for the uninstaller's mutex to go; r 1 when the process was started */
static int Opcode_Sys_RunProgramWait(Thread_t* t) // 80 E2: p1, p2, p3 → r
{
	const char* p3 = (const char*)PopPtr(t);
	const char* p2 = (const char*)PopPtr(t);
	const char* p1 = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)RunProgram(p1, p2, p3, 1, 1, 0, 1));
	return 0;
}

// open a file or URL with its associated program (ShellExecute "open")
static int Opcode_Sys_ShellOpen(Thread_t* t) // 80 E3: path → ok
{
	Thread_Push(t, (uint32_t)ShellOpen((const char*)PopPtr(t)));
	return 0;
}

/* the 8-byte hash of a file (the hash of the BGI.hvl list, HvHash_File)
 * into *out; a relative path is taken under the base directory.  ok 0
 * when the file cannot be opened. */
static int Opcode_Sys_HashFile(Thread_t* t) // 80 E9 (1.69 build 472 on): out, path → ok
{
	const char* path = (const char*)PopPtr(t);
	uint8_t* out = (uint8_t*)PopPtr(t);
	char full[0x210];
	if(path[0] == '\\' || path[1] == ':')
		strcpy(full, path);
	else
		sprintf(full, "%s%s", gBaseDir, path);
	Thread_Push(t, (uint32_t)HvHash_File(out, full));
	return 0;
}

/* the product name into buf: every build of the original copies out its
 * own literal ("Tayutama", "NurseryRhyme", "ItsukaTodokuAnoSorani", ...),
 * here it is the "product" of the games.json entry that matched the game
 * (Engine_ProductName, version.h) */
static int Opcode_Sys_GetProductName(Thread_t* t) // 80 E8: buf →
{
	strcpy((char*)PopPtr(t), Engine_ProductName());
	return 0;
}

/* whether the game is registered (comap.dat holds the machine key); when
 * it is not, the registration program is run and given time to write it */
static int Opcode_Sys_CheckRegistration(Thread_t* t) // 80 EC: → f
{
	Thread_Push(t, (uint32_t)CheckRegistration());
	return 0;
}

// the first 0x404 bytes of comap.dat into buf; ok 1 when the file could be opened
static int Opcode_Sys_ReadComap(Thread_t* t) // 80 ED: buf → ok
{
	Thread_Push(t, (uint32_t)ReadComapDat(PopPtr(t)));
	return 0;
}

// the machine key (install.h): processor and OS bits, the user's and the computer's first letter
static int Opcode_Sys_MachineKey(Thread_t* t) // 80 EE: → v
{
	Thread_Push(t, GetMachineKey());
	return 0;
}

// the machine key XOR x
static int Opcode_Sys_MachineKeyXor(Thread_t* t) // 80 EF: x → v
{
	Thread_Push(t, GetMachineKeyXor(Thread_Pop(t)));
	return 0;
}

// -------------------------------------------------------------------------
// 80 F0 .. 80 FF : the installer
// -------------------------------------------------------------------------

/* the installer's options dialog: the installation path and two check
 * boxes (install.h).  The chosen path goes to p1, the boxes to p2 / p3;
 * r 1 for OK, 0 for cancel. */
static int Opcode_Sys_InstallDialogA(Thread_t* t) // 80 F0: p1, p2, p3, p4, n5, n6, p7, p8 → r
{
	const char* p8 = (const char*)PopPtr(t);         // OK button caption, or NULL
	const char* p7 = (const char*)PopPtr(t);         // product folder appended to a browsed root
	uint32_t n6 = Thread_Pop(t), n5 = Thread_Pop(t); // initial state of the check boxes
	const char* p4 = (const char*)PopPtr(t);         // initial path
	int32_t* p3 = (int32_t*)PopPtr(t);               // out: check box 2
	int32_t* p2 = (int32_t*)PopPtr(t);               // out: check box 1
	char* p1 = (char*)PopPtr(t);                     // out: the chosen path
	Thread_Push(t, (uint32_t)Install_DialogA(p1, p2, p3, p4, (int)n5, (int)n6, p7, p8));
	return 0;
}

/* the installer's choice dialog: the text p1 and two or three radio
 * buttons p2 .. p4 (p2 NULL selects the two-button form); n5 is the
 * button to disable (0 .. 2, else none), n6 shows the extra button.  r -1
 * cancelled, 0 .. 2 the chosen button, 3 the extra button or no choice. */
static int Opcode_Sys_InstallDialogB(Thread_t* t) // 80 F1: p1, p2, p3, p4, n5, n6 → r
{
	uint32_t n6 = Thread_Pop(t), n5 = Thread_Pop(t);
	const char* p4 = (const char*)PopPtr(t);
	const char* p3 = (const char*)PopPtr(t);
	const char* p2 = (const char*)PopPtr(t);
	const char* p1 = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)Install_DialogB(p1, p2, p3, p4, (int)n5, (int)n6));
	return 0;
}

/*
 * The installation itself (Install_RunDialog, install.h).  Fourteen
 * operands, numbered in the order they are popped (v1 is on top of the
 * stack, p14 was pushed first):
 *   v1   cancel button allowed        s2   "insert the disc" message
 *   s3   uninstaller file name        s4   product (registry key)
 *   s5   company (registry key)       s6   save file name format or NULL
 *   v7   number of save slots         l8   "insert disc i" messages
 *   l9   disc marker files            s10  int array: files per disc
 *   v11  number of discs (entries in l8 / l9)
 *   l12  files to install (0-terminated)
 *   l13  sub-directories to create (0-terminated) or NULL
 *   p14  the installation directory
 * The lists of tagged string pointers are resolved to host pointers here;
 * the installer sees plain char* arrays.  r 1 when everything succeeded,
 * 0 when not (everything created is removed again then).
 */
static int Opcode_Sys_InstallRun(Thread_t* t) // 80 F2: p14, l13, l12, v11, s10, l9, l8, v7, s6, s5, s4, s3, s2, v1 → r
{
	uint32_t v1 = Thread_Pop(t);
	const char* s2 = (const char*)PopPtr(t);
	const char* s3 = (const char*)PopPtr(t);
	const char* s4 = (const char*)PopPtr(t);
	const char* s5 = (const char*)PopPtr(t);
	const char* s6 = (const char*)PopPtr(t);
	uint32_t v7 = Thread_Pop(t);
	const uint32_t* l8 = (const uint32_t*)PopPtr(t);
	const uint32_t* l9 = (const uint32_t*)PopPtr(t);
	const char* s10 = (const char*)PopPtr(t);
	uint32_t v11 = Thread_Pop(t);
	const uint32_t* l12 = (const uint32_t*)PopPtr(t);
	const uint32_t* l13 = (const uint32_t*)PopPtr(t);
	void* p14 = PopPtr(t);
	char **n13 = NULL, **n12, **n9, **n8;
	int len, r;

	if(l13)
	{
		len = TaggedListLen(l13);
		n13 = (char**)BGI_Alloc((size_t)len * sizeof(char*));
		ResolvePtrArray((void**)n13, t, l13, len);
	}
	len = TaggedListLen(l12);
	n12 = (char**)BGI_Alloc((size_t)len * sizeof(char*));
	ResolvePtrArray((void**)n12, t, l12, len);
	n9 = (char**)BGI_Alloc(v11 * sizeof(char*));
	ResolvePtrArray((void**)n9, t, l9, (int)v11);
	n8 = (char**)BGI_Alloc(v11 * sizeof(char*));
	ResolvePtrArray((void**)n8, t, l8, (int)v11);

	r = Install_RunDialog((const char*)p14, n13, n12, (int)v11, (const int*)s10, n9, n8,
		(int)v7, s6, s5, s4, s3, s2, (int)v1);
	Thread_Push(t, (uint32_t)r);

	BGI_Free(n8);
	BGI_Free(n9);
	BGI_Free(n12);
	if(n13)
		BGI_Free(n13);
	return 0;
}

/* the links of an installation (install.h): "<p1>\<p2>" as <p3> on the
 * desktop when n2 is set and, when n1 is set, the start-menu folder <p6>
 * with <p3> -> "<p1>\<p2>" and <p5> -> "<p1>\<p4>".  r 1 when the
 * start-menu part succeeded or was not asked for. */
static int Opcode_Sys_CreateShortcuts(Thread_t* t) // 80 F3: p1 .. p6, n1, n2 → r
{
	uint32_t n2 = Thread_Pop(t), n1 = Thread_Pop(t);
	const char* p6 = (const char*)PopPtr(t);
	const char* p5 = (const char*)PopPtr(t);
	const char* p4 = (const char*)PopPtr(t);
	const char* p3 = (const char*)PopPtr(t);
	const char* p2 = (const char*)PopPtr(t);
	const char* p1 = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)CreateShortcuts(p1, p2, p3, p4, p5, p6, (int)n1, (int)n2));
	return 0;
}

/* delete every file named in "<dir>\uninst.lst" except the lines starting
 * with '@', the list itself and the names of the 0-terminated `list`; r 1
 * when the list could be opened */
static int Opcode_Sys_UninstProcess(Thread_t* t) // 80 F4: dir, list → r
{
	const uint32_t* list = (const uint32_t*)PopPtr(t);
	const char* dir = (const char*)PopPtr(t);
	int len = TaggedListLen(list);
	char** names = (char**)BGI_Alloc((size_t)len * sizeof(char*));
	ResolvePtrArray((void**)names, t, list, len);
	Thread_Push(t, (uint32_t)UninstListProcess(dir, names));
	BGI_Free(names);
	return 0;
}

// append the names of the 0-terminated `list` that "<dir>\uninst.lst" lacks; r 1 when the list was rewritten
static int Opcode_Sys_UninstAppend(Thread_t* t) // 80 F5: dir, list → r
{
	const uint32_t* list = (const uint32_t*)PopPtr(t);
	const char* dir = (const char*)PopPtr(t);
	int len = TaggedListLen(list);
	char** names = (char**)BGI_Alloc((size_t)len * sizeof(char*));
	ResolvePtrArray((void**)names, t, list, len);
	Thread_Push(t, (uint32_t)UninstListAppend(dir, names));
	BGI_Free(names);
	return 0;
}

/* delete the links "<Desktop>\<p1>", "<Programs>\<p3>\<p1>" and
 * "<Programs>\<p3>\<p2>", then the folder <p3> itself when f is set */
static int Opcode_Sys_DeleteShortcuts(Thread_t* t) // 80 F6: p1, p2, p3, f →
{
	uint32_t f = Thread_Pop(t);
	const char* p3 = (const char*)PopPtr(t);
	const char* p2 = (const char*)PopPtr(t);
	const char* p1 = (const char*)PopPtr(t);
	DeleteShortcuts(p1, p2, p3, (int)f);
	return 0;
}

/* one link: "<Programs>\<p1>\<p2>" -> p3, or "<Desktop>\<p2>" when p1 is
 * NULL (the folder is created as needed); r 1 when made */
static int Opcode_Sys_CreateShortcut(Thread_t* t) // 80 F7: p1, p2, p3 → r
{
	const char* p3 = (const char*)PopPtr(t);
	const char* p2 = (const char*)PopPtr(t);
	const char* p1 = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)CreateShortcut(p1, p2, p3));
	return 0;
}

// HKLM\Software\<p1>\<p2>\InstalledFolder into buf (0x104 bytes); ok 1 when present
static int Opcode_Sys_RegGetInstallDir(Thread_t* t) // 80 F8: buf, p1, p2 → ok
{
	const char* p2 = (const char*)PopPtr(t);
	const char* p1 = (const char*)PopPtr(t);
	char* buf = (char*)PopPtr(t);
	Thread_Push(t, (uint32_t)RegGetInstalledFolder(buf, p1, p2));
	return 0;
}

// delete the registry key HKLM\Software\<p1>\<p2>; ok 1 when deleted
static int Opcode_Sys_RegDeleteKey(Thread_t* t) // 80 F9: p1, p2 → ok
{
	const char* p2 = (const char*)PopPtr(t);
	const char* p1 = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)RegDeleteSoftwareKey(p1, p2));
	return 0;
}

/* the path stored in "<Windows dir>\<name>" into buf (the file holds a
 * path ending in "\", which is removed); ok 1 when read */
static int Opcode_Sys_ReadWindirFile(Thread_t* t) // 80 FA: buf, name → ok
{
	const char* name = (const char*)PopPtr(t);
	char* buf = (char*)PopPtr(t);
	Thread_Push(t, (uint32_t)ReadWinDirFile(buf, name));
	return 0;
}

// the Windows directory into buf (selector 0 of "80 3A")
static int Opcode_Sys_GetWindowsDir(Thread_t* t) // 80 FB: buf →
{
	GetSpecialFolder((char*)PopPtr(t), 0);
	return 0;
}

/* associate file extension p1 with the game: HKCR\.<p1> = p2 (the
 * progid), HKCR\<p2> = p3 (the description), its DefaultIcon = p4 and its
 * Shell\Open\Command = p5; ok 1 when every key was written */
static int Opcode_Sys_RegisterAssoc(Thread_t* t) // 80 FC: p5 .. p1 → ok
{
	const char* p5 = (const char*)PopPtr(t);
	const char* p4 = (const char*)PopPtr(t);
	const char* p3 = (const char*)PopPtr(t);
	const char* p2 = (const char*)PopPtr(t);
	const char* p1 = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)RegisterFileAssoc(p1, p2, p3, p4, p5));
	return 0;
}

// whether the engine was started as a launcher (the command line's "Execute as a launcher.")
static int Opcode_Sys_GetLauncherMode(Thread_t* t) // 80 FD: → v
{
	Thread_Push(t, (uint32_t)gLauncherMode);
	return 0;
}

// whether DirectSound 8 can be instantiated (the installer's sound check)
static int Opcode_Sys_CheckCom(Thread_t* t) // 80 FE: → f
{
	Thread_Push(t, (uint32_t)DSound8_IsAvailable());
	return 0;
}

// -------------------------------------------------------------------------
// the table
// -------------------------------------------------------------------------

/* Fill vm_optable_80 for the selected engine profile.  Each entry names an
 * opcode, its handler and the range of generations (version.h) it applies
 * to; where several entries cover an opcode the narrowest range wins, so
 * a variant for particular builds can sit next to the general entry.  An
 * opcode the profile defines but no entry covers gets the "not
 * implemented" stub, one it does not define stays empty (Vm_FillTable,
 * ops_base.c). */
void Vm_Optable80Init(void)
{
	static const OpEntry_t ops[] = {
		{0x00, Opcode_Sys_Srand, GEN_FIRST, GEN_LAST},
		{0x01, Opcode_Sys_Rand, GEN_FIRST, GEN_LAST},
		{0x02, Opcode_Sys_Random, GEN_FIRST, GEN_LAST},
		{0x04, Opcode_Sys_GetTicks, GEN_FIRST, GEN_LAST},
		{0x05, Opcode_Sys_PerfCounter, GEN_1_69_472, GEN_LAST},
		{0x08, Opcode_Sys_GetMousePos, GEN_FIRST, GEN_LAST},
		{0x0A, Opcode_Sys_GetSysinfo, GEN_FIRST, GEN_LAST},
		{0x0B, Opcode_Sys_GetGfxProp, GEN_FIRST, GEN_LAST},
		{0x0C, Opcode_Sys_GetLocaltime, GEN_FIRST, GEN_LAST},
		{0x0D, Opcode_Sys_GetMemstatus, GEN_FIRST, GEN_LAST},
		{0x0E, Opcode_Sys_GetMinimized, GEN_FIRST, GEN_LAST},
		{0x0F, Opcode_Sys_GetActive, GEN_FIRST, GEN_LAST},
		{0x10, Opcode_Sys_InputEnable, GEN_FIRST, GEN_LAST},
		{0x11, Opcode_Sys_KeyDown, GEN_FIRST, GEN_LAST},
		{0x12, Opcode_Sys_KeyCounterSum, GEN_FIRST, GEN_LAST},
		{0x13, Opcode_Sys_InputSerial, GEN_FIRST, GEN_LAST},
		{0x14, Opcode_Sys_SetSkipEnable, GEN_FIRST, GEN_LAST},
		{0x15, Opcode_Sys_SetSkipLatch, GEN_FIRST, GEN_LAST},
		{0x16, Opcode_Sys_InputFlush, GEN_FIRST, GEN_LAST},
		{0x17, Opcode_Sys_CheckSkip, GEN_FIRST, GEN_LAST},
		{0x18, Opcode_Sys_InputLayerPush, GEN_FIRST, GEN_LAST},
		{0x19, Opcode_Sys_InputLayerPop, GEN_FIRST, GEN_LAST},
		{0x1A, Opcode_Sys_InputPoll, GEN_FIRST, GEN_LAST},
		{0x1B, Opcode_Sys_SetStandardKey, GEN_FIRST, GEN_LAST},
		{0x1C, Opcode_Sys_CountKeys, GEN_FIRST, GEN_LAST},
		{0x1D, Opcode_Sys_KeyTrigger, GEN_FIRST, GEN_LAST},
		{0x1E, Opcode_Sys_SwapMouseButtons, GEN_FIRST, GEN_LAST},
		{0x1F, Opcode_Sys_MouseMove, GEN_FIRST, GEN_LAST},
		{0x20, Opcode_Sys_Galloc, GEN_FIRST, GEN_LAST},
		{0x21, Opcode_Sys_Gfree, GEN_FIRST, GEN_LAST},
		{0x24, Opcode_Sys_CountFiles, GEN_FIRST, GEN_LAST},
		{0x25, Opcode_Sys_ListFiles, GEN_FIRST, GEN_LAST},
		{0x28, Opcode_Sys_Mkdir, GEN_FIRST, GEN_LAST},
		{0x29, Opcode_Sys_Rmdir, GEN_FIRST, GEN_LAST},
		{0x2A, Opcode_Sys_IsDir, GEN_FIRST, GEN_LAST},
		{0x2C, Opcode_Sys_GetAttr, GEN_FIRST, GEN_LAST},
		{0x2D, Opcode_Sys_SetAttr, GEN_FIRST, GEN_LAST},
		{0x2F, Opcode_Sys_CopyFile, GEN_FIRST, GEN_LAST},
		{0x2B, Opcode_Sys_SplitPath, GEN_1_69_472, GEN_LAST},
		{0x30, Opcode_Sys_LoadFile, GEN_FIRST, GEN_LAST},
		{0x31, Opcode_Sys_LoadFileRange, GEN_FIRST, GEN_1_69_451},
		{0x31, Opcode_Sys_LoadFileRangeDecoded, GEN_1_69_472, GEN_LAST},
		{0x32, Opcode_Sys_WriteFile, GEN_FIRST, GEN_LAST},
		{0x33, Opcode_Sys_DeleteFile, GEN_FIRST, GEN_LAST},
		{0x34, Opcode_Sys_FileExists, GEN_FIRST, GEN_LAST},
		{0x35, Opcode_Sys_FileSize, GEN_FIRST, GEN_LAST},
		{0x36, Opcode_Sys_SetFileSearch, GEN_FIRST, GEN_LAST},
		{0x37, Opcode_Sys_PushSearchName, GEN_FIRST, GEN_LAST},
		{0x38, Opcode_Sys_RegisterFilelist, GEN_FIRST, GEN_LAST},
		{0x39, Opcode_Sys_SetAltDir, GEN_1_69_451, GEN_LAST},
		{0x3A, Opcode_Sys_GetSpecialFolder, GEN_FIRST, GEN_LAST},
		{0x3B, Opcode_Sys_FileDialog, GEN_FIRST, GEN_LAST},
		{0x3C, Opcode_Sys_PromptDisk, GEN_FIRST, GEN_LAST},
		{0x3D, Opcode_Sys_GetBasePath, GEN_FIRST, GEN_LAST},
		{0x3E, Opcode_Sys_SetBasePath, GEN_FIRST, GEN_LAST},
		{0x3F, Opcode_Sys_SetInstallPaths, GEN_FIRST, GEN_LAST},
		{0x40, Opcode_Sys_LoadProgram, GEN_FIRST, GEN_LAST},
		{0x41, Opcode_Sys_UnloadProgram, GEN_FIRST, GEN_LAST},
		{0x44, Opcode_Sys_SpawnThread, GEN_FIRST, GEN_LAST},
		{0x45, Opcode_Sys_ExitThread, GEN_FIRST, GEN_LAST},
		{0x46, Opcode_Sys_GetThreadId, GEN_FIRST, GEN_LAST},
		{0x47, Opcode_Sys_ThreadExists, GEN_FIRST, GEN_LAST},
		{0x48, Opcode_Sys_ThreadPost, GEN_FIRST, GEN_LAST},
		{0x49, Opcode_Sys_ThreadRecv, GEN_FIRST, GEN_LAST},
		{0x4A, Opcode_Sys_ThreadPostN, GEN_FIRST, GEN_LAST},
		{0x4B, Opcode_Sys_ThreadRecvN, GEN_FIRST, GEN_LAST},
		{0x4C, Opcode_Sys_ThreadNotify_Old, GEN_1_58, GEN_1_66},
		{0x4C, Opcode_Sys_ThreadNotify, GEN_1_69_444, GEN_LAST},
		{0x06, Opcode_Sys_ProfileEnable, GEN_1_529, GEN_LAST},
		{0x07, Opcode_Sys_ProfileQuery, GEN_1_529, GEN_LAST},
		{0x52, Opcode_Sys_SetIdleSleep, GEN_1_535, GEN_LAST},
		{0x53, Opcode_Sys_SeqDeferDecode, GEN_1_494, GEN_LAST},
		{0x50, Opcode_Sys_WaitSetBlocking, GEN_FIRST, GEN_LAST},
		{0x54, Opcode_Sys_WaitWinmsg, GEN_FIRST, GEN_LAST},
		{0x58, Opcode_Sys_TimerSet, GEN_FIRST, GEN_LAST},
		{0x59, Opcode_Sys_TimerAdd, GEN_FIRST, GEN_LAST},
		{0x5A, Opcode_Sys_TimerWait, GEN_FIRST, GEN_LAST},
		{0x5C, Opcode_Sys_WaitTimeKey, GEN_FIRST, GEN_LAST},
		{0x5D, Opcode_Sys_SetExclusive, GEN_FIRST, GEN_LAST},
		{0x5E, Opcode_Sys_SwitchThread, GEN_FIRST, GEN_LAST},
		{0x5F, Opcode_Sys_Yield, GEN_FIRST, GEN_LAST},
		{0x60, Opcode_Sys_SetDisplayMode, GEN_FIRST, GEN_LAST},
		{0x61, Opcode_Sys_IsFullscreen, GEN_FIRST, GEN_LAST},
		{0x62, Opcode_Sys_SetStyleKeys, GEN_FIRST, GEN_LAST},
		/* 1.58 had "80 63" minimise, "80 64" set title and "80 66" cursor
		 * shape; 1.64 inserted the window position mode and the show at 63 /
		 * 64 and moved the rest up by two ("80 65" .. "80 67") */
		{0x63, Opcode_Sys_Minimize_158, GEN_1_58, GEN_1_58},
		{0x63, Opcode_Sys_SetWindowPosMode, GEN_1_64, GEN_LAST},
		{0x64, Opcode_Sys_SetTitle, GEN_1_58, GEN_1_58},
		{0x64, Opcode_Sys_ShowWindow, GEN_1_64, GEN_LAST},
		{0x65, Opcode_Sys_Minimize, GEN_FIRST, GEN_1_69_451},
		{0x65, Opcode_Sys_Minimize_472, GEN_1_69_472, GEN_LAST},
		{0x66, Opcode_Sys_SetCursorShape, GEN_1_58, GEN_1_58},
		{0x66, Opcode_Sys_SetTitle, GEN_1_64, GEN_LAST},
		{0x67, Opcode_Sys_SetCursorShape, GEN_1_64, GEN_LAST},
		{0x68, Opcode_Sys_SetCloseMode, GEN_FIRST, GEN_LAST},
		{0x6E, Opcode_Sys_SetVsyncWait, GEN_1_69_451, GEN_LAST},
		{0x69, Opcode_Sys_PostClose, GEN_FIRST, GEN_LAST},
		{0x6A, Opcode_Sys_Quit, GEN_FIRST, GEN_LAST},
		{0x6B, Opcode_Sys_Reboot, GEN_FIRST, GEN_LAST},
		{0x6C, Opcode_Sys_DragdropEnable, GEN_FIRST, GEN_LAST},
		{0x6D, Opcode_Sys_GetDroppedFile, GEN_FIRST, GEN_LAST},
		{0x6F, Opcode_Sys_CanFullscreen, GEN_FIRST, GEN_LAST},
		{0x70, Opcode_Sys_SetGmemSize, GEN_FIRST, GEN_LAST},
		{0x71, Opcode_Sys_ClearGmem, GEN_FIRST, GEN_LAST},
		{0x74, Opcode_Sys_SetSaveScramble, GEN_FIRST, GEN_LAST},
		{0x78, Opcode_Sys_Save, GEN_FIRST, GEN_LAST},
		{0x79, Opcode_Sys_Load, GEN_FIRST, GEN_LAST},
		{0x7A, Opcode_Sys_SaveHeader, GEN_FIRST, GEN_LAST},
		{0x7B, Opcode_Sys_SaveCheck, GEN_FIRST, GEN_LAST},
		{0x80, Opcode_Sys_GdbLoad, GEN_FIRST, GEN_1_69_444},
		{0x80, Opcode_Sys_GdbLoad_472, GEN_1_69_451, GEN_LAST},
		{0x81, Opcode_Sys_GdbSave, GEN_FIRST, GEN_LAST},
		{0x82, Opcode_Sys_SysareaWrite, GEN_FIRST, GEN_LAST},
		{0x83, Opcode_Sys_SysareaRead, GEN_FIRST, GEN_LAST},
		{0x84, Opcode_Sys_GnameAdd, GEN_FIRST, GEN_LAST},
		{0x85, Opcode_Sys_GnameExists, GEN_FIRST, GEN_LAST},
		{0x88, Opcode_Sys_GflagRegister, GEN_FIRST, GEN_LAST},
		{0x89, Opcode_Sys_GflagSet, GEN_FIRST, GEN_LAST},
		{0x8A, Opcode_Sys_GflagSetN, GEN_FIRST, GEN_LAST},
		{0x8B, Opcode_Sys_GflagGet, GEN_FIRST, GEN_LAST},
		{0x90, Opcode_Sys_HistReset, GEN_FIRST, GEN_LAST},
		{0x91, Opcode_Sys_HistCount, GEN_FIRST, GEN_LAST},
		{0x94, Opcode_Sys_HistAdd, GEN_FIRST, GEN_LAST},
		{0x95, Opcode_Sys_HistGet, GEN_FIRST, GEN_LAST},
		{0x96, Opcode_Sys_HistAddStruct, GEN_FIRST, GEN_LAST},
		{0x97, Opcode_Sys_HistGetEx, GEN_FIRST, GEN_LAST},
		{0x98, Opcode_Sys_RingCreate, GEN_FIRST, GEN_LAST},
		{0x99, Opcode_Sys_RingDelete, GEN_FIRST, GEN_LAST},
		{0x9A, Opcode_Sys_RingCount, GEN_FIRST, GEN_LAST},
		{0x9C, Opcode_Sys_RingPush, GEN_FIRST, GEN_LAST},
		{0x9D, Opcode_Sys_RingGet, GEN_FIRST, GEN_LAST},
		{0x9E, Opcode_Sys_RingRemove, GEN_1_616, GEN_LAST},
		{0xA0, Opcode_Sys_MsgqGet, GEN_FIRST, GEN_LAST},
		{0xA1, Opcode_Sys_MsgqPost, GEN_FIRST, GEN_LAST},
		{0xA8, Opcode_Sys_SubobjSetEnabled, GEN_FIRST, GEN_LAST},
		{0xA9, Opcode_Sys_SubobjGetEnabled, GEN_FIRST, GEN_LAST},
		{0xAC, Opcode_Sys_SubobjPost, GEN_FIRST, GEN_LAST},
		{0xAF, Opcode_Sys_SetPanelPollMode, GEN_1_573, GEN_LAST},
		{0xB0, Opcode_Sys_SemCreate, GEN_FIRST, GEN_LAST},
		{0xB1, Opcode_Sys_SemDelete, GEN_FIRST, GEN_LAST},
		{0xB4, Opcode_Sys_SemWait, GEN_FIRST, GEN_LAST},
		{0xB5, Opcode_Sys_SemRelease, GEN_FIRST, GEN_LAST},
		{0xB6, Opcode_Sys_SemQuery, GEN_FIRST, GEN_LAST},
		{0xC0, Opcode_Sys_SdcEncodeAsync, GEN_FIRST, GEN_LAST},
		{0xC1, Opcode_Sys_SdcDecode, GEN_FIRST, GEN_LAST},
		{0xC4, Opcode_Sys_DcfsEncodeAsync, GEN_FIRST, GEN_LAST},
		{0xC5, Opcode_Sys_DcfsDecode, GEN_FIRST, GEN_LAST},
		{0xD0, Opcode_Sys_DictCreate, GEN_FIRST, GEN_LAST},
		{0xD1, Opcode_Sys_DictDelete, GEN_FIRST, GEN_LAST},
		{0xD2, Opcode_Sys_DictSet, GEN_FIRST, GEN_LAST},
		{0xD3, Opcode_Sys_DictRemove, GEN_FIRST, GEN_LAST},
		{0xD4, Opcode_Sys_DictGet, GEN_FIRST, GEN_LAST},
		{0xD8, Opcode_Sys_StrtabClear, GEN_FIRST, GEN_LAST},
		{0xD9, Opcode_Sys_StrtabCount, GEN_FIRST, GEN_LAST},
		{0xDC, Opcode_Sys_StrtabAdd, GEN_FIRST, GEN_LAST},
		{0xDC, Opcode_Sys_StrtabAddIdx, GEN_1_599, GEN_LAST},
		{0xDD, Opcode_Sys_StrtabGet, GEN_FIRST, GEN_LAST},
		{0xDA, Opcode_Sys_StrtabSetAll, GEN_1_599, GEN_LAST},
		{0xDB, Opcode_Sys_StrtabGather, GEN_1_599, GEN_LAST},
		{0xDE, Opcode_Sys_StrtabLength, GEN_1_599, GEN_LAST},
		{0xE0, Opcode_Sys_RunProgram, GEN_FIRST, GEN_LAST},
		{0xE1, Opcode_Sys_ExecOnExit, GEN_FIRST, GEN_LAST},
		{0xE2, Opcode_Sys_RunProgramWait, GEN_FIRST, GEN_LAST},
		{0xE3, Opcode_Sys_ShellOpen, GEN_FIRST, GEN_LAST},
		{0xE8, Opcode_Sys_GetProductName, GEN_FIRST, GEN_LAST},
		{0xE9, Opcode_Sys_HashFile, GEN_1_69_472, GEN_LAST},
		{0xEC, Opcode_Sys_CheckRegistration, GEN_FIRST, GEN_LAST},
		{0xED, Opcode_Sys_ReadComap, GEN_FIRST, GEN_LAST},
		{0xEE, Opcode_Sys_MachineKey, GEN_FIRST, GEN_LAST},
		{0xEF, Opcode_Sys_MachineKeyXor, GEN_FIRST, GEN_LAST},
		{0xF0, Opcode_Sys_InstallDialogA, GEN_FIRST, GEN_LAST},
		{0xF1, Opcode_Sys_InstallDialogB, GEN_FIRST, GEN_LAST},
		{0xF2, Opcode_Sys_InstallRun, GEN_FIRST, GEN_LAST},
		{0xF3, Opcode_Sys_CreateShortcuts, GEN_FIRST, GEN_LAST},
		{0xF4, Opcode_Sys_UninstProcess, GEN_FIRST, GEN_LAST},
		{0xF5, Opcode_Sys_UninstAppend, GEN_FIRST, GEN_LAST},
		{0xF6, Opcode_Sys_DeleteShortcuts, GEN_FIRST, GEN_LAST},
		{0xF7, Opcode_Sys_CreateShortcut, GEN_FIRST, GEN_LAST},
		{0xF8, Opcode_Sys_RegGetInstallDir, GEN_FIRST, GEN_LAST},
		{0xF9, Opcode_Sys_RegDeleteKey, GEN_FIRST, GEN_LAST},
		{0xFA, Opcode_Sys_ReadWindirFile, GEN_FIRST, GEN_LAST},
		{0xFB, Opcode_Sys_GetWindowsDir, GEN_FIRST, GEN_LAST},
		{0xFC, Opcode_Sys_RegisterAssoc, GEN_FIRST, GEN_LAST},
		{0xFD, Opcode_Sys_GetLauncherMode, GEN_FIRST, GEN_LAST},
		{0xFE, Opcode_Sys_CheckCom, GEN_FIRST, GEN_LAST},
	};
	Vm_FillTable(vm_optable_80, OPFAM_80, ops, BGI_COUNTOF(ops));
}
