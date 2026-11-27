/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_host.c - the host of an engine on the OS layer (src/host/os.h; the
 *             engine's side is include/openbgi.h): the services an engine is
 *             made with, the answers to its requests and the main loop
 *
 * The programs (src/host/<platform>/main.c), the tools and the tests make
 * their engines with OS_Host: the clocks are the back end's OS_TicksMs,
 * OS_PerfCounterNs and OS_LocalTime, the fonts its font files or its
 * renderer (OS_FontFiles, OS_FontRenderer), the files, the registry and
 * the windows its tables (os_files.c, os_window.c).  OS_AnswerRequests
 * answers what the engine asks with the back end's dialogs, one after the
 * other, each modal, the engine's sound kept going under them
 * (OS_SetModalTick); OS_ShowFrame puts what changed in a frame's picture
 * on the main window and paints the window again from that frame when it
 * is uncovered; OS_RunEngine is the original's main loop on top of these,
 * with the window's messages pumped and their input and events handed to
 * the engine, the system's pointer told and moved, and the back end's
 * sound output playing the engine's sound.
 * Apart from os_common.c, which some tests build with only part of a
 * back end.
 */
#include "host/os.h"
#include "host/os_common.h"

#include <string.h>

static uint32_t HostTicksMs(void* user)
{
	BGI_UNUSED(user);
	return OS_TicksMs();
}

static int HostPerfNs(void* user, int64_t* out)
{
	BGI_UNUSED(user);
	return OS_PerfCounterNs(out);
}

static void HostLocalTime(void* user, OpenBGI_DateTime_t* out)
{
	OsLocalTime_t t;
	BGI_UNUSED(user);
	OS_LocalTime(&t);
	out->year = t.year;
	out->month = t.month;
	out->dayOfWeek = t.dayOfWeek;
	out->day = t.day;
	out->hour = t.hour;
	out->minute = t.minute;
	out->second = t.second;
	out->millis = t.millis;
}

// "81 00": the back end shows the scripts' text in their encoding
static void HostTextEncoding(void* user, int utf8)
{
	BGI_UNUSED(user);
	if(!utf8)
		OsPreview_Stop(); // (a run for a first picture names its files in the launcher's UTF-8)
	OS_SetTextEncoding(utf8);
}

static OpenBGI_Host_t gHost; // made on the first call

/* the clocks, the back end's fonts (its font files or its renderer, the
 * other one NULL), what it does itself, its text entry, its child
 * windows, its files and registry (src/host/os_files.c), its text
 * encoding, its main window and its debugger's window
 * (src/host/os_window.c), its machine and its shell (src/host/os_machine.c),
 * its CD audio and movie player (src/host/os_media.c) and the movie
 * decoder (src/host/os_ffmpeg.c) */
const OpenBGI_Host_t* OS_Host(void)
{
	if(!gHost.ticksMs)
	{
		gHost.size = sizeof gHost;
		gHost.api = OPENBGI_API_VERSION;
		gHost.fontFiles = OS_FontFiles();
		gHost.fontRenderer = OS_FontRenderer();
		gHost.localTime = HostLocalTime;
		gHost.perfNs = HostPerfNs;
		gHost.ticksMs = HostTicksMs;
		gHost.caps = OS_HostCaps();
#if !defined(BGI_WASM)
		gHost.caps |= OPENBGI_CAP_JOBS; // the launcher's reading goes on a thread beside the frames (OsPreview_Poll)
#endif
		gHost.textEntry = OS_TextEntry();
		gHost.childWindows = OS_ChildWindows();
		gHost.files = OsCommon_Files();
		OsCommon_BeforeGameDir(OsPreview_Stop); // (a run for a first picture reads its files by the directory that was)
		gHost.registry = OsCommon_Registry();
		gHost.textEncoding = HostTextEncoding;
		gHost.window = OsCommon_Window();
		gHost.debugWindow = OsCommon_DebugWindow();
		gHost.machine = OsCommon_Machine();
		gHost.shell = OsCommon_Shell();
		gHost.cdAudio = OsCommon_CdAudio();
		gHost.moviePlayer = OsCommon_MoviePlayer();
		gHost.movieDecoder = OsCommon_MovieDecoder();
	}
	return &gHost;
}

// ---- the sound ----------------------------------------------------------------------

/* How far ahead of the sound output the engine mixes (OpenBGI_SetAudioLead):
 * the longest frame, the loop's wait between two frames (at most 10 ms,
 * OS_IdleSleepMs) and the 20 ms a back end reads at once, with room to
 * spare. */
#define HOST_AUDIO_LEAD_MS 60

// the output's callback, on its thread: the engine's sound
static int HostAudioFill(void* user, int16_t* samples, int frames)
{
	return OpenBGI_AudioRead((OpenBGI_t*)user, samples, frames);
}

// the tick of a box's modal loop: the engine's sound goes on (and its fade timer, OpenBGI_RunAudio)
static void HostModalTick(void* user)
{
	OpenBGI_RunAudio((OpenBGI_t*)user);
}

// ---- the input --------------------------------------------------------------------

// the window's input (OS_SetHostHandlers), handed to the engine as it comes
static void HostKey(void* user, int vk, int down, int flags)
{
	OpenBGI_Key((OpenBGI_t*)user, vk, down, flags);
}

/* The pointer's reports.  It is a move of the player's when the system's
 * pointer is not where it was on the screen at the last report while the
 * window is where it was; a place in the picture that changed otherwise
 * - the window came up or moved under a pointer at rest, the engine put
 * the pointer there itself - is none.  1 for a move. */
static int gPointerKnown, gPointerX, gPointerY; // the pointer's place on the screen at the last report,
static int gOriginX, gOriginY;                  // .. and the picture's

static int HostPointerMoved(void)
{
	int sx, sy, ox = 0, oy = 0, moved;
	OS_CursorGetPos(&sx, &sy);
	OS_ClientToScreen(&ox, &oy);
	moved = gPointerKnown && ox == gOriginX && oy == gOriginY && (sx != gPointerX || sy != gPointerY);
	gPointerKnown = 1;
	gPointerX = sx;
	gPointerY = sy;
	gOriginX = ox;
	gOriginY = oy;
	return moved;
}

// the window's own report of a move: one that is none (above) is left to the place told before each frame
static void HostPointer(void* user, int x, int y)
{
	if(HostPointerMoved())
		OpenBGI_Pointer((OpenBGI_t*)user, x, y);
}

static void HostButton(void* user, int button, int down, int x, int y, int flags)
{
	OpenBGI_Button((OpenBGI_t*)user, button, down, x, y, flags);
}

static void HostWheel(void* user, int delta)
{
	OpenBGI_Wheel((OpenBGI_t*)user, delta);
}

static void HostTouch(void* user, int id, int phase, int x, int y)
{
	OpenBGI_Touch((OpenBGI_t*)user, id, phase, x, y);
}

static void HostTouchScale(void* user, int picturePixelsPerInch)
{
	OpenBGI_TouchScale((OpenBGI_t*)user, picturePixelsPerInch);
}

static void HostSurface(void* user, const OpenBGI_Surface_t* surface)
{
	OpenBGI_SetSurface((OpenBGI_t*)user, surface);
}

static void HostSurfaceTouch(void* user, int id, int phase, int x, int y)
{
	OpenBGI_SurfaceTouch((OpenBGI_t*)user, id, phase, x, y);
}

// the pads whose state changed since the last frame, to the engine
static void HostPads(OpenBGI_t* o)
{
	uint32_t changed = OS_PadPoll();
	int port;
	for(port = 0; changed && port < 4; port++)
		if(changed & (1u << port))
			OpenBGI_Pad(o, port, OS_PadState(port));
}

static void HostFocus(void* user, int active)
{
	OpenBGI_Focus((OpenBGI_t*)user, active);
}

static void HostText(void* user, const char* utf8)
{
	OpenBGI_Text((OpenBGI_t*)user, utf8);
}

// ---- the window's events ----------------------------------------------------------

static void HostClose(void* user)
{
	OpenBGI_Close((OpenBGI_t*)user);
}

static void HostMinimized(void* user, int minimized)
{
	OpenBGI_Minimized((OpenBGI_t*)user, minimized);
}

static void HostResized(void* user, int w, int h)
{
	OpenBGI_WindowSize((OpenBGI_t*)user, w, h);
}

static void HostDisplayChanged(void* user)
{
	OpenBGI_DisplayChanged((OpenBGI_t*)user);
}

static void HostScreenChanged(void* user)
{
	OpenBGI_ScreenChanged((OpenBGI_t*)user);
}

static void HostDropFile(void* user, const char* path)
{
	OpenBGI_DropFile((OpenBGI_t*)user, path);
}

static void HostMessage(void* user, uint32_t msg, uint32_t wParam, uint32_t lParam)
{
	OpenBGI_WindowMessage((OpenBGI_t*)user, msg, wParam, lParam);
}

static void HostCdNotify(void* user, int status)
{
	OpenBGI_CdNotify((OpenBGI_t*)user, status);
}

static void HostMovieEvent(void* user, int completed)
{
	OpenBGI_MovieEvent((OpenBGI_t*)user, completed);
}

static void HostQuit(void* user)
{
	OpenBGI_Quit((OpenBGI_t*)user);
}

static void HostDebugKey(void* user, int vk, int down, int ch)
{
	OpenBGI_DebugKey((OpenBGI_t*)user, vk, down, ch);
}

static void HostDebugMouse(void* user, int x, int y, int button, int down)
{
	OpenBGI_DebugMouse((OpenBGI_t*)user, x, y, button, down);
}

static void HostDebugWheel(void* user, int delta)
{
	OpenBGI_DebugWheel((OpenBGI_t*)user, delta);
}

static void HostDebugClose(void* user)
{
	OpenBGI_DebugClose((OpenBGI_t*)user);
}

/* Where the system's pointer is, in the picture's pixels, to the engine:
 * before each frame, as the original reads it whenever it needs it (also
 * when it is outside the window, where no event says so). */
static void HostTellPointer(OpenBGI_t* o)
{
	int x, y, moved = HostPointerMoved();
	OS_CursorGetPos(&x, &y);
	OS_ScreenToClient(&x, &y);
	if(moved)
		OpenBGI_Pointer(o, x, y);
	else
		OpenBGI_PointerPlace(o, x, y);
}

// the system's pointer where the frame moved the engine's
static void HostMovePointer(const OpenBGI_Frame_t* f)
{
	int x = f->pointerX, y = f->pointerY;
	if(!f->pointerMoved)
		return;
	OS_ClientToScreen(&x, &y);
	OS_CursorSetPos(x, y);
	OS_CursorGetPos(&gPointerX, &gPointerY); // (where the system put it: no move of the player's)
}

// ---- the requests ----------------------------------------------------------------

// the answer the back end's window gives to request q (openbgi.h states each)
static int AnswerRequest(const OpenBGI_Request_t* q)
{
	switch(q->kind)
	{
		case OPENBGI_ASK_MESSAGE: return OS_MessageBox(q->u.message.text, q->u.message.caption, q->u.message.flags);
		case OPENBGI_ASK_INPUT:
			return OS_DialogInput1(q->u.input.title, q->u.input.initial, q->u.input.maxLen, q->u.input.digits, q->u.input.out);
		case OPENBGI_ASK_INPUT2:
			return OS_DialogInput2(q->u.input2.title, q->u.input2.label[0], q->u.input2.initial[0], q->u.input2.maxLen[0], q->u.input2.digits[0],
				q->u.input2.out[0], q->u.input2.label[1], q->u.input2.initial[1], q->u.input2.maxLen[1], q->u.input2.digits[1], q->u.input2.out[1],
				q->u.input2.compact);
		case OPENBGI_ASK_PROFILE:
			return OS_DialogProfile(q->u.profile.surname, q->u.profile.givenName, q->u.profile.nickname, q->u.profile.pronoun, q->u.profile.month,
				q->u.profile.day);
		case OPENBGI_ASK_FILE:
			if(q->u.file.filter)
				return OS_FileDialog(q->u.file.save, q->u.file.path, q->u.file.pathSize, q->u.file.filter, q->u.file.defExt, q->u.file.initialDir,
					q->u.file.title);
			return OS_FileDialogN(q->u.file.save, q->u.file.path, q->u.file.pathSize, q->u.file.desc, q->u.file.pattern, q->u.file.count,
				q->u.file.defExt, q->u.file.initialDir, q->u.file.title);
		case OPENBGI_ASK_FOLDER: return OS_BrowseFolder(q->u.folder.path, q->u.folder.pathSize, q->u.folder.title, q->u.folder.initial);
		case OPENBGI_ASK_GAME_CHOOSER: return OS_GameChooserDialog((OpenBGI_GameChooser_t*)q->u.chooser.chooser);
		case OPENBGI_ASK_INSTALLER: return OS_InstallerDialog(q->u.installer.dialog, q->u.installer.ctx);
		default: return 0;
	}
}

void OS_AnswerRequests(OpenBGI_t* o)
{
	OpenBGI_Request_t q = {.size = sizeof q};
	while(OpenBGI_NextRequest(o, &q))
	{
		int r;
		OS_SetModalTick(HostModalTick, o);
		r = AnswerRequest(&q);
		OS_SetModalTick(NULL, NULL);
		OpenBGI_Answer(o, q.id, r);
	}
}

// ---- the picture --------------------------------------------------------------

static int gTextOn;                                      // the input method is on (OS_TextInput) ..
static OpenBGI_Rect_t gTextRect;                         // .. for the field there
static OpenBGI_Frame_t gFrame = {.size = sizeof gFrame}; // the frame shown last ..
static int gFrameKept;                                   // .. while its pixels are valid (until the next frame)
static int gFrameShown;                                  // a frame was shown since the start
static int gLayerOn;                                     // the back end has a layer of a frame (OS_WindowLayer)
static int gRepaint;                                     // the window was uncovered while a frame was being made: all of it again after

// w x h pixels of the kept frame's picture at (x, y) to the main window
static void BlitKept(int x, int y, int w, int h)
{
	OsSurface_t s;
	OsDc_t* dc;
	if(!gFrame.pixels || w <= 0 || h <= 0 || !(dc = OS_WindowGetDc()))
		return;
	s.pixels = (void*)gFrame.pixels; // only read
	s.pitch = gFrame.pitch;
	s.width = gFrame.width;
	s.height = gFrame.height;
	s.bpp = gFrame.bpp;
	OS_BlitToWindow(dc, x, y, w, h, &s, x, y);
	OS_WindowReleaseDc(dc);
}

// the text entry over the picture again
static void RefreshEntry(void)
{
	const OpenBGI_TextEntry_t* t = OS_TextEntry();
	if(t && t->refresh)
		t->refresh(NULL);
}

/* The window shown again as the frame says (OpenBGI_Frame_t.expose):
 * its picture with the text entry over it, black, or left alone. */
static void Expose(void)
{
	if(gLayerOn && gFrame.layer) // (a back end that lost what it drew with has lost the layer too)
		OS_WindowLayer(gFrame.layer, gFrame.layerWidth, gFrame.layerHeight, gFrame.layerPitch, 0, 0, gFrame.layerWidth, gFrame.layerHeight);
	if(gFrame.expose == OPENBGI_EXPOSE_NONE)
		return;
	if(gFrame.expose == OPENBGI_EXPOSE_BLACK || !gFrame.pixels)
	{
		OS_WindowFillBlack();
		return;
	}
	BlitKept(0, 0, gFrame.width, gFrame.height);
	RefreshEntry();
	OS_WindowPresent();
}

// the frame's layer to the back end, where it changed; 1 when it did
static int ShowLayer(const OpenBGI_Frame_t* f)
{
	if(!f->layer)
	{
		if(!gLayerOn)
			return 0;
		gLayerOn = 0;
		OS_WindowLayer(NULL, 0, 0, 0, 0, 0, 0, 0);
		return 1;
	}
	if(!f->layerChanges && gLayerOn)
		return 0;
	if(f->layerChanges > 0 && gLayerOn)
		OS_WindowLayer(f->layer, f->layerWidth, f->layerHeight, f->layerPitch, f->layerChange.x, f->layerChange.y, f->layerChange.w, f->layerChange.h);
	else
		OS_WindowLayer(f->layer, f->layerWidth, f->layerHeight, f->layerPitch, 0, 0, f->layerWidth, f->layerHeight);
	gLayerOn = 1;
	return 1;
}

/* The changed rectangles of the frame's picture (all of it for -1) to the
 * main window, and the back end's text entry painted over them again (its
 * background is the picture); the input method on where the frame takes
 * typed text, off when it no longer does.  The frame is kept for the
 * repaints until the next one; a window uncovered while it was made is
 * shown again as it says. */
void OS_ShowFrame(const OpenBGI_Frame_t* f)
{
	int i, layer;
	if(f->textInput != gTextOn || (f->textInput && memcmp(&gTextRect, &f->textRect, sizeof gTextRect) != 0))
	{
		gTextOn = f->textInput;
		gTextRect = f->textRect;
		OS_TextInput(f->textInput, f->textRect.x, f->textRect.y, f->textRect.w, f->textRect.h);
	}
	gFrame = *f;
	gFrameKept = 1;
	gFrameShown = 1;
	layer = ShowLayer(f);
	if(gRepaint)
	{ // uncovered in the meantime
		gRepaint = 0;
		Expose();
		if(f->expose == OPENBGI_EXPOSE_PICTURE)
			return; // (all of it)
	}
	if(!f->pixels || !f->changes)
	{
		if(layer)
			OS_WindowPresent(); // (the layer alone is another)
		return;
	}
	if(f->changes < 0)
		BlitKept(0, 0, f->width, f->height);
	else
		for(i = 0; i < f->changes; i++)
			BlitKept(f->change[i].x, f->change[i].y, f->change[i].w, f->change[i].h);
	RefreshEntry();
	OS_WindowPresent();
}

/* Before the next frame (or the stop): the kept frame's pixels are no
 * longer to be read; a repaint until the next frame paints all of it. */
static void ForgetFrame(void)
{
	gFrameKept = 0;
}

/* The window was uncovered (the input handlers' `paint`): again from the
 * frame shown last; black before the first one (the engine is not up);
 * after the frame being made while there is one (a window shown at once,
 * "80 64", paints within it). */
static void HostPaint(void* user)
{
	BGI_UNUSED(user);
	if(gFrameKept)
		Expose();
	else if(gFrameShown)
		gRepaint = 1;
	else
		OS_WindowFillBlack();
}

/* The text entry paints itself (the input handlers' `entryPaint`): the
 * picture under it first, a rectangle of the main window's client area;
 * while a frame is being made, all of that frame after it. */
static void HostEntryPaint(void* user, int x, int y, int w, int h)
{
	BGI_UNUSED(user);
	if(!gFrameKept)
	{
		gRepaint = gFrameShown;
		return;
	}
	if(gFrame.expose != OPENBGI_EXPOSE_PICTURE)
		return;
	if(x < 0)
	{
		w += x;
		x = 0;
	}
	if(y < 0)
	{
		h += y;
		y = 0;
	}
	if(x + w > gFrame.width)
		w = gFrame.width - x;
	if(y + h > gFrame.height)
		h = gFrame.height - y;
	BlitKept(x, y, w, h);
}

// ---- the main loop --------------------------------------------------------------

/* Run the game the command line names; the process exit code.  The timer
 * resolution and the idle wait are the host's: the original sets them up
 * with the engine (Engine_Init) and frees them with it.  The window's
 * input goes to the engine from the start (the window is made during the
 * first frames); the sound output runs while the frames do, and without
 * one the engine's sound is mixed for nobody (lead 0). */
int OS_RunEngine(OpenBGI_t* o, const char* cmdLine)
{
	OpenBGI_Frame_t f = {.size = sizeof f};
	OsHostHandlers_t in;
	int r, audio = 0;
	OS_TimerBeginPeriod();
	OS_IdleInit();
	memset(&in, 0, sizeof in);
	in.key = HostKey;
	in.pointer = HostPointer;
	in.button = HostButton;
	in.wheel = HostWheel;
	in.focus = HostFocus;
	in.text = HostText;
	in.touch = HostTouch;
	in.touchScale = HostTouchScale;
	in.surface = HostSurface;
	in.surfaceTouch = HostSurfaceTouch;
	in.paint = HostPaint;
	in.entryPaint = HostEntryPaint;
	in.close = HostClose;
	in.minimized = HostMinimized;
	in.resized = HostResized;
	in.displayChanged = HostDisplayChanged;
	in.screenChanged = HostScreenChanged;
	in.modal = HostModalTick;
	in.dropFile = HostDropFile;
	in.message = HostMessage;
	in.cdNotify = HostCdNotify;
	in.movieEvent = HostMovieEvent;
	in.quit = HostQuit;
	in.debugKey = HostDebugKey;
	in.debugMouse = HostDebugMouse;
	in.debugWheel = HostDebugWheel;
	in.debugClose = HostDebugClose;
	in.user = o;
	OS_SetHostHandlers(&in);
	if(OpenBGI_Start(o, cmdLine) == OPENBGI_RUNNING)
	{
		audio = OS_AudioStart(OPENBGI_AUDIO_RATE, HostAudioFill, o);
		if(audio)
			OpenBGI_SetAudioLead(o, HOST_AUDIO_LEAD_MS);
		HostPads(o); // (a pad that is there from the start is known before the first box is asked for)
		for(;;)
		{
			OS_AnswerRequests(o);
			HostTellPointer(o);
			ForgetFrame();
			if(OpenBGI_RunFrame(o, &f) != OPENBGI_RUNNING)
				break;
			OS_ShowFrame(&f);
			HostMovePointer(&f);
			while(OS_PumpMessages())
				; // the window's messages, to the engine through the handlers above
			HostPads(o);
			OpenBGI_PumpEvents(o);
			OsPreview_Poll(o); // the launcher's first pictures: a game at a time, on a thread beside this one
			if(f.idleMs)
				OS_IdleSleepMs(f.idleMs); // "80 52" (1.535 on) after a frame that presented nothing, or what the engine waits for
			else
				OS_IdleSleep(); // the half-millisecond wait of 1.69
		}
	}
	if(gTextOn)
	{ // the input method off again
		gTextOn = 0;
		OS_TextInput(0, 0, 0, 0, 0);
	}
	ForgetFrame(); // (a repaint from here on: black)
	memset(&gFrame, 0, sizeof gFrame);
	gFrame.size = sizeof gFrame;
	gFrameShown = 0;
	if(gLayerOn)
		OS_WindowLayer(NULL, 0, 0, 0, 0, 0, 0, 0);
	gLayerOn = 0;
	gRepaint = 0;
	OsPreview_Stop();
	r = OpenBGI_Stop(o);
	OS_AnswerRequests(o); // a box the shutdown left ("80 E1")
	if(audio)
		OS_AudioStop();
	OS_SetHostHandlers(NULL);
	OS_PadShutdown();
	OS_IdleShutdown();
	OS_TimerEndPeriod();
	return r;
}

static const void* gLauncher; // OS_UseLauncher: the launcher's program for OS_RunMain's engine
static uint32_t gLauncherSize;

void OS_UseLauncher(const void* program, uint32_t size)
{
	gLauncher = program;
	gLauncherSize = size;
}

static const void* gIcon; // OS_UseIcon: the program's icon for OS_RunMain's engine
static uint32_t gIconSize;

void OS_UseIcon(const void* ico, uint32_t size)
{
	gIcon = ico;
	gIconSize = size;
}

/* The program: the engine made before the OS layer starts and freed after
 * it stopped (the sound output reads from it until OS_RunEngine stops it). */
int OS_RunMain(const char* cmdLine)
{
	OpenBGI_t* o = OpenBGI_New(OS_Host());
	int r = -1;
	if(o && gLauncher) // started without a game, the program shows the launcher it carries
		OpenBGI_SetLauncher(o, gLauncher, gLauncherSize);
	if(o && gIcon) // its window has the program's icon, but for a game with one of its own
		OpenBGI_SetIcon(o, gIcon, gIconSize);
	if(o && OS_Init())
	{
		r = OS_RunEngine(o, cmdLine);
		OS_Shutdown();
	}
	OpenBGI_Delete(o);
	return r;
}

/* The arguments of main() as one command line, as GetCommandLine would
 * give it without the program name: joined by single spaces, an argument
 * with a space or a tab in it (a game directory "VN/[August] Fortune
 * Arterial", Do not use mutex.) in double quotes unless it already is in
 * them.  Arguments that do not fit are dropped. */
void OS_JoinArgs(int argc, char** argv, char* out, size_t n)
{
	size_t len = 0;
	int i;
	if(!n)
		return;
	out[0] = 0;
	for(i = 1; i < argc; i++)
	{
		const char* a = argv[i];
		size_t k = strlen(a);
		int quote = strpbrk(a, " \t") != NULL && !(k >= 2 && a[0] == '"' && a[k - 1] == '"');
		if(len + k + 4 >= n)
			break;
		if(len)
			out[len++] = ' ';
		if(quote)
			out[len++] = '"';
		memcpy(out + len, a, k);
		len += k;
		if(quote)
			out[len++] = '"';
		out[len] = 0;
	}
}
