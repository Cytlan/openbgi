/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ops_obgi.c - the "EF xx" instruction family: OpenBGI's own extensions
 *
 * No original build has an instruction that starts with the byte EF, in
 * any generation, which is why the extensions live there: a program that
 * uses them cannot be mistaken for anything else, and one that does not is
 * unaffected.  The family exists in every engine profile (kOpSet, opset.c)
 * and is called "obgi" by the assembler and the compiler ("obgi.version",
 * "EF 00").  docs/modding/obgi.md describes the instructions from a script
 * writer's point of view.
 *
 * A program built with these instructions does not run on the original
 * engine, which stops at the first EF byte with "undefined instruction".
 *
 * The conventions are those of ops_sys.c: the trailing comment of a
 * handler gives its stack effect, the inputs in the order the script
 * pushed them (the last one on top), then the outputs; every handler
 * returns scheduler code 0.  The interface (o->vm.optableEF,
 * Vm_OptableEFInit) is declared in vm.h.
 */
#include <stdio.h>

#include "bgi/vm.h"
#include "bgi/error.h"
#include "bgi/sys.h"
#include "bgi/msg.h"
#include "bgi/strutil.h"
#include "bgi/gfx/text.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/gfx/pattern.h"
#include "bgi/launcher.h"
#include "bgi/host.h"
#include "bgi/wait.h"
#include "bgi/waitobj.h"
#include "bgi/version.h"
#include "bgi/engine.h"
#include "bgi/sjis.h"
#include "bgi/input.h"
#include "bgi/core.h"

#define OBGI_VERSION 3 // the level of the extension family: raised whenever instructions are added (3: "EF 31", "EF 32")

// raise a script error on thread t with a formatted MSG_* text; does not return
#define GFX_ERROR(o, t, ...)        \
	do                              \
	{                               \
		char msg_[0x104];           \
		sprintf(msg_, __VA_ARGS__); \
		ScriptError(o, msg_, (t));  \
	} while(0)

// the level of the extensions this engine has, so that a program can ask before it uses a later addition
static int Opcode_Obgi_Version(OpenBGI_t* o, Thread_t* t) // EF 00: → level
{
	Thread_Push(t, OBGI_VERSION);
	return 0;
}

/* the direction of rich text (message windows, "91 9C" bitmap text):
 * 0 the original left-to-right layout, 1 right-to-left paragraphs (Arabic,
 * Hebrew: lines start at the right edge, Arabic letters are joined, Latin
 * words and numbers inside keep their own direction), 2 left-to-right
 * paragraphs that may contain right-to-left text.  Any other value is 0.
 * In 1 and 2 a character that is not East Asian is as wide as its font
 * makes it - the original gives everything beyond ASCII a full cell - so 2
 * is the setting for European languages.  Text beyond the code page needs
 * the UTF-8 text encoding ("EF 02"). */
static int Opcode_Obgi_TextDirection(OpenBGI_t* o, Thread_t* t) // EF 01: mode →
{
	Text_SetDirection(o, (int)Thread_Pop(t));
	return 0;
}

/* the encoding of the script's strings: 0 Shift-JIS (code page 932), 1
 * UTF-8; other values change nothing.  This is "81 00" of 1.653 on made
 * available to every generation.  It belongs at the very start of the boot
 * program: every string the scripts hold must be in the encoding, and
 * glyphs drawn before a switch stay cached under their old codes. */
static int Opcode_Obgi_TextEncoding(OpenBGI_t* o, Thread_t* t) // EF 02: mode →
{
	uint32_t mode = Thread_Pop(t);
	if(mode <= 1)
		Text_SetEncoding(o, (int)mode);
	return 0;
}

/* break lines between words: the generations before 1.492 break a line
 * wherever it is full, also inside a Latin word.  With this on they keep a
 * word together and drop the space a line would end with, as the later
 * generations do (which this does not change). */
static int Opcode_Obgi_TextWordWrap(OpenBGI_t* o, Thread_t* t) // EF 03: on →
{
	Text_SetWordWrap(o, (int)Thread_Pop(t));
	return 0;
}

/* scrolling message text: with `on`, a message that is longer than the
 * text area of its window is no longer cut off.  The text scrolls up as
 * it is printed, pauses when a hurried output has filled the window, and
 * can be scrolled back and forth while the output waits for the click
 * (wheel, up / down keys, the arrows of "EF 09").  `msPerLine` is the time
 * a scroll of one line takes; 0 or less keeps the current one (120 at
 * start). */
static int Opcode_Obgi_TextScroll(OpenBGI_t* o, Thread_t* t) // EF 08: on, msPerLine →
{
	int ms = (int)Thread_Pop(t);
	int on = (int)Thread_Pop(t);
	Text_SetScrolling(o, on, ms);
	return 0;
}

/* the two arrows a scrolling window shows while there is text above or
 * below the view: for each the bitmap number of its picture (-1: a
 * triangle in the text colour, sized by the font) and its position in the
 * window in pixels (a negative x or y: the end of the first line for "up",
 * of the last line for "down" - the right end, or the left end while the
 * text direction is right to left).  The pictures are copied, so the
 * bitmaps may be freed afterwards.  A bitmap that does not exist is a
 * script error. */
static int Opcode_Obgi_TextScrollArrows(OpenBGI_t* o, Thread_t* t) // EF 09: bmpUp, upX, upY, bmpDown, downX, downY →
{
	int downY = (int)Thread_Pop(t), downX = (int)Thread_Pop(t), bmpDown = (int)Thread_Pop(t);
	int upY = (int)Thread_Pop(t), upX = (int)Thread_Pop(t), bmpUp = (int)Thread_Pop(t);
	if(bmpUp >= 0)
		CheckBitmapNo(o, bmpUp, t);
	if(bmpDown >= 0)
		CheckBitmapNo(o, bmpDown, t);
	if(!Text_SetArrow(o, 0, bmpUp, upX, upY))
		GFX_ERROR(o, t, MSG_BMP_NOT_EXIST, bmpUp);
	if(!Text_SetArrow(o, 1, bmpDown, downX, downY))
		GFX_ERROR(o, t, MSG_BMP_NOT_EXIST, bmpDown);
	return 0;
}

/* how much of the string `src` fits into `lines` lines of window `h`,
 * laid out like a message ("91 92" with the same rubyOn and hang) from the
 * start of a line.  That part is copied to `dst` - at most `maxBytes`
 * bytes plus the terminator, cut at the end of a line, without the line
 * break or space it ended with - and the number of bytes of `src` it used
 * up is pushed (0 only for an empty string or a window without a font).
 * Calling it in a loop cuts a long text into pieces of a few lines each. */
static int Opcode_Obgi_TextFit(OpenBGI_t* o, Thread_t* t) // EF 0A: h, dst, src, lines, maxBytes, rubyOn, hang → used
{
	int hang = (int)Thread_Pop(t), rubyOn = (int)Thread_Pop(t);
	int maxBytes = (int)Thread_Pop(t), lines = (int)Thread_Pop(t);
	const char* src = (const char*)PopPtr(o, t);
	char* dst = (char*)PopPtr(o, t);
	uint32_t h = Thread_Pop(t);
	Window_t* win = (Window_t*)Gfx_FindWindow(o->gfx.gfx, h);
	if(!win)
	{
		ScriptError(o, MSG_BAD_WINDOW_HANDLE, t);
		Thread_Push(t, 0);
		return 0;
	}
	Thread_Push(t, (uint32_t)Text_Fit(o, win, dst, src, lines, maxBytes, rubyOn, hang));
	return 0;
}

/* the width in pixels of `str` drawn on one line in the numbered font, as
 * the rich text layout places it (so it is right for proportional fonts
 * and for joined Arabic too); 0 for an empty string.  For centring a name
 * or a caption. */
static int Opcode_Obgi_TextWidth(OpenBGI_t* o, Thread_t* t) // EF 0B: str, fontNo, size, widthPct, bold, prop → width
{
	int prop = (int)Thread_Pop(t), bold = (int)Thread_Pop(t), widthPct = (int)Thread_Pop(t);
	int size = (int)Thread_Pop(t), fontNo = (int)Thread_Pop(t);
	const char* str = (const char*)PopPtr(o, t);
	int handle = 0;
	CheckFontNo(o, fontNo, t);
	if(BmpOp_FontOpen(o, &handle, fontNo, size, widthPct, bold) != 0)
	{
		Thread_Push(t, 0);
		return 0;
	}
	Thread_Push(t, (uint32_t)Text_LineWidth(o, str, handle, prop));
	return 0;
}

/* how far the text of window `h` is scrolled, in pixels: 0 while the
 * view is at the top (always, for a window whose text fits), and during a
 * scroll the position reached so far.  A program that gives the mouse
 * wheel a meaning of its own (wheel up opens the backlog) asks this to
 * leave the wheel to the text while there is something to scroll back to. */
static int Opcode_Obgi_TextScrollPos(OpenBGI_t* o, Thread_t* t) // EF 0C: h → y
{
	uint32_t h = Thread_Pop(t);
	Window_t* win = (Window_t*)Gfx_FindWindow(o->gfx.gfx, h);
	if(!win)
		ScriptError(o, MSG_BAD_WINDOW_HANDLE, t);
	Thread_Push(t, win ? (uint32_t)Window_DocScroll(win) : 0);
	return 0;
}

// -------------------------------------------------------------------------
// EF 20 .. EF 2D : the host's folders, the games and the settings, for a
// program that chooses a game (src/sys/launcher.c, docs/dev/launcher.md)
// -------------------------------------------------------------------------

/* The strings of these instructions are in the scripts' text encoding
 * ("EF 02"); the engine's side of them is UTF-8.  A path is the host's
 * own, as text: it is not one of the game's. */
static void FromScript(OpenBGI_t* o, const char* s, char* out, size_t n)
{
	Sjis_ToUtf8(s ? s : "", out, n, o->enc.textUtf8);
}

// a UTF-8 string into a script's buffer of `size` bytes, cut between characters; the length written
static int ToScript(OpenBGI_t* o, const char* utf8, char* dst, int size)
{
	char buf[LAUNCHER_PATH * 2];
	size_t n;
	if(size <= 0 || !dst)
		return 0;
	if(o->enc.textUtf8)
		snprintf(buf, sizeof buf, "%s", utf8);
	else
		Sjis_FromUtf8(utf8, buf, sizeof buf);
	n = strlen(buf);
	if(n >= (size_t)size)
	{
		n = (size_t)size - 1;
		if(o->enc.textUtf8)
			while(n > 0 && ((uint8_t)buf[n] & 0xc0) == 0x80)
				n--;
		else
		{ // not through a two-byte character
			size_t i = 0;
			while(i < n)
			{
				size_t step = Sjis_IsLead((uint8_t)buf[i]) && buf[i + 1] ? 2 : 1;
				if(i + step > n)
					break;
				i += step;
			}
			n = i;
		}
	}
	memcpy(dst, buf, n);
	dst[n] = 0;
	return (int)n;
}

/* read the folder `path` of the host ("" the computer: its drives, or the
 * root): its folders, then its files, each sorted by name; with flag 1
 * the hidden ones as well, after the others.  0 when it cannot be read. */
// ---- the work a host does beside the frames (bgi/launcher.h) -------------------------------

typedef struct WaitJob
{
	Wait_t base;
	struct OpenBGI_Job* job; // NULL once it went with the run
	uint8_t* info;           // "EF 28": where the program wants what was found
} WaitJob_t;

static void WaitJob_Destroy(OpenBGI_t* o, Wait_t* w);
static int WaitJob_Poll(OpenBGI_t* o, Wait_t* w);
static const WaitVtbl_t WaitJob_Vtbl = {WaitJob_Destroy, WaitJob_Poll, Wait_SetTimer, Wait_IsDirtyBase, Wait_OnMessage};

const char* WaitJob_ClassName(const WaitVtbl_t* vt)
{
	return vt == &WaitJob_Vtbl ? "WaitJob" : NULL;
}

void Launcher_JobGone(Wait_t* w)
{
	if(w->vt == &WaitJob_Vtbl)
		((WaitJob_t*)w)->job = NULL;
}

// a game as the program's struct GameInfo
static void PutGameInfo(OpenBGI_t* o, uint8_t* info, const LauncherGame_t* g)
{
	int32_t kind = g->kind;
	memset(info, 0, 128 + 16 + 64 + 4);
	ToScript(o, g->name, (char*)info, 128);
	ToScript(o, g->profile, (char*)info + 128, 16);
	ToScript(o, g->product, (char*)info + 144, 64);
	memcpy(info + 208, &kind, 4);
}

// a job that is done as its instruction's result: a listing's handle, or whether a game was found (and which)
static void JobResult(OpenBGI_t* o, Thread_t* t, struct OpenBGI_Job* job, uint8_t* info)
{
	LauncherGame_t g;
	int r;
	memset(&g, 0, sizeof g);
	r = Launcher_JobTake(o, job, &g);
	if(info)
		PutGameInfo(o, info, &g);
	Thread_Push(t, (uint32_t)r);
}

static void WaitJob_Destroy(OpenBGI_t* o, Wait_t* w)
{
	WaitJob_t* j = (WaitJob_t*)w;
	w->vt = &WaitJob_Vtbl;
	if(j->job) // the thread goes before its answer came
		Launcher_JobDrop(o, j->job);
	Wait_Dtor(o, w);
	BGI_Free(w);
}

// done when the host handed the job back (the work is not cut short: "80 50" does not end this wait)
static int WaitJob_Poll(OpenBGI_t* o, Wait_t* w)
{
	WaitJob_t* j = (WaitJob_t*)w;
	Wait_Tick(o, w);
	if(!j->job)
	{ // (the run is ending)
		if(j->info)
			memset(j->info, 0, 128 + 16 + 64 + 4);
		Thread_Push(w->thread, 0);
		return 1;
	}
	if(!Launcher_JobReady(j->job))
		return 0;
	JobResult(o, w->thread, j->job, j->info);
	j->job = NULL;
	return 1;
}

/* The instruction's work as a job: what a job made ahead found is the
 * result at once; else the thread waits for it.  -1 when there is no
 * such host, or no memory: the instruction does the work itself. */
static int JobAsk(OpenBGI_t* o, Thread_t* t, int kind, const char* path, int flags, uint8_t* info)
{
	struct OpenBGI_Job* job = Launcher_JobsOn(o) ? Launcher_JobFor(o, kind, path, flags) : NULL;
	WaitJob_t* w;
	if(!job)
		return -1;
	if(Launcher_JobReady(job))
	{
		JobResult(o, t, job, info);
		return 0;
	}
	w = (WaitJob_t*)BGI_Alloc(sizeof *w);
	Wait_Ctor(o, &w->base, t);
	w->base.vt = &WaitJob_Vtbl;
	w->job = job;
	w->info = info;
	Launcher_JobWait(job, &w->base);
	Thread_SetWait(o, t, &w->base);
	return 2;
}

static int Opcode_Obgi_DirOpen(OpenBGI_t* o, Thread_t* t) // EF 20: path, flags → h
{
	int flags = (int)Thread_Pop(t);
	char path[LAUNCHER_PATH * 2];
	int r = -1;
	FromScript(o, (const char*)PopPtr(o, t), path, sizeof path);
	if(path[0] || !Launcher_TopKnown(o)) // (the computer's drives, or the host's roots, are known at once)
		r = JobAsk(o, t, LAUNCHER_JOB_LIST, path, flags, NULL);
	if(r >= 0)
		return r;
	Thread_Push(t, (uint32_t)Launcher_DirOpen(o, path, flags));
	return 0;
}

static int Opcode_Obgi_DirCount(OpenBGI_t* o, Thread_t* t) // EF 21: h → n
{
	Thread_Push(t, (uint32_t)Launcher_DirCount(o, (int)Thread_Pop(t)));
	return 0;
}

// the name of entry `i` into dst (at most `size` bytes with the terminator) and what it is: 0 none, 1 a file, 2 a folder
static int Opcode_Obgi_DirEntry(OpenBGI_t* o, Thread_t* t) // EF 22: h, i, dst, size → kind
{
	int size = (int)Thread_Pop(t);
	char* dst = (char*)PopPtr(o, t);
	int i = (int)Thread_Pop(t);
	int h = (int)Thread_Pop(t);
	char name[LAUNCHER_PATH];
	int kind = Launcher_DirEntry(o, h, i, name, sizeof name);
	ToScript(o, name, dst, size);
	Thread_Push(t, (uint32_t)kind);
	return 0;
}

static int Opcode_Obgi_DirClose(OpenBGI_t* o, Thread_t* t) // EF 23: h →
{
	Launcher_DirClose(o, (int)Thread_Pop(t));
	return 0;
}

/* whether there is a game at `path` - a folder, a zip archive, a disc
 * image - and which: `info` receives its name (128 bytes), the profile
 * the detection chose (16), its product name (64) and its kind (a
 * number: 1 a folder, 2 a zip archive, 3 a disc image); all zero when
 * there is none */
static int Opcode_Obgi_GameProbe(OpenBGI_t* o, Thread_t* t) // EF 28: path, info → found
{
	uint8_t* info = (uint8_t*)PopPtr(o, t);
	char path[LAUNCHER_PATH * 2];
	LauncherGame_t g;
	int found, r;
	FromScript(o, (const char*)PopPtr(o, t), path, sizeof path);
	r = JobAsk(o, t, LAUNCHER_JOB_PROBE, path, 0, info);
	if(r >= 0)
		return r;
	found = Launcher_Probe(o, path, &g);
	PutGameInfo(o, info, &g);
	Thread_Push(t, (uint32_t)found);
	return 0;
}

/* start the game at `path` in place of this program's run: with an engine
 * profile ("" for the detection), patches (strings one after the other,
 * an empty one at the end; 0 for none) and more options of the command
 * line.  The machine stops and the game boots in a run of its own; 0
 * comes back only when there is no game to start. */
static int Opcode_Obgi_LaunchGame(OpenBGI_t* o, Thread_t* t) // EF 29: path, profile, patches, options → ok
{
	char path[LAUNCHER_PATH * 2], profile[64], options[0x400], block[0x1000];
	const char* p;
	size_t used = 0;
	uint32_t raw;
	FromScript(o, (const char*)PopPtr(o, t), options, sizeof options);
	raw = Thread_Pop(t);
	p = raw ? (const char*)ResolvePtr(o, raw, t) : NULL;
	FromScript(o, (const char*)PopPtr(o, t), profile, sizeof profile);
	FromScript(o, (const char*)PopPtr(o, t), path, sizeof path);
	for(; p && *p && used + LAUNCHER_PATH * 2 + 2 < sizeof block; p += strlen(p) + 1)
	{
		FromScript(o, p, block + used, sizeof block - used - 1);
		used += strlen(block + used) + 1;
	}
	block[used] = 0;
	if(!Launcher_Launch(o, path, profile, block, options))
	{
		Thread_Push(t, 0);
		return 0;
	}
	Thread_Push(t, 1);
	return 6; // the machine stops; the frame's end starts the game
}

/* The settings of openbgi.json (version.h, Engine_Cfg*): `path` "" for the
 * program's own (and the lists "profiles" and "games", which are only
 * read), a game's path for what is kept of that game.  A setting is a
 * list of values, most often of one.
 *
 * value `i` of a setting into dst: its length, -1 when there is none - a
 * list is read from 0 on until that comes back */
static int Opcode_Obgi_CfgGet(OpenBGI_t* o, Thread_t* t) // EF 2C: path, key, i, dst, size → len
{
	int size = (int)Thread_Pop(t);
	char* dst = (char*)PopPtr(o, t);
	int i = (int)Thread_Pop(t);
	char key[128], path[LAUNCHER_PATH * 2], value[LAUNCHER_PATH];
	int len;
	FromScript(o, (const char*)PopPtr(o, t), key, sizeof key);
	FromScript(o, (const char*)PopPtr(o, t), path, sizeof path);
	value[0] = 0;
	if(!path[0] && (len = Settings_Value(o, key, i, value, sizeof value)) != 0)
		len = len > 0 ? (int)strlen(value) : -1; // the run's mode, and in the settings over a game what they show of the game's engine
	else if(!path[0] && strcmp(key, "dropped") == 0)
		len = Launcher_Dropped(o, i, value, sizeof value); // what was dropped on the window
	else if(!path[0] && strcmp(key, "previews") == 0)
		len = i == 0 ? Launcher_PreviewCount(o, value, sizeof value) : -1; // the first pictures that came in
	else if(!path[0] && (len = Launcher_RootValue(o, key, i, value, sizeof value)) != 0)
		len = len > 0 ? (int)strlen(value) : -1; // the host's roots, a list
	else if(!path[0] && Launcher_HostValue(o, key, value, sizeof value))
		len = i == 0 ? (int)strlen(value) : -1; // what the engine says of the host
	else
		len = Engine_CfgGet(o, path, key, i, value, sizeof value);
	if(size > 0 && dst)
		dst[0] = 0;
	Thread_Push(t, len < 0 ? 0xffffffffu : (uint32_t)ToScript(o, value, dst, size));
	return 0;
}

/* value `i` of a setting becomes `value`, a new last one when `i` is the
 * number of values; the value 0 deletes it.  The engine writes the file
 * itself, a moment after the last change and when the program ends. */
static int Opcode_Obgi_CfgSet(OpenBGI_t* o, Thread_t* t) // EF 2D: path, key, i, value → ok
{
	uint32_t raw = Thread_Pop(t);
	const char* value = raw ? (const char*)ResolvePtr(o, raw, t) : NULL;
	int i = (int)Thread_Pop(t);
	char key[128], path[LAUNCHER_PATH * 2], text[LAUNCHER_PATH * 2], fixed[8];
	int ok;
	FromScript(o, (const char*)PopPtr(o, t), key, sizeof key);
	FromScript(o, (const char*)PopPtr(o, t), path, sizeof path);
	if(value)
		FromScript(o, value, text, sizeof text);
	if(!path[0] && (ok = Settings_Set(o, key, value ? text : NULL)) >= 0)
	{ // what the settings over a game change of the game's engine
		Thread_Push(t, (uint32_t)ok);
		return 0;
	}
	if(!path[0] && strcmp(key, "dropped") == 0)
	{ // what was dropped on the window is taken by deleting it, and nothing else
		Thread_Push(t, (uint32_t)(!value && Launcher_DropTake(o, i)));
		return 0;
	}
	if(!path[0] && (strcmp(key, "preview") == 0 || strcmp(key, "previews") == 0))
	{ // a game the program lists: its icon and a first picture are the engine's to get
		Thread_Push(t, (uint32_t)(value && strcmp(key, "preview") == 0 && Launcher_Listed(o, text)));
		return 0;
	}
	// (what the engine says of the host is not a setting)
	ok = !(!path[0] && (Launcher_HostValue(o, key, fixed, sizeof fixed) || Launcher_RootValue(o, key, 0, fixed, sizeof fixed))) &&
		Engine_CfgSet(o, path, key, i, value ? text : NULL);
	if(ok)
		Launcher_SettingsChanged(o);
	Thread_Push(t, (uint32_t)ok);
	return 0;
}

// -------------------------------------------------------------------------
// EF 30 : gray patterns made by rule (src/gfx/pattern.c)
// -------------------------------------------------------------------------

/* draw a pattern into the gray bitmap `bmp` ("90 11" with mode 3) over
 * what it holds: `kind` is the pattern (a ramp at an angle, rings, a clock
 * sweep, noise, tiles, a polygon, a line, shaded triangles), `mode` how it
 * meets what is there, and `params` points at the `count` numbers the
 * kind reads (src/include/bgi/gfx/pattern.h, docs/modding/obgi.md).  The bitmap then
 * serves wherever the engine takes a gray one: as an alpha channel
 * ("91 1E"), as the pattern of a wipe ("90 19", "90 43", "90 5A"), as a
 * mask ("90 55").  ok 0, and nothing drawn, when the bitmap is not a gray
 * one or the numbers do not fit the kind. */
static int Opcode_Obgi_GeneratePattern(OpenBGI_t* o, Thread_t* t) // EF 30: bmp, kind, mode, params, count → ok
{
	int count = (int)Thread_Pop(t);
	const int32_t* params = (const int32_t*)PopPtr(o, t);
	int mode = (int)Thread_Pop(t), kind = (int)Thread_Pop(t), bmp = (int)Thread_Pop(t);
	Bmp_t b;
	CheckBitmapNo(o, bmp, t);
	if(!BmpOp_GetInfo(o, &b, bmp) || !b.pixels)
		GFX_ERROR(o, t, MSG_BMP_NOT_EXIST, bmp);
	if(count < 0 || count > PAT_POINTS_MAX * 2 + 8)
		count = 0;
	Thread_Push(t, (uint32_t)Pattern_Generate(&b, kind, mode, params, count));
	return 0;
}

// -------------------------------------------------------------------------
// EF 31 : a see-through picture
// -------------------------------------------------------------------------

/* The picture gets an alpha channel (on 1) or loses it again (0): a
 * 32-bit screen is made as ARGB32, no background is drawn, and what no
 * object covers is clear.  The engine that shows the picture over
 * another's - the settings over a running game (sub.h) - blends it by
 * that alpha; a host shows it as ever, the fourth byte being none of its
 * colours.  A screen that is up is made anew, so the program says it
 * before it sets its display mode.  Not every effect draws into such a
 * picture (docs/modding/obgi.md). */
static int Opcode_Obgi_SeeThrough(OpenBGI_t* o, Thread_t* t) // EF 31: on →
{
	int on = Thread_Pop(t) != 0;
	if(on != o->gfx.seeThrough)
	{
		o->gfx.seeThrough = on;
		if(o->gfx.gfx && ScreenMode(o) != PM_RGB16 && o->screen.pic.pixels)
			SetDisplayMode(o, o->display.sizeIndex, o->display.pixelMode, o->display.fullscreen);
	}
	return 0;
}

// -------------------------------------------------------------------------
// EF 32 : the finger as a held button
// -------------------------------------------------------------------------

/* A finger on a touch screen is the left button from now on (on 1): held
 * from when the finger goes down until it lifts, the pointer where the
 * finger is.  The program then tells a tap from a drag itself and moves
 * its list under the finger.  The engine's own gestures - the tap aimed
 * at a target, the long press, the flick, two fingers - are off for as
 * long; 0 brings them back.  A mouse and a pad are as ever. */
static int Opcode_Obgi_TouchDrag(OpenBGI_t* o, Thread_t* t) // EF 32: on →
{
	Touch_SetDrag(o, Thread_Pop(t) != 0);
	return 0;
}

void Vm_OptableEFInit(OpenBGI_t* o)
{
	static const OpEntry_t ops[] = {
		{0x00, Opcode_Obgi_Version, GEN_FIRST, GEN_LAST},
		{0x01, Opcode_Obgi_TextDirection, GEN_FIRST, GEN_LAST},
		{0x02, Opcode_Obgi_TextEncoding, GEN_FIRST, GEN_LAST},
		{0x03, Opcode_Obgi_TextWordWrap, GEN_FIRST, GEN_LAST},
		{0x08, Opcode_Obgi_TextScroll, GEN_FIRST, GEN_LAST},
		{0x09, Opcode_Obgi_TextScrollArrows, GEN_FIRST, GEN_LAST},
		{0x0A, Opcode_Obgi_TextFit, GEN_FIRST, GEN_LAST},
		{0x0B, Opcode_Obgi_TextWidth, GEN_FIRST, GEN_LAST},
		{0x0C, Opcode_Obgi_TextScrollPos, GEN_FIRST, GEN_LAST},
		{0x20, Opcode_Obgi_DirOpen, GEN_FIRST, GEN_LAST},
		{0x21, Opcode_Obgi_DirCount, GEN_FIRST, GEN_LAST},
		{0x22, Opcode_Obgi_DirEntry, GEN_FIRST, GEN_LAST},
		{0x23, Opcode_Obgi_DirClose, GEN_FIRST, GEN_LAST},
		{0x28, Opcode_Obgi_GameProbe, GEN_FIRST, GEN_LAST},
		{0x29, Opcode_Obgi_LaunchGame, GEN_FIRST, GEN_LAST},
		{0x2C, Opcode_Obgi_CfgGet, GEN_FIRST, GEN_LAST},
		{0x2D, Opcode_Obgi_CfgSet, GEN_FIRST, GEN_LAST},
		{0x30, Opcode_Obgi_GeneratePattern, GEN_FIRST, GEN_LAST},
		{0x31, Opcode_Obgi_SeeThrough, GEN_FIRST, GEN_LAST},
		{0x32, Opcode_Obgi_TouchDrag, GEN_FIRST, GEN_LAST},
	};
	Vm_FillTable(o, o->vm.optableEF, OPFAM_EF, ops, BGI_COUNTOF(ops));
}
