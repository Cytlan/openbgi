/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * display.h - the main window, the display mode and the presenting
 *
 * The engine renders the display objects into a back buffer of the
 * picture size and copies it (or its dirty rectangles) to the window's
 * picture once per frame interval; the host shows that picture.  Seven
 * files share this header:
 *
 *   src/sys/window.c   the window itself: creation, the engine's view of
 *                      its state, the input and events the host hands over
 *   src/sys/mainwin.c  the calls of the host's window, or the window
 *                      nobody sees
 *   src/sys/display.c  the display mode: the picture sizes, windowed /
 *                      full screen, the window's place on the screen
 *   src/sys/present.c  when and how the back buffer reaches the window
 *   src/sys/screen.c   the window's picture, as the host gets it
 *   src/sys/winmsg.c   the scripts' waits on window messages, dropped files
 *   src/sys/child.c    the child windows of "B0 1x"
 */
#ifndef BGI_DISPLAY_H_
#define BGI_DISPLAY_H_

#include "bgi/common.h"
#include "openbgi.h"        // OpenBGI_Frame_t
#include "bgi/gfx/bitmap.h" // Bmp_t: the child windows' pictures

/* The picture sizes by size index: the generation's defaults
 * (Display_InitSizeTable), and from 1.492 on whatever "81 60" set. */
void Display_InitSizeTable(OpenBGI_t* o);
int Display_SetSizeEntry(OpenBGI_t* o, int idx, int w, int h); // "81 60": 0 ok, 1 bad index, 2 bad size
void Display_GetPictureSize(OpenBGI_t* o, int* w, int* h);     // of the current size index

// ---- the window (window.c) ----------------------------------------------------------------
int MainWindow_Create(OpenBGI_t* o);                   // 0 on failure (an error box was shown)
void MainWindow_Destroy(OpenBGI_t* o);                 // DestroyWindow: the window and the run end (WM_DESTROY posts the quit)
void MainWindow_PostClose(OpenBGI_t* o);               // PostMessage(WM_CLOSE): the next pump asks as the close box does
void ShowMainWindow(OpenBGI_t* o, int show);           // "80 64": show or hide
void Window_Minimize(OpenBGI_t* o);                    // minimise ("80 65")
int Window_IsActive(OpenBGI_t* o);                     // "80 0F"
void Window_ChildFocus(OpenBGI_t* o, int child);       // a child window the engine draws became the active window (1), or none is (0)
int Window_IsRestored(OpenBGI_t* o);                   // not minimised
int Window_MinimizedByScript(OpenBGI_t* o);            // "80 0E"
void Window_SetMinimizedByScript(OpenBGI_t* o, int v); // "80 65" minimised the window
void Set_CloseMode(OpenBGI_t* o, int v);               // "80 68"

// a click of a mouse button at the cursor ("81 1E"): kind 1 left, 2 right, 4 middle, 5 / 6 the X buttons; 1 when sent
int Window_SimulateClick(OpenBGI_t* o, int kind);
/* the state of the keys and the pointer as the host reported it
 * (OpenBGI_Key, OpenBGI_Button, OpenBGI_Pointer, openbgi.h) */
int Input_KeyDown(OpenBGI_t* o, int vk);                  // key or button vk is down (GetAsyncKeyState's bit 15)
void Input_KeyboardState(OpenBGI_t* o, uint8_t out[256]); // as GetKeyboardState ("81 11"): bit 7 down, bit 0 toggled
void Pointer_TakeFrame(OpenBGI_t* o, OpenBGI_Frame_t* f); // where the engine moved the pointer since the last frame, into *f

// ---- the host's window or the one nobody sees (mainwin.c; openbgi.h, "The main window") ---
int MainWin_Create(OpenBGI_t* o, const char* title, int x, int y, int w, int h); // hidden; 1 ok
void MainWin_Destroy(OpenBGI_t* o);
void MainWin_Show(OpenBGI_t* o, int show);
void MainWin_SetTitle(OpenBGI_t* o, const char* title);
void MainWin_Move(OpenBGI_t* o, int x, int y);                      // the outer origin
void MainWin_GetRect(OpenBGI_t* o, int* x, int* y, int* w, int* h); // the outer rectangle
int MainWin_Minimized(OpenBGI_t* o);
void MainWin_Minimize(OpenBGI_t* o);
void MainWin_ScreenSize(OpenBGI_t* o, int* w, int* h);
void MainWin_FrameMetrics(OpenBGI_t* o, int* fw, int* fh); // what the frame adds to the client area
int MainWin_FrameTop(OpenBGI_t* o);                        // the caption and the top border
int MainWin_SetMode(OpenBGI_t* o, int w, int h, int bpp);  // a display mode for the full screen; 1 ok
void MainWin_RestoreMode(OpenBGI_t* o);
void MainWin_SetFullscreen(OpenBGI_t* o, int w, int h);
void MainWin_SetWindowed(OpenBGI_t* o, int x, int y, int w, int h);
void MainWin_SetPicture(OpenBGI_t* o, int w, int h);
void MainWin_CursorShape(OpenBGI_t* o, int shape);
void MainWin_CursorShow(OpenBGI_t* o, int show);
void MainWin_ImeStatus(OpenBGI_t* o, int open);
void MainWin_DragAccept(OpenBGI_t* o, int accept);
void MainWin_SetResizable(OpenBGI_t* o, int on);
int MainWin_CanScale(OpenBGI_t* o);               // the host scales by the rule it is told (OpenBGI_Window_t.setScaling)
void MainWin_SetScaling(OpenBGI_t* o, int rule);  // OPENBGI_SCALE_*
int MainWin_TakeFocus(OpenBGI_t* o, int* active); // the window nobody sees: the next focus change for the pump; 0 when none waits

/* The view: the window's real size on a host that says its screen's scale
 * and room (OpenBGI_Window_t.workArea, openbgi.h "The window's size on the
 * screen").  The calls above translate: what the scripts ask for and are
 * told is in the picture's pixels on a desktop smaller by the scale. */
#define VIEW_SIZE_AUTO       0 // the screen's scale
#define VIEW_SIZE_100        1 // a pixel a pixel
#define VIEW_SIZE_150        2
#define VIEW_SIZE_200        3
#define VIEW_SIZE_FIT        4 // as large as the screen has room for
#define VIEW_FILTER_GAME     0 // the rule the game asks for (bilinear until it asks)
#define VIEW_FILTER_BICUBIC  1
#define VIEW_FILTER_BILINEAR 2
#define VIEW_FILTER_NEAREST  3
#define VIEW_FILTER_OWN      4 // .. and on: the host's own filters, number n at VIEW_FILTER_OWN + n
int MainWin_ViewOn(OpenBGI_t* o);
int MainWin_ViewScales(OpenBGI_t* o);   // the settings' filter and steps hold: a fitted window, or a surface the picture is laid on
void MainWin_ViewRescale(OpenBGI_t* o); // the host's scaling rule again (the surface came or went)
int MainWin_FrameLimits(OpenBGI_t* o);  // the host can hold its frames to the screen's refresh rate (OpenBGI_Window_t.setFrameLimit)
void MainWin_SetFrameLimit(OpenBGI_t* o, int on); // tell it to, or not to; nothing where it cannot
// the player gave the window a client area of w x h: 1 when the scripts are to hear of it, with *w, *h as they count
int MainWin_UserSized(OpenBGI_t* o, int* w, int* h);
// the screen, its scale or room, or the window's frame changed: the window is fitted again
void MainWin_ViewRefresh(OpenBGI_t* o);
// the settings: as they are, changed (the filter and the steps are kept with the game), the player's own size dropped
void MainWin_ViewSettings(OpenBGI_t* o, int* size, int* filter, int* whole);
void MainWin_ViewApply(OpenBGI_t* o, int size, int filter, int whole);
/* The host's own filters (OpenBGI_Window_t.filterName): the name of
 * number n, NULL for none - the settings' filter VIEW_FILTER_OWN + n -
 * and which kind of them the player wants (0 the fast, 1 the better). */
const char* MainWin_ViewFilterName(OpenBGI_t* o, int n);
int MainWin_ViewHigh(OpenBGI_t* o);
void MainWin_ViewSetHigh(OpenBGI_t* o, int high);
void MainWin_ViewReset(OpenBGI_t* o);
// the debugger's window (OpenBGI_DebugWindow_t): 0 from open without one
int MainWin_DebugOpen(OpenBGI_t* o, int w, int h, const char* title);
void MainWin_DebugClose(OpenBGI_t* o);
void MainWin_DebugPresent(OpenBGI_t* o, const uint8_t* pixels, int pitch, int w, int h);

// ---- window messages for the scripts (winmsg.c) -------------------------------------------
// dropped files ("80 6C" / "80 6D")
void EnableDragDrop(OpenBGI_t* o, int on);            // "80 6C"
int GetDroppedFile(OpenBGI_t* o, char* buf);          // "80 6D": 1 and the path when one was dropped
void DragDrop_OnFile(OpenBGI_t* o, const char* path); // a file was dropped (from the window's events)

/* The waits on a window message ("80 54"): a thread subscribes to a
 * message number and the window's events record the parameters of the
 * next message with that number. */
struct Thread;
typedef struct WinMsgRec
{
	struct Thread* thread;   // the subscribed thread
	uint32_t msg;            // the message number
	uint32_t wParam, lParam; // the parameters of the last arrival
	int32_t arrived;         // a message came since the last poll
	struct WinMsgRec* next;  // the next subscription (NULL in a polled copy)
} WinMsgRec_t;
void WinMsgWait_Register(OpenBGI_t* o, struct Thread* t, uint32_t msg);
void WinMsgWait_Unregister(OpenBGI_t* o, struct Thread* t, uint32_t msg);
int WinMsgWait_Poll(OpenBGI_t* o, WinMsgRec_t* out, struct Thread* t, uint32_t msg); // copies the record and clears `arrived`; 1 when subscribed
void WinMsgWait_Dispatch(OpenBGI_t* o, uint32_t msg, uint32_t wParam, uint32_t lParam);
void WinMsgWait_Clear(OpenBGI_t* o);

// ---- the display mode (display.c) -------------------------------------------------------
// "80 60": windowed or full screen at a picture size and pixel mode
void SetDisplayMode(OpenBGI_t* o, int sizeIdx, int pixelMode, int fullscreen);
// "80 6F": whether the screen can show the next larger mode (the letter-boxed full screen of wide displays)
int CanFullscreen(OpenBGI_t* o, int sizeIdx);
void Display_RestoreMode(OpenBGI_t* o);                                    // back to the desktop's display mode
void Display_SetPosMode(OpenBGI_t* o, int v);                              // "80 63": letter-box the full screen on wide displays
void Display_MeasureScreen(OpenBGI_t* o);                                  // the screen's size and aspect, before the window is made
void Display_ClientOrigin(OpenBGI_t* o, int* x, int* y);                   // where the main window's client area lies on the screen
void Display_CentredWindowPos(OpenBGI_t* o, int w, int h, int* x, int* y); // where a w x h window sits centred
void Display_DefaultWindowPos(OpenBGI_t* o, int* x, int* y);               // the centred position of the current size; 0, 0 in full screen
void Display_OnDisplayChange(OpenBGI_t* o);                                // the screen's resolution changed: re-centre
void Display_SetVsyncWait(OpenBGI_t* o, int on);                           // "80 6E" of 1.69/472 on (stored only)
/* "80 62": the keys that toggle windowed / full screen (at most 15,
 * 0-terminated); 0 when the list is too long */
int SetStyleChangeKeys(OpenBGI_t* o, int enable, const uint32_t* keys);
int Display_StyleKeyMatch(OpenBGI_t* o, uint32_t vk);              // whether a key toggles the display style
int Display_ToggleStyle(OpenBGI_t* o);                             // toggle it; 1 when toggled
void Display_Enable(OpenBGI_t* o, int on);                         // "90 01"
void GetCursorBufferPos(OpenBGI_t* o, int xy[2]);                  // the pointer in back-buffer coordinates
void SetCursorBufferPos(OpenBGI_t* o, int x, int y);               // move the pointer to a back-buffer position (the host moves the system's after the frame)
int Display_SetFullscreenFit(OpenBGI_t* o, int mode);              // "81 63": 1 when 0 .. 2 (stored only)
int Display_SetRenderSize(OpenBGI_t* o, int w, int h);             // "81 6C" (1.669 on): 1 when the aspect is the desktop's (stored only)
void Display_SetWindowSize(OpenBGI_t* o, int w, int h);            // "81 64" (stored only)
void Display_GetWindowSize(OpenBGI_t* o, int* w, int* h);          // "81 67": the stored size (the player's, of a window with a sizing border), else the picture size
void Display_SetResizable(OpenBGI_t* o, int on);                   // "81 66": the window gets a sizing border, or loses it
int Display_ShaderSupport(OpenBGI_t* o);                           // "81 6D" / "81 6E": the bicubic scaler can be had
int Display_SetShader(OpenBGI_t* o, int on);                       // "81 6F": scale bicubically (on) or bilinearly; 1 when that is so now
void Display_GetDesktopSize(OpenBGI_t* o, int out[2], int adjust); // "81 0E"

// ---- presenting (present.c) ---------------------------------------------------------------
void Present_SetRate(OpenBGI_t* o, int fps); // "90 02": the frame interval becomes 1000 / fps ms
int Present_GetInterval(OpenBGI_t* o);
void Present_Allow(OpenBGI_t* o, int on); // suspend / allow presenting
int Present_IsAllowed(OpenBGI_t* o);
void Present_RequestFull(OpenBGI_t* o);        // the whole picture at the next PresentFrame
void Present_RequestDirty(OpenBGI_t* o);       // the dirty rectangles at the next PresentFrame
void Present_Full(OpenBGI_t* o);               // render everything and copy it now
void Present_Dirty(OpenBGI_t* o);              // render and copy the dirty rectangles now
int PresentFrame(OpenBGI_t* o);                // once per pass; -1 nothing presented, else 0
void Present_SetDrawing(OpenBGI_t* o, int on); // OpenBGI_SetDrawing: presents without the compositor while off
void Present_Freshen(OpenBGI_t* o);            // the back buffer painted now when painting is off and it is stale
/* "80 52" (1.535 on): the sleep of a pass that presented nothing, in ms
 * (0: the half-millisecond idle wait) */
void Display_SetIdleSleep(OpenBGI_t* o, int ms);
int Display_IdleMs(OpenBGI_t* o, int presented); // the host's wait after a frame: 0 the idle wait, else ms (presented: PresentFrame's result)
// the render-time profiler of 1.535 on ("80 06" / "80 07")
void Profile_Enable(OpenBGI_t* o, int on);
int32_t Profile_Query(OpenBGI_t* o, int mode);         // 0 passes, 1 mean ms, 2 budget ratio %, 3 squared slow-pass ratio %
void Window_Repaint(OpenBGI_t* o);                     // copy the whole back buffer to the window ("90 F1")
void Window_PresentOffset(OpenBGI_t* o, int x, int y); // the screen quake's shifted present
int Present_Expose(OpenBGI_t* o);                      // what an uncovered window shows now: OPENBGI_EXPOSE_* (openbgi.h)
uint32_t Sys_GetGfxProp(OpenBGI_t* o);                 // "80 0B": the compositor's band size
struct Bmp;
// a bitmap to the main window's picture at (x, y), through the display scaling
void Window_Blit(OpenBGI_t* o, int x, int y, const struct Bmp* b);
/* Where a bitmap blitted at (x, y) of a window lands through the display
 * scaling: (x, y) mapped, the height stretched, the output clipped to
 * the mapped picture (`clip` {l, t, r, b}, exclusive).  0 when the display
 * is not scaled and the bitmap lands as it is. */
int Window_MapBlit(OpenBGI_t* o, int* x, int* y, int* dstH, int clip[4]);
int Window_BlitBitmap(OpenBGI_t* o, int x, int y, int bmp); // "B0 00": 0 ok, 1 no window, 2 no bitmap

/* ---- the window's picture (screen.c) ----------------------------------------------------
 *
 * What the main window shows, in the back buffer's pixel mode and the
 * size of the window's picture (the back buffer's, or the scaled
 * surface's on a letter-boxed full screen); every frame hands it to the
 * host with the rectangles that changed (openbgi.h, OpenBGI_Frame_t).
 * Screen_Setup makes a new black one (all of it changed), Screen_Free
 * drops it - but the pixels a frame handed over stay readable for the
 * host until Screen_NextFrame, which the next frame begins with (and the
 * stop ends with).  Screen_Copy is BitBlt: w x h pixels of `src` from (sx, sy) to
 * (dx, dy), clipped to both.  Screen_Stretch is StretchDIBits of all of
 * `src` to dw x dh at (dx, dy): nearest-neighbour, or with `clip` ({left,
 * top, right, bottom}, right / bottom exclusive) clipped to it and
 * filtered bilinearly (HALFTONE).  The sources are RGB16, RGB32 or ARGB32
 * (its alpha ignored).  Screen_TakeFrame fills the frame's picture and
 * changes and starts the changes over.  Screen_StretchTo is
 * Screen_Stretch into another bitmap of 16 or 32 bits, Screen_ToRgb32 all
 * of a bitmap as 0x00RRGGBB pixels (`pitch` pixels a row).
 *
 * Screen_Overlay puts something over the picture in the frames without
 * changing it (the text entry, the child windows and the dialogs drawn in
 * the frame, the on-screen keyboard and a pad's pointer: src/sys/edit.c,
 * child.c, inframe.c, osk.c and pad.c): w x h pixels (`pitch` pixels a
 * row, kept by the caller until it removes them) at (x, y) of the picture,
 * in slot `slot` (the higher over the lower) - 0x00RRGGBB, or with
 * `alpha` 0xAARRGGBB composited over what is under them; `pix` NULL
 * removes the slot's.  Calling it again after the pixels changed shows
 * them anew.  While an overlay is up the frames hand over a copy of the
 * picture with the overlays drawn over it, kept up to date with the
 * picture under them. */
#define SCREEN_OVERLAYS        13
#define SCREEN_OVERLAY_EDIT    0  // the slots: the text entry, ..
#define SCREEN_OVERLAY_CHILD   1  // .. the eight child windows, the one on top last, ..
#define SCREEN_OVERLAY_DIALOG  9  // .. a dialog, and a complaint over it in the next, ..
#define SCREEN_OVERLAY_OSK     11 // .. the on-screen keyboard (osk.c), ..
#define SCREEN_OVERLAY_POINTER 12 // .. and the pointer the engine draws for a pad (pad.c)
void Screen_Setup(OpenBGI_t* o, int w, int h, int mode);
void Screen_Free(OpenBGI_t* o);
void Screen_NextFrame(OpenBGI_t* o);
void Screen_Copy(OpenBGI_t* o, int dx, int dy, const struct Bmp* src, int sx, int sy, int w, int h);
void Screen_Stretch(OpenBGI_t* o, int dx, int dy, int dw, int dh, const struct Bmp* src, const int* clip);
void Screen_TakeFrame(OpenBGI_t* o, OpenBGI_Frame_t* f);
void Screen_StretchTo(struct Bmp* dst, int dx, int dy, int dw, int dh, const struct Bmp* src, const int* clip);
void Screen_ToRgb32(uint32_t* dst, int pitch, const struct Bmp* src);
void Screen_Overlay(OpenBGI_t* o, int slot, const uint32_t* pix, int pitch, int x, int y, int w, int h, int alpha);

// ---- child windows ("B0 10" .. "B0 1C"; child.c) ----------------------------------------
void Child_InitTable(OpenBGI_t* o);
void Child_DestroyAll(OpenBGI_t* o); // every window closes (the shutdown, the main window's end)
/* "B0 10": a hidden pop-up window with a w x h client area at the screen
 * position (x, y); *out receives the handle 0xff000000 | slot.  0 ok,
 * 0x80000001 size outside 0x20 .. 0x400, 0x80000002 all 8 slots in use */
uint32_t Child_Create(OpenBGI_t* o, uint32_t* out, const char* title, int x, int y, int w, int h);
// the rest return 1 when the handle names a slot, 0 otherwise (they do not check that it is in use)
int Child_Destroy(OpenBGI_t* o, uint32_t handle);                       // "B0 11": the window closes and the slot is free
int Child_Show(OpenBGI_t* o, uint32_t handle, int show);                // "B0 14"
int Child_Move(OpenBGI_t* o, uint32_t handle, int x, int y);            // "B0 16": screen coordinates of the outer rectangle
int Child_GetPos(OpenBGI_t* o, int32_t out[2], uint32_t handle);        // "B0 17": ... as the window manager reports it
int Child_SetTitle(OpenBGI_t* o, uint32_t handle, const char* title);   // "B0 15"
int Child_SetCopyText(OpenBGI_t* o, uint32_t handle, const char* text); // "B0 1C" of 1.69/472 on
int Child_Fill(OpenBGI_t* o, uint32_t handle, uint32_t colour);         // "B0 18": fill the back buffer and present it
/* "B0 19": blit a managed bitmap into the back buffer, then present.  0 ok,
 * -1 bad handle, 0x80000003 no bitmap, 4 incompatible pixel modes, 5 unknown
 * effect, 6 level out of range, 7 nothing inside the client, 8 any other
 * blit result (the 4 .. 8 as 0x8000000n) */
uint32_t Child_DrawBitmap(OpenBGI_t* o, uint32_t handle, int x, int y, int bmp, int effect, int level);
/* "B0 1A": text with the bitmap manager's font cache, then present.  0 ok,
 * -1 bad handle, 0x80000009 bad size, 0x8000000a bad width, 0x8000000b
 * unknown font number; *measure receives the width of the widest line */
uint32_t Child_DrawText(OpenBGI_t* o, uint32_t handle, int x, int y, const char* str, int fontNo, int size, int widthPct,
	int bold, int proportional, uint32_t colour, int32_t* measure);
/* The windows drawn in the frame (without a host's): at the end of a
 * frame they are placed and drawn again where they changed.  The input
 * calls ask them before the text entry: Child_Button takes a press on a
 * window (it activates it and drags it by its caption) and that button's
 * release, Child_Pointer the moves while one is dragged or over one,
 * Child_Key Ctrl+C for the window clicked last.  1 when taken. */
void Child_Frame(OpenBGI_t* o, OpenBGI_Frame_t* f);
int Child_Button(OpenBGI_t* o, int button, int down, int x, int y);
int Child_Pointer(OpenBGI_t* o, int x, int y);
int Child_Key(OpenBGI_t* o, int vk, int down, int flags);
int Child_Over(OpenBGI_t* o, int x, int y); // a window drawn in the frame lies at (x, y); changes nothing

// a slot of the child window table (o->child.children; src/sys/child.c)
#define CHILD_SLOTS 8 // slots, and so windows at a time
typedef struct ChildWindow
{
	int used;       // the slot is taken, from Child_Create until Child_Destroy
	void* win;      // the host's window (OpenBGI_ChildWindows_t); NULL in the frame or when the host made none
	Bmp_t back;     // the client picture, screen pixel mode
	Bmp_t scaled;   // the client picture through the display scaling, while it is scaled (else no pixels)
	char* copyText; // "B0 1C" (1.69 build 472 on): what Ctrl+C in the window puts on the clipboard
	// in the frame
	char title[0x200];
	int shown;       // "B0 14"
	int x, y;        // the outer origin, relative to the main window's client area
	int z;           // the stacking order: the higher over the lower
	uint32_t* frame; // the window with its border and caption bar, 0x00RRGGBB
	int fw, fh;      // its size
} ChildWindow_t;

#endif // BGI_DISPLAY_H_
