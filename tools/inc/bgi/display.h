/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * display.h - the main window, the display mode and the presenting
 *
 * The engine renders the display objects into a back buffer of the
 * picture size and copies it (or its dirty rectangles) to the window once
 * per frame interval.  Five files share this header:
 *
 *   src/sys/window.c   the window itself: creation, the engine's view of
 *                      its state, the events the OS layer reports for it
 *   src/sys/display.c  the display mode: the picture sizes, windowed /
 *                      full screen, the window's place on the screen
 *   src/sys/present.c  when and how the back buffer reaches the window
 *   src/sys/winmsg.c   the scripts' waits on window messages, dropped files
 *   src/sys/child.c    the child windows of "B0 1x"
 */
#ifndef BGI_DISPLAY_H_
#define BGI_DISPLAY_H_

#include "bgi/common.h"

// ---- the state --------------------------------------------------------------------------
extern int gFullscreen;        // the current mode is full screen
extern int gSizeIndex;         // the picture size: an index into the size table
extern int gPixelMode;         // 0 = 16-bit, 1 = 32-bit pixels
extern int gWindowActive;      // the window has the focus
extern int gMinimizedByScript; // "80 65" minimised the window; restoring it resumes the sound
extern int gCloseMode;         // "80 68": 1 closing destroys the window, 0 posts event 2 and lets the script decide
extern int gWindowRestored;    // the window is not minimised
extern int gDisplayEnabled;    // "90 01": nothing is presented while 0
extern int gWindowAlive;       // the window exists (between its creation and its destruction)

/* The picture sizes by size index: the generation's defaults
 * (Display_InitSizeTable), and from 1.494 on whatever "81 60" set. */
extern int gScreenWidths[8];
extern int gScreenHeights[8];
void Display_InitSizeTable(void);
int Display_SetSizeEntry(int idx, int w, int h);          // "81 60": 0 ok, 1 bad index, 2 bad size
static inline void Display_GetPictureSize(int* w, int* h) // of the current size index
{
	*w = gScreenWidths[gSizeIndex];
	*h = gScreenHeights[gSizeIndex];
}

// ---- the window (window.c) ----------------------------------------------------------------
int MainWindow_Create(void);            // 0 on failure (an error box was shown)
void ShowMainWindow(int show);          // "80 64": show or hide
void Window_Minimize(void);             // minimise ("80 65")
static inline int Window_IsActive(void) // "80 0F"
{
	return gWindowActive;
}

static inline int Window_IsRestored(void)
{
	return gWindowRestored;
}

static inline int Window_MinimizedByScript(void) // "80 0E"
{
	return gMinimizedByScript;
}

static inline void Window_SetMinimizedByScript(int v)
{
	gMinimizedByScript = v;
}

static inline void Set_CloseMode(int v) // "80 68"
{
	gCloseMode = v;
}

// process one pending window message; 1 while there was one; throws 0 on a quit
int PumpMessages(void);
// a click of a mouse button at the cursor ("81 1E"): kind 1 left, 2 right, 4 middle, 5 / 6 the X buttons; 1 when sent
int Window_SimulateClick(int kind);

// ---- window messages for the scripts (winmsg.c) -------------------------------------------
// dropped files ("80 6C" / "80 6D")
void EnableDragDrop(int on);            // "80 6C"
int GetDroppedFile(char* buf);          // "80 6D": 1 and the path when one was dropped
void DragDrop_OnFile(const char* path); // a file was dropped (from the window's events)

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
void WinMsgWait_Register(struct Thread* t, uint32_t msg);
void WinMsgWait_Unregister(struct Thread* t, uint32_t msg);
int WinMsgWait_Poll(WinMsgRec_t* out, struct Thread* t, uint32_t msg); // copies the record and clears `arrived`; 1 when subscribed
void WinMsgWait_Dispatch(uint32_t msg, uint32_t wParam, uint32_t lParam);
void WinMsgWait_Clear(void);

// ---- the display mode (display.c) -------------------------------------------------------
// "80 60": windowed or full screen at a picture size and pixel mode
void SetDisplayMode(int sizeIdx, int pixelMode, int fullscreen);
// "80 6F": whether the screen can show the next larger mode (the letter-boxed full screen of wide displays)
int CanFullscreen(int sizeIdx);
void Display_RestoreMode(void); // back to the desktop's display mode
void Display_SetPosMode(int v); // "80 63": letter-box the full screen on wide displays
int Display_GetPosMode(void);
void Display_MeasureScreen(void);                            // the screen's size and aspect, before the window is made
void Display_CentredWindowPos(int w, int h, int* x, int* y); // where a w x h window sits centred
void Display_DefaultWindowPos(int* x, int* y);               // the centred position of the current size; 0, 0 in full screen
void Display_OnDisplayChange(void);                          // the screen's resolution changed: re-centre
void Display_SetVsyncWait(int on);                           // "80 6E" of 1.69/472 on (stored only)
int Display_GetVsyncWait(void);
/* "80 62": the keys that toggle windowed / full screen (at most 15,
 * 0-terminated); 0 when the list is too long */
int SetStyleChangeKeys(int enable, const uint32_t* keys);
int Display_StyleKeyMatch(uint32_t vk); // whether a key toggles the display style
int Display_ToggleStyle(void);          // toggle it; 1 when toggled
void Display_Enable(int on);            // "90 01"
void GetCursorBufferPos(int xy[2]);     // the cursor in back-buffer coordinates
void SetCursorBufferPos(int x, int y);  // move the cursor to a back-buffer position
int Display_SetFullscreenFit(int mode); // "81 63": 1 when 0 .. 2 (stored only)
int Display_GetFullscreenFit(void);
int Display_SetRenderSize(int w, int h);             // "81 6C" (1.669 on): 1 when the aspect is the desktop's (stored only)
void Display_SetWindowSize(int w, int h);            // "81 64" (stored only)
void Display_GetWindowSize(int* w, int* h);          // "81 67": the stored size, else the picture size
void Display_GetDesktopSize(int out[2], int adjust); // "81 0E"

// ---- presenting (present.c) ---------------------------------------------------------------
void Present_SetRate(int fps); // "90 02": the frame interval becomes 1000 / fps ms
int Present_GetInterval(void);
void Present_Allow(int on); // suspend / allow presenting
int Present_IsAllowed(void);
void Present_RequestFull(void);  // the whole picture at the next PresentFrame
void Present_RequestDirty(void); // the dirty rectangles at the next PresentFrame
void Present_Full(void);         // render everything and copy it now
void Present_Dirty(void);        // render and copy the dirty rectangles now
int PresentFrame(void);          // once per pass; -1 nothing presented, else 0
/* "80 52" (1.535 on): the sleep of a pass that presented nothing, in ms
 * (0: the half-millisecond idle wait) */
void Display_SetIdleSleep(int ms);
int Display_GetIdleSleep(void);
void Display_FrameIdle(int presented); // the sleep after a pass (presented: PresentFrame's result)
// the render-time profiler of 1.535 on ("80 06" / "80 07")
void Profile_Enable(int on);
int32_t Profile_Query(int mode);         // 0 passes, 1 mean ms, 2 budget ratio %, 3 squared slow-pass ratio %
void Window_Repaint(void);               // copy the whole back buffer to the window ("90 F1")
void Window_PresentOffset(int x, int y); // the screen quake's shifted present
void Present_EditPaint(void);            // re-present under the text entry control before it paints
uint32_t Sys_GetGfxProp(void);           // "80 0B": the compositor's band size
struct Bmp;
// blit a bitmap to a device context at (x, y) through the display scaling
void Window_DcBlit(void* dc, int x, int y, const struct Bmp* b);
int Window_BlitBitmap(int x, int y, int bmp); // "B0 00": 0 ok, 1 no window, 2 no bitmap

// ---- child windows ("B0 10" .. "B0 1C"; child.c) ----------------------------------------
void Child_InitTable(void);
void Child_DestroyAll(void);
/* "B0 10": a hidden pop-up window with a w x h client area at the screen
 * position (x, y); *out receives the handle 0xff000000 | slot.  0 ok,
 * 0x80000001 size outside 0x20 .. 0x400, 0x80000002 all 8 slots in use */
uint32_t Child_Create(uint32_t* out, const char* title, int x, int y, int w, int h);
// the rest return 1 when the handle names a slot, 0 otherwise (they do not check that it is in use)
int Child_Destroy(uint32_t handle);                       // "B0 11": the window closes; its buffer is freed when it is gone
int Child_Show(uint32_t handle, int show);                // "B0 14"
int Child_Move(uint32_t handle, int x, int y);            // "B0 16": screen coordinates of the outer rectangle
int Child_GetPos(int32_t out[2], uint32_t handle);        // "B0 17": ... as the window manager reports it
int Child_SetTitle(uint32_t handle, const char* title);   // "B0 15"
int Child_SetCopyText(uint32_t handle, const char* text); // "B0 1C" of 1.69/472 on
int Child_Fill(uint32_t handle, uint32_t colour);         // "B0 18": fill the back buffer and present it
/* "B0 19": blit a managed bitmap into the back buffer, then present.  0 ok,
 * -1 bad handle, 0x80000003 no bitmap, 4 incompatible pixel modes, 5 unknown
 * effect, 6 level out of range, 7 nothing inside the client, 8 any other
 * blit result (the 4 .. 8 as 0x8000000n) */
uint32_t Child_DrawBitmap(uint32_t handle, int x, int y, int bmp, int effect, int level);
/* "B0 1A": text with the bitmap manager's font cache, then present.  0 ok,
 * -1 bad handle, 0x80000009 bad size, 0x8000000a bad width, 0x8000000b
 * unknown font number; *measure receives the width of the widest line */
uint32_t Child_DrawText(uint32_t handle, int x, int y, const char* str, int fontNo, int size, int widthPct,
	int bold, int proportional, uint32_t colour, int32_t* measure);
// the window procedure's pieces, called from the OS layer's events
void Child_OnPaint(int index, void* dc);
void Child_OnDestroyed(int index);

#endif // BGI_DISPLAY_H_
