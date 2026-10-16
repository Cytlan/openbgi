/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sys.c - the WebAssembly back end: start-up and shutdown, time, threads
 *         and locks, machine identity, processes, the registry emulation
 *         and the shell services
 *
 * The files and directories are in fs.c, the window in window.c.  What a
 * page has not got - other processes, shortcuts, the installer dialogs, a
 * second instance - reports failure the way the POSIX back end does; the
 * registry emulation keeps its text file in the overlay (fs.c), so it
 * persists with the save data.  The engine runs on the browser's main
 * thread alone, so the locks only count and the atomics are plain
 * arithmetic.
 *
 * The interface is os.h (the process, time, thread, memory and machine,
 * process, shell and registry sections).  The WasmJs_* functions are the
 * page side (EM_JS); tools/wasm/fakebrowser.c stands in for them on a host.
 */
#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/os_wasm.h"

#include <emscripten/emscripten.h>
#include <sys/stat.h>
#include <time.h>

static char gRegFile[0x400]; // the registry emulation (a file of the overlay, OsCommon_RegFile* format)

// ---- process ------------------------------------------------------------------------

// tell the page the engine has ended (Module.onEngineEnded, shell.html shows it in the status line)
// clang-format off
EM_JS(void, WasmJs_Ended, (int code), {
	if(Module['onEngineEnded'])
		Module['onEngineEnded'](code);
});
// clang-format on

/* connect to the asset server (0 when it cannot be reached: the engine does
 * not start), mount the overlay and load what an earlier visit saved, set up
 * the file layer and name the registry file */
int OS_Init(void)
{
	if(!WasmHost_Connect())
	{
		fprintf(stderr, "openbgi: the asset server cannot be reached\n");
		return 0;
	}
	WasmHost_OverlayMount(OsWasm_OverlayRoot());
	OsWasm_FsInit();
	snprintf(gRegFile, sizeof gRegFile, "%s/.openbgi/registry.txt", OsWasm_OverlayRoot());
	return 1;
}

// the end of the run: free the file layer, sync the overlay a last time, tell the page
void OS_Shutdown(void)
{
	OsWasm_FsShutdown();
	WasmHost_OverlayChanged(); // whatever the engine wrote last is synced
	WasmJs_Ended(0);
}

struct OsMutex
{
	int dummy; // nothing to hold: a page runs one engine
};

// always a fresh "mutex": a second instance cannot exist in the same page
OsMutex_t* OS_InstanceMutexCreate(const char* name, int* alreadyExists)
{
	BGI_UNUSED(name);
	*alreadyExists = 0; // one engine per page
	return (OsMutex_t*)calloc(1, sizeof(OsMutex_t));
}

void OS_InstanceMutexRelease(OsMutex_t* m)
{
	free(m);
}

int OS_NamedMutexExists(const char* name)
{
	BGI_UNUSED(name);
	return 0;
}

// ---- time ---------------------------------------------------------------------------

// performance.now() in whole milliseconds (emscripten_get_now)
uint32_t OS_TicksMs(void)
{
	return (uint32_t)emscripten_get_now();
}

// a sleep is a return to the browser for that long (emscripten_sleep through OsWasm_Yield)
void OS_SleepMs(uint32_t ms)
{
	OsWasm_Yield((int)(ms > 0x7fffffffu ? 0x7fffffffu : ms));
}

// no timer resolution to set in a browser
void OS_TimerBeginPeriod(void)
{
}

void OS_TimerEndPeriod(void)
{
}

// the browser's local time (Emscripten's localtime follows the page's time zone)
void OS_LocalTime(OsLocalTime_t* t)
{
	struct timespec ts;
	struct tm tm;
	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	t->year = (uint16_t)(tm.tm_year + 1900);
	t->month = (uint16_t)(tm.tm_mon + 1);
	t->dayOfWeek = (uint16_t)tm.tm_wday;
	t->day = (uint16_t)tm.tm_mday;
	t->hour = (uint16_t)tm.tm_hour;
	t->minute = (uint16_t)tm.tm_min;
	t->second = (uint16_t)tm.tm_sec;
	t->millis = (uint16_t)(ts.tv_nsec / 1000000);
}

// the idle sleep needs nothing created (the MessageChannel of OsWasm_Yield is made on first use)
void OS_IdleInit(void)
{
}

void OS_IdleShutdown(void)
{
}

/* The idle step of every pass is where the engine returns to the browser:
 * a zero duration takes the fast path of OsWasm_Yield (a message to itself),
 * which lets the page paint and the websocket deliver and comes back within
 * a few milliseconds, like the original's half-millisecond timer did on a
 * slow machine. */
void OS_IdleSleep(void)
{
	OsWasm_Yield(0);
}

/* the 1.535+ variant (os.h): 1 ms is the same fast yield as OS_IdleSleep,
 * any other duration a timed sleep capped at 10 ms as the original caps its
 * timer wait; 0 or less returns at once */
void OS_IdleSleepMs(int ms)
{
	if(ms <= 0)
		return;
	OsWasm_Yield(ms > 10 ? 10 : ms == 1 ? 0
										: ms);
}

// ---- threads and locks: one thread, no contention ---------------------------------------------

struct OsLock
{
	int depth; // how often it was entered without being left (never waited on)
};

OsLock_t* OS_LockCreate(void)
{
	return (OsLock_t*)calloc(1, sizeof(OsLock_t));
}

void OS_LockDestroy(OsLock_t* l)
{
	free(l);
}

// a recursive lock on one thread: enter only counts (NULL is accepted)
void OS_LockEnter(OsLock_t* l)
{
	if(l)
		l->depth++;
}

void OS_LockLeave(OsLock_t* l)
{
	if(l && l->depth > 0)
		l->depth--;
}

struct OsThread
{
	int done; // 1: the function has returned (always, by the time the caller sees the handle)
};

/* Nothing in the engine starts a thread (the loader runs its jobs inline,
 * the mixer is pumped from the main loop); should one appear, its function
 * runs to completion here and now, and the handle is given back to
 * OS_ThreadJoin as usual.  NULL only when out of memory. */
OsThread_t* OS_ThreadStart(OsThreadFn_t fn, void* arg, int priority)
{
	OsThread_t* t = (OsThread_t*)calloc(1, sizeof *t);
	BGI_UNUSED(priority);
	if(!t)
		return NULL;
	fn(arg);
	t->done = 1;
	return t;
}

// the function has run already: only the handle is freed (NULL accepted)
void OS_ThreadJoin(OsThread_t* t)
{
	free(t);
}

int OS_CpuCount(void)
{
	return 1;
}

// plain increments: there is no second thread to race with
int32_t OS_AtomicInc(volatile int32_t* p)
{
	return ++*p;
}

int32_t OS_AtomicDec(volatile int32_t* p)
{
	return --*p;
}

// ---- machine ------------------------------------------------------------------------------

// navigator.deviceMemory (GB, rounded by the browser) in bytes; 4 GB when the browser does not say
// clang-format off
EM_JS(double, WasmJs_DeviceMemoryBytes, (void), {
	var gb = (typeof navigator !== 'undefined' && navigator.deviceMemory) ? navigator.deviceMemory : 4;
	return gb * 1024 * 1024 * 1024;
});
// clang-format on

/* GlobalMemoryStatus from what the browser tells: the device memory as the
 * physical total (capped at 4 GB), half of it as available, no page file,
 * the 2 GB user address space of 32-bit Windows as virtual, 50 % load */
void OS_MemoryStatus(OsMemStatus_t* m)
{
	double bytes = WasmJs_DeviceMemoryBytes();
	memset(m, 0, sizeof *m);
	m->totalPhys = bytes > 4294967295.0 ? 0xffffffffu : (uint32_t)bytes;
	m->availPhys = m->totalPhys / 2;
	m->totalPageFile = m->availPageFile = 0;
	m->totalVirtual = 0x7ffe0000u;
	m->availVirtual = m->totalVirtual;
	m->loadPercent = 50;
}

// "81 0D": the device memory in bytes (64 bit, uncapped), half of it available
void OS_MemoryStatusEx(uint64_t* totalPhys, uint64_t* availPhys)
{
	double bytes = WasmJs_DeviceMemoryBytes();
	*totalPhys = (uint64_t)bytes;
	*availPhys = *totalPhys / 2;
}

// fixed names: a page knows neither; the installer's machine key is the same for every visitor
int OS_ComputerName(char* buf, size_t n)
{
	snprintf(buf, n, "browser");
	return 1;
}

int OS_UserName(char* buf, size_t n)
{
	snprintf(buf, n, "user");
	return 1;
}

/* No CPUID in WebAssembly: the engine's start-up skips its processor checks
 * on this platform (Engine_Main, BGI_WASM) and everything else that asks
 * takes its "unknown processor" path. */
int OS_Cpuid(uint32_t leaf, uint32_t out[4])
{
	BGI_UNUSED(leaf);
	BGI_UNUSED(out);
	return 0;
}

uint64_t OS_ReadTsc(void)
{
	return 0;
}

// "80 05": performance.now() as nanoseconds; always available
int OS_PerfCounterNs(int64_t* out)
{
	*out = (int64_t)(emscripten_get_now() * 1000000.0);
	return 1;
}

uint32_t OS_CpuSignature(void)
{
	return 0;
}

// reports Windows XP (NT 5.1): the engine takes the NT paths for platform 2
int OS_UiLanguage(void)
{
	return 0x411; // Japanese: the games' own language
}

int OS_Version(int* platform, int* major, int* minor)
{
	*platform = 2; // VER_PLATFORM_WIN32_NT: the engine takes the NT paths
	*major = 5;
	*minor = 1;
	return 1;
}

// "81 0C": the same as XP build 2600 with Service Pack 3
void OS_VersionEx(uint32_t out[4], char* csd, size_t n)
{
	int platform, major, minor;
	OS_Version(&platform, &major, &minor);
	out[0] = (uint32_t)major;
	out[1] = (uint32_t)minor;
	out[2] = 2600;
	out[3] = (uint32_t)platform;
	if(n)
		snprintf(csd, n, "Service Pack 3");
}

// "81 0B": a fixed description and a zero driver version
int OS_DisplayAdapter(char* desc, size_t n, uint32_t version[4])
{
	if(n)
		snprintf(desc, n, "HTML canvas");
	version[0] = version[1] = version[2] = version[3] = 0;
	return 1;
}

// ---- processes and the shell: not on a page ----------------------------------------------------

struct OsProcess
{
	int dummy; // never allocated: OS_ProcessStart always fails
};

OsProcess_t* OS_ProcessStart(const char* cmdLine)
{
	BGI_UNUSED(cmdLine);
	return NULL;
}

void OS_ProcessWaitIdle(OsProcess_t* p)
{
	BGI_UNUSED(p);
}

// "ended" at once: no process ever runs
int OS_ProcessWait(OsProcess_t* p, uint32_t ms)
{
	BGI_UNUSED(p);
	BGI_UNUSED(ms);
	return 1;
}

void OS_ProcessClose(OsProcess_t* p)
{
	BGI_UNUSED(p);
}

// open an http:// or https:// address in a new tab (window.open; the browser may block a pop-up)
// clang-format off
EM_JS(void, WasmJs_OpenUrl, (const char* utf8), {
	var s = UTF8ToString(utf8);
	var l = s.toLowerCase();
	if(l.indexOf('http://') === 0 || l.indexOf('https://') === 0)
		window.open(s, '_blank');
});
// clang-format on

/* ShellExecute "open": an address starting with "http://" or "https://"
 * (lower case) opens in a new tab (1); anything else cannot (0) */
int OS_ShellExecute(const char* file, const char* params, const char* dir)
{
	char utf[0x1000];
	BGI_UNUSED(params);
	BGI_UNUSED(dir);
	OsWasm_SjisToUtf8(file, utf, sizeof utf);
	if(strncmp(utf, "http://", 7) != 0 && strncmp(utf, "https://", 8) != 0)
		return 0;
	WasmJs_OpenUrl(utf);
	return 1;
}

int OS_SetWallpaper(const char* path, int stretch, int tile)
{
	BGI_UNUSED(path);
	BGI_UNUSED(stretch);
	BGI_UNUSED(tile);
	return 0;
}

int OS_CreateShortcut(const char* linkPath, const char* target)
{
	BGI_UNUSED(linkPath);
	BGI_UNUSED(target);
	return 0;
}

int OS_CreateShortcutArgs(const char* linkPath, const char* target, const char* args)
{
	BGI_UNUSED(linkPath);
	BGI_UNUSED(target);
	BGI_UNUSED(args);
	return 0;
}

// ---- registry emulation (a text file in the overlay) ------------------------------------------

// the OsCommon_RegFile* functions do the work; every write schedules a sync of the overlay

int OS_RegReadString(const char* key, const char* value, char* buf, size_t n)
{
	return OsCommon_RegFileRead(gRegFile, key, value, buf, n);
}

// the class is not kept; the key is created with an empty default value
int OS_RegCreateKey(const char* key, const char* cls)
{
	int r;
	BGI_UNUSED(cls);
	r = OsCommon_RegFileWrite(gRegFile, key, NULL, "");
	WasmHost_OverlayChanged();
	return r;
}

int OS_RegWriteString(const char* key, const char* value, const char* s)
{
	int r = OsCommon_RegFileWrite(gRegFile, key, value, s);
	WasmHost_OverlayChanged();
	return r;
}

int OS_RegDeleteKey(const char* key)
{
	int r = OsCommon_RegFileDeleteKey(gRegFile, key);
	WasmHost_OverlayChanged();
	return r;
}

void OS_ShellNotifyAssocChanged(void)
{
}

/* the special folders are directories of the overlay: the engine sees them
 * as "C:\.openbgi\..." and fs.c keeps them in the browser (they are made
 * when something is created in them); always 1 */
int OS_SpecialFolder(int csidl, char* buf, size_t n)
{
	switch(csidl)
	{
		case 0x10: snprintf(buf, n, "C:\\.openbgi\\desktop"); break; // CSIDL_DESKTOPDIRECTORY
		case 2: snprintf(buf, n, "C:\\.openbgi\\programs"); break;   // CSIDL_PROGRAMS
		case 5: snprintf(buf, n, "C:\\.openbgi\\documents"); break;  // CSIDL_PERSONAL
		default: snprintf(buf, n, "C:\\.openbgi"); break;
	}
	return 1;
}

// the "Windows directory" is the back end's own directory of the overlay
int OS_WindowsDir(char* buf, size_t n)
{
	snprintf(buf, n, "C:\\.openbgi");
	return 1;
}

// no installer dialogs on a page: 0, the "cancelled" result of every dialog
int OS_InstallerDialog(int id, void* ctx)
{
	BGI_UNUSED(id);
	BGI_UNUSED(ctx);
	return 0;
}

int OS_IpcSendToRunning(const char* windowClass, const char* data)
{
	BGI_UNUSED(windowClass);
	BGI_UNUSED(data);
	return 0;
}
