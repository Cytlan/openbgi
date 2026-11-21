/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/core.c - the core on its own: an engine whose host is this file
 *                and nothing else, driven through the calls of
 *                include/openbgi.h alone; built as test_core by `make test`
 *                and, linked with nothing but the core, by `make
 *                check-core`
 *
 * The host has a clock that moves five milliseconds a frame, answers the
 * requests itself and keeps the game's files in memory.  The groups, in
 * the order they run:
 *
 *   nothing - a host of clocks alone: no game to start, so the engine
 *             asks for the game chooser; quitting it ends the run
 *   game    - a boot program in the host's memory ("C:\ipl._bp"): the
 *             engine starts it, its box reaches the host with its text,
 *             the run goes on once the box is answered and ends with the
 *             program; the exit code is 0
 *   reload  - the same engine started again and again, and new ones: each
 *             run goes as the first; a start over a run that is still
 *             up, a second stop and a delete without a stop leave
 *             nothing behind (built with SAN=address, the sanitizer
 *             watches for what does)
 *   sizes   - a host built with another copy of the header: the engine
 *             takes no host without its size or of another version;
 *             reads of a host struct and of a table only what their
 *             sizes say (the members behind count as missing) and keeps
 *             copies, so the host's need not stay; and fills in of a
 *             frame and a request no more than the host's hold - each
 *             struct lies in memory of just its size here, for the
 *             sanitizer to see a byte too many
 */
#include "openbgi.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

// ---- the clock ------------------------------------------------------------------------------

static uint32_t gNow = 1000; // ms

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
	(void)user;
	memset(out, 0, sizeof *out);
	out->year = 2026;
	out->month = 1;
	out->day = 1;
}

// ---- the game's files, in memory ----------------------------------------------------------------

#define GAME_FILES 4

static struct
{
	char path[64];
	uint8_t data[0x400];
	uint32_t size;
} gFile[GAME_FILES];
static int gFileCount;

typedef struct Stream
{
	int file;
	uint32_t pos;
} Stream_t;

static int Lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

// the file of a game's path ("C:\NAME" or "NAME", any case); -1 for none
static int FileOf(const char* path)
{
	int i;
	if((path[0] == 'C' || path[0] == 'c') && path[1] == ':' && path[2] == '\\')
		path += 3;
	for(i = 0; i < gFileCount; i++)
	{
		const char* a = gFile[i].path;
		const char* b = path;
		while(*a && Lower((unsigned char)*a) == Lower((unsigned char)*b))
			a++, b++;
		if(!*a && !*b)
			return i;
	}
	return -1;
}

static void* Open(void* user, const char* path, int mode)
{
	Stream_t* s;
	int f = FileOf(path);
	(void)user;
	if(f < 0 || mode != OPENBGI_OPEN_READ)
		return NULL;
	s = (Stream_t*)calloc(1, sizeof *s);
	if(s)
		s->file = f;
	return s;
}

static void Close(void* user, void* f)
{
	(void)user;
	free(f);
}

static uint32_t Read(void* user, void* f, void* buf, uint32_t n)
{
	Stream_t* s = (Stream_t*)f;
	uint32_t left = gFile[s->file].size - s->pos;
	(void)user;
	if(n > left)
		n = left;
	memcpy(buf, gFile[s->file].data + s->pos, n);
	s->pos += n;
	return n;
}

static int Seek(void* user, void* f, uint64_t pos)
{
	Stream_t* s = (Stream_t*)f;
	(void)user;
	if(pos > gFile[s->file].size)
		return 0;
	s->pos = (uint32_t)pos;
	return 1;
}

static uint64_t Size(void* user, void* f)
{
	(void)user;
	return gFile[((Stream_t*)f)->file].size;
}

static uint32_t Attrs(void* user, const char* path)
{
	(void)user;
	if(strcmp(path, "C:\\") == 0 || strcmp(path, "C:") == 0)
		return OPENBGI_ATTR_DIRECTORY;
	return FileOf(path) >= 0 ? OPENBGI_ATTR_ARCHIVE : OPENBGI_INVALID_ATTRS;
}

static void ExeDir(void* user, char* buf, size_t n)
{
	(void)user;
	snprintf(buf, n, "C:\\");
}

static uint32_t DriveType(void* user, const char* root)
{
	(void)user;
	return root[0] == 'C' || root[0] == 'c' ? OPENBGI_DRIVE_FIXED : OPENBGI_DRIVE_NO_ROOT;
}

static const OpenBGI_Files_t kFiles = {.size = sizeof(OpenBGI_Files_t), .open = Open, .close = Close, .read = Read, .seek = Seek, .length = Size, .attrs = Attrs, .exeDir = ExeDir, .driveType = DriveType};

// ---- a boot program ----------------------------------------------------------------------------------

/* "ipl._bp": its 16-byte header (the header's size, the code's), "05" the
 * address of the text, "B0 80" an information box with it, "17" the end
 * of the thread, the text */
static void PutBootProgram(const char* text)
{
	uint8_t* d = gFile[gFileCount].data;
	uint32_t code = 0, len = (uint32_t)strlen(text) + 1, n;
	snprintf(gFile[gFileCount].path, sizeof gFile[gFileCount].path, "ipl._bp");
	d[16 + code++] = 0x05; // push the address of the text: a displacement from this instruction
	d[16 + code++] = 6;
	d[16 + code++] = 0;
	d[16 + code++] = 0xb0; // B0 80: the box
	d[16 + code++] = 0x80;
	d[16 + code++] = 0x17; // the end
	memcpy(d + 16 + code, text, len);
	n = code + len + 0x40; // and room behind it
	memset(d, 0, 16);
	d[0] = 16;
	d[4] = (uint8_t)n;
	d[5] = (uint8_t)(n >> 8);
	gFile[gFileCount].size = 16 + n;
	gFileCount++;
}

// ---- the runs ------------------------------------------------------------------------------------

/* Frames until the run is over (at most `max`): every request answered
 * with `answer`, the kinds seen in *kinds (a bit each) and the first
 * message's text in `msg`.  The frames run. */
static int Run(OpenBGI_t* o, int rc, int answer, uint32_t* kinds, char* msg, size_t n, int max)
{
	OpenBGI_Frame_t f = {.size = sizeof f};
	OpenBGI_Request_t q = {.size = sizeof q};
	int frames = 0;
	while(frames < max)
	{
		while(OpenBGI_NextRequest(o, &q))
		{
			*kinds |= 1u << q.kind;
			if(q.kind == OPENBGI_ASK_MESSAGE && msg[0] == 0 && q.u.message.text)
				snprintf(msg, n, "%s", q.u.message.text);
			OpenBGI_Answer(o, q.id, q.kind == OPENBGI_ASK_MESSAGE ? OPENBGI_IDOK : answer);
		}
		if(rc != OPENBGI_RUNNING)
			break;
		rc = OpenBGI_RunFrame(o, &f);
		gNow += 5;
		frames++;
	}
	return frames;
}

static OpenBGI_Host_t Host(void)
{
	OpenBGI_Host_t h;
	memset(&h, 0, sizeof h);
	h.size = sizeof h;
	h.api = OPENBGI_API_VERSION;
	h.ticksMs = TicksMs;
	h.perfNs = PerfNs;
	h.localTime = LocalTime;
	h.caps = OPENBGI_CAP_DIALOGS;
	return h;
}

static void TestNothing(void)
{
	OpenBGI_Host_t host = Host();
	OpenBGI_t* o = OpenBGI_New(&host);
	uint32_t kinds = 0;
	char msg[0x100] = "";
	int frames;
	printf("nothing\n");
	CHECK(o != NULL);
	if(!o)
		return;
	frames = Run(o, OpenBGI_Start(o, ""), 0, &kinds, msg, sizeof msg, 200);
	CHECK(kinds & (1u << OPENBGI_ASK_GAME_CHOOSER));
	CHECK(frames < 200);
	OpenBGI_Stop(o);
	OpenBGI_Delete(o);
}

static void TestGame(void)
{
	OpenBGI_Host_t host = Host();
	OpenBGI_t* o;
	uint32_t kinds = 0;
	char msg[0x100] = "";
	int frames, code;
	printf("game\n");
	gFileCount = 0;
	PutBootProgram("the core runs alone");
	host.files = &kFiles;
	o = OpenBGI_New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	frames = Run(o, OpenBGI_Start(o, ""), 1, &kinds, msg, sizeof msg, 2000);
	CHECK(strcmp(msg, "the core runs alone") == 0);
	CHECK(!(kinds & (1u << OPENBGI_ASK_GAME_CHOOSER)));
	CHECK(frames > 0 && frames < 2000);
	code = OpenBGI_Stop(o);
	CHECK(code == 0);
	Run(o, OPENBGI_DONE, 1, &kinds, msg, sizeof msg, 1);
	OpenBGI_Delete(o);
}

// frames until the run asks for something (at most `max`), the request taken but not answered; the frames run, -1 when it asked nothing
static int RunToRequest(OpenBGI_t* o, int rc, int max)
{
	OpenBGI_Frame_t f = {.size = sizeof f};
	OpenBGI_Request_t q = {.size = sizeof q};
	int frames = 0;
	while(frames < max)
	{
		if(OpenBGI_NextRequest(o, &q))
			return frames;
		if(rc != OPENBGI_RUNNING)
			break;
		rc = OpenBGI_RunFrame(o, &f);
		gNow += 5;
		frames++;
	}
	return -1;
}

static void TestReload(void)
{
	OpenBGI_Host_t host = Host();
	OpenBGI_t* o;
	uint32_t kinds;
	char msg[0x100];
	int frames[4], i, pass, toBox;
	printf("reload\n");
	gFileCount = 0;
	PutBootProgram("the core runs alone");
	host.files = &kFiles;
	for(pass = 0; pass < 2; pass++)
	{ // two engines, each run four times
		o = OpenBGI_New(&host);
		CHECK(o != NULL);
		if(!o)
			return;
		for(i = 0; i < 4; i++)
		{
			kinds = 0;
			msg[0] = 0;
			gNow = 1000; // (the same clock for every run)
			frames[i] = Run(o, OpenBGI_Start(o, ""), 1, &kinds, msg, sizeof msg, 2000);
			CHECK(strcmp(msg, "the core runs alone") == 0);
			CHECK(frames[i] > 0 && frames[i] == frames[0]);
			CHECK(kinds == 1u << OPENBGI_ASK_MESSAGE);
			CHECK(OpenBGI_Stop(o) == 0);
			if(i == 2)
				CHECK(OpenBGI_Stop(o) == 0); // a second stop does nothing
		}
		OpenBGI_Delete(o);
	}
	// a start over a run whose box is still up: that run is stopped, the new one goes as any
	o = OpenBGI_New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	gNow = 1000;
	toBox = RunToRequest(o, OpenBGI_Start(o, ""), 2000);
	CHECK(toBox > 0);
	gNow = 1000;
	CHECK(RunToRequest(o, OpenBGI_Start(o, ""), 2000) == toBox);
	kinds = 0;
	msg[0] = 0;
	gNow = 1000;
	CHECK(Run(o, OpenBGI_Start(o, ""), 1, &kinds, msg, sizeof msg, 2000) == frames[0] && strcmp(msg, "the core runs alone") == 0);
	// deleted while the next run is up, unstopped
	gNow = 1000;
	CHECK(RunToRequest(o, OpenBGI_Start(o, ""), 2000) == toBox);
	OpenBGI_Delete(o);
	// .. and one that never ran, and none
	o = OpenBGI_New(&host);
	CHECK(o != NULL && OpenBGI_Stop(o) == 0);
	OpenBGI_Delete(o);
	OpenBGI_Delete(NULL);
}

// a copy of the first `size` bytes of a struct in memory of just that size, its `size` member saying so
static void* Cut(const void* whole, size_t size)
{
	uint32_t n = (uint32_t)size;
	void* p = malloc(size);
	if(!p)
		return NULL;
	memcpy(p, whole, size);
	memcpy(p, &n, sizeof n);
	return p;
}

static void TestSizes(void)
{
	OpenBGI_Host_t host = Host();
	OpenBGI_Host_t* cutHost;
	OpenBGI_Files_t* cutFiles;
	OpenBGI_Frame_t whole = {.size = sizeof whole};
	OpenBGI_Frame_t* cutFrame;
	OpenBGI_Request_t* cutRequest;
	OpenBGI_Request_t q = {.size = sizeof q};
	OpenBGI_t* o;
	uint32_t kinds = 0;
	char msg[0x100] = "";
	uint8_t big[sizeof(OpenBGI_Frame_t) + 32];
	uint32_t n;
	size_t i;
	int rc, zero;
	printf("sizes\n");
	gFileCount = 0;
	PutBootProgram("the core runs alone");
	CHECK(OpenBGI_ApiVersion() == OPENBGI_API_VERSION);

	// no engine for a host without its size, without the clocks in it, or of another version
	host.size = 0;
	CHECK(OpenBGI_New(&host) == NULL);
	host.size = offsetof(OpenBGI_Host_t, localTime);
	CHECK(OpenBGI_New(&host) == NULL);
	host = Host();
	host.api = OPENBGI_API_VERSION + 1; // a later minor than the library's
	CHECK(OpenBGI_New(&host) == NULL);
	host.api = OPENBGI_API_VERSION + 0x10000; // another major
	CHECK(OpenBGI_New(&host) == NULL);
	host.api = 0;
	CHECK(OpenBGI_New(&host) == NULL || OPENBGI_API_VERSION == 0);
	// nor for one with a table that lacks its size
	host = Host();
	cutFiles = (OpenBGI_Files_t*)Cut(&kFiles, sizeof kFiles);
	cutFiles->size = 0;
	host.files = cutFiles;
	CHECK(OpenBGI_New(&host) == NULL);
	free(cutFiles);

	// a host struct that ends before its tables: the files behind its size are not seen (no game: the chooser)
	host = Host();
	host.files = &kFiles;
	cutHost = (OpenBGI_Host_t*)Cut(&host, offsetof(OpenBGI_Host_t, files));
	o = OpenBGI_New(cutHost);
	free(cutHost); // (the engine keeps its own copy)
	CHECK(o != NULL);
	if(!o)
		return;
	Run(o, OpenBGI_Start(o, ""), 0, &kinds, msg, sizeof msg, 200);
	CHECK(kinds == 1u << OPENBGI_ASK_GAME_CHOOSER && msg[0] == 0);
	OpenBGI_Stop(o);
	OpenBGI_Delete(o);

	// a files table that ends before `attrs`: the game directory cannot be looked at, so again no game
	cutFiles = (OpenBGI_Files_t*)Cut(&kFiles, offsetof(OpenBGI_Files_t, attrs));
	host.files = cutFiles;
	o = OpenBGI_New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	kinds = 0;
	Run(o, OpenBGI_Start(o, ""), 0, &kinds, msg, sizeof msg, 200);
	CHECK(kinds == 1u << OPENBGI_ASK_GAME_CHOOSER && msg[0] == 0);
	OpenBGI_Stop(o);
	OpenBGI_Delete(o);
	free(cutFiles);

	// the whole table, gone once the engine is made: the game runs from the engine's copy, also when started again
	cutFiles = (OpenBGI_Files_t*)Cut(&kFiles, sizeof kFiles);
	host.files = cutFiles;
	o = OpenBGI_New(&host);
	memset(cutFiles, 0, sizeof *cutFiles);
	free(cutFiles);
	memset(&host, 0, sizeof host);
	CHECK(o != NULL);
	if(!o)
		return;
	for(i = 0; i < 2; i++)
	{
		kinds = 0;
		msg[0] = 0;
		Run(o, OpenBGI_Start(o, ""), 1, &kinds, msg, sizeof msg, 2000);
		CHECK(kinds == 1u << OPENBGI_ASK_MESSAGE && strcmp(msg, "the core runs alone") == 0);
		CHECK(OpenBGI_Stop(o) == 0);
	}

	// a request and a frame that end early: filled in up to their sizes, and those stay
	rc = OpenBGI_Start(o, "");
	CHECK(rc == OPENBGI_RUNNING);
	cutRequest = (OpenBGI_Request_t*)Cut(&q, offsetof(OpenBGI_Request_t, u));
	cutFrame = (OpenBGI_Frame_t*)Cut(&whole, offsetof(OpenBGI_Frame_t, pixels));
	cutFrame->idleMs = -1;
	for(i = 0; i < 2000 && rc == OPENBGI_RUNNING && !OpenBGI_NextRequest(o, cutRequest); i++)
	{
		rc = OpenBGI_RunFrame(o, cutFrame);
		gNow += 5;
	}
	CHECK(i > 0 && i < 2000 && rc == OPENBGI_RUNNING);
	CHECK(cutFrame->size == offsetof(OpenBGI_Frame_t, pixels) && cutFrame->idleMs >= 0);
	CHECK(cutRequest->size == offsetof(OpenBGI_Request_t, u) && cutRequest->kind == OPENBGI_ASK_MESSAGE && cutRequest->id != 0);
	// a frame larger than the engine's (a host of a later header): what the engine does not know of is zero
	memset(big, 0xa5, sizeof big);
	n = sizeof big;
	memcpy(big, &n, sizeof n);
	CHECK(OpenBGI_RunFrame(o, (OpenBGI_Frame_t*)(void*)big) == OPENBGI_RUNNING);
	memcpy(&n, big, sizeof n);
	for(zero = 1, i = sizeof(OpenBGI_Frame_t); i < sizeof big; i++)
		zero &= big[i] == 0;
	CHECK(n == sizeof big && zero);
	memcpy(&whole, big, sizeof whole);
	CHECK(whole.changes == 0 && whole.idleMs >= 0); // (the box is up: the engine holds still)
	// a frame and a request without a size get nothing
	memset(&whole, 0xa5, sizeof whole);
	whole.size = 0;
	CHECK(OpenBGI_RunFrame(o, &whole) == OPENBGI_RUNNING && whole.idleMs == (int)0xa5a5a5a5);
	OpenBGI_Answer(o, cutRequest->id, OPENBGI_IDOK);
	free(cutRequest);
	free(cutFrame);
	kinds = 0;
	Run(o, OPENBGI_RUNNING, 1, &kinds, msg, sizeof msg, 2000);
	CHECK(OpenBGI_Stop(o) == 0);
	OpenBGI_Delete(o);
}

int main(void)
{
	TestNothing();
	TestGame();
	TestReload();
	TestSizes();
	if(gFails)
	{
		printf("core: %d checks failed\n", gFails);
		return 1;
	}
	printf("core: all tests passed\n");
	return 0;
}
