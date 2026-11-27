/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * openbgi.c - making and freeing an engine (include/openbgi.h, the struct in
 *             src/include/bgi/core.h)
 *
 * A new engine is all zero but for the host's services and the fields
 * below, which start with the values the original's globals are
 * initialised with.
 */
#include "bgi/request.h"
#include "bgi/inframe.h"
#include "bgi/display.h"
#include "bgi/gfx/typeface.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/input.h"
#include "bgi/fs.h"
#include "bgi/host.h"
#include "bgi/version.h"
#include "bgi/async.h"
#include "bgi/core.h"
#include "bgi/engine.h" // PRODUCT_NAME_DEFAULT

// the key lists of the standard input groups, by group (src/include/bgi/input.h); 0-terminated
static const uint32_t kStdKeys[STDKEY_COUNT][16] = {
	{0x01},       // bit 0        left button
	{0x02},       // bit 1        right button
	{0x04},       // bit 2        middle button
	{0x05},       // bit 4        X button 1
	{0x06},       // bit 5        X button 2
	{0x0e},       // bit 6  0x40  wheel up    ("80 1B")
	{0x0f},       // bit 7  0x80  wheel down
	{0x0d},       // bit 8  0x100 decide: Enter
	{0x20},       // bit 9  0x200 cancel: Space
	{0x26},       // bit 12 0x1000 up
	{0x28},       // bit 13 0x2000 down
	{0x25},       // bit 14 0x4000 left
	{0x27},       // bit 15 0x8000 right
	{0x31, 0x61}, // bit 16 '1' / numpad 1
	{0x32, 0x62}, // bit 17 '2'
	{0x33, 0x63}, // bit 18 '3'
	{0x34, 0x64}, // bit 19 '4'
	{0x35, 0x65}, // bit 20 '5'
	{0x36, 0x66}, // bit 21 '6'
	{0x37, 0x67}, // bit 22 '7'
	{0x38, 0x68}, // bit 23 '8'
	{0x39, 0x69}, // bit 24 '9'
	{0x30, 0x60}, // bit 25 '0'
	{0x09},       // bit 30 Tab
};

// the dynamic memory's pools (1.535 on): slots, offset bits per slot, largest block, first tag
static const DynPool_t kDynPools[5] = {
	{0x10000, 12, 0x1000, 0x10, NULL, NULL},
	{0x1000, 16, 0x10000, 0x14, NULL, NULL},
	{0x100, 20, 0x100000, 0x18, NULL, NULL},
	{0x10, 24, 0x1000000, 0x1c, NULL, NULL},
	{0x20, 26, 0x4000000, 0x20, NULL, NULL},
};

// the picture sizes of the size index ("80 61"), the last entry unused
static const int kScreenWidths[8] = {320, 640, 800, 1024, 1152, 1280, 1600, 0};
static const int kScreenHeights[8] = {240, 480, 600, 768, 864, 960, 1200, 0};

// the parameters of the text functions ("91 9A"): function 0 on, 0x80000001 0x10000, 0x80000007 on
static const int32_t kTextFunc[TEXT_FUNC_MAX] = {1, 0, 0x10000, 0, 0, 0, 0, 0, 1, 0, 0, 0};

static void StartingValues(OpenBGI_t* o)
{
	int i;

	o->util.clockMaxStep = 500;
	o->vm.dbgInsnLimit = 0x100000;
	o->util.randSeed = 1;
	o->util.hashKey = 0x3039;
	o->enc.kinsokuMode = -1; // not built yet
	memcpy(o->mem.dynPools, kDynPools, sizeof kDynPools);

	strcpy(o->files.bootArc, "system.arc");
	strcpy(o->files.bootFile, "ipl._bp");
	o->files.saveEncode = 1;
	o->engine.profile = Engine_ProfileOfGen(GEN_1_69_444);
	strcpy(o->engine.product, PRODUCT_NAME_DEFAULT);

	o->wait.waitBlocking = 1;
	o->wait.presentOnWait = 1;
	o->wait.menuBlinkMs = 600;
	o->wait.menuCursorColour[0] = 0x00ff0000; // "90 A4": the cursor's text colour in each blink phase
	o->wait.menuCursorColour[1] = 0x00ffff00;

	o->display.displayEnabled = 1;
	memcpy(o->display.screenWidths, kScreenWidths, sizeof kScreenWidths);
	memcpy(o->display.screenHeights, kScreenHeights, sizeof kScreenHeights);
	o->display.vsyncWait = 1;
	o->display.fullscreenFit = 1;
	o->display.windowRestored = 1;
	o->display.closeMode = 1;
	o->present.presentAllowed = 1;
	o->present.frameInterval = 4;

	o->input.inputEnable = 1;
	memcpy(o->input.stdKeys, kStdKeys, sizeof kStdKeys);
	o->input.skipKeys[0] = 0x11; // Ctrl
	o->input.wheelUpKeys[0] = 0x26;
	o->input.wheelUpKeys[1] = 0x0e;
	o->input.wheelDownKeys[0] = 0x28;
	o->input.wheelDownKeys[1] = 0x0f;
	o->input.skipKeyEnable = 1;
	o->input.tweenKeyMask = 0x2000;
	o->cursor.cursorShown = 1;
	Pad_Init(o);
	o->panel.procPresent = 1;
	o->panel.panelKeyMapsStale = 1;
	o->edit.editWidthPct = 100;

	o->gfx.windowsShown = 1;
	o->font.aaScale = 1;
	o->text.rubyCharDelay = 25;
	o->text.rubyFadeSteps = 30;
	o->text.rubySizePct = 40;
	memcpy(o->text.textFunc, kTextFunc, sizeof kTextFunc);
	o->text.rubyBold = -1;
	o->text.rubyShadow = -1;
	o->text.linkColour = -1;
	o->text.textScrollMs = 120;
	for(i = 0; i < 2; i++)
	{
		o->text.textArrows[i].x = -1; // at the end of the first / last text line
		o->text.textArrows[i].y = -1;
	}
	o->rain.effectFrameMs = 50;
	o->rain.effectsEnabled = 1;

#if !defined(BGI_NO_DEBUGGER)
	o->dbg.dbgThreadId = 1;
	o->dbg.mouseX = -1;
	o->dbg.mouseY = -1;
	o->dbg.skipThread = ~0u;
	o->dbg.dbgCellW = 8;
	o->dbg.programsFocus = 1;
	o->dbg.logFollow = 1;
#endif
}

uint32_t OpenBGI_ApiVersion(void)
{
	return OPENBGI_API_VERSION;
}

// whether a host built with version `api` of openbgi.h works with this engine
static int ApiFits(uint32_t api)
{
	uint32_t major = api >> 16, minor = api & 0xffff;
	if(major != OPENBGI_API_MAJOR)
		return 0;
	return minor <= OPENBGI_API_MINOR;
}

// one of the host's tables into the engine's copy of it; 0 when the host left its size out
#define TAKE_TABLE(name)                                                                 \
	do                                                                                   \
	{                                                                                    \
		if(o->host.name)                                                                 \
		{                                                                                \
			if(!Host_Take(&o->hostTables.name, sizeof o->hostTables.name, o->host.name)) \
				ok = 0;                                                                  \
			o->host.name = &o->hostTables.name;                                          \
		}                                                                                \
	} while(0)

// the host's services into the engine's copies; 0 when a size is missing
static int TakeHost(OpenBGI_t* o, const OpenBGI_Host_t* host)
{
	int ok = Host_Take(&o->host, sizeof o->host, host);
	TAKE_TABLE(fontFiles);
	TAKE_TABLE(fontRenderer);
	TAKE_TABLE(textEntry);
	TAKE_TABLE(childWindows);
	TAKE_TABLE(files);
	TAKE_TABLE(registry);
	TAKE_TABLE(window);
	TAKE_TABLE(debugWindow);
	TAKE_TABLE(machine);
	TAKE_TABLE(shell);
	TAKE_TABLE(cdAudio);
	TAKE_TABLE(moviePlayer);
	TAKE_TABLE(threads);
	TAKE_TABLE(movieDecoder);
	return ok;
}

OpenBGI_t* OpenBGI_New(const OpenBGI_Host_t* host)
{
	OpenBGI_t* o;
	if(!host || host->size < offsetof(OpenBGI_Host_t, api) + sizeof host->api || !ApiFits(host->api))
		return NULL;
	o = (OpenBGI_t*)calloc(1, sizeof *o);
	if(!o)
		return NULL;
	if(!TakeHost(o, host) || !o->host.ticksMs || !o->host.perfNs || !o->host.localTime)
	{
		free(o);
		return NULL;
	}
	StartingValues(o);
	return o;
}

/* What a stopped engine still holds goes - all that is left once
 * OpenBGI_Stop has shut the run down, and what there is of an engine that
 * was never stopped; the requests only with `requests`. */
static void FreeRest(OpenBGI_t* o, int requests)
{
	Settings_Close(o);   // the settings over an engine that was not stopped
	LoadFile_FreeCache(o); // the unpacked files kept for the run
	Inframe_Free(o);     // a dialog in the frame when the engine was not stopped
	InputMap_Stop(o);    // the on-screen keyboard and the drawn pointer then
	InputMap_Free(o);    // the host's input settings
	Child_DestroyAll(o); // the child windows left then (before their captions' font goes)
	if(requests)
		Request_Clear(o);         // a request left after OpenBGI_Stop, answered or not
	Screen_Free(o);               // a picture left when the engine never got to its end
	Screen_NextFrame(o);          // .. and the pixels of it a frame had handed over
	Overlay_Free(o);              // the layer over it
	Perf_Stop(o);                 // the performance display's characters and picture
	Typefaces_Free(o);            // the fonts still open, and the engine's font library
	FontNames_Free(o);            // the faces, the font files and the fallbacks the scripts registered
	Input_FreeLayers(o);          // the input layers (the base ones the shutdown leaves)
	Engine_FreeGames(o);          // the games table and the input settings of openbgi.json
	Fs_Unmount(o);                // a game's zip
	BGI_Free(o->engine.startCmd); // the command line of the start
	o->engine.startCmd = NULL;
}

void Engine_FreeRest(OpenBGI_t* o)
{
	FreeRest(o, 0);
}

void Engine_Renew(OpenBGI_t* o)
{
	OpenBGI_Host_t host = o->host; // (it points at the tables, which stay where they are)
	struct HostTables tables = o->hostTables;
	uint32_t pads = o->input.pad.attached;
	int ppi = o->input.touch.ppi;
	struct InputSettings *input = o->input.cfgHost, *inputBase = o->input.cfgHostBase;
	uint32_t lead = o->snd.out.lead;
	void* soundLock;
	struct Async* async;
	uint32_t mhz = o->util.sysInfo.mhz ? o->util.sysInfo.mhz : o->util.cpuKept;
	LauncherState_t launcher = o->launcher; // (the stop closed its listings)
	OpenBGI_Surface_t surface = o->overlay.surface;
	o->input.cfgHost = o->input.cfgHostBase = NULL; // (not FreeRest's this time)
	FreeRest(o, 1);
	soundLock = SndOut_Renew(o); // (the reader goes on reading: it stays out while the state is cleared)
	async = o->async;
	memset(o, 0, sizeof *o);
	o->async = async; // (the helpers sleep: every job of the run was finished when its wait went)
	o->host = host;
	o->hostTables = tables;
	SndOut_Renewed(o, soundLock);
	o->launcher = launcher; // the host's launcher, and the game it started
	o->launcher.running = 0;
	StartingValues(o);
	o->input.pad.attached = pads; // what the host said of its devices still holds,
	o->input.touch.ppi = ppi;
	o->input.cfgHost = input; // .. and its input settings (OpenBGI_InputSettings)
	o->input.cfgHostBase = inputBase;
	o->snd.out.lead = lead;       // .. and of its sound output, which goes on reading (OpenBGI_SetAudioLead)
	o->util.cpuKept = mhz;        // .. and the processor's clock, measured at the first start (Cpu_Detect)
	o->overlay.surface = surface; // .. and what it draws on (OpenBGI_SetSurface); the layer is made anew
	o->overlay.ppiSet = -1;
	Host_TextEncoding(o, 0); // a run begins in Shift-JIS, whatever the one before it left the host with (the launcher's is UTF-8)
}

void OpenBGI_Delete(OpenBGI_t* o)
{
	if(!o)
		return;
	if(!Engine_Idle(o))
		OpenBGI_Stop(o); // (a host that did not stop the run: nothing of it is left behind either)
	FreeRest(o, 1);
	SndOut_Free(o);
	Async_Free(o);
	Launcher_Free(o);
	free(o);
}
