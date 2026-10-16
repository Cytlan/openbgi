/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ops_sys81.c - the "81 xx" second system-control instruction family
 *
 * The dispatcher appeared in 1.69 build 472 (TayutamaHD) with six opcodes;
 * the family filled up from 1.529 on (docs/versions.md).  The conventions
 * are those of ops_sys.c: the trailing comment of a handler gives its stack
 * effect, the inputs in the order the script pushed them (the last one on
 * top), then the outputs; every handler returns scheduler code 0.  The interface (vm_optable_81, Vm_Optable81Init) is
 * declared in vm.h.
 *
 * Several instructions of this family drove Windows-only features of the
 * original (WinInet, Direct3D shaders, touch input, the window border and
 * placement).  Where the OS layer has no counterpart the handler only
 * stores the setting or answers "not available", as noted on each.
 */
#include "bgi/vm.h"
#include "bgi/error.h"
#include "bgi/sys.h"
#include "bgi/msg.h"
#include "bgi/input.h"
#include "bgi/display.h"
#include "bgi/os.h"
#include "bgi/file.h"
#include "bgi/install.h"
#include "bgi/strutil.h"
#include "bgi/sysobj.h"

// whether the auto-repeat of virtual key vk counts as presses; pushes the previous setting
static int Opcode_Sys81_KeyRepeatNew(Thread_t* t) // 81 10: vk, on → old
{
	int on = (int)Thread_Pop(t);
	int vk = (int)Thread_Pop(t);
	Thread_Push(t, (uint32_t)Key_SetRepeatNew(vk, on));
	return 0;
}

// the state of all 256 virtual keys into buf (GetKeyboardState: bit 7 of a byte = down)
static int Opcode_Sys81_KeyboardState(Thread_t* t) // 81 11: buf →
{
	OS_KeyboardState((uint8_t*)PopPtr(t));
	return 0;
}

// the standard keys (bits of the "80 1A" mask) that break a tween
static int Opcode_Sys81_SetTweenKeyMask(Thread_t* t) // 81 1F: mask →
{
	Input_SetTweenKeyMask(Thread_Pop(t));
	return 0;
}

/* the folder browser (SHBrowseForFolder returning file-system folders
 * only and not descending below the domain level, `initial` selected at
 * the start); the chosen path into out (0x104 bytes), ok 1 when one was
 * chosen.  A NULL title shows the default prompt MSG_SELECT_FOLDER
 * ("フォルダを選択してください", "Please select a folder"). */
static int Opcode_Sys81_BrowseFolder(Thread_t* t) // 81 3A: out, title, initial → ok
{
	const char* initial = (const char*)PopPtr(t);
	const char* title = (const char*)PopPtr(t);
	char* out = (char*)PopPtr(t);
	Thread_Push(t, (uint32_t)OS_BrowseFolder(out, 0x104, title ? title : MSG_SELECT_FOLDER, initial));
	return 0;
}

/* keep the window at its computed position: in the original the
 * WM_WINDOWPOSCHANGING handler re-imposes it while the flag is set.  Only
 * stored here, window placement being the OS layer's. */
static int gWindowPosLock;                         // "81 65": the window may not be moved
static int Opcode_Sys81_LockWindowPos(Thread_t* t) // 81 65: on →
{
	gWindowPosLock = (int)Thread_Pop(t);
	return 0;
}

// ---- the 1.529 family -----------------------------------------------------------------

// the processor brand string (0x40 bytes, runs of spaces collapsed) into out; ok 1 when the CPUID leaves exist
static int Opcode_Sys81_CpuBrand(Thread_t* t) // 81 0A: out → ok
{
	char* out = (char*)PopPtr(t);
	Thread_Push(t, (uint32_t)Cpu_BrandString(out));
	return 0;
}

/* the display adapter's description (runs of spaces collapsed, the ends
 * trimmed) into desc and its driver version as four words, most
 * significant first, into ver[0 .. 3]; the top operand is popped and
 * ignored */
static int Opcode_Sys81_DisplayAdapter(Thread_t* t) // 81 0B: desc, ver, x →
{
	uint32_t* ver;
	char* desc;
	char raw[0x200];
	const char* p;
	char* d;
	PopPtr(t);
	ver = (uint32_t*)PopPtr(t);
	desc = (char*)PopPtr(t);
	OS_DisplayAdapter(raw, sizeof raw, ver);
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
static int Opcode_Sys81_OsVersion(Thread_t* t) // 81 0C: ver, csd →
{
	char* csd = (char*)PopPtr(t);
	uint32_t* ver = (uint32_t*)PopPtr(t);
	OS_VersionEx(ver, csd, 0x80);
	return 0;
}

// the physical memory in MB (from the 64-bit figures of GlobalMemoryStatusEx) into *total and *avail
static int Opcode_Sys81_MemoryMB(Thread_t* t) // 81 0D: total, avail →
{
	uint32_t* avail = (uint32_t*)PopPtr(t);
	uint32_t* total = (uint32_t*)PopPtr(t);
	uint64_t tp, ap;
	OS_MemoryStatusEx(&tp, &ap);
	*total = (uint32_t)(tp >> 20);
	*avail = (uint32_t)(ap >> 20);
	return 0;
}

// the desktop size into out[0 .. 1], adjusted for multi-monitor layouts (Display_GetDesktopSize)
static int Opcode_Sys81_DesktopSize(Thread_t* t) // 81 0E: out →
{
	int32_t* out = (int32_t*)PopPtr(t);
	int v[2];
	Display_GetDesktopSize(v, 1);
	out[0] = v[0];
	out[1] = v[1];
	return 0;
}

// whether the window is minimised (IsIconic)
static int Opcode_Sys81_IsIconic(Thread_t* t) // 81 0F: → f
{
	Thread_Push(t, (uint32_t)OS_WindowMinimized());
	return 0;
}

// whether key states are read while the window is inactive
static int Opcode_Sys81_KeysWhenInactive(Thread_t* t) // 81 14: on →
{
	Input_SetKeysWhenInactive((int)Thread_Pop(t));
	return 0;
}

// a click of a mouse button at the cursor: kind 1 left, 2 right, 4 middle, 5 / 6 the X buttons; ok 1 when sent
static int Opcode_Sys81_SimulateClick(Thread_t* t) // 81 1E: kind → ok
{
	Thread_Push(t, (uint32_t)Window_SimulateClick((int)Thread_Pop(t)));
	return 0;
}

/* Can a file be created in the directory `dir`?  The original makes a
 * temporary file there (GetTempFileName with the prefix "BGI"), writes
 * to it and deletes it again, impersonating the logged-on user first
 * when it runs with another token; 1 when all of that worked, 0 when
 * not.  The scripts ask it about their user-data folder. */
static int Opcode_Sys81_DirWritable(Thread_t* t) // 81 2F (1.616 on): dir → ok
{
	const char* dir = (const char*)PopPtr(t);
	char path[0x210];
	char name[32];
	size_t n = strlen(dir);
	OsFile_t* f;
	int ok = 0;
	snprintf(name, sizeof name, "BGI%04X.tmp", (unsigned)(OS_TicksMs() & 0xffffu));
	if(n + 1 + strlen(name) >= sizeof path)
	{
		Thread_Push(t, 0);
		return 0;
	}
	snprintf(path, sizeof path, "%s%s%s", dir, n && (dir[n - 1] == '\\' || dir[n - 1] == '/') ? "" : "\\", name);
	f = OS_FileCreate(path);
	if(f)
	{
		ok = OS_FileWrite(f, "BGI", 3) == 3;
		OS_FileClose(f);
		OS_FileDelete(path);
	}
	Thread_Push(t, (uint32_t)ok);
	return 0;
}

/* `size` bytes from offset `off` of the raw (stored) file into buf - what
 * "80 31" did before 1.69 build 472 made it the decoded range; 0 ok, 1
 * not found, 2 / 3 range errors, -1 short read */
static int Opcode_Sys81_LoadFileRangeRaw(Thread_t* t) // 81 30: buf, arc, name, off, size → code
{
	uint32_t size = Thread_Pop(t), off = Thread_Pop(t);
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	void* buf = PopPtr(t);
	Thread_Push(t, (uint32_t)LoadFileRange(buf, arc, name, off, size));
	return 0;
}

/* a range of a web resource, fetched through WinInet in the original (0
 * ok, 1 no connection or open failure, 2 read failure, 3 short read).
 * There is no network access here: the operands are dropped and 1 is
 * pushed. */
static int Opcode_Sys81_HttpGet(Thread_t* t) // 81 31: buf, url, off, size → code
{
	Thread_Pop(t);
	Thread_Pop(t);
	PopPtr(t);
	PopPtr(t);
	Thread_Push(t, 1);
	return 0;
}

/* 1.662 on: the UI language the scripts ask the engine about, a Windows
 * LANGID (the primary language in the low 10 bits): "81 02" sets it - 0
 * takes the user's own from the OS - and returns it, "81 03" returns it
 * (0 until "81 02" ran, as in the original).  The boot programs of the
 * builds with other languages (Haruka Ao no Hanayome ni) pick their text
 * by it. */
static int32_t gUiLanguage;
static int Opcode_Sys81_SetUiLanguage(Thread_t* t) // 81 02 (1.662 on): lang → lang
{
	int32_t lang = (int32_t)Thread_Pop(t);
	gUiLanguage = lang ? lang : OS_UiLanguage();
	Thread_Push(t, (uint32_t)gUiLanguage);
	return 0;
}

static int Opcode_Sys81_GetUiLanguage(Thread_t* t) // 81 03 (1.662 on): → lang
{
	Thread_Push(t, (uint32_t)gUiLanguage);
	return 0;
}

/* the "choose an entry of an archive" dialog of the development builds (a
 * resource dialog listing the archive's names, the chosen one copied
 * out).  Not provided: the four pointer operands are dropped and r is -1
 * (cancelled). */
static int Opcode_Sys81_ArcEntryDialog(Thread_t* t) // 81 3B: p1, p2, p3, p4 → r
{
	PopPtr(t);
	PopPtr(t);
	PopPtr(t);
	PopPtr(t);
	Thread_Push(t, 0xffffffffu);
	return 0;
}

// how the picture fits the full screen: mode 0 stretched, 1 keeping the aspect, 2 centred; ok 1 for 0 .. 2 (stored only)
static int Opcode_Sys81_SetFullscreenFit(Thread_t* t) // 81 63: mode → ok
{
	Thread_Push(t, (uint32_t)Display_SetFullscreenFit((int)Thread_Pop(t)));
	return 0;
}

// 1.669 on: the size the renderer scales the picture to; ok 1 when its aspect is the desktop's (stored only)
static int Opcode_Sys81_SetRenderSize(Thread_t* t) // 81 6C (1.669 on): w, h → ok
{
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	Thread_Push(t, (uint32_t)Display_SetRenderSize(w, h));
	return 0;
}

// the window's client size in pixels (0, 0: the picture size); "81 67" reads it back
static int Opcode_Sys81_SetWindowSize(Thread_t* t) // 81 64: w, h →
{
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	Display_SetWindowSize(w, h);
	return 0;
}

/* convert the text at src into dst: mode 0 to Shift-JIS, 1 to UTF-8,
 * anything else to the current script text encoding ("81 00").  The
 * source's encoding is judged from its bytes (TextSniff, strutil.h):
 * plain ASCII and text already in the target encoding are copied as they
 * are, a conversion is bounded to 0x10000 bytes of dst.  Always pushes 1. */
static int Opcode_Sys81_ConvertText(Thread_t* t) // 81 20 (1.653 on): dst, src, mode → 1
{
	uint32_t mode = Thread_Pop(t);
	const char* src = (const char*)PopPtr(t);
	char* dst = (char*)PopPtr(t);
	uint32_t target = mode == 0 ? TEXT_SJIS : mode == 1 ? TEXT_UTF8
														: (uint32_t)gTextUtf8;
	uint32_t have = TextSniff(src);
	if(have == TEXT_RAW || have == target)
	{
		if(dst != src) // a script may convert in place
			memmove(dst, src, strlen(src) + 1);
	}
	else if(target == TEXT_UTF8)
		OS_SjisToUtf8(src, dst, 0x10000);
	else
		OS_Utf8ToSjis(src, dst, 0x10000);
	Thread_Push(t, 1);
	return 0;
}

// Shift-JIS at src to UTF-8 at dst (0x10000 bytes), without sniffing
static int Opcode_Sys81_SjisToUtf8(Thread_t* t) // 81 21 (1.653 on): dst, src →
{
	const char* src = (const char*)PopPtr(t);
	char* dst = (char*)PopPtr(t);
	OS_SjisToUtf8(src, dst, 0x10000);
	return 0;
}

// the size of a file without reading it (GetFileSizeFast), unlike "80 35"
static int Opcode_Sys81_FileSizeFast(Thread_t* t) // 81 35 (1.588 on): arc, name → size
{
	const char* name = (const char*)PopPtr(t);
	const char* arc = (const char*)PopPtr(t);
	Thread_Push(t, GetFileSizeFast(arc, name));
	return 0;
}

/* load a block of named string groups (strgroups.h; `size` bytes at data)
 * on top of those already known; data NULL drops them all.  ok 1 when the
 * block was consumed exactly. */
static int Opcode_Sys81_StrGroupsLoad(Thread_t* t) // 81 D8 (1.653 on): data, size → ok
{
	uint32_t size = Thread_Pop(t);
	const uint8_t* data = (const uint8_t*)PopPtr(t);
	Thread_Push(t, (uint32_t)StrGroups_Load(data, size));
	return 0;
}

/* the string stored under `key` in `group` into out (when out is not
 * NULL) and its length; 0 when there is none */
static int Opcode_Sys81_StrGroupsLookup(Thread_t* t) // 81 DA (1.653 on): out, group, key → len
{
	const char* key = (const char*)PopPtr(t);
	const char* group = (const char*)PopPtr(t);
	char* out = (char*)PopPtr(t);
	const char* v = StrGroups_Lookup(group, key);
	if(v && out)
		strcpy(out, v);
	Thread_Push(t, v ? (uint32_t)strlen(v) : 0u);
	return 0;
}

/* whether the main window gets a sizing border (WS_THICKFRAME in the
 * original, which picks the window style by this flag).  Only stored
 * here. */
static int gResizableWindow;                      // "81 66": the window has a sizing border
static int Opcode_Sys81_SetResizable(Thread_t* t) // 81 66 (1.669 on): on →
{
	gResizableWindow = (int)Thread_Pop(t);
	return 0;
}

// the window's client size - what "81 64" set, else the picture size - into out[0 .. 1]
static int Opcode_Sys81_GetWindowSize(Thread_t* t) // 81 67 (1.669 on): out →
{
	int32_t* out = (int32_t*)PopPtr(t);
	int w = 0, h = 0;
	Display_GetWindowSize(&w, &h);
	out[0] = w;
	out[1] = h;
	return 0;
}

/* Pixel shaders: from 1.616 on the original's Direct3D presentation can
 * run a shader effect (its "SHADER" resource, compiled through d3dx9_43).
 * "81 6D" tells whether shader support was found at start-up, "81 6E"
 * whether d3dx9 could be loaded, "81 6F" loads (on 1) or releases (on 0)
 * the effect and pushes 1 when that worked.  There are no shaders here:
 * no support, no library, and only switching the effect off succeeds. */
static int Opcode_Sys81_ShaderSupport(Thread_t* t) // 81 6D (1.616 on): → f
{
	Thread_Push(t, 0);
	return 0;
}

static int Opcode_Sys81_ShaderLibrary(Thread_t* t) // 81 6E (1.616 on): → f
{
	Thread_Push(t, 0);
	return 0;
}

static int Opcode_Sys81_ShaderEnable(Thread_t* t) // 81 6F (1.616 on): on → ok
{
	uint32_t on = Thread_Pop(t);
	Thread_Push(t, on == 0);
	return 0;
}

/* Touch input: from 1.599 on the original registers its window for touch
 * messages through user32's RegisterTouchWindow / GetTouchInputInfo,
 * loaded at run time.  "81 18" registers (on 1, palm touches included) or
 * unregisters the window and pushes the API's result, 0 without the API;
 * "81 19" copies the touch points the window procedure collected (six
 * values each) into buf and pushes their number.  There is no touch input
 * here: 0 and no points. */
static int Opcode_Sys81_TouchEnable(Thread_t* t) // 81 18 (1.599 on): on → ok
{
	Thread_Pop(t);
	Thread_Push(t, 0);
	return 0;
}

static int Opcode_Sys81_TouchPoints(Thread_t* t) // 81 19 (1.599 on): buf → n
{
	PopPtr(t);
	Thread_Push(t, 0);
	return 0;
}

/* which Direct3D adapter the original presents through: mode 1 the
 * adapter of the monitor the window is on, 0 the default adapter.  ok 1
 * for 0 / 1, 0 otherwise.  Only stored here: the OS layer presents
 * through the window system. */
static int gAdapterOfMonitor;                            // "81 62": present through the adapter of the window's monitor
static int Opcode_Sys81_SetAdapterOfMonitor(Thread_t* t) // 81 62 (1.599 on): mode → ok
{
	uint32_t mode = Thread_Pop(t);
	if(mode <= 1)
		gAdapterOfMonitor = (int)mode;
	Thread_Push(t, mode <= 1);
	return 0;
}

// continue the hash-list hash (the BGI.hvl hash, install.h) in `state` over `size` bytes at data
static int Opcode_Sys81_HashBuffer(Thread_t* t) // 81 E9: state, data, size →
{
	uint32_t size = Thread_Pop(t);
	const void* data = PopPtr(t);
	uint8_t* state = (uint8_t*)PopPtr(t);
	HvHash_UpdateBuf(state, data, size);
	return 0;
}

// redefine entry idx (0 .. 7) of the picture size table as w x h; r 0 ok, 1 bad index, 2 a size that is not positive
static int Opcode_Sys81_SetSizeEntry(Thread_t* t) // 81 60 (1.494 on): idx, w, h → r
{
	int h = (int)Thread_Pop(t);
	int w = (int)Thread_Pop(t);
	int idx = (int)Thread_Pop(t);
	Thread_Push(t, (uint32_t)Display_SetSizeEntry(idx, w, h));
	return 0;
}

/* the encoding of the script text: mode 0 Shift-JIS (code page 932), 1
 * UTF-8 (65001); ok 1 when accepted, 0 for any other value (and no
 * change).  arcana and haruyome select UTF-8 first thing in ipl._bp. */
static int Opcode_Sys81_SetTextEncoding(Thread_t* t) // 81 00 (1.653 on): mode → ok
{
	uint32_t mode = Thread_Pop(t);
	if(mode > 1)
	{
		Thread_Push(t, 0);
		return 0;
	}
	Text_SetEncoding((int)mode);
	Thread_Push(t, 1);
	return 0;
}

/* a shortcut `name` to `target` with the command line `args`, in the
 * start-menu folder `folder`, or on the desktop when folder is NULL; ok 1
 * when made.  (The user-data window makes a desktop shortcut this way
 * that resumes the game with "autoload.arc".) */
static int Opcode_Sys81_CreateShortcutArgs(Thread_t* t) // 81 F7 (1.553 on): args, target, name, folder → ok
{
	const char* folder = (const char*)PopPtr(t);
	const char* name = (const char*)PopPtr(t);
	const char* target = (const char*)PopPtr(t);
	const char* args = (const char*)PopPtr(t);
	Thread_Push(t, (uint32_t)CreateShortcutArgs(folder, name, target, args));
	return 0;
}

/* Fill vm_optable_81 for the selected engine profile; the entries name an
 * opcode, its handler and the generations it exists in (see
 * Vm_Optable80Init in ops_sys.c for the rules) */
void Vm_Optable81Init(void)
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
		{0x30, Opcode_Sys81_LoadFileRangeRaw, GEN_1_529, GEN_LAST},
		{0x31, Opcode_Sys81_HttpGet, GEN_1_529, GEN_LAST},
		{0x3B, Opcode_Sys81_ArcEntryDialog, GEN_1_529, GEN_LAST},
		{0x60, Opcode_Sys81_SetSizeEntry, GEN_1_494, GEN_LAST},
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
		{0xE9, Opcode_Sys81_HashBuffer, GEN_1_529, GEN_LAST},
		{0x65, Opcode_Sys81_LockWindowPos, GEN_1_69_472, GEN_LAST},
	};
	Vm_FillTable(vm_optable_81, OPFAM_81, ops, BGI_COUNTOF(ops));
}
