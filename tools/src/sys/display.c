/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * display.c - the display mode: picture sizes, windowed / full screen,
 *             the window's place on the screen; interface in bgi/display.h
 *
 * The engine draws a picture of one of the sizes in the size table (the
 * "size index" of "80 60") into a back buffer; this file decides how that
 * picture reaches the screen - in a window of the picture size, or full
 * screen in a display mode of that size (letter-boxed into the next
 * larger mode on wide screens when "80 63" asks for it).  The OS layer
 * does the actual mode switch and window placement; the compositor
 * (Gfx_Resize) is told the picture size, the mode size and the scaling.
 * Two settings of later builds ("80 6E", "81 63") are only stored here,
 * since the OS layer presents in its own way; "81 64", the window size
 * those builds let the player choose independently of the picture size,
 * sizes the client area, and the OS layer scales the picture into it.
 */
#include "bgi/display.h"
#include "bgi/engine.h"
#include "bgi/gfx.h"
#include "bgi/input.h"
#include "bgi/sysobj.h"
#include "bgi/sys.h"
#include "bgi/version.h"
#include "bgi/os.h"

int gFullscreen;         // the current mode is full screen
int gSizeIndex;          // the current picture size: an index into the size table
int gPixelMode;          // 0 = 16-bit, 1 = 32-bit pixels
int gDisplayEnabled = 1; // "90 01": nothing is presented while 0

/* The picture sizes by size index.  The table changed with the builds,
 * and from 1.494 on a script can redefine an entry ("81 60"). */
int gScreenWidths[8] = {320, 640, 800, 1024, 1152, 1280, 1600, 0};
int gScreenHeights[8] = {240, 480, 600, 768, 864, 960, 1200, 0};

static int gFrameW, gFrameH;           // what the window manager adds around the client area, pixels
static int gScreenW, gScreenH;         // the screen size (halved for two monitors side by side)
static int gAspectClass;               // 0 4:3 and others, 1 5:4, 2 16:10, 3 5:3, 4 1024x600, 5 16:9
static int gPosMode;                   // "80 63": letter-box into the next larger full-screen mode
static int gInSetDisplayMode;          // SetDisplayMode is running (its window changes must not re-enter it)
static int gStyleKeysOn;               // "80 62": the style keys are active
static uint32_t gStyleKeys[16];        // the keys that toggle windowed / full screen, 0-terminated
static int gWindowSizeW, gWindowSizeH; // "81 64" (1.529 on): the client size of the window, 0 for the picture size

/* the client size of the windowed window: the "81 64" size of 1.529 on
 * when the script set one, else the picture size */
static void Display_ClientSize(int picW, int picH, int* w, int* h)
{
	*w = picW;
	*h = picH;
	if(gEngine->gen >= GEN_1_529 && gWindowSizeW > 0 && gWindowSizeH > 0)
	{
		*w = gWindowSizeW;
		*h = gWindowSizeH;
	}
}

// Install the generation's size table (start-up, once the engine profile is known).
void Display_InitSizeTable(void)
{
	static const int w158[8] = {320, 640, 800, 1024, 0, 0, 0, 0}; // 1.58 .. 1.66: four modes
	static const int h158[8] = {240, 480, 600, 768, 0, 0, 0, 0};
	static const int w169[8] = {320, 640, 800, 1024, 1152, 1280, 1600, 0}; // 1.69
	static const int h169[8] = {240, 480, 600, 768, 864, 960, 1200, 0};
	static const int w494[8] = {320, 640, 800, 1024, 1024, 1024, 1280, 1920}; // 1.494 .. 1.654
	static const int h494[8] = {240, 480, 600, 768, 576, 600, 720, 1080};
	static const int w659[8] = {320, 640, 800, 1024, 1024, 1280, 1280, 1920}; // 1.659 on
	static const int h659[8] = {240, 480, 600, 768, 576, 960, 720, 1440};
	const int *w = w169, *h = h169;
	if(gEngine->gen <= GEN_1_66)
		w = w158, h = h158;
	else if(gEngine->gen >= GEN_1_659)
		w = w659, h = h659;
	else if(gEngine->gen >= GEN_1_494)
		w = w494, h = h494;
	memcpy(gScreenWidths, w, sizeof gScreenWidths);
	memcpy(gScreenHeights, h, sizeof gScreenHeights);
}

// "81 60" (1.494 on): redefine a size entry; 0 ok, 1 bad index, 2 bad size.
int Display_SetSizeEntry(int idx, int w, int h)
{
	if((unsigned)idx >= 8)
		return 1;
	if(w <= 0 || h <= 0)
		return 2;
	gScreenWidths[idx] = w;
	gScreenHeights[idx] = h;
	return 0;
}

// "80 63" (1.64 on): whether the full screen is letter-boxed on wide displays
void Display_SetPosMode(int v)
{
	gPosMode = v;
}

int Display_GetPosMode(void)
{
	return gPosMode;
}

// "90 01": switch presenting on or off
void Display_Enable(int on)
{
	gDisplayEnabled = on;
}

/* Measure the screen for the window placement: the frame metrics, the
 * screen size (halved for two monitors side by side) and the aspect
 * class the full-screen letter-boxing is chosen by.  Called once before
 * the window is created. */
void Display_MeasureScreen(void)
{
	OS_FrameMetrics(&gFrameW, &gFrameH);
	OS_ScreenSize(&gScreenW, &gScreenH);
	if(gScreenW * 4 / gScreenH >= 10) // wider than 2.5 : 1 is taken for two monitors
		gScreenW /= 2;
	switch(gScreenW * 100 / gScreenH)
	{
		case 125: gAspectClass = 1; break; // 5:4
		case 160: gAspectClass = 2; break; // 16:10
		case 166: gAspectClass = 3; break; // 5:3
		case 170: gAspectClass = 4; break; // 1024x600
		case 177: gAspectClass = 5; break; // 16:9
		default: gAspectClass = 0; break;  // 4:3 and others
	}
}

// The centred position of a window with a w x h client area.
void Display_CentredWindowPos(int w, int h, int* x, int* y)
{
	*x = (gScreenW - (w + gFrameW)) / 2;
	*y = (gScreenH - h) / 2 - OS_FrameTop();
}

/* "80 6F": whether the screen can show the next larger mode than size
 * index `sizeIdx` (for the letter-boxed full screen of wide displays);
 * always 0 on a 4:3 screen. */
int CanFullscreen(int sizeIdx)
{
	if(gAspectClass == 0 || sizeIdx >= 7)
		return 0;
	// the next entry must be a larger mode (the 1.494+ tables are not sorted)
	if(gScreenWidths[sizeIdx + 1] <= gScreenWidths[sizeIdx] || gScreenHeights[sizeIdx + 1] <= gScreenHeights[sizeIdx])
		return 0;
	return gScreenW >= gScreenWidths[sizeIdx + 1] && gScreenH >= gScreenHeights[sizeIdx + 1];
}

/* "80 60": switch between windowed and full screen and between picture
 * sizes.  Full screen tries the colour depth asked for, then 24 and 16
 * bits, and falls back to windowed mode.  On a wide display (aspect
 * class >= 2) with "80 63" set the next larger mode is used and the
 * picture is centred (letter-boxed) by the compositor.  A windowed
 * window keeps its position unless it comes out of full screen, when it
 * is centred.  Also reached by the style keys, Alt+Enter and the return
 * from a scripted minimise. */
void SetDisplayMode(int sizeIdx, int pixelMode, int fullscreen)
{
	int picW, picH, modeW, modeH, letterbox = 0;
	if(gInSetDisplayMode)
		return;
	gInSetDisplayMode = 1;
	picW = gScreenWidths[sizeIdx];
	picH = gScreenHeights[sizeIdx];
	modeW = picW;
	modeH = picH;
	if(fullscreen && CanFullscreen(sizeIdx) && gPosMode)
	{
		letterbox = 1;
		if(gAspectClass >= 2)
		{
			modeW = gScreenWidths[sizeIdx + 1];
			modeH = gScreenHeights[sizeIdx + 1];
		}
	}
	if(fullscreen)
	{
		static const int depths[3] = {0, 24, 16};
		int i, ok = 0;
		for(i = 0; i < 3 && !ok; i++)
		{
			int bits = i == 0 ? ModeBits(pixelMode) : depths[i];
			ok = OS_DisplaySetMode(1, modeW, modeH, bits);
		}
		if(!ok)
		{
			gInSetDisplayMode = 0;
			SetDisplayMode(sizeIdx, pixelMode, 0);
			return;
		}
		OS_WindowSetFullscreen(modeW, modeH);
	}
	else
	{
		int x, y, w, h, cw, ch;
		Display_ClientSize(picW, picH, &cw, &ch);
		if(gFullscreen)
		{
			OS_DisplayRestore();
			Display_CentredWindowPos(cw, ch, &x, &y);
		}
		else
		{
			OS_WindowGetRect(&x, &y, &w, &h); // keep the position
		}
		OS_WindowSetWindowed(x, y, cw, ch);
		OS_WindowSetPicture(picW, picH);
	}
	gSizeIndex = sizeIdx;
	gPixelMode = pixelMode;
	gFullscreen = fullscreen;
	Gfx_Resize(gGfx, picW, picH, pixelMode, letterbox, modeW, modeH, gAspectClass);
	OS_CursorShow(Cursor_IsShown()); // the mode switch resets the cursor count
	gInSetDisplayMode = 0;
}

// Back to the desktop's display mode (the scripted minimise, shutdown).
void Display_RestoreMode(void)
{
	OS_DisplayRestore();
}

/* "80 62": the keys (at most 15, 0-terminated) that toggle the display
 * style; NULL keys clears the list, enable 0 switches the toggling off
 * (the list is kept).  1 ok, 0 when the list is too long. */
int SetStyleChangeKeys(int enable, const uint32_t* keys)
{
	int n = 0;
	gStyleKeysOn = enable;
	if(!enable)
		return 1;
	if(!keys)
	{
		gStyleKeys[0] = 0;
		return 1;
	}
	while(keys[n])
		n++;
	if(n >= 0x10)
		return 0;
	memcpy(gStyleKeys, keys, (size_t)(n + 1) * sizeof(uint32_t));
	return 1;
}

// Whether a key toggles the display style (the window's key events ask).
int Display_StyleKeyMatch(uint32_t vk)
{
	const uint32_t* k;
	if(!gStyleKeysOn)
		return 0;
	for(k = gStyleKeys; *k; k++)
		if(*k == vk)
			return 1;
	return 0;
}

/* Toggle windowed / full screen (a style key or Alt+Enter): not while the
 * style keys are off or a movie plays.  Posts event 1 with the new state
 * to the script event queue; 1 when toggled. */
int Display_ToggleStyle(void)
{
	if(!gStyleKeysOn || gMoviePlaying)
		return 0;
	SetDisplayMode(gSizeIndex, gPixelMode, !gFullscreen);
	MsgQueue_Post(1, (uint32_t)gFullscreen, 0);
	return 1;
}

// The centred window position of the current size; 0, 0 in full screen ("80 80" of 1.69/472, "B0 02").
void Display_DefaultWindowPos(int* x, int* y)
{
	int cw, ch;
	if(gFullscreen)
	{
		*x = *y = 0;
		return;
	}
	Display_ClientSize(gScreenWidths[gSizeIndex], gScreenHeights[gSizeIndex], &cw, &ch);
	Display_CentredWindowPos(cw, ch, x, y);
}

// Re-centre the window after the screen's resolution changed (not in full screen or mid-switch).
void Display_OnDisplayChange(void)
{
	int x, y;
	if(gInSetDisplayMode || gFullscreen)
		return;
	OS_ScreenSize(&gScreenW, &gScreenH);
	Display_DefaultWindowPos(&x, &y);
	OS_WindowMove(x, y);
}

/* "80 6E" of 1.69 build 472 on: wait for the vertical blank before the
 * flip.  The OS layer presents here, so the flag is only stored. */
static int gVsyncWait = 1;

void Display_SetVsyncWait(int on)
{
	gVsyncWait = on;
}

int Display_GetVsyncWait(void)
{
	return gVsyncWait;
}

/* "81 63" of 1.529 on: how the picture fills the screen in full-screen
 * mode - 0 stretched, 1 keeping the aspect ratio, 2 unscaled and centred.
 * Stored only: this implementation presents as 1.69 does. */
static int gFullscreenFit = 1;

// 1 when the mode is 0 .. 2 and was stored, 0 otherwise
int Display_SetFullscreenFit(int mode)
{
	if((unsigned)mode >= 3)
		return 0;
	gFullscreenFit = mode;
	return 1;
}

int Display_GetFullscreenFit(void)
{
	return gFullscreenFit;
}

static int gRenderW, gRenderH; // "81 6C": the picture size the 1.669 renderer scales to (stored only)

/* "81 6C" of 1.669 on: a picture size the renderer of that build scales
 * its output to; accepted (1) when its aspect is the desktop's (the two
 * ratios within 0.001 of each other), else 0 and nothing changes.  A
 * non-positive size means the desktop's.  Stored only: the output here
 * is the game's own picture size. */
int Display_SetRenderSize(int w, int h)
{
	int sw, sh;
	float d;
	OS_ScreenSize(&sw, &sh);
	if(w <= 0 || h <= 0)
	{
		w = sw;
		h = sh;
	}
	d = (float)sw / (float)sh - (float)w / (float)h;
	if(d < 0)
		d = -d;
	if(d > 0.001f)
		return 0;
	gRenderW = w;
	gRenderH = h;
	return 1;
}

/* "81 64" of 1.529 on: the client size of the window, which those builds
 * stretch the picture to; 0, 0 restores the picture size.  A window that
 * is up is resized at once, where it stands (the scripts centre it
 * afterwards when they want that), and repainted. */
void Display_SetWindowSize(int w, int h)
{
	gWindowSizeW = w;
	gWindowSizeH = h;
	if(gEngine->gen >= GEN_1_529 && gWindowExists && !gFullscreen && !gInSetDisplayMode)
	{
		int x, y, ow, oh, cw, ch;
		int picW = gScreenWidths[gSizeIndex], picH = gScreenHeights[gSizeIndex];
		Display_ClientSize(picW, picH, &cw, &ch);
		OS_WindowGetRect(&x, &y, &ow, &oh);
		OS_WindowSetWindowed(x, y, cw, ch);
		OS_WindowSetPicture(picW, picH);
		Present_RequestFull();
	}
}

// "81 67" (1.669 on): the stored window size, or the picture size while none is set
void Display_GetWindowSize(int* w, int* h)
{
	if(gWindowSizeW > 0 && gWindowSizeH > 0)
	{
		*w = gWindowSizeW;
		*h = gWindowSizeH;
	}
	else
		Display_GetPictureSize(w, h);
}

/* "81 0E" of 1.529 on: the desktop size.  With `adjust` a desktop wider
 * than 2.5 times its height (two monitors side by side) reports half the
 * width, and one between 1 and 1.25 times as high as wide half the
 * height. */
void Display_GetDesktopSize(int out[2], int adjust)
{
	int w, h;
	OS_ScreenSize(&w, &h);
	if(adjust && w > 0 && h > 0)
	{
		int w0 = w;
		if(w * 4 / h >= 10)
			w /= 2;
		if(h / w0 >= 1 && h * 100 / w0 < 125)
			h /= 2;
	}
	out[0] = w;
	out[1] = h;
}

// The cursor position in back-buffer coordinates (through the display scaling).
void GetCursorBufferPos(int xy[2])
{
	int32_t p[2];
	int sx, sy;
	OS_CursorGetPos(&sx, &sy);
	Gfx_MapPoint(gGfx, p, sx, sy, 1);
	xy[0] = p[0];
	xy[1] = p[1];
}

// Move the cursor to a back-buffer position.
void SetCursorBufferPos(int x, int y)
{
	int32_t s[2];
	Gfx_MapPoint(gGfx, s, x, y, 0);
	OS_CursorSetPos(s[0], s[1]);
}
