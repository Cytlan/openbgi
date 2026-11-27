/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * perf.c - the performance display (src/include/bgi/perf.h): the frames
 *          timed, the figures of a span worked out, the panel painted
 *          and the log's line written; not part of the original engine
 *
 * The figures are of two spans that run side by side: the panel's, half
 * a second, and the log's, five seconds.  Each frame adds itself to both
 * when the engine has its picture (Perf_End); a span that is over is
 * turned into figures and begins anew.  What is counted elsewhere - the
 * files' reads here in o->perf, the sound's lost frames in its ring - is
 * a running count, of which a span notes the value at its start.
 *
 * The panel is a picture of text and two graphs.  The text is the
 * engine's own smooth text (the dialogs' typeface, at 2 mm a line for the
 * surface's density), of which the characters ' ' to '~' are rendered
 * once, when the panel is made: smooth text renders a character four
 * times its size each time it is drawn, and a panel of 250 characters
 * took 10 ms that way - a frame lost twice a second to the display that
 * is there to find such frames.  From the kept characters it takes
 * under a millisecond.  It is painted only when the layer asks for it
 * (Perf_Picture) and its figures are newer than it: a host without a
 * surface never pays for it.
 */
#include "bgi/perf.h"
#include "bgi/engine.h"
#include "bgi/display.h"
#include "bgi/host.h"
#include "bgi/sys.h"
#include "bgi/ui.h"
#include "bgi/inframe.h"
#include "bgi/launcher.h"
#include "bgi/gfx/typeface.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/gfx/bitmap.h"
#include "bgi/gfx/compositor.h"
#include "bgi/release.h"
#include "bgi/snd/sndout.h"
#include "bgi/snd/wavemaster.h"
#include "bgi/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_MS     1000000
#define CELL_MM10 20 // the text's cell height, tenths of a millimetre
#define CELL_MIN  12 // .. and in pixels at least, and at most
#define CELL_MAX  64
#define GRAPH_MS  50       // the frame graph's full height
#define COL_BACK  0x0c1016 // the panel,
#define COL_GRAPH 0x1a222c // .. a graph's ground,
#define COL_GUIDE 0x3c4a58 // .. the lines at 60 and 30 frames a second in it,
#define COL_TEXT  0xe6ebf2 // .. the text,
#define COL_DIM   0x8e9aaa // .. the last line,
#define COL_GOOD  0x48c468 // .. a frame within a sixtieth of a second,
#define COL_SLOW  0xe2c044 // .. within a thirtieth,
#define COL_BAD   0xe2503c // .. longer,
#define COL_MEM   0x4f8dff // .. the memory

// the host's fine clock, or its coarse one where it has no fine one
static int64_t Now(OpenBGI_t* o)
{
	int64_t ns = 0;
	if(Clock_PerfNs(o, &ns))
		return ns;
	return (int64_t)Clock_HostMs(o) * NS_MS;
}

// ---- on and off -------------------------------------------------------------------------------------

int Perf_On(OpenBGI_t* o)
{
	return o->perf.on;
}

// a setting of the "launcher" object, written with the others
static void Keep(OpenBGI_t* o, const char* key, const char* value)
{
	Engine_CfgSet(o, "", key, 0, value);
	if(o->launcher.running)
		Launcher_SettingsChanged(o); // (its frames write them)
	else
		Engine_SaveGames(o);
}

void Perf_Set(OpenBGI_t* o, int on, int keep)
{
	PerfState_t* p = &o->perf;
	on = on != 0;
	if(p->on != on)
	{ // nothing of an earlier time in the figures
		p->on = on;
		if(on)
		{ // what runs, once, before the figures
			char line[96];
			Perf_Engine(o, line, sizeof line);
			fprintf(stderr, "openbgi: perf: %s\n", line);
		}
		p->begin = p->last = 0;
		p->at = p->count = 0;
		p->memAt = p->memCount = 0;
		p->peakKb = 0;
		memset(&p->panel, 0, sizeof p->panel);
		memset(&p->log, 0, sizeof p->log);
		memset(&p->shown, 0, sizeof p->shown);
		p->changed = 1;
	}
	if(keep)
		Keep(o, "perf", on ? "on" : NULL);
}

int Perf_Limit(OpenBGI_t* o)
{
	return MainWin_FrameLimits(o) ? !o->perf.freeRun : -1;
}

void Perf_SetLimit(OpenBGI_t* o, int on, int keep)
{
	o->perf.freeRun = !on;
	MainWin_SetFrameLimit(o, on);
	if(keep)
		Keep(o, "frames", on ? NULL : "free");
}

/* What the settings say, once they are read: the run's programs are up.
 * Not in a run nobody sees (a first picture for the launcher), nor in the
 * settings' own engine: the figures are the game's. */
static void Load(OpenBGI_t* o)
{
	PerfState_t* p = &o->perf;
	const char* env = getenv("BGI_PERF");
	char value[16];
	p->loaded = 1;
	if(o->engine.optPreview || o->settings.game)
		return;
	p->forced = env && env[0] && strcmp(env, "0") != 0;
	p->watch = env && strcmp(env, "2") == 0;
	Perf_Set(o, p->forced || (Engine_CfgGet(o, "", "perf", 0, value, sizeof value) > 0 && strcmp(value, "on") == 0), 0);
	Perf_SetLimit(o, !(Engine_CfgGet(o, "", "frames", 0, value, sizeof value) > 0 && strcmp(value, "free") == 0), 0); // (a run before may have left the host free)
}

void Perf_Stop(OpenBGI_t* o)
{
	PerfState_t* p = &o->perf;
	BGI_Free(p->glyphs);
	p->glyphs = NULL;
	BGI_Free(p->pix);
	p->pix = NULL;
	p->w = p->h = p->cell = 0;
}

// ---- the spans --------------------------------------------------------------------------------------

static void SpanOpen(OpenBGI_t* o, PerfSpan_t* s, int64_t now)
{
	uint32_t ahead;
	memset(s, 0, sizeof *s);
	s->t0 = now;
	s->aheadMin = 0xffffffffu;
	s->diskBytes = o->perf.diskBytes;
	s->diskReads = o->perf.diskReads;
	s->diskNs = o->perf.diskNs;
	SndOut_Stats(o, &ahead, &s->dropped, &s->starved);
}

static void SpanAdd(PerfSpan_t* s, int64_t period, int64_t engine, int drawn, uint32_t ahead)
{
	s->frames++;
	s->drawn += drawn != 0;
	s->engineNs += engine;
	if(period > s->maxPeriodNs)
		s->maxPeriodNs = period;
	if(engine > s->maxEngineNs)
		s->maxEngineNs = engine;
	if(ahead < s->aheadMin)
		s->aheadMin = ahead;
	if(ahead > s->aheadMax)
		s->aheadMax = ahead;
}

// the voices that play now
static int Voices(OpenBGI_t* o)
{
	WmVoiceInfo_t v;
	int bgm, i, n = 0;
	if(!Wm_MixRunning(o))
		return 0;
	for(bgm = 0; bgm < 2; bgm++)
		for(i = 0; Wm_DebugVoice(o, bgm, i, &v); i++)
			n += v.playing && !v.paused;
	return n;
}

// a span that ends now, as figures
static void Figures(OpenBGI_t* o, const PerfSpan_t* s, int64_t now, PerfFigures_t* g)
{
	PerfState_t* p = &o->perf;
	int64_t span = now - s->t0;
	uint64_t mem = Host_ProcessMemory(o), disk = p->diskBytes - s->diskBytes;
	uint32_t ahead, dropped, starved;
	memset(g, 0, sizeof *g);
	if(span <= 0 || !s->frames)
		return;
	g->valid = 1;
	g->fps10 = (int)((int64_t)s->frames * 10 * 1000 * NS_MS / span);
	g->drawn10 = (int)((int64_t)s->drawn * 10 * 1000 * NS_MS / span);
	g->frameUs = (int)(span / s->frames / 1000);
	g->frameMaxUs = (int)(s->maxPeriodNs / 1000);
	g->engineUs = (int)(s->engineNs / s->frames / 1000);
	g->engineMaxUs = (int)(s->maxEngineNs / 1000);
	g->busy = (int)(s->engineNs * 100 / span);
	g->memKb = (uint32_t)(mem / 1024 > 0xffffffffu ? 0xffffffffu : mem / 1024);
	if(g->memKb > p->peakKb)
		p->peakKb = g->memKb;
	g->peakKb = p->peakKb;
	g->bmpKb = o->gfx.bmpMgr ? (uint32_t)(BmpMgr_Bytes(o->gfx.bmpMgr) / 1024) : 0;
	g->voices = Voices(o);
	g->aheadMinMs = (int)((uint64_t)(s->aheadMin == 0xffffffffu ? 0 : s->aheadMin) * 1000 / SNDOUT_RATE);
	g->aheadMaxMs = (int)((uint64_t)s->aheadMax * 1000 / SNDOUT_RATE);
	SndOut_Stats(o, &ahead, &dropped, &starved);
	g->dropped = dropped - s->dropped;
	g->starved = starved - s->starved;
	g->diskKbs = (uint32_t)(disk * 1000 * NS_MS / 1024 / (uint64_t)span);
	g->diskReads = p->diskReads - s->diskReads;
	g->diskUs = (int)((p->diskNs - s->diskNs) / 1000);
}

/* What had the picture painted again since the last time, a line each
 * (BGI_PERF=2): how often, the object's class and priority, and its
 * rectangle.  The class and the priority are those of the object's sort
 * key, which is what the compositor is told. */
static void Redraws(OpenBGI_t* o)
{
	static const char* const kClass[16] = {"background", "map", "sprite", "window", "particles", "rain", "effector", "filter", "virtual object", "group",
		"knob", "class 11", "class 12", "class 13", "class 14", "class 15"};
	Compositor_t* c = o->gfx.gfx ? o->gfx.gfx->comp : NULL;
	int i;
	if(!c)
		return;
	if(c->watchOn)
	{
		if(c->watchAll)
			fprintf(stderr, "openbgi: perf: redraw: %u times: the whole picture\n", (unsigned)c->watchAll);
		for(i = 0; i < COMP_WATCH; i++)
		{
			const CompWatch_t* w = &c->watch[i];
			if(w->count)
				fprintf(stderr, "openbgi: perf: redraw: %u times: %s, priority %u, %d x %d, last at %d, %d\n", (unsigned)w->count, kClass[(w->key >> 16) & 15],
					(unsigned)(w->key >> 20), (int)(w->r.r - w->r.l + 1), (int)(w->r.b - w->r.t + 1), (int)w->r.l, (int)w->r.t);
		}
		if(c->watchMore)
			fprintf(stderr, "openbgi: perf: redraw: %u times: others\n", (unsigned)c->watchMore);
	}
	memset(c->watch, 0, sizeof c->watch);
	c->watchAll = c->watchMore = 0;
	c->watchOn = 1;
}

/* The engine itself: its version, the blitters' fast rows of this build
 * (SSE2, NEON or none) and the threads a frame is painted on, of the
 * processors the machine has. */
void Perf_Engine(OpenBGI_t* o, char* out, size_t n)
{
	int cpus = Host_CpuCount(o);
	snprintf(out, n, "OpenBGI %s  SIMD %s  threads %d of %d", OPENBGI_VERSION, kBlitRows.name, Compositor_Threads(o), cpus < 1 ? 1 : cpus);
}

void Perf_Line(const PerfFigures_t* g, char* out, size_t n)
{
	char mem[48];
	if(g->memKb)
		snprintf(mem, sizeof mem, "memory %u MB (most %u)", (unsigned)(g->memKb / 1024), (unsigned)(g->peakKb / 1024));
	else
		snprintf(mem, sizeof mem, "memory not known");
	snprintf(out, n,
		"%d.%d fps, %d.%d drawn; frame %d.%d ms, longest %d.%d; engine %d.%d ms, longest %d.%d, %d %%; %s, bitmaps %u MB; "
		"sound %d voices, %d to %d ms ahead, %u dropped, %u starved; disk %u KB/s, %u reads, %d.%d ms",
		g->fps10 / 10, g->fps10 % 10, g->drawn10 / 10, g->drawn10 % 10, g->frameUs / 1000, g->frameUs % 1000 / 100, g->frameMaxUs / 1000,
		g->frameMaxUs % 1000 / 100, g->engineUs / 1000, g->engineUs % 1000 / 100, g->engineMaxUs / 1000, g->engineMaxUs % 1000 / 100, g->busy, mem,
		(unsigned)(g->bmpKb / 1024), g->voices, g->aheadMinMs, g->aheadMaxMs, (unsigned)g->dropped, (unsigned)g->starved, (unsigned)g->diskKbs,
		(unsigned)g->diskReads, g->diskUs / 1000, g->diskUs % 1000 / 100);
}

// ---- the frames -------------------------------------------------------------------------------------

void Perf_Begin(OpenBGI_t* o)
{
	PerfState_t* p = &o->perf;
	if(!p->loaded && o->engine.stage == ENGINE_STAGE_VM)
		Load(o);
	if(!p->on)
		return;
	p->last = p->begin;
	p->begin = Now(o);
}

void Perf_End(OpenBGI_t* o, const OpenBGI_Frame_t* f)
{
	PerfState_t* p = &o->perf;
	int64_t now, engine, period = 0;
	uint32_t ahead, dropped, starved;
	if(!p->on || !p->begin)
		return;
	now = Now(o);
	engine = now - p->begin;
	if(engine < 0)
		engine = 0;
	if(p->last)
	{ // the period into the graph's ring
		int64_t tenths;
		period = p->begin - p->last;
		if(period < 0)
			period = 0;
		tenths = period / (NS_MS / 10);
		p->period[p->at] = (uint16_t)(tenths > 0xffff ? 0xffff : tenths);
		p->at = (p->at + 1) % PERF_FRAMES;
		if(p->count < PERF_FRAMES)
			p->count++;
	}
	if(!p->panel.t0)
	{ // the first frame: the spans begin with it
		SpanOpen(o, &p->panel, p->begin);
		SpanOpen(o, &p->log, p->begin);
	}
	SndOut_Stats(o, &ahead, &dropped, &starved);
	SpanAdd(&p->panel, period, engine, f->changes != 0, ahead);
	SpanAdd(&p->log, period, engine, f->changes != 0, ahead);
	if(now - p->panel.t0 >= (int64_t)PERF_PANEL_MS * NS_MS)
	{
		Figures(o, &p->panel, now, &p->shown);
		p->mem[p->memAt] = p->shown.memKb;
		p->memAt = (p->memAt + 1) % PERF_MEMS;
		if(p->memCount < PERF_MEMS)
			p->memCount++;
		p->changed = 1;
		SpanOpen(o, &p->panel, now);
	}
	if(now - p->log.t0 >= (int64_t)PERF_LOG_MS * NS_MS)
	{
		PerfFigures_t g;
		char line[512];
		Figures(o, &p->log, now, &g);
		if(g.valid)
		{
			Perf_Line(&g, line, sizeof line);
			fprintf(stderr, "openbgi: perf: %s\n", line);
		}
		if(p->watch)
			Redraws(o);
		SpanOpen(o, &p->log, now);
	}
}

// ---- the panel --------------------------------------------------------------------------------------

static void Fill(PerfState_t* p, int x, int y, int w, int h, uint32_t rgb)
{
	Ui_FillRect(p->pix, p->w, p->w, p->h, x, y, w, h, rgb);
}

/* The characters ' ' .. '~' of the dialogs' typeface at the panel's cell
 * height, each rendered once in white on black into a cell of its own:
 * what is kept is how much of each pixel the character covers, and how
 * far it moves the pen.  Nothing where no font can be had. */
static void Glyphs(OpenBGI_t* o)
{
	PerfState_t* p = &o->perf;
	Typeface_t* face = Inframe_OpenFontSized(o, p->cell);
	size_t cell = (size_t)p->cell, area = cell * cell;
	uint32_t* tmp;
	int i;
	size_t k;
	if(!face)
		return;
	p->glyphs = (uint8_t*)BGI_Calloc(PERF_GLYPHS * area);
	tmp = (uint32_t*)BGI_Alloc(area * 4);
	for(i = 0; i < PERF_GLYPHS; i++)
	{
		char ch[2] = {(char)(' ' + i), 0};
		int pen;
		memset(tmp, 0, area * 4);
		pen = Typeface_SmoothText(face, tmp, p->cell, p->cell, p->cell, 0, 0, ch, 0xffffff, 0);
		p->advance[i] = (uint8_t)(pen < 0 ? 0 : pen > p->cell ? p->cell
															  : pen);
		for(k = 0; k < area; k++)
			p->glyphs[(size_t)i * area + k] = (uint8_t)(tmp[k] & 0xff);
	}
	BGI_Free(tmp);
	Typeface_Close(face);
}

// `text` with its cell's top left at (x, y) of the panel, in `rgb`, from the kept characters
static void Text(PerfState_t* p, int x, int y, const char* text, uint32_t rgb)
{
	size_t cell = (size_t)p->cell;
	if(!p->glyphs)
		return;
	for(; *text; text++)
	{
		int i = (unsigned char)*text - ' ', gx, gy;
		const uint8_t* g;
		if(i < 0 || i >= PERF_GLYPHS)
			i = '?' - ' ';
		g = p->glyphs + (size_t)i * cell * cell;
		for(gy = 0; gy < p->cell && y + gy < p->h; gy++)
			for(gx = 0; gx < p->cell && x + gx < p->w; gx++)
			{
				uint32_t a = g[(size_t)gy * cell + (size_t)gx], *d, inv;
				if(!a || x + gx < 0 || y + gy < 0)
					continue;
				d = p->pix + (size_t)(y + gy) * (size_t)p->w + (size_t)(x + gx);
				inv = 255 - a;
				*d = ((((rgb >> 16) & 0xff) * a + ((*d >> 16) & 0xff) * inv) / 255) << 16 | ((((rgb >> 8) & 0xff) * a + ((*d >> 8) & 0xff) * inv) / 255) << 8 |
					((rgb & 0xff) * a + (*d & 0xff) * inv) / 255;
			}
		x += p->advance[i];
	}
}

// the last frames' periods as bars, the newest at the right; lines where a sixtieth and a thirtieth of a second are
static void FrameGraph(PerfState_t* p, int x, int y, int w, int h)
{
	int bar = w / PERF_FRAMES > 0 ? w / PERF_FRAMES : 1, n = w / bar < p->count ? w / bar : p->count, i;
	Fill(p, x, y, w, h, COL_GRAPH);
	for(i = 0; i < n; i++)
	{ // i frames back from the newest
		int tenths = p->period[(p->at - 1 - i + 2 * PERF_FRAMES) % PERF_FRAMES], bh = tenths * h / (GRAPH_MS * 10);
		uint32_t rgb = tenths <= 180 ? COL_GOOD : tenths <= 345 ? COL_SLOW
																: COL_BAD;
		if(bh > h)
			bh = h;
		if(bh < 1)
			bh = 1;
		Fill(p, x + w - (i + 1) * bar, y + h - bh, bar, bh, rgb);
	}
	Fill(p, x, y + h - 167 * h / (GRAPH_MS * 10), w, 1, COL_GUIDE);
	Fill(p, x, y + h - 333 * h / (GRAPH_MS * 10), w, 1, COL_GUIDE);
}

// the memory over the last minute, from nothing up to the next 32 MB over the most there was
static void MemGraph(PerfState_t* p, int x, int y, int w, int h)
{
	int bar = w / PERF_MEMS > 0 ? w / PERF_MEMS : 1, n = w / bar < p->memCount ? w / bar : p->memCount, i;
	uint32_t top = (p->peakKb / (32 * 1024) + 1) * 32 * 1024;
	Fill(p, x, y, w, h, COL_GRAPH);
	for(i = 0; i < n; i++)
	{
		uint32_t kb = p->mem[(p->memAt - 1 - i + 2 * PERF_MEMS) % PERF_MEMS];
		int bh = (int)((uint64_t)kb * (uint64_t)h / top);
		if(kb && bh < 1)
			bh = 1;
		Fill(p, x + w - (i + 1) * bar, y + h - bh, bar, bh, COL_MEM);
	}
}

static void Paint(OpenBGI_t* o, int dpi)
{
	PerfState_t* p = &o->perf;
	const PerfFigures_t* g = &p->shown;
	const OpenBGI_Surface_t* s = &o->overlay.surface;
	int cell = dpi * CELL_MM10 / 254, pad, line, gh, gap, w, h, x, y;
	char t[96];
	if(cell < CELL_MIN)
		cell = CELL_MIN;
	if(cell > CELL_MAX)
		cell = CELL_MAX;
	pad = cell / 2;
	line = cell + cell / 4;
	gh = cell * 5 / 2;
	gap = cell / 3;
	w = PERF_COLS * (cell / 2) + 2 * pad;
	h = 2 * pad + 9 * line + 2 * (gh + 2 * gap);
	if(cell != p->cell || !p->pix)
	{
		Perf_Stop(o);
		p->cell = cell;
		p->w = w;
		p->h = h;
		p->pix = (uint32_t*)BGI_Calloc((size_t)w * (size_t)h * 4);
		Glyphs(o); // (none where no font can be had: the graphs alone)
	}
	Fill(p, 0, 0, w, h, COL_BACK);
	x = pad;
	y = pad;
	snprintf(t, sizeof t, "FPS %d.%d   drawn %d.%d/s", g->fps10 / 10, g->fps10 % 10, g->drawn10 / 10, g->drawn10 % 10);
	Text(p, x, y, t, COL_TEXT);
	y += line;
	snprintf(t, sizeof t, "frame  %d.%d ms   max %d.%d ms", g->frameUs / 1000, g->frameUs % 1000 / 100, g->frameMaxUs / 1000, g->frameMaxUs % 1000 / 100);
	Text(p, x, y, t, COL_TEXT);
	y += line;
	snprintf(t, sizeof t, "engine %d.%d ms   max %d.%d ms  %d %%", g->engineUs / 1000, g->engineUs % 1000 / 100, g->engineMaxUs / 1000,
		g->engineMaxUs % 1000 / 100, g->busy);
	Text(p, x, y, t, COL_TEXT);
	y += line + gap;
	FrameGraph(p, x, y, w - 2 * pad, gh);
	y += gh + gap;
	if(g->memKb)
		snprintf(t, sizeof t, "MEM %u MB  peak %u  bitmaps %u MB", (unsigned)(g->memKb / 1024), (unsigned)(g->peakKb / 1024), (unsigned)(g->bmpKb / 1024));
	else
		snprintf(t, sizeof t, "MEM ?   bitmaps %u MB", (unsigned)(g->bmpKb / 1024));
	Text(p, x, y, t, COL_TEXT);
	y += line + gap;
	MemGraph(p, x, y, w - 2 * pad, gh);
	y += gh + gap;
	snprintf(t, sizeof t, "SND %d voices   %d to %d ms ahead", g->voices, g->aheadMinMs, g->aheadMaxMs);
	Text(p, x, y, t, COL_TEXT);
	y += line;
	snprintf(t, sizeof t, "    dropped %u   starved %u", (unsigned)g->dropped, (unsigned)g->starved);
	Text(p, x, y, t, g->dropped || g->starved ? COL_BAD : COL_TEXT);
	y += line;
	snprintf(t, sizeof t, "DISK %u.%u MB/s  %u reads  %d.%d ms", (unsigned)(g->diskKbs / 1024), (unsigned)(g->diskKbs % 1024 * 10 / 1024), (unsigned)g->diskReads,
		g->diskUs / 1000, g->diskUs % 1000 / 100);
	Text(p, x, y, t, COL_TEXT);
	y += line;
	snprintf(t, sizeof t, "%dx%d on %dx%d  %d dpi", o->screen.pic.w, o->screen.pic.h, s->width, s->height, dpi);
	Text(p, x, y, t, COL_DIM);
	y += line;
	Perf_Engine(o, t, sizeof t);
	Text(p, x, y, t, COL_DIM);
}

const uint32_t* Perf_Picture(OpenBGI_t* o, int dpi, int* w, int* h, int* changed)
{
	PerfState_t* p = &o->perf;
	int cell = dpi * CELL_MM10 / 254;
	cell = cell < CELL_MIN ? CELL_MIN : cell > CELL_MAX ? CELL_MAX
														: cell;
	*changed = 0;
	if(!p->on || !p->shown.valid)
		return NULL;
	if(p->changed || !p->pix || cell != p->cell)
	{
		Paint(o, dpi);
		p->changed = 0;
		*changed = 1;
	}
	*w = p->w;
	*h = p->h;
	return p->pix;
}
