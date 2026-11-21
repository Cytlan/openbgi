/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * main.c - the test host (make headless): a game run on a clock of its
 *          own, with pictures of its frames and its sound written or
 *          compared
 *
 *     openbgi-headless [options] [--] <the engine's command line>
 *
 *     --run=MS       how long the game runs on the host's clock (30000)
 *     --step=MS      how far the clock moves after a frame (1); a frame that
 *                    asks for a longer wait ("80 52") gets that
 *     --refresh=HZ   the display's refresh rate (60): a frame that presented
 *                    a picture waits for the next vertical blank, as the
 *                    original's presents through Direct3D do; 0 no wait
 *     --every=MS     a picture every MS of the clock (1000; 0: none)
 *     --shots=DIR    write the pictures into DIR as <ms>.png
 *     --compare=DIR  compare each picture with DIR/<ms>.png and the sound
 *                    with DIR/sound.txt; the exit code is 1 when a picture
 *                    differs or is missing there, or the sound differs
 *     --input=FILE   the player's input and more pictures, from a script
 *     --wav=FILE     write the sound as a WAVE file (44.1 kHz 16-bit stereo)
 *     --targets      with each picture what a click reaches (OpenBGI_Targets),
 *                    written to DIR/targets.txt with --shots and compared
 *                    with the baseline's with --compare
 *     --reload=N     make the run N more times in the same process, in turn
 *                    on the same engine started again (OpenBGI_Stop,
 *                    OpenBGI_Start) and on a new one (OpenBGI_Delete,
 *                    OpenBGI_New); each must give the first run's
 *                    pictures, sound and targets, else the exit code is 1.
 *                    The files a game writes stay between the runs: a game
 *                    in a zip gets a folder of its own for each run under
 *                    BGI_DATA when that is set, and then nothing differs
 *     --preview=FILE the run a desktop host makes for the launcher's first
 *                    picture of the game (OsPreview_Run, src/host/os_common.h),
 *                    and nothing else: its picture as the PNG file FILE; the
 *                    exit code is 1 when the run gave none.  The command line
 *                    is the one the launcher would hand out (--preview and
 *                    the game's path at least)
 *     --preview-icon=FILE
 *                    with --preview=: the icon of the game's own program that
 *                    the run found, as an .ico file
 *
 * The script has one action per line, `MS ACTION [ARGUMENTS]`, MS the
 * host's clock when it happens (FROM-TO/EVERY: at FROM, FROM + EVERY, ...
 * up to TO); `#` begins a comment:
 *
 *     key K          press the key K and let go of it 100 ms later
 *     down K, up K   press it, let go of it
 *     click X Y      move the pointer to X Y (the picture's pixels) and
 *                    click the left button (100 ms); rclick the right one
 *     move X Y       move the pointer
 *     wheel N        turn the wheel N notches (+ away from the player)
 *     resize W H     the player gave the window a client area of W x H
 *                    pixels (OpenBGI_WindowSize; a window with a sizing
 *                    border, "81 66")
 *     type TEXT      type the rest of the line (UTF-8) as an input method
 *                    hands it over (OpenBGI_Text)
 *     tap X Y        touch the screen at X Y for 80 ms (OpenBGI_Touch);
 *                    longpress for 800 ms; tap2 with two fingers 20 pixels
 *                    left and right of it for 100 ms
 *     swipe X Y X2 Y2 [MS]
 *                    a finger from X Y to X2 Y2 over MS (150), a move
 *                    every 16 ms; drag the same over 1000 ms unless it says
 *     scroll2 X Y DY [MS]
 *                    two fingers as tap2 moving DY pixels down (up when
 *                    negative) over MS (500)
 *     touch ID down|move|up|cancel X Y
 *                    a contact of the host's id ID (tap, swipe and their
 *                    kin use 1, and 2 for the second finger)
 *     touchscale PPI the picture's pixels per inch (OpenBGI_TouchScale)
 *     pad C          press the control C of the pad (OpenBGI_Pad, port 0)
 *                    and let go of it 100 ms later; paddown C and padup C
 *                    press it and let go of it
 *     stick left|right X Y [MS]
 *                    a stick to X Y, -100 .. 100 of its travel (right and
 *                    down positive); back to its rest after MS when given
 *     action A       the action A begins (OpenBGI_Action) and ends 100 ms
 *                    later; actiondown A and actionup A begin and end it
 *     shot           a picture at MS besides those of --every
 *     targets        what a click reaches at MS (with --targets), besides
 *                    what it reaches at the pictures' times
 *     end            stop the run
 *
 * A key is a name (RETURN, SPACE, ESCAPE, BACK, TAB, SHIFT, CONTROL, ALT,
 * UP, DOWN, LEFT, RIGHT, PRIOR, NEXT, HOME, END, INSERT, DELETE, F1 to
 * F12), a letter or digit, or a virtual-key code (0x0D).  A pad's
 * control is south, east, west, north (the face buttons), l1, r1, l2, r2,
 * l3, r3, select, start, guide, or up, down, left, right (the D-pad); an
 * action is confirm, cancel, skip, backlog, forward, up, down, left,
 * right or keyboard.  Input is
 * handed to the engine (OpenBGI_Key and its kin) right after the first
 * frame at or after its time, where a window's messages are pumped; the
 * pointer stays where the script or the engine ("80 1F") put it last.  A
 * picture shows what the frames before its time presented.
 *
 * The host is deterministic: its clock starts at 0 and moves only between
 * frames (a frame that presented waits for a vertical blank of the
 * display, --refresh), the date is 1 January 2026, noon, the time stamp
 * counter counts the same clock (os_headless.h), and it plays no sound:
 * the engine mixes it for the time that passed (OpenBGI_SetAudioLead 0)
 * and the host reads it after each frame.  The same game and command line
 * give the same frames and the same sound on every run.  With --shots the
 * sound is summed up a second at a time in DIR/sound.txt, a line
 * "<second> <FNV-1a of its frames>" each (the last, shorter second has
 * its frame count after the sum), which --compare compares.  The targets
 * of --targets are a line each, "<ms> <kind> <x>,<y> <w>x<h> at <x>,<y>
 * owner <owner> <group>/<index> layer <key>" and the target's flags (a
 * knob's travel after "drag"), "<ms> none" for a time with none.  Requests
 * are answered by the OS layer as the program's host answers them
 * (OS_AnswerRequests): the boxes go to stderr and take their default
 * button, the dialogs are cancelled.  With the engine's --dialogs=frame
 * the engine draws the boxes and the dialogs in its pictures instead, and
 * the script's input answers them.  The text entry and the child windows
 * are always the engine's, drawn in the pictures.  A picture is what a
 * window that shows each frame's picture (OpenBGI_Frame_t) would show.
 * The host has no window table: the engine's window is the one nobody
 * sees (openbgi.h, "The main window"), on a desktop of 1024 x 768, with
 * the focus while it is shown.  The files, the registry, the machine
 * (its time stamp counter the host's clock), the shell and the movie
 * decoder are the OS layer's (host/os_common.h); there is no CD audio and
 * no movie player, so the movies are decoded (the movie library, on the
 * engine's thread and by the host's clock) and shown in the pictures: a
 * run that plays a movie is repeated exactly, like any other.
 */
#include "host/os.h"
#include "host/os_common.h"
#include "host/os_headless.h"
#ifdef BGI_TOOLS
#include "tools/tools.h"
#endif
#include "bgi/png.h"
#include "openbgi.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t gNow; // the host's clock (ms)

static uint32_t HostTicksMs(void* user)
{
	BGI_UNUSED(user);
	return gNow;
}

static int HostPerfNs(void* user, int64_t* out)
{
	BGI_UNUSED(user);
	*out = (int64_t)gNow * 1000000;
	return 1;
}

static void HostLocalTime(void* user, OpenBGI_DateTime_t* out)
{
	BGI_UNUSED(user);
	memset(out, 0, sizeof *out);
	out->year = 2026;
	out->month = 1;
	out->day = 1;
	out->dayOfWeek = 4; // a Thursday
	out->hour = 12;
}

// "81 00": the back end's text in the scripts' encoding
static void HostTextEncoding(void* user, int utf8)
{
	BGI_UNUSED(user);
	OS_SetTextEncoding(utf8);
}

/* the host's clocks, with the fonts of the back end (OS_FontFiles: the
 * system's font files), the OS layer's dialogs, files and registry */
static OpenBGI_Machine_t gMachine; // the back end's, but for the language

// the test host's system speaks English wherever it runs
static int FixedUiLanguage(void* user)
{
	(void)user;
	return 0x409;
}

static OpenBGI_Host_t gHost = {.size = sizeof(OpenBGI_Host_t), .api = OPENBGI_API_VERSION, .ticksMs = HostTicksMs, .perfNs = HostPerfNs, .localTime = HostLocalTime, .caps = OPENBGI_CAP_DIALOGS, .textEncoding = HostTextEncoding};

// ---- the window ----------------------------------------------------------------------

/* What the host's window shows: its own copy of the engine's picture,
 * brought up to date with the changes of each frame (OpenBGI_Frame_t), as
 * a window on a screen would be.  The pictures are taken from it, so they
 * also show whether the frames report every change. */
typedef struct Window
{
	uint8_t* px; // h rows of `pitch` bytes; NULL before the first picture
	int w, h, pitch, bpp;
} Window_t;

static void Window_Update(Window_t* win, const OpenBGI_Frame_t* f)
{
	int i, y;
	if(!f->pixels || !f->changes)
		return;
	if(f->changes < 0 || f->width != win->w || f->height != win->h || f->bpp != win->bpp || f->pitch != win->pitch)
	{ // a new picture: all of it
		uint8_t* px = (uint8_t*)realloc(win->px, (size_t)f->pitch * (size_t)f->height);
		if(!px)
			return;
		win->px = px;
		win->w = f->width;
		win->h = f->height;
		win->pitch = f->pitch;
		win->bpp = f->bpp;
		memcpy(win->px, f->pixels, (size_t)f->pitch * (size_t)f->height);
		return;
	}
	for(i = 0; i < f->changes; i++)
	{
		const OpenBGI_Rect_t* r = &f->change[i];
		size_t off = (size_t)r->x * (size_t)(f->bpp / 8), n = (size_t)r->w * (size_t)(f->bpp / 8);
		for(y = r->y; y < r->y + r->h; y++)
			memcpy(win->px + (size_t)y * (size_t)win->pitch + off, f->pixels + (size_t)y * (size_t)f->pitch + off, n);
	}
}

// ---- pictures ------------------------------------------------------------------------

typedef struct Shot
{
	int w, h;
	uint8_t* bgr; // w * h * 3: B G R, as the PNG layer takes it
} Shot_t;

// what the window shows, as a picture; 0 when it shows nothing yet
static int Shot_Grab(const Window_t* win, Shot_t* s)
{
	int x, y;
	if(!win->px)
		return 0;
	s->w = win->w;
	s->h = win->h;
	s->bgr = (uint8_t*)malloc((size_t)s->w * (size_t)s->h * 3);
	if(!s->bgr)
		return 0;
	for(y = 0; y < s->h; y++)
	{
		const uint8_t* row = win->px + (size_t)y * (size_t)win->pitch;
		uint8_t* d = s->bgr + (size_t)y * (size_t)s->w * 3;
		for(x = 0; x < s->w; x++, d += 3)
		{
			if(win->bpp == 16)
			{
				unsigned p = (unsigned)(row[x * 2] | row[x * 2 + 1] << 8); // 5-5-5, widened as the window back ends do
				unsigned b = p & 31, g = (p >> 5) & 31, r = (p >> 10) & 31;
				d[0] = (uint8_t)(b << 3 | b >> 2);
				d[1] = (uint8_t)(g << 3 | g >> 2);
				d[2] = (uint8_t)(r << 3 | r >> 2);
			}
			else
				memcpy(d, row + x * 4, 3); // B G R x
		}
	}
	return 1;
}

static int WriteFile(const char* path, const void* data, size_t n)
{
	FILE* f = fopen(path, "wb");
	int ok;
	if(!f)
		return 0;
	ok = fwrite(data, 1, n, f) == n;
	return fclose(f) == 0 && ok;
}

static uint8_t* ReadFile(const char* path, size_t* n)
{
	FILE* f = fopen(path, "rb");
	uint8_t* data;
	long size;
	if(!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = (uint8_t*)malloc(size > 0 ? (size_t)size : 1);
	if(data && fread(data, 1, (size_t)size, f) != (size_t)size)
	{
		free(data);
		data = NULL;
	}
	fclose(f);
	*n = (size_t)size;
	return data;
}

/* The pixels of `s` that differ from the PNG at `path`: 0 the same, -1
 * when there is no such picture or it has another size. */
static long Shot_Compare(const Shot_t* s, const char* path)
{
	size_t n;
	uint8_t* data = ReadFile(path, &n);
	PngImage_t img;
	char err[128];
	long diff = 0;
	int x, y;
	if(!data)
		return -1;
	memset(&img, 0, sizeof img);
	if(Png_Decode(data, n, &img, err, sizeof err))
		Png_Convert(&img, 3); // an alpha channel of another tool's picture is dropped
	if(!img.pixels || img.channels != 3 || (int)img.width != s->w || (int)img.height != s->h)
	{
		free(data);
		free(img.pixels);
		return -1;
	}
	for(y = 0; y < s->h; y++)
		for(x = 0; x < s->w; x++)
		{
			const uint8_t* a = s->bgr + ((size_t)y * (size_t)s->w + (size_t)x) * 3;
			const uint8_t* b = img.pixels + ((size_t)y * img.width + (size_t)x) * 3;
			if(a[0] != b[0] || a[1] != b[1] || a[2] != b[2])
				diff++;
		}
	free(data);
	free(img.pixels);
	return diff;
}

// ---- the input script ----------------------------------------------------------------

enum
{
	ACT_KEY,    // a: vk, b: down
	ACT_BUTTON, // a: button, b: down
	ACT_MOVE,   // a, b: x, y
	ACT_WHEEL,  // a: delta
	ACT_RESIZE, // a, b: the window's client size
	ACT_TEXT,   // text
	ACT_TOUCH,  // a: the contact's id, b: OPENBGI_TOUCH_*, x, y
	ACT_SCALE,  // a: the picture's pixels per inch (OpenBGI_TouchScale)
	ACT_PAD,    // a: the control (0 .. 12 a button's bit, 16 .. 19 a direction's bit + 16), b: down
	ACT_STICK,  // a: 0 the left one, 1 the right one; x, y: where it is, -32767 .. 32767
	ACT_ACTION, // a: OPENBGI_ACTION_*, b: down
	ACT_SHOT,
	ACT_TARGETS,
	ACT_END
};

typedef struct Action
{
	uint32_t ms;
	int seq; // the position in the script, which orders actions of the same time
	int kind, a, b, x, y;
	char text[128]; // ACT_TEXT's, UTF-8
} Action_t;

typedef struct Script
{
	Action_t* acts;
	int count, cap, next;
} Script_t;

#define HOLD_MS     100  // how long `key` and `click` hold the key or button
#define TAP_MS      80   // how long a `tap` touches
#define TAP2_MS     100  // how long a `tap2` touches
#define LONG_MS     800  // how long a `longpress` touches
#define SWIPE_MS    150  // how long a `swipe` takes unless it says
#define DRAG_MS     1000 // how long a `drag` takes unless it says
#define SCROLL2_MS  500  // how long a `scroll2` takes unless it says
#define TOUCH_EVERY 16   // a moving finger is reported this often
#define TWO_APART   20   // the fingers of `tap2` and `scroll2` are this far left and right of the point

static const struct
{
	const char* name;
	int vk;
} kKeys[] = {
	{"RETURN", 0x0D},
	{"ENTER", 0x0D},
	{"SPACE", 0x20},
	{"ESCAPE", 0x1B},
	{"ESC", 0x1B},
	{"BACK", 0x08},
	{"TAB", 0x09},
	{"SHIFT", 0x10},
	{"CONTROL", 0x11},
	{"CTRL", 0x11},
	{"ALT", 0x12},
	{"MENU", 0x12},
	{"LEFT", 0x25},
	{"UP", 0x26},
	{"RIGHT", 0x27},
	{"DOWN", 0x28},
	{"PRIOR", 0x21},
	{"NEXT", 0x22},
	{"END", 0x23},
	{"HOME", 0x24},
	{"INSERT", 0x2D},
	{"DELETE", 0x2E},
};

// a key's virtual-key code; 0 for none
static int KeyCode(const char* k)
{
	size_t i;
	char* e;
	long v;
	if(k[0] && !k[1] && ((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= '0' && k[0] <= '9')))
		return k[0];
	if(k[0] && !k[1] && k[0] >= 'a' && k[0] <= 'z')
		return k[0] - 'a' + 'A';
	if((k[0] == 'F' || k[0] == 'f') && k[1] >= '1' && k[1] <= '9')
	{
		v = strtol(k + 1, &e, 10);
		if(!*e && v >= 1 && v <= 12)
			return 0x70 + (int)v - 1;
	}
	for(i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++)
	{
		const char *a = kKeys[i].name, *b = k;
		while(*a && (*b == *a || *b == *a - 'A' + 'a'))
			a++, b++;
		if(!*a && !*b)
			return kKeys[i].vk;
	}
	if(k[0] == '0' && (k[1] == 'x' || k[1] == 'X'))
	{
		v = strtol(k + 2, &e, 16);
		if(!*e && v > 0 && v < 256)
			return (int)v;
	}
	return 0;
}

// a pad's control by name: 0 .. 12 a button's bit, 16 .. 19 a direction's bit + 16; -1 for none
static int PadControl(const char* name)
{
	static const char* const kNames[] = {"south", "east", "west", "north", "l1", "r1", "select", "start", "l3", "r3", "l2", "r2", "guide"};
	static const char* const kDirs[] = {"up", "down", "left", "right"};
	int i;
	for(i = 0; i < (int)(sizeof kNames / sizeof kNames[0]); i++)
		if(strcmp(name, kNames[i]) == 0)
			return i;
	for(i = 0; i < 4; i++)
		if(strcmp(name, kDirs[i]) == 0)
			return 16 + i;
	return -1;
}

// an action by name (OPENBGI_ACTION_*); 0 for none
static int ActionCode(const char* name)
{
	static const char* const kNames[] = {"confirm", "cancel", "skip", "backlog", "forward", "up", "down", "left", "right", "keyboard"};
	int i;
	for(i = 0; i < (int)(sizeof kNames / sizeof kNames[0]); i++)
		if(strcmp(name, kNames[i]) == 0)
			return OPENBGI_ACTION_CONFIRM + i;
	return 0;
}

// an action with a position (ACT_TOUCH)
static void Script_AddAt(Script_t* sc, uint32_t ms, int kind, int a, int b, int x, int y)
{
	if(sc->count == sc->cap)
	{
		int cap = sc->cap ? sc->cap * 2 : 64;
		Action_t* n = (Action_t*)realloc(sc->acts, (size_t)cap * sizeof *n);
		if(!n)
			return;
		sc->acts = n;
		sc->cap = cap;
	}
	memset(&sc->acts[sc->count], 0, sizeof sc->acts[sc->count]);
	sc->acts[sc->count].ms = ms;
	sc->acts[sc->count].seq = sc->count;
	sc->acts[sc->count].kind = kind;
	sc->acts[sc->count].a = a;
	sc->acts[sc->count].b = b;
	sc->acts[sc->count].x = x;
	sc->acts[sc->count].y = y;
	sc->count++;
}

static void Script_Add(Script_t* sc, uint32_t ms, int kind, int a, int b)
{
	Script_AddAt(sc, ms, kind, a, b, 0, 0);
}

/* fingers `ids` (n of them) from (x0 + dx[i], y0) to (x1 + dx[i], y1) over
 * `dur` ms from `ms`: down, a move every TOUCH_EVERY ms, the last one at
 * the end, up there */
static void Script_Stroke(Script_t* sc, uint32_t ms, int n, const int* dx, int x0, int y0, int x1, int y1, uint32_t dur)
{
	uint32_t t;
	int i;
	for(i = 0; i < n; i++)
		Script_AddAt(sc, ms, ACT_TOUCH, i + 1, OPENBGI_TOUCH_DOWN, x0 + dx[i], y0);
	if(x1 != x0 || y1 != y0)
		for(t = TOUCH_EVERY; t < dur + TOUCH_EVERY; t += TOUCH_EVERY)
		{
			uint32_t at = t < dur ? t : dur;
			int x = x0 + (int)((long)(x1 - x0) * (long)at / (long)(dur ? dur : 1));
			int y = y0 + (int)((long)(y1 - y0) * (long)at / (long)(dur ? dur : 1));
			for(i = 0; i < n; i++)
				Script_AddAt(sc, ms + at, ACT_TOUCH, i + 1, OPENBGI_TOUCH_MOVE, x + dx[i], y);
		}
	for(i = 0; i < n; i++)
		Script_AddAt(sc, ms + dur, ACT_TOUCH, i + 1, OPENBGI_TOUCH_UP, x1 + dx[i], y1);
}

// by time; actions of the same time keep their order
static int ActionCmp(const void* x, const void* y)
{
	const Action_t *a = (const Action_t*)x, *b = (const Action_t*)y;
	if(a->ms != b->ms)
		return a->ms < b->ms ? -1 : 1;
	return a->seq < b->seq ? -1 : a->seq > b->seq;
}

/* One line's action at the clock's `ms` into `sc`; 0 when the line is not
 * understood.  `word` and `args` are the line after its time. */
static int Script_Line(Script_t* sc, uint32_t ms, const char* word, const char* args)
{
	char arg[32];
	int x, y, vk;
	if(strcmp(word, "key") == 0 || strcmp(word, "down") == 0 || strcmp(word, "up") == 0)
	{
		if(sscanf(args, "%31s", arg) != 1 || !(vk = KeyCode(arg)))
			return 0;
		if(word[0] != 'u')
			Script_Add(sc, ms, ACT_KEY, vk, 1);
		if(word[0] != 'd')
			Script_Add(sc, ms + (word[0] == 'k' ? HOLD_MS : 0), ACT_KEY, vk, 0);
	}
	else if(strcmp(word, "click") == 0 || strcmp(word, "rclick") == 0 || strcmp(word, "move") == 0)
	{
		if(sscanf(args, "%d %d", &x, &y) != 2)
			return 0;
		Script_Add(sc, ms, ACT_MOVE, x, y);
		if(word[0] != 'm')
		{
			int b = word[0] == 'r' ? 1 : 0;
			Script_Add(sc, ms, ACT_BUTTON, b, 1);
			Script_Add(sc, ms + HOLD_MS, ACT_BUTTON, b, 0);
		}
	}
	else if(strcmp(word, "wheel") == 0)
	{
		if(sscanf(args, "%d", &x) != 1)
			return 0;
		Script_Add(sc, ms, ACT_WHEEL, x * 120, 0);
	}
	else if(strcmp(word, "resize") == 0)
	{
		if(sscanf(args, "%d %d", &x, &y) != 2)
			return 0;
		Script_Add(sc, ms, ACT_RESIZE, x, y);
	}
	else if(strcmp(word, "type") == 0)
	{
		int n = (int)strlen(args);
		while(n > 0 && (args[n - 1] == '\n' || args[n - 1] == '\r' || args[n - 1] == ' ' || args[n - 1] == '\t'))
			n--;
		if(n <= 0 || n >= (int)sizeof sc->acts[0].text)
			return 0;
		Script_Add(sc, ms, ACT_TEXT, 0, 0);
		if(sc->count && sc->acts[sc->count - 1].kind == ACT_TEXT)
			memcpy(sc->acts[sc->count - 1].text, args, (size_t)n);
	}
	else if(strcmp(word, "tap") == 0 || strcmp(word, "longpress") == 0 || strcmp(word, "tap2") == 0)
	{
		static const int one[1] = {0}, two[2] = {-TWO_APART, TWO_APART};
		if(sscanf(args, "%d %d", &x, &y) != 2)
			return 0;
		if(word[3] == '2')
			Script_Stroke(sc, ms, 2, two, x, y, x, y, TAP2_MS);
		else
			Script_Stroke(sc, ms, 1, one, x, y, x, y, word[0] == 'l' ? LONG_MS : TAP_MS);
	}
	else if(strcmp(word, "swipe") == 0 || strcmp(word, "drag") == 0)
	{
		static const int one[1] = {0};
		int x1, y1, dur = word[0] == 's' ? SWIPE_MS : DRAG_MS, n = sscanf(args, "%d %d %d %d %d", &x, &y, &x1, &y1, &dur);
		if(n < 4 || dur < 0)
			return 0;
		Script_Stroke(sc, ms, 1, one, x, y, x1, y1, (uint32_t)dur);
	}
	else if(strcmp(word, "scroll2") == 0)
	{
		static const int two[2] = {-TWO_APART, TWO_APART};
		int dy, dur = SCROLL2_MS, n = sscanf(args, "%d %d %d %d", &x, &y, &dy, &dur);
		if(n < 3 || dur < 0)
			return 0;
		Script_Stroke(sc, ms, 2, two, x, y, x, y + dy, (uint32_t)dur);
	}
	else if(strcmp(word, "touch") == 0)
	{
		static const char* const kPhases[] = {"down", "move", "up", "cancel"};
		int id, phase;
		if(sscanf(args, "%d %31s %d %d", &id, arg, &x, &y) != 4)
			return 0;
		for(phase = 0; phase < 4 && strcmp(arg, kPhases[phase]) != 0; phase++)
			;
		if(phase == 4)
			return 0;
		Script_AddAt(sc, ms, ACT_TOUCH, id, phase, x, y); // (OPENBGI_TOUCH_DOWN 0 ... CANCEL 3)
	}
	else if(strcmp(word, "touchscale") == 0)
	{
		if(sscanf(args, "%d", &x) != 1 || x < 0)
			return 0;
		Script_Add(sc, ms, ACT_SCALE, x, 0);
	}
	else if(strcmp(word, "pad") == 0 || strcmp(word, "paddown") == 0 || strcmp(word, "padup") == 0)
	{
		if(sscanf(args, "%31s", arg) != 1 || (x = PadControl(arg)) < 0)
			return 0;
		if(word[3] != 'u')
			Script_Add(sc, ms, ACT_PAD, x, 1);
		if(word[3] != 'd')
			Script_Add(sc, ms + (word[3] ? 0 : HOLD_MS), ACT_PAD, x, 0);
	}
	else if(strcmp(word, "stick") == 0)
	{
		int dur = -1, n = sscanf(args, "%31s %d %d %d", arg, &x, &y, &dur), which = strcmp(arg, "right") == 0;
		if(n < 3 || (!which && strcmp(arg, "left") != 0) || x < -100 || x > 100 || y < -100 || y > 100)
			return 0;
		Script_AddAt(sc, ms, ACT_STICK, which, 0, x * 32767 / 100, y * 32767 / 100);
		if(dur >= 0)
			Script_AddAt(sc, ms + (uint32_t)dur, ACT_STICK, which, 0, 0, 0);
	}
	else if(strcmp(word, "action") == 0 || strcmp(word, "actiondown") == 0 || strcmp(word, "actionup") == 0)
	{
		if(sscanf(args, "%31s", arg) != 1 || !(x = ActionCode(arg)))
			return 0;
		if(word[6] != 'u')
			Script_Add(sc, ms, ACT_ACTION, x, 1);
		if(word[6] != 'd')
			Script_Add(sc, ms + (word[6] ? 0 : HOLD_MS), ACT_ACTION, x, 0);
	}
	else if(strcmp(word, "shot") == 0)
		Script_Add(sc, ms, ACT_SHOT, 0, 0);
	else if(strcmp(word, "targets") == 0)
		Script_Add(sc, ms, ACT_TARGETS, 0, 0);
	else if(strcmp(word, "end") == 0)
		Script_Add(sc, ms, ACT_END, 0, 0);
	else
		return 0;
	return 1;
}

/* Reads the script at `path` into `sc`, sorted by time; 0 with a message
 * on stderr when a line is wrong.  A line's time is MS, or FROM-TO/EVERY
 * for the same action at FROM, FROM + EVERY, ... up to TO. */
static int Script_Load(Script_t* sc, const char* path)
{
	FILE* f = fopen(path, "r");
	char line[512];
	int n = 0, ok = 1;
	if(!f)
	{
		fprintf(stderr, "headless: cannot read %s\n", path);
		return 0;
	}
	while(fgets(line, sizeof line, f))
	{
		char when[64], word[32];
		unsigned long from, to, every;
		int used = 0, good;
		char* hash = strchr(line, '#');
		n++;
		if(hash)
			*hash = 0;
		if(sscanf(line, "%63s %31s %n", when, word, &used) < 2)
		{
			if(sscanf(line, "%63s", when) == 1)
				goto bad;
			continue; // empty
		}
		if(sscanf(when, "%lu-%lu/%lu", &from, &to, &every) == 3 && every > 0 && to >= from)
		{
			unsigned long t;
			good = 1;
			for(t = from; t <= to && good; t += every)
				good = Script_Line(sc, (uint32_t)t, word, line + used);
		}
		else
		{
			char* end;
			from = strtoul(when, &end, 10);
			good = !*end && Script_Line(sc, (uint32_t)from, word, line + used);
		}
		if(good)
			continue;
	bad:
		fprintf(stderr, "headless: %s:%d: not understood: %s", path, n, line);
		ok = 0;
	}
	fclose(f);
	if(sc->count)
		qsort(sc->acts, (size_t)sc->count, sizeof *sc->acts, ActionCmp);
	return ok;
}

// ---- the run -------------------------------------------------------------------------

typedef struct Options
{
	uint32_t runMs, stepMs, everyMs, refreshHz;
	const char* shots;
	const char* compare;
	const char* input;
	const char* wav;
	int targets;
	int reload; // --reload: the runs after the first
	const char* preview;
	const char* icon;
	char cmdLine[0x1000];
} Options_t;

static void ParseOptions(int argc, char** argv, Options_t* opt)
{
	int i;
	memset(opt, 0, sizeof *opt);
	opt->runMs = 30000;
	opt->stepMs = 1;
	opt->refreshHz = 60;
	opt->everyMs = 1000;
	for(i = 1; i < argc; i++)
	{
		const char* a = argv[i];
		if(strcmp(a, "--") == 0)
		{
			i++;
			break;
		}
		if(strncmp(a, "--run=", 6) == 0)
			opt->runMs = (uint32_t)strtoul(a + 6, NULL, 10);
		else if(strncmp(a, "--step=", 7) == 0)
			opt->stepMs = (uint32_t)strtoul(a + 7, NULL, 10);
		else if(strncmp(a, "--refresh=", 10) == 0)
			opt->refreshHz = (uint32_t)strtoul(a + 10, NULL, 10);
		else if(strncmp(a, "--every=", 8) == 0)
			opt->everyMs = (uint32_t)strtoul(a + 8, NULL, 10);
		else if(strncmp(a, "--shots=", 8) == 0)
			opt->shots = a + 8;
		else if(strncmp(a, "--compare=", 10) == 0)
			opt->compare = a + 10;
		else if(strncmp(a, "--input=", 8) == 0)
			opt->input = a + 8;
		else if(strncmp(a, "--wav=", 6) == 0)
			opt->wav = a + 6;
		else if(strcmp(a, "--targets") == 0)
			opt->targets = 1;
		else if(strncmp(a, "--reload=", 9) == 0)
			opt->reload = atoi(a + 9);
		else if(strncmp(a, "--preview=", 10) == 0 && strcmp(a, "--preview=none") != 0)
			opt->preview = a + 10;
		else if(strncmp(a, "--preview-icon=", 15) == 0)
			opt->icon = a + 15;
		else
			break; // the engine's command line begins
	}
	if(opt->stepMs == 0)
		opt->stepMs = 1;
	OS_JoinArgs(argc - i + 1, argv + i - 1, opt->cmdLine, sizeof opt->cmdLine);
}

// ---- the sound ---------------------------------------------------------------------------

#define SOUND_RATE     OPENBGI_AUDIO_RATE
#define SOUND_MAX      32768 // frames read at once at most (the engine's ring holds no more)
#define SOUND_SLICE_MS 500   // the longest stretch of the clock the engine mixes for at once

/* What the host heard: the engine's sound on the clock's grid of frames
 * (floor(ms * 44100 / 1000) frames lie before the clock's ms), silence
 * where there was none.  Each second of it is summed up in a line
 * "<second> <FNV-1a of its frames>" - the last, shorter one with its
 * frame count after the sum - written to DIR/sound.txt with --shots=DIR
 * and compared with the lines of DIR/sound.txt with --compare=DIR; --wav
 * writes all of it as a WAVE file. */
typedef struct Sound
{
	uint32_t next;     // the frame of the grid the next one heard is
	uint64_t sum;      // of the second's frames so far
	uint32_t inSecond; // frames of the second summed
	FILE* list;        // sound.txt being written, NULL for none
	char* base;        // the baseline's sound.txt (NUL-terminated), NULL without one
	size_t at;         // where in `base` the next second's line is
	uint32_t differs;  // 1 + the second the sound first differed in, 0 while the same
	FILE* wav;         // --wav, NULL for none
	int16_t* buf;      // SOUND_MAX frames
	uint64_t all;      // the sum of every frame of the run (--reload compares the runs by it)
} Sound_t;

#define FNV_OFFSET 0xcbf29ce484222325ull
#define FNV_PRIME  0x100000001b3ull

static uint32_t Sound_Grid(uint32_t ms)
{
	return (uint32_t)((uint64_t)ms * SOUND_RATE / 1000u);
}

// a second's line, written and compared; the next second begins
static void Sound_Line(Sound_t* s, uint32_t second, int last)
{
	char line[64];
	size_t n;
	if(last)
		snprintf(line, sizeof line, "%u %016llx %u\n", (unsigned)second, (unsigned long long)s->sum, (unsigned)s->inSecond);
	else
		snprintf(line, sizeof line, "%u %016llx\n", (unsigned)second, (unsigned long long)s->sum);
	if(s->list)
		fputs(line, s->list);
	n = strlen(line);
	if(s->base && !s->differs)
	{
		if(strncmp(s->base + s->at, line, n) != 0)
			s->differs = second + 1;
		else
			s->at += n;
	}
	s->sum = FNV_OFFSET;
	s->inSecond = 0;
}

// `n` frames heard at the end of what was (NULL: silence)
static void Sound_Add(Sound_t* s, const int16_t* f, uint32_t n)
{
	static const int16_t kSilence[1024 * 2];
	while(n > 0)
	{
		uint32_t k = SOUND_RATE - s->inSecond, i;
		const int16_t* p = f ? f : kSilence;
		if(k > n)
			k = n;
		if(!f && k > 1024)
			k = 1024;
		for(i = 0; i < k * 2; i++)
		{
			uint16_t v = (uint16_t)p[i];
			s->sum = (s->sum ^ (v & 0xff)) * FNV_PRIME;
			s->sum = (s->sum ^ (v >> 8)) * FNV_PRIME;
			s->all = (s->all ^ v) * FNV_PRIME;
		}
		if(s->wav)
			fwrite(p, 4, k, s->wav); // (little-endian hosts)
		s->inSecond += k;
		s->next += k;
		if(f)
			f += k * 2;
		n -= k;
		if(s->inSecond == SOUND_RATE)
			Sound_Line(s, s->next / SOUND_RATE - 1, 0);
	}
}

/* What the engine mixed up to the clock's `ms`, read out of it
 * (OpenBGI_AudioRead): silence up to where it begins, then its frames. */
static void Sound_Take(Sound_t* s, OpenBGI_t* o, uint32_t ms)
{
	uint32_t end = Sound_Grid(ms), got = 0, start;
	int n;
	while(got < SOUND_MAX && (n = OpenBGI_AudioRead(o, s->buf + (size_t)got * 2, (int)(SOUND_MAX - got))) > 0)
		got += (uint32_t)n;
	start = end - got;
	if((int32_t)(start - s->next) < 0)
	{
		fprintf(stderr, "headless: %u ms: %u frames of sound more than the time holds\n", (unsigned)ms, (unsigned)(s->next - start));
		start = s->next;
	}
	Sound_Add(s, NULL, start - s->next);
	Sound_Add(s, s->buf, got);
}

static void Sound_Begin(Sound_t* s, const Options_t* opt)
{
	char path[0x400];
	memset(s, 0, sizeof *s);
	s->sum = s->all = FNV_OFFSET;
	s->buf = (int16_t*)malloc((size_t)SOUND_MAX * 4);
	if(opt->shots)
	{
		snprintf(path, sizeof path, "%s/sound.txt", opt->shots);
		if(!(s->list = fopen(path, "w")))
			fprintf(stderr, "headless: cannot write %s\n", path);
	}
	if(opt->compare)
	{
		size_t n;
		snprintf(path, sizeof path, "%s/sound.txt", opt->compare);
		s->base = (char*)ReadFile(path, &n);
		if(s->base)
		{
			char* t = (char*)realloc(s->base, n + 1);
			if(t)
				t[n] = 0;
			s->base = t;
		}
	}
	if(opt->wav)
	{
		static const uint8_t kHeader[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
			0x44, 0xac, 0, 0, 0x10, 0xb1, 2, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0}; // 44100 Hz, 16-bit stereo
		if(!(s->wav = fopen(opt->wav, "wb")))
			fprintf(stderr, "headless: cannot write %s\n", opt->wav);
		else
			fwrite(kHeader, 1, sizeof kHeader, s->wav);
	}
}

// the last second's line, the files closed; what the log says of the sound with --compare
static const char* Sound_End(Sound_t* s, char* say, size_t n)
{
	if(s->inSecond)
		Sound_Line(s, s->next / SOUND_RATE, 1);
	if(s->list)
		fclose(s->list);
	if(s->wav)
	{
		uint32_t bytes = s->next * 4, riff = bytes + 36;
		uint8_t b[4];
		fseek(s->wav, 4, SEEK_SET);
		b[0] = (uint8_t)riff, b[1] = (uint8_t)(riff >> 8), b[2] = (uint8_t)(riff >> 16), b[3] = (uint8_t)(riff >> 24);
		fwrite(b, 1, 4, s->wav);
		fseek(s->wav, 40, SEEK_SET);
		b[0] = (uint8_t)bytes, b[1] = (uint8_t)(bytes >> 8), b[2] = (uint8_t)(bytes >> 16), b[3] = (uint8_t)(bytes >> 24);
		fwrite(b, 1, 4, s->wav);
		fclose(s->wav);
	}
	if(!s->base)
		snprintf(say, n, "no sound to compare with");
	else if(!s->differs && s->base[s->at] == 0)
		snprintf(say, n, "the sound the same");
	else if(!s->differs)
	{
		s->differs = s->next / SOUND_RATE + 1;
		snprintf(say, n, "the sound ends at %u s, earlier than the baseline's", (unsigned)(s->next / SOUND_RATE));
	}
	else
		snprintf(say, n, "the sound differs from %u s on", (unsigned)(s->differs - 1));
	free(s->base);
	free(s->buf);
	return say;
}

// ---- what a click reaches -------------------------------------------------------------

typedef struct Text // a growing string
{
	char* s;
	size_t n, cap;
} Text_t;

static void Text_Add(Text_t* t, const char* fmt, ...)
{
	char line[512];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	if(n < 0)
		return;
	if((size_t)n >= sizeof line)
		n = (int)sizeof line - 1;
	if(t->n + (size_t)n + 1 > t->cap)
	{
		size_t cap = t->cap ? t->cap * 2 : 0x10000;
		char* s;
		while(cap < t->n + (size_t)n + 1)
			cap *= 2;
		if(!(s = (char*)realloc(t->s, cap)))
			return;
		t->s = s;
		t->cap = cap;
	}
	memcpy(t->s + t->n, line, (size_t)n + 1);
	t->n += (size_t)n;
}

// the targets of the clock's `ms`, a line each (src/host/headless/main.c's head describes them)
static void Targets_At(Text_t* t, OpenBGI_t* o, uint32_t ms)
{
	static const char* const kKinds[] = {"?", "text", "wait", "layer", "raw", "button", "panel", "item", "icon", "scroll", "knob", "sprite", "probe", "entry", "hotspot"};
	static const char* const kFlags[] = {"anywhere", "disabled", "covered", "hover", "selected", "held", "probed"};
	OpenBGI_Target_t tg[256];
	int n = OpenBGI_Targets(o, tg, 256, sizeof(OpenBGI_Target_t)), i, k;
	if(n > 256)
		n = 256;
	if(n == 0)
		Text_Add(t, "%u none\n", (unsigned)ms);
	for(i = 0; i < n; i++)
	{
		const OpenBGI_Target_t* g = &tg[i];
		Text_Add(t, "%u %s %d,%d %dx%d at %d,%d owner %u %d/%d layer 0x%08x", (unsigned)ms,
			g->kind > 0 && g->kind < (int)(sizeof kKinds / sizeof kKinds[0]) ? kKinds[g->kind] : "?", g->rect.x, g->rect.y,
			g->rect.w, g->rect.h, g->clickX, g->clickY, (unsigned)g->owner, g->group, g->index, (unsigned)g->layer);
		if(g->kind == OPENBGI_TARGET_KNOB)
			Text_Add(t, " drag %d,%d %dx%d", g->drag.x, g->drag.y, g->drag.w, g->drag.h);
		for(k = 0; k < (int)(sizeof kFlags / sizeof kFlags[0]); k++)
			if(g->flags & (1u << k))
				Text_Add(t, " %s", kFlags[k]);
		Text_Add(t, "\n");
	}
}

/* the targets written to DIR/targets.txt (--shots) and compared with the
 * baseline's (--compare); what the log says of them, NULL without
 * --targets.  *differs is 1 when they differ from the baseline's. */
static const char* Targets_End(const Options_t* opt, Text_t* t, char* say, size_t n, int* differs)
{
	char path[0x400];
	*differs = 0;
	if(!opt->targets)
		return NULL;
	if(opt->shots)
	{
		snprintf(path, sizeof path, "%s/targets.txt", opt->shots);
		if(!WriteFile(path, t->s ? t->s : "", t->n))
			fprintf(stderr, "headless: cannot write %s\n", path);
	}
	snprintf(say, n, "no targets to compare with");
	if(opt->compare)
	{
		size_t len, i = 0, line = 0;
		uint8_t* base;
		snprintf(path, sizeof path, "%s/targets.txt", opt->compare);
		base = ReadFile(path, &len);
		if(base)
		{
			while(i < len && i < t->n && base[i] == (uint8_t)t->s[i])
			{
				if(base[i] == '\n')
					line = i + 1;
				i++;
			}
			if(i == len && i == t->n)
				snprintf(say, n, "the targets the same");
			else
			{
				*differs = 1;
				snprintf(say, n, "the targets differ from %lu ms on", line < t->n ? strtoul(t->s + line, NULL, 10) : strtoul((const char*)base + line, NULL, 10));
			}
			free(base);
		}
	}
	free(t->s);
	return say;
}

// ---- the run, continued ---------------------------------------------------------------

typedef struct Run
{
	OpenBGI_t* o;
	const Options_t* opt;
	Window_t win;
	int shots, differ;
	int pointerX, pointerY; // where the pointer is: the script's moves and the engine's (OpenBGI_Frame_t.pointerMoved)
	uint8_t keys[256];      // the keys the script holds down (a press of a held key is the auto-repeat's)
	OpenBGI_PadState_t pad; // the pad as the script holds it
	Text_t targets;         // --targets
	uint64_t pictures;      // the sum of the pictures taken, with their times (--reload compares the runs by it)
} Run_t;

// the picture of the clock's `ms`: written, compared
static void Run_Shot(Run_t* r, uint32_t ms)
{
	Shot_t s;
	char path[0x400];
	if(!Shot_Grab(&r->win, &s))
		return; // nothing shown yet
	r->shots++;
	{
		size_t i, n = (size_t)s.w * (size_t)s.h * 3;
		uint64_t sum = r->pictures ? r->pictures : FNV_OFFSET;
		sum = (sum ^ ms) * FNV_PRIME;
		for(i = 0; i < n; i++)
			sum = (sum ^ s.bgr[i]) * FNV_PRIME;
		r->pictures = sum;
	}
	if(r->opt->shots)
	{
		size_t n;
		uint8_t* png = Png_Encode(s.bgr, (uint32_t)s.w, (uint32_t)s.h, 3, (size_t)s.w * 3, 6, &n);
		snprintf(path, sizeof path, "%s/%09u.png", r->opt->shots, (unsigned)ms);
		if(!png || !WriteFile(path, png, n))
			fprintf(stderr, "headless: cannot write %s\n", path);
		free(png);
	}
	if(r->opt->compare)
	{
		long d;
		snprintf(path, sizeof path, "%s/%09u.png", r->opt->compare, (unsigned)ms);
		d = Shot_Compare(&s, path);
		if(d != 0)
		{
			r->differ++;
			if(d < 0)
				fprintf(stderr, "headless: %u ms: no picture to compare with (%s)\n", (unsigned)ms, path);
			else
				fprintf(stderr, "headless: %u ms: %ld pixels differ\n", (unsigned)ms, d);
		}
	}
	free(s.bgr);
}

/* The script's input up to the clock's `now`, handed to the engine as a
 * window would have it: a button where the pointer is, a key pressed
 * while it is down as the keyboard's auto-repeat. */
static void Deliver(Run_t* r, Script_t* in, uint32_t now)
{
	while(in->next < in->count && in->acts[in->next].ms <= now)
	{
		const Action_t* a = &in->acts[in->next++];
		switch(a->kind)
		{
			case ACT_KEY:
				OpenBGI_Key(r->o, a->a, a->b, a->b && r->keys[a->a] ? OPENBGI_KEY_REPEAT : 0);
				r->keys[a->a] = (uint8_t)a->b;
				break;
			case ACT_BUTTON: OpenBGI_Button(r->o, a->a, a->b, r->pointerX, r->pointerY, 0); break;
			case ACT_MOVE:
				r->pointerX = a->a;
				r->pointerY = a->b;
				OpenBGI_Pointer(r->o, a->a, a->b);
				break;
			case ACT_WHEEL: OpenBGI_Wheel(r->o, a->a); break;
			case ACT_RESIZE: OpenBGI_WindowSize(r->o, a->a, a->b); break;
			case ACT_TEXT: OpenBGI_Text(r->o, a->text); break;
			case ACT_TOUCH: OpenBGI_Touch(r->o, a->a, a->b, a->x, a->y); break;
			case ACT_SCALE: OpenBGI_TouchScale(r->o, a->a); break;
			case ACT_PAD:
			{
				uint32_t* bits = a->a < 16 ? &r->pad.buttons : &r->pad.dpad;
				uint32_t bit = 1u << (a->a & 15);
				*bits = a->b ? *bits | bit : *bits & ~bit;
				OpenBGI_Pad(r->o, 0, &r->pad);
				break;
			}
			case ACT_STICK:
				if(a->a)
				{
					r->pad.rightX = (int16_t)a->x;
					r->pad.rightY = (int16_t)a->y;
				}
				else
				{
					r->pad.leftX = (int16_t)a->x;
					r->pad.leftY = (int16_t)a->y;
				}
				OpenBGI_Pad(r->o, 0, &r->pad);
				break;
			case ACT_ACTION: OpenBGI_Action(r->o, a->a, a->b); break;
		}
	}
}

// the window's focus (it has it once shown) to the engine
// SIGTERM / SIGINT (the OS layer's pump): the run ends through the engine's shutdown
static void HostQuit(void* user)
{
	OpenBGI_Quit((OpenBGI_t*)user);
}

/* How far the clock moves after frame `f`: the wait it asked for, or the
 * step; one that presented a picture waits for the next vertical blank,
 * the first one after the clock's millisecond (blank k is at k * 1000 / HZ
 * ms, rounded up). */
static uint32_t Advance(const Options_t* opt, const OpenBGI_Frame_t* f)
{
	uint32_t d = (uint32_t)f->idleMs > opt->stepMs ? (uint32_t)f->idleMs : opt->stepMs;
	if(f->presented && opt->refreshHz)
	{
		uint64_t k = (uint64_t)gNow * opt->refreshHz / 1000u + 1;
		uint32_t blank = (uint32_t)((k * 1000u + opt->refreshHz - 1) / opt->refreshHz);
		if(blank - gNow > d)
			d = blank - gNow;
	}
	return d;
}

/* The sums of a run's pictures, sound and targets: two runs that gave
 * the same have the same. */
typedef struct Sums
{
	uint64_t pictures, sound, targets;
	int shots;
} Sums_t;

/* an engine of the test host's: with the launcher's program when the
 * build carries one (make headless LAUNCHER=1, the default), so that a
 * run without a game shows the launcher as the desktop programs do */
static OpenBGI_t* NewEngine(void)
{
	OpenBGI_t* o = OpenBGI_New(&gHost);
#ifdef BGI_LAUNCHER
	if(o)
		OpenBGI_SetLauncher(o, gOsLauncherSources, gOsLauncherSourcesSize);
#endif
	return o;
}

/* --preview: the hidden run of the desktop hosts, here on this thread;
 * its picture as a PNG.  The exit code. */
static int Preview(const Options_t* opt)
{
	OsPreviewResult_t pic;
	int never = 0, x, y, ok = 0;
	OsPreview_Run(opt->cmdLine, OPENBGI_PREVIEW_PICTURE | OPENBGI_PREVIEW_ICON, &never, &pic);
	if(opt->icon)
	{ // the icon the run found, as the .ico file it would be kept as
		FILE* fp = pic.icon ? fopen(opt->icon, "wb") : NULL;
		if(fp)
		{
			fwrite(pic.icon, 1, pic.iconSize, fp);
			fclose(fp);
		}
		else
			fprintf(stderr, "openbgi-headless: %s\n", pic.icon ? "the icon cannot be written" : "the game has no icon");
	}
	free(pic.icon);
	if(!pic.pixels)
	{
		fprintf(stderr, "openbgi-headless: the run gave no picture\n");
		return 1;
	}
	{ // B G R rows, as the encoder takes them
		uint8_t* bgr = (uint8_t*)malloc((size_t)pic.width * (size_t)pic.height * 3 + 1);
		size_t n = 0;
		uint8_t* png;
		for(y = 0; bgr && y < pic.height; y++)
			for(x = 0; x < pic.width; x++)
			{
				const uint8_t* row = pic.pixels + (size_t)y * (size_t)pic.pitch;
				uint8_t* d = bgr + ((size_t)y * (size_t)pic.width + (size_t)x) * 3;
				if(pic.bpp == 32)
				{
					d[0] = row[x * 4];
					d[1] = row[x * 4 + 1];
					d[2] = row[x * 4 + 2];
				}
				else
				{ // 5-5-5
					unsigned v = (unsigned)row[x * 2] | (unsigned)row[x * 2 + 1] << 8;
					d[0] = (uint8_t)((v & 31) * 255 / 31);
					d[1] = (uint8_t)((v >> 5 & 31) * 255 / 31);
					d[2] = (uint8_t)((v >> 10 & 31) * 255 / 31);
				}
			}
		png = bgr ? Png_Encode(bgr, (uint32_t)pic.width, (uint32_t)pic.height, 3, (size_t)pic.width * 3, 6, &n) : NULL;
		if(png)
		{
			FILE* fp = fopen(opt->preview, "wb");
			ok = fp && fwrite(png, 1, n, fp) == n;
			if(fp)
				fclose(fp);
		}
		free(png);
		free(bgr);
	}
	free(pic.pixels);
	if(!ok)
		fprintf(stderr, "openbgi-headless: %s: cannot be written\n", opt->preview);
	return ok ? 0 : 2;
}

int main(int argc, char** argv)
{
	Options_t opt, quiet;
	Script_t sc, in, pics;
	Run_t run;
	Sound_t snd;
	Sums_t first, sums;
	OpenBGI_Frame_t f = {.size = sizeof f};
	OpenBGI_t* o;
	uint32_t nextEvery, endMs, frames = 0, shown = 0;
	int code = 0, rc = OPENBGI_DONE, i, pass, targetsDiffer = 0, reloadsDiffer = 0, differ = 0, soundDiffers = 0;
	char said[96], saidTargets[96], dataDir[0x400];
	const char *targetsSaid = NULL, *dataBase, *what;

#ifdef BGI_TOOLS
	OS_SetArgv0(argv[0]);
	if(Tools_Run(argc, argv, &code)) // "openbgi-headless bpc ...": a tool of this build (tools/tools.h)
		return code;
#endif
	ParseOptions(argc, argv, &opt);
	memset(&sc, 0, sizeof sc);
	memset(&in, 0, sizeof in);
	memset(&pics, 0, sizeof pics);
	memset(&first, 0, sizeof first);
	memset(&run, 0, sizeof run);
	if(opt.input && !Script_Load(&sc, opt.input))
		return 2;
	// the script in three parts: the input, the pictures, the end
	endMs = opt.runMs;
	for(i = 0; i < sc.count; i++)
	{
		const Action_t* a = &sc.acts[i];
		if(a->kind == ACT_SHOT || a->kind == ACT_TARGETS)
			Script_Add(&pics, a->ms, a->kind, 0, 0);
		else if(a->kind == ACT_END)
			endMs = a->ms < endMs ? a->ms : endMs;
		else
		{
			Script_AddAt(&in, a->ms, a->kind, a->a, a->b, a->x, a->y);
			if(in.count)
				memcpy(in.acts[in.count - 1].text, a->text, sizeof a->text);
		}
	}
	free(sc.acts);

	OS_SetArgv0(argv[0]);
	gHost.fontFiles = OS_FontFiles();
	gHost.fontRenderer = OS_FontRenderer();
	gHost.files = OsCommon_Files();
	gHost.registry = OsCommon_Registry();
	gMachine = *OsCommon_Machine(); // (the language of the system it runs on is not the test's business)
	gMachine.uiLanguage = FixedUiLanguage;
	gHost.machine = &gMachine;
	gHost.shell = OsCommon_Shell();
	gHost.movieDecoder = OsCommon_MovieDecoder();
	o = NewEngine();
	if(!o || !OS_Init())
		return 2;
	if(opt.preview)
		return Preview(&opt);
	quiet = opt; // the runs after the first write and compare nothing: their sums are compared
	quiet.shots = quiet.compare = quiet.wav = NULL;
	dataBase = opt.reload > 0 ? getenv("BGI_DATA") : NULL;
	if(dataBase && !*dataBase)
		dataBase = NULL;
	if(dataBase)
		dataBase = strcpy((char*)malloc(strlen(dataBase) + 1), dataBase);
	for(pass = 0; pass <= opt.reload; pass++)
	{
		const Options_t* op = pass == 0 ? &opt : &quiet;
		if(pass > 0 && pass % 2 == 0)
		{ // every second time a new engine; in between the same one is started again
			OpenBGI_Delete(o);
			o = NewEngine();
			if(!o)
				return 2;
		}
		if(dataBase)
		{ // a game in a zip: what it writes in a folder of this run's own
			snprintf(dataDir, sizeof dataDir, "%s/run%d", dataBase, pass);
			setenv("BGI_DATA", dataDir, 1);
		}
		gNow = 0;
		OsHeadless_SetNow(0);
		free(run.win.px);
		free(run.targets.s);
		memset(&run, 0, sizeof run);
		run.pad.size = sizeof run.pad;
		run.o = o;
		run.opt = op;
		in.next = pics.next = 0;
		frames = shown = 0;
		{
			OsHostHandlers_t ih;
			memset(&ih, 0, sizeof ih);
			ih.quit = HostQuit;
			ih.user = run.o;
			OS_SetHostHandlers(&ih);
		}
		nextEvery = opt.everyMs ? opt.everyMs : UINT32_MAX;
		Sound_Begin(&snd, op);
		rc = OpenBGI_Start(run.o, opt.cmdLine);
		while(rc == OPENBGI_RUNNING && gNow < endMs)
		{
			uint32_t d;
			OS_AnswerRequests(run.o);
			OpenBGI_PointerPlace(run.o, run.pointerX, run.pointerY); // where the pointer is, as a host says before each frame (a move is the script's)
			rc = OpenBGI_RunFrame(run.o, &f);                        // (it mixes the sound up to the clock first)
			frames++;
			Sound_Take(&snd, run.o, gNow);
			if(rc != OPENBGI_RUNNING)
				break;
			shown += f.presented != 0;
			Window_Update(&run.win, &f); // the window shows the frame
			if(f.pointerMoved)
			{ // the engine moved the pointer
				run.pointerX = f.pointerX;
				run.pointerY = f.pointerY;
			}
			Deliver(&run, &in, gNow); // the input due by now, between this frame and the next
			while(OS_PumpMessages())
				; // (a signal's quit)
			OpenBGI_PumpEvents(run.o);
			for(d = Advance(&opt, &f); d > SOUND_SLICE_MS; d -= SOUND_SLICE_MS)
			{ // a long wait: the sound mixed as it goes, as a host's output would have it
				gNow += SOUND_SLICE_MS;
				OsHeadless_SetNow(gNow);
				OpenBGI_RunAudio(run.o);
				Sound_Take(&snd, run.o, gNow);
			}
			gNow += d;
			OsHeadless_SetNow(gNow);
			for(;;)
			{ // the pictures (and targets) due: of --every and of the script, in time order, each time once
				uint32_t t = nextEvery;
				int shot, targets;
				if(pics.next < pics.count && pics.acts[pics.next].ms < t)
					t = pics.acts[pics.next].ms;
				if(t > gNow || t > endMs)
					break;
				shot = nextEvery == t;
				targets = 0;
				for(; pics.next < pics.count && pics.acts[pics.next].ms == t; pics.next++)
				{
					if(pics.acts[pics.next].kind == ACT_SHOT)
						shot = 1;
					else
						targets = 1;
				}
				if(shot)
					Run_Shot(&run, t);
				if(opt.targets && (shot || targets))
					Targets_At(&run.targets, run.o, t);
				if(nextEvery == t)
					nextEvery += opt.everyMs;
			}
		}
		if(rc == OPENBGI_RUNNING)
		{ // the sound up to the end of the run
			OpenBGI_RunAudio(run.o);
			Sound_Take(&snd, run.o, gNow);
		}
		if(opt.reload > 0)
			OpenBGI_Pad(run.o, 0, NULL); // (the script's pad is unplugged: the next run starts without one, as the first did)
		code = OpenBGI_Stop(run.o);
		OS_AnswerRequests(run.o);
		sums.pictures = run.pictures;
		sums.shots = run.shots;
		sums.sound = snd.all;
		sums.targets = FNV_OFFSET;
		for(i = 0; i < (int)run.targets.n; i++)
			sums.targets = (sums.targets ^ (uint8_t)run.targets.s[i]) * FNV_PRIME;
		if(pass == 0)
		{
			first = sums;
			differ = run.differ;
			Sound_End(&snd, said, sizeof said);
			soundDiffers = snd.differs != 0;
			targetsSaid = Targets_End(&opt, &run.targets, saidTargets, sizeof saidTargets, &targetsDiffer);
			memset(&run.targets, 0, sizeof run.targets); // (freed there)
			fprintf(stderr, "headless: %s after %u ms (%u frames, %u presented), exit code %d; %d pictures", rc == OPENBGI_RUNNING ? "stopped" : "the run ended",
				(unsigned)gNow, (unsigned)frames, (unsigned)shown, code, run.shots);
			if(opt.compare)
				fprintf(stderr, ", %d differ, %s", run.differ, said);
			if(opt.compare && targetsSaid)
				fprintf(stderr, ", %s", targetsSaid);
			fprintf(stderr, "\n");
		}
		else
		{
			int same = sums.shots == first.shots && sums.pictures == first.pictures && sums.sound == first.sound && sums.targets == first.targets;
			Sound_End(&snd, said, sizeof said);
			reloadsDiffer += !same;
			what = "as the first";
			if(sums.shots != first.shots || sums.pictures != first.pictures)
				what = "its pictures differ from the first run's";
			else if(sums.sound != first.sound)
				what = "its sound differs from the first run's";
			else if(sums.targets != first.targets)
				what = "its targets differ from the first run's";
			fprintf(stderr, "headless: run %d (%s): %u frames, %d pictures; %s\n", pass + 1, pass % 2 ? "the same engine again" : "a new engine", (unsigned)frames,
				run.shots, what);
		}
	}
	OpenBGI_Delete(o);
	OS_Shutdown();
	free(in.acts);
	free(pics.acts);
	free(run.win.px);
	free(run.targets.s);
	free((void*)dataBase);
	if(opt.reload > 0)
		fprintf(stderr, "headless: %d more run%s, %d differ from the first\n", opt.reload, opt.reload == 1 ? "" : "s", reloadsDiffer);
	return (opt.compare && (differ || soundDiffers || targetsDiffer)) || reloadsDiffer ? 1 : 0;
}
