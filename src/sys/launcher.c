/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * launcher.c - the launcher's side of the engine (src/include/bgi/launcher.h):
 *              the folders and paths of the host it reads, what it
 *              learns of a game, the start of one, the picture and
 *              play time kept of a game it started, and what it gathers
 *              of a game it lists for the first time: its program's icon
 *              and, with a host that runs it out of sight, a first
 *              picture (its program, and the boot of it, are
 *              launcher_src.c's)
 */
#include "bgi/launcher.h"
#include "bgi/release.h"
#include "bgi/msg.h"
#include "bgi/engine.h"
#include "bgi/version.h"
#include "bgi/detect.h"
#include "bgi/display.h"
#include "bgi/host.h"
#include "bgi/file.h"
#include "bgi/fs.h"
#include "bgi/icon.h"
#include "bgi/vol.h"
#include "bgi/png.h"
#include "bgi/request.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/waitobj.h"
#include "bgi/core.h"

#include <ctype.h>

// ---- folders ---------------------------------------------------------------------------------

typedef struct DirEntry
{
	char* name; // UTF-8
	uint8_t folder;
	uint8_t hidden;
} DirEntry_t;

typedef struct LauncherDir
{
	OpenBGI_t* o;
	DirEntry_t* e;
	int count, cap;
	int hidden; // hidden entries are listed too
} LauncherDir_t;

static int IsHidden(const char* name)
{
	return name[0] == '.' || name[0] == '$';
}

static void DirAdd(LauncherDir_t* d, const char* utf8, int folder)
{
	int hidden = IsHidden(utf8);
	if((hidden && !d->hidden) || !utf8[0])
		return;
	if(d->count == d->cap)
	{
		int cap = d->cap ? d->cap * 2 : 64;
		DirEntry_t* grown = (DirEntry_t*)realloc(d->e, sizeof *grown * (size_t)cap);
		if(!grown)
			return;
		d->e = grown;
		d->cap = cap;
	}
	d->e[d->count].name = BGI_Strdup(utf8);
	d->e[d->count].folder = (uint8_t)(folder != 0);
	d->e[d->count].hidden = (uint8_t)hidden;
	d->count++;
}

static void DirListed(void* ctx, const char* name, int isDir)
{
	LauncherDir_t* d = (LauncherDir_t*)ctx;
	char utf8[LAUNCHER_PATH];
	Host_NativeToUtf8(d->o, name, utf8, sizeof utf8);
	DirAdd(d, utf8, isDir);
}

// folders before files, the hidden ones of each after the others, then by name without regard to the case of ASCII letters
static int DirOrder(const void* pa, const void* pb)
{
	const DirEntry_t* a = (const DirEntry_t*)pa;
	const DirEntry_t* b = (const DirEntry_t*)pb;
	const uint8_t *x = (const uint8_t*)a->name, *y = (const uint8_t*)b->name;
	if(a->folder != b->folder)
		return a->folder ? -1 : 1;
	if(a->hidden != b->hidden)
		return a->hidden ? 1 : -1;
	for(; *x && *y; x++, y++)
	{
		int cx = *x < 0x80 ? tolower(*x) : *x, cy = *y < 0x80 ? tolower(*y) : *y;
		if(cx != cy)
			return cx - cy;
	}
	if(*x != *y)
		return *x ? 1 : -1;
	return strcmp(a->name, b->name);
}

static void DirFree(LauncherDir_t* d)
{
	int i;
	if(!d)
		return;
	for(i = 0; i < d->count; i++)
		BGI_Free(d->e[i].name);
	free(d->e);
	free(d);
}

// the listing of the folder at `path` as the host of `o` reads it; NULL when it cannot be read
static LauncherDir_t* DirList(OpenBGI_t* o, const char* path, int hidden)
{
	LauncherDir_t* d = (LauncherDir_t*)calloc(1, sizeof *d);
	char native[LAUNCHER_PATH];
	if(!d)
		return NULL;
	d->o = o;
	d->hidden = hidden;
	Host_Utf8ToNative(o, path[0] ? path : "/", native, sizeof native);
	if(Host_NativeList(o, native, DirListed, d) < 0)
	{
		DirFree(d);
		return NULL;
	}
	if(d->count)
		qsort(d->e, (size_t)d->count, sizeof *d->e, DirOrder);
	return d;
}

// a handle for the listing `d` (which may be NULL); 0, and the listing freed, when none is free
static int DirHandle(OpenBGI_t* o, LauncherDir_t* d)
{
	int h;
	for(h = 0; h < LAUNCHER_DIRS && o->launcher.dirs[h]; h++)
		;
	if(!d || h == LAUNCHER_DIRS)
	{
		DirFree(d);
		return 0;
	}
	d->o = o;
	o->launcher.dirs[h] = d;
	return h + 1;
}

static int CopyText(char* dst, size_t size, const char* s);
static int IsSep(OpenBGI_t* o, char c);

/* The host's roots (Host_NativeRoots; openbgi.h, OpenBGI_Files_t.nativeRoots):
 * number i's path, without a separator at its end, and its name for the
 * player, the path where the host gave none - both UTF-8, either buffer
 * may be NULL.  0 when there is no such root. */
static int Root(OpenBGI_t* o, int i, char* path, size_t pathSize, char* label, size_t labelSize)
{
	char list[LAUNCHER_ROOTS_TEXT], utf8[LAUNCHER_PATH], name[LAUNCHER_PATH];
	const char* p;
	size_t n;
	if(i < 0 || !Host_NativeRoots(o, list, (uint32_t)sizeof list))
		return 0;
	for(p = list; *p; p += strlen(p) + 1, i--)
	{
		const char* text = p + strlen(p) + 1;
		if(i > 0)
		{
			p = text;
			continue;
		}
		Host_NativeToUtf8(o, p, utf8, sizeof utf8);
		for(n = strlen(utf8); n > 1 && IsSep(o, utf8[n - 1]); n--)
			utf8[n - 1] = 0;
		Host_NativeToUtf8(o, text, name, sizeof name);
		if(path)
			CopyText(path, pathSize, utf8);
		if(label)
			CopyText(label, labelSize, name[0] ? name : utf8);
		return 1;
	}
	return 0;
}

int Launcher_RootValue(OpenBGI_t* o, const char* key, int i, char* dst, size_t size)
{
	int names = strcmp(key, "root_names") == 0;
	if(!names && strcmp(key, "roots") != 0)
		return 0;
	if(size)
		dst[0] = 0;
	return Root(o, i, names ? NULL : dst, size, names ? dst : NULL, size) ? 1 : -1;
}

int Launcher_TopKnown(OpenBGI_t* o)
{
	return Host_NativeSep(o) == '\\' || Root(o, 0, NULL, 0, NULL, 0);
}

int Launcher_DirOpen(OpenBGI_t* o, const char* path, int flags)
{
	LauncherDir_t* d;
	if(!path[0] && Host_NativeSep(o) != '\\')
	{ // above everything, where the host names roots: those, in its order
		char root[LAUNCHER_PATH];
		int i;
		if(Root(o, 0, root, sizeof root, NULL, 0))
		{
			d = (LauncherDir_t*)calloc(1, sizeof *d);
			if(!d)
				return 0;
			d->hidden = 1; // (a root is never one)
			for(i = 0; Root(o, i, root, sizeof root, NULL, 0); i++)
				DirAdd(d, root, 1);
			if(!d->count)
			{
				DirFree(d);
				return 0;
			}
			return DirHandle(o, d);
		}
	}
	if(!path[0] && Host_NativeSep(o) == '\\')
	{ // the computer: its drives
		char drives[0x200];
		const char* p;
		d = (LauncherDir_t*)calloc(1, sizeof *d);
		if(!d)
			return 0;
		memset(drives, 0, sizeof drives);
		Host_LogicalDrives(o, drives, (uint32_t)sizeof drives - 2);
		d->hidden = 1; // (a drive is never one)
		for(p = drives; *p; p += strlen(p) + 1)
			DirAdd(d, p, 1);
		if(!d->count)
		{
			DirFree(d);
			return 0;
		}
		qsort(d->e, (size_t)d->count, sizeof *d->e, DirOrder);
		return DirHandle(o, d);
	}
	return DirHandle(o, DirList(o, path, flags & LAUNCHER_DIR_HIDDEN));
}

static LauncherDir_t* DirOf(OpenBGI_t* o, int h)
{
	return h >= 1 && h <= LAUNCHER_DIRS ? o->launcher.dirs[h - 1] : NULL;
}

int Launcher_DirCount(OpenBGI_t* o, int h)
{
	LauncherDir_t* d = DirOf(o, h);
	return d ? d->count : 0;
}

// a string into a buffer, cut between two characters of UTF-8 when it is too long; its length there
static int CopyText(char* dst, size_t size, const char* s)
{
	size_t n = strlen(s);
	if(!size)
		return 0;
	if(n >= size)
	{
		n = size - 1;
		while(n > 0 && ((uint8_t)s[n] & 0xc0) == 0x80)
			n--;
	}
	memcpy(dst, s, n);
	dst[n] = 0;
	return (int)n;
}

int Launcher_DirEntry(OpenBGI_t* o, int h, int i, char* dst, size_t size)
{
	LauncherDir_t* d = DirOf(o, h);
	if(size)
		dst[0] = 0;
	if(!d || i < 0 || i >= d->count)
		return 0;
	CopyText(dst, size, d->e[i].name);
	return d->e[i].folder ? LAUNCHER_ENTRY_FOLDER : LAUNCHER_ENTRY_FILE;
}

void Launcher_DirClose(OpenBGI_t* o, int h)
{
	if(h >= 1 && h <= LAUNCHER_DIRS)
	{
		DirFree(o->launcher.dirs[h - 1]);
		o->launcher.dirs[h - 1] = NULL;
	}
}

void Launcher_DirCloseAll(OpenBGI_t* o)
{
	int h;
	for(h = 1; h <= LAUNCHER_DIRS; h++)
		Launcher_DirClose(o, h);
}

// ---- paths -----------------------------------------------------------------------------------

static int IsSep(OpenBGI_t* o, char c)
{
	return c == '/' || (c == '\\' && Host_NativeSep(o) == '\\');
}

// a drive's root, "C:" or "C:\", where paths have drives
static int IsDriveRoot(OpenBGI_t* o, const char* p, size_t n)
{
	return Host_NativeSep(o) == '\\' && n >= 2 && n <= 3 && p[1] == ':' && (n == 2 || IsSep(o, p[2]));
}

// the folder above `dir` into buf (LAUNCHER_PATH bytes)
static void Parent(OpenBGI_t* o, const char* dir, char* buf)
{
	size_t n = strlen(dir), k;
	if(n >= LAUNCHER_PATH)
		n = LAUNCHER_PATH - 1;
	memcpy(buf, dir, n);
	while(n > 1 && IsSep(o, buf[n - 1]) && !IsDriveRoot(o, buf, n))
		n--;
	buf[n] = 0;
	if(n == 0 || IsDriveRoot(o, buf, n))
	{ // above a drive: the computer
		buf[0] = 0;
		return;
	}
	for(k = n; k > 0 && !IsSep(o, buf[k - 1]); k--)
		;
	if(k == 0)
		buf[0] = 0; // a name alone has nothing above it
	else if(k == 1 || IsDriveRoot(o, buf, k))
		buf[k] = 0; // the root, with its separator
	else
		buf[k - 1] = 0;
}

int Launcher_HostValue(OpenBGI_t* o, const char* key, char* dst, size_t size)
{
	char native[LAUNCHER_PATH], utf8[LAUNCHER_PATH], up[LAUNCHER_PATH];
	if(strcmp(key, "separator") == 0)
	{
		char sep[2] = {Host_NativeSep(o), 0};
		CopyText(dst, size, sep);
		return 1;
	}
	if(strcmp(key, "language") == 0)
	{
		CopyText(dst, size, gMsgEnglish ? "en" : "ja");
		return 1;
	}
	if(strcmp(key, "version") == 0)
	{ // OpenBGI's own, as --version says it
		CopyText(dst, size, OPENBGI_VERSION);
		return 1;
	}
	if(strcmp(key, "home") != 0)
		return 0;
	// the folder the user's documents are in
	native[0] = 0;
	up[0] = 0;
	if(Host_SpecialFolder(o, 5, native, sizeof native) && native[0])
	{
		Host_NativeToUtf8(o, native, utf8, sizeof utf8);
		Parent(o, utf8, up);
	}
	CopyText(dst, size, up);
	return 1;
}

// ---- what was dropped on the window ----------------------------------------------------------

static void DropsFree(OpenBGI_t* o)
{
	int i;
	for(i = 0; i < o->launcher.droppedCount; i++)
		BGI_Free(o->launcher.dropped[i]);
	o->launcher.droppedCount = 0;
}

void Launcher_Drop(OpenBGI_t* o, const char* native)
{
	char utf8[LAUNCHER_PATH];
	size_t n;
	if(!o->launcher.running || o->launcher.droppedCount >= LAUNCHER_DROPS || !native[0] || strlen(native) >= LAUNCHER_PATH / 2)
		return;
	Host_NativeToUtf8(o, native, utf8, sizeof utf8);
	n = strlen(utf8);
	while(n > 1 && IsSep(o, utf8[n - 1]) && !IsDriveRoot(o, utf8, n))
		utf8[--n] = 0; // a folder without the separator after it, as the browser names one
	if(n)
		o->launcher.dropped[o->launcher.droppedCount++] = BGI_Strdup(utf8);
}

int Launcher_Dropped(OpenBGI_t* o, int i, char* dst, size_t size)
{
	if(i < 0 || i >= o->launcher.droppedCount)
		return -1;
	return CopyText(dst, size, o->launcher.dropped[i]);
}

int Launcher_DropTake(OpenBGI_t* o, int i)
{
	if(i < 0 || i >= o->launcher.droppedCount)
		return 0;
	BGI_Free(o->launcher.dropped[i]);
	o->launcher.droppedCount--;
	memmove(&o->launcher.dropped[i], &o->launcher.dropped[i + 1], sizeof o->launcher.dropped[0] * (size_t)(o->launcher.droppedCount - i));
	return 1;
}

// ---- what a path holds ---------------------------------------------------------------------

/* A product name as a name to show: the words of "HanairoHeptagram" or
 * "Gackoh_HD" apart.  ASCII only; anything else is copied. */
static void WordsOf(const char* s, char* out, size_t n)
{
	size_t k = 0, i;
	for(i = 0; s[i] && k + 2 < n; i++)
	{
		unsigned char c = (unsigned char)s[i], p = i ? (unsigned char)s[i - 1] : 0, x = (unsigned char)s[i + 1];
		if(c == '_')
		{
			out[k++] = ' ';
			continue;
		}
		if(i && p != '_' && p != ' ' && c < 0x80 && p < 0x80 &&
			((isupper(c) && (islower(p) || isdigit(p))) || (isupper(c) && isupper(p) && x < 0x80 && islower(x)) || (isdigit(c) && isalpha(p))))
			out[k++] = ' ';
		out[k++] = (char)c;
	}
	out[k] = 0;
}

// 1 when the native file starts as a zip archive does
static int IsZip(OpenBGI_t* o, const char* native)
{
	HostFile_t* f = Host_NativeOpen(o, native, OPENBGI_OPEN_READ);
	uint8_t sig[4] = {0, 0, 0, 0};
	if(!f)
		return 0;
	Host_FileRead(o, f, sig, 4);
	Host_FileClose(o, f);
	return sig[0] == 'P' && sig[1] == 'K';
}

int Launcher_Probe(OpenBGI_t* o, const char* path, LauncherGame_t* out)
{
	char native[LAUNCHER_PATH];
	DetectSet_t set;
	DetectResult_t r;
	EngineGame_t g;
	int kind;
	memset(out, 0, sizeof *out);
	if(!path[0] || strlen(path) >= LAUNCHER_PATH)
		return 0;
	Host_Utf8ToNative(o, path, native, sizeof native);
	kind = Host_NativeStat(o, native, NULL);
	DetectSet_Init(&set);
	if(kind == OPENBGI_NATIVE_DIR)
	{
		DetectSet_LoadDirectory(o, &set, native);
		out->kind = LAUNCHER_GAME_FOLDER;
	}
	else if(kind == OPENBGI_NATIVE_FILE && !Fs_Mounted(o) && Fs_IsArchive(o, native))
	{ // the archive stands in for the game directory while its programs are read
		out->kind = IsZip(o, native) ? LAUNCHER_GAME_ZIP : LAUNCHER_GAME_ISO;
		if(Fs_MountArchive(o, native))
		{
			DetectSet_LoadGame(o, &set, o->files.baseDir);
			Fs_Unmount(o);
		}
	}
	if(!set.haveIpl)
	{ // no boot program: nothing the engine could start
		DetectSet_Free(&set);
		memset(out, 0, sizeof *out);
		return 0;
	}
	Detect_Game(o, &set, NULL, "", &r);
	DetectSet_Free(&set);
	snprintf(out->profile, sizeof out->profile, "%s", r.profile);
	CopyText(out->product, sizeof out->product, r.product);
	if(r.entry >= 0 && Engine_GameAt(o, r.entry, &g) && g.name[0])
	{
		if(g.product && strcmp(g.name, g.product) == 0)
			WordsOf(g.product, out->name, sizeof out->name);
		else
			CopyText(out->name, sizeof out->name, g.name);
	}
	else if(r.product[0])
		WordsOf(r.product, out->name, sizeof out->name);
	return 1;
}

// ---- starting a game -------------------------------------------------------------------------

// text to the end of a command line of n bytes; 0 when it does not fit
static int CmdAdd(char* cmd, size_t n, const char* fmt, const char* text)
{
	size_t len = strlen(cmd);
	int w = snprintf(cmd + len, n - len, fmt, text);
	return w >= 0 && (size_t)w < n - len;
}

int Launcher_Launch(OpenBGI_t* o, const char* path, const char* profile, const char* patches, const char* options)
{
	char cmd[0x1800], native[LAUNCHER_PATH], when[32];
	LauncherGame_t game;
	OpenBGI_DateTime_t dt;
	const char* p;
	int ok = 1;
	if(o->launcher.relaunch || strchr(path, '"') || !Launcher_Probe(o, path, &game))
		return 0;
	cmd[0] = 0;
	if(o->engine.optGames[0] && !strchr(o->engine.optGames, '"'))
		ok &= CmdAdd(cmd, sizeof cmd, "--games=\"%s\" ", o->engine.optGames);
	ok &= CmdAdd(cmd, sizeof cmd, "--lang=%s ", gMsgEnglish ? "en" : "ja"); // the language stays; the game's own options come after it
	if(profile && profile[0] && !strpbrk(profile, "\" "))
		ok &= CmdAdd(cmd, sizeof cmd, "--engine=%s ", profile);
	for(p = patches; p && *p; p += strlen(p) + 1)
	{
		if(strchr(p, '"') || strlen(p) >= LAUNCHER_PATH)
			return 0;
		Host_Utf8ToNative(o, p, native, sizeof native);
		ok &= CmdAdd(cmd, sizeof cmd, "--patch=\"%s\" ", native);
	}
	if(options && options[0])
		ok &= CmdAdd(cmd, sizeof cmd, "%s ", options);
	Host_Utf8ToNative(o, path, native, sizeof native);
	ok &= CmdAdd(cmd, sizeof cmd, "\"%s\"", native);
	if(!ok)
		return 0;
	// when it was played, for the launcher's list
	o->host.localTime(o->host.user, &dt);
	snprintf(when, sizeof when, "%04u-%02u-%02u %02u:%02u", (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day, (unsigned)dt.hour,
		(unsigned)dt.minute);
	Engine_CfgSet(o, path, "played", 0, when);
	Engine_SaveGames(o);
	o->launcher.cfgChanged = 0;
	snprintf(o->launcher.gamePath, sizeof o->launcher.gamePath, "%s", path);
	snprintf(o->launcher.gamesPath, sizeof o->launcher.gamesPath, "%s", o->engine.gamesPath);
	if(Engine_CfgGet(o, path, "icon", 0, o->launcher.gameIcon, sizeof o->launcher.gameIcon) <= 0)
		o->launcher.gameIcon[0] = 0;
	o->launcher.relaunch = BGI_Strdup(cmd);
	// the way back: this run's own command line and the directory it began in, for the run that follows the game's (Launcher_Return)
	BGI_Free(o->launcher.back);
	o->launcher.back = BGI_Strdup(o->engine.startCmd ? o->engine.startCmd : "");
	snprintf(o->launcher.backDir, sizeof o->launcher.backDir, "%s", o->engine.startDir);
	return 1;
}

int Launcher_Relaunch(OpenBGI_t* o)
{
	char* cmd = o->launcher.relaunch;
	int r;
	o->launcher.relaunch = NULL;
	OpenBGI_Stop(o);
	MainWin_Destroy(o);       // the launcher's window goes with its run: the game's run makes a window of its own
	o->launcher.launched = 1; // (kept by the start: Engine_Renew)
	fprintf(stderr, "openbgi: starting %s\n", o->launcher.gamePath);
	r = OpenBGI_Start(o, cmd);
	o->launcher.startMs = o->host.ticksMs(o->host.user);
	o->launcher.shotMs = o->launcher.startMs + LAUNCHER_SHOT_1ST;
	BGI_Free(cmd);
	return r;
}

int Launcher_Returns(OpenBGI_t* o)
{
	return o->launcher.launched && o->launcher.back && !o->launcher.hostQuit;
}

int Launcher_Return(OpenBGI_t* o)
{
	char* cmd = o->launcher.back;
	int r;
	o->launcher.back = NULL;
	OpenBGI_Stop(o);    // the game's run: its play time and its last picture are noted with it (Launcher_GameEnded)
	MainWin_Destroy(o); // a window the game left (a run that ended in an error): the launcher makes its own
	fprintf(stderr, "openbgi: back to the launcher\n");
	// the start is the launcher's own once more: the same command line where it was given first - not in the
	// game's directory, where the engine would find the game to run and its settings file
	if(o->launcher.backDir[0])
		Host_SetGameDir(o, o->launcher.backDir);
	r = OpenBGI_Start(o, cmd);
	BGI_Free(cmd);
	return r;
}

// ---- the picture and the play time of a game ----------------------------------------------

// the red, green and blue of a pixel of a frame's picture (16 bits: 5-5-5; 32: B G R x)
static void FramePixel(const OpenBGI_Frame_t* f, int x, int y, int* rgb)
{
	const uint8_t* p = f->pixels + (size_t)y * (size_t)f->pitch;
	if(f->bpp == 16)
	{
		unsigned v = (unsigned)p[x * 2] | (unsigned)p[x * 2 + 1] << 8;
		rgb[0] = (int)((v >> 10 & 31) * 255 / 31);
		rgb[1] = (int)((v >> 5 & 31) * 255 / 31);
		rgb[2] = (int)((v & 31) * 255 / 31);
	}
	else
	{
		rgb[0] = p[x * 4 + 2];
		rgb[1] = p[x * 4 + 1];
		rgb[2] = p[x * 4];
	}
}

// the size of w x h scaled to fit maxW x maxH with its aspect kept (never larger than it is when `shrinkOnly`)
static void FitSize(int w, int h, int maxW, int maxH, int shrinkOnly, int* tw, int* th)
{
	int64_t a = (int64_t)maxW * h, b = (int64_t)maxH * w;
	if(shrinkOnly && w <= maxW && h <= maxH)
	{
		*tw = w;
		*th = h;
	}
	else if(a <= b)
	{ // the width decides
		*tw = maxW;
		*th = (int)(((int64_t)h * maxW + w / 2) / w);
	}
	else
	{
		*th = maxH;
		*tw = (int)(((int64_t)w * maxH + h / 2) / h);
	}
	if(*tw < 1)
		*tw = 1;
	if(*th < 1)
		*th = 1;
}

/* The picture of frame `f` made small for the list: its pixels (B G R x,
 * *tw x *th of them, free() them), or NULL for a picture that says
 * nothing of the game - black, a fade, one colour - and when memory is
 * short. */
static uint8_t* SmallPicture(const OpenBGI_Frame_t* f, int* tw, int* th)
{
	int x, y, lo = 255, hi = 0;
	uint8_t* shot;
	FitSize(f->width, f->height, LAUNCHER_SHOT_W, LAUNCHER_SHOT_H, 1, tw, th);
	shot = (uint8_t*)malloc((size_t)*tw * (size_t)*th * 4);
	if(!shot)
		return NULL;
	for(y = 0; y < *th; y++)
	{
		int y0 = (int)((int64_t)y * f->height / *th), y1 = (int)((int64_t)(y + 1) * f->height / *th);
		for(x = 0; x < *tw; x++)
		{
			int x0 = (int)((int64_t)x * f->width / *tw), x1 = (int)((int64_t)(x + 1) * f->width / *tw);
			int sum[3] = {0, 0, 0}, n = 0, sx, sy, rgb[3], luma;
			for(sy = y0; sy < y1 || sy == y0; sy++)
				for(sx = x0; sx < x1 || sx == x0; sx++)
				{
					FramePixel(f, sx, sy, rgb);
					sum[0] += rgb[0];
					sum[1] += rgb[1];
					sum[2] += rgb[2];
					n++;
				}
			shot[((size_t)y * (size_t)*tw + (size_t)x) * 4] = (uint8_t)(sum[2] / n);
			shot[((size_t)y * (size_t)*tw + (size_t)x) * 4 + 1] = (uint8_t)(sum[1] / n);
			shot[((size_t)y * (size_t)*tw + (size_t)x) * 4 + 2] = (uint8_t)(sum[0] / n);
			shot[((size_t)y * (size_t)*tw + (size_t)x) * 4 + 3] = 0xff;
			luma = (sum[0] * 2 + sum[1] * 5 + sum[2]) / (n * 8);
			if(luma < lo)
				lo = luma;
			if(luma > hi)
				hi = luma;
		}
	}
	if(hi - lo < 32)
	{
		free(shot);
		return NULL;
	}
	return shot;
}

static int HasPicture(const OpenBGI_Frame_t* f)
{
	return f->pixels && f->width >= 16 && f->height >= 16 && (f->bpp == 16 || f->bpp == 32);
}

void Launcher_Frame(OpenBGI_t* o, const OpenBGI_Frame_t* f)
{
	uint32_t now;
	int tw, th;
	uint8_t* shot;
	// (not while a host's run for a first picture or its work for the program is out: its engine reads the file)
	if(o->launcher.cfgChanged && !o->launcher.previewOut && !o->launcher.jobsOut &&
		(int32_t)(o->host.ticksMs(o->host.user) - o->launcher.cfgSaveMs) >= 0)
		Launcher_SaveSettings(o);
	if(!o->launcher.launched || !HasPicture(f))
		return;
	now = o->host.ticksMs(o->host.user);
	if((int32_t)(now - o->launcher.shotMs) < 0 || Request_Pending(o))
		return;
	o->launcher.shotMs = now + LAUNCHER_SHOT_MS;
	shot = SmallPicture(f, &tw, &th);
	if(!shot)
		return; // (a blank picture: the last one stays)
	free(o->launcher.shot);
	o->launcher.shot = shot;
	o->launcher.shotW = tw;
	o->launcher.shotH = th;
}

// a sum of a game's path, which names what is kept of it (FNV-1a)
static uint64_t PathSum(const char* path)
{
	uint64_t hash = 0xcbf29ce484222325ull;
	const uint8_t* p;
	for(p = (const uint8_t*)path; *p; p++)
		hash = (hash ^ *p) * 0x100000001b3ull;
	return hash;
}

/* where something of the game at `path` is kept in the folder `folder`
 * of the user's data, as a file ending in `ext`: the file's name into
 * `name` (32 bytes) and its native path into `out`, the folder made when
 * it is missing; 0 when the user's data has no folder */
static int KeptPath(OpenBGI_t* o, const char* path, const char* folder, const char* ext, char* name, char* out, size_t n)
{
	char dir[LAUNCHER_PATH];
	uint64_t hash = PathSum(path);
	Host_UserDataDir(o, dir, sizeof dir);
	if(!dir[0])
		return 0;
	snprintf(out, n, "%s%s", dir, folder);
	if(!Host_NativeMkdir(o, out))
		return 0;
	snprintf(name, 32, "%08x%08x.%s", (unsigned)(hash >> 32), (unsigned)(hash & 0xffffffffu), ext);
	snprintf(out, n, "%s%s%c%s", dir, folder, Host_NativeSep(o), name);
	return 1;
}

// the small picture `shot` (w x h) of the game at `path` as a file of the pictures: its name into `name` (32 bytes); 0 when it could not be written
static int PictureWrite(OpenBGI_t* o, const char* path, const uint8_t* shot, int w, int h, char* name)
{
	char file[LAUNCHER_PATH + 64];
	size_t size = 0;
	uint8_t* png;
	int ok;
	if(!KeptPath(o, path, LAUNCHER_PICTURES, "png", name, file, sizeof file))
		return 0;
	png = Png_Encode(shot, (uint32_t)w, (uint32_t)h, 4, (size_t)w * 4, 6, &size);
	ok = png && Host_WriteNativeFile(o, file, png, (uint32_t)size);
	free(png);
	return ok;
}

void Launcher_GameEnded(OpenBGI_t* o)
{
	uint32_t ms = o->host.ticksMs(o->host.user) - o->launcher.startMs;
	char num[32], picture[32];
	if(!o->launcher.launched)
		return;
	o->launcher.launched = 0;
	picture[0] = 0;
	if(o->launcher.shot && !PictureWrite(o, o->launcher.gamePath, o->launcher.shot, o->launcher.shotW, o->launcher.shotH, picture))
		picture[0] = 0;
	free(o->launcher.shot);
	o->launcher.shot = NULL;
	// the time it ran and its picture, onto what the launcher's settings say of the game
	if(o->launcher.gamesPath[0] && Engine_LoadGamesJson(o, o->launcher.gamesPath) > 0)
	{
		unsigned long total = 0;
		if(Engine_CfgGet(o, o->launcher.gamePath, "seconds", 0, num, sizeof num) > 0)
			total = strtoul(num, NULL, 10);
		total += ms / 1000;
		snprintf(num, sizeof num, "%lu", total);
		Engine_CfgSet(o, o->launcher.gamePath, "seconds", 0, num);
		if(picture[0])
			Engine_CfgSet(o, o->launcher.gamePath, "picture", 0, picture);
		Engine_SaveGamesJson(o, o->launcher.gamesPath);
	}
}

// ---- a listed game's icon and first picture ---------------------------------------------------

#define ICON_EXE_MAX 0x4000000u // a program larger than 64 MB is not looked into

// whether a program of this name (in lower case) comes with a game without being the game
static int IsSideProgram(const char* name)
{
	static const char* const kParts[] = {"setup", "install", "unins", "bhvc", "update", "patch", "config", "redist"};
	size_t i;
	for(i = 0; i < BGI_COUNTOF(kParts); i++)
		if(strstr(name, kParts[i]))
			return 1;
	return 0;
}

static int Contains(const uint8_t* d, size_t n, const char* text)
{
	size_t len = strlen(text), i;
	for(i = 0; i + len <= n; i++)
		if(d[i] == (uint8_t)text[0] && memcmp(d + i, text, len) == 0)
			return 1;
	return 0;
}

uint8_t* Launcher_GameIcon(OpenBGI_t* o, const char* native, size_t* size)
{
	Vol_t* v = Vol_Open(o, native);
	uint8_t* best = NULL;
	int bestScore = 0;
	uint32_t i;
	*size = 0;
	if(!v)
		return NULL;
	for(i = 0; i < Vol_Count(v); i++)
	{ // the programs beside system.arc: the engine's own (it names the boot program) before any other with an icon
		const VolEntry_t* e = Vol_At(v, i);
		size_t len = strlen(e->key), n = 0, icoSize = 0;
		uint8_t *data, *ico;
		int score;
		if(e->isDir || !e->readable || strchr(e->key, '/') || len < 5 || strcmp(e->key + len - 4, ".exe") != 0)
			continue;
		if(IsSideProgram(e->key) || e->size < 0x400 || e->size > ICON_EXE_MAX)
			continue;
		data = Vol_Load(v, e, &n);
		if(!data)
			continue;
		ico = Icon_FromExe(data, n, &icoSize);
		score = ico ? 1 + (Contains(data, n, "ipl._bp") ? 2 : 0) : 0;
		free(data);
		if(score > bestScore)
		{
			BGI_Free(best);
			best = ico;
			bestScore = score;
			*size = icoSize;
		}
		else
			BGI_Free(ico);
	}
	Vol_Close(v);
	return best;
}

int Launcher_PreviewsOff(OpenBGI_t* o)
{
	char value[16];
	return o->engine.optPreviewNone || (Engine_CfgGet(o, "", "preview", 0, value, sizeof value) > 0 && strcmp(value, "none") == 0);
}

// what a host's run is to bring of the game at `path`: OPENBGI_PREVIEW_*, 0 for nothing
static int PreviewWant(OpenBGI_t* o, const char* path)
{
	char value[64];
	int want = 0;
	if(Engine_CfgGet(o, path, "icon", 0, value, sizeof value) <= 0)
		want |= OPENBGI_PREVIEW_ICON;
	// (a game whose run showed nothing is not run again: "preview" says so)
	if(!Launcher_PreviewsOff(o) && Engine_CfgGet(o, path, "picture", 0, value, sizeof value) <= 0 &&
		Engine_CfgGet(o, path, "preview", 0, value, sizeof value) <= 0)
		want |= OPENBGI_PREVIEW_PICTURE;
	return want;
}

int Launcher_Listed(OpenBGI_t* o, const char* path)
{
	uint64_t sum = PathSum(path);
	int i;
	if(!o->launcher.running || !path[0] || strlen(path) >= LAUNCHER_PATH || strchr(path, '"'))
		return 0;
	for(i = 0; i < o->launcher.listedCount; i++)
		if(o->launcher.listed[i] == sum)
			return 0;
	if(o->launcher.listedCount == o->launcher.listedCap)
	{
		int cap = o->launcher.listedCap ? o->launcher.listedCap * 2 : 64;
		uint64_t* grown = (uint64_t*)realloc(o->launcher.listed, (size_t)cap * sizeof(uint64_t));
		if(!grown)
			return 0;
		o->launcher.listed = grown;
		o->launcher.listedCap = cap;
	}
	o->launcher.listed[o->launcher.listedCount++] = sum;
	if(PreviewWant(o, path) && o->launcher.previewCount < LAUNCHER_PREVIEWS)
		o->launcher.previews[o->launcher.previewCount++] = BGI_Strdup(path);
	return 1;
}

int Launcher_PreviewCount(OpenBGI_t* o, char* dst, size_t size)
{
	return snprintf(dst, size, "%u", (unsigned)o->launcher.previewSerial);
}

static void PreviewsFree(OpenBGI_t* o)
{
	int i;
	for(i = 0; i < o->launcher.previewCount; i++)
		BGI_Free(o->launcher.previews[i]);
	o->launcher.previewCount = 0;
	BGI_Free(o->launcher.previewOut);
	o->launcher.previewOut = NULL;
	o->launcher.previewWant = 0;
	BGI_Free(o->launcher.runIcon);
	o->launcher.runIcon = NULL;
	o->launcher.runIconSize = 0;
	o->launcher.runIconKnown = 0;
	free(o->launcher.listed);
	o->launcher.listed = NULL;
	o->launcher.listedCount = o->launcher.listedCap = 0;
}

/* the command line that runs the game at `path` for its first picture:
 * as the launcher would start it (the player's profile, patches and
 * options), but laid over a folder for what it writes, so that the
 * host's run of it can keep that out of the game's own */
static int PreviewCmd(OpenBGI_t* o, const char* path, char* cmd, size_t n)
{
	char native[LAUNCHER_PATH], value[LAUNCHER_PATH];
	int ok = 1, i;
	cmd[0] = 0;
	if(o->engine.optGames[0] && !strchr(o->engine.optGames, '"'))
		ok &= CmdAdd(cmd, n, "--games=\"%s\" ", o->engine.optGames);
	ok &= CmdAdd(cmd, n, "--lang=%s --preview ", gMsgEnglish ? "en" : "ja");
	if(Engine_CfgGet(o, path, "profile", 0, value, sizeof value) > 0 && !strpbrk(value, "\" "))
		ok &= CmdAdd(cmd, n, "--engine=%s ", value);
	for(i = 0; Engine_CfgGet(o, path, "patches", i, value, sizeof value) > 0; i++)
	{
		if(strchr(value, '"'))
			return 0;
		Host_Utf8ToNative(o, value, native, sizeof native);
		ok &= CmdAdd(cmd, n, "--patch=\"%s\" ", native);
	}
	if(Engine_CfgGet(o, path, "options", 0, value, sizeof value) > 0)
		ok &= CmdAdd(cmd, n, "%s ", value);
	Host_Utf8ToNative(o, path, native, sizeof native);
	ok &= CmdAdd(cmd, n, "\"%s\"", native);
	return ok;
}

int OpenBGI_PreviewNext(OpenBGI_t* o, char* cmdLine, size_t n)
{
	while(o->launcher.running && !o->launcher.relaunch && !o->launcher.previewOut && o->launcher.previewCount > 0)
	{
		char* path = o->launcher.previews[0];
		int want = PreviewWant(o, path); // (as the settings are now: the game may have been played meanwhile)
		o->launcher.previewCount--;
		memmove(o->launcher.previews, o->launcher.previews + 1, (size_t)o->launcher.previewCount * sizeof(char*));
		if(want && cmdLine && PreviewCmd(o, path, cmdLine, n))
		{
			/* The run's engine reads openbgi.json as it starts: what is
			 * to be written of it is written first, and nothing more is
			 * until the run is back (Launcher_Frame) - unless reading is
			 * out (OpenBGI_JobNext), whose engine reads the file too. */
			if(!o->launcher.jobsOut)
				Launcher_SaveSettings(o);
			o->launcher.previewOut = path;
			o->launcher.previewWant = want;
			return want;
		}
		BGI_Free(path);
	}
	return 0;
}

void OpenBGI_PreviewIcon(OpenBGI_t* o, const void* ico, size_t size)
{
	const char* path = o->launcher.previewOut;
	char name[32], file[LAUNCHER_PATH + 64];
	if(!path || !o->launcher.running || !(o->launcher.previewWant & OPENBGI_PREVIEW_ICON))
		return;
	if(!ico || !size)
		Engine_CfgSet(o, path, "icon", 0, "none");
	else if(KeptPath(o, path, LAUNCHER_ICONS, "ico", name, file, sizeof file) && Host_WriteNativeFile(o, file, ico, (uint32_t)size))
		Engine_CfgSet(o, path, "icon", 0, name);
	Launcher_SettingsChanged(o);
}

void OpenBGI_PreviewDone(OpenBGI_t* o, const OpenBGI_Frame_t* picture)
{
	char* path = o->launcher.previewOut;
	OpenBGI_Frame_t f;
	char name[32], value[64];
	int w, h, want = o->launcher.previewWant;
	uint8_t* shot;
	o->launcher.previewOut = NULL;
	o->launcher.previewWant = 0;
	if(!path)
		return;
	memset(&f, 0, sizeof f);
	if(picture)
		memcpy(&f, picture, picture->size < sizeof f ? picture->size : sizeof f);
	// (a picture from play that came in meanwhile stays)
	if(o->launcher.running && picture && (want & OPENBGI_PREVIEW_PICTURE) && Engine_CfgGet(o, path, "picture", 0, value, sizeof value) <= 0)
	{
		shot = HasPicture(&f) ? SmallPicture(&f, &w, &h) : NULL;
		if(shot && PictureWrite(o, path, shot, w, h, name))
		{
			Engine_CfgSet(o, path, "picture", 0, name);
			o->launcher.previewSerial++;
		}
		else
			Engine_CfgSet(o, path, "preview", 0, "none"); // nothing to show for it: not run again
		Launcher_SettingsChanged(o);
		free(shot);
	}
	BGI_Free(path);
}

void OpenBGI_SetIcon(OpenBGI_t* o, const void* ico, uint32_t size)
{
	o->launcher.ownIcon = ico && size ? (const uint8_t*)ico : NULL;
	o->launcher.ownIconSize = ico ? size : 0;
}

const uint8_t* Launcher_OwnIcon(OpenBGI_t* o, size_t* size)
{
	*size = o->launcher.ownIconSize;
	return o->launcher.ownIcon;
}

const void* OpenBGI_GameIcon(OpenBGI_t* o, size_t* size)
{
	if(!o->launcher.runIconKnown && !o->launcher.running)
	{
		char path[LAUNCHER_PATH + 64];
		if(Fs_Mounted(o))
			snprintf(path, sizeof path, "%s", Fs_MountedPath(o));
		else
			Host_GameDir(o, path, sizeof path);
		o->launcher.runIcon = path[0] ? Launcher_GameIcon(o, path, &o->launcher.runIconSize) : NULL;
		o->launcher.runIconKnown = 1;
	}
	if(size)
		*size = o->launcher.runIcon ? o->launcher.runIconSize : 0;
	return o->launcher.runIcon;
}

uint8_t* Launcher_RunIcon(OpenBGI_t* o, size_t* size)
{
	char path[LAUNCHER_PATH + 64];
	*size = 0;
	if(o->launcher.running)
		return NULL;
	if(o->launcher.launched && o->launcher.gameIcon[0])
	{ // what the launcher kept: a file of the icons, or that the game has none
		char dir[LAUNCHER_PATH];
		uint32_t n = 0;
		uint8_t* ico;
		if(strcmp(o->launcher.gameIcon, "none") == 0)
			return NULL;
		Host_UserDataDir(o, dir, sizeof dir);
		snprintf(path, sizeof path, "%s" LAUNCHER_ICONS "%c%s", dir, Host_NativeSep(o), o->launcher.gameIcon);
		ico = dir[0] ? Host_ReadNativeFile(o, path, &n) : NULL;
		if(ico)
		{
			*size = n;
			return ico;
		}
	}
	{ // a game started by itself: its program's icon, looked for now
		const void* found = OpenBGI_GameIcon(o, size);
		uint8_t* copy = found ? (uint8_t*)BGI_Alloc(*size) : NULL;
		if(copy)
			memcpy(copy, found, *size);
		return copy;
	}
}

// ---- work beside the frames ----------------------------------------------------------------

enum
{
	JOB_QUEUED, // waits for the host
	JOB_OUT,    // the host has it
	JOB_DONE    // back, and what came of it waits to be asked for
};

struct OpenBGI_Job
{
	struct OpenBGI_Job* next;
	int kind;                 // LAUNCHER_JOB_*
	int state;                // JOB_*
	int ahead;                // made before the program asked for it
	int gone;                 // nobody will ask: it is freed when the host hands it back
	struct Wait* wait;        // the thread of the program that waits for it; NULL for none
	char path[LAUNCHER_PATH]; // UTF-8
	int flags;                // a listing's: LAUNCHER_DIR_* (the engine's thread may add to them while the job is out)
	int hidden;               // .. with the hidden entries, as it was asked for first: what OpenBGI_JobRun goes by
	char games[0x408];        // the settings file of the engine that asks, and how often it was written
	uint32_t gamesSerial;
	LauncherDir_t* dir;  // what came of a listing; NULL: the folder cannot be read
	int found;           // .. and of a look: a game is there,
	LauncherGame_t game; // .. this one
};

int Launcher_JobsOn(OpenBGI_t* o)
{
	return o->launcher.running && (o->host.caps & OPENBGI_CAP_JOBS) != 0;
}

static void JobFree(struct OpenBGI_Job* job)
{
	DirFree(job->dir);
	free(job);
}

static void JobUnlink(OpenBGI_t* o, struct OpenBGI_Job* job)
{
	struct OpenBGI_Job** at;
	for(at = &o->launcher.jobs; *at; at = &(*at)->next)
		if(*at == job)
		{
			*at = job->next;
			job->next = NULL;
			return;
		}
}

// out of the queue, and freed now or - while the host has it - when it comes back
static void JobEnd(OpenBGI_t* o, struct OpenBGI_Job* job)
{
	JobUnlink(o, job);
	job->wait = NULL;
	if(job->state == JOB_OUT)
		job->gone = 1;
	else
		JobFree(job);
}

static struct OpenBGI_Job* JobFind(OpenBGI_t* o, int kind, const char* path, int flags)
{
	struct OpenBGI_Job* job;
	for(job = o->launcher.jobs; job; job = job->next)
		if(job->kind == kind && !job->wait && strcmp(job->path, path) == 0 &&
			(kind != LAUNCHER_JOB_LIST || (job->flags & LAUNCHER_DIR_HIDDEN) == (flags & LAUNCHER_DIR_HIDDEN)))
			return job;
	return NULL;
}

static struct OpenBGI_Job* JobAdd(OpenBGI_t* o, int kind, const char* path, int flags, int ahead)
{
	struct OpenBGI_Job* job;
	struct OpenBGI_Job** at;
	if(strlen(path) >= LAUNCHER_PATH || (job = (struct OpenBGI_Job*)calloc(1, sizeof *job)) == NULL)
		return NULL;
	job->kind = kind;
	job->flags = flags;
	job->hidden = flags & LAUNCHER_DIR_HIDDEN;
	job->ahead = ahead;
	strcpy(job->path, path);
	for(at = &o->launcher.jobs; *at; at = &(*at)->next)
		;
	*at = job;
	return job;
}

// `dir` and `name` as one path, as the program joins them
static int JobJoin(OpenBGI_t* o, const char* dir, const char* name, char* out, size_t n)
{
	size_t len = strlen(dir);
	int sep = len && !IsSep(o, dir[len - 1]);
	return (size_t)snprintf(out, n, "%s%s%s", dir, sep ? (Host_NativeSep(o) == '\\' ? "\\" : "/") : "", name) < n;
}

static int EndsNoCase(const char* s, const char* end)
{
	size_t n = strlen(s), k = strlen(end), i;
	if(n < k)
		return 0;
	for(i = 0; i < k; i++)
		if(tolower((uint8_t)s[n - k + i]) != end[i])
			return 0;
	return 1;
}

/* A listing for a search came back: the looks at what may be a game in
 * it, and the listings of its folders, go into the queue ahead of the
 * program's asking, in the order the program goes through them. */
static void JobsAhead(OpenBGI_t* o, const struct OpenBGI_Job* list)
{
	const LauncherDir_t* d = list->dir;
	char path[LAUNCHER_PATH];
	int i, own = 0;
	for(i = 0; i < d->count; i++)
	{
		const DirEntry_t* e = &d->e[i];
		if(e->folder)
		{
			if((list->flags & LAUNCHER_DIR_DEEPER) && JobJoin(o, list->path, e->name, path, sizeof path) &&
				!JobFind(o, LAUNCHER_JOB_LIST, path, list->flags))
				JobAdd(o, LAUNCHER_JOB_LIST, path, (list->flags & LAUNCHER_DIR_HIDDEN) | LAUNCHER_DIR_GAMES, 1);
		}
		else if(EndsNoCase(e->name, ".zip") || EndsNoCase(e->name, ".iso"))
		{
			if(JobJoin(o, list->path, e->name, path, sizeof path) && !JobFind(o, LAUNCHER_JOB_PROBE, path, 0))
				JobAdd(o, LAUNCHER_JOB_PROBE, path, 0, 1);
		}
		else if(!own && ((strlen(e->name) == 10 && EndsNoCase(e->name, "system.arc")) || (strlen(e->name) == 7 && EndsNoCase(e->name, "ipl._bp"))))
		{ // the folder is a game itself
			own = 1;
			if(!JobFind(o, LAUNCHER_JOB_PROBE, list->path, 0))
				JobAdd(o, LAUNCHER_JOB_PROBE, list->path, 0, 1);
		}
	}
}

struct OpenBGI_Job* Launcher_JobFor(OpenBGI_t* o, int kind, const char* path, int flags)
{
	struct OpenBGI_Job* job;
	if(kind == LAUNCHER_JOB_LIST && (flags & LAUNCHER_DIR_GAMES) && (flags & LAUNCHER_DIR_DEEPER))
	{ // a search begins at this place: what the one before left unasked goes
		struct OpenBGI_Job* next;
		for(job = o->launcher.jobs; job; job = next)
		{
			next = job->next;
			if(job->ahead && !job->wait)
				JobEnd(o, job);
		}
	}
	job = JobFind(o, kind, path, flags);
	if(job)
	{
		if(job->state == JOB_QUEUED && job != o->launcher.jobs)
		{ // somebody waits for it now: before what is only made ahead
			JobUnlink(o, job);
			job->next = o->launcher.jobs;
			o->launcher.jobs = job;
		}
		if(kind == LAUNCHER_JOB_LIST)
			job->flags |= flags & (LAUNCHER_DIR_GAMES | LAUNCHER_DIR_DEEPER);
		job->ahead = 0;
		return job;
	}
	return JobAdd(o, kind, path, flags, 0);
}

int Launcher_JobReady(const struct OpenBGI_Job* job)
{
	return job->state == JOB_DONE;
}

void Launcher_JobWait(struct OpenBGI_Job* job, struct Wait* w)
{
	job->wait = w;
}

int Launcher_JobTake(OpenBGI_t* o, struct OpenBGI_Job* job, LauncherGame_t* out)
{
	int r;
	if(job->kind == LAUNCHER_JOB_LIST)
	{
		r = DirHandle(o, job->dir);
		job->dir = NULL;
	}
	else
	{
		r = job->found;
		if(out)
			*out = job->game;
	}
	JobEnd(o, job);
	return r;
}

void Launcher_JobDrop(OpenBGI_t* o, struct OpenBGI_Job* job)
{
	JobEnd(o, job);
}

// the run ends: every job goes; a thread that waits for one is told (its wait would end a job that is no more)
static void JobsFree(OpenBGI_t* o)
{
	while(o->launcher.jobs)
	{
		struct OpenBGI_Job* job = o->launcher.jobs;
		if(job->wait)
			Launcher_JobGone(job->wait);
		JobEnd(o, job);
	}
}

struct OpenBGI_Job* OpenBGI_JobNext(OpenBGI_t* o)
{
	struct OpenBGI_Job* job;
	if(!o->launcher.running || o->launcher.relaunch)
		return NULL;
	// what a thread of the program waits for goes before what is only made ahead
	for(job = o->launcher.jobs; job && (job->state != JOB_QUEUED || !job->wait); job = job->next)
		;
	for(job = job ? job : o->launcher.jobs; job && job->state != JOB_QUEUED; job = job->next)
		;
	if(!job)
		return NULL;
	/* The engine that does the work reads openbgi.json for its tables:
	 * what is to be written of it is written before the first job goes
	 * out, and nothing more is until all are back (Launcher_Frame) - nor
	 * now, when a run for a first picture is out, whose engine reads the
	 * file too: the tables are then the file's as it stands. */
	if(!o->launcher.jobsOut && !o->launcher.previewOut)
		Launcher_SaveSettings(o);
	snprintf(job->games, sizeof job->games, "%s", o->engine.gamesPath);
	job->gamesSerial = o->launcher.cfgSerial;
	job->state = JOB_OUT;
	o->launcher.jobsOut++;
	return job;
}

void OpenBGI_JobRun(struct OpenBGI_Job* job, OpenBGI_t* worker)
{
	if(job->kind == LAUNCHER_JOB_LIST)
	{
		DirFree(job->dir); // (a job that was given back after it was run is run again)
		job->dir = DirList(worker, job->path, job->hidden);
		return;
	}
	if(!worker->launcher.jobTables || worker->launcher.jobSerial != job->gamesSerial || strcmp(worker->launcher.jobGames, job->games) != 0)
	{ // the tables the look goes by: those of the engine that asked, as its file has them now
		Engine_ResetGames(worker);
		if(job->games[0] && !Engine_LoadGamesJson(worker, job->games))
			Engine_ResetGames(worker);
		snprintf(worker->launcher.jobGames, sizeof worker->launcher.jobGames, "%s", job->games);
		worker->launcher.jobSerial = job->gamesSerial;
		worker->launcher.jobTables = 1;
	}
	job->found = Launcher_Probe(worker, job->path, &job->game);
}

void OpenBGI_JobBack(OpenBGI_t* o, struct OpenBGI_Job* job)
{
	if(o->launcher.jobsOut > 0)
		o->launcher.jobsOut--;
	if(job->gone)
		JobFree(job);
	else
		job->state = JOB_QUEUED;
}

void OpenBGI_JobDone(OpenBGI_t* o, struct OpenBGI_Job* job)
{
	if(o->launcher.jobsOut > 0)
		o->launcher.jobsOut--;
	if(job->gone)
	{
		JobFree(job);
		return;
	}
	job->state = JOB_DONE;
	if(job->kind == LAUNCHER_JOB_LIST && (job->flags & LAUNCHER_DIR_GAMES) && job->dir && o->launcher.running)
		JobsAhead(o, job);
}

// ---- the ends --------------------------------------------------------------------------------

void Launcher_SettingsChanged(OpenBGI_t* o)
{
	o->launcher.cfgChanged = 1;
	o->launcher.cfgSaveMs = o->host.ticksMs(o->host.user) + LAUNCHER_SAVE_MS;
}

void Launcher_SaveSettings(OpenBGI_t* o)
{
	if(o->launcher.cfgChanged)
	{
		Engine_SaveGames(o);
		o->launcher.cfgSerial++;
	}
	o->launcher.cfgChanged = 0;
}

void Launcher_RunEnded(OpenBGI_t* o)
{
	Launcher_DirCloseAll(o);
	DropsFree(o);
	PreviewsFree(o);
	JobsFree(o);
	Launcher_SaveSettings(o); // (whatever program changed them)
	Launcher_GameEnded(o);
	Launcher_SourcesFree(o); // the program compiled for this run
	o->launcher.running = 0;
}

void Launcher_Free(OpenBGI_t* o)
{
	Launcher_DirCloseAll(o);
	DropsFree(o);
	PreviewsFree(o);
	JobsFree(o);
	Launcher_SourcesFree(o);
	BGI_Free(o->launcher.relaunch);
	BGI_Free(o->launcher.back);
	BGI_Free(o->launcher.kept);
	free(o->launcher.shot);
	memset(&o->launcher, 0, sizeof o->launcher);
}
