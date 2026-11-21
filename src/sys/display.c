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
 * larger mode on wide screens when "80 63" asks for it).  The host's
 * window does the actual mode switch and window placement (MainWin_*);
 * the compositor (Gfx_Resize) is told the picture size, the mode size and
 * the scaling.  Two settings of later builds ("80 6E", "81 63") are only
 * stored here, since the host presents in its own way; "81 64", the
 * window size those builds let the player choose independently of the
 * picture size, sizes the client area, and the host scales the picture
 * into it.
 */
#include "bgi/display.h"
#include "bgi/engine.h"
#include "bgi/gfx.h"
#include "bgi/input.h"
#include "bgi/sysobj.h"
#include "bgi/sys.h"
#include "bgi/version.h"
#include "bgi/core.h"

/* The picture sizes by size index.  The table changed with the builds,
 * and from 1.492 on a script can redefine an entry ("81 60"). */

/* the client size of the windowed window: the "81 64" size of 1.529 on
 * when the script set one, else the picture size */
static void Display_ClientSize(OpenBGI_t* o, int picW, int picH, int* w, int* h)
{
	*w = picW;
	*h = picH;
	if(o->engine.profile->gen >= GEN_1_529 && o->display.windowSizeW > 0 && o->display.windowSizeH > 0)
	{
		*w = o->display.windowSizeW;
		*h = o->display.windowSizeH;
	}
}

// Install the generation's size table (start-up, once the engine profile is known).
void Display_InitSizeTable(OpenBGI_t* o)
{
	static const int w158[8] = {320, 640, 800, 1024, 0, 0, 0, 0}; // 1.58 .. 1.66: four modes
	static const int h158[8] = {240, 480, 600, 768, 0, 0, 0, 0};
	static const int w169[8] = {320, 640, 800, 1024, 1152, 1280, 1600, 0}; // 1.69
	static const int h169[8] = {240, 480, 600, 768, 864, 960, 1200, 0};
	static const int w494[8] = {320, 640, 800, 1024, 1024, 1024, 1280, 1920}; // 1.492 .. 1.654
	static const int h494[8] = {240, 480, 600, 768, 576, 600, 720, 1080};
	static const int w659[8] = {320, 640, 800, 1024, 1024, 1280, 1280, 1920}; // 1.659 on
	static const int h659[8] = {240, 480, 600, 768, 576, 960, 720, 1440};
	const int *w = w169, *h = h169;
	if(o->engine.profile->gen <= GEN_1_66)
		w = w158, h = h158;
	else if(o->engine.profile->gen >= GEN_1_659)
		w = w659, h = h659;
	else if(o->engine.profile->gen >= GEN_1_492)
		w = w494, h = h494;
	memcpy(o->display.screenWidths, w, sizeof o->display.screenWidths);
	memcpy(o->display.screenHeights, h, sizeof o->display.screenHeights);
}

// "81 60" (1.492 on): redefine a size entry; 0 ok, 1 bad index, 2 bad size.
int Display_SetSizeEntry(OpenBGI_t* o, int idx, int w, int h)
{
	if((unsigned)idx >= 8)
		return 1;
	if(w <= 0 || h <= 0)
		return 2;
	o->display.screenWidths[idx] = w;
	o->display.screenHeights[idx] = h;
	return 0;
}

// "80 63" (1.64 on): whether the full screen is letter-boxed on wide displays
void Display_SetPosMode(OpenBGI_t* o, int v)
{
	o->display.posMode = v;
}

// "90 01": switch presenting on or off
void Display_Enable(OpenBGI_t* o, int on)
{
	o->display.displayEnabled = on;
}

/* Measure the screen for the window placement: the frame metrics, the
 * screen size (halved for two monitors side by side) and the aspect
 * class the full-screen letter-boxing is chosen by.  Called once before
 * the window is created. */
void Display_MeasureScreen(OpenBGI_t* o)
{
	MainWin_FrameMetrics(o, &o->display.frameW, &o->display.frameH);
	MainWin_ScreenSize(o, &o->display.screenW, &o->display.screenH);
	if(o->display.screenW * 4 / o->display.screenH >= 10) // wider than 2.5 : 1 is taken for two monitors
		o->display.screenW /= 2;
	switch(o->display.screenW * 100 / o->display.screenH)
	{
		case 125: o->display.aspectClass = 1; break; // 5:4
		case 160: o->display.aspectClass = 2; break; // 16:10
		case 166: o->display.aspectClass = 3; break; // 5:3
		case 170: o->display.aspectClass = 4; break; // 1024x600
		case 177: o->display.aspectClass = 5; break; // 16:9
		default: o->display.aspectClass = 0; break;  // 4:3 and others
	}
}

/* Where the main window's client area lies on the screen, as the frame
 * the window manager puts around it says (the screen's origin in full
 * screen or without a window): what the positions of the child windows
 * the engine draws are taken from. */
void Display_ClientOrigin(OpenBGI_t* o, int* x, int* y)
{
	int w, h, fw, fh;
	*x = *y = 0;
	if(o->display.fullscreen || !o->display.windowAlive)
		return;
	MainWin_GetRect(o, x, y, &w, &h);
	MainWin_FrameMetrics(o, &fw, &fh);
	*x += fw / 2;
	*y += MainWin_FrameTop(o);
}

// The centred position of a window with a w x h client area.
void Display_CentredWindowPos(OpenBGI_t* o, int w, int h, int* x, int* y)
{
	*x = (o->display.screenW - (w + o->display.frameW)) / 2;
	*y = (o->display.screenH - h) / 2 - MainWin_FrameTop(o);
}

/* "80 6F": whether the screen can show the next larger mode than size
 * index `sizeIdx` (for the letter-boxed full screen of wide displays);
 * always 0 on a 4:3 screen. */
int CanFullscreen(OpenBGI_t* o, int sizeIdx)
{
	if(o->display.aspectClass == 0 || sizeIdx >= 7)
		return 0;
	// the next entry must be a larger mode (the 1.492+ tables are not sorted)
	if(o->display.screenWidths[sizeIdx + 1] <= o->display.screenWidths[sizeIdx] || o->display.screenHeights[sizeIdx + 1] <= o->display.screenHeights[sizeIdx])
		return 0;
	return o->display.screenW >= o->display.screenWidths[sizeIdx + 1] && o->display.screenH >= o->display.screenHeights[sizeIdx + 1];
}

/* "80 60": switch between windowed and full screen and between picture
 * sizes.  Full screen tries the colour depth asked for, then 24 and 16
 * bits, and falls back to windowed mode.  On a wide display (aspect
 * class >= 2) with "80 63" set the next larger mode is used and the
 * picture is centred (letter-boxed) by the compositor.  A windowed
 * window keeps its position unless it comes out of full screen, when it
 * is centred.  Also reached by the style keys, Alt+Enter and the return
 * from a scripted minimise. */
void SetDisplayMode(OpenBGI_t* o, int sizeIdx, int pixelMode, int fullscreen)
{
	int picW, picH, modeW, modeH, letterbox = 0;
	if(o->display.inSetDisplayMode)
		return;
	o->display.inSetDisplayMode = 1;
	picW = o->display.screenWidths[sizeIdx];
	picH = o->display.screenHeights[sizeIdx];
	modeW = picW;
	modeH = picH;
	if(fullscreen && CanFullscreen(o, sizeIdx) && o->display.posMode)
	{
		letterbox = 1;
		if(o->display.aspectClass >= 2)
		{
			modeW = o->display.screenWidths[sizeIdx + 1];
			modeH = o->display.screenHeights[sizeIdx + 1];
		}
	}
	if(fullscreen)
	{
		static const int depths[3] = {0, 24, 16};
		int i, ok = 0;
		for(i = 0; i < 3 && !ok; i++)
		{
			int bits = i == 0 ? ModeBits(pixelMode) : depths[i];
			ok = MainWin_SetMode(o, modeW, modeH, bits);
		}
		if(!ok)
		{
			o->display.inSetDisplayMode = 0;
			SetDisplayMode(o, sizeIdx, pixelMode, 0);
			return;
		}
		MainWin_SetFullscreen(o, modeW, modeH);
	}
	else
	{
		int x, y, w, h, cw, ch;
		Display_ClientSize(o, picW, picH, &cw, &ch);
		if(o->display.fullscreen)
		{
			MainWin_RestoreMode(o);
			Display_CentredWindowPos(o, cw, ch, &x, &y);
		}
		else
		{
			MainWin_GetRect(o, &x, &y, &w, &h); // keep the position
		}
		MainWin_SetWindowed(o, x, y, cw, ch);
		MainWin_SetPicture(o, picW, picH);
	}
	o->display.sizeIndex = sizeIdx;
	o->display.pixelMode = pixelMode;
	o->display.fullscreen = fullscreen;
	Gfx_Resize(o, o->gfx.gfx, picW, picH, pixelMode, letterbox, modeW, modeH, o->display.aspectClass);
	MainWin_CursorShow(o, Cursor_IsShown(o)); // the mode switch resets the cursor count
	o->display.inSetDisplayMode = 0;
}

// Back to the desktop's display mode (the scripted minimise, shutdown).
void Display_RestoreMode(OpenBGI_t* o)
{
	MainWin_RestoreMode(o);
}

/* "80 62": the keys (at most 15, 0-terminated) that toggle the display
 * style; NULL keys clears the list, enable 0 switches the toggling off
 * (the list is kept).  1 ok, 0 when the list is too long. */
int SetStyleChangeKeys(OpenBGI_t* o, int enable, const uint32_t* keys)
{
	int n = 0;
	o->display.styleKeysOn = enable;
	if(!enable)
		return 1;
	if(!keys)
	{
		o->display.styleKeys[0] = 0;
		return 1;
	}
	while(keys[n])
		n++;
	if(n >= 0x10)
		return 0;
	memcpy(o->display.styleKeys, keys, (size_t)(n + 1) * sizeof(uint32_t));
	return 1;
}

// Whether a key toggles the display style (the window's key events ask).
int Display_StyleKeyMatch(OpenBGI_t* o, uint32_t vk)
{
	const uint32_t* k;
	if(!o->display.styleKeysOn)
		return 0;
	for(k = o->display.styleKeys; *k; k++)
		if(*k == vk)
			return 1;
	return 0;
}

/* Toggle windowed / full screen (a style key or Alt+Enter): not while the
 * style keys are off or a movie plays.  Posts event 1 with the new state
 * to the script event queue; 1 when toggled. */
int Display_ToggleStyle(OpenBGI_t* o)
{
	if(!o->display.styleKeysOn || o->movie.moviePlaying)
		return 0;
	SetDisplayMode(o, o->display.sizeIndex, o->display.pixelMode, !o->display.fullscreen);
	MsgQueue_Post(o, 1, (uint32_t)o->display.fullscreen, 0);
	return 1;
}

// The centred window position of the current size; 0, 0 in full screen ("80 80" of 1.69/472, "B0 02").
void Display_DefaultWindowPos(OpenBGI_t* o, int* x, int* y)
{
	int cw, ch;
	if(o->display.fullscreen)
	{
		*x = *y = 0;
		return;
	}
	Display_ClientSize(o, o->display.screenWidths[o->display.sizeIndex], o->display.screenHeights[o->display.sizeIndex], &cw, &ch);
	Display_CentredWindowPos(o, cw, ch, x, y);
}

// Re-centre the window after the screen's resolution changed (not in full screen or mid-switch).
void Display_OnDisplayChange(OpenBGI_t* o)
{
	int x, y;
	if(o->display.inSetDisplayMode || o->display.fullscreen)
		return;
	MainWin_ScreenSize(o, &o->display.screenW, &o->display.screenH);
	Display_DefaultWindowPos(o, &x, &y);
	MainWin_Move(o, x, y);
}

/* "80 6E" of 1.69 build 472 on: wait for the vertical blank before the
 * flip.  The host presents here, so the flag is only stored. */
void Display_SetVsyncWait(OpenBGI_t* o, int on)
{
	o->display.vsyncWait = on;
}

/* "81 63" of 1.529 on: how the picture fills the screen in full-screen
 * mode - 0 stretched, 1 keeping the aspect ratio, 2 unscaled and centred.
 * Stored only: this implementation presents as 1.69 does. */
// 1 when the mode is 0 .. 2 and was stored, 0 otherwise
int Display_SetFullscreenFit(OpenBGI_t* o, int mode)
{
	if((unsigned)mode >= 3)
		return 0;
	o->display.fullscreenFit = mode;
	return 1;
}

/* "81 6C" of 1.669 on: a picture size the renderer of that build scales
 * its output to; accepted (1) when its aspect is the desktop's (the two
 * ratios within 0.001 of each other), else 0 and nothing changes.  A
 * non-positive size means the desktop's.  Stored only: the output here
 * is the game's own picture size. */
int Display_SetRenderSize(OpenBGI_t* o, int w, int h)
{
	int sw, sh;
	float d;
	MainWin_ScreenSize(o, &sw, &sh);
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
	o->display.renderW = w;
	o->display.renderH = h;
	return 1;
}

/* "81 64" of 1.529 on: the client size of the window, which those builds
 * stretch the picture to; 0, 0 restores the picture size.  A window that
 * is up is resized at once, where it stands (the scripts centre it
 * afterwards when they want that), and repainted. */
void Display_SetWindowSize(OpenBGI_t* o, int w, int h)
{
	o->display.windowSizeW = w;
	o->display.windowSizeH = h;
	if(o->engine.profile->gen >= GEN_1_529 && o->display.windowExists && !o->display.fullscreen && !o->display.inSetDisplayMode)
	{
		int x, y, ow, oh, cw, ch;
		int picW = o->display.screenWidths[o->display.sizeIndex], picH = o->display.screenHeights[o->display.sizeIndex];
		Display_ClientSize(o, picW, picH, &cw, &ch);
		MainWin_GetRect(o, &x, &y, &ow, &oh);
		MainWin_SetWindowed(o, x, y, cw, ch);
		MainWin_SetPicture(o, picW, picH);
		Present_RequestFull(o);
	}
}

/* The pixel shader of 1.616 on.  The original's Direct3D presentation
 * stretches the picture into the window with the bilinear filter; its
 * one shader effect is a bicubic scaler, which a script loads ("81 6F")
 * when the player chose that in the game's settings, after asking
 * whether shaders are there ("81 6D") and whether the library that
 * compiles them is ("81 6E").  Here the host scales, by the rule it is
 * told: both answers are yes when it takes one (MainWin_CanScale), and
 * without that the shader cannot be loaded, only released. */
int Display_ShaderSupport(OpenBGI_t* o)
{
	return MainWin_CanScale(o);
}

int Display_SetShader(OpenBGI_t* o, int on)
{
	if(on && !MainWin_CanScale(o))
		return 0;
	o->display.bicubic = on != 0;
	MainWin_SetScaling(o, on ? OPENBGI_SCALE_BICUBIC : OPENBGI_SCALE_BILINEAR);
	return 1;
}

/* "81 66" of 1.669 on: whether the window has a sizing border
 * (WS_THICKFRAME in the original, which picks the window's style by the
 * flag).  The host's window gets one; what the player makes of it comes
 * back as the window's size (OpenBGI_WindowSize), which "81 67" reports. */
void Display_SetResizable(OpenBGI_t* o, int on)
{
	o->ops.resizableWindow = on != 0;
	MainWin_SetResizable(o, on != 0);
}

// "81 67" (1.669 on): the stored window size, or the picture size while none is set
void Display_GetWindowSize(OpenBGI_t* o, int* w, int* h)
{
	if(o->display.windowSizeW > 0 && o->display.windowSizeH > 0)
	{
		*w = o->display.windowSizeW;
		*h = o->display.windowSizeH;
	}
	else
		Display_GetPictureSize(o, w, h);
}

/* "81 0E" of 1.529 on: the desktop size.  With `adjust` a desktop wider
 * than 2.5 times its height (two monitors side by side) reports half the
 * width, and one between 1 and 1.25 times as high as wide half the
 * height. */
void Display_GetDesktopSize(OpenBGI_t* o, int out[2], int adjust)
{
	int w, h;
	MainWin_ScreenSize(o, &w, &h);
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

/* The pointer in back-buffer coordinates: where the host reported it
 * (OpenBGI_Pointer, in the pixels of the frame's picture), through the
 * display scaling. */
void GetCursorBufferPos(OpenBGI_t* o, int xy[2])
{
	int32_t p[2] = {o->input.pointerX, o->input.pointerY};
	if(o->gfx.gfx)
		Gfx_MapPoint(o->gfx.gfx, p, o->input.pointerX, o->input.pointerY, 1);
	xy[0] = p[0];
	xy[1] = p[1];
}

/* Move the pointer to a back-buffer position (the original's
 * SetCursorPos): the engine counts it there at once, and the frame tells
 * the host to move the system's pointer (OpenBGI_Frame_t.pointerMoved). */
void SetCursorBufferPos(OpenBGI_t* o, int x, int y)
{
	int32_t s[2] = {x, y};
	if(o->gfx.gfx)
		Gfx_MapPoint(o->gfx.gfx, s, x, y, 0);
	o->input.pointerX = s[0];
	o->input.pointerY = s[1];
	o->input.pointerMoved = 1;
}

// the picture size of the current size index
void Display_GetPictureSize(OpenBGI_t* o, int* w, int* h)
{
	*w = o->display.screenWidths[o->display.sizeIndex];
	*h = o->display.screenHeights[o->display.sizeIndex];
}
