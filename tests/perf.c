/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/perf.c - unit test of the performance display (src/sys/perf.c,
 *                bgi/perf.h): the frames timed, the figures of a span,
 *                the counts taken elsewhere, the log's line, the panel's
 *                picture and its place in the screen layer; built as
 *                test_perf by `make test`
 *
 * An engine that never runs a program, with a host whose clocks the test
 * moves, whose machine says how much memory the process holds and whose
 * files answer a read after 3 ms.  A frame is Perf_Begin, the engine's
 * work as time that passes, Perf_End with a frame that changed its
 * picture or not, and the rest of the period.  The groups:
 *
 *   off     - nothing is timed or counted until it is turned on; a run
 *             nobody sees and the settings' own engine never have it on;
 *             the setting "perf" turns it on when a run's programs start
 *   figures - 51 frames of 10 ms with 2 ms of work make half a second:
 *             frames and pictures a second, the period's mean and
 *             longest, the work's, its share of the time, the memory
 *             and its most; the periods' ring; a span begins anew
 *   counts  - the files' reads and their time, counted only while on;
 *             the sound a host asked for in vain; the bitmaps' bytes;
 *             a host of interface 1.0 says no memory
 *   line    - the log's line for given figures, with and without memory
 *   panel   - no picture before there are figures; its size for the
 *             surface's density; made anew only when its figures are
 *             newer; a frame's bar in the graph; gone when turned off
 *   layer   - the panel in the screen layer: to the right of the settings
 *             button and the pad's left hand, a little clear, handed
 *             over as a change with nothing else; twice a second
 *             and not with every frame; clear again when turned off
 */
#include "bgi/core.h"
#include "bgi/perf.h"
#include "bgi/release.h"
#include "bgi/gfx/bitmap.h"
#include "bgi/engine.h"
#include "bgi/host.h"
#include "bgi/overlay.h"
#include "bgi/input.h"
#include "bgi/gfx.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/snd/sndout.h"
#include "bgi/sysobj/events.h"
#include "host/os.h"

#include <stdio.h>
#include <string.h>

#include "check.h"

#define W  640
#define H  480
#define SW 2400
#define SH 1080

static int64_t gNs = 1000000000; // the host's clocks
static uint64_t gMem;            // what its machine says of the process's memory
static uint8_t gPicture[W * H * 4];
static OpenBGI_Host_t gHost;

static uint32_t TicksMs(void* user)
{
	(void)user;
	return (uint32_t)(gNs / 1000000);
}

static int PerfNs(void* user, int64_t* out)
{
	(void)user;
	*out = gNs;
	return 1;
}

static uint64_t ProcessMemory(void* user)
{
	(void)user;
	return gMem;
}

// a read takes 3 ms and gives all that was asked for
static uint32_t Read(void* user, void* f, void* buf, uint32_t n)
{
	(void)user;
	(void)f;
	memset(buf, 0, n);
	gNs += 3000000;
	return n;
}

static const OpenBGI_Machine_t kMachine = {.size = sizeof(OpenBGI_Machine_t), .processMemory = ProcessMemory};
// .. and one of a host compiled with the interface 1.0: the table ends before the member
static const OpenBGI_Machine_t kOldMachine = {.size = offsetof(OpenBGI_Machine_t, processMemory), .processMemory = ProcessMemory};
static const OpenBGI_Files_t kFiles = {.size = sizeof(OpenBGI_Files_t), .read = Read};

static OpenBGI_t* EngineUp(const OpenBGI_Machine_t* machine)
{
	OpenBGI_t* o;
	gHost = *OS_Host();
	if(gHost.window)
	{ // a phone's host fits no window to a screen (one that does gets no on-screen controls: tests/settings.c)
		static OpenBGI_Window_t window;
		window = *gHost.window;
		window.workArea = NULL;
		gHost.window = &window;
	}
	gHost.ticksMs = TicksMs;
	gHost.perfNs = PerfNs;
	gHost.machine = machine;
	gHost.files = &kFiles;
	o = OpenBGI_New(&gHost);
	if(!o)
		return NULL;
	Gfx_Create(o, 0xffffffffu, 1);
	Input_ResetLayers(o);
	o->display.windowAlive = 1;
	memset(&o->screen.pic, 0, sizeof o->screen.pic);
	o->screen.pic.pixels = gPicture;
	o->screen.pic.w = W;
	o->screen.pic.h = H;
	o->screen.pic.pitch = W * 4;
	o->screen.pic.bpp = 4;
	o->screen.pic.mode = PM_RGB32;
	return o;
}

static void EngineDown(OpenBGI_t* o)
{
	MsgQueue_Clear(o);
	Input_ResetLayers(o);
	memset(&o->screen.pic, 0, sizeof o->screen.pic);
	o->display.windowAlive = 0;
	Gfx_Destroy(o);
	OpenBGI_Delete(o);
}

// a frame of `period` microseconds, `work` of them the engine's; `drawn`: its picture changed
static void Frame(OpenBGI_t* o, int period, int work, int drawn)
{
	OpenBGI_Frame_t f;
	memset(&f, 0, sizeof f);
	f.size = sizeof f;
	Perf_Begin(o);
	gNs += (int64_t)work * 1000;
	f.changes = drawn ? 1 : 0;
	Perf_End(o, &f);
	gNs += (int64_t)(period - work) * 1000;
}

// 51 frames of 10 ms with 2 ms of work, every other one drawn: the panel's span is over with the last (502 ms)
static void HalfSecond(OpenBGI_t* o)
{
	int i;
	for(i = 0; i < 51; i++)
		Frame(o, 10000, 2000, i % 2 == 0);
}

// ---- the groups -----------------------------------------------------------------------------------

static void TestOff(void)
{
	OpenBGI_t* o = EngineUp(&kMachine);
	uint8_t buf[64];
	int w, h, changed;
	printf("off\n");
	if(!o)
		return;
	CHECK(!Perf_On(o));
	HalfSecond(o);
	CHECK(!o->perf.begin && !o->perf.count && !o->perf.shown.valid && !o->perf.panel.frames);
	CHECK(Host_FileRead(o, (HostFile_t*)buf, buf, 64) == 64 && !o->perf.diskReads && !o->perf.diskBytes);
	CHECK(!Perf_Picture(o, 400, &w, &h, &changed) && !changed);
	// the setting is read when the run's programs are up, once
	Engine_CfgSet(o, "", "perf", 0, "on");
	Frame(o, 10000, 2000, 1);
	CHECK(!Perf_On(o) && !o->perf.loaded);
	o->engine.stage = ENGINE_STAGE_VM;
	Frame(o, 10000, 2000, 1);
	CHECK(Perf_On(o) && o->perf.loaded && o->perf.begin);
	// turned off, and kept so
	Perf_Set(o, 0, 1);
	CHECK(!Perf_On(o) && Engine_CfgGet(o, "", "perf", 0, (char*)buf, sizeof buf) < 0);
	Perf_Set(o, 1, 1);
	CHECK(Perf_On(o) && Engine_CfgGet(o, "", "perf", 0, (char*)buf, sizeof buf) == 2 && strcmp((char*)buf, "on") == 0);
	// a run for the launcher's first picture never has it on, whatever the settings say
	o->perf.loaded = 0;
	o->perf.on = 0;
	o->engine.optPreview = 1;
	Frame(o, 10000, 2000, 1);
	CHECK(o->perf.loaded && !Perf_On(o));
	o->engine.optPreview = 0;
	// .. nor the settings' own engine
	o->perf.loaded = 0;
	o->settings.game = o;
	Frame(o, 10000, 2000, 1);
	CHECK(o->perf.loaded && !Perf_On(o));
	o->settings.game = NULL;
	o->engine.stage = ENGINE_STAGE_NONE;
	Engine_CfgSet(o, "", "perf", 0, NULL);
	EngineDown(o);
}

static void TestFigures(void)
{
	OpenBGI_t* o = EngineUp(&kMachine);
	const PerfFigures_t* g;
	int64_t t0;
	int i;
	printf("figures\n");
	if(!o)
		return;
	g = &o->perf.shown;
	gMem = (uint64_t)100 * 1024 * 1024;
	Perf_Set(o, 1, 0);
	t0 = gNs;
	for(i = 0; i < 50; i++)
		Frame(o, 10000, 2000, i % 2 == 0);
	CHECK(!g->valid && o->perf.panel.frames == 50 && o->perf.panel.t0 == t0 && o->perf.count == 49);
	Frame(o, 10000, 2000, 1); // its end is 502 ms after the first one's start
	CHECK(g->valid && g->fps10 == 51 * 10000 / 502 && g->drawn10 == 26 * 10000 / 502);
	CHECK(g->frameUs == 502000 / 51 && g->frameMaxUs == 10000);
	CHECK(g->engineUs == 2000 && g->engineMaxUs == 2000 && g->busy == 51 * 2 * 100 / 502);
	CHECK(g->memKb == 100 * 1024 && g->peakKb == 100 * 1024);
	CHECK(g->voices == 0 && g->dropped == 0 && g->starved == 0 && g->diskReads == 0 && g->diskKbs == 0);
	CHECK(o->perf.memCount == 1 && o->perf.mem[0] == 100 * 1024);
	// the span began anew where that one ended
	CHECK(o->perf.panel.frames == 0 && o->perf.panel.t0 == t0 + 502000000 && o->perf.log.frames == 51);
	// the periods, the newest last: one of 60 ms, one longer than the ring's values go
	CHECK(o->perf.count == 50 && o->perf.period[49] == 100);
	gMem = (uint64_t)80 * 1024 * 1024;
	Frame(o, 60000, 30000, 0);
	Frame(o, 7000000, 1000, 0);
	Frame(o, 10000, 1000, 0);
	CHECK(o->perf.count == 53 && o->perf.period[50] == 100 && o->perf.period[51] == 600 && o->perf.period[52] == 0xffff);
	// .. and in the figures of the span they fell into; the memory's most stays
	CHECK(g->valid && g->frameMaxUs == 7000000 && g->engineMaxUs == 30000);
	CHECK(g->memKb == 80 * 1024 && g->peakKb == 100 * 1024 && o->perf.memCount == 2);
	// the ring holds PERF_FRAMES and no more
	for(i = 0; i < PERF_FRAMES + 10; i++)
		Frame(o, 10000, 1000, 0);
	CHECK(o->perf.count == PERF_FRAMES && o->perf.at == (53 + PERF_FRAMES + 10) % PERF_FRAMES);
	// five seconds: the log's span was written and began anew
	CHECK(o->perf.log.t0 > t0);
	// turned off and on: nothing of before
	Perf_Set(o, 0, 0);
	Perf_Set(o, 1, 0);
	CHECK(!g->valid && !o->perf.count && !o->perf.memCount && !o->perf.peakKb && !o->perf.begin);
	EngineDown(o);
}

static void TestCounts(void)
{
	OpenBGI_t* o = EngineUp(&kMachine);
	const PerfFigures_t* g;
	uint8_t buf[4096];
	int16_t out[128];
	uint32_t ahead, dropped, starved;
	BmpMgr_t* m;
	int i;
	printf("counts\n");
	if(!o)
		return;
	g = &o->perf.shown;
	gMem = 0;
	Perf_Set(o, 1, 0);
	// two reads in a span: 8192 bytes, 6 ms
	Frame(o, 10000, 2000, 1);
	CHECK(Host_FileRead(o, (HostFile_t*)buf, buf, 4096) == 4096 && Host_FileRead(o, (HostFile_t*)buf, buf, 4096) == 4096);
	CHECK(o->perf.diskReads == 2 && o->perf.diskBytes == 8192 && o->perf.diskNs == 6000000);
	// a host that reads sound the mixer has not made: starved, while the mixer runs only
	CHECK(OpenBGI_AudioRead(o, out, 64) == 0);
	SndOut_Stats(o, &ahead, &dropped, &starved);
	CHECK(ahead == 0 && dropped == 0 && starved == 0);
	{ // (with a host that has no locks: with them the reader would mix what it lacks itself)
		const OpenBGI_Threads_t* threads = o->host.threads;
		o->host.threads = NULL;
		SndOut_Start(o);
		o->host.threads = threads;
	}
	CHECK(OpenBGI_AudioRead(o, out, 64) == 0);
	SndOut_Stats(o, &ahead, &dropped, &starved);
	CHECK(ahead == 0 && dropped == 0 && starved == 64);
	for(i = 0; i < 50; i++)
		Frame(o, 10000, 2000, 1);
	CHECK(g->valid && g->diskReads == 2 && g->diskUs == 6000 && g->diskKbs > 0 && g->starved == 64 && g->memKb == 0 && g->peakKb == 0);
	// the next span has none of them
	for(i = 0; i < 51; i++)
		Frame(o, 10000, 2000, 1);
	CHECK(g->valid && g->diskReads == 0 && g->diskUs == 0 && g->diskKbs == 0 && g->starved == 0);
	SndOut_Stop(o);
	{ // BGI_PERF=2: the parts of a pass - told apart, summed over the passes, and kept for the longest one
		OpenBGI_Frame_t f;
		memset(&f, 0, sizeof f);
		f.size = sizeof f;
		Perf_PartBegin(o, PERF_PART_LOAD); // (nothing without the log's second level)
		Perf_PartEnd(o, PERF_PART_LOAD);
		CHECK(o->perf.pass.n[PERF_PART_LOAD] == 0);
		o->perf.watch = 1;
		memset(&o->perf.sum, 0, sizeof o->perf.sum);
		memset(&o->perf.worst, 0, sizeof o->perf.worst);
		for(i = 0; i < 2; i++)
		{ // 1 ms of the programs, 3 (then 5) ms of loading in two files, 2 ms of painting, one part left open
			Perf_Begin(o);
			gNs += 1000000;
			Perf_PartBegin(o, PERF_PART_LOAD);
			gNs += 1000000 + (int64_t)i * 2000000;
			Perf_PartEnd(o, PERF_PART_LOAD);
			Perf_PartBegin(o, PERF_PART_LOAD);
			gNs += 2000000;
			Perf_PartEnd(o, PERF_PART_LOAD);
			Perf_PartBegin(o, PERF_PART_PAINT);
			gNs += 2000000;
			Perf_PartEnd(o, PERF_PART_PAINT);
			Perf_PartEnd(o, PERF_PART_SOUND); // (an end without a beginning counts nothing)
			Perf_PartBegin(o, PERF_PART_JOB); // (left open: an error jumped out of it)
			Perf_End(o, &f);
			gNs += 1000000;
		}
		CHECK(o->perf.sum.all == 14000000 && o->perf.sum.ns[PERF_PART_LOAD] == 8000000 && o->perf.sum.n[PERF_PART_LOAD] == 4);
		CHECK(o->perf.sum.ns[PERF_PART_PAINT] == 4000000 && o->perf.sum.n[PERF_PART_PAINT] == 2 && o->perf.sum.n[PERF_PART_SOUND] == 0);
		CHECK(o->perf.sum.n[PERF_PART_JOB] == 0 && o->perf.partT0[PERF_PART_JOB] == 0);
		CHECK(o->perf.worst.all == 8000000 && o->perf.worst.ns[PERF_PART_LOAD] == 5000000 && o->perf.worst.n[PERF_PART_LOAD] == 2);
		o->perf.watch = 0;
	}
	// the bitmaps' pixels
	m = BmpMgr_New(4);
	CHECK(m && BmpMgr_Bytes(m) == 0);
	if(m)
	{
		BmpMgr_Create(m, 1, 10, 20, PM_ARGB32);
		BmpMgr_Create(m, 3, 4, 4, PM_ARGB32);
		CHECK(BmpMgr_Bytes(m) == 10 * 20 * 4 + 4 * 4 * 4);
		BmpMgr_FreeSlot(m, 1);
		CHECK(BmpMgr_Bytes(m) == 4 * 4 * 4);
		BmpMgr_Delete(m);
	}
	EngineDown(o);
	// a host of the interface 1.0 has no such member: the memory is not known
	gMem = (uint64_t)100 * 1024 * 1024;
	o = EngineUp(&kOldMachine);
	if(!o)
		return;
	CHECK(Host_ProcessMemory(o) == 0);
	EngineDown(o);
	o = EngineUp(NULL);
	if(!o)
		return;
	CHECK(Host_ProcessMemory(o) == 0);
	EngineDown(o);
}

static void TestLine(void)
{
	PerfFigures_t g;
	char line[512];
	printf("line\n");
	memset(&g, 0, sizeof g);
	g.valid = 1;
	g.fps10 = 599;
	g.drawn10 = 241;
	g.frameUs = 16694;
	g.frameMaxUs = 33410;
	g.engineUs = 3250;
	g.engineMaxUs = 12049;
	g.busy = 19;
	g.memKb = 182 * 1024 + 512;
	g.peakKb = 190 * 1024;
	g.bmpKb = 64 * 1024;
	g.voices = 3;
	g.aheadMinMs = 42;
	g.aheadMaxMs = 58;
	g.dropped = 0;
	g.starved = 128;
	g.diskKbs = 1234;
	g.diskReads = 14;
	g.diskUs = 3140;
	Perf_Line(&g, line, sizeof line);
	CHECK(strcmp(line, "59.9 fps, 24.1 drawn; frame 16.6 ms, longest 33.4; engine 3.2 ms, longest 12.0, 19 %; memory 182 MB (most 190), bitmaps 64 MB; "
					   "sound 3 voices, 42 to 58 ms ahead, 0 dropped, 128 starved; disk 1234 KB/s, 14 reads, 3.1 ms") == 0);
	g.memKb = 0;
	Perf_Line(&g, line, sizeof line);
	CHECK(strstr(line, "; memory not known, bitmaps 64 MB; ") != NULL);
	Perf_Line(&g, line, 8); // (cut, and ended)
	CHECK(strcmp(line, "59.9 fp") == 0);
}

static void TestPanel(void)
{
	OpenBGI_t* o = EngineUp(&kMachine);
	const uint32_t* pix;
	int w = 0, h = 0, changed = 0;
	printf("panel\n");
	if(!o)
		return;
	gMem = (uint64_t)100 * 1024 * 1024;
	Perf_Set(o, 1, 0);
	Frame(o, 10000, 2000, 1);
	CHECK(!Perf_Picture(o, 400, &w, &h, &changed) && !changed); // nothing to show yet
	HalfSecond(o);
	// 400 pixels an inch: a cell of 31, 40 half cells and the edges wide; three lines, a graph, a line, a graph, four lines high
	pix = Perf_Picture(o, 400, &w, &h, &changed);
	CHECK(pix && changed && w == 40 * 15 + 2 * 15 && h == 2 * 15 + 9 * 38 + 2 * (77 + 2 * 10));
	{ // the engine's own line: its version, the fast rows of this build, the threads of the processors
		char line[96];
		Perf_Engine(o, line, sizeof line);
		CHECK(strncmp(line, "OpenBGI " OPENBGI_VERSION "  SIMD ", 14 + strlen(OPENBGI_VERSION)) == 0 && strstr(line, kBlitRows.name) && strstr(line, "  threads 1 of "));
	}
	if(pix)
	{
		CHECK(pix[0] == 0x0c1016 && pix[(size_t)(h - 1) * (size_t)w + (size_t)(w - 1)] == 0x0c1016);
		// the frame graph (at 15, 139; 600 x 77): the newest frame's bar at its right end, 10 ms of 50 high, in the colour of a fast frame
		CHECK(pix[(size_t)(139 + 76) * (size_t)w + 614] == 0x48c468 && pix[(size_t)(139 + 76 - 14) * (size_t)w + 614] == 0x48c468);
		CHECK(pix[(size_t)(139 + 76 - 16) * (size_t)w + 614] == 0x1a222c && pix[(size_t)(139 + 76) * (size_t)w + 615] == 0x0c1016);
		// .. and the lines at a sixtieth and a thirtieth of a second across it
		CHECK(pix[(size_t)(139 + 77 - 25) * (size_t)w + 20] == 0x3c4a58 && pix[(size_t)(139 + 77 - 51) * (size_t)w + 20] == 0x3c4a58);
		// the memory graph (at 15, 274): 100 MB of 128, in the memory's colour
		CHECK(pix[(size_t)(274 + 76) * (size_t)w + 614] == 0x4f8dff && pix[(size_t)(274 + 77 - 60) * (size_t)w + 614] == 0x4f8dff);
		CHECK(pix[(size_t)(274 + 77 - 62) * (size_t)w + 614] == 0x1a222c);
	}
	// the same figures: the same picture, not made anew
	CHECK(Perf_Picture(o, 400, &w, &h, &changed) == pix && !changed);
	Frame(o, 10000, 2000, 1);
	CHECK(Perf_Picture(o, 400, &w, &h, &changed) == pix && !changed);
	// new figures: anew; a slow frame's bar is in the colour of one
	Frame(o, 40000, 2000, 1);
	HalfSecond(o);
	pix = Perf_Picture(o, 400, &w, &h, &changed);
	CHECK(pix && changed);
	// another density: another size; a small one has a cell of 12 at least
	pix = Perf_Picture(o, 160, &w, &h, &changed);
	CHECK(pix && changed && w == 40 * 6 + 2 * 6 && h == 2 * 6 + 9 * 15 + 2 * (30 + 2 * 4));
	pix = Perf_Picture(o, 96, &w, &h, &changed);
	CHECK(pix && !changed && w == 40 * 6 + 2 * 6);
	// off: none; the run's end frees it
	Perf_Set(o, 0, 0);
	CHECK(!Perf_Picture(o, 160, &w, &h, &changed) && !changed);
	Perf_Stop(o);
	CHECK(!o->perf.pix && !o->perf.glyphs);
	EngineDown(o);
}

static const uint8_t* At(const OpenBGI_Frame_t* f, int x, int y)
{
	return f->layer + (size_t)y * (size_t)f->layerPitch + (size_t)x * 4;
}

static void TestLayer(void)
{
	OpenBGI_t* o = EngineUp(&kMachine);
	OpenBGI_Surface_t s;
	OpenBGI_Frame_t f;
	const OverlayControl_t *m, *d, *st;
	int px, py, i;
	printf("layer\n");
	if(!o)
		return;
	memset(&s, 0, sizeof s);
	s.size = sizeof s;
	s.width = SW;
	s.height = SH;
	s.picture.x = 480;
	s.picture.w = 1440;
	s.picture.h = SH;
	s.dpi = 400;
	OpenBGI_SetSurface(o, &s);
	memset(&f, 0, sizeof f);
	f.size = sizeof f;
	Overlay_Frame(o, &f);
	m = &o->overlay.c[OVERLAY_MENU];
	d = &o->overlay.c[OVERLAY_DPAD];
	st = &o->overlay.c[OVERLAY_STICK];
	// 2 mm to the right of the settings button and of where the D-pad and the stick are, 2 mm under the top
	px = m->x + m->side;
	if(d->x + d->side > px)
		px = d->x + d->side;
	if(st->x + st->side > px)
		px = st->x + st->side;
	px += 31;
	py = 31;
	CHECK(px > 300 && px < 600);
	CHECK(f.layer && f.layerChanges == -1 && At(&f, px + 2, py + 2)[3] == 0 && !o->overlay.perfShown);
	// on, and no figures yet: nothing
	Perf_Set(o, 1, 0);
	Frame(o, 10000, 2000, 1);
	memset(&f, 0, sizeof f);
	Overlay_Frame(o, &f);
	CHECK(f.layerChanges == 0 && At(&f, px + 2, py + 2)[3] == 0);
	// figures: the panel there, a little clear, and that part handed over
	HalfSecond(o);
	memset(&f, 0, sizeof f);
	Overlay_Frame(o, &f);
	CHECK(o->overlay.perfShown && o->overlay.perfX == px && o->overlay.perfY == py && o->overlay.perfW == 630 && o->overlay.perfH == 566);
	CHECK(At(&f, px + 2, py + 2)[0] == 0x16 && At(&f, px + 2, py + 2)[1] == 0x10 && At(&f, px + 2, py + 2)[2] == 0x0c && At(&f, px + 2, py + 2)[3] == 0xd8);
	CHECK(At(&f, px + 629, py + 565)[3] == 0xd8 && At(&f, px + 630, py + 565)[3] == 0 && At(&f, px + 629, py + 566)[3] == 0 && At(&f, px - 1, py + 2)[3] == 0);
	CHECK(f.layerChanges == 1 && f.layerChange.x == px && f.layerChange.y == py && f.layerChange.w == 630 && f.layerChange.h == 566);
	// frames without new figures hand nothing over
	for(i = 0; i < 10; i++)
	{
		Frame(o, 10000, 2000, 1);
		memset(&f, 0, sizeof f);
		Overlay_Frame(o, &f);
		CHECK(f.layerChanges == 0);
	}
	// new figures: the panel's part again
	HalfSecond(o);
	memset(&f, 0, sizeof f);
	Overlay_Frame(o, &f);
	CHECK(f.layerChanges == 1 && f.layerChange.x == px && f.layerChange.w == 630);
	// the settings button's square is its own still
	CHECK(At(&f, m->cx, m->cy)[3] != 0 && At(&f, m->cx, m->cy)[3] != 0xd8);
	// off: clear again
	Perf_Set(o, 0, 0);
	memset(&f, 0, sizeof f);
	Overlay_Frame(o, &f);
	CHECK(!o->overlay.perfShown && f.layerChanges == 1 && f.layerChange.x == px && At(&f, px + 2, py + 2)[3] == 0 && At(&f, px + 300, py + 300)[3] == 0);
	OpenBGI_SetSurface(o, NULL);
	EngineDown(o);
}

int main(void)
{
	OS_Init();
	TestOff();
	TestFigures();
	TestCounts();
	TestLine();
	TestPanel();
	TestLayer();
	OS_Shutdown();
	if(gFails)
	{
		printf("perf: %d checks failed\n", gFails);
		return 1;
	}
	printf("perf: all tests passed\n");
	return 0;
}
