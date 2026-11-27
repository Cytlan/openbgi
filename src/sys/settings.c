/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * settings.c - the settings over a running game (src/include/bgi/settings.h):
 *              the panel's engine made and freed, its frames, its
 *              picture, the input it takes, and what its program reads
 *              and sets of the game's engine; not part of the original
 *              engine
 */
#include "bgi/settings.h"
#include "bgi/overlay.h"
#include "bgi/launcher.h"
#include "bgi/engine.h"
#include "bgi/request.h"
#include "bgi/input.h"
#include "bgi/display.h"
#include "bgi/gfx/bitmap.h"
#include "bgi/core.h"

#include <stdio.h>
#include <string.h>

int Settings_Up(OpenBGI_t* o)
{
	return o->settings.panel != NULL;
}

/* The panel's files: none.  It runs "in" the root of a drive that is
 * nowhere, where the one file it reads - its program, which the engine
 * carries - is looked for and nothing is found, written or listed. */
static void NoDir(void* user, char* buf, size_t n)
{
	(void)user;
	if(n)
		snprintf(buf, n, "c:\\");
}

static int NoDrive(void* user, const char* root)
{
	(void)user;
	(void)root;
	return 1; // (ready, and empty)
}

int Settings_Open(OpenBGI_t* o)
{
	const OpenBGI_Surface_t* s = &o->overlay.surface;
	OpenBGI_Host_t host;
	OpenBGI_Files_t files;
	OpenBGI_t* p;
	const uint8_t* program;
	uint32_t size = 0;
	if(o->settings.panel)
		return 1;
	if(o->settings.game || s->width <= 0 || s->height <= 0)
		return 0; // (a panel has none of its own; without a surface there is no layer to show one in)
	program = Launcher_Program(o, &size);
	if(!program)
		return 0;
	// the game's clocks and fonts, and nothing else of its host
	memset(&host, 0, sizeof host);
	host.size = sizeof host;
	host.api = OPENBGI_API_VERSION;
	host.user = o->host.user;
	host.ticksMs = o->host.ticksMs;
	host.perfNs = o->host.perfNs;
	host.localTime = o->host.localTime;
	host.fontFiles = o->host.fontFiles;
	host.fontRenderer = o->host.fontRenderer;
	memset(&files, 0, sizeof files);
	files.size = sizeof files;
	files.exeDir = NoDir;
	files.volumeReady = NoDrive;
	host.files = &files;
	p = OpenBGI_New(&host);
	if(!p)
		return 0;
	p->settings.game = o;
	o->settings.panel = p;
	o->settings.close = 0;
	o->settings.shown = 0;
	o->settings.openMs = o->host.ticksMs(o->host.user);
	o->settings.timed = 0;
	memset(&o->settings.frame, 0, sizeof o->settings.frame);
	o->settings.frame.size = sizeof o->settings.frame;
	OpenBGI_TouchScale(p, s->dpi > 0 ? s->dpi : 160); // (its picture's pixels are the surface's)
	if(Engine_StartBuiltin(p, program, size) != OPENBGI_RUNNING)
	{
		Settings_Close(o);
		return 0;
	}
	OpenBGI_Focus(p, 1); // (its "window" is the one in front)
	Overlay_PanelChanged(o);
	return 1;
}

void Settings_Close(OpenBGI_t* o)
{
	OpenBGI_t* p = o->settings.panel;
	if(!p)
		return;
	BGI_Free(o->settings.error);
	o->settings.error = NULL;
	o->settings.panel = NULL;
	o->settings.close = 0;
	o->settings.closeGame = 0;
	o->settings.shown = 0;
	memset(&o->settings.frame, 0, sizeof o->settings.frame);
	OpenBGI_Stop(p);
	OpenBGI_Delete(p);
	Overlay_PanelChanged(o);
}

void Settings_Failed(OpenBGI_t* panel, const char* text)
{
	OpenBGI_t* g = panel->settings.game;
	if(!g || g->settings.panel != panel)
		return;
	BGI_Free(g->settings.error);
	g->settings.error = BGI_Strdup(text);
	g->settings.close = 1;
	g->settings.closeGame = 0;
}

void Settings_Frame(OpenBGI_t* o, OpenBGI_Frame_t* f)
{
	OpenBGI_t* p = o->settings.panel;
	OpenBGI_Request_t q;
	int result;
	if(!p)
		return;
	// a box the panel cannot draw (nothing of it should ask for one): said, and answered as "no"
	memset(&q, 0, sizeof q);
	q.size = sizeof q;
	while(OpenBGI_NextRequest(p, &q))
	{
		fprintf(stderr, "openbgi: the settings asked the host for something (request %d): it is not answered\n", (int)q.kind);
		OpenBGI_Answer(p, q.id, 0);
	}
	memset(&o->settings.frame, 0, sizeof o->settings.frame);
	o->settings.frame.size = sizeof o->settings.frame;
	result = OpenBGI_RunFrame(p, &o->settings.frame);
	o->settings.shown = 0;
	if(result != OPENBGI_RUNNING || o->settings.close)
	{
		int game = o->settings.closeGame;
		char* error = o->settings.error;
		o->settings.error = NULL;
		Settings_Close(o);
		if(error)
		{ // the panel's program stopped with a script error: said here, where there is a screen for it
			fprintf(stderr, "openbgi: the settings stopped with an error:\n%s\n", error);
			Request_Forget(o, Request_Box(o, error, "Error!!", OPENBGI_MB_ICONHAND, REQUEST_TREAT_BOX));
			BGI_Free(error);
		}
		else if(game)
			OpenBGI_Close(o); // as the window's close button: the game asks what it asks, and ends
		return;
	}
	if(o->settings.frame.idleMs < f->idleMs)
		f->idleMs = o->settings.frame.idleMs;
}

const uint8_t* Settings_Picture(OpenBGI_t* o, int* w, int* h, int* pitch, int* changes, const OpenBGI_Rect_t** change)
{
	const OpenBGI_Frame_t* f = &o->settings.frame;
	OpenBGI_t* p = o->settings.panel;
	*w = *h = *pitch = *changes = 0;
	*change = NULL;
	// (only a picture with alpha can lie over the game: one without is a program that has not said "EF 31" yet)
	if(!p || !f->pixels || f->bpp != 32 || p->screen.pic.mode != PM_ARGB32)
		return NULL;
	*w = f->width;
	*h = f->height;
	*pitch = f->pitch;
	if(!o->settings.timed)
	{ // (a number to watch on a slow device: the wait between the button and the panel)
		o->settings.timed = 1;
		fprintf(stderr, "openbgi: the settings were up after %u ms\n", (unsigned)(o->host.ticksMs(o->host.user) - o->settings.openMs));
	}
	if(!o->settings.shown)
	{
		*changes = f->changes;
		*change = f->change;
	}
	o->settings.shown = 1;
	return f->pixels;
}

// ---- the input ----------------------------------------------------------------------------------

int Settings_Touch(OpenBGI_t* o, int id, int phase, int x, int y)
{
	if(!o->settings.panel)
		return 0;
	OpenBGI_Touch(o->settings.panel, id, phase, x, y);
	return 1;
}

int Settings_Pad(OpenBGI_t* o, const OpenBGI_PadState_t* state)
{
	if(!o->settings.panel)
		return 0;
	Pad_Port(o->settings.panel, PAD_PORT_OWN, state);
	return 1;
}

int Settings_Key(OpenBGI_t* o, int vk, int down, int flags)
{
	if(!o->settings.panel)
		return 0;
	OpenBGI_Key(o->settings.panel, vk, down, flags);
	return 1;
}

// (a button where the panel's own pointer is: the host's place is one of the game's picture)
int Settings_Button(OpenBGI_t* o, int button, int down, int flags)
{
	OpenBGI_t* p = o->settings.panel;
	if(!p)
		return 0;
	OpenBGI_Button(p, button, down, p->input.pointerX, p->input.pointerY, flags);
	return 1;
}

// ---- what the program reads and sets ------------------------------------------------------------

static void Text(char* dst, size_t size, const char* s)
{
	if(size)
		snprintf(dst, size, "%s", s);
}

// the filters by the names the program knows them by: the four of every host, then the host's own
static const char* const kFilters[VIEW_FILTER_OWN] = {"game", "bicubic", "bilinear", "nearest"};

static const char* FilterName(OpenBGI_t* g, int i)
{
	if(i < 0 || !MainWin_ViewScales(g))
		return NULL; // (a host that shows the picture as it is has no filter to choose)
	return i < VIEW_FILTER_OWN ? kFilters[i] : MainWin_ViewFilterName(g, i - VIEW_FILTER_OWN);
}

static int OnOff(const char* value, int* on)
{
	if(!value || (strcmp(value, "on") != 0 && strcmp(value, "off") != 0))
		return 0;
	*on = strcmp(value, "on") == 0;
	return 1;
}

int Settings_Value(OpenBGI_t* o, const char* key, int i, char* dst, size_t size)
{
	OpenBGI_t* g = o->settings.game;
	char num[16];
	if(strcmp(key, "mode") == 0)
	{
		Text(dst, size, g ? "settings" : "launcher");
		return i == 0 ? 1 : -1;
	}
	if(!g)
		return 0;
	if(strcmp(key, "surface") == 0)
	{
		const OpenBGI_Surface_t* s = &g->overlay.surface;
		if(i < 0 || i > 2)
			return -1;
		snprintf(num, sizeof num, "%d", i == 0 ? s->width : i == 1 ? s->height
				: s->dpi > 0                                       ? s->dpi
																   : 160);
		Text(dst, size, num);
		return 1;
	}
	if(strcmp(key, "pad") == 0 || strcmp(key, "through") == 0)
	{
		Text(dst, size, (key[0] == 'p' ? Overlay_PadOn(g) : Overlay_Through(g)) ? "on" : "off");
		return i == 0 ? 1 : -1;
	}
	if(strcmp(key, "perf") == 0)
	{
		Text(dst, size, Perf_On(g) ? "on" : "off");
		return i == 0 ? 1 : -1;
	}
	if(strcmp(key, "limit") == 0)
	{ // (no value where the host cannot hold its frames: the row is not shown)
		int limit = Perf_Limit(g);
		Text(dst, size, limit < 0 ? "" : limit ? "on" : "off");
		return limit >= 0 && i == 0 ? 1 : -1;
	}
	if(strcmp(key, "filters") == 0)
	{
		const char* name = FilterName(g, i);
		Text(dst, size, name ? name : "");
		return name ? 1 : -1;
	}
	if(strcmp(key, "filter") == 0 || strcmp(key, "quality") == 0 || strcmp(key, "whole") == 0)
	{
		int vsize, filter, whole;
		const char* name;
		MainWin_ViewSettings(g, &vsize, &filter, &whole);
		name = FilterName(g, filter);
		if(i != 0 || !name)
			return -1;
		Text(dst, size, key[0] == 'f' ? name : key[0] == 'q' ? (MainWin_ViewHigh(g) ? "high" : "normal")
				: whole                                      ? "on"
															 : "off");
		return 1;
	}
	if(strcmp(key, "game") == 0)
	{
		Text(dst, size, g->launcher.running ? "launcher" : "game");
		return i == 0 ? 1 : -1;
	}
	if(strcmp(key, "panel") == 0)
		return -1; // (only set)
	return 0;
}

int Settings_Set(OpenBGI_t* o, const char* key, const char* value)
{
	OpenBGI_t* g = o->settings.game;
	int on = 0;
	if(strcmp(key, "mode") == 0)
		return 0; // (only read)
	if(!g)
		return -1;
	if(strcmp(key, "surface") == 0 || strcmp(key, "filters") == 0)
		return 0; // (only read)
	if(strcmp(key, "pad") == 0)
	{
		if(!OnOff(value, &on))
			return 0;
		Overlay_SetPad(g, on, 1);
		return 1;
	}
	if(strcmp(key, "through") == 0)
	{
		if(!OnOff(value, &on))
			return 0;
		Overlay_SetThrough(g, on);
		return 1;
	}
	if(strcmp(key, "perf") == 0)
	{
		if(!OnOff(value, &on))
			return 0;
		Perf_Set(g, on, 1);
		return 1;
	}
	if(strcmp(key, "limit") == 0)
	{
		if(!OnOff(value, &on) || Perf_Limit(g) < 0)
			return 0;
		Perf_SetLimit(g, on, 1);
		return 1;
	}
	if(strcmp(key, "filter") == 0 || strcmp(key, "quality") == 0 || strcmp(key, "whole") == 0)
	{
		int vsize, filter, whole, i;
		const char* name;
		if(!value || !MainWin_ViewScales(g))
			return 0;
		MainWin_ViewSettings(g, &vsize, &filter, &whole);
		if(key[0] == 'q')
		{
			if(strcmp(value, "high") != 0 && strcmp(value, "normal") != 0)
				return 0;
			MainWin_ViewSetHigh(g, strcmp(value, "high") == 0);
			return 1;
		}
		if(key[0] == 'w')
		{
			if(!OnOff(value, &on))
				return 0;
			MainWin_ViewApply(g, vsize, filter, on);
			return 1;
		}
		for(i = 0; (name = FilterName(g, i)) != NULL; i++)
			if(strcmp(name, value) == 0)
			{
				MainWin_ViewApply(g, vsize, i, whole);
				return 1;
			}
		return 0;
	}
	if(strcmp(key, "game") == 0 || strcmp(key, "panel") == 0)
	{
		if(!value || strcmp(value, "close") != 0)
			return 0;
		g->settings.close = 1; // (at the frame's end: this is the panel's own instruction)
		g->settings.closeGame = key[0] == 'g';
		return 1;
	}
	return -1;
}
