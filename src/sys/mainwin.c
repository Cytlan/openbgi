/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * mainwin.c - the main window as the engine asks for it: the host's
 *             (OpenBGI_Window_t of openbgi.h), or one nobody sees;
 *             interface in bgi/display.h
 *
 * Each call is the member of the host's table.  Without the table the
 * engine keeps the window itself, as openbgi.h describes it: there from
 * its creation to its destruction, where the engine put it and of the size
 * it gave it, on a desktop of 1024 x 768 pixels with no frame, never
 * minimised, every display mode taken; showing it gives it the focus and
 * hiding it takes the focus away, which OpenBGI_PumpEvents hands to the
 * input as the host's would be (MainWin_TakeFocus).  The window's
 * rectangle is kept either way, so that a host's table without a query
 * answers as that window would.
 *
 * The view.  A host that says how much room its screen has
 * (OpenBGI_Window_t.workArea) leaves the window's size on the screen to
 * this file: the scripts ask for client areas in the picture's pixels as
 * ever, and the window gets that area times the screen's scale - or the
 * settings' size - made smaller in proportion until the window with its
 * frame fits the work area.  The scripts are told of a desktop, a frame
 * and a window rectangle smaller by the scale, so that what they work
 * out from them (the centre of the screen, whether a size fits) holds;
 * the window's centre is where theirs is, times the scale.  The window
 * has a sizing border: the player's own size holds until the scripts
 * ask for another client area than they had, and the scripts hear of it
 * only when the game asked for the border ("81 66").  BGI_DPI in the
 * environment says the scale for a run (96: none).
 */
#include "bgi/display.h"
#include "bgi/engine.h"
#include "bgi/fs.h"
#include "bgi/host.h"
#include "bgi/icon.h"
#include "bgi/launcher.h"
#include "bgi/version.h"
#include "bgi/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOBODY_SCREEN_W 1024 // the desktop of the window nobody sees
#define NOBODY_SCREEN_H 768
#define VIEW_PATH       0x800 // bytes for the game's place in the settings file

// the host's window, NULL for the one nobody sees
static const OpenBGI_Window_t* Host(OpenBGI_t* o)
{
	return o->host.window;
}

// ---- the frames a second ----------------------------------------------------------------------

int MainWin_FrameLimits(OpenBGI_t* o)
{
	const OpenBGI_Window_t* t = Host(o);
	return t && t->setFrameLimit;
}

void MainWin_SetFrameLimit(OpenBGI_t* o, int on)
{
	if(MainWin_FrameLimits(o))
		Host(o)->setFrameLimit(o->host.user, on != 0);
}

// ---- the view ---------------------------------------------------------------------------------

int MainWin_ViewOn(OpenBGI_t* o)
{
	const OpenBGI_Window_t* t = Host(o);
	return t && t->workArea;
}

// the screen's scale as the host (or BGI_DPI) says it now
static void ViewReadDpi(OpenBGI_t* o)
{
	const OpenBGI_Window_t* t = Host(o);
	const char* e = getenv("BGI_DPI");
	int dpi = t && t->dpi ? t->dpi(o->host.user) : 96;
	if(e && atoi(e) > 0)
		dpi = atoi(e);
	o->window.viewDpi = dpi < 48 ? 48 : dpi > 480 ? 480
												  : dpi;
}

// the scale the scripts' desktop is smaller by: 1 without the view
static double ViewScale(OpenBGI_t* o)
{
	if(!MainWin_ViewOn(o))
		return 1.0;
	if(o->window.viewDpi <= 0)
		ViewReadDpi(o);
	return o->window.viewDpi / 96.0;
}

// the host's frame as it is on the screen: the width and height it adds, the caption and top border
static void FrameReal(OpenBGI_t* o, int* fw, int* fh, int* top)
{
	const OpenBGI_Window_t* t = Host(o);
	*fw = *fh = *top = 0;
	if(t && t->frame)
		t->frame(o->host.user, fw, fh, top);
}

static int Round(double v)
{
	return (int)(v < 0 ? v - 0.5 : v + 0.5);
}

// BGI_VIEW_TRACE: what the scripts asked for and what the host is given, with the scale and the frame, on stderr
static void ViewTrace(OpenBGI_t* o, const char* what, int x, int y, int w, int h, int rx, int ry, int rw, int rh)
{
	int fw, fh, top;
	if(!MainWin_ViewOn(o) || !getenv("BGI_VIEW_TRACE"))
		return;
	FrameReal(o, &fw, &fh, &top);
	fprintf(stderr, "openbgi: view: %s %d,%d %dx%d -> %d,%d %dx%d (%d dpi, frame %dx%d)\n", what, x, y, w, h, rx, ry, rw, rh,
		o->window.viewDpi, fw, fh);
}

/* The client area the window gets for the w x h the scripts ask for: the
 * player's own while the scripts ask for what they had then; else times
 * the scale (the screen's, or the settings'), smaller in proportion when
 * the window with its frame would not fit the work area, and a whole
 * number of times their size where the settings ask for whole steps. */
static void ViewClient(OpenBGI_t* o, int w, int h, int* rw, int* rh)
{
	const OpenBGI_Window_t* t = Host(o);
	int ax = 0, ay = 0, aw = 0, ah = 0, fw, fh, top;
	double s = ViewScale(o), roomW, roomH, cw, ch;
	*rw = w;
	*rh = h;
	if(!MainWin_ViewOn(o) || w <= 0 || h <= 0)
		return;
	if(o->window.userW > 0 && o->window.userVW == w && o->window.userVH == h)
	{
		*rw = o->window.userW;
		*rh = o->window.userH;
		return;
	}
	FrameReal(o, &fw, &fh, &top);
	t->workArea(o->host.user, &ax, &ay, &aw, &ah);
	roomW = aw - fw;
	roomH = ah - fh;
	switch(o->window.setSize)
	{
		case VIEW_SIZE_100: s = 1.0; break;
		case VIEW_SIZE_150: s = 1.5; break;
		case VIEW_SIZE_200: s = 2.0; break;
		case VIEW_SIZE_FIT:
			if(roomW >= 160 && roomH >= 120)
				s = roomW / w < roomH / h ? roomW / w : roomH / h;
			break;
		default: break;
	}
	cw = w * s;
	ch = h * s;
	if(roomW >= 160 && roomH >= 120 && (cw > roomW || ch > roomH))
	{
		double fit = roomW / cw < roomH / ch ? roomW / cw : roomH / ch;
		cw *= fit;
		ch *= fit;
	}
	if(o->window.setWhole && cw >= w)
	{ // by whole numbers: the largest that still fits
		int k = (int)(cw / w + 0.001);
		cw = (double)w * k;
		ch = (double)h * k;
	}
	*rw = Round(cw) > 0 ? Round(cw) : 1;
	*rh = Round(ch) > 0 ? Round(ch) : 1;
}

/* Where the window goes for the outer origin (x, y) the scripts ask for,
 * with the client area w x h they asked for and rw x rh it really has:
 * its centre where theirs is, times the scale, and inside the work area
 * when the view made it another size than they asked for.  A window of
 * the very size they asked for, on a screen without a scale, goes where
 * they say. */
static void ViewPlace(OpenBGI_t* o, int x, int y, int w, int h, int rw, int rh, int* rx, int* ry)
{
	const OpenBGI_Window_t* t = Host(o);
	int ax = 0, ay = 0, aw = 0, ah = 0, fw, fh, top;
	double s = ViewScale(o), px, py;
	*rx = x;
	*ry = y;
	if(!MainWin_ViewOn(o) || (s == 1.0 && rw == w && rh == h))
		return;
	FrameReal(o, &fw, &fh, &top);
	px = (x + (w + fw / s) / 2) * s - (rw + fw) / 2.0;
	py = (y + (h + fh / s) / 2) * s - (rh + fh) / 2.0;
	t->workArea(o->host.user, &ax, &ay, &aw, &ah);
	if(aw > 0 && ah > 0)
	{
		if(px + rw + fw > ax + aw)
			px = ax + aw - (rw + fw);
		if(px < ax)
			px = ax;
		if(py + rh + fh > ay + ah)
			py = ay + ah - (rh + fh);
		if(py < ay)
			py = ay;
	}
	*rx = Round(px);
	*ry = Round(py);
}

// the proportions the host keeps while the player sizes the window: the scripts' client area's, or any when the game sizes it itself
static void ViewAspect(OpenBGI_t* o)
{
	const OpenBGI_Window_t* t = Host(o);
	int w = o->ops.resizableWindow ? 0 : o->window.w, h = o->ops.resizableWindow ? 0 : o->window.h;
	if(!MainWin_ViewOn(o) || !t->setAspect || (w == o->window.aspectW && h == o->window.aspectH))
		return;
	o->window.aspectW = w;
	o->window.aspectH = h;
	t->setAspect(o->host.user, w, h);
}

/* Whether the settings' filter and steps hold: the host shows the
 * picture at another size than its own - a window fitted to its screen,
 * or a surface the picture is laid on (OpenBGI_SetSurface: a phone). */
int MainWin_ViewScales(OpenBGI_t* o)
{
	const OpenBGI_Window_t* t = Host(o);
	return MainWin_ViewOn(o) || (t && t->setScaling && o->overlay.surface.width > 0);
}

const char* MainWin_ViewFilterName(OpenBGI_t* o, int n)
{
	const OpenBGI_Window_t* t = Host(o);
	return MainWin_ViewScales(o) && t->filterName && n >= 0 ? t->filterName(o->host.user, n) : NULL;
}

// two names without regard to the case of their letters
static int SameName(const char* a, const char* b)
{
	for(; *a && *b; a++, b++)
	{
		int x = *a >= 'A' && *a <= 'Z' ? *a - 'A' + 'a' : *a, y = *b >= 'A' && *b <= 'Z' ? *b - 'A' + 'a' : *b;
		if(x != y)
			return 0;
	}
	return !*a && !*b;
}

/* The settings' filter by its name, when that is one of the host's own
 * and the host has it now (it tells its filters only once there is a
 * window, and may have none in a run without a graphics processor). */
static void ViewFilterFind(OpenBGI_t* o)
{
	const char* name;
	int n;
	if(!o->window.setFilterName[0] || o->window.setFilter >= VIEW_FILTER_OWN)
		return;
	for(n = 0; (name = MainWin_ViewFilterName(o, n)) != NULL; n++)
		if(SameName(name, o->window.setFilterName))
		{
			o->window.setFilter = VIEW_FILTER_OWN + n;
			return;
		}
}

// the rule the host scales by: the settings' filter, or the game's own; with the steps
static void ViewScaling(OpenBGI_t* o)
{
	const OpenBGI_Window_t* t = Host(o);
	int rule = o->window.gameRule;
	if(!t || !t->setScaling)
		return;
	if(MainWin_ViewScales(o))
	{
		ViewFilterFind(o);
		switch(o->window.setFilter)
		{
			case VIEW_FILTER_GAME: break;
			case VIEW_FILTER_BICUBIC: rule = OPENBGI_SCALE_BICUBIC | OPENBGI_SCALE_CHOSEN; break;
			case VIEW_FILTER_BILINEAR: rule = OPENBGI_SCALE_BILINEAR | OPENBGI_SCALE_CHOSEN; break;
			case VIEW_FILTER_NEAREST: rule = OPENBGI_SCALE_NEAREST | OPENBGI_SCALE_CHOSEN; break;
			default: rule = (OPENBGI_SCALE_OWN + o->window.setFilter - VIEW_FILTER_OWN) | OPENBGI_SCALE_CHOSEN; break;
		}
		if(o->window.setHigh)
			rule |= OPENBGI_SCALE_HIGH;
		if(o->window.setWhole)
			rule |= OPENBGI_SCALE_WHOLE;
	}
	t->setScaling(o->host.user, rule);
}

/* the settings file's entry the filter and the steps are kept in: the
 * game's - as the launcher started it, else by the archive or the folder
 * it runs from - and the launcher's own ("") while that runs */
static void ViewCfgPath(OpenBGI_t* o, char* path, size_t n)
{
	path[0] = 0;
	if(o->launcher.running)
		return;
	if(o->launcher.gamePath[0])
		snprintf(path, n, "%s", o->launcher.gamePath);
	else if(Fs_Mounted(o))
		snprintf(path, n, "%s", Fs_MountedPath(o));
	else
		Host_GameDir(o, path, n);
}

// the standard filters' names in the settings file, by VIEW_FILTER_*
static const char* const kFilterNames[VIEW_FILTER_OWN] = {NULL, "bicubic", "bilinear", "nearest"};

/* the filter, its quality and the steps as the settings file has them for
 * the game; a filter of the host's own is kept by its name until the host
 * can be asked for it (ViewFilterFind) */
static void ViewLoad(OpenBGI_t* o)
{
	char value[32], path[VIEW_PATH];
	int i;
	ViewCfgPath(o, path, sizeof path);
	o->window.setFilter = VIEW_FILTER_GAME;
	o->window.setFilterName[0] = 0;
	o->window.setWhole = 0;
	o->window.setHigh = 0;
	if(Engine_CfgGet(o, path, "filter", 0, value, sizeof value) > 0)
	{
		for(i = 1; i < VIEW_FILTER_OWN && strcmp(value, kFilterNames[i]) != 0; i++)
		{
		}
		if(i < VIEW_FILTER_OWN)
			o->window.setFilter = i;
		else
			snprintf(o->window.setFilterName, sizeof o->window.setFilterName, "%s", value);
	}
	if(Engine_CfgGet(o, path, "filterQuality", 0, value, sizeof value) > 0)
		o->window.setHigh = strcmp(value, "high") == 0;
	if(Engine_CfgGet(o, path, "wholeSteps", 0, value, sizeof value) > 0)
		o->window.setWhole = strcmp(value, "true") == 0;
}

/* A window the game sizes itself ("81 66") that had to be made smaller
 * to fit: the game is told, as of a size the player gave it, so that it
 * lays its picture out for the room there is instead of having it
 * scaled down. */
static void ViewTellFitted(OpenBGI_t* o)
{
	double s = ViewScale(o);
	if(!MainWin_ViewOn(o) || !o->ops.resizableWindow || !o->window.alive || o->window.fullscreen || o->window.userW > 0)
		return;
	if(o->window.realW < Round(o->window.w * s) || o->window.realH < Round(o->window.h * s))
		OpenBGI_WindowSize(o, o->window.realW, o->window.realH);
}

// the window as a framed one for the scripts' (x, y, w, h), through the view
static void ViewWindowed(OpenBGI_t* o, int x, int y, int w, int h)
{
	const OpenBGI_Window_t* t = Host(o);
	int rx, ry, rw, rh;
	if(o->window.userW > 0 && (o->window.userVW != w || o->window.userVH != h))
		o->window.userW = o->window.userH = 0; // the game asks for another size: the player's own is over
	ViewClient(o, w, h, &rw, &rh);
	ViewPlace(o, x, y, w, h, rw, rh, &rx, &ry);
	o->window.realW = rw;
	o->window.realH = rh;
	ViewTrace(o, "windowed", x, y, w, h, rx, ry, rw, rh);
	if(t && t->setWindowed)
		t->setWindowed(o->host.user, rx, ry, rw, rh);
	ViewAspect(o);
	ViewTellFitted(o);
}

int MainWin_UserSized(OpenBGI_t* o, int* w, int* h)
{
	double s = ViewScale(o);
	if(!MainWin_ViewOn(o))
		return 1;
	if(o->window.fullscreen || !o->window.alive)
		return 0;
	o->window.realW = o->window.userW = *w;
	o->window.realH = o->window.userH = *h;
	ViewTrace(o, "sized by the player", o->window.x, o->window.y, o->window.w, o->window.h, 0, 0, *w, *h);
	if(!o->ops.resizableWindow)
	{ // the picture is shown at that size; the scripts go on with theirs
		o->window.userVW = o->window.w;
		o->window.userVH = o->window.h;
		return 0;
	}
	*w = Round(*w / s) > 0 ? Round(*w / s) : 1;
	*h = Round(*h / s) > 0 ? Round(*h / s) : 1;
	o->window.userVW = *w; // (when the scripts set this size in turn, the window stays as it is)
	o->window.userVH = *h;
	return 1;
}

/* The player's size on a screen whose scale became another: larger or
 * smaller with the scale, and no larger than the work area has room for.
 * A window the game sizes itself needs none of it: the scripts' size
 * times the new scale is that size. */
static void ViewUserRescale(OpenBGI_t* o, int wasDpi)
{
	const OpenBGI_Window_t* t = Host(o);
	int ax = 0, ay = 0, aw = 0, ah = 0, fw, fh, top;
	double k = (double)o->window.viewDpi / wasDpi, cw = o->window.userW * k, ch = o->window.userH * k, roomW, roomH;
	if(o->ops.resizableWindow)
	{
		o->window.userW = o->window.userH = 0;
		return;
	}
	FrameReal(o, &fw, &fh, &top);
	t->workArea(o->host.user, &ax, &ay, &aw, &ah);
	roomW = aw - fw;
	roomH = ah - fh;
	if(roomW >= 160 && roomH >= 120 && (cw > roomW || ch > roomH))
	{
		double fit = roomW / cw < roomH / ch ? roomW / cw : roomH / ch;
		cw *= fit;
		ch *= fit;
	}
	o->window.userW = Round(cw) > 0 ? Round(cw) : 1;
	o->window.userH = Round(ch) > 0 ? Round(ch) : 1;
}

void MainWin_ViewRefresh(OpenBGI_t* o)
{
	int x, y, w, h, was = o->window.viewDpi;
	if(!MainWin_ViewOn(o) || !o->window.alive || o->window.fullscreen)
	{
		o->window.viewDpi = 0; // (read again when it is next needed)
		return;
	}
	MainWin_GetRect(o, &x, &y, &w, &h); // where the scripts have it, by the scale it was placed with
	ViewReadDpi(o);
	if(o->window.userW > 0 && was > 0 && was != o->window.viewDpi)
		ViewUserRescale(o, was);
	ViewWindowed(o, x, y, o->window.w, o->window.h);
}

void MainWin_ViewSettings(OpenBGI_t* o, int* size, int* filter, int* whole)
{
	ViewFilterFind(o);
	*size = o->window.setSize;
	*filter = o->window.setFilter;
	*whole = o->window.setWhole;
}

int MainWin_ViewHigh(OpenBGI_t* o)
{
	return o->window.setHigh;
}

void MainWin_ViewSetHigh(OpenBGI_t* o, int high)
{
	char path[VIEW_PATH];
	if((high != 0) == o->window.setHigh)
		return;
	o->window.setHigh = high != 0;
	ViewCfgPath(o, path, sizeof path);
	Engine_CfgSet(o, path, "filterQuality", 0, o->window.setHigh ? "high" : NULL);
	Engine_SaveGames(o);
	ViewScaling(o);
}

void MainWin_ViewApply(OpenBGI_t* o, int size, int filter, int whole)
{
	int resize = size != o->window.setSize || ((whole != 0) != o->window.setWhole && o->window.userW <= 0), x, y, w, h;
	ViewFilterFind(o);
	if(filter != o->window.setFilter || (whole != 0) != o->window.setWhole)
	{
		char path[VIEW_PATH];
		ViewCfgPath(o, path, sizeof path);
		if(filter != o->window.setFilter)
		{ // the filter by its name: one of the four, or the host's own for one of its
			const char* own = filter >= VIEW_FILTER_OWN ? MainWin_ViewFilterName(o, filter - VIEW_FILTER_OWN) : NULL;
			o->window.setFilter = own || (filter >= VIEW_FILTER_GAME && filter < VIEW_FILTER_OWN) ? filter : VIEW_FILTER_GAME;
			snprintf(o->window.setFilterName, sizeof o->window.setFilterName, "%s", own ? own : "");
			Engine_CfgSet(o, path, "filter", 0, own ? own : kFilterNames[o->window.setFilter]);
		}
		if((whole != 0) != o->window.setWhole)
		{
			o->window.setWhole = whole != 0;
			Engine_CfgSet(o, path, "wholeSteps", 0, o->window.setWhole ? "true" : NULL);
		}
		Engine_SaveGames(o);
		ViewScaling(o);
	}
	if(!resize)
		return;
	if(size != o->window.setSize)
		o->window.userW = o->window.userH = 0; // a size of the settings is the one asked for
	o->window.setSize = size >= VIEW_SIZE_AUTO && size <= VIEW_SIZE_FIT ? size : VIEW_SIZE_AUTO;
	if(!MainWin_ViewOn(o) || !o->window.alive || o->window.fullscreen || o->ops.resizableWindow)
		return;
	MainWin_GetRect(o, &x, &y, &w, &h);
	ViewWindowed(o, x, y, o->window.w, o->window.h);
}

// the host's rule again, after what decides it changed elsewhere (a surface came or went)
void MainWin_ViewRescale(OpenBGI_t* o)
{
	if(o->window.alive)
		ViewScaling(o);
}

void MainWin_ViewReset(OpenBGI_t* o)
{
	int x, y, w, h;
	if(!MainWin_ViewOn(o) || !o->window.alive || o->window.fullscreen || o->window.userW <= 0 || o->ops.resizableWindow)
		return;
	MainWin_GetRect(o, &x, &y, &w, &h);
	o->window.userW = o->window.userH = 0;
	ViewWindowed(o, x, y, o->window.w, o->window.h);
}

// ---- the window -------------------------------------------------------------------------------

// a focus change of the window nobody sees, for the next pumps (at most eight wait)
static void NoteFocus(OpenBGI_t* o, int active)
{
	if(o->window.focusCount < (int)BGI_COUNTOF(o->window.focus))
		o->window.focus[o->window.focusCount++] = active;
}

/* The window's icon, where the host has a way to set one: the icon of
 * the game's own program (Launcher_RunIcon) at the sizes it has, else
 * the host's (OpenBGI_SetIcon), else OpenBGI's own. */
static void SetIcon(OpenBGI_t* o)
{
	const OpenBGI_Window_t* t = Host(o);
	IconImage_t images[12];
	OpenBGI_Icon_t icons[12];
	size_t size = 0;
	uint8_t* ico;
	int count = 0, i;
	if(!t || !t->setIcon)
		return;
	ico = Launcher_RunIcon(o, &size);
	if(ico)
		count = Icon_Decode(ico, size, images, (int)BGI_COUNTOF(images));
	BGI_Free(ico);
	if(!count)
	{ // the host's own (OpenBGI_SetIcon), and without one the icon the engine carries
		const uint8_t* own = Launcher_OwnIcon(o, &size);
		if(own)
			count = Icon_Decode(own, size, images, (int)BGI_COUNTOF(images));
	}
	if(!count)
		count = Icon_Own(images, (int)BGI_COUNTOF(images));
	for(i = 0; i < count; i++)
	{
		icons[i].width = images[i].w;
		icons[i].height = images[i].h;
		icons[i].pixels = images[i].bgra;
	}
	t->setIcon(o->host.user, icons, count);
	Icon_Free(images, count);
}

int MainWin_Create(OpenBGI_t* o, const char* title, int x, int y, int w, int h)
{
	const OpenBGI_Window_t* t = Host(o);
	int rx, ry, rw, rh, fw, fh, top, fw2, fh2;
	if(o->window.alive)
		return 0;
	o->window.fullscreen = 0;
	o->window.userW = o->window.userH = 0;
	o->window.aspectW = o->window.aspectH = -1; // (the host is told at the first chance)
	o->window.viewDpi = 0;
	ViewLoad(o);
	FrameReal(o, &fw, &fh, &top);
	ViewClient(o, w, h, &rw, &rh);
	ViewPlace(o, x, y, w, h, rw, rh, &rx, &ry);
	ViewTrace(o, "create", x, y, w, h, rx, ry, rw, rh);
	if(t && t->create && !t->create(o->host.user, title, rx, ry, rw, rh))
		return 0;
	SetIcon(o);
	MainWin_SetScaling(o, o->display.bicubic ? OPENBGI_SCALE_BICUBIC : OPENBGI_SCALE_BILINEAR); // (a host keeps the last game's)
	o->window.alive = 1;
	o->window.shown = 0;
	o->window.x = x;
	o->window.y = y;
	o->window.w = w;
	o->window.h = h;
	o->window.realW = rw;
	o->window.realH = rh;
	if(MainWin_ViewOn(o))
	{ // the player may size it; in proportion
		if(t->setResizable)
			t->setResizable(o->host.user, 1);
		ViewAspect(o);
		FrameReal(o, &fw2, &fh2, &top);
		if(fw2 != fw || fh2 != fh)
			ViewWindowed(o, x, y, w, h); // (a host that knows the frame only once there is a window: fitted with it now)
	}
	return 1;
}

void MainWin_Destroy(OpenBGI_t* o)
{
	const OpenBGI_Window_t* t = Host(o);
	if(!o->window.alive)
		return;
	o->window.alive = 0;
	o->window.shown = 0;
	if(t && t->destroy)
		t->destroy(o->host.user);
}

void MainWin_Show(OpenBGI_t* o, int show)
{
	const OpenBGI_Window_t* t = Host(o);
	show = show != 0;
	if(t)
	{
		o->window.shown = show;
		if(t->show)
			t->show(o->host.user, show);
		return;
	}
	if(!o->window.alive || o->window.shown == show)
		return;
	o->window.shown = show;
	NoteFocus(o, show);
}

void MainWin_SetTitle(OpenBGI_t* o, const char* title)
{
	const OpenBGI_Window_t* t = Host(o);
	if(t && t->setTitle)
		t->setTitle(o->host.user, title);
}

void MainWin_Move(OpenBGI_t* o, int x, int y)
{
	const OpenBGI_Window_t* t = Host(o);
	int rx = x, ry = y;
	o->window.x = x;
	o->window.y = y;
	if(!o->window.fullscreen)
		ViewPlace(o, x, y, o->window.w, o->window.h, o->window.realW, o->window.realH, &rx, &ry);
	ViewTrace(o, "move", x, y, o->window.w, o->window.h, rx, ry, o->window.realW, o->window.realH);
	if(t && t->move)
		t->move(o->host.user, rx, ry);
}

void MainWin_GetRect(OpenBGI_t* o, int* x, int* y, int* w, int* h)
{
	const OpenBGI_Window_t* t = Host(o);
	double s = ViewScale(o);
	int fw, fh, top;
	*x = o->window.x;
	*y = o->window.y;
	*w = o->window.w;
	*h = o->window.h;
	if(t && t->getRect)
		t->getRect(o->host.user, x, y, w, h);
	if(!MainWin_ViewOn(o) || o->window.fullscreen || !t->getRect || !o->window.alive)
		return;
	// as the scripts count: their client area with a frame smaller by the scale, its centre where the window's is
	FrameReal(o, &fw, &fh, &top);
	if(s != 1.0 || *w != o->window.w + fw || *h != o->window.h + fh)
	{
		*x = Round((*x + *w / 2.0) / s - (o->window.w + fw / s) / 2);
		*y = Round((*y + *h / 2.0) / s - (o->window.h + fh / s) / 2);
	}
	*w = o->window.w + Round(fw / s);
	*h = o->window.h + Round(fh / s);
}

int MainWin_Minimized(OpenBGI_t* o)
{
	const OpenBGI_Window_t* t = Host(o);
	return t && t->minimized ? t->minimized(o->host.user) != 0 : 0;
}

void MainWin_Minimize(OpenBGI_t* o)
{
	const OpenBGI_Window_t* t = Host(o);
	if(t && t->minimize)
		t->minimize(o->host.user);
}

void MainWin_ScreenSize(OpenBGI_t* o, int* w, int* h)
{
	const OpenBGI_Window_t* t = Host(o);
	double s = ViewScale(o);
	*w = NOBODY_SCREEN_W;
	*h = NOBODY_SCREEN_H;
	if(t && t->screenSize)
		t->screenSize(o->host.user, w, h);
	if(s != 1.0)
	{ // the desktop as the scripts count
		*w = Round(*w / s);
		*h = Round(*h / s);
	}
}

// the frame's three measures as the scripts count them
static void Frame(OpenBGI_t* o, int* fw, int* fh, int* top)
{
	double s = ViewScale(o);
	FrameReal(o, fw, fh, top);
	if(s != 1.0)
	{
		*fw = Round(*fw / s);
		*fh = Round(*fh / s);
		*top = Round(*top / s);
	}
}

void MainWin_FrameMetrics(OpenBGI_t* o, int* fw, int* fh)
{
	int top;
	Frame(o, fw, fh, &top);
}

int MainWin_FrameTop(OpenBGI_t* o)
{
	int fw, fh, top;
	Frame(o, &fw, &fh, &top);
	return top;
}

int MainWin_SetMode(OpenBGI_t* o, int w, int h, int bpp)
{
	const OpenBGI_Window_t* t = Host(o);
	if(t && t->setMode)
		return t->setMode(o->host.user, w, h, bpp) != 0;
	return w > 0 && h > 0;
}

void MainWin_RestoreMode(OpenBGI_t* o)
{
	const OpenBGI_Window_t* t = Host(o);
	if(t && t->restoreMode)
		t->restoreMode(o->host.user);
}

void MainWin_SetFullscreen(OpenBGI_t* o, int w, int h)
{
	const OpenBGI_Window_t* t = Host(o);
	o->window.x = o->window.y = 0;
	o->window.w = w;
	o->window.h = h;
	o->window.fullscreen = 1;
	if(t && t->setFullscreen)
		t->setFullscreen(o->host.user, w, h);
}

void MainWin_SetWindowed(OpenBGI_t* o, int x, int y, int w, int h)
{
	o->window.x = x;
	o->window.y = y;
	o->window.w = w;
	o->window.h = h;
	o->window.fullscreen = 0;
	ViewWindowed(o, x, y, w, h);
}

void MainWin_SetPicture(OpenBGI_t* o, int w, int h)
{
	const OpenBGI_Window_t* t = Host(o);
	if(t && t->setPicture)
		t->setPicture(o->host.user, w, h);
}

void MainWin_CursorShape(OpenBGI_t* o, int shape)
{
	const OpenBGI_Window_t* t = Host(o);
	if(t && t->cursorShape)
		t->cursorShape(o->host.user, shape);
}

void MainWin_CursorShow(OpenBGI_t* o, int show)
{
	const OpenBGI_Window_t* t = Host(o);
	if(t && t->cursorShow)
		t->cursorShow(o->host.user, show);
}

void MainWin_ImeStatus(OpenBGI_t* o, int open)
{
	const OpenBGI_Window_t* t = Host(o);
	if(t && t->imeStatus)
		t->imeStatus(o->host.user, open);
}

void MainWin_DragAccept(OpenBGI_t* o, int accept)
{
	const OpenBGI_Window_t* t = Host(o);
	if(t && t->dragAccept)
		t->dragAccept(o->host.user, accept);
}

void MainWin_SetResizable(OpenBGI_t* o, int on)
{
	const OpenBGI_Window_t* t = Host(o);
	if(MainWin_ViewOn(o))
	{ // (the border is always there: what changes is who hears of the sizes, and the proportions)
		if(!on)
			o->window.userW = o->window.userH = 0;
		ViewAspect(o);
		ViewTellFitted(o);
		return;
	}
	if(t && t->setResizable)
		t->setResizable(o->host.user, on);
}

int MainWin_CanScale(OpenBGI_t* o)
{
	const OpenBGI_Window_t* t = Host(o);
	return t && t->setScaling;
}

void MainWin_SetScaling(OpenBGI_t* o, int rule)
{
	o->window.gameRule = rule;
	ViewScaling(o);
}

int MainWin_TakeFocus(OpenBGI_t* o, int* active)
{
	if(!o->window.focusCount || !o->window.alive)
		return 0;
	*active = o->window.focus[0];
	memmove(o->window.focus, o->window.focus + 1, (size_t)--o->window.focusCount * sizeof o->window.focus[0]);
	return 1;
}

// ---- the debugger's window ------------------------------------------------------------------

int MainWin_DebugOpen(OpenBGI_t* o, int w, int h, const char* title)
{
	const OpenBGI_DebugWindow_t* d = o->host.debugWindow;
	return d && d->open && d->open(o->host.user, w, h, title);
}

void MainWin_DebugClose(OpenBGI_t* o)
{
	const OpenBGI_DebugWindow_t* d = o->host.debugWindow;
	if(d && d->close)
		d->close(o->host.user);
}

void MainWin_DebugPresent(OpenBGI_t* o, const uint8_t* pixels, int pitch, int w, int h)
{
	const OpenBGI_DebugWindow_t* d = o->host.debugWindow;
	if(d && d->present)
		d->present(o->host.user, pixels, pitch, w, h);
}
