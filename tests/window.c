/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/window.c - unit test of the main window as the engine asks for it
 *                  (src/sys/mainwin.c, the window nobody sees and the
 *                  host's OpenBGI_Window_t), the window's events the host
 *                  hands over (OpenBGI_Close and its kin, src/sys/window.c),
 *                  the messages the engine posts itself and the
 *                  debugger's window; built as test_window by `make test`
 *
 * The groups, in the order they run:
 *
 *   nobody   - without a window table: the window is made where the
 *              engine put it, on a 1024 x 768 desktop without a frame,
 *              every mode taken, the full screen at (0, 0); showing it
 *              gives it the focus and hiding it takes it away at the next
 *              OpenBGI_PumpEvents, a destroyed one has none to give
 *   host     - with a table: every call reaches its member with the
 *              host's user and the engine's arguments, the frame's three
 *              measures come from one call, a failed creation fails, a
 *              table without queries answers as the window nobody sees
 *   icon     - the window's icon without a game's own: the host's
 *              (OpenBGI_SetIcon), and OpenBGI's own for bytes that
 *              are no icon
 *   events   - the close box asks the script (close mode 0) at once, or
 *              destroys the window and ends the run at the next pump
 *              (close mode 1); "80 69" and Alt+F4 post the close for the
 *              next pump; a minimise and a restore, a dropped file (with
 *              drops on and off), a message for the message waits, a
 *              display change re-centring the window, the host's quit
 *   view     - a host that fits the window to its screen (dpi, workArea,
 *              setAspect): a window that fits goes where and as the
 *              scripts say; one too large is made smaller in proportion
 *              and kept inside the work area; the player's size stays
 *              while the scripts ask for what they had, and ends with
 *              another size of theirs or a reset; the settings' sizes,
 *              whole steps and filter; a window the game sizes itself;
 *              a screen at one and a half, and its scale changing
 *   debugger - the debugger's window table: open, present and close reach
 *              the host; without the table nothing opens
 */
#include "bgi/core.h"
#include "bgi/release.h"
#include "bgi/display.h"
#include "bgi/sysobj/events.h"
#include "host/os.h"
#include "host/os_common.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "iconexe.h"

// ---- the host's clock -------------------------------------------------------------------

static uint32_t HostTicksMs(void* user)
{
	BGI_UNUSED(user);
	return 0;
}

static int HostPerfNs(void* user, int64_t* out)
{
	BGI_UNUSED(user);
	*out = 0;
	return 1;
}

static void HostLocalTime(void* user, OpenBGI_DateTime_t* out)
{
	BGI_UNUSED(user);
	memset(out, 0, sizeof *out);
	out->year = 2026;
	out->month = 1;
	out->day = 1;
}

// ---- a host's window that notes every call -------------------------------------------------

static struct
{
	char log[0x800]; // the calls, "name(args);" each
	int badUser;     // calls with another user than the host's
	int createOk;    // what create answers
	int minimized;   // what minimized answers
	int screenW, screenH;
	int debugOpenOk; // what the debugger's open answers
} gWin;

static void Note(void* user, const char* fmt, ...)
{
	char buf[0x100];
	va_list ap;
	if(user != &gWin)
		gWin.badUser++;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	snprintf(gWin.log + strlen(gWin.log), sizeof gWin.log - strlen(gWin.log), "%s;", buf);
}

static int WCreate(void* user, const char* title, int x, int y, int w, int h)
{
	Note(user, "create(%s,%d,%d,%d,%d)", title, x, y, w, h);
	return gWin.createOk;
}

static void WDestroy(void* user)
{
	Note(user, "destroy");
}

static void WShow(void* user, int show)
{
	Note(user, "show(%d)", show);
}

static void WSetTitle(void* user, const char* title)
{
	Note(user, "title(%s)", title ? title : "NULL");
}

static void WMove(void* user, int x, int y)
{
	Note(user, "move(%d,%d)", x, y);
}

static void WGetRect(void* user, int* x, int* y, int* w, int* h)
{
	Note(user, "rect");
	*x = 7;
	*y = 8;
	*w = 816;
	*h = 638;
}

static int WMinimized(void* user)
{
	Note(user, "minimized");
	return gWin.minimized;
}

static void WMinimize(void* user)
{
	Note(user, "minimize");
}

static void WScreenSize(void* user, int* w, int* h)
{
	Note(user, "screen");
	*w = gWin.screenW;
	*h = gWin.screenH;
}

static void WFrame(void* user, int* fw, int* fh, int* top)
{
	Note(user, "frame");
	*fw = 6;
	*fh = 29;
	*top = 26;
}

static int WSetMode(void* user, int w, int h, int bpp)
{
	Note(user, "mode(%d,%d,%d)", w, h, bpp);
	return bpp == 16;
}

static void WRestoreMode(void* user)
{
	Note(user, "restore");
}

static void WSetFullscreen(void* user, int w, int h)
{
	Note(user, "fullscreen(%d,%d)", w, h);
}

static void WSetWindowed(void* user, int x, int y, int w, int h)
{
	Note(user, "windowed(%d,%d,%d,%d)", x, y, w, h);
}

static void WSetPicture(void* user, int w, int h)
{
	Note(user, "picture(%d,%d)", w, h);
}

static void WCursorShape(void* user, int shape)
{
	Note(user, "shape(%d)", shape);
}

static void WCursorShow(void* user, int show)
{
	Note(user, "cursor(%d)", show);
}

static void WImeStatus(void* user, int open)
{
	Note(user, "ime(%d)", open);
}

static void WDragAccept(void* user, int accept)
{
	Note(user, "drag(%d)", accept);
}

static void WSetResizable(void* user, int on)
{
	Note(user, "resizable(%d)", on);
}

static void WSetScaling(void* user, int rule)
{
	Note(user, "scaling(%d)", rule);
}

static void WSetIcon(void* user, const OpenBGI_Icon_t* icons, int count)
{
	Note(user, "icon(%d,%d,%d)", count, count ? icons[0].width : 0, count ? icons[count - 1].width : 0);
}

static const OpenBGI_Window_t kWindow = {sizeof(OpenBGI_Window_t), WCreate, WDestroy, WShow, WSetTitle, WMove, WGetRect, WMinimized, WMinimize, WScreenSize, WFrame,
	WSetMode, WRestoreMode, WSetFullscreen, WSetWindowed, WSetPicture, WCursorShape, WCursorShow, WImeStatus, WDragAccept, WSetResizable, WSetScaling, WSetIcon,
	NULL, NULL, NULL, NULL};

// ---- a host's window that is fitted to its screen ---------------------------------------------

static struct
{
	int x, y, w, h;     // the frame's corner and the client area, as the engine last set them
	int dpi;            // what dpi answers
	int ax, ay, aw, ah; // what workArea answers
	int aspectW, aspectH, rule, resizable;
} gFit;

static int FCreate(void* user, const char* title, int x, int y, int w, int h)
{
	BGI_UNUSED(user);
	BGI_UNUSED(title);
	gFit.x = x;
	gFit.y = y;
	gFit.w = w;
	gFit.h = h;
	return 1;
}

static void FMove(void* user, int x, int y)
{
	BGI_UNUSED(user);
	gFit.x = x;
	gFit.y = y;
}

static void FGetRect(void* user, int* x, int* y, int* w, int* h)
{
	BGI_UNUSED(user);
	*x = gFit.x;
	*y = gFit.y;
	*w = gFit.w + 6; // (WFrame's frame)
	*h = gFit.h + 29;
}

static void FSetWindowed(void* user, int x, int y, int w, int h)
{
	FCreate(user, NULL, x, y, w, h);
}

static void FSetResizable(void* user, int on)
{
	BGI_UNUSED(user);
	gFit.resizable = on;
}

static void FSetScaling(void* user, int rule)
{
	BGI_UNUSED(user);
	gFit.rule = rule;
}

static int FDpi(void* user)
{
	BGI_UNUSED(user);
	return gFit.dpi;
}

static void FWorkArea(void* user, int* x, int* y, int* w, int* h)
{
	BGI_UNUSED(user);
	*x = gFit.ax;
	*y = gFit.ay;
	*w = gFit.aw;
	*h = gFit.ah;
}

static void FSetAspect(void* user, int w, int h)
{
	BGI_UNUSED(user);
	gFit.aspectW = w;
	gFit.aspectH = h;
}

// two filters of the host's own, once it has a window (as a host that needs its graphics processor for them)
static const char* FFilterName(void* user, int n)
{
	static const char* const kOwn[2] = {"Sharp A", "Sharp B"};
	BGI_UNUSED(user);
	return gFit.w > 0 && n >= 0 && n < 2 ? kOwn[n] : NULL;
}

static const OpenBGI_Window_t kFit = {sizeof(OpenBGI_Window_t), FCreate, WDestroy, WShow, WSetTitle, FMove, FGetRect, WMinimized, WMinimize, WScreenSize, WFrame,
	WSetMode, WRestoreMode, WSetFullscreen, FSetWindowed, WSetPicture, WCursorShape, WCursorShow, WImeStatus, WDragAccept, FSetResizable, FSetScaling, WSetIcon,
	FDpi, FWorkArea, FSetAspect, FFilterName};

static int DOpen(void* user, int w, int h, const char* title)
{
	Note(user, "dopen(%d,%d,%s)", w, h, title);
	return gWin.debugOpenOk;
}

static void DClose(void* user)
{
	Note(user, "dclose");
}

static void DPresent(void* user, const uint8_t* pixels, int pitch, int w, int h)
{
	Note(user, "dpresent(%d,%d,%d,%d)", pixels ? pixels[0] : -1, pitch, w, h);
}

static const OpenBGI_DebugWindow_t kDebugWindow = {sizeof(OpenBGI_DebugWindow_t), DOpen, DClose, DPresent};

static OpenBGI_Host_t gBare = {.size = sizeof(OpenBGI_Host_t), .api = OPENBGI_API_VERSION, .ticksMs = HostTicksMs, .perfNs = HostPerfNs, .localTime = HostLocalTime, .caps = OPENBGI_CAP_DIALOGS};

static void Reset(void)
{
	memset(&gWin, 0, sizeof gWin);
	gWin.createOk = 1;
	gWin.screenW = 1920;
	gWin.screenH = 1080;
	gWin.debugOpenOk = 1;
}

// the scripts' next event (MsgQueue), its first value; -1 when there is none
static int NextEvent(OpenBGI_t* o)
{
	uint32_t v[3];
	return MsgQueue_Get(o, v) ? (int)v[0] : -1;
}

// ---- the tests ------------------------------------------------------------------------

static void TestNobody(void)
{
	OpenBGI_t* o = OpenBGI_New(&gBare);
	int x, y, w, h, fw, fh;
	printf("nobody\n");
	CHECK(o != NULL);
	if(!o)
		return;
	MainWin_ScreenSize(o, &w, &h);
	CHECK(w == 1024 && h == 768);
	MainWin_FrameMetrics(o, &fw, &fh);
	CHECK(fw == 0 && fh == 0 && MainWin_FrameTop(o) == 0);
	CHECK(MainWindow_Create(o));
	CHECK(o->display.windowAlive && o->display.windowExists);
	MainWin_GetRect(o, &x, &y, &w, &h);
	CHECK(x == (1024 - 800) / 2 && y == (768 - 600) / 2 && w == 800 && h == 600); // centred on the desktop
	CHECK(!MainWin_Create(o, "again", 0, 0, 10, 10));                             // there is one already
	MainWin_Move(o, 30, 40);
	MainWin_GetRect(o, &x, &y, &w, &h);
	CHECK(x == 30 && y == 40 && w == 800 && h == 600);
	CHECK(!MainWin_Minimized(o));
	CHECK(MainWin_SetMode(o, 640, 480, 16) && !MainWin_SetMode(o, 0, 480, 16));
	MainWin_SetFullscreen(o, 640, 480);
	MainWin_GetRect(o, &x, &y, &w, &h);
	CHECK(x == 0 && y == 0 && w == 640 && h == 480);
	MainWin_SetWindowed(o, 5, 6, 800, 600);
	MainWin_GetRect(o, &x, &y, &w, &h);
	CHECK(x == 5 && y == 6 && w == 800 && h == 600);
	// the focus comes with a show, at the next pump
	CHECK(!o->display.windowActive);
	ShowMainWindow(o, 1);
	CHECK(!o->display.windowActive);
	OpenBGI_PumpEvents(o);
	CHECK(o->display.windowActive);
	ShowMainWindow(o, 1); // shown already: nothing new
	CHECK(o->window.focusCount == 0);
	ShowMainWindow(o, 0);
	ShowMainWindow(o, 1);
	OpenBGI_PumpEvents(o); // two changes, delivered in turn
	CHECK(o->display.windowActive);
	ShowMainWindow(o, 0);
	OpenBGI_PumpEvents(o);
	CHECK(!o->display.windowActive);
	ShowMainWindow(o, 1);
	MainWindow_Destroy(o); // a destroyed window gives no focus
	CHECK(!o->display.windowAlive && !o->display.windowExists);
	o->vm.sched.running = 1;
	OpenBGI_PumpEvents(o);
	CHECK(!o->display.windowActive);
	CHECK(o->vm.sched.running == 0); // WM_DESTROY's quit stopped the machine
	OpenBGI_Delete(o);
}

static void TestHost(void)
{
	OpenBGI_Host_t host = gBare;
	OpenBGI_Window_t partial;
	OpenBGI_t* o;
	int x, y, w, h, fw, fh;
	printf("host\n");
	Reset();
	host.user = &gWin;
	host.window = &kWindow;
	o = OpenBGI_New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	CHECK(MainWindow_Create(o));
	CHECK(strncmp(gWin.log, "frame;screen;", 13) == 0);           // measured first
	CHECK(strstr(gWin.log, "create(" OPENBGI_TITLE ",") != NULL); // with the engine's name and version
	CHECK(strstr(gWin.log, ",557,214,800,600);") != NULL);        // centred on 1920 x 1080 with the host's frame
	CHECK(strstr(gWin.log, "scaling(0);") != NULL);               // a new window scales bilinearly
	CHECK(strstr(gWin.log, "icon(4,256,16);") != NULL);           // .. and has OpenBGI's own icon, at four sizes
	gWin.log[0] = 0;
	ShowMainWindow(o, 1);
	MainWin_SetTitle(o, "game");
	MainWin_SetTitle(o, NULL);
	MainWin_Move(o, 1, 2);
	MainWin_GetRect(o, &x, &y, &w, &h);
	CHECK(x == 7 && y == 8 && w == 816 && h == 638);
	gWin.minimized = 1;
	CHECK(MainWin_Minimized(o));
	MainWin_Minimize(o);
	MainWin_FrameMetrics(o, &fw, &fh);
	CHECK(fw == 6 && fh == 29 && MainWin_FrameTop(o) == 26);
	CHECK(!MainWin_SetMode(o, 640, 480, 32) && MainWin_SetMode(o, 640, 480, 16));
	MainWin_RestoreMode(o);
	MainWin_SetFullscreen(o, 640, 480);
	MainWin_SetWindowed(o, 3, 4, 800, 600);
	MainWin_SetPicture(o, 640, 480);
	MainWin_CursorShape(o, 1);
	MainWin_CursorShow(o, 0);
	MainWin_ImeStatus(o, 1);
	EnableDragDrop(o, 1);
	CHECK(strcmp(gWin.log, "show(1);title(game);title(NULL);move(1,2);rect;minimized;minimize;frame;frame;mode(640,480,32);mode(640,480,16);"
						   "restore;fullscreen(640,480);windowed(3,4,800,600);picture(640,480);shape(1);cursor(0);ime(1);drag(1);") == 0);
	o->vm.sched.running = 1;
	OpenBGI_PumpEvents(o); // a host's window reports its own focus: no show gives one
	CHECK(!o->display.windowActive);
	// a sizing border ("81 66"): the host is told, and the size the player gives the window is the window's size ("81 67")
	gWin.log[0] = 0;
	Display_GetWindowSize(o, &fw, &fh);
	OpenBGI_WindowSize(o, 1000, 700); // not while the border is the fixed one
	Display_GetWindowSize(o, &w, &h);
	CHECK(w == fw && h == fh && (w != 1000 || h != 700));
	Display_SetResizable(o, 1);
	OpenBGI_WindowSize(o, 1000, 700);
	Display_GetWindowSize(o, &w, &h);
	CHECK(w == 1000 && h == 700);
	OpenBGI_WindowSize(o, 0, 700); // no size
	Display_GetWindowSize(o, &w, &h);
	CHECK(w == 1000 && h == 700);
	Display_SetResizable(o, 0);
	CHECK(strcmp(gWin.log, "resizable(1);resizable(0);") == 0);
	// the bicubic scaler (the shader of "81 6D" .. "81 6F"): a host that scales by a rule has it, and is told
	gWin.log[0] = 0;
	CHECK(Display_ShaderSupport(o) && Display_SetShader(o, 1) && o->display.bicubic);
	CHECK(Display_SetShader(o, 0) && !o->display.bicubic);
	CHECK(strcmp(gWin.log, "scaling(1);scaling(0);") == 0);
	gWin.log[0] = 0;
	MainWindow_Destroy(o);
	CHECK(strcmp(gWin.log, "destroy;") == 0);
	CHECK(gWin.badUser == 0);
	OpenBGI_Delete(o);

	// a creation the host refuses fails
	Reset();
	gWin.createOk = 0;
	o = OpenBGI_New(&host);
	if(o)
	{
		CHECK(!MainWin_Create(o, "t", 0, 0, 800, 600));
		CHECK(!o->window.alive);
		OpenBGI_Delete(o);
	}

	// a table without the queries answers as the window nobody sees
	memset(&partial, 0, sizeof partial);
	partial.size = sizeof partial;
	partial.create = WCreate;
	host.window = &partial;
	Reset();
	o = OpenBGI_New(&host);
	if(o)
	{
		MainWin_ScreenSize(o, &w, &h);
		CHECK(w == 1024 && h == 768);
		CHECK(MainWin_Create(o, "t", 11, 12, 800, 600));
		MainWin_GetRect(o, &x, &y, &w, &h);
		CHECK(x == 11 && y == 12 && w == 800 && h == 600);
		CHECK(MainWin_SetMode(o, 640, 480, 16) && !MainWin_Minimized(o) && MainWin_FrameTop(o) == 0);
		MainWin_Show(o, 1); // nothing to call
		// no rule to tell it: no shader, and only its release succeeds
		CHECK(!Display_ShaderSupport(o) && !Display_SetShader(o, 1) && Display_SetShader(o, 0) && !o->display.bicubic);
		OpenBGI_Delete(o);
	}
}

/* The window's icon without a game's own: the host's (OpenBGI_SetIcon),
 * an .ico file - here one picture of 4 x 4 pixels - and a file that is
 * none leaves OpenBGI's own. */
static void TestIcon(void)
{
	static uint8_t ico[6 + 16 + 0x100];
	OpenBGI_Host_t host = gBare;
	OpenBGI_t* o;
	uint32_t n = IconExe_Picture(ico + 22, 4, 1, 2, 3);
	printf("icon\n");
	IconExe_Put16(ico + 2, 1);
	IconExe_Put16(ico + 4, 1);
	ico[6] = ico[7] = 4;
	IconExe_Put32(ico + 14, n);
	IconExe_Put32(ico + 18, 22);
	host.user = &gWin;
	host.window = &kWindow;
	Reset();
	o = OpenBGI_New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	OpenBGI_SetIcon(o, ico, 22 + n);
	CHECK(MainWin_Create(o, "t", 0, 0, 100, 100) && strstr(gWin.log, "icon(1,4,4);") != NULL);
	OpenBGI_Delete(o);
	Reset();
	o = OpenBGI_New(&host);
	if(!o)
		return;
	OpenBGI_SetIcon(o, "no icon at all", 14);
	CHECK(MainWin_Create(o, "t", 0, 0, 100, 100) && strstr(gWin.log, "icon(4,256,16);") != NULL);
	OpenBGI_SetIcon(o, NULL, 0);
	OpenBGI_Delete(o);
}

static void TestEvents(void)
{
	OpenBGI_Host_t host = gBare;
	OpenBGI_t* o;
	WinMsgRec_t rec;
	char path[0x104];
	printf("events\n");
	Reset();
	host.user = &gWin;
	host.window = &kWindow;
	o = OpenBGI_New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	Display_InitSizeTable(o);
	CHECK(MainWindow_Create(o));
	o->vm.sched.running = 1;
	// the close box in close mode 0: the script hears of it at once (event 2)
	Set_CloseMode(o, 0);
	OpenBGI_Close(o);
	CHECK(NextEvent(o) == 2 && NextEvent(o) == -1);
	// "80 69" and Alt+F4 post it: nothing until the next pump
	MainWindow_PostClose(o);
	CHECK(NextEvent(o) == -1);
	OpenBGI_PumpEvents(o);
	CHECK(NextEvent(o) == 2 && NextEvent(o) == -1);
	OpenBGI_Key(o, 0x73, 1, OPENBGI_KEY_SYSTEM); // Alt+F4
	CHECK(NextEvent(o) == -1);
	OpenBGI_Key(o, 0x73, 0, OPENBGI_KEY_SYSTEM);
	OpenBGI_PumpEvents(o);
	CHECK(NextEvent(o) == 2);
	// minimised and restored by the player
	OpenBGI_Minimized(o, 1);
	CHECK(!Window_IsRestored(o));
	OpenBGI_Minimized(o, 0);
	CHECK(Window_IsRestored(o));
	// a dropped file: event 0x10, the path for "80 6D" while drops are on
	gWin.log[0] = 0;
	EnableDragDrop(o, 1);
	CHECK(strcmp(gWin.log, "drag(1);") == 0);
	OpenBGI_DropFile(o, "C:\\game\\save.dat");
	CHECK(NextEvent(o) == 0x10);
	CHECK(GetDroppedFile(o, path) && strcmp(path, "C:\\game\\save.dat") == 0);
	EnableDragDrop(o, 0);
	CHECK(!GetDroppedFile(o, path));
	// a message for the scripts' message waits ("80 54")
	WinMsgWait_Register(o, NULL, 0x400);
	OpenBGI_WindowMessage(o, 0x400, 0x80000001u, 0x12345678u);
	CHECK(WinMsgWait_Poll(o, &rec, NULL, 0x400) && rec.arrived && rec.wParam == 0x80000001u && rec.lParam == 0x12345678u);
	OpenBGI_WindowMessage(o, 0x401, 1, 2);
	CHECK(WinMsgWait_Poll(o, &rec, NULL, 0x400) && !rec.arrived);
	WinMsgWait_Unregister(o, NULL, 0x400);
	// the desktop changed: the window is centred on the new one
	gWin.screenW = 1280;
	gWin.screenH = 1024;
	gWin.log[0] = 0;
	OpenBGI_DisplayChanged(o);
	{
		// the picture in the frame of 6 x 29, 26 above it, in the middle of 1280 x 1024
		char want[32];
		int pw, ph;
		Display_GetPictureSize(o, &pw, &ph);
		snprintf(want, sizeof want, "move(%d,%d);", (1280 - (pw + 6)) / 2, (1024 - ph) / 2 - 26);
		CHECK(pw > 0 && ph > 0 && strstr(gWin.log, "screen;") != NULL && strstr(gWin.log, want) != NULL);
	}
	// the host's quit: at the next pump
	OpenBGI_Quit(o);
	CHECK(o->vm.sched.running == 1);
	OpenBGI_PumpEvents(o);
	CHECK(o->vm.sched.running == 0);
	// the close box in close mode 1: the window goes, the run ends at the next pump
	o->vm.sched.running = 1;
	Set_CloseMode(o, 1);
	gWin.log[0] = 0;
	OpenBGI_Close(o);
	CHECK(strstr(gWin.log, "destroy;") != NULL && !o->display.windowExists);
	CHECK(o->vm.sched.running == 1);
	OpenBGI_PumpEvents(o);
	CHECK(o->vm.sched.running == 0);
	// without a window nothing is posted
	MainWindow_PostClose(o);
	CHECK(!o->window.closePosted);
	// the CD and the movie notifications without a CD or a movie do nothing
	o->vm.sched.running = 1;
	OpenBGI_CdNotify(o, 1);
	OpenBGI_MovieEvent(o, 1);
	CHECK(o->vm.sched.running == 1);
	CHECK(gWin.badUser == 0);
	OpenBGI_Delete(o);
}

// an engine on the host whose window is fitted to its screen, on a 1920 x 1080 screen with a task bar of 40 pixels
static OpenBGI_t* FitUp(int dpi)
{
	OpenBGI_Host_t host = gBare;
	Reset();
	memset(&gFit, 0, sizeof gFit);
	gFit.dpi = dpi;
	gFit.aw = 1920;
	gFit.ah = 1040;
	host.user = &gWin;
	host.window = &kFit;
	return OpenBGI_New(&host);
}

static void TestView(void)
{
	OpenBGI_t* o = FitUp(96);
	int x, y, w, h, fw, fh, size, filter, whole;
	printf("view\n");
	CHECK(o != NULL);
	if(!o)
		return;
	// a screen without a scale and a window that fits: where and as the scripts say; the player may size it, in proportion
	CHECK(MainWin_ViewOn(o));
	CHECK(MainWindow_Create(o));
	CHECK(gFit.x == 557 && gFit.y == 214 && gFit.w == 800 && gFit.h == 600);
	CHECK(gFit.resizable == 1 && gFit.aspectW == 800 && gFit.aspectH == 600);
	MainWin_GetRect(o, &x, &y, &w, &h);
	CHECK(x == 557 && y == 214 && w == 806 && h == 629);
	MainWin_Move(o, 30, 40);
	CHECK(gFit.x == 30 && gFit.y == 40);
	// one that has no room beside the task bar: smaller in proportion and inside the work area; the scripts count theirs
	MainWin_SetWindowed(o, 0, 0, 1920, 1080);
	CHECK(gFit.w == 1797 && gFit.h == 1011 && gFit.y == 0 && gFit.x >= 0 && gFit.x + gFit.w + 6 <= 1920);
	CHECK(gFit.aspectW == 1920 && gFit.aspectH == 1080);
	MainWin_GetRect(o, &x, &y, &w, &h);
	CHECK(w == 1926 && h == 1109 && x >= -1 && x <= 1);
	// the player gives it a size: the scripts hear nothing, and it stays while they ask for what they had
	OpenBGI_WindowSize(o, 1280, 720);
	CHECK(o->display.windowSizeW == 0 && NextEvent(o) == -1);
	gFit.w = 1280; // (the host's window is of that size now)
	gFit.h = 720;
	MainWin_SetWindowed(o, 0, 0, 1920, 1080);
	CHECK(gFit.w == 1280 && gFit.h == 720);
	MainWin_SetFullscreen(o, 1920, 1080); // .. also over the full screen and back
	OpenBGI_WindowSize(o, 1920, 1080);    // (not the player's: not noted)
	MainWin_SetWindowed(o, 0, 0, 1920, 1080);
	CHECK(gFit.w == 1280 && gFit.h == 720);
	MainWin_SetWindowed(o, 0, 0, 1024, 576); // another size asked for: the game's wins
	CHECK(gFit.w == 1024 && gFit.h == 576 && gFit.aspectW == 1024);
	OpenBGI_WindowSize(o, 900, 500);
	gFit.w = 900;
	gFit.h = 500;
	MainWin_ViewReset(o); // "Reset window size"
	CHECK(gFit.w == 1024 && gFit.h == 576);
	// the settings' sizes: times the picture, what fits, whole steps
	MainWin_ViewApply(o, VIEW_SIZE_150, VIEW_FILTER_GAME, 0);
	CHECK(gFit.w == 1536 && gFit.h == 864);
	MainWin_ViewApply(o, VIEW_SIZE_200, VIEW_FILTER_GAME, 0);
	CHECK(gFit.w == 1797 && gFit.h == 1011);
	MainWin_ViewApply(o, VIEW_SIZE_100, VIEW_FILTER_GAME, 0);
	CHECK(gFit.w == 1024 && gFit.h == 576);
	MainWin_ViewApply(o, VIEW_SIZE_FIT, VIEW_FILTER_GAME, 0);
	CHECK(gFit.w == 1797 && gFit.h == 1011 && !(gFit.rule & OPENBGI_SCALE_WHOLE));
	MainWin_ViewApply(o, VIEW_SIZE_FIT, VIEW_FILTER_GAME, 1);
	CHECK(gFit.w == 1024 && gFit.h == 576 && (gFit.rule & OPENBGI_SCALE_WHOLE));
	// the filter: the player's before the game's
	MainWin_SetScaling(o, OPENBGI_SCALE_BICUBIC);
	CHECK(gFit.rule == (OPENBGI_SCALE_BICUBIC | OPENBGI_SCALE_WHOLE));
	MainWin_ViewApply(o, VIEW_SIZE_AUTO, VIEW_FILTER_NEAREST, 0);
	CHECK(gFit.rule == (OPENBGI_SCALE_NEAREST | OPENBGI_SCALE_CHOSEN));
	MainWin_SetScaling(o, OPENBGI_SCALE_BILINEAR);
	CHECK(gFit.rule == (OPENBGI_SCALE_NEAREST | OPENBGI_SCALE_CHOSEN));
	MainWin_ViewSettings(o, &size, &filter, &whole);
	CHECK(size == VIEW_SIZE_AUTO && filter == VIEW_FILTER_NEAREST && whole == 0);
	MainWin_ViewApply(o, VIEW_SIZE_AUTO, VIEW_FILTER_GAME, 0);
	CHECK(gFit.rule == OPENBGI_SCALE_BILINEAR);
	// the host's own filters by their names, the second one, and its better kind; one it has not got is the game's
	CHECK(strcmp(MainWin_ViewFilterName(o, 1), "Sharp B") == 0 && MainWin_ViewFilterName(o, 2) == NULL && MainWin_ViewFilterName(o, -1) == NULL);
	MainWin_ViewApply(o, VIEW_SIZE_AUTO, VIEW_FILTER_OWN + 1, 0);
	CHECK(gFit.rule == ((OPENBGI_SCALE_OWN + 1) | OPENBGI_SCALE_CHOSEN) && strcmp(o->window.setFilterName, "Sharp B") == 0);
	MainWin_ViewSetHigh(o, 1);
	CHECK(gFit.rule == ((OPENBGI_SCALE_OWN + 1) | OPENBGI_SCALE_CHOSEN | OPENBGI_SCALE_HIGH) && MainWin_ViewHigh(o) == 1);
	MainWin_ViewSetHigh(o, 0);
	MainWin_ViewApply(o, VIEW_SIZE_AUTO, VIEW_FILTER_OWN + 5, 0);
	CHECK(gFit.rule == OPENBGI_SCALE_BILINEAR && !o->window.setFilterName[0]);
	// one that was chosen in an earlier run is found by its name once the host has it, whatever the letters' case
	snprintf(o->window.setFilterName, sizeof o->window.setFilterName, "%s", "sharp a");
	MainWin_SetScaling(o, OPENBGI_SCALE_BILINEAR);
	MainWin_ViewSettings(o, &size, &filter, &whole);
	CHECK(filter == VIEW_FILTER_OWN && gFit.rule == (OPENBGI_SCALE_OWN | OPENBGI_SCALE_CHOSEN));
	snprintf(o->window.setFilterName, sizeof o->window.setFilterName, "%s", "gone");
	o->window.setFilter = VIEW_FILTER_GAME;
	MainWin_SetScaling(o, OPENBGI_SCALE_BICUBIC);
	CHECK(gFit.rule == OPENBGI_SCALE_BICUBIC && strcmp(o->window.setFilterName, "gone") == 0); // (kept for a run that has it)
	o->window.setFilterName[0] = 0;
	MainWin_SetScaling(o, OPENBGI_SCALE_BILINEAR);
	// a window the game sizes ("81 66"): any proportions, and the scripts hear of the player's size
	Display_SetResizable(o, 1);
	CHECK(gFit.aspectW == 0 && gFit.aspectH == 0 && gFit.resizable == 1);
	OpenBGI_WindowSize(o, 1000, 700);
	CHECK(o->display.windowSizeW == 1000 && o->display.windowSizeH == 700);
	MainWin_SetWindowed(o, 0, 0, 1920, 1080); // a size of its own that has no room: fitted, and the game is told
	CHECK(gFit.w == 1797 && gFit.h == 1011 && o->display.windowSizeW == 1797 && o->display.windowSizeH == 1011);
	MainWin_SetWindowed(o, 0, 0, 1797, 1011); // (which it sets in turn)
	CHECK(gFit.w == 1797 && gFit.h == 1011);
	Display_SetResizable(o, 0);
	CHECK(gFit.aspectW == 1797 && gFit.aspectH == 1011 && gFit.resizable == 1); // (the border stays: the view's)
	MainWindow_Destroy(o);
	OpenBGI_Delete(o);

	// a screen at one and a half: the scripts count a desktop of 1280 x 720, the window is one and a half theirs
	o = FitUp(144);
	CHECK(o != NULL);
	if(!o)
		return;
	MainWin_ScreenSize(o, &w, &h);
	MainWin_FrameMetrics(o, &fw, &fh);
	CHECK(w == 1280 && h == 720 && fw == 4 && fh == 19 && MainWin_FrameTop(o) == 17);
	CHECK(MainWindow_Create(o));
	CHECK(gFit.w == 1200 && gFit.h == 900 && gFit.x == 357 && gFit.y >= 64 && gFit.y <= 65);
	MainWin_GetRect(o, &x, &y, &w, &h);
	CHECK(x == 238 && y == 43 && w == 804 && h == 619); // centred on their desktop
	MainWin_SetWindowed(o, 0, 0, 1280, 720);            // "full" on their desktop: fitted
	CHECK(gFit.w == 1797 && gFit.h == 1011 && gFit.y == 0);
	Display_SetResizable(o, 1); // the player's size as the scripts count
	OpenBGI_WindowSize(o, 1500, 900);
	CHECK(o->display.windowSizeW == 1000 && o->display.windowSizeH == 600);
	MainWin_SetWindowed(o, 0, 0, 1000, 600); // (which they set in turn: the window stays as the player made it)
	CHECK(gFit.w == 1500 && gFit.h == 900);
	Display_SetResizable(o, 0);
	// the screen's scale becomes another: fitted again
	MainWin_SetWindowed(o, 100, 100, 800, 600);
	CHECK(gFit.w == 1200 && gFit.h == 900);
	gFit.dpi = 96;
	OpenBGI_ScreenChanged(o);
	CHECK(gFit.w == 800 && gFit.h == 600);
	MainWin_ScreenSize(o, &w, &h);
	CHECK(w == 1920 && h == 1080);
	// .. and the player's size follows it, as far as there is room
	OpenBGI_WindowSize(o, 1000, 750);
	gFit.w = 1000;
	gFit.h = 750;
	gFit.dpi = 120;
	OpenBGI_ScreenChanged(o);
	CHECK(gFit.w == 1250 && gFit.h == 938);
	gFit.dpi = 192;
	OpenBGI_ScreenChanged(o);
	CHECK(gFit.w == 1347 && gFit.h == 1011); // (1011 is all the height there is under the frame)
	MainWin_SetWindowed(o, 0, 0, 800, 600);  // still the player's while the scripts ask for what they had
	CHECK(gFit.w == 1347 && gFit.h == 1011);
	MainWindow_Destroy(o);
	OpenBGI_Delete(o);
}

static void TestDebugger(void)
{
	OpenBGI_Host_t host = gBare;
	OpenBGI_t* o;
	uint8_t px[32] = {9};
	printf("debugger\n");
	Reset();
	o = OpenBGI_New(&gBare);
	if(o)
	{
		CHECK(!MainWin_DebugOpen(o, 640, 480, "dbg")); // no table: the debugger draws over the picture
		MainWin_DebugPresent(o, px, 32, 4, 1);
		MainWin_DebugClose(o);
		OpenBGI_Delete(o);
	}
	host.user = &gWin;
	host.debugWindow = &kDebugWindow;
	o = OpenBGI_New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	CHECK(MainWin_DebugOpen(o, 640, 480, "dbg"));
	MainWin_DebugPresent(o, px, 32, 4, 1);
	MainWin_DebugClose(o);
	CHECK(strcmp(gWin.log, "dopen(640,480,dbg);dpresent(9,32,4,1);dclose;") == 0);
	gWin.debugOpenOk = 0;
	CHECK(!MainWin_DebugOpen(o, 640, 480, "dbg"));
	CHECK(gWin.badUser == 0);
	OpenBGI_Delete(o);
}

int main(void)
{
	TestNobody();
	TestHost();
	TestIcon();
	TestEvents();
	TestView();
	TestDebugger();
	if(gFails)
	{
		printf("window: %d checks failed\n", gFails);
		return 1;
	}
	printf("window: all tests passed\n");
	return 0;
}
