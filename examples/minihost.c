/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * minihost.c - the smallest host: a game played from a zip archive
 *              without a window, its pictures written as image files;
 *              built with the library alone (include/openbgi.h, libopenbgi)
 *              by `make lib-example`
 *
 *     minihost [--run=MS] [--every=MS] [--font=FILE] GAME.zip PREFIX [ARGUMENT...]
 *
 * runs the game for MS milliseconds of its own clock (10000) and writes
 * the picture every MS of them (1000) as PREFIX<ms>.ppm (a directory in
 * PREFIX has to exist).  FILE is a font for the game's text, which is
 * not drawn without one unless the library has fonts built in.  The
 * ARGUMENTs are the game's.
 *
 * It shows what a host is made of, with nothing of a system in it but the
 * ISO C library:
 *
 *   the clock - the host's: here one that moves only between the frames,
 *               by what each frame asks to wait, so a run takes the same
 *               course every time
 *   the files - the archive is read with stdio; what the game writes -
 *               its settings, its saves - is kept in memory and gone at
 *               the end (ISO C makes no directories)
 *   the loop  - the requests answered (a box's text is printed and its
 *               first button taken), a frame run, its picture kept
 *
 * There is no input and no sound here: a host hands the engine its keys
 * and its pointer between the frames (OpenBGI_Key, OpenBGI_Pointer, ...)
 * and reads the sound from its output's thread (OpenBGI_AudioRead).
 */
#include "openbgi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---- the clock ----------------------------------------------------------------------------------

static uint32_t gNow; // milliseconds: moved by the loop alone

static uint32_t TicksMs(void* user)
{
	(void)user;
	return gNow;
}

static int PerfNs(void* user, int64_t* out)
{
	(void)user;
	*out = (int64_t)gNow * 1000000;
	return 1;
}

static void LocalTime(void* user, OpenBGI_DateTime_t* out)
{
	time_t t = time(NULL);
	const struct tm* tm = localtime(&t);
	(void)user;
	memset(out, 0, sizeof *out);
	if(!tm)
		return;
	out->year = (uint16_t)(tm->tm_year + 1900);
	out->month = (uint16_t)(tm->tm_mon + 1);
	out->dayOfWeek = (uint16_t)tm->tm_wday;
	out->day = (uint16_t)tm->tm_mday;
	out->hour = (uint16_t)tm->tm_hour;
	out->minute = (uint16_t)tm->tm_min;
	out->second = (uint16_t)tm->tm_sec;
}

// ---- the files ----------------------------------------------------------------------------------

/* The game directory is "C:\" to the game and MEM_DIR to the engine, and
 * lies in memory: a list of what the game wrote, by the path below it
 * with the ASCII letters in lower case.  Every other native path is a
 * file of the system's, which is only read. */
#define MEM_DIR   "mem/"
#define PATH_MAX_ 1024

typedef struct MemFile
{
	char key[PATH_MAX_]; // the path below the game directory, '\\' between its parts
	int isDir;
	uint8_t* data;
	uint32_t size, cap;
	struct MemFile* next;
} MemFile_t;

// a stream: a file of the system's, or one in memory with a position
typedef struct Stream
{
	FILE* fp;
	MemFile_t* mem;
	uint32_t pos;
} Stream_t;

static MemFile_t* gFiles;                  // what the game wrote
static char gCwd[PATH_MAX_];               // the game's current directory, a key ("": the game directory)
static char gGameDir[PATH_MAX_] = MEM_DIR; // the game directory's native path, a trailing '/'

/* The key of a game's path: 1 and `key` for one in the game directory
 * (absolute on "C:\", or relative to the current directory), 0 for
 * another drive's. */
static int Key(const char* path, char* key, size_t n)
{
	size_t len = 0;
	key[0] = 0;
	if(path[0] && path[1] == ':')
	{
		if((path[0] | 32) != 'c')
			return 0;
		path += 2;
	}
	else if(path[0] != '\\' && path[0] != '/')
		len = (size_t)snprintf(key, n, "%s", gCwd);
	while(*path)
	{
		size_t part = strcspn(path, "\\/"), i;
		if(part == 2 && path[0] == '.' && path[1] == '.')
		{ // the directory above
			while(len && key[--len] != '\\')
				;
			key[len] = 0;
		}
		else if(part && !(part == 1 && path[0] == '.'))
		{
			if(len + part + 2 > n)
				return 0;
			if(len)
				key[len++] = '\\';
			for(i = 0; i < part; i++)
				key[len++] = (char)(path[i] >= 'A' && path[i] <= 'Z' ? path[i] + 32 : path[i]);
			key[len] = 0;
		}
		path += part;
		if(*path)
			path++;
	}
	return 1;
}

// the file or directory of a key; NULL for none
static MemFile_t* Find(const char* key)
{
	MemFile_t* f;
	for(f = gFiles; f; f = f->next)
		if(strcmp(f->key, key) == 0)
			return f;
	return NULL;
}

static MemFile_t* Create(const char* key, int isDir)
{
	MemFile_t* f = (MemFile_t*)calloc(1, sizeof *f);
	if(!f)
		return NULL;
	snprintf(f->key, sizeof f->key, "%s", key);
	f->isDir = isDir;
	f->next = gFiles;
	gFiles = f;
	return f;
}

static void* Open(void* user, const char* path, int mode)
{
	char key[PATH_MAX_];
	MemFile_t* f;
	Stream_t* s;
	(void)user;
	if(!Key(path, key, sizeof key))
		return NULL;
	f = Find(key);
	if(f && f->isDir)
		return NULL;
	if(!f && (mode == OPENBGI_OPEN_READ || !(f = Create(key, 0))))
		return NULL;
	if(mode == OPENBGI_OPEN_CREATE)
		f->size = 0;
	s = (Stream_t*)calloc(1, sizeof *s);
	if(s)
		s->mem = f;
	return s;
}

static void Close(void* user, void* stream)
{
	Stream_t* s = (Stream_t*)stream;
	(void)user;
	if(s && s->fp)
		fclose(s->fp);
	free(s);
}

static uint32_t Read(void* user, void* stream, void* buf, uint32_t n)
{
	Stream_t* s = (Stream_t*)stream;
	(void)user;
	if(s->fp)
		return (uint32_t)fread(buf, 1, n, s->fp);
	if(s->pos >= s->mem->size)
		return 0;
	if(n > s->mem->size - s->pos)
		n = s->mem->size - s->pos;
	memcpy(buf, s->mem->data + s->pos, n);
	s->pos += n;
	return n;
}

static uint32_t Write(void* user, void* stream, const void* buf, uint32_t n)
{
	Stream_t* s = (Stream_t*)stream;
	MemFile_t* f = s->mem;
	(void)user;
	if(!f)
		return 0; // (the system's files are only read)
	if(s->pos + n > f->cap)
	{
		uint32_t cap = (s->pos + n) * 2;
		uint8_t* data = (uint8_t*)realloc(f->data, cap);
		if(!data)
			return 0;
		f->data = data;
		f->cap = cap;
	}
	if(s->pos > f->size)
		memset(f->data + f->size, 0, s->pos - f->size); // (written past the end: zeros in between)
	memcpy(f->data + s->pos, buf, n);
	s->pos += n;
	if(s->pos > f->size)
		f->size = s->pos;
	return n;
}

static int Seek(void* user, void* stream, uint64_t pos)
{
	Stream_t* s = (Stream_t*)stream;
	(void)user;
	if(s->fp)
		return pos <= 0x7fffffff && fseek(s->fp, (long)pos, SEEK_SET) == 0;
	if(pos > 0xffffffffu)
		return 0;
	s->pos = (uint32_t)pos;
	return 1;
}

static uint64_t Length(void* user, void* stream)
{
	Stream_t* s = (Stream_t*)stream;
	long at, end;
	(void)user;
	if(!s->fp)
		return s->mem->size;
	at = ftell(s->fp);
	if(at < 0 || fseek(s->fp, 0, SEEK_END) != 0)
		return 0;
	end = ftell(s->fp);
	fseek(s->fp, at, SEEK_SET);
	return end < 0 ? 0 : (uint64_t)end;
}

static uint32_t Attrs(void* user, const char* path)
{
	char key[PATH_MAX_];
	const MemFile_t* f;
	(void)user;
	if(!Key(path, key, sizeof key))
		return OPENBGI_INVALID_ATTRS;
	if(!key[0])
		return OPENBGI_ATTR_DIRECTORY; // the game directory
	f = Find(key);
	if(!f)
		return OPENBGI_INVALID_ATTRS;
	return f->isDir ? OPENBGI_ATTR_DIRECTORY : OPENBGI_ATTR_ARCHIVE;
}

static int Remove(void* user, const char* path)
{
	char key[PATH_MAX_];
	MemFile_t** at;
	(void)user;
	if(!Key(path, key, sizeof key))
		return 0;
	for(at = &gFiles; *at; at = &(*at)->next)
		if(!(*at)->isDir && strcmp((*at)->key, key) == 0)
		{
			MemFile_t* f = *at;
			*at = f->next;
			free(f->data);
			free(f);
			return 1;
		}
	return 0;
}

static int Move(void* user, const char* from, const char* to)
{
	char a[PATH_MAX_], b[PATH_MAX_];
	MemFile_t* f;
	(void)user;
	if(!Key(from, a, sizeof a) || !Key(to, b, sizeof b) || !(f = Find(a)) || Find(b))
		return 0;
	snprintf(f->key, sizeof f->key, "%s", b);
	return 1;
}

static int MakeDir(void* user, const char* path)
{
	char key[PATH_MAX_];
	(void)user;
	return Key(path, key, sizeof key) && key[0] && !Find(key) && Create(key, 1) != NULL;
}

static int SetCurrentDir(void* user, const char* dir)
{
	char key[PATH_MAX_];
	const MemFile_t* f;
	(void)user;
	if(!Key(dir, key, sizeof key) || (key[0] && (!(f = Find(key)) || !f->isDir)))
		return 0;
	snprintf(gCwd, sizeof gCwd, "%s", key);
	return 1;
}

static void ExeDir(void* user, char* buf, size_t n)
{
	(void)user;
	snprintf(buf, n, "C:\\");
}

static uint32_t DriveType(void* user, const char* root)
{
	(void)user;
	return (root[0] | 32) == 'c' ? OPENBGI_DRIVE_FIXED : OPENBGI_DRIVE_NO_ROOT;
}

// whether a native path lies in the directory in memory
static int InMemory(const char* path)
{
	return strncmp(path, MEM_DIR, strlen(MEM_DIR)) == 0;
}

static void* NativeOpen(void* user, const char* path, int mode)
{
	Stream_t* s;
	FILE* fp;
	(void)user;
	if(mode != OPENBGI_OPEN_READ || InMemory(path) || !(fp = fopen(path, "rb")))
		return NULL;
	s = (Stream_t*)calloc(1, sizeof *s);
	if(s)
		s->fp = fp;
	else
		fclose(fp);
	return s;
}

static int NativeStat(void* user, const char* path, OpenBGI_NativeStat_t* out)
{
	Stream_t s;
	if(InMemory(path))
		return OPENBGI_NATIVE_DIR; // (only directories are asked for there, and each one "exists")
	memset(&s, 0, sizeof s);
	s.fp = fopen(path, "rb");
	if(!s.fp)
		return OPENBGI_NATIVE_NONE; // (or a directory: ISO C cannot tell)
	if(out)
	{
		out->size = Length(user, &s);
		out->mtime = 0;
	}
	fclose(s.fp);
	return OPENBGI_NATIVE_FILE;
}

static int NativeMkdir(void* user, const char* path)
{
	(void)user;
	return InMemory(path);
}

static void NativeAbsolute(void* user, const char* path, char* out, size_t n)
{
	(void)user;
	snprintf(out, n, "%s", path);
}

static void GameDir(void* user, char* buf, size_t n)
{
	(void)user;
	snprintf(buf, n, "%s", gGameDir);
}

// the engine's folder for the archive's game, below UserDataDir: the game directory from now on
static int SetGameDir(void* user, const char* dir)
{
	size_t len = strlen(dir);
	(void)user;
	if(!InMemory(dir) || len + 2 > sizeof gGameDir)
		return 0;
	snprintf(gGameDir, sizeof gGameDir, "%s%s", dir, len && dir[len - 1] == '/' ? "" : "/");
	gCwd[0] = 0;
	return 1;
}

static void UserDataDir(void* user, char* buf, size_t n)
{
	(void)user;
	snprintf(buf, n, "%s", MEM_DIR);
}

static const OpenBGI_Files_t kFiles = {
	.size = sizeof(OpenBGI_Files_t),
	.open = Open,
	.close = Close,
	.read = Read,
	.write = Write,
	.seek = Seek,
	.length = Length,
	.attrs = Attrs,
	.remove = Remove,
	.move = Move,
	.makeDir = MakeDir,
	.setCurrentDir = SetCurrentDir,
	.exeDir = ExeDir,
	.driveType = DriveType,
	.nativeSep = '/',
	.nativeOpen = NativeOpen,
	.nativeStat = NativeStat,
	.nativeMkdir = NativeMkdir,
	.nativeAbsolute = NativeAbsolute,
	.gameDir = GameDir,
	.setGameDir = SetGameDir,
	.userDataDir = UserDataDir,
};

// ---- the font ----------------------------------------------------------------------------------

static const char* gFont; // --font: the file every family is drawn with

static int FontFind(void* user, const char* family, int serif, int bold, int italic, OpenBGI_FontFile_t* out)
{
	(void)user;
	(void)family;
	(void)serif;
	(void)bold;
	(void)italic;
	memset(out, 0, sizeof *out);
	snprintf(out->path, sizeof out->path, "%s", gFont);
	return OPENBGI_FONT_FOUND;
}

static const OpenBGI_FontFiles_t kFonts = {.size = sizeof(OpenBGI_FontFiles_t), .find = FontFind};

// ---- the pictures ------------------------------------------------------------------------------

/* The frame's picture as a binary PPM file, PREFIX<ms>.ppm; 0 when there
 * is no picture yet or the file cannot be written. */
static int WritePicture(const OpenBGI_Frame_t* f, const char* prefix, uint32_t ms)
{
	char name[PATH_MAX_];
	FILE* fp;
	int x, y;
	if(!f->pixels)
		return 0;
	snprintf(name, sizeof name, "%s%06lu.ppm", prefix, (unsigned long)ms);
	fp = fopen(name, "wb");
	if(!fp)
	{
		fprintf(stderr, "minihost: cannot write %s\n", name);
		return 0;
	}
	fprintf(fp, "P6\n%d %d\n255\n", f->width, f->height);
	for(y = 0; y < f->height; y++)
	{
		const uint8_t* row = f->pixels + (size_t)y * (size_t)f->pitch;
		for(x = 0; x < f->width; x++)
		{
			uint8_t rgb[3];
			if(f->bpp == 16)
			{ // RGB 5-5-5
				unsigned v = row[x * 2] | (unsigned)row[x * 2 + 1] << 8;
				rgb[0] = (uint8_t)((v >> 10 & 31) * 255 / 31);
				rgb[1] = (uint8_t)((v >> 5 & 31) * 255 / 31);
				rgb[2] = (uint8_t)((v & 31) * 255 / 31);
			}
			else
			{ // B G R x
				rgb[0] = row[x * 4 + 2];
				rgb[1] = row[x * 4 + 1];
				rgb[2] = row[x * 4];
			}
			fwrite(rgb, 1, 3, fp);
		}
	}
	fclose(fp);
	return 1;
}

// ---- the loop ----------------------------------------------------------------------------------

/* Every request the engine waits with, answered: a box's text printed
 * and its first button taken (its second where that is the default, and
 * Cancel rather than Retry); everything else - a text to type, a file to
 * pick, the game chooser - cancelled. */
static void AnswerRequests(OpenBGI_t* o)
{
	OpenBGI_Request_t q = {.size = sizeof q};
	while(OpenBGI_NextRequest(o, &q))
	{
		int answer = 0;
		if(q.kind == OPENBGI_ASK_MESSAGE)
		{
			uint32_t buttons = q.u.message.flags & 0xf;
			int second = (q.u.message.flags & OPENBGI_MB_DEFBUTTON2) != 0;
			printf("minihost: [%s] %s\n", q.u.message.caption ? q.u.message.caption : "", q.u.message.text ? q.u.message.text : "");
			if(buttons == OPENBGI_MB_YESNO)
				answer = second ? OPENBGI_IDNO : OPENBGI_IDYES;
			else if(buttons == OPENBGI_MB_RETRYCANCEL || (buttons == OPENBGI_MB_OKCANCEL && second))
				answer = OPENBGI_IDCANCEL;
			else
				answer = OPENBGI_IDOK;
		}
		OpenBGI_Answer(o, q.id, answer);
	}
}

/* How far the clock moves after a frame: what the frame asks to wait (a
 * millisecond at least), and to the display's next refresh - 60 a second
 * - when it presented a picture. */
static uint32_t Wait(const OpenBGI_Frame_t* f)
{
	uint32_t d = f->idleMs > 1 ? (uint32_t)f->idleMs : 1;
	if(f->presented)
	{
		uint32_t next = (uint32_t)((((uint64_t)gNow * 60 / 1000 + 1) * 1000 + 59) / 60);
		if(next - gNow > d)
			d = next - gNow;
	}
	return d;
}

// the number after a "--name=" option; `fallback` when it is none
static uint32_t Number(const char* text, uint32_t fallback)
{
	char* end;
	unsigned long v = strtoul(text, &end, 10);
	return *text && !*end ? (uint32_t)v : fallback;
}

int main(int argc, char** argv)
{
	OpenBGI_Host_t host = {.size = sizeof host, .api = OPENBGI_API_VERSION};
	OpenBGI_Frame_t f = {.size = sizeof f};
	OpenBGI_t* o;
	uint32_t runMs = 10000, everyMs = 1000, next;
	char cmd[4096];
	const char* prefix;
	size_t len;
	int i = 1, rc, pictures = 0, code;

	for(; i < argc && strncmp(argv[i], "--", 2) == 0; i++)
	{
		if(strncmp(argv[i], "--run=", 6) == 0)
			runMs = Number(argv[i] + 6, runMs);
		else if(strncmp(argv[i], "--every=", 8) == 0)
			everyMs = Number(argv[i] + 8, everyMs);
		else if(strncmp(argv[i], "--font=", 7) == 0)
			gFont = argv[i] + 7;
		else
			break;
	}
	if(argc - i < 2 || everyMs == 0)
	{
		fprintf(stderr, "usage: minihost [--run=MS] [--every=MS] [--font=FILE] GAME.zip PREFIX [ARGUMENT...]\n");
		return 2;
	}
	prefix = argv[i + 1];
	len = (size_t)snprintf(cmd, sizeof cmd, "%s", argv[i]);
	for(i += 2; i < argc && len < sizeof cmd; i++)
		len += (size_t)snprintf(cmd + len, sizeof cmd - len, " %s", argv[i]);

	if(OpenBGI_ApiVersion() >> 16 != OPENBGI_API_MAJOR)
	{
		fprintf(stderr, "minihost: the library speaks version %u of the interface, this program %u\n", (unsigned)(OpenBGI_ApiVersion() >> 16), (unsigned)OPENBGI_API_MAJOR);
		return 1;
	}
	host.ticksMs = TicksMs;
	host.perfNs = PerfNs;
	host.localTime = LocalTime;
	host.caps = OPENBGI_CAP_DIALOGS; // the boxes are this program's to show (AnswerRequests)
	host.files = &kFiles;
	if(gFont)
		host.fontFiles = &kFonts;
	o = OpenBGI_New(&host);
	if(!o)
	{
		fprintf(stderr, "minihost: no engine (out of memory, or a library of another version)\n");
		return 1;
	}

	next = everyMs;
	rc = OpenBGI_Start(o, cmd);
	while(rc == OPENBGI_RUNNING && gNow < runMs)
	{
		AnswerRequests(o);
		rc = OpenBGI_RunFrame(o, &f);
		if(rc != OPENBGI_RUNNING)
			break;
		OpenBGI_PumpEvents(o);
		gNow += Wait(&f);
		for(; next <= gNow && next <= runMs; next += everyMs)
			pictures += WritePicture(&f, prefix, next); // (the picture is the frame's until the next frame)
	}
	AnswerRequests(o); // what an ended run still asks
	code = OpenBGI_Stop(o);
	AnswerRequests(o); // and what the shutdown does
	OpenBGI_Delete(o);
	printf("minihost: %lu ms, %d picture%s, exit code %d\n", (unsigned long)gNow, pictures, pictures == 1 ? "" : "s", code);

	while(gFiles)
	{
		MemFile_t* file = gFiles;
		gFiles = file->next;
		free(file->data);
		free(file);
	}
	return code;
}
