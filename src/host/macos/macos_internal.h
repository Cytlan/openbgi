/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * macos_internal.h - what the files of the macOS back end share (the .m
 *                    files of src/host/macos/; docs/dev/os_layer.md,
 *                    "macOS")
 *
 * The back end is the POSIX one's system files (src/host/posix/: files,
 * time, threads, fonts, the sound through an AudioQueue) with these in the
 * place of its X11 files: the application and its events (app.m), the main
 * window (window.m), its picture through Metal (present.m), the boxes and
 * the pickers (dialogs.m) and the debugger's window (dbgwin.m).  They are
 * Objective-C because AppKit and Metal have no C interface; built for
 * PLATFORM=macos alone.
 *
 * Lengths and positions are points, as AppKit counts them, with the origin
 * at the top left as the engine expects: a display that draws two pixels a
 * point shows the picture at twice its size in pixels, scaled by the
 * graphics processor.
 */
#ifndef BGI_OS_MACOS_INTERNAL_H
#define BGI_OS_MACOS_INTERNAL_H

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "host/os.h"
#include "host/os_common.h"
#include "host/os_posix.h"
#include "bgi/sjis.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- app.m ---------------------------------------------------------------------------------------

extern OsHostHandlers_t gMacIn; // OS_SetHostHandlers

// the application object, its menu and its delegate, once; on the main thread
void Mac_AppInit(void);

/* What the windows report outside the pump (AppKit tells a delegate from
 * within its own calls) waits in a queue, and the pump raises it: the
 * host's handlers are never called from within one of the engine's own
 * calls. */
enum
{
	MAC_EV_FOCUS,       // a: active
	MAC_EV_MINIMIZED,   // a: minimised
	MAC_EV_RESIZED,     // a, b: the client area
	MAC_EV_CLOSE,       // the close box, Quit
	MAC_EV_PAINT,       // all of the picture is to be handed over again
	MAC_EV_DISPLAY,     // the screens changed
	MAC_EV_DROP,        // path: a file dropped on the window, or opened with the application
	MAC_EV_KEY,         // a: the virtual key, b: down, c: OPENBGI_KEY_*
	MAC_EV_DEBUG_CLOSE, // the debugger's window's close box
};
void Mac_Post(int type, int a, int b, int c, const char* path);

// text in the engine's encoding (os.h, "Conventions") as a string, and back into `out` (n bytes)
NSString* Mac_Text(const char* text);
void Mac_FromString(NSString* s, char* out, size_t n);

// ---- window.m ------------------------------------------------------------------------------------

// the picture again when it is waiting to be shown (the pump calls it, and the window's own events)
void Mac_PresentIfDirty(void);
// the virtual key of a key event (0: none), and OPENBGI_KEY_SYSTEM where Windows would say so
int Mac_EventVk(NSEvent* e, int* flags);
// the main window, for a box to be shown over it; nil when there is none
NSWindow* Mac_MainWindow(void);
// the screens changed: their sizes are asked for again
void Mac_ScreensChanged(void);

// ---- present.m -----------------------------------------------------------------------------------

#define MAC_FILTER_NEAREST  0
#define MAC_FILTER_BILINEAR 1
#define MAC_FILTER_BICUBIC  2

// the layer the picture is drawn into, set up for the device; 0 when this Mac has no Metal device
int MacPresent_Attach(CAMetalLayer* layer);
void MacPresent_Detach(void);
// w x h pixels of the surface at (srcX, srcY) into the picture at (dstX, dstY); 0 when nothing was taken
int MacPresent_Upload(const OsSurface_t* s, int dstX, int dstY, int w, int h, int srcX, int srcY);
void MacPresent_Clear(void);              // the picture black
int MacPresent_Size(int* w, int* h);      // the picture's size; 0 while there is none
// the layer black with the picture over the rectangle (the layer's pixels), scaled by the filter
void MacPresent_Draw(double x, double y, double w, double h, int filter);

#endif // BGI_OS_MACOS_INTERNAL_H
