/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ops_sys81.c - the "81 xx" second system-control instruction family
 *
 * The dispatcher appeared in 1.69 build 472 (TayutamaHD) with six opcodes;
 * the family filled up from 1.529 on (docs/dev/versions.md).  The conventions
 * are those of ops_sys.c: the trailing comment of a handler gives its stack
 * effect, the inputs in the order the script pushed them (the last one on
 * top), then the outputs; every handler returns scheduler code 0.  The
 * interface (o->vm.optable81, Vm_Optable81Init) is declared in vm.h.
 *
 * Several instructions of this family drove Windows-only features of the
 * original (WinInet, Direct3D shaders, touch input, the window border and
 * placement).  Where the host has no counterpart the handler only
 * stores the setting or answers "not available", as noted on each.
 */
#include "bgi/vm.h"
#include "bgi/error.h"
#include "bgi/waitobj.h"
#include "bgi/request.h"
#include "bgi/sys.h"
#include "bgi/msg.h"
#include "bgi/input.h"
#include "bgi/display.h"
#include "bgi/file.h"
#include "bgi/install.h"
#include "bgi/strutil.h"
#include "bgi/sysobj.h"
#include "bgi/sysobj/database.h"
#include "bgi/sjis.h"
#include "bgi/host.h"
#include "bgi/core.h"

// whether the auto-repeat of virtual key vk counts as presses; pushes the previous setting
static int Opcode_Sys81_KeyRepeatNew(OpenBGI_t* o, Thread_t* t) // 81 10: vk, on → old
{
	int on = (int)Thread_Pop(t);
	int vk = (int)Thread_Pop(t);
	Thread_Push(t, (uint32_t)Key_SetRepeatNew(o, vk, on));
	return 0;
}

// the state of all 256 virtual keys into buf (GetKeyboardState: bit 7 of a byte = down)
static int Opcode_Sys81_KeyboardState(OpenBGI_t* o, Thread_t* t) // 81 11: buf →
{
	Input_KeyboardState(o, (uint8_t*)PopPtr(o, t));
	return 0;
}

// the standard keys (bits of the "80 1A" mask) that break a tween
static int Opcode_Sys81_SetTweenKeyMask(OpenBGI_t* o, Thread_t* t) // 81 1F: mask →
{
	Input_SetTweenKeyMask(o, Thread_Pop(t));
	return 0;
}

/* the folder browser (SHBrowseForFolder returning file-system folders
 * only and not descending below the domain level, `initial` selected at
 * the start); the chosen path into out (0x104 bytes), ok 1 when one was
 * chosen.  A NULL title shows the default prompt MSG_SELECT_FOLDER
 * ("フォルダを選択してください", "Please select a folder"). */
static int Opcode_Sys81_BrowseFolder(OpenBGI_t* o, Thread_t* t) // 81 3A: out, title, initial → ok
{
	OpenBGI_Request_t q;
	const char* initial = (const char*)PopPtr(o, t);
	const char* title = (const char*)PopPtr(o, t);
	char* out = (char*)PopPtr(o, t);
	memset(&q, 0, sizeof q);
	q.kind = OPENBGI_ASK_FOLDER;
	q.u.folder.path = out;
	q.u.folder.pathSize = 0x104;
	q.u.folder.title = title ? title : MSG_SELECT_FOLDER;
	q.u.folder.initial = initial;
	return WaitAsk_Begin(o, WaitAsk_New(o, t, sizeof(WaitAsk_t), "WaitFolder", NULL, NULL), Request_Post(o, &q, REQUEST_TREAT_NONE));
}

/* keep the window at its computed position: in the original the
 * WM_WINDOWPOSCHANGING handler re-imposes it while the flag is set.  Only
 * stored here, window placement being the host's. */
static int Opcode_Sys81_LockWindowPos(OpenBGI_t* o, Thread_t* t) // 81 65: on →
{
	o->ops.windowPosLock = (int)Thread_Pop(t);
	return 0;
}

// ---- the 1.529 family -----------------------------------------------------------------

// the processor brand string (0x40 bytes, runs of spaces collapsed) into out; ok 1 when the CPUID leaves exist
static int Opcode_Sys81_CpuBrand(OpenBGI_t* o, Thread_t* t) // 81 0A: out → ok
{
	char* out = (char*)PopPtr(o, t);
	Thread_Push(t, (uint32_t)Cpu_BrandString(o, out));
	return 0;
}

/* the display adapter's description (runs of spaces collapsed, the ends
 * trimmed) into desc and its driver version as four words, most
 * significant first, into ver[0 .. 3]; the top operand is popped and
 * ignored */
static int Opcode_Sys81_DisplayAdapter(OpenBGI_t* o, Thread_t* t) // 81 0B: desc, ver, x →
{
	uint32_t* ver;
	char* desc;
	char raw[0x200];
	const char* p;
	char* d;
	PopPtr(o, t);
	ver = (uint32_t*)PopPtr(o, t);
	desc = (char*)PopPtr(o, t);
	Host_DisplayAdapter(o, raw, sizeof raw, ver);
	for(p = raw; *p == ' '; p++)
		;
	for(d = desc; *p; p++)
		if(*p != ' ' || p[1] != ' ')
			*d++ = *p;
	if(d > desc && d[-1] == ' ')
		d--;
	*d = 0;
	return 0;
}

/* the OS version (GetVersionEx): major, minor, build and platform into
 * ver[0 .. 3], the service pack string (up to 0x80 bytes) into csd */
static int Opcode_Sys81_OsVersion(OpenBGI_t* o, Thread_t* t) // 81 0C: ver, csd →
{
	char* csd = (char*)PopPtr(o, t);
	uint32_t* ver = (uint32_t*)PopPtr(o, t);
	Host_Version(o, ver, csd, 0x80);
	return 0;
}

// the physical memory in MB (from the 64-bit figures of GlobalMemoryStatusEx) into *total and *avail
static int Opcode_Sys81_MemoryMB(OpenBGI_t* o, Thread_t* t) // 81 0D: total, avail →
{
	uint32_t* avail = (uint32_t*)PopPtr(o, t);
	uint32_t* total = (uint32_t*)PopPtr(o, t);
	uint64_t tp, ap;
	Host_MemoryStatusEx(o, &tp, &ap);
	*total = (uint32_t)(tp >> 20);
	*avail = (uint32_t)(ap >> 20);
	return 0;
}

// the desktop size into out[0 .. 1], adjusted for multi-monitor layouts (Display_GetDesktopSize)
static int Opcode_Sys81_DesktopSize(OpenBGI_t* o, Thread_t* t) // 81 0E: out →
{
	int32_t* out = (int32_t*)PopPtr(o, t);
	int v[2];
	Display_GetDesktopSize(o, v, 1);
	out[0] = v[0];
	out[1] = v[1];
	return 0;
}

// whether the window is minimised (IsIconic)
static int Opcode_Sys81_IsIconic(OpenBGI_t* o, Thread_t* t) // 81 0F: → f
{
	Thread_Push(t, (uint32_t)MainWin_Minimized(o));
	return 0;
}

// whether key states are read while the window is inactive
static int Opcode_Sys81_KeysWhenInactive(OpenBGI_t* o, Thread_t* t) // 81 14: on →
{
	Input_SetKeysWhenInactive(o, (int)Thread_Pop(t));
	return 0;
}

// a click of a mouse button at the cursor: kind 1 left, 2 right, 4 middle, 5 / 6 the X buttons; ok 1 when sent
static int Opcode_Sys81_SimulateClick(OpenBGI_t* o, Thread_t* t) // 81 1E: kind → ok
{
	Thread_Push(t, (uint32_t)Window_SimulateClick(o, (int)Thread_Pop(t)));
	return 0;
}

/* Can a file be created in the directory `dir`?  The original makes a
 * temporary file there (GetTempFileName with the prefix "BGI"), writes
 * to it and deletes it again, impersonating the logged-on user first
 * when it runs with another token; 1 when all of that worked, 0 when
 * not.  The scripts ask it about their user-data folder. */
static int Opcode_Sys81_DirWritable(OpenBGI_t* o, Thread_t* t) // 81 2F (1.616 on): dir → ok
{
	const char* dir = (const char*)PopPtr(o, t);
	char path[0x210];
	char name[32];
	size_t n = strlen(dir);
	FsFile_t* f;
	int ok = 0;
	snprintf(name, sizeof name, "BGI%04X.tmp", (unsigned)(Clock_HostMs(o) & 0xffffu));
	if(n + 1 + strlen(name) >= sizeof path)
	{
		Thread_Push(t, 0);
		return 0;
	}
	snprintf(path, sizeof path, "%s%s%s", dir, n && (dir[n - 1] == '\\' || dir[n - 1] == '/') ? "" : "\\", name);
	f = Fs_FileCreate(o, path);
	if(f)
	{
		ok = Fs_FileWrite(f, "BGI", 3) == 3;
		Fs_FileClose(f);
		Fs_FileDelete(o, path);
	}
	Thread_Push(t, (uint32_t)ok);
	return 0;
}

/* `size` bytes from offset `off` of the raw (stored) file into buf - what
 * "80 31" did before 1.69 build 472 made it the decoded range; 0 ok, 1
 * not found, 2 / 3 range errors, -1 short read */
static int Opcode_Sys81_LoadFileRangeRaw(OpenBGI_t* o, Thread_t* t) // 81 30: buf, arc, name, off, size → code
{
	uint32_t size = Thread_Pop(t), off = Thread_Pop(t);
	const char* name = (const char*)PopPtr(o, t);
	const char* arc = (const char*)PopPtr(o, t);
	void* buf = PopPtr(o, t);
	int r = LoadFileRange(o, buf, arc, name, off, size);
	Vm_FollowRange(o, buf, arc, name, off, size, r);
	Thread_Push(t, (uint32_t)r);
	return 0;
}

/* a range of a web resource, fetched through WinInet in the original (0
 * ok, 1 no connection or open failure, 2 read failure, 3 short read).
 * There is no network access here: the operands are dropped and 1 is
 * pushed. */
static int Opcode_Sys81_HttpGet(OpenBGI_t* o, Thread_t* t) // 81 31: buf, url, off, size → code
{
	Thread_Pop(t);
	Thread_Pop(t);
	PopPtr(o, t);
	PopPtr(o, t);
	Thread_Push(t, 1);
	return 0;
}

/* 1.662 on: the UI language the scripts ask the engine about, a Windows
 * LANGID (the primary language in the low 10 bits): "81 02" sets it - 0
 * takes the user's own from the OS - and returns it, "81 03" returns it
 * (0 until "81 02" ran, as in the original).  The boot programs of the
 * builds with other languages (Haruka Ao no Hanayome ni) pick their text
 * by it. */
static int Opcode_Sys81_SetUiLanguage(OpenBGI_t* o, Thread_t* t) // 81 02 (1.662 on): lang → lang
{
	int32_t lang = (int32_t)Thread_Pop(t);
	o->ops.uiLanguage = lang ? lang : Host_UiLanguage(o);
	Thread_Push(t, (uint32_t)o->ops.uiLanguage);
	return 0;
}

static int Opcode_Sys81_GetUiLanguage(OpenBGI_t* o, Thread_t* t) // 81 03 (1.662 on): → lang
{
	Thread_Push(t, (uint32_t)o->ops.uiLanguage);
	return 0;
}

/* the "choose an entry of an archive" dialog of the development builds (a
 * resource dialog listing the archive's names, the chosen one copied
 * out).  Not provided: the four pointer operands are dropped and r is -1
 * (cancelled). */
static int Opcode_Sys81_ArcEntryDialog(OpenBGI_t* o, Thread_t* t) // 81 3B: p1, p2, p3, p4 → r
{
	PopPtr(o, t);
	PopPtr(o, t);
	PopPtr(o, t);
	PopPtr(o, t);
	Thread_Push(t, 0xffffffffu);
	return 0;
}

// how the picture fits the full screen: mode 0 stretched, 1 keeping the aspect, 2 centred; ok 1 for 0 .. 2 (stored only)
static int Opcode_Sys81_SetFullscreenFit(OpenBGI_t* o, Thread_t* t) // 81 63: mode → ok
{
	Thread_Push(t, (uint32_t)Display_SetFullscreenFit(o, (int)Thread_Pop(t)));
	return 0;
}

// 1.669 on: the size the renderer scales the picture to; ok 1 when its aspect is the desktop's (stored only)
static int Opcode_Sys81_SetRenderSize(OpenBGI_t* o, Thread_t* t) // 81 6C (1.669 on): w, h → ok
{
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	Thread_Push(t, (uint32_t)Display_SetRenderSize(o, w, h));
	return 0;
}

// the window's client size in pixels (0, 0: the picture size); "81 67" reads it back
static int Opcode_Sys81_SetWindowSize(OpenBGI_t* o, Thread_t* t) // 81 64: w, h →
{
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	Display_SetWindowSize(o, w, h);
	return 0;
}

/* convert the text at src into dst: mode 0 to Shift-JIS, 1 to UTF-8,
 * anything else to the current script text encoding ("81 00").  The
 * source's encoding is judged from its bytes (TextSniff, strutil.h):
 * plain ASCII and text already in the target encoding are copied as they
 * are, a conversion is bounded to 0x10000 bytes of dst.  Always pushes 1. */
static int Opcode_Sys81_ConvertText(OpenBGI_t* o, Thread_t* t) // 81 20 (1.653 on): dst, src, mode → 1
{
	uint32_t mode = Thread_Pop(t);
	const char* src = (const char*)PopPtr(o, t);
	char* dst = (char*)PopPtr(o, t);
	uint32_t target = mode == 0 ? TEXT_SJIS : mode == 1 ? TEXT_UTF8
														: (uint32_t)o->enc.textUtf8;
	uint32_t have = TextSniff(src);
	if(have == TEXT_RAW || have == target)
	{
		if(dst != src) // a script may convert in place
			memmove(dst, src, strlen(src) + 1);
	}
	else if(target == TEXT_UTF8)
		Sjis_ToUtf8(src, dst, 0x10000, o->enc.textUtf8);
	else
		Sjis_FromUtf8(src, dst, 0x10000);
	Thread_Push(t, 1);
	return 0;
}

// Shift-JIS at src to UTF-8 at dst (0x10000 bytes), without sniffing
static int Opcode_Sys81_SjisToUtf8(OpenBGI_t* o, Thread_t* t) // 81 21 (1.653 on): dst, src →
{
	const char* src = (const char*)PopPtr(o, t);
	char* dst = (char*)PopPtr(o, t);
	Sjis_ToUtf8(src, dst, 0x10000, o->enc.textUtf8);
	return 0;
}

// the size of a file without reading it (GetFileSizeFast), unlike "80 35"
static int Opcode_Sys81_FileSizeFast(OpenBGI_t* o, Thread_t* t) // 81 35 (1.588 on): arc, name → size
{
	const char* name = (const char*)PopPtr(o, t);
	const char* arc = (const char*)PopPtr(o, t);
	Thread_PushMarked(t, GetFileSizeFast(o, arc, name), (uint8_t)Program_Is(o, arc, name));
	return 0;
}

/* load a block of named string groups (strgroups.h; `size` bytes at data)
 * on top of those already known; data NULL drops them all.  ok 1 when the
 * block was consumed exactly. */
static int Opcode_Sys81_StrGroupsLoad(OpenBGI_t* o, Thread_t* t) // 81 D8 (1.653 on): data, size → ok
{
	uint32_t size = Thread_Pop(t);
	const uint8_t* data = (const uint8_t*)PopPtr(o, t);
	Thread_Push(t, (uint32_t)StrGroups_Load(o, data, size));
	return 0;
}

/* the string stored under `key` in `group` into out (when out is not
 * NULL) and its length; 0 when there is none */
static int Opcode_Sys81_StrGroupsLookup(OpenBGI_t* o, Thread_t* t) // 81 DA (1.653 on): out, group, key → len
{
	const char* key = (const char*)PopPtr(o, t);
	const char* group = (const char*)PopPtr(o, t);
	char* out = (char*)PopPtr(o, t);
	const char* v = StrGroups_Lookup(o, group, key);
	if(v && out)
		strcpy(out, v);
	Thread_Push(t, v ? (uint32_t)strlen(v) : 0u);
	return 0;
}

/* whether the main window gets a sizing border (WS_THICKFRAME in the
 * original, which picks the window style by this flag): the host's
 * window gets one where it has windows of that kind */
static int Opcode_Sys81_SetResizable(OpenBGI_t* o, Thread_t* t) // 81 66 (1.669 on): on →
{
	Display_SetResizable(o, (int)Thread_Pop(t));
	return 0;
}

/* the window's client size - what the player made it with the sizing
 * border, or "81 64" set, else the picture size - into out[0 .. 1] */
static int Opcode_Sys81_GetWindowSize(OpenBGI_t* o, Thread_t* t) // 81 67 (1.669 on): out →
{
	int32_t* out = (int32_t*)PopPtr(o, t);
	int w = 0, h = 0;
	Display_GetWindowSize(o, &w, &h);
	out[0] = w;
	out[1] = h;
	return 0;
}

/* Pixel shaders: from 1.616 on the original's Direct3D presentation can
 * run a shader effect (its "SHADER" resource, compiled through d3dx9_43):
 * a bicubic scaler for a picture stretched into a window of another size.
 * "81 6D" tells whether shader support was found at start-up, "81 6E"
 * whether d3dx9 could be loaded, "81 6F" loads (on 1) or releases (on 0)
 * the effect and pushes 1 when that worked.  Here the host scales the
 * picture: with one that scales by the rule it is told the answers are
 * yes and the effect is its bicubic rule (Display_SetShader); with any
 * other there is no support, no library, and only switching the effect
 * off succeeds. */
static int Opcode_Sys81_ShaderSupport(OpenBGI_t* o, Thread_t* t) // 81 6D (1.616 on): → f
{
	Thread_Push(t, (uint32_t)Display_ShaderSupport(o));
	return 0;
}

static int Opcode_Sys81_ShaderLibrary(OpenBGI_t* o, Thread_t* t) // 81 6E (1.616 on): → f
{
	Thread_Push(t, (uint32_t)Display_ShaderSupport(o));
	return 0;
}

static int Opcode_Sys81_ShaderEnable(OpenBGI_t* o, Thread_t* t) // 81 6F (1.616 on): on → ok
{
	uint32_t on = Thread_Pop(t);
	Thread_Push(t, (uint32_t)Display_SetShader(o, on != 0));
	return 0;
}

/* Touch input: from 1.599 on the original registers its window for touch
 * messages through user32's RegisterTouchWindow / GetTouchInputInfo,
 * loaded at run time.  "81 18" registers (on 1, palm touches included) or
 * unregisters the window and pushes the API's result, 0 without the API;
 * "81 19" copies the touch points the window procedure collected (six
 * values each) into buf and pushes their number.  There is no touch input
 * here: 0 and no points. */
static int Opcode_Sys81_TouchEnable(OpenBGI_t* o, Thread_t* t) // 81 18 (1.599 on): on → ok
{
	Thread_Pop(t);
	Thread_Push(t, 0);
	return 0;
}

static int Opcode_Sys81_TouchPoints(OpenBGI_t* o, Thread_t* t) // 81 19 (1.599 on): buf → n
{
	PopPtr(o, t);
	Thread_Push(t, 0);
	return 0;
}

/* The tracking of the touch points "81 19" reports: the window procedure
 * keeps up to `count` (at most 0x200) points and starts a new one when a
 * touch moved farther than `distance` pixels from where it was; "81 17"
 * copies the tracked points from index `first` on: their positions (two
 * values each) into xy, the distance to the previous one (-1 for the
 * first) into dist, at most `count`, and pushes how many it copied.  ok 1,
 * 0 for a count above 0x200.  Stored only: there is no touch input here,
 * so nothing is ever tracked. */
static int Opcode_Sys81_TouchTracking(OpenBGI_t* o, Thread_t* t) // 81 16 (1.616 on): count, distance → ok
{
	uint32_t distance = Thread_Pop(t);
	uint32_t count = Thread_Pop(t);
	if(count > 0x200)
	{
		Thread_Push(t, 0);
		return 0;
	}
	o->ops.touchMaxPoints = count;
	o->ops.touchDistance = distance;
	Thread_Push(t, 1);
	return 0;
}

static int Opcode_Sys81_TouchHistory(OpenBGI_t* o, Thread_t* t) // 81 17 (1.616 on): xy, dist, first, count → n
{
	Thread_Pop(t); // count
	Thread_Pop(t); // first
	PopPtr(o, t);  // dist
	PopPtr(o, t);  // xy
	Thread_Push(t, 0);
	return 0;
}

/* 1.573 on: on 1 the original takes the Print Screen key for itself
 * (RegisterHotKey of VK_SNAPSHOT with each of the 16 modifier
 * combinations), so that the system's screenshot no longer sees it; 0
 * gives it back.  Stored only: the OS
 * layer here registers no global keys. */
static int Opcode_Sys81_TakePrintScreen(OpenBGI_t* o, Thread_t* t) // 81 69 (1.573 on): on →
{
	o->ops.printScreenTaken = Thread_Pop(t) != 0;
	return 0;
}

/* the archive file `name` (base directory, then the alternative one) has
 * its index read again on its next use: 0 found, 1 not (ReloadArchive) */
static int Opcode_Sys81_ReloadArchive(OpenBGI_t* o, Thread_t* t) // 81 3C (1.573 on): name → r
{
	const char* name = (const char*)PopPtr(o, t);
	Thread_Push(t, (uint32_t)ReloadArchive(o, name));
	return 0;
}

/* start a thread that runs at code offset ip in parts of this thread's
 * areas (of its lender's, when it runs in borrowed ones): codeSize bytes
 * cut from the top of the code area for the modules it loads itself,
 * dataSize bytes from the top of the data area for its frames, the heap
 * shared, and a stack of stackEntries values of its own
 * (Thread_SpawnBorrower).  It ends at the latest when the lender does.
 * Parts that do not fit, or an ip at or past the end of the new thread's
 * part of the code area, are script errors (the latter with the thread
 * already started, as in the original). */
static int Opcode_Sys81_SpawnBorrower(OpenBGI_t* o, Thread_t* t) // 81 44 (1.599 on): stackEntries, codeSize, dataSize, ip → id
{
	uint32_t ip = Thread_Pop(t);
	uint32_t dataSize = Thread_Pop(t);
	uint32_t codeSize = Thread_Pop(t);
	uint32_t stackEntries = Thread_Pop(t);
	char msg[0x104];
	Thread_t* c = NULL;
	switch(Thread_SpawnBorrower(o, t, stackEntries, codeSize, dataSize, ip, &c))
	{
		case 2:
			sprintf(msg, MSG_CHILD_CODE_FULL, (int)codeSize);
			ScriptError(o, msg, t);
			break;
		case 3:
			sprintf(msg, MSG_CHILD_DATA_FULL, (int)dataSize);
			ScriptError(o, msg, t);
			break;
		default: break;
	}
	if(ip >= c->codeSize)
	{
		sprintf(msg, MSG_CHILD_IP_OUT_OF_RANGE, (unsigned)ip);
		ScriptError(o, msg, t);
	}
	Thread_Push(t, Thread_GetId(c));
	return 0;
}

/* the engine clock's step limit: a reading more than this above the
 * previous one is taken for a stall and not counted (GetTicks).  ok 1 for
 * 50 .. 60000 ms, 0 otherwise. */
static int Opcode_Sys81_ClockMaxStep(OpenBGI_t* o, Thread_t* t) // 81 04 (1.616 on): ms → ok
{
	Thread_Push(t, (uint32_t)Clock_SetMaxStep(o, Thread_Pop(t)));
	return 0;
}

/* stop the engine clock while the window is inactive, and from 1.547 on
 * hold the playing sounds with it (Wnd_Activate); the previous setting */
static int Opcode_Sys81_StopWhenInactive(OpenBGI_t* o, Thread_t* t) // 81 68 (1.535 on): on → previous
{
	Thread_Push(t, (uint32_t)Clock_SetStopWhenInactive(o, (int)Thread_Pop(t)));
	return 0;
}

/* the key a gamepad control stands for: index 0 .. 31 the buttons, 32 ..
 * 35 the directions up, down, left, right (the hat or the stick); 0 none.
 * ok 1, 0 for an index past the table (Pad_SetKey). */
static int Opcode_Sys81_PadKey(OpenBGI_t* o, Thread_t* t) // 81 1B (1.547 on): index, key → ok
{
	uint32_t vk = Thread_Pop(t);
	uint32_t index = Thread_Pop(t);
	Thread_Push(t, Pad_SetKey(o, index, vk) == 0);
	return 0;
}

/* a fatal script error ends only the thread that raised it, the first
 * one's message being kept (ThrowScriptError, the scheduler's catch) */
static int Opcode_Sys81_KeepErrors(OpenBGI_t* o, Thread_t* t) // 81 6A (1.547 on): on →
{
	Error_SetKeep(o, (int)Thread_Pop(t));
	return 0;
}

// the kept error message into buf (may be 0); its length + 1, 0 when none was kept
static int Opcode_Sys81_KeptError(OpenBGI_t* o, Thread_t* t) // 81 6B (1.547 on): buf → n
{
	char* buf = (char*)PopPtr(o, t);
	Thread_Push(t, Error_KeptMessage(o, buf));
	return 0;
}

/* which Direct3D adapter the original presents through: mode 1 the
 * adapter of the monitor the window is on, 0 the default adapter.  ok 1
 * for 0 / 1, 0 otherwise.  Only stored here: the host presents through
 * its window system. */
static int Opcode_Sys81_SetAdapterOfMonitor(OpenBGI_t* o, Thread_t* t) // 81 62 (1.599 on): mode → ok
{
	uint32_t mode = Thread_Pop(t);
	if(mode <= 1)
		o->ops.adapterOfMonitor = (int)mode;
	Thread_Push(t, mode <= 1);
	return 0;
}

// continue the hash-list hash (the BGI.hvl hash, install.h) in `state` over `size` bytes at data
static int Opcode_Sys81_HashBuffer(OpenBGI_t* o, Thread_t* t) // 81 E9: state, data, size →
{
	uint32_t size = Thread_Pop(t);
	const void* data = PopPtr(o, t);
	uint8_t* state = (uint8_t*)PopPtr(o, t);
	HvHash_UpdateBuf(state, data, size);
	return 0;
}

/* the path of the program the engine runs in into buf (0x104 bytes): the
 * engine's own program file (file.h), which a game reads to check the
 * engine */
static int Opcode_Sys81_ProgramPath(OpenBGI_t* o, Thread_t* t) // 81 3F (1.492): buf →
{
	ProgramPath(o, (char*)PopPtr(o, t));
	return 0;
}

// redefine entry idx (0 .. 7) of the picture size table as w x h; r 0 ok, 1 bad index, 2 a size that is not positive
static int Opcode_Sys81_SetSizeEntry(OpenBGI_t* o, Thread_t* t) // 81 60 (1.492 on): idx, w, h → r
{
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int idx = (int)Thread_Pop(t);
	Thread_Push(t, (uint32_t)Display_SetSizeEntry(o, idx, w, h));
	return 0;
}

/* the encoding of the script text: mode 0 Shift-JIS (code page 932), 1
 * UTF-8 (65001); ok 1 when accepted, 0 for any other value (and no
 * change).  arcana and haruyome select UTF-8 first thing in ipl._bp. */
static int Opcode_Sys81_SetTextEncoding(OpenBGI_t* o, Thread_t* t) // 81 00 (1.653 on): mode → ok
{
	uint32_t mode = Thread_Pop(t);
	if(mode > 1)
	{
		Thread_Push(t, 0);
		return 0;
	}
	Text_SetEncoding(o, (int)mode);
	Thread_Push(t, 1);
	return 0;
}

/* a shortcut `name` to `target` with the command line `args`, in the
 * start-menu folder `folder`, or on the desktop when folder is NULL; ok 1
 * when made.  (The user-data window makes a desktop shortcut this way
 * that resumes the game with "autoload.arc".) */
static int Opcode_Sys81_CreateShortcutArgs(OpenBGI_t* o, Thread_t* t) // 81 F7 (1.553 on): args, target, name, folder → ok
{
	const char* folder = (const char*)PopPtr(o, t);
	const char* name = (const char*)PopPtr(o, t);
	const char* target = (const char*)PopPtr(o, t);
	const char* args = (const char*)PopPtr(o, t);
	Thread_Push(t, (uint32_t)CreateShortcutArgs(o, folder, name, target, args));
	return 0;
}

typedef struct WaitFileDialogN
{
	WaitAsk_t a;
	const char** d; // the filters' descriptions and patterns as host pointers
	const char** p;
} WaitFileDialogN_t;

static void WaitFileDialogN_Release(OpenBGI_t* o, WaitStep_t* base)
{
	WaitFileDialogN_t* w = (WaitFileDialogN_t*)base;
	BGI_UNUSED(o);
	BGI_Free(w->d);
	BGI_Free(w->p);
}

/* the open (kind 0) or save (kind 1) file dialog with `count` filters:
 * descs[] and patterns[] are arrays of `count` string pointers (a name
 * with its patterns, and the patterns ";"-separated), title and initialDir may be 0; code 0
 * when a file was chosen (its path in out, 0x30c bytes), -1 cancelled, 4
 * another kind, 7 count below 1 */
static int Opcode_Sys81_FileDialogMulti(OpenBGI_t* o, Thread_t* t) // 81 38 (1.553 on): out, count, descs, patterns, title, initialDir, kind → code
{
	int kind = (int)Thread_Pop(t);
	const char* initialDir = (const char*)PopPtr(o, t);
	const char* title = (const char*)PopPtr(o, t);
	const uint32_t* pats = (const uint32_t*)PopPtr(o, t);
	const uint32_t* descs = (const uint32_t*)PopPtr(o, t);
	int count = (int)Thread_Pop(t);
	char* out = (char*)PopPtr(o, t);
	WaitFileDialogN_t* w;
	uint32_t req = 0;
	int r;
	if(count < 1)
	{
		Thread_Push(t, 7);
		return 0;
	}
	w = (WaitFileDialogN_t*)WaitAsk_New(o, t, sizeof *w, "WaitFileDialog", WaitAsk_Chosen, WaitFileDialogN_Release);
	w->d = (const char**)BGI_Alloc((size_t)count * sizeof *w->d);
	w->p = (const char**)BGI_Alloc((size_t)count * sizeof *w->p);
	ResolvePtrArray(o, (void**)w->d, t, descs, count);
	ResolvePtrArray(o, (void**)w->p, t, pats, count);
	r = FileDialogN_Ask(o, &req, out, count, w->d, w->p, title, initialDir, kind);
	if(r != 0)
	{
		WaitStep_Discard(o, &w->a.s);
		Thread_Push(t, (uint32_t)r);
		return 0;
	}
	return WaitAsk_Begin(o, &w->a, req);
}

/* merge the SDC-encoded global database image `image` (the bytes of a
 * BGI.gdb in GDB 3.00 layout) into the live names and flag groups, and
 * hand its global memory prefix and system area to the script: 0x400
 * bytes into globals and 0x100000 into sysarea when they are not 0, the
 * sizes into *gSize and *sSize always; ok 1, 0 when the image is not a
 * GDB 3.00 database (the online-storage scripts merge a cloud copy this
 * way) */
static int Opcode_Sys81_GdbMerge(OpenBGI_t* o, Thread_t* t) // 81 80 (1.636 on): globals, gSize, sysarea, sSize, image → ok
{
	const void* image = PopPtr(o, t);
	uint32_t* sSize = (uint32_t*)PopPtr(o, t);
	void* sysarea = PopPtr(o, t);
	uint32_t* gSize = (uint32_t*)PopPtr(o, t);
	void* globals = PopPtr(o, t);
	Thread_Push(t, GlobalDB_MergeImage(o, image, globals, gSize, sysarea, sSize) == 0);
	return 0;
}

// ---- the file accessor (src/sys/fileacc.c) -----------------------------------

/* open `path` (as given, relative to the game directory) for reading (mode
 * 0), writing from scratch (1) or, from 1.573 on, writing into it as it
 * is (2); the handle number goes to *out; 0, 1 bad mode, 2 already open,
 * 3 cannot be opened */
static int Opcode_Sys81_FileOpen(OpenBGI_t* o, Thread_t* t) // 81 28 (1.547 on): out, path, mode → code
{
	int mode = (int)Thread_Pop(t);
	const char* path = (const char*)PopPtr(o, t);
	uint32_t* out = (uint32_t*)PopPtr(o, t);
	Thread_Push(t, (uint32_t)FileAcc_Open(o, out, path, mode));
	return 0;
}

// close a handle; *status becomes 1; 0, 4 when the handle is not open
static int Opcode_Sys81_FileClose(OpenBGI_t* o, Thread_t* t) // 81 29 (1.573 on): status, handle → code
{
	uint32_t handle = Thread_Pop(t);
	int32_t* status = (int32_t*)PopPtr(o, t);
	Thread_Push(t, (uint32_t)FileAcc_Close(o, status, handle));
	return 0;
}

// the same without a status word: 0 closed, -1 when the handle is not open
static int Opcode_Sys81_FileClose547(OpenBGI_t* o, Thread_t* t) // 81 29 (1.547, 1.553): handle → code
{
	uint32_t handle = Thread_Pop(t);
	Thread_Push(t, (uint32_t)FileAcc_Close(o, NULL, handle));
	return 0;
}

/* read `size` bytes into buf, or write them from it, as the handle was
 * opened; *status becomes the number of bytes, or -1 (1.547 / 1.553: -2)
 * when none were; 0, 4 when the handle is not open, 6 for a size of 0
 * (1.547 / 1.553) */
static int Opcode_Sys81_FileTransfer(OpenBGI_t* o, Thread_t* t) // 81 2A (1.547 on): status, handle, buf, size → code
{
	uint32_t size = Thread_Pop(t);
	void* buf = PopPtr(o, t);
	uint32_t handle = Thread_Pop(t);
	int32_t* status = (int32_t*)PopPtr(o, t);
	Thread_Push(t, (uint32_t)FileAcc_Transfer(o, status, handle, buf, size));
	return 0;
}

/* move to byte `pos` from the start, -1 for the end; *status becomes 1,
 * or -1 for a position past the end; 0, 4 when the handle is not open */
static int Opcode_Sys81_FileSeek(OpenBGI_t* o, Thread_t* t) // 81 2B (1.573 on): status, handle, pos → code
{
	uint32_t pos = Thread_Pop(t);
	uint32_t handle = Thread_Pop(t);
	int32_t* status = (int32_t*)PopPtr(o, t);
	Thread_Push(t, (uint32_t)FileAcc_Seek(o, status, handle, pos));
	return 0;
}

/* Fill o->vm.optable81 for the selected engine profile; the entries
 * name an opcode, its handler and the generations it exists in (see
 * Vm_Optable80Init in ops_sys.c for the rules) */
void Vm_Optable81Init(OpenBGI_t* o)
{
	static const OpEntry_t ops[] = {
		{0x00, Opcode_Sys81_SetTextEncoding, GEN_1_653, GEN_LAST},
		{0x10, Opcode_Sys81_KeyRepeatNew, GEN_1_69_451, GEN_LAST},
		{0x11, Opcode_Sys81_KeyboardState, GEN_1_69_472, GEN_LAST},
		{0x1F, Opcode_Sys81_SetTweenKeyMask, GEN_1_69_451, GEN_LAST},
		{0x3A, Opcode_Sys81_BrowseFolder, GEN_1_69_472, GEN_LAST},
		{0x02, Opcode_Sys81_SetUiLanguage, GEN_1_662, GEN_LAST},
		{0x03, Opcode_Sys81_GetUiLanguage, GEN_1_662, GEN_LAST},
		{0x0A, Opcode_Sys81_CpuBrand, GEN_1_529, GEN_LAST},
		{0x0B, Opcode_Sys81_DisplayAdapter, GEN_1_529, GEN_LAST},
		{0x0C, Opcode_Sys81_OsVersion, GEN_1_529, GEN_LAST},
		{0x0D, Opcode_Sys81_MemoryMB, GEN_1_529, GEN_LAST},
		{0x0E, Opcode_Sys81_DesktopSize, GEN_1_529, GEN_LAST},
		{0x0F, Opcode_Sys81_IsIconic, GEN_1_529, GEN_LAST},
		{0x14, Opcode_Sys81_KeysWhenInactive, GEN_1_529, GEN_LAST},
		{0x1E, Opcode_Sys81_SimulateClick, GEN_1_529, GEN_LAST},
		{0x2F, Opcode_Sys81_DirWritable, GEN_1_616, GEN_LAST},
		{0x28, Opcode_Sys81_FileOpen, GEN_1_547, GEN_LAST},
		{0x29, Opcode_Sys81_FileClose547, GEN_1_547, GEN_1_553},
		{0x29, Opcode_Sys81_FileClose, GEN_1_573, GEN_LAST},
		{0x2A, Opcode_Sys81_FileTransfer, GEN_1_547, GEN_LAST},
		{0x2B, Opcode_Sys81_FileSeek, GEN_1_573, GEN_LAST},
		{0x30, Opcode_Sys81_LoadFileRangeRaw, GEN_1_529, GEN_LAST},
		{0x31, Opcode_Sys81_HttpGet, GEN_1_529, GEN_LAST},
		{0x3B, Opcode_Sys81_ArcEntryDialog, GEN_1_529, GEN_LAST},
		{0x38, Opcode_Sys81_FileDialogMulti, GEN_1_553, GEN_LAST},
		{0x3F, Opcode_Sys81_ProgramPath, GEN_1_492, GEN_LAST},
		{0x60, Opcode_Sys81_SetSizeEntry, GEN_1_492, GEN_LAST},
		{0x63, Opcode_Sys81_SetFullscreenFit, GEN_1_529, GEN_LAST},
		{0x64, Opcode_Sys81_SetWindowSize, GEN_1_529, GEN_LAST},
		{0x6C, Opcode_Sys81_SetRenderSize, GEN_1_669, GEN_LAST},
		{0x62, Opcode_Sys81_SetAdapterOfMonitor, GEN_1_599, GEN_LAST},
		{0x18, Opcode_Sys81_TouchEnable, GEN_1_599, GEN_LAST},
		{0x35, Opcode_Sys81_FileSizeFast, GEN_1_588, GEN_LAST},
		{0xD8, Opcode_Sys81_StrGroupsLoad, GEN_1_653, GEN_LAST},
		{0xDA, Opcode_Sys81_StrGroupsLookup, GEN_1_653, GEN_LAST},
		{0x66, Opcode_Sys81_SetResizable, GEN_1_669, GEN_LAST},
		{0x67, Opcode_Sys81_GetWindowSize, GEN_1_669, GEN_LAST},
		{0x6D, Opcode_Sys81_ShaderSupport, GEN_1_616, GEN_LAST},
		{0x6E, Opcode_Sys81_ShaderLibrary, GEN_1_616, GEN_LAST},
		{0x6F, Opcode_Sys81_ShaderEnable, GEN_1_616, GEN_LAST},
		{0x20, Opcode_Sys81_ConvertText, GEN_1_653, GEN_LAST},
		{0xF7, Opcode_Sys81_CreateShortcutArgs, GEN_1_553, GEN_LAST},
		{0x21, Opcode_Sys81_SjisToUtf8, GEN_1_653, GEN_LAST},
		{0x19, Opcode_Sys81_TouchPoints, GEN_1_599, GEN_LAST},
		{0x16, Opcode_Sys81_TouchTracking, GEN_1_616, GEN_LAST},
		{0x17, Opcode_Sys81_TouchHistory, GEN_1_616, GEN_LAST},
		{0x69, Opcode_Sys81_TakePrintScreen, GEN_1_573, GEN_LAST},
		{0x04, Opcode_Sys81_ClockMaxStep, GEN_1_616, GEN_LAST},
		{0x3C, Opcode_Sys81_ReloadArchive, GEN_1_573, GEN_LAST},
		{0x44, Opcode_Sys81_SpawnBorrower, GEN_1_599, GEN_LAST},
		{0x80, Opcode_Sys81_GdbMerge, GEN_1_636, GEN_LAST},
		{0x68, Opcode_Sys81_StopWhenInactive, GEN_1_535, GEN_LAST},
		{0x1B, Opcode_Sys81_PadKey, GEN_1_547, GEN_LAST},
		{0x6A, Opcode_Sys81_KeepErrors, GEN_1_547, GEN_LAST},
		{0x6B, Opcode_Sys81_KeptError, GEN_1_547, GEN_LAST},
		{0xE9, Opcode_Sys81_HashBuffer, GEN_1_529, GEN_LAST},
		{0x65, Opcode_Sys81_LockWindowPos, GEN_1_69_472, GEN_LAST},
	};
	Vm_FillTable(o, o->vm.optable81, OPFAM_81, ops, BGI_COUNTOF(ops));
}
