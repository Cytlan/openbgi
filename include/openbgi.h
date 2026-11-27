/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * openbgi.h - the engine as a host drives it: the services a host provides,
 *          the calls that run a game and the questions the engine asks
 *          (src/core/openbgi.c, src/sys/engine.c, src/sys/request.c,
 *          src/sys/window.c, src/sys/mainwin.c, src/snd/sndout.c,
 *          src/gfx/typeface.c); not part of the original engine
 *
 * The host owns the loop.  It makes an engine with its services, starts
 * the game, and then asks for one frame at a time.  Between two frames it
 * answers what the engine asked, hands the player's input and the
 * window's events to the engine and waits as long as the frame asked,
 * while its sound device takes the engine's sound:
 *
 *     OpenBGI_t* o = OpenBGI_New(OS_Host());
 *     OpenBGI_Frame_t f = {.size = sizeof f};
 *     OpenBGI_Request_t q = {.size = sizeof q};
 *     if(OpenBGI_Start(o, cmdLine) == OPENBGI_RUNNING)
 *     {
 *         the sound device opened, its thread calling OpenBGI_AudioRead
 *         OpenBGI_SetAudioLead(o, the lead it needs);
 *         for(;;)
 *         {
 *             while(OpenBGI_NextRequest(o, &q))
 *                 OpenBGI_Answer(o, q.id, the answer to q);
 *             OpenBGI_Pointer(o, where the pointer is);
 *             if(OpenBGI_RunFrame(o, &f) != OPENBGI_RUNNING)
 *                 break;
 *             the changes of f.pixels to the window
 *             the system's pointer to f.pointerX, f.pointerY if f.pointerMoved
 *             the window's messages: OpenBGI_Key, OpenBGI_Button, OpenBGI_Close, ...
 *             OpenBGI_PumpEvents(o);
 *             the wait of f.idleMs (with OpenBGI_RunAudio every so often)
 *         }
 *     }
 *     exitCode = OpenBGI_Stop(o);
 *     while(OpenBGI_NextRequest(o, &q))
 *         OpenBGI_Answer(o, q.id, the answer to q);
 *     the sound device closed
 *     OpenBGI_Delete(o);
 *
 * A frame is one pass of the script scheduler (every thread has its turn)
 * and the services after it (particles, panels, input, the display); it
 * presents a picture at most once.  The engine never waits inside a
 * frame: the half-millisecond idle wait and the "80 52" sleep are the
 * host's, as is the message pump, and whatever the original waited for
 * in the middle of a pass - a message box, a dialog, a disc, another
 * program, the processor's clock measurement - is either a request the
 * host answers or a step the engine takes once per frame.  While one is
 * outstanding the engine holds still as the original did: no thread runs
 * and nothing is presented until it is over, and the pass then goes on
 * where it stopped.  A host may answer a request at once (a modal window)
 * or frames later (a window of a page that reports back); the frames in
 * between do nothing.
 *
 * Each frame hands the host the window's picture and what changed in it
 * (OpenBGI_Frame_t); showing it is the host's, right after the frame.  The
 * sound comes out as a stream of frames the host reads from any thread
 * (OpenBGI_AudioRead, below).  The player's input goes in through
 * OpenBGI_Key, OpenBGI_Pointer, OpenBGI_Button, OpenBGI_Wheel and
 * OpenBGI_Focus (below): the engine keeps the state of the keys and the
 * pointer from what it is told and asks nothing of the system.  The
 * window is the host's (OpenBGI_Window_t, "The main window", below): the
 * engine asks for what the original did with its window, and the host
 * pumps the window's messages and hands over what they say - the input,
 * the close box, a minimise, a dropped file, ... ("The window's events",
 * below) - before OpenBGI_PumpEvents.  It draws its text with fonts the
 * host finds for it, or with the host's own renderer ("The fonts",
 * below), and reaches every file, the drives and the registry through the
 * host's tables ("The files", below).  src/host/os_host.c is the host this
 * loop is written in, over the OS layer (host/os.h).
 *
 * The engine's clock is the host's: every timer, fade, wait and date the
 * engine reads comes from the services below.
 *
 * This header needs no other of the engine's: it and the library
 * (libopenbgi) are all a host is built with.  So that a host built with
 * an older copy of it goes on working with a newer library, every struct
 * that crosses between the two and may grow begins with `size`, the
 * struct's size as the host was compiled - `OpenBGI_Frame_t f = {.size =
 * sizeof f};` sets it and clears the rest - and the library reads and writes no
 * more than that of it: a table's functions past it count as NULL, a
 * frame's or a request's members past it are not filled in.  New members
 * only come at a struct's end.  The small records without one (a
 * rectangle, a date, a file's facts, ...) keep their shape.
 *
 * What a later version of the library adds may reach a host built with
 * this copy of the header: a flag in a set it knows (the rule of
 * OpenBGI_Window_t.setScaling, the style of a text entry, a target's
 * flags), a kind it has no name for (a request, a target, what a frame
 * says of `expose`).  A host ignores the flags it does not know.  It
 * answers a request of a kind it does not know - or does not serve -
 * with 0, which declines every request but a message box (and that one
 * every host knows); it leaves a target of a kind it does not know out;
 * and it takes an `expose` it does not know for OPENBGI_EXPOSE_PICTURE.
 */
#ifndef OPENBGI_H_
#define OPENBGI_H_

#include <stddef.h>
#include <stdint.h>

/* The version of this interface, major and minor.  A minor step only
 * adds (members at the end of a struct, functions, constants); a major
 * one may change what was there.  A host works with every library of its
 * major whose minor is not older than its own; OpenBGI_New refuses a
 * host of another major, or of a newer minor than the library's.
 * OpenBGI_ApiVersion is the library's. */
#define OPENBGI_API_MAJOR   1
#define OPENBGI_API_MINOR   1
#define OPENBGI_API_VERSION ((OPENBGI_API_MAJOR << 16) | OPENBGI_API_MINOR)

/* What marks the library's functions (OPENBGI_API): C linkage for a
 * host in C++, and whatever the build says with OPENBGI_EXPORT - nothing
 * as a rule; the shared library's own build marks them as what it lets
 * out (the Makefile's PIC_CFLAGS), and a host of the DLL may mark them
 * as imports (__declspec(dllimport)), which saves a jump each call. */
#ifndef OPENBGI_EXPORT
#define OPENBGI_EXPORT
#endif
#ifdef __cplusplus
#define OPENBGI_API extern "C" OPENBGI_EXPORT
#else
#define OPENBGI_API OPENBGI_EXPORT
#endif

// an engine: made by OpenBGI_New, its inside is the library's
#ifndef OPENBGI_T_DEFINED
#define OPENBGI_T_DEFINED
typedef struct OpenBGI OpenBGI_t;
#endif

OPENBGI_API uint32_t OpenBGI_ApiVersion(void);

// what OpenBGI_Start and OpenBGI_RunFrame return
#define OPENBGI_DONE    0 // nothing (more) to run: OpenBGI_Stop next
#define OPENBGI_RUNNING 1 // the game runs: answer the requests, call OpenBGI_RunFrame (again)

// a local date and time, as GetLocalTime gives it
typedef struct OpenBGI_DateTime
{
	uint16_t year, month, dayOfWeek, day, hour, minute, second, millis; // month 1 .. 12, dayOfWeek 0 = Sunday
} OpenBGI_DateTime_t;

/* The fonts.  The scripts ask for fonts as the original asks GDI for them
 * - a face name, a cell height and the width of a half-width character -
 * and the engine filters the glyphs it gets itself.  Unless the host has
 * a renderer of its own it wants used (OpenBGI_FontRenderer_t: GDI on
 * Windows, a page's canvas), the engine rasterises them itself with
 * FreeType, in the builds that have it, from three kinds of fonts: the
 * ones built into the binary, the game's own ("B0 C2", "B0 C3"), and the
 * system's, which the host finds for it (OpenBGI_FontFiles_t).  A host
 * with neither gets the first two.  Every member of the two tables may be
 * NULL; the engine calls them on its own thread with the host's `user`.
 *
 * Family names are UTF-8 here.  A name asks for the serif (Mincho) class
 * when it has "明朝", "mincho" or "serif" in it, for the sans (gothic) one
 * otherwise; the host may take that as a hint for a family it lacks. */

/* A font file the host found: a native path, which the engine opens and
 * reads as it needs through the host's files (nativeOpen, "The files"),
 * or (an empty path) data the host keeps for as long as the engine lives;
 * `index` is the face within a collection (.ttc). */
typedef struct OpenBGI_FontFile
{
	char path[1024];
	const void* data;
	uint32_t size;
	int index;
} OpenBGI_FontFile_t;

#define OPENBGI_FONT_FOUND   1 // find, findChar: a file (of the family, or the nearest the host has); family: installed
#define OPENBGI_FONT_FORCED  2 // find, family: the host puts a file of its choice in the family's place, before the engine's own fonts
#define OPENBGI_FONT_UNKNOWN 4 // family: the host cannot tell whether it is installed

typedef struct OpenBGI_FontFiles
{
	uint32_t size; // sizeof(OpenBGI_FontFiles_t) as the host was compiled
	// the file to open for a family, bold and italic as asked: 0 when the host has none, else OPENBGI_FONT_FOUND / _FORCED
	int (*find)(void* user, const char* family, int serif, int bold, int italic, OpenBGI_FontFile_t* out);
	// a file with a glyph for the code point `cp`, for a character the family lacks: 0 or OPENBGI_FONT_FOUND
	int (*findChar)(void* user, uint32_t cp, int serif, int bold, OpenBGI_FontFile_t* out);
	// what the host knows of a family of exactly that name ("B0 C4" asks): OPENBGI_FONT_* flags, 0 when it is not installed
	int (*family)(void* user, const char* family);
	/* the installed families that cover a language ("ja", "zh-cn", "zh-tw",
	 * "ko", or NULL for any), each handed to `each` once or more */
	void (*families)(void* user, const char* lang, void (*each)(void* ctx, const char* family), void* ctx);
	/* a font the engine was handed as data (built into the binary, or the
	 * game's own), for the host's own windows to find by its families too;
	 * the host copies what it keeps */
	void (*added)(void* user, const void* data, uint32_t size);
} OpenBGI_FontFiles_t;

/* A renderer of the host's own, used in place of the engine's with GDI's
 * contract: face names and characters in the scripts' text encoding
 * (Shift-JIS, or UTF-8 after "81 00"), paths as the engine spells them
 * (Shift-JIS, backslashes), fonts as opaque handles. */
typedef struct OpenBGI_FontRenderer
{
	uint32_t size; // sizeof(OpenBGI_FontRenderer_t) as the host was compiled
	/* CreateFontA(height, width, 0, 0, bold ? FW_BOLD : FW_THIN, italic, 0,
	 * 0, SHIFTJIS_CHARSET, OUT_TT_PRECIS, 0, PROOF_QUALITY, FIXED_PITCH,
	 * face): `height` the cell (ascent and descent), `width` the half-width
	 * character's, a full-width one twice as wide; NULL when it cannot */
	void* (*open)(void* user, const char* face, int height, int width, int bold, int italic);
	// the GDI text instruction's font: CreateFontA(size, size / 2, ..., DRAFT_QUALITY, FIXED_PITCH | FF_ROMAN, face)
	void* (*openMono)(void* user, const char* face, int size, int bold);
	void (*close)(void* user, void* font);
	/* clear the w x h buffer (pitch bytes a row) and draw the character
	 * (its `len` bytes) with its cell's top left at the buffer's, as TextOut
	 * into the original's 8-bit DIB: 0 blank, 255 ink, not antialiased */
	void (*render)(void* user, void* font, const char* ch, int len, uint8_t* buf, int pitch, int w, int h);
	// the same into a 1-bit bitmap, most significant bit first: a set bit is ink
	void (*renderMono)(void* user, void* font, const char* ch, int len, uint8_t* bits, int pitch, int w, int h);
	// the character's advance in pixels (GetTextExtentPoint32)
	int (*advance)(void* user, void* font, const char* ch, int len);
	// "B0 C2": a font file of the game (AddFontResource), then opened by its families and listed; 1 ok
	int (*addFile)(void* user, const char* path);
	// "B0 C3", the fonts built into the binary: a font's data (AddFontMemResourceEx), opened by its families, not listed; 1 ok
	int (*addMemory)(void* user, const void* data, uint32_t size);
	// 1 when a face of exactly that name is installed ("B0 C4", "B0 C6"); 1 when it cannot tell
	int (*faceExists)(void* user, const char* face);
	// 1 when `open` would open that face itself rather than another; 0 when it cannot tell
	int (*faceInstalled)(void* user, const char* face);
	/* the faces of a Windows charset (0x80 Shift-JIS, 0 ANSI, 0x81 Hangul,
	 * 0x86 GB2312, 0x88 Big5): their names, NUL-terminated one after the
	 * other, into `buf` when it is not NULL and the bytes they take into
	 * *bytes; the number of faces */
	int (*enumFaces)(void* user, char* buf, int charset, int* bytes);
	/* a font of the engine's own, which it opens in the place of MS Gothic
	 * or MS Mincho (built into the binary, or the player's): taken as
	 * addMemory takes one, and a font of its families is opened at the
	 * font's own proportions - a full-width character 2 * width wide,
	 * whatever its average character is (GDI makes the average character
	 * `width` wide, which squeezes a font whose average is not half a
	 * full-width one as it is in the MS fonts).  NULL when `open` gives
	 * every font its own proportions: addMemory is called then.  1 ok */
	int (*addSubstitute)(void* user, const void* data, uint32_t size);
} OpenBGI_FontRenderer_t;

/* What a host does itself (OpenBGI_Host_t.caps).  OPENBGI_CAP_DIALOGS:
 * it shows the message boxes and the scripts' dialogs (the requests
 * OPENBGI_ASK_MESSAGE .. OPENBGI_ASK_PROFILE) in windows of its own.
 * Without it the engine draws them over the frame's picture, in the
 * classic grey look, and runs them with the input the host hands over
 * (OpenBGI_Key, OpenBGI_Text, the pointer and its buttons); the host
 * never sees those requests then.  While a pad has the turn - it was
 * used last, or one is there and no device was used yet (OpenBGI_Pad) -
 * the engine draws them on a host that has them as well, since a pad
 * does not reach the host's windows; the same goes for the text entry
 * (OpenBGI_TextEntry_t).  The command line's --dialogs=frame or
 * --dialogs=host decides instead.  A box made while there is
 * no picture (before the window, after the shutdown) and the other
 * requests (files, folders, the game chooser, the installer) are always
 * the host's. */
#define OPENBGI_CAP_DIALOGS       1
/* OPENBGI_CAP_POINTER: the host shows a pointer of its own and puts it
 * where the frames say (OpenBGI_Frame_t.pointerMoved).  Without it the
 * engine draws an arrow in the frame's picture where the pointer is
 * while a pad's focus or stick has put it there (OpenBGI_Pad), as long
 * as the game shows the system's cursor; a host that hands a mouse over
 * and shows no pointer for it either, as a frontend in full screen does,
 * has the arrow drawn for the mouse too with the input setting
 * "pointer": "always" (OpenBGI_InputSettings).  OPENBGI_CAP_KEYBOARD: the host
 * has a keyboard, or shows one of its own while a frame says textInput.
 * Without it a touch on a text field brings up the engine's on-screen
 * keyboard (a pad brings it up on every host). */
#define OPENBGI_CAP_POINTER       2
#define OPENBGI_CAP_KEYBOARD      4
/* OPENBGI_CAP_JOBS: the host does the launcher's reading from the drives
 * - a folder's listing, a look at what may be a game - beside the frames
 * (OpenBGI_JobNext, below).  Without it the launcher's program waits for
 * each read where it asks for it, and with it the picture. */
#define OPENBGI_CAP_JOBS          8
/* OPENBGI_CAP_MESSAGES: the host shows the message boxes
 * (OPENBGI_ASK_MESSAGE) in windows of its own and leaves the scripts'
 * dialogs (OPENBGI_ASK_INPUT .. OPENBGI_ASK_PROFILE) to the engine, which
 * draws them in the frame: a host whose system has a box to show a
 * message with and nothing that fits the dialogs.  OPENBGI_CAP_DIALOGS
 * includes it. */
#define OPENBGI_CAP_MESSAGES      16

/* A text entry of the host's own: the edit control of the "B0 20" ..
 * "B0 2A" instructions as a window of the system's (Windows' EDIT
 * control, which the original used, transparent over the picture).
 * Without one (OpenBGI_Host_t.textEntry NULL) the engine draws the entry
 * over the frame's picture itself and takes its keys, its text
 * (OpenBGI_Text) and the clicks on it.  Texts are in the scripts'
 * encoding; the host reports the keys its entry takes with
 * OPENBGI_KEY_TAKEN, so that the key state follows them.  Every member
 * may be NULL.
 *
 * How the control is made and how Tab and Enter work changed over the
 * engine's builds; setStyle tells the host before each setup (and again
 * when "B0 28" changes), with OPENBGI_ENTRY_* flags:
 *   up to 1.69/451  centred, one line scrolled sideways (ES_CENTER |
 *                   ES_AUTOHSCROLL | ES_NOHIDESEL); Tab and Enter
 *                   select the text and give the keyboard back to the
 *                   game's window;
 *   1.69/472 on     left-aligned; several lines, wrapped, when the
 *                   rectangle is more than a line high (ES_MULTILINE,
 *                   without ES_AUTOVSCROLL: no more text than the lines
 *                   hold); showing and hiding it also opens and closes
 *                   the input method's status window; with "B0 28"
 *                   Enter hides it as well;
 *   1.529 on        Tab hides it;
 *   1.636 on        aligned as "B0 2A" says.
 * Tab acts on its key press, Enter on its character (WM_CHAR), which a
 * key the input method takes does not give. */
#define OPENBGI_ENTRY_CENTRE      1  // ES_CENTER ..
#define OPENBGI_ENTRY_RIGHT       2  // .. or ES_RIGHT; neither: ES_LEFT
#define OPENBGI_ENTRY_MULTILINE   4  // ES_MULTILINE (else ES_AUTOHSCROLL)
#define OPENBGI_ENTRY_TAB_HIDES   8  // Tab hides it (else Tab selects the text and gives the keyboard back)
#define OPENBGI_ENTRY_ENTER_HIDES 16 // Enter also hides it
#define OPENBGI_ENTRY_IME_STATUS  32 // show and hide open and close the input method's status window
typedef struct OpenBGI_TextEntry
{
	uint32_t size;                                                        // sizeof(OpenBGI_TextEntry_t) as the host was compiled
	void (*show)(void* user, int show);                                   // ShowWindow
	void (*move)(void* user, int x, int y, int w, int h);                 // MoveWindow, in the picture's pixels
	void (*setFont)(void* user, const char* face, int height, int width); // CreateFontA(height, width, .., FW_THIN, .., FIXED_PITCH, face)
	void (*limitText)(void* user, int maxBytes);                          // EM_LIMITTEXT
	void (*setText)(void* user, const char* text);                        // SetWindowText
	void (*selectAll)(void* user);                                        // EM_SETSEL 0, -1
	void (*focus)(void* user, int toEntry);                               // SetFocus(the entry / the game's window)
	int (*getText)(void* user, char* buf, size_t n);                      // GetWindowText into n bytes; the length
	void (*setColour)(void* user, uint32_t rgb);                          // the text colour, 0x00RRGGBB
	void (*refresh)(void* user);                                          // paint it again (the host's own, after it put the picture back)
	void (*destroy)(void* user);                                          // it goes with the window
	void (*rejectAscii)(void* user, int reject);                          // "B0 29": single-byte printable characters are refused
	void (*setStyle)(void* user, int flags);                              // OPENBGI_ENTRY_*: a new control when the alignment or the lines change
	int (*shown)(void* user);                                             // it is shown (Tab and Enter may have hidden it): "B0 23"
} OpenBGI_TextEntry_t;

/* Child windows of the host's own: the pop-up windows of "B0 10" ..
 * "B0 1C" as windows of the system's (the original's: WS_POPUP |
 * WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, owned by the game's window,
 * up to eight).  Positions are the outer windows' origins in the screen's
 * pixels; the sizes are the client areas', and the host adds its frame.
 * The engine keeps each window's picture and hands it over with
 * `present` when the window is made and whenever it changed; the pixels
 * stay valid and unchanged until the next present or the close, so the
 * window repaints itself from them (bpp and pitch as OpenBGI_Frame_t's).
 * The player cannot close a child window; the keys typed into one go to
 * the game's window as if typed there, but for Ctrl+C, which copies the
 * text of "B0 1C" to the clipboard (the text stays valid until the next
 * setCopyText or the close).  `create` answers the host's handle of the
 * window, NULL when it could not be made (the engine's slot is free
 * again then).  Without such windows
 * (OpenBGI_Host_t.childWindows NULL, or --child-windows=frame) the engine
 * draws them over the frame's picture itself, with a caption bar. */
typedef struct OpenBGI_ChildWindows
{
	uint32_t size;                                                                                   // sizeof(OpenBGI_ChildWindows_t) as the host was compiled
	void* (*create)(void* user, int slot, const char* title, int x, int y, int w, int h);            // hidden; `slot` 0 .. 7
	void (*close)(void* user, void* win);                                                            // it goes for good
	void (*show)(void* user, void* win, int show);                                                   // without taking the focus (SW_SHOWNA)
	void (*setTitle)(void* user, void* win, const char* title);                                      // SetWindowText
	void (*move)(void* user, void* win, int x, int y);                                               // the outer origin; the size stays
	void (*getPos)(void* user, void* win, int* x, int* y);                                           // the outer origin, wherever the player or the system put it
	void (*present)(void* user, void* win, const uint8_t* pixels, int pitch, int w, int h, int bpp); // the client's picture
	void (*setCopyText)(void* user, void* win, const char* text);                                    // what Ctrl+C copies; NULL: nothing
} OpenBGI_ChildWindows_t;

/* The main window: the game's window of the host's, through which the
 * engine makes, shows, titles, moves and resizes it, switches the display
 * mode and puts it in full screen, as the original did with its window
 * (WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 800 x 600 client
 * pixels at first, made hidden).  Positions are the outer window's origin
 * in the desktop's pixels; sizes are the client area's, the host adds its
 * frame (`frame` says how much: the width and the height it adds and the
 * caption with the top border).  The client area shows the frames'
 * pictures (OpenBGI_Frame_t); when it is of another size than the picture
 * (`setPicture`, "81 64") the host shows the picture scaled into it, and
 * the input it hands over stays in the picture's pixels.  A script may
 * give the window a sizing border (`setResizable`, "81 66" of 1.669 on):
 * the host then tells the engine the client size the player gave it
 * (OpenBGI_WindowSize), goes on scaling the picture into it, and the
 * script follows with a picture of that size when it likes ("81 67").
 * A host that can scale by more than one rule says so with `setScaling`:
 * the engine names the rule a game chose (OPENBGI_SCALE_*; the builds
 * from 1.616 on scale with Direct3D's bilinear filter, or with a bicubic
 * pixel shader when a script loaded it, "81 6F") and tells the scripts
 * that the shader is there; without the member the scripts hear that it
 * is not, and the host scales as it likes.
 * The window's input and events come back through the calls of "The
 * input" and "The window's events" below.  `setMode` asks for a display mode for the full
 * screen (ChangeDisplaySettings: 1 when the system took it), `restoreMode`
 * gives the desktop its own again; the cursor's `shape` is what "80 67"
 * sets (0 the arrow, 1 the game's own cursor, the original's resource
 * 0x6A; 2 .. 4 the arrow) and `cursorShow` counts as ShowCursor does.
 *
 * Without the table (OpenBGI_Host_t.window NULL) the engine's window is
 * one nobody sees: it exists from its creation to its destruction, at the
 * place and of the size the engine gave it, on a desktop of 1024 x 768
 * pixels, with no frame, never minimised; every display mode is taken,
 * the full screen covers the mode from (0, 0), and the window has the
 * focus while it is shown (as if the host said so with OpenBGI_Focus at
 * the next OpenBGI_PumpEvents).  With it, a NULL member does nothing, and
 * a NULL query answers as that window would. */
struct OpenBGI_Icon;
typedef struct OpenBGI_Window
{
	uint32_t size;                                                            // sizeof(OpenBGI_Window_t) as the host was compiled
	int (*create)(void* user, const char* title, int x, int y, int w, int h); // hidden; the game's title (the host may add its own name); 1 ok
	void (*destroy)(void* user);                                              // it goes for good
	void (*show)(void* user, int show);                                       // ShowWindow + UpdateWindow ("80 64")
	void (*setTitle)(void* user, const char* title);                          // "80 66"; NULL: the game's part cleared
	void (*move)(void* user, int x, int y);                                   // the outer origin
	void (*getRect)(void* user, int* x, int* y, int* w, int* h);              // the outer rectangle (GetWindowRect)
	int (*minimized)(void* user);                                             // IsIconic
	void (*minimize)(void* user);                                             // "80 65"
	void (*screenSize)(void* user, int* w, int* h);                           // the desktop (SM_CXSCREEN, SM_CYSCREEN)
	void (*frame)(void* user, int* fw, int* fh, int* top);                    // the frame: the width and height it adds, the caption and top border
	int (*setMode)(void* user, int w, int h, int bpp);                        // a display mode for the full screen; 1 ok
	void (*restoreMode)(void* user);                                          // the desktop's mode again
	void (*setFullscreen)(void* user, int w, int h);                          // borderless over the w x h mode
	void (*setWindowed)(void* user, int x, int y, int w, int h);              // framed again, at the outer origin, with a w x h client area
	void (*setPicture)(void* user, int w, int h);                             // the picture's size, after setWindowed
	void (*cursorShape)(void* user, int shape);                               // "80 67": 0 .. 4
	void (*cursorShow)(void* user, int show);                                 // ShowCursor
	void (*imeStatus)(void* user, int open);                                  // the input method's status window: closed while the game runs
	void (*dragAccept)(void* user, int accept);                               // DragAcceptFiles: whether OpenBGI_DropFile may come
	void (*setResizable)(void* user, int on);                                 // "81 66": a sizing border (and a maximise box); OpenBGI_WindowSize says what the player made of it
	void (*setScaling)(void* user, int rule);                                 // how a picture of another size than the client area is scaled: OPENBGI_SCALE_* (bilinear until told)
	/* the window's icon: one picture at `count` sizes, the largest first
	 * (the game's own program's, else the host's of OpenBGI_SetIcon, else
	 * OpenBGI's own); the pictures are valid during the call */
	void (*setIcon)(void* user, const struct OpenBGI_Icon* icons, int count);
	/* The window's size on the screen.  A host that gives `workArea` hands
	 * the size of the window to the engine: the scripts go on asking for
	 * client areas in the picture's pixels, and the engine makes the
	 * window's real one of them - enlarged by the screen's scale (`dpi`),
	 * made smaller in proportion until the window with its frame fits the
	 * work area - and tells the scripts of a desktop, a frame and a window
	 * rectangle that are smaller by the scale, as a program that knows
	 * nothing of such a screen is told.  The window then always has a
	 * sizing border (setResizable), and what the player makes of it comes
	 * back with OpenBGI_WindowSize: the scripts hear of it only when the
	 * game asked for the border itself ("81 66"; such a game is also told
	 * when a size it asked for had to be made smaller); otherwise the
	 * picture is shown at the new size until the game next asks for
	 * another one.  `setAspect` keeps the client area in proportion while
	 * it is sized.  The host draws no child windows there (the engine
	 * draws them in the picture), and the player has the display settings
	 * on the settings key (F9).
	 * The picture's pixels and the positions in it stay the engine's own
	 * throughout: the host scales the picture into the client area and
	 * maps the pointer back, as for any client area of another size.
	 * Without `workArea` the window is of the size the scripts ask for. */
	int (*dpi)(void* user);                                       // the scale of the window's screen in dots per inch: 96 for none, 144 for one and a half; NULL: 96
	void (*workArea)(void* user, int* x, int* y, int* w, int* h); // the part of the window's screen a window may take (without a task bar, a menu bar, a dock)
	void (*setAspect)(void* user, int w, int h);                  // the client area keeps the proportions w : h while the player sizes it; 0, 0: any
	/* Filters of the host's own beyond the three rules of setScaling - a
	 * shader of its graphics processor, say.  `filterName` answers the
	 * name of number n (0 on; ASCII, shown to the player in the display
	 * settings) and NULL where there is none, and for every n beyond.  The
	 * player's choice comes back through setScaling as OPENBGI_SCALE_OWN
	 * + n.  The names may differ from run to run (a filter that needs the
	 * graphics processor is not there without one). */
	const char* (*filterName)(void* user, int n);
} OpenBGI_Window_t;

// a picture of an icon (OpenBGI_Window_t.setIcon)
typedef struct OpenBGI_Icon
{
	int width, height;
	const uint8_t* pixels; // height rows of width pixels, the top row first: blue, green, red, alpha (not multiplied in)
} OpenBGI_Icon_t;

/* OpenBGI_Window_t.setScaling: the rule a picture is scaled by,
 * `rule & OPENBGI_SCALE_RULE`, and flags or'ed to it */
#define OPENBGI_SCALE_RULE     0xFF // the bits of the rule itself
#define OPENBGI_SCALE_BILINEAR 0    // the two nearest pixels each way, by their distance
#define OPENBGI_SCALE_BICUBIC  1    // four each way: sharper
#define OPENBGI_SCALE_NEAREST  2    // the nearest one: hard edges
/* or'ed to the rule: a picture is made larger by whole numbers alone (1,
 * 2, 3 times), with black around it where the client area has room left;
 * one that has to be made smaller is scaled as without it */
#define OPENBGI_SCALE_WHOLE    0x100
/* or'ed to the rule: the player chose it in the settings.  A host that
 * otherwise picks a rule of its own somewhere - for a picture only the
 * display's own pixels enlarge - follows the rule there too */
#define OPENBGI_SCALE_CHOSEN   0x200
/* a filter of the host's own (OpenBGI_Window_t.filterName): number n is
 * OPENBGI_SCALE_OWN + n (n below 240) */
#define OPENBGI_SCALE_OWN      16
/* or'ed to the rule: the filter's better and slower kind, where it has
 * two (the player's "quality" in the display settings) */
#define OPENBGI_SCALE_HIGH     0x400

/* The debugger's window (--debug, F12): a second window of the host's
 * with a w x h client area that shows the debugger's picture 1:1 (32-bit
 * pixels as OpenBGI_Frame_t's, valid during the call) and hands back its
 * input with OpenBGI_DebugKey, OpenBGI_DebugMouse, OpenBGI_DebugWheel and
 * OpenBGI_DebugClose (its close box, or the window gone).  `open` answers
 * 1 when the window is up.  Without the table, or when `open` fails, the
 * debugger draws over the game's picture. */
typedef struct OpenBGI_DebugWindow
{
	uint32_t size; // sizeof(OpenBGI_DebugWindow_t) as the host was compiled
	int (*open)(void* user, int w, int h, const char* title);
	void (*close)(void* user);
	void (*present)(void* user, const uint8_t* pixels, int pitch, int w, int h);
} OpenBGI_DebugWindow_t;

/* The files.  The engine reaches every file through the host's table
 * (OpenBGI_Files_t) and never opens one itself.  It speaks of two kinds of
 * paths:
 *
 *   * the game's paths, as the scripts and the original build them: in the
 *     scripts' encoding (Shift-JIS, or UTF-8 once the host was told so,
 *     OpenBGI_Host_t.textEncoding), '\\' as the separator, absolute ones
 *     with a drive ("C:\..."), relative ones under the current directory
 *     (setCurrentDir).  The game directory is `exeDir`, a path of this kind
 *     with a trailing separator, where the original's executable lies: the
 *     engine's base directory.  A host on a system without drives presents
 *     the game directory as one (the POSIX hosts: "C:\").  The calls keep
 *     the Win32 conventions the engine was written against: CreateFile's
 *     three ways of opening (OPENBGI_OPEN_*), GetFileAttributes' bits
 *     (OPENBGI_ATTR_*, OPENBGI_INVALID_ATTRS when there is no such file),
 *     FILETIME stamps, FindFirstFile's wildcards ('*', '?', ASCII case
 *     ignored), CopyFile, MoveFile, CreateDirectory (0 when it exists),
 *     RemoveDirectory (only empty ones), and the drives the disc checks
 *     ask about (GetDriveType's OPENBGI_DRIVE_*, GetLogicalDriveStrings'
 *     list "A:\" NUL "C:\" NUL NUL, a ready volume, a medium in a drive);
 *     the installer's shell folders (SHGetSpecialFolderLocation's CSIDL,
 *     GetWindowsDirectory) are paths of this kind too.
 *   * native paths: the host's own, for what is not the game's - the
 *     binary's directory (openbgi.json), the folders the player picks for
 *     the game chooser, the user's data folder, where a game played from a
 *     zip archive keeps what it writes (and its archive), the extraction
 *     options' output.  The engine only joins them with `nativeSep` (and
 *     '/'), takes no Shift-JIS lead byte for a separator where
 *     `nativeSjis` says the native paths are Shift-JIS (Windows), and
 *     shows them through toUtf8 / fromUtf8 (NULL: they are UTF-8).
 *     setGameDir makes a native directory the game directory: exeDir and
 *     the current directory from then on.  nativeRoots (interface 1.1)
 *     is for a system on which the folders above its storage cannot be
 *     listed (Android): it names the native folders a folder browser
 *     offers in their place - a storage volume each, with what the
 *     system calls it.  The launcher lists them above everything, as it
 *     lists the drives where paths have drives, and takes none of them
 *     for a folder inside another.  A host on which every folder can be
 *     reached from the root leaves it NULL.
 *
 * A stream - what open and nativeOpen give - is read, written, sought,
 * measured and closed with the same calls; positions are 64-bit but a
 * game file is at most 4 GB, as in the original.  Every member may be
 * NULL (and the whole table): what it does then fails, as when the file
 * is not there. */
#define OPENBGI_OPEN_READ       0 // an existing file, for reading (GENERIC_READ, OPEN_EXISTING)
#define OPENBGI_OPEN_CREATE     1 // a new file, an existing one emptied, for writing (GENERIC_WRITE, CREATE_ALWAYS)
#define OPENBGI_OPEN_WRITE      2 // an existing file kept, else a new one, for writing from its start (GENERIC_WRITE, OPEN_ALWAYS)

#define OPENBGI_INVALID_ATTRS   0xFFFFFFFFu // no such file (INVALID_FILE_ATTRIBUTES)
#define OPENBGI_ATTR_READONLY   0x01u
#define OPENBGI_ATTR_HIDDEN     0x02u
#define OPENBGI_ATTR_SYSTEM     0x04u
#define OPENBGI_ATTR_DIRECTORY  0x10u
#define OPENBGI_ATTR_ARCHIVE    0x20u
#define OPENBGI_ATTR_NORMAL     0x80u // no other attribute is set

#define OPENBGI_DRIVE_UNKNOWN   0 // the GetDriveType values
#define OPENBGI_DRIVE_NO_ROOT   1
#define OPENBGI_DRIVE_REMOVABLE 2
#define OPENBGI_DRIVE_FIXED     3
#define OPENBGI_DRIVE_REMOTE    4
#define OPENBGI_DRIVE_CDROM     5
#define OPENBGI_DRIVE_RAMDISK   6

#define OPENBGI_NATIVE_NONE     0 // nativeStat: nothing of that name
#define OPENBGI_NATIVE_FILE     1
#define OPENBGI_NATIVE_DIR      2

// an entry of a directory listing (findFirst / findNext)
typedef struct OpenBGI_FindData
{
	uint32_t attrs; // OPENBGI_ATTR_* of the entry
	char name[260]; // its name alone (no directory), NUL-terminated
} OpenBGI_FindData_t;

// what nativeStat tells besides the kind
typedef struct OpenBGI_NativeStat
{
	uint64_t size; // a file's size, bytes
	int64_t mtime; // its last modification, milliseconds since 1970-01-01 UTC
} OpenBGI_NativeStat_t;

typedef struct OpenBGI_Files
{
	uint32_t size; // sizeof(OpenBGI_Files_t) as the host was compiled
	// the game's files, by the game's paths
	void* (*open)(void* user, const char* path, int mode);                        // OPENBGI_OPEN_*; NULL when it cannot be opened (or is a directory)
	void (*close)(void* user, void* f);                                           // a stream of open or nativeOpen
	uint32_t (*read)(void* user, void* f, void* buf, uint32_t n);                 // the bytes read; 0 at the end or on an error
	uint32_t (*write)(void* user, void* f, const void* buf, uint32_t n);          // the bytes written
	int (*seek)(void* user, void* f, uint64_t pos);                               // from the start; 1 ok
	uint64_t (*length)(void* user, void* f);                                      // the stream's size, bytes
	int (*getTime)(void* user, void* f, uint32_t* lo, uint32_t* hi);              // the last write, a FILETIME (100 ns since 1601) in two halves
	int (*setTime)(void* user, void* f, uint32_t lo, uint32_t hi);                // (open's streams)
	uint32_t (*attrs)(void* user, const char* path);                              // OPENBGI_ATTR_*, OPENBGI_INVALID_ATTRS without such a file
	int (*setAttrs)(void* user, const char* path, uint32_t attrs);                // the read-only bit at least
	int (*remove)(void* user, const char* path);                                  // DeleteFile
	int (*move)(void* user, const char* from, const char* to);                    // MoveFile
	int (*copy)(void* user, const char* from, const char* to, int failIfExists);  // CopyFile
	int (*makeDir)(void* user, const char* path);                                 // CreateDirectory: 0 when it exists already
	int (*removeDir)(void* user, const char* path);                               // RemoveDirectory: an empty one
	void* (*findFirst)(void* user, const char* pattern, OpenBGI_FindData_t* out); // the first match; NULL: none (nothing to close)
	int (*findNext)(void* user, void* find, OpenBGI_FindData_t* out);             // the next match; 0 at the end
	void (*findClose)(void* user, void* find);                                    // accepts NULL
	int (*setCurrentDir)(void* user, const char* dir);                            // SetCurrentDirectory
	void (*exeDir)(void* user, char* buf, size_t n);                              // the game directory, a trailing separator
	uint32_t (*driveType)(void* user, const char* root);                          // OPENBGI_DRIVE_* of "X:\"
	uint32_t (*logicalDrives)(void* user, char* buf, uint32_t n);                 // the roots, NUL-separated, a NUL more at the end; the length without it
	int (*volumeReady)(void* user, const char* root);                             // a volume is mounted at "X:\" and readable
	int (*mediaPresent)(void* user, char drive);                                  // a removable drive ('A' ..) holds a medium
	int (*specialFolder)(void* user, int csidl, char* buf, size_t n);             // a shell folder; 1 ok
	int (*windowsDir)(void* user, char* buf, size_t n);                           // the Windows directory; 1 ok
	// native paths
	int nativeSep;                                                                                                 // the separator to join them with: '/' or '\\'
	int nativeSjis;                                                                                                // they are Shift-JIS: a lead byte's trail byte is no separator
	void* (*nativeOpen)(void* user, const char* path, int mode);                                                   // OPENBGI_OPEN_READ or _CREATE; NULL when it cannot be opened
	int (*nativeStat)(void* user, const char* path, OpenBGI_NativeStat_t* out);                                    // OPENBGI_NATIVE_*; `out` may be NULL
	int (*nativeMkdir)(void* user, const char* path);                                                              // 1 when it exists afterwards (the parent has to)
	int (*nativeRemove)(void* user, const char* path);                                                             // a file; 1 when it is gone
	int (*nativeList)(void* user, const char* dir, void (*fn)(void* ctx, const char* name, int isDir), void* ctx); // every entry but "." and ".."; their count, -1 when unreadable
	void (*nativeAbsolute)(void* user, const char* path, char* out, size_t n);                                     // the full path of one that may be relative
	void (*toUtf8)(void* user, const char* native, char* out, size_t n);                                           // a native string as UTF-8
	void (*fromUtf8)(void* user, const char* utf8, char* out, size_t n);                                           // and back
	void (*gameDir)(void* user, char* buf, size_t n);                                                              // the game directory, a native path with a trailing separator
	int (*setGameDir)(void* user, const char* dir);                                                                // a native directory as the game directory; 1 when it exists
	void (*userDataDir)(void* user, char* buf, size_t n);                                                          // the user's data folder (a trailing separator), made when missing; "" for none
	void (*binaryDir)(void* user, char* buf, size_t n);                                                            // the running program's directory (a trailing separator)
	int (*nativeWritable)(void* user, const char* path);                                                           // 1 when the file there can be written to as it is (nothing is changed); NULL: every file can
	// interface 1.1 on
	uint32_t (*nativeRoots)(void* user, char* buf, uint32_t n); // the folders a browser starts from: for each its native path, NUL, its name for the player (may be empty), NUL; a NUL more at the end; the length without it, 0 for none
} OpenBGI_Files_t;

/* The registry the installer's instructions read and write ("80 Ex",
 * src/sys/install/): keys start with "HKLM\", "HKCU\" or "HKCR\" (none:
 * HKCU) and go on with the path below the hive; only REG_SZ values.  A
 * host without a registry keeps the values in a file.  Every member may
 * be NULL (and the table): what it does then fails. */
typedef struct OpenBGI_Registry
{
	uint32_t size;                                                                    // sizeof(OpenBGI_Registry_t) as the host was compiled
	int (*read)(void* user, const char* key, const char* value, char* buf, size_t n); // a REG_SZ into buf (`value` NULL: the default one); 1 when read
	int (*createKey)(void* user, const char* key, const char* cls);                   // the key, made with the class `cls` (RegCreateKeyEx); 1 when it exists now
	int (*write)(void* user, const char* key, const char* value, const char* s);      // made with its key (`value` NULL: the default one); 1 ok
	int (*deleteKey)(void* user, const char* key);                                    // with its values; 1 ok
} OpenBGI_Registry_t;

/* The machine: what the start-up and the scripts ask about the computer,
 * as the original read it from Windows and the processor - the
 * processor's CPUID and time stamp counter (the start-up's checks and its
 * clock measurement, "80 0A", "81 0A", the SSE blitters, the installer's
 * machine key), the processors, the memory ("80 0D", "81 0D"), the
 * computer's and the user's names, the Windows version ("81 0C"; the
 * platform decides the engine's NT paths), the user's language ("81
 * 02"), the display adapter ("81 0B") and whether DirectSound 8 is
 * there (the start-up refuses to run without it, "80 FE").  Every member
 * may be NULL, and the table: the engine then answers as an ordinary
 * machine of the original's time, as each member says.  Without `cpuid`
 * the host has no x86 processor to report: the start-up's processor
 * checks are skipped and the blitters run without SSE. */
typedef struct OpenBGI_MemoryStatus
{
	uint32_t loadPercent, totalPhys, availPhys, totalPageFile, availPageFile, totalVirtual, availVirtual; // GlobalMemoryStatus: bytes, capped at 4 GB
} OpenBGI_MemoryStatus_t;

typedef struct OpenBGI_Machine
{
	uint32_t size;                                                                // sizeof(OpenBGI_Machine_t) as the host was compiled
	int (*cpuid)(void* user, uint32_t leaf, uint32_t out[4]);                     // EAX, EBX, ECX, EDX; 0 without CPUID or above the highest leaf
	uint64_t (*readTsc)(void* user);                                              // RDTSC after a serialising CPUID; NULL: 0
	int (*cpuCount)(void* user);                                                  // processors online; NULL: 1
	void (*memoryStatus)(void* user, OpenBGI_MemoryStatus_t* out);                // NULL: 1 GB, half of it free, 2 GB of page file
	void (*memoryStatusEx)(void* user, uint64_t* totalPhys, uint64_t* availPhys); // the physical memory in full; NULL: as above
	int (*computerName)(void* user, char* buf, size_t n);                         // GetComputerName into the emptied buffer; 1 ok; NULL: "OPENBGI"
	int (*userName)(void* user, char* buf, size_t n);                             // GetUserName, the same; NULL: "user"
	void (*version)(void* user, uint32_t out[4], char* csd, size_t n);            // major, minor, build, platform (2 NT) and the service pack; NULL: XP SP3, 5.1.2600
	int (*uiLanguage)(void* user);                                                // GetUserDefaultUILanguage, a LANGID; NULL: 0x411 (Japanese).  Japanese here makes the engine's own messages Japanese (else English), unless --lang= or BGI_LANG say
	int (*displayAdapter)(void* user, char* desc, size_t n, uint32_t version[4]); // the adapter's description and driver version, most significant first; NULL: none
	int (*audioAvailable)(void* user);                                            // DirectSound 8 is installed; NULL: 1
	// interface 1.1 on
	uint64_t (*processMemory)(void* user); // the bytes of memory this process holds now (its resident set), for the engine's performance display; NULL or 0: not known
} OpenBGI_Machine_t;

/* The shell: the desktop, the clipboard and other copies of the game, as
 * the original reached them through Windows.  `instanceLock` takes the
 * name that keeps a second copy of the game from starting (CreateMutex),
 * *alreadyExists 1 when another holds it; `instanceUnlock` gives it
 * back.  `sendToRunning` hands a boot path to the running copy whose main
 * window has class `windowClass` (the file mapping and the message
 * 0x9000, which the receiver's host hands over as OpenBGI_DropFile).
 * `openDocument` shows a web link ("http://...", "https://...") or a
 * document - a text, a page, a PDF, a picture: a path of the game's that
 * the host can open - with the program the desktop has for it ("80 E3",
 * the original's ShellExecute "open"); the engine asks for nothing else
 * through it, and a host hands the target on as one argument, never
 * through a command line.  `clipboardSetText` is
 * SetClipboardData(CF_TEXT) ("7E", the child windows' Ctrl+C),
 * `setWallpaper` the desktop's wallpaper ("B0 F0": WallpaperStyle "2"
 * when stretched, TileWallpaper "1" when tiled), `createShortcut` an
 * IShellLink to `target` with the command line `args` (NULL: none),
 * `notifyAssocChanged` SHChangeNotify (SHCNE_ASSOCCHANGED).  Paths and
 * texts are the game's (the scripts' encoding); each call returns 1 when
 * done.  Every member may be NULL, and the table: the call then fails
 * and no copy holds the names.
 *
 * There is no call that starts a program: the engine starts none for a
 * game.  The scripts' requests for one ("80 E0" .. "80 E2": a
 * registration tool, an uninstaller, another game) are refused and
 * answered as a program that could not be started. */
typedef struct OpenBGI_Shell
{
	uint32_t size;                                                           // sizeof(OpenBGI_Shell_t) as the host was compiled
	void* (*instanceLock)(void* user, const char* name, int* alreadyExists); // the handle either way (NULL allowed)
	void (*instanceUnlock)(void* user, void* lock);
	int (*sendToRunning)(void* user, const char* windowClass, const char* data);
	int (*openDocument)(void* user, const char* target);
	int (*clipboardSetText)(void* user, const char* text);
	int (*setWallpaper)(void* user, const char* path, int stretch, int tile);
	int (*createShortcut)(void* user, const char* linkPath, const char* target, const char* args);
	void (*notifyAssocChanged)(void* user);
} OpenBGI_Shell_t;

/* CD audio ("A0 80" .. "A0 86"), as the original drove the "cdaudio"
 * MCI device: `open` with the drive ("d:") as the element, or NULL for
 * the default drive, gives a device id (0 for none); `setTimeFormatTmsf`
 * sets MCI_FORMAT_TMSF; `status` answers an MCI_STATUS_ITEM
 * (OPENBGI_MCI_STATUS_*: the mode, MCI_MODE_NOT_READY onwards, or the
 * number of tracks); `play` plays from one TMSF position to another,
 * with `notify` the host hands the end over as OpenBGI_CdNotify; `stop`
 * and `close`.  Each returns 1 when done.  Every member may be NULL, and
 * the table: there is no CD audio then. */
#define OPENBGI_MCI_STATUS_MODE    4
#define OPENBGI_MCI_STATUS_TRACKS  7
#define OPENBGI_MCI_MODE_NOT_READY 0x20c // .. 0x212: stopped, playing, recording, seeking, paused, open

typedef struct OpenBGI_CdAudio
{
	uint32_t size; // sizeof(OpenBGI_CdAudio_t) as the host was compiled
	uint32_t (*open)(void* user, const char* element);
	int (*setTimeFormatTmsf)(void* user, uint32_t device);
	int (*status)(void* user, uint32_t device, uint32_t item, uint32_t* out);
	int (*play)(void* user, uint32_t device, uint32_t from, uint32_t to, int notify);
	int (*stop)(void* user, uint32_t device);
	int (*close)(void* user, uint32_t device);
} OpenBGI_CdAudio_t;

/* The movies of "90 F0" .. "90 F3" (the opening films beside the
 * archives).  The original plays them with DirectShow in a child window
 * over the picture.  A host has a player of its own that does the same
 * (OpenBGI_MoviePlayer_t), a decoder that hands the engine the frames
 * and the sound to show and mix itself (OpenBGI_MovieDecoder_t), both or
 * neither.  The engine plays a movie with the decoder when the host has
 * one - the same on every system; openbgi_ffmovie.h has one for the hosts to
 * share - and with the player when it has only that, or when the player
 * was asked for ("movies": "player" in openbgi.json, --movies=player):
 * then the decoder gets what the player cannot play.  The player opens
 * the file by its name: `path` is a game's path of a file the host can
 * open (one that only a game's archive or disc image holds is extracted
 * for it first).  The decoder reads the file through the engine
 * (OpenBGI_MovieFile_t), in place wherever the game has it.  Volumes are
 * IBasicAudio's hundredths of a decibel (-10000 silent .. 0).
 *
 * The player: `open` builds the player for the file (OPENBGI_MOVIE_*:
 * OK, a missing audio device or renderer the engine reports as a script
 * error, FAILED when it cannot play the file - everything released
 * again); `start` shows it in the window's client rectangle (x, y, w,
 * h), starts it and gives its length in ms; `stop` stops it and releases
 * everything; `running` is 1 until it reached its end; `setVolume` works
 * without a movie too (ignored).  The host hands the player's events
 * over as OpenBGI_MovieEvent (completed at the end).
 *
 * The decoder: `open` starts decoding the movie it reads from `file`
 * (`path`, the game's path of it, is for the host's messages only) -
 * the frames 32-bit BGRA
 * at w x h, numbered from 0 at the rate it says (`info`: the file's
 * frame rate, or a multiple of it), with `audio` its sound too, 16-bit
 * stereo at OPENBGI_AUDIO_RATE; NULL when it cannot.  `frames` is how
 * many frames have been decoded so far (counting from 0, never going
 * back), *ended 1 once the last one is.  `take` copies the picture that
 * stands at frame `index` into `out` (w * h * 4 bytes) - the frame's
 * own, or where the file has none for that number the one before it -
 * and lets go of it and every older one; 0 when there is nothing new to
 * show there: the picture was taken already, or the frame is not
 * decoded yet.  The host decodes ahead until it holds at least four
 * pictures that were not taken (the engine drops the late ones) and may
 * wait there.
 * `samples` moves up to `frames`
 * stereo frames of the sound decoded so far into `out` (NULL: they are
 * dropped; `frames` 0 only asks) and returns their count, *ended 1 once
 * the sound is over and all of it was taken.  `close` stops decoding and
 * releases everything.  The engine calls it on its own thread only. */
#define OPENBGI_MOVIE_FAILED             0
#define OPENBGI_MOVIE_OK                 1
#define OPENBGI_MOVIE_NO_AUDIO_DEVICE    2 // VFW_E_NO_AUDIO_HARDWARE
#define OPENBGI_MOVIE_VIDEO_NOT_RENDERED 3 // VFW_S_VIDEO_NOT_RENDERED
#define OPENBGI_MOVIE_AUDIO_NOT_RENDERED 4 // VFW_S_AUDIO_NOT_RENDERED

typedef struct OpenBGI_MoviePlayer
{
	uint32_t size; // sizeof(OpenBGI_MoviePlayer_t) as the host was compiled
	int (*open)(void* user, const char* path);
	int (*start)(void* user, int x, int y, int w, int h, int32_t volume, int32_t* lengthMs);
	void (*stop)(void* user);
	int (*running)(void* user);
	void (*setVolume)(void* user, int32_t volume);
} OpenBGI_MoviePlayer_t;

typedef struct OpenBGI_MovieInfo
{
	double fps;       // the rate the frames are numbered at (30 when the file does not say)
	int32_t lengthMs; // the length, 0 when unknown
	int audio;        // the sound is decoded too
} OpenBGI_MovieInfo_t;

/* A movie's file as the decoder reads it: through the engine, which has
 * the file wherever the game keeps it - its folder, a zip archive, a
 * disc image - and reads it in place there (a file that is compressed in
 * an archive from memory, or from a copy made once when it is large).
 * `read` moves up to `n` bytes from the current position into `buf` and
 * returns how many (0 at the end, less than 0 for an error); `seek` sets
 * the position, counted from the start, and returns 1 when it could.
 * `length` is the file's size in bytes, -1 when it is not known (the
 * file is then read from its start to its end, without looking ahead).
 * `ctx` is handed back to both, on the engine's thread - the one the
 * decoder is called on.  The decoder keeps a copy of the struct; the
 * file stays open until its `close` has returned. */
typedef struct OpenBGI_MovieFile
{
	uint32_t size; // sizeof(OpenBGI_MovieFile_t) as its maker was compiled
	void* ctx;
	int (*read)(void* ctx, uint8_t* buf, int n);
	int (*seek)(void* ctx, int64_t pos);
	int64_t length;
} OpenBGI_MovieFile_t;

typedef struct OpenBGI_MovieDecoder
{
	uint32_t size; // sizeof(OpenBGI_MovieDecoder_t) as the host was compiled
	void* (*open)(void* user, const OpenBGI_MovieFile_t* file, const char* path, int w, int h, int audio, OpenBGI_MovieInfo_t* info);
	int32_t (*frames)(void* user, void* movie, int* ended);
	int (*take)(void* user, void* movie, int32_t index, uint8_t* out);
	int (*samples)(void* user, void* movie, int16_t* out, int frames, int* ended);
	void (*close)(void* user, void* movie);
} OpenBGI_MovieDecoder_t;

/* The services a host provides.  The engine copies the struct and the
 * tables it points at (OpenBGI_New), so they need not stay; `user` is
 * handed back to every call, always on the thread that runs the engine.
 * `size` and `api` say which header the host was built with:
 * `OpenBGI_Host_t host = {.size = sizeof host, .api =
 * OPENBGI_API_VERSION};`. */
typedef struct OpenBGI_Host
{
	uint32_t size; // sizeof(OpenBGI_Host_t) as the host was compiled
	uint32_t api;  // OPENBGI_API_VERSION as the host was compiled
	void* user;
	uint32_t (*ticksMs)(void* user);                        // milliseconds since an arbitrary start, wrapping at 2^32
	int (*perfNs)(void* user, int64_t* out);                // a fine-grained count of nanoseconds; 0 when there is none
	void (*localTime)(void* user, OpenBGI_DateTime_t* out); // the wall clock in the local time zone
	const OpenBGI_FontFiles_t* fontFiles;                   // the system's fonts for the engine's rasteriser; NULL: none
	const OpenBGI_FontRenderer_t* fontRenderer;             // a renderer to draw with instead of the engine's; NULL: the engine's
	uint32_t caps;                                          // OPENBGI_CAP_*: what the host does itself
	const OpenBGI_TextEntry_t* textEntry;                   // a text entry of the host's own; NULL: the engine's in the frame
	const OpenBGI_ChildWindows_t* childWindows;             // child windows of the host's own; NULL: the engine's in the frame
	const OpenBGI_Files_t* files;                           // the files; NULL: none
	const OpenBGI_Registry_t* registry;                     // the registry; NULL: none
	/* "81 00" of 1.653 on: the scripts' text - and so the texts and the
	 * game's paths the engine hands the host - is UTF-8 from now on (1),
	 * or Shift-JIS again (0); the engine's own messages stay Shift-JIS,
	 * so a well-formed UTF-8 sequence is taken as it is and the rest
	 * converted.  NULL: the host converts nothing */
	void (*textEncoding)(void* user, int utf8);
	const OpenBGI_Window_t* window;             // the game's window; NULL: one nobody sees
	const OpenBGI_DebugWindow_t* debugWindow;   // the debugger's window; NULL: the debugger draws over the picture
	const OpenBGI_Machine_t* machine;           // the machine's facts; NULL: an ordinary machine of the original's time
	const OpenBGI_Shell_t* shell;               // other programs, the clipboard, the desktop; NULL: none
	const OpenBGI_CdAudio_t* cdAudio;           // CD audio; NULL: none
	const OpenBGI_MoviePlayer_t* moviePlayer;   // a movie player of the host's own; NULL: none
	const OpenBGI_MovieDecoder_t* movieDecoder; // a movie decoder for the engine to show and mix; NULL: none
} OpenBGI_Host_t;

// a rectangle of the window's picture: its top left pixel and its size
typedef struct OpenBGI_Rect
{
	int x, y, w, h;
} OpenBGI_Rect_t;

#define OPENBGI_FRAME_CHANGES  32 // the most rectangles a frame reports changed (OpenBGI_Frame_t.change)

/* What a frame tells the host: how long to wait, and the window's picture.
 *
 * The picture is everything the main window shows - the game's frames, the
 * screen quake's shifted copies, the bitmaps put straight on the window, a
 * movie's frames, the debugger's overlay - in the display mode's pixels:
 * the picture size, or the whole display mode when the full screen is
 * letter-boxed.  `bpp` 16 is RGB 5-5-5 (bit 15 unused), 32 is B G R x (the
 * top byte unused); rows are `pitch` bytes apart (a multiple of 4), top
 * row first.  `pixels` is NULL while there is no picture.  The host shows
 * it scaled to its window as it sees fit (the window's size is the
 * picture size unless the game set another, or the full screen's).
 *
 * `changes` says what changed since the frame before, whatever changed it
 * (the frame, or the events handed over in between): 0 nothing, n > 0 the
 * rectangles change[0 .. n - 1] (they may overlap), -1 all of it - a new
 * picture, of another size or pixel mode perhaps.  A host that keeps its
 * window up to date copies just those.  The pixels stay valid until the
 * next call of OpenBGI_RunFrame or OpenBGI_Stop, so that the host can
 * paint its window again from them when the window is uncovered - as
 * `expose` says - or put the picture back under a window of its own over
 * it (its text entry); what the events handed over before the next frame
 * change in them, the next frame reports.  `expose` OPENBGI_EXPOSE_PICTURE: the
 * picture; OPENBGI_EXPOSE_BLACK: black, while the engine is not up yet or
 * a movie of the host's draws over the window; OPENBGI_EXPOSE_NONE:
 * nothing, while the game has presenting off. */
#define OPENBGI_EXPOSE_PICTURE 0
#define OPENBGI_EXPOSE_BLACK   1
#define OPENBGI_EXPOSE_NONE    2
typedef struct OpenBGI_Frame
{
	uint32_t size; // sizeof(OpenBGI_Frame_t) as the host was compiled: the host sets it once, the engine fills in the rest
	int presented; // 1 when the frame presented the game's picture (the compositor's)
	/* the wait before the next frame: 0 the idle wait of about half a
	 * millisecond, any other value the "80 52" sleep in ms (1.535 on, for a
	 * frame that presented nothing) or what the engine is waiting for */
	int idleMs;
	const uint8_t* pixels; // the window's picture
	int width, height, pitch, bpp;
	int changes; // 0 none, n the rectangles in `change`, -1 everything
	OpenBGI_Rect_t change[OPENBGI_FRAME_CHANGES];
	int expose; // what an uncovered window shows: OPENBGI_EXPOSE_*
	/* 1 when the engine moved the pointer since the frame before (a
	 * script's glide, "80 1F"): the host puts the system's pointer at
	 * (pointerX, pointerY), in the picture's pixels as OpenBGI_Pointer
	 * has them.  The engine already counts it there. */
	int pointerMoved;
	int pointerX, pointerY;
	/* 1 while the engine takes typed text (OpenBGI_Text): the text entry
	 * it draws, or a text field of a dialog it draws, has the focus.  The
	 * host has its input method (an IME, an on-screen keyboard) on for
	 * that time, placed at textRect, the field in the picture's pixels. */
	int textInput;
	OpenBGI_Rect_t textRect;
	/* Interface 1.1 on: the layer, for a host that said what it draws on
	 * (OpenBGI_SetSurface, below) - what the engine draws over everything
	 * there, the bars beside the picture included: its on-screen controls.
	 * `layer` is layerWidth x layerHeight pixels (the surface's size) of
	 * four bytes, blue, green, red and alpha, the colours not multiplied
	 * by the alpha, layerPitch bytes a row; NULL when there is nothing to
	 * draw.  The host draws it over the scaled picture, a pixel a pixel.
	 * `layerChanges` is 0 when it is as the frame before had it, 1 when
	 * the rectangle `layerChange` of it changed, -1 for all of it (another
	 * size, or a layer where there was none).  The pixels stay valid as
	 * the picture's do. */
	const uint8_t* layer;
	int layerWidth, layerHeight, layerPitch;
	int layerChanges;
	OpenBGI_Rect_t layerChange;
} OpenBGI_Frame_t;

/* What the engine asks the host (OpenBGI_Request_t.kind).  The strings
 * are in the engine's text encoding (Shift-JIS, or UTF-8 after "81 00");
 * the pointers - the buffers for the answer included - stay valid until
 * the request is answered.  The answer is one int, as the Windows call
 * of the same purpose returns it, and 0 where the host does not show
 * what is asked for: a text, a file, a folder not chosen, the game
 * chooser left, an installer's dialog cancelled.  The game chooser runs
 * engine code while it is up, through the callbacks in its description
 * (OpenBGI_GameChooser_t below).
 *
 * The installer's dialogs (OPENBGI_ASK_INSTALLER: the three of a setup
 * program's "80 F0" .. "80 F2") are not part of this interface.  Their
 * descriptions are the engine's own and may change with it; only a host
 * that is built with the engine's sources shows them.  A host of the
 * library answers 0. */
#define OPENBGI_ASK_MESSAGE              1 // a message box (MessageBox): the button pressed, OPENBGI_ID*
#define OPENBGI_ASK_INPUT                2 // one text field ("B0 84"): 1 ok (the entry in `out`), 0 cancelled
#define OPENBGI_ASK_INPUT2               3 // two text fields ("B0 85" / "B0 87"): 1 ok, 0 cancelled
#define OPENBGI_ASK_PROFILE              4 // the player profile ("B0 8F"): 1 ok
#define OPENBGI_ASK_FILE                 5 // a file to open or save ("80 3B" / "81 38", GetOpenFileName / GetSaveFileName): 1 chosen, 0 cancelled
#define OPENBGI_ASK_FOLDER               6 // a folder ("81 3A", SHBrowseForFolder): 1 chosen, 0 cancelled
#define OPENBGI_ASK_GAME_CHOOSER         7 // the game chooser: 1 launch, 0 quit, -1 no display to show it on
#define OPENBGI_ASK_INSTALLER            8 // an installer's dialog ("80 F0" .. "80 F2"): 0 cancelled (see above)

/* A message box's flags (MessageBox's MB_* values: the buttons, the
 * icon, the default button, system modal) and the buttons it answers
 * with (the close box counts as cancel). */
#define OPENBGI_MB_OK                    0x0000 // buttons (flags & 0xf)
#define OPENBGI_MB_OKCANCEL              0x0001
#define OPENBGI_MB_YESNO                 0x0004
#define OPENBGI_MB_RETRYCANCEL           0x0005
#define OPENBGI_MB_ICONHAND              0x0010 // icon (flags & 0xf0)
#define OPENBGI_MB_ICONQUESTION          0x0020
#define OPENBGI_MB_ICONWARNING           0x0030
#define OPENBGI_MB_ICONINFO              0x0040
#define OPENBGI_MB_DEFBUTTON2            0x0100 // the second button is the default
#define OPENBGI_MB_SYSTEMMODAL           0x1000
#define OPENBGI_IDOK                     1
#define OPENBGI_IDCANCEL                 2
#define OPENBGI_IDRETRY                  4
#define OPENBGI_IDYES                    6
#define OPENBGI_IDNO                     7

/* The game chooser (OPENBGI_ASK_GAME_CHOOSER), shown when the engine starts
 * outside a game directory: a list of the games openbgi.json records, a
 * directory (typed, browsed, or taken from the list), the state of that
 * directory in a line of text, and under "Advanced" the engine profile
 * (one of `profiles`) and extra command-line options.  The engine owns
 * the description: the host's dialog calls `onSelect` when a list entry
 * is chosen and `onDirectory` when the directory changes (both update
 * `dir`, `status`, `launchable` and `profile`), and shows what they set.
 * Launch is taken only while `launchable` is set (else `refused` is
 * shown and the dialog stays).  Texts are the engine's (Shift-JIS) but
 * `dir`, which is a native path.  The engine makes the struct and a host
 * never does, so it has no `size`: members may come at its end. */
#define OPENBGI_GAME_CHOOSER_DIR_MAX     0x400
#define OPENBGI_GAME_CHOOSER_OPTIONS_MAX 0x200
typedef struct OpenBGI_GameChooser OpenBGI_GameChooser_t;
struct OpenBGI_GameChooser
{
	const char* const* names; // the list: the games' names
	int count;
	int selected;                                   // the list selection, -1 for none
	char dir[OPENBGI_GAME_CHOOSER_DIR_MAX];         // the directory (native)
	char status[0x200];                             // what the directory holds ("Heptagram (profile 1.553)", "Unknown game", ..)
	int launchable;                                 // 1 when the directory holds a game
	char profile[32];                               // the profile to run with (Advanced)
	char options[OPENBGI_GAME_CHOOSER_OPTIONS_MAX]; // extra options (Advanced)
	const char* const* profiles;                    // the profile names to choose from
	int profileCount;
	int advanced;        // 1 when the Advanced fields are shown (in and out)
	const char* refused; // the message shown when Launch is pressed without a game
	const char* title;   // the dialog's title (NULL: "Select game", in the engine's language)
	void (*onSelect)(OpenBGI_t* o, OpenBGI_GameChooser_t* l, int index);
	void (*onDirectory)(OpenBGI_t* o, OpenBGI_GameChooser_t* l);
	OpenBGI_t* o; // the engine the callbacks are called with
};

typedef struct OpenBGI_Request
{
	uint32_t size; // sizeof(OpenBGI_Request_t) as the host was compiled: the host sets it once
	uint32_t id;   // handed back with the answer
	int kind;      // OPENBGI_ASK_*
	union
	{
		struct
		{
			const char* text;
			const char* caption;
			uint32_t flags; // OPENBGI_MB_*: buttons, icon, default button
		} message;
		struct
		{
			const char* title;
			const char* initial;
			int maxLen; // characters, 0 no limit
			int digits; // digits only
			char* out;  // 0x100 bytes
		} input;
		struct
		{
			const char* title;
			const char* label[2];
			const char* initial[2];
			int maxLen[2];
			int digits[2];
			char* out[2]; // 0x100 bytes each
			int compact;  // the fields side by side
		} input2;
		struct
		{
			char* surname; // 0x100 bytes each: the initial texts in, the entries out
			char* givenName;
			char* nickname;
			char* pronoun;
			int32_t* month; // 0-based indices, in and out
			int32_t* day;
		} profile;
		struct
		{
			int save;   // 1 a save dialog, 0 an open one
			char* path; // the chosen path, `pathSize` bytes
			size_t pathSize;
			const char* filter;      // "80 3B": "<desc>(*.<ext>) *.<ext>"; NULL for the lists below
			const char* const* desc; // "81 38": `count` descriptions and their ";"-separated patterns
			const char* const* pattern;
			int count;
			const char* defExt; // the default extension, "" for none
			const char* initialDir;
			const char* title;
		} file;
		struct
		{
			char* path; // the chosen folder, `pathSize` bytes
			size_t pathSize;
			const char* title;
			const char* initial;
		} folder;
		struct
		{
			OpenBGI_GameChooser_t* chooser; // the dialog's description and results
		} chooser;
		struct
		{
			int dialog; // which of them: the engine's own number
			void* ctx;  // its description, the engine's own (not part of this interface)
		} installer;
	} u;
} OpenBGI_Request_t;

/* A new engine with every field at its starting value and the host's
 * services, of which it keeps copies of its own.  NULL when memory runs
 * out, when there is no host or none with the three clocks, when the
 * host was built for a version of this interface the library does not
 * serve (`api`, OPENBGI_API_VERSION above), or when the host struct or
 * one of its tables lacks its `size`.  OpenBGI_Delete frees it, stopping
 * a run that is still up first, and accepts NULL.  Engines are
 * independent of each other: a program may have several, one after
 * another or at once. */
OPENBGI_API OpenBGI_t* OpenBGI_New(const OpenBGI_Host_t* host);
OPENBGI_API void OpenBGI_Delete(OpenBGI_t* o);

/* Start the game the command line names (one string, as the original's
 * WinMain gets it; the engine strips its own "--" options and parses the
 * original's switches).  The tool options (--list-*, --extract=, --detect,
 * --version) run and finish here.  Otherwise the start goes on over the
 * first frames: the game chooser when no game is given, the
 * single-instance mutex, the processor and audio checks, the engine's
 * start-up and the boot.  OPENBGI_RUNNING when frames follow (also for a
 * run that already ended with a box still to be answered), OPENBGI_DONE
 * when the run already ended.
 *
 * An engine can be started again, for the same game or another: the
 * start then begins as a new engine's would, with nothing of the run
 * before (one that is still up is stopped first); only what the host
 * said of its devices stays - the pads that are there, the touch
 * screen's scale, its input settings (OpenBGI_InputSettings).  Not while
 * another thread reads the sound (OpenBGI_AudioRead). */
OPENBGI_API int OpenBGI_Start(OpenBGI_t* o, const char* cmdLine);

/* The launcher: a Buriko program that the engine boots when it is
 * started without a game - no boot path and no game where it runs - in
 * place of asking the host for one (OPENBGI_ASK_GAME_CHOOSER).  The host
 * hands over its sources, packed (tools/mkpack.py makes and describes
 * the pack; the first file is the program, for the newest engine
 * profile).  The engine writes them into the folder "launcher" of the
 * user's data folder, unless they are there, and compiles them there:
 * what is in that folder is what runs, so anyone can change it
 * (docs/dev/launcher.md says what happens when a newer pack meets changed
 * files, and when the files do not compile).  The compiled program is
 * kept in the folder and compiled again when a file it was made from
 * changed, or the engine did - which a pack may help it tell by a file
 * "build.id" of any content that changes with the host's build (it is
 * no source and is not written out).  The program lists the
 * games, lets the player choose and starts one with OpenBGI's own
 * instructions (docs/modding/obgi.md); the run then becomes that game's, in the
 * same window.  When the game ends the launcher runs again, started as
 * it was started first, and the frames go on (interface 1.1; before, they
 * ended with the game): they end when the launcher's program does, or
 * after OpenBGI_Quit.  A game named on the command line is followed by
 * no launcher.  The bytes are not copied
 * and have to stay where they are while the engine lives; NULL or a size
 * of 0 takes the launcher away again.  It is kept over a restart.  The
 * command line's --launcher=dialog asks the host as before for one
 * start. */
OPENBGI_API void OpenBGI_SetLauncher(OpenBGI_t* o, const void* sources, uint32_t size);

/* The names of the engine profiles built into the library, oldest first:
 * that of profile `i` (0 ..), NULL past the last.  For a host that lets
 * the player say which engine a game is run as (the command line's
 * --engine=NAME); openbgi.json may define more. */
OPENBGI_API const char* OpenBGI_ProfileName(int i);

/* Run one frame.  OPENBGI_DONE once the scripts ended, the window closed
 * or a script error stopped the machine (a reboot, scheduler code 5,
 * boots again within the run) and every request was answered; *f is
 * filled either way.  While a request is unanswered a frame does nothing. */
OPENBGI_API int OpenBGI_RunFrame(OpenBGI_t* o, OpenBGI_Frame_t* f);

/* Painting off (0) and on again (1).  While it is off the engine goes
 * through its frames as ever - the scripts run, the clock counts, a frame
 * says it presented - but composes nothing: the picture a frame hands over
 * stays what it was.  When it is on again the next frame that presents
 * paints the whole picture, and one is asked for at once.  For a host
 * that runs a game out of sight and wants one picture of it at the end
 * (the launcher's first pictures, below). */
OPENBGI_API void OpenBGI_SetDrawing(OpenBGI_t* o, int on);

/* The launcher's first pictures, and the games' icons.  The launcher's
 * program names every game it lists; a game without a picture from play
 * waits for a first one, and a game whose settings name no icon for that
 * of its own program.  A host that can run a second engine out of sight
 * takes the next game with OpenBGI_PreviewNext: 0 when none waits (or the
 * one taken before is still out), else what is wanted of it
 * (OPENBGI_PREVIEW_PICTURE, OPENBGI_PREVIEW_ICON, or both) and the
 * command line that runs it.  The host starts an engine of its own with
 * that command line, takes the icon from it when one is wanted
 * (OpenBGI_GameIcon, below) and hands it to OpenBGI_PreviewIcon - NULL
 * when the game has none -, which keeps it in the user's data and names
 * it in the game's settings.  For a picture it then runs the game for a
 * while and hands the picture of its last frame to OpenBGI_PreviewDone,
 * which keeps it small in the user's data, names it in the game's
 * settings and lets the program know.  A frame without a picture (its
 * pixels NULL, or all of one colour) says that the run showed nothing:
 * the game's settings note it, and the game is not handed out again for
 * one.  NULL says that the run was given up, or that no picture was
 * wanted: a game given up is handed out again another time the launcher
 * runs.  OpenBGI_PreviewDone ends every game taken, whatever was wanted.
 *
 * All three are called on this engine's thread, between frames, and
 * before OpenBGI_Stop; the second engine may run on another thread, with
 * services of its own.  This engine writes its settings before it hands
 * a game out and not again until the game is back, so that the second
 * engine reads the file whole.  The command line has --preview: the game
 * is laid over a folder for what it writes, as with --overlay, it says
 * nothing on stderr of what goes as expected, and its message boxes are
 * the host's to answer, whatever its other options say.  A host that
 * keeps that folder to itself leaves no trace of the run.  A host that
 * takes no game leaves the games without a first picture until they are
 * played, and without an icon.  With --preview=none on this engine's
 * command line, or "preview": "none" among the launcher's settings in
 * openbgi.json, no picture is wanted of any game. */
#define OPENBGI_PREVIEW_PICTURE 1
#define OPENBGI_PREVIEW_ICON    2
OPENBGI_API int OpenBGI_PreviewNext(OpenBGI_t* o, char* cmdLine, size_t n);
OPENBGI_API void OpenBGI_PreviewIcon(OpenBGI_t* o, const void* ico, size_t size);
OPENBGI_API void OpenBGI_PreviewDone(OpenBGI_t* o, const OpenBGI_Frame_t* picture);

/* The launcher's reading, beside the frames.  A host that says
 * OPENBGI_CAP_JOBS takes the pieces of work the launcher's program asks
 * for - a folder's listing, a look at a place for a game - with
 * OpenBGI_JobNext (NULL: none waits; several may be taken, in the order
 * given), does each with OpenBGI_JobRun and hands it back with
 * OpenBGI_JobDone; the thread of the program that asked waits meanwhile
 * and the program's other threads go on, so that a drive that takes its
 * time (a disk that spins up, a share) does not stop the picture.
 *
 * OpenBGI_JobNext, OpenBGI_JobDone and OpenBGI_JobBack are called on
 * this engine's thread, between frames; every job taken is handed back
 * before OpenBGI_Stop: with OpenBGI_JobDone when it was run (as soon as
 * it was, so that the program goes on), with OpenBGI_JobBack when the
 * host did not get to it - it waits again then, and what a thread of the
 * program waits for is handed out before the rest.  A job handed to
 * OpenBGI_JobDone without having been run counts as one that found
 * nothing.  OpenBGI_JobRun may be called on another
 * thread: it touches the job and the engine `worker` alone - an engine of
 * the host's own, made with OpenBGI_New and never started, which no
 * other thread uses meanwhile and whose host reads native files
 * (nativeStat, nativeList, nativeOpen to read) from any thread.  The
 * worker reads the first engine's openbgi.json for its tables; the first
 * engine writes that file before a job goes out and not while one is
 * out. */
typedef struct OpenBGI_Job OpenBGI_Job_t;
OPENBGI_API OpenBGI_Job_t* OpenBGI_JobNext(OpenBGI_t* o);
OPENBGI_API void OpenBGI_JobRun(OpenBGI_Job_t* job, OpenBGI_t* worker);
OPENBGI_API void OpenBGI_JobDone(OpenBGI_t* o, OpenBGI_Job_t* job);
OPENBGI_API void OpenBGI_JobBack(OpenBGI_t* o, OpenBGI_Job_t* job);

/* The icon of the game this engine runs: that of the game's own program
 * (the Windows executable in its folder that is the engine, not the
 * setup, installer or uninstaller beside it), as the bytes of an .ico
 * file, *size of them; NULL when the game has none.  Looked for at the
 * first call after OpenBGI_Start and kept until OpenBGI_Stop, which is
 * also how long the bytes are valid.  NULL while the launcher runs.
 *
 * OpenBGI_SetIcon hands the engine the host's own icon, an .ico file:
 * the window gets it (OpenBGI_Window_t.setIcon) while the launcher runs
 * and for a game without an icon.  Without one the window has OpenBGI's
 * own icon.  The bytes are not copied and have to stay where they are
 * while the engine lives; NULL takes the icon away again. */
OPENBGI_API const void* OpenBGI_GameIcon(OpenBGI_t* o, size_t* size);
OPENBGI_API void OpenBGI_SetIcon(OpenBGI_t* o, const void* ico, uint32_t size);

/* The next request the engine waits for that the host has not been
 * given yet, in the order they were made: 1 and *out, 0 when there is
 * none.  The host answers each with OpenBGI_Answer, between two frames. */
OPENBGI_API int OpenBGI_NextRequest(OpenBGI_t* o, OpenBGI_Request_t* out);
OPENBGI_API void OpenBGI_Answer(OpenBGI_t* o, uint32_t id, int result);

/* The end of the message pump between two frames, after the host handed
 * over its window's messages: the engine's own - a close it posted itself
 * ("80 69", Alt+F4: WM_CLOSE), the focus of the window nobody sees, and
 * the quit that ends the run once the window is gone (WM_QUIT) - and the
 * sound library's fade timer, served when there is no message left.  A
 * closed window stops the machine; the next frame says so. */
OPENBGI_API void OpenBGI_PumpEvents(OpenBGI_t* o);

/* The input, as the game's window gets it.  The host calls these on the
 * engine's thread, between frames, in the order things happened; a quit
 * or a script error they cause stops the machine (the next frame says
 * so).
 *
 * Positions are pixels of the frame's picture (OpenBGI_Frame_t: x to the
 * right, y down from its top left corner), however the host shows it; a
 * pointer outside the picture is outside its bounds.  OpenBGI_Pointer
 * says where the pointer is: the host calls it before each frame (the
 * original reads the system's pointer whenever it needs it) and may call
 * it whenever the pointer moves.  OpenBGI_PointerPlace is for a position
 * that changed although the player did not move the pointer - the first
 * one a host has to tell, the window having moved under a pointer at
 * rest: the engine takes the place as it takes OpenBGI_Pointer's, but not
 * the mouse as the device in use (which a pad's focus, and the dialogs
 * drawn for a pad, go by).  OpenBGI_Button carries the position of the
 * press or release as well.
 *
 * Keys are Windows virtual-key codes (VK_RETURN 0x0D, VK_SPACE 0x20, ...).
 * `flags` of OpenBGI_Key: OPENBGI_KEY_REPEAT for a press of the
 * keyboard's auto-repeat while the key is down, OPENBGI_KEY_SYSTEM for a
 * key with Alt held and for F10 (what Windows sends as WM_SYSKEYDOWN /
 * WM_SYSKEYUP), OPENBGI_KEY_TAKEN for a key a window of the host's own
 * took (its text entry): the engine notes it in the key state and does
 * nothing else with it.  Buttons are OPENBGI_BUTTON_* as the window reports them
 * (the primary button is the left one, whatever hand the system is set
 * for); `flags` of OpenBGI_Button: OPENBGI_CLICK_DOUBLE marks a press that
 * is the second of a double click (the host's system decides what is one).  The wheel turns
 * in WHEEL_DELTA units, 120 a notch, positive away from the player.
 * OpenBGI_Focus says that the window got (1) or lost (0) the keyboard
 * focus; with the loss every key and button counts as up. */
#define OPENBGI_KEY_REPEAT    1
#define OPENBGI_KEY_SYSTEM    2
#define OPENBGI_KEY_TAKEN     4
#define OPENBGI_BUTTON_LEFT   0
#define OPENBGI_BUTTON_RIGHT  1
#define OPENBGI_BUTTON_MIDDLE 2
#define OPENBGI_BUTTON_X1     3
#define OPENBGI_BUTTON_X2     4
#define OPENBGI_CLICK_DOUBLE  1
OPENBGI_API void OpenBGI_Key(OpenBGI_t* o, int vk, int down, int flags);
OPENBGI_API void OpenBGI_Pointer(OpenBGI_t* o, int x, int y);
OPENBGI_API void OpenBGI_PointerPlace(OpenBGI_t* o, int x, int y);
OPENBGI_API void OpenBGI_Button(OpenBGI_t* o, int button, int down, int x, int y, int flags);
OPENBGI_API void OpenBGI_Wheel(OpenBGI_t* o, int delta);
OPENBGI_API void OpenBGI_Focus(OpenBGI_t* o, int active);
/* Text typed, in UTF-8: what WM_CHAR delivers after the key press that
 * typed it, and what an input method composed.  Control characters are
 * ignored; text that comes while no frame says textInput is dropped. */
OPENBGI_API void OpenBGI_Text(OpenBGI_t* o, const char* utf8);

/* What a click reaches now: the targets of the game's mouse input, from
 * the engine's state after the last frame (calling this changes
 * nothing).  Up to `max` targets go to `out`, the topmost first; the
 * return value is how many there are (more than `max` when some did not
 * fit).  `size` is sizeof(OpenBGI_Target_t) as the host was compiled,
 * the place of each in `out`: a host of an older header gets what its
 * own targets hold of each.  Nothing is clickable before the game shows its window, nor
 * while a dialog the engine draws in the frame is up (the dialog takes
 * every click); where a child window it draws in the frame or its text
 * entry lies, the click is theirs and reaches no target.
 *
 * Each target says what it is (`kind`), where it is (`rect`, in the
 * frame's picture's pixels, cut to the picture) and a point inside it
 * where a click reaches it (`clickX`, `clickY`): the centre when that
 * does, else the reachable point nearest to it; -1, -1 when no point of
 * it does (OPENBGI_TARGET_COVERED: something above covers all of it, or
 * its hit mask is empty, as some games make a choice already taken).  A
 * target that a click anywhere reaches - the text being shown, a wait
 * that input ends, a layer of the script's own - is
 * OPENBGI_TARGET_ANYWHERE: its rect is the area of its layer, usually the
 * whole picture, and it is reached where nothing above it is.  `layer` is
 * the input layer key it is hit
 * through, by which the list is ordered (highest first); `owner`, `group`
 * and `index` say whose it is:
 *
 *   TEXT    a text being shown ("90 90" and its kin): a click hurries it
 *           or goes on; owner the waiting thread's id
 *   WAIT    a wait that a click ends early ("80 5C" with keys, a tween
 *           or a quake that may be skipped); owner the thread's id
 *   LAYER   an input layer of the script's ("80 18"); owner its priority,
 *           PROBED when the script polled it lately
 *   RAW     the script reads the mouse buttons itself ("80 11"): a click
 *           anywhere reaches it; layer 0, owner the button (1 left, 2 right)
 *   BUTTON  a button of a panel ("90 BA", "91 BA"); owner the panel's id,
 *           group and index the button's; SELECTED for its group's selection
 *   PANEL   beside a sprite panel's buttons, where the panel reports a
 *           click; owner the panel's id
 *   ITEM    an entry of a text menu ("90 A0", "90 A2"); owner the thread's
 *           id, index the entry, SELECTED for the menu's cursor
 *   ICON    an icon of an icon selection ("90 B0", "90 B1"); owner the
 *           thread's id, index the icon
 *   SCROLL  an arrow of a long message that scrolls (OpenBGI's "EF 08");
 *           owner the thread's id, index 0 up, 1 down
 *   KNOB    the thumb of a slider ("90 D0"); owner the knob's handle;
 *           `drag` where the thumb can be dragged; HELD while it is
 *   SPRITE  a sprite registered as a click target ("90 FA"); owner its
 *           handle, index its number
 *   PROBE   an object the script tested the pointer against lately
 *           ("90 3D"): what a click on it does is the script's; owner its
 *           handle
 *   ENTRY   the text entry the engine draws in the frame ("B0 20"): a
 *           click gives it the keys; owner 0
 *   HOTSPOT a place the game's settings name (openbgi.json, "hotspots"),
 *           where a script reads the mouse itself: listed while the
 *           screen has no other target with a place of its own that
 *           takes a click; index its number in the list, layer 0
 *
 * `group` and `index` are -1 where the kind has none, `drag` is `rect`
 * but for knobs.  DISABLED marks a target that is there but takes no
 * click now (a panel whose input is off or whose window lacks the input
 * focus, a button that cannot be decided, all of them while the game has
 * input off); HOVER one the pointer is on as things are. */
#define OPENBGI_TARGET_TEXT     1
#define OPENBGI_TARGET_WAIT     2
#define OPENBGI_TARGET_LAYER    3
#define OPENBGI_TARGET_RAW      4
#define OPENBGI_TARGET_BUTTON   5
#define OPENBGI_TARGET_PANEL    6
#define OPENBGI_TARGET_ITEM     7
#define OPENBGI_TARGET_ICON     8
#define OPENBGI_TARGET_SCROLL   9
#define OPENBGI_TARGET_KNOB     10
#define OPENBGI_TARGET_SPRITE   11
#define OPENBGI_TARGET_PROBE    12
#define OPENBGI_TARGET_ENTRY    13
#define OPENBGI_TARGET_HOTSPOT  14

#define OPENBGI_TARGET_ANYWHERE 0x01
#define OPENBGI_TARGET_DISABLED 0x02
#define OPENBGI_TARGET_COVERED  0x04
#define OPENBGI_TARGET_HOVER    0x08
#define OPENBGI_TARGET_SELECTED 0x10
#define OPENBGI_TARGET_HELD     0x20
#define OPENBGI_TARGET_PROBED   0x40
typedef struct OpenBGI_Target
{
	int kind;            // OPENBGI_TARGET_TEXT ...
	uint32_t flags;      // OPENBGI_TARGET_ANYWHERE ...
	OpenBGI_Rect_t rect; // the picture's pixels
	int clickX, clickY;  // where a click reaches it; -1, -1 nowhere
	OpenBGI_Rect_t drag; // a knob's travel; rect for the others
	uint32_t owner;      // a thread's id, a panel's id, an object's handle, a layer's priority
	int group, index;    // which of the owner's; -1 for none
	uint32_t layer;      // the input layer key
} OpenBGI_Target_t;
OPENBGI_API int OpenBGI_Targets(OpenBGI_t* o, OpenBGI_Target_t* out, int max, size_t size);

/* A touch screen.  The host reports each contact as it goes down, moves
 * and lifts (or is cancelled, as when the system takes the gesture), with
 * an id of its own that stays the same for the contact's life, at a
 * position in the picture's pixels as OpenBGI_Pointer has them, on the
 * engine's thread between frames.  The engine turns the gestures into
 * the mouse the game knows, a step a frame, so that every frame between
 * two steps runs a pass of the scripts (a menu that marks the button
 * under the pointer in one pass acts on the press in a later one):
 *
 *   tap               the pointer to the touch point, a pass, the left
 *                     button pressed, a pass, released.  A tap beside
 *                     every button (OpenBGI_Targets) lands on the
 *                     nearest one within the snap radius, before the
 *                     text or a layer a click anywhere reaches
 *   long press        the same with the right button, as soon as the
 *                     finger has stayed put for half a second (the
 *                     game's menu)
 *   two-finger tap    the right button at the middle of the two
 *   drag              the pointer follows the finger without a press, as
 *                     a mouse over the buttons; lifting does nothing.
 *                     One that starts on a knob's thumb, or beside it
 *                     within the snap radius with no other target under
 *                     the finger, drags the knob (the left button held)
 *   swipe             a quick vertical flick: one wheel notch, away from
 *                     the player for a swipe down (the backlog), towards
 *                     for a swipe up
 *   two-finger drag   wheel notches as the fingers move up and down
 *
 * More fingers than two are ignored until all have lifted.  While a
 * dialog the engine draws in the frame is up, the steps go to it, a step
 * a frame, and nothing snaps.  The distances (how far a tap may slip, the
 * snap radius, a swipe's length, the scroll step) are physical when the
 * host says how many of the picture's pixels make an inch on its screen
 * (OpenBGI_TouchScale; 0 forgets it), else fractions of the picture's
 * height, as if it filled a screen 75 mm high.  The pointer the gestures
 * move is reported in the frame as one the engine moved
 * (OpenBGI_Frame_t.pointerMoved). */
#define OPENBGI_TOUCH_DOWN   0
#define OPENBGI_TOUCH_MOVE   1
#define OPENBGI_TOUCH_UP     2
#define OPENBGI_TOUCH_CANCEL 3
OPENBGI_API void OpenBGI_Touch(OpenBGI_t* o, int id, int phase, int x, int y);
OPENBGI_API void OpenBGI_TouchScale(OpenBGI_t* o, int picturePixelsPerInch);

/* The surface (interface 1.1): what a host draws on, when the picture
 * does not fill it - a phone's screen, with bars beside the picture.  A
 * host that says so gets the engine's on-screen controls: a button that
 * turns them on and off, and a pad - a D-pad, a stick, and buttons to
 * confirm, cancel, skip and turn the backlog - that plays as a pad does
 * (OpenBGI_Pad, below).  They are drawn in the frame's layer
 * (OpenBGI_Frame_t.layer), placed by the surface's edges and sized in
 * millimetres, so they lie in the bars where those are wide enough.
 *
 * OpenBGI_SetSurface says the surface, on the engine's thread between
 * frames, at first and whenever something of it changes: its size in its
 * own pixels; `picture`, where the host shows the picture on it; `safe`,
 * the part of it that nothing covers (a camera's cutout, rounded
 * corners; a width of 0: all of it); and how many of its pixels make an
 * inch (0 when the host cannot say: 160 are taken).  NULL, or a size of
 * 0 x 0, takes the surface away again, and the layer with it.  The host
 * goes on placing the picture itself.
 *
 * OpenBGI_SurfaceTouch is OpenBGI_Touch for such a host: a contact in the
 * surface's pixels.  One that goes down on a control is the control's
 * until it lifts; any other is a touch on the picture, where the engine
 * finds it by `picture`, and the distances of the gestures are physical
 * without OpenBGI_TouchScale.  A host calls one of the two, not both. */
typedef struct OpenBGI_Surface
{
	uint32_t size;          // sizeof(OpenBGI_Surface_t) as the host was compiled
	int width, height;      // the surface, in its own pixels
	OpenBGI_Rect_t picture; // where the host shows the picture on it
	OpenBGI_Rect_t safe;    // the part of it that nothing covers; a width of 0: all of it
	int dpi;                // its pixels an inch; 0: not known
} OpenBGI_Surface_t;
OPENBGI_API void OpenBGI_SetSurface(OpenBGI_t* o, const OpenBGI_Surface_t* surface);
OPENBGI_API void OpenBGI_SurfaceTouch(OpenBGI_t* o, int id, int phase, int x, int y);

/* A pad.  The host hands over a pad's state when it changed (it may hand
 * it over every frame), on the engine's thread between frames: the
 * buttons that are down, the D-pad and the two sticks (with its `size`:
 * what a shorter state lacks is at rest); NULL for a pad that went away.  `port` is 0 .. 3; the pads count as one.  A button is
 * a bit of `buttons`, and the bit's number is the button's number as a
 * DirectInput pad of the common layout has it, which the games' own pad
 * keys go by: the four face buttons by where they sit, the shoulders, the
 * two in the middle, the stick clicks, the triggers as buttons; what else
 * a pad has goes into the bits above.  The sticks are -32768 .. 32767,
 * right and down positive.
 *
 * What the controls do unless bound otherwise (OpenBGI_Action says what
 * each of these means):
 *
 *   bottom face button    confirm        right face button, START   cancel
 *   D-pad                 the directions R2, held                   skip
 *   L1                    backlog        R1                         forward
 *   top face button       the on-screen keyboard
 *   left stick            the pointer: it moves while the stick is off
 *                         its centre, the faster the further
 *   right stick           the wheel, a notch at a time, up and down
 *
 * A game of 1.547 or later that gave a pad's controls keys itself
 * ("81 1B") has it its way, as in the original: a button or a direction
 * (the D-pad, or the left stick past a quarter of its travel) with a key
 * of the game's taps that key when it goes down, and only the controls
 * it gave none do the above. */
#define OPENBGI_PAD_SOUTH  0x0001u // the bottom face button (Xbox A, PlayStation Cross, Nintendo B): button 0
#define OPENBGI_PAD_EAST   0x0002u // the right one (Xbox B, Circle, Nintendo A)
#define OPENBGI_PAD_WEST   0x0004u // the left one (Xbox X, Square, Nintendo Y)
#define OPENBGI_PAD_NORTH  0x0008u // the top one (Xbox Y, Triangle, Nintendo X)
#define OPENBGI_PAD_L1     0x0010u
#define OPENBGI_PAD_R1     0x0020u
#define OPENBGI_PAD_SELECT 0x0040u // Back, Share, Minus
#define OPENBGI_PAD_START  0x0080u // Start, Options, Plus
#define OPENBGI_PAD_L3     0x0100u // the left stick pressed
#define OPENBGI_PAD_R3     0x0200u
#define OPENBGI_PAD_L2     0x0400u // the triggers, down past half their travel
#define OPENBGI_PAD_R2     0x0800u
#define OPENBGI_PAD_GUIDE  0x1000u
#define OPENBGI_DPAD_UP    1
#define OPENBGI_DPAD_DOWN  2
#define OPENBGI_DPAD_LEFT  4
#define OPENBGI_DPAD_RIGHT 8
typedef struct OpenBGI_PadState
{
	uint32_t size;          // sizeof(OpenBGI_PadState_t) as the host was compiled
	uint32_t buttons;       // OPENBGI_PAD_*, and the pad's further buttons above them
	uint32_t dpad;          // OPENBGI_DPAD_*
	int16_t leftX, leftY;   // the left stick
	int16_t rightX, rightY; // the right stick
} OpenBGI_PadState_t;
OPENBGI_API void OpenBGI_Pad(OpenBGI_t* o, int port, const OpenBGI_PadState_t* state);

/* What the player means, whatever said it: a pad's control (above), a
 * button a host draws on a touch screen, a remote control.  `down` is 1
 * when it begins and 0 when it ends; the engine's thread, between frames.
 *
 *   CONFIRM   With a focus (below): a click on the focused target.
 *             Without one: go on - a left click where the text being
 *             shown or a waiting script takes it (where the pointer is,
 *             if it does there); on a screen that has only targets the
 *             focus shows first, as for a direction; the Enter key when
 *             nothing takes a click.  While a stick moved the pointer
 *             last: the left button where the pointer is, held as long
 *             as this is
 *   CANCEL    the right button where the pointer is (the game's menu, or
 *             back), held as long as this is.  With a focus on a screen
 *             that also takes a click to go on, it only takes the focus
 *             away
 *   SKIP      the game's first skip key (Ctrl), held as long as this is
 *   BACKLOG   a wheel notch away from the player; FORWARD one towards
 *   UP, DOWN, LEFT, RIGHT
 *             The focus to the next target that way (OpenBGI_Targets:
 *             those a click reaches now, without the ones a click
 *             anywhere reaches); held, it repeats.  The first one only
 *             shows the focus: on the target under the pointer, else on
 *             the selection of a panel or a menu, else on the target
 *             nearest to the pointer.  The pointer is put on the focused
 *             target, so the game marks it as under a mouse, and the
 *             pointer is where the player sees the focus.  On a slider's
 *             knob the directions along its travel move the knob, a
 *             sixteenth of the way each.  When there is no such target:
 *             the arrow keys
 *   KEYBOARD  the engine's on-screen keyboard, shown or put away, while
 *             a text field has the focus
 *
 * The focus stays on its target while that is there, and follows it
 * when it moves.  When the target goes, the focus goes with it on a
 * screen that takes a click to go on (CONFIRM goes on again), and to the
 * nearest target otherwise.  A mouse that moves or clicks, a key or a
 * touch ends it: the device the player took up last has the turn (SKIP,
 * BACKLOG and FORWARD claim none).  A pointer position the host reports
 * within three pixels of where a pad put the pointer is not taken for a
 * mouse's.
 *
 * While a dialog the engine draws in the frame is up the actions work
 * the dialog: UP and DOWN step through the rows of a list or the values
 * of a field that picks one, and go to the control before or the next
 * one otherwise; LEFT and RIGHT go along a row of buttons, and from
 * control to control otherwise; BACKLOG and FORWARD always go from
 * control to control.  CONFIRM presses the focused button (the default
 * one from a list) or brings up the on-screen keyboard for the focused
 * field, CANCEL is the dialog's cancel.
 *
 * The on-screen keyboard is the engine's, drawn in the picture along its
 * bottom (along its top when the field lies there): a table of the fifty
 * sounds as hiragana or as katakana with the voiced and the small ones,
 * one of the full-width letters, digits and signs, and one of the
 * half-width ones, with keys below to change the table, for a space, to
 * move the caret, to delete and to put the keyboard away ("done", which
 * also gives the scripts' text entry up, as Enter does).  It types the
 * characters as they are: kana are not converted to kanji.  It comes up
 * when the text entry of the scripts gets the keys while a pad is in use
 * (or a touch screen, on a host without OPENBGI_CAP_KEYBOARD), with
 * CONFIRM on a text field, with KEYBOARD, and with a touch on a text
 * field when the host has no keyboard (the input setting
 * "onScreenKeyboard": false leaves only CONFIRM and KEYBOARD to bring
 * it up).  The directions move over its
 * keys, CONFIRM types, CANCEL deletes, BACKLOG and FORWARD move the
 * caret, SKIP turns to the next table; a touch or a click on a key types
 * it.  A key of a real keyboard puts it away.  While it is up the frames
 * say textInput 0.  It serves the fields the engine draws; a text entry
 * of the host's own (OpenBGI_Host_t.textEntry) is the host's to type
 * into. */
#define OPENBGI_ACTION_CONFIRM  1
#define OPENBGI_ACTION_CANCEL   2
#define OPENBGI_ACTION_SKIP     3
#define OPENBGI_ACTION_BACKLOG  4
#define OPENBGI_ACTION_FORWARD  5
#define OPENBGI_ACTION_UP       6
#define OPENBGI_ACTION_DOWN     7
#define OPENBGI_ACTION_LEFT     8
#define OPENBGI_ACTION_RIGHT    9
#define OPENBGI_ACTION_KEYBOARD 10
OPENBGI_API void OpenBGI_Action(OpenBGI_t* o, int action, int down);

/* Input settings of the host's own, laid over those of openbgi.json: the
 * text of an "input" object as that file has it (docs/dev/input.md; what a
 * pad's controls do, "ownPadKeys", "tapDelay", "pointer", "pointerSize",
 * "stickSpeed", "onScreenKeyboard", "hotspots"), as
 *
 *     OpenBGI_InputSettings(o, "{\"pad\": {\"west\": \"skip\"}, \"pointer\": \"always\"}");
 *
 * What the object names wins over the file's settings for every game and
 * over the game's own; what it does not name stays as they have it.  A
 * member "defaults" of it, an object of the same kind, is laid under the
 * file's settings instead: what holds on this host unless the file, or
 * the rest of the object, says otherwise - a frontend whose Enter key is
 * the pad's Start has Start confirm with
 * "defaults": {"pad": {"start": "confirm"}}.  It
 * holds from the call on - at any time between two frames, before a
 * start or while a game runs - until the next call, which replaces it,
 * and through OpenBGI_Stop and a new start; NULL or "" takes it away.
 * A host that offers such settings to the player (a frontend's options)
 * hands over only what the player chose.  It returns 1, or 0 when the
 * text is no JSON object (the settings stay as they were); a member that
 * is not understood is said on stderr and skipped. */
OPENBGI_API int OpenBGI_InputSettings(OpenBGI_t* o, const char* json);

/* The window's events: what the original's window procedure got besides
 * the input, from the host's pump in the order it got them (on the
 * engine's thread, between frames; a quit or a script error they cause
 * stops the machine, as with the input).
 *
 * OpenBGI_Close: WM_CLOSE, the close box or the system's close (Alt+F4
 * is the engine's, a key).  OpenBGI_Minimized: WM_SIZE, the window was
 * minimised (1) or restored (0) by the player or the system.
 * OpenBGI_WindowSize: WM_SIZE of a window with a sizing border
 * (`setResizable`): the client area is w x h pixels now.  The engine
 * takes it as the window's size (as "81 64" sets one) until the display
 * mode changes to full screen and back; the host shows the picture
 * scaled into it meanwhile.
 * OpenBGI_DisplayChanged: WM_DISPLAYCHANGE.  OpenBGI_DropFile: a file
 * or folder dropped on the window (WM_DROPFILES, after dragAccept(1)) or
 * the boot path a second instance hands over (the message 0x9000), by
 * its path as the host's native file calls take one (`nativeStat`); a
 * drop of several is a call for each.  A game's scripts are told the
 * path in their own form - the same on Windows, its Shift-JIS spelling
 * elsewhere - and the launcher lists what was dropped on it as a place
 * of games.  OpenBGI_WindowMessage: any other message of a host whose window
 * gets Windows messages, for the scripts that wait for one ("80 54").
 * OpenBGI_CdNotify: MM_MCINOTIFY of the CD audio the engine plays
 * (`status` MCI_NOTIFY_SUCCESSFUL 1 when a track played to its end).
 * OpenBGI_MovieEvent: the host's movie player had events (WM_APP of the
 * movie graph); `completed` when the movie reached its end.  OpenBGI_Quit:
 * the run is to end as the original's ends on WM_QUIT (a signal, the
 * page going away): the next OpenBGI_PumpEvents stops the machine.
 *
 * The debugger's window (OpenBGI_DebugWindow_t) hands back its input
 * through the rest: a key (a Windows virtual-key code, down or up, `ch`
 * the character it typed as a code point, 0 for none), the mouse (its
 * client pixels; `button` -1 a move, 0 left, 1 right, 2 middle, with
 * down or up), the wheel (WHEEL_DELTA units) and the close box. */
OPENBGI_API void OpenBGI_Close(OpenBGI_t* o);
OPENBGI_API void OpenBGI_Minimized(OpenBGI_t* o, int minimized);
OPENBGI_API void OpenBGI_WindowSize(OpenBGI_t* o, int w, int h);
OPENBGI_API void OpenBGI_DisplayChanged(OpenBGI_t* o);
/* the window's screen, its scale or work area, or the window's frame
 * changed (OpenBGI_Window_t.dpi, workArea, frame) without the display's
 * mode changing: the engine fits the window again; the scripts hear
 * nothing.  OpenBGI_DisplayChanged does as much besides. */
OPENBGI_API void OpenBGI_ScreenChanged(OpenBGI_t* o);
OPENBGI_API void OpenBGI_DropFile(OpenBGI_t* o, const char* path);
OPENBGI_API void OpenBGI_WindowMessage(OpenBGI_t* o, uint32_t msg, uint32_t wParam, uint32_t lParam);
OPENBGI_API void OpenBGI_CdNotify(OpenBGI_t* o, int status);
OPENBGI_API void OpenBGI_MovieEvent(OpenBGI_t* o, int completed);
OPENBGI_API void OpenBGI_Quit(OpenBGI_t* o);
OPENBGI_API void OpenBGI_DebugKey(OpenBGI_t* o, int vk, int down, int ch);
OPENBGI_API void OpenBGI_DebugMouse(OpenBGI_t* o, int x, int y, int button, int down);
OPENBGI_API void OpenBGI_DebugWheel(OpenBGI_t* o, int delta);
OPENBGI_API void OpenBGI_DebugClose(OpenBGI_t* o);

/* The sound: 16-bit stereo frames (left, then right) at
 * OPENBGI_AUDIO_RATE frames a second.  The engine mixes them on its own
 * thread - at the start and the end of every frame, and in
 * OpenBGI_RunAudio - for the time that passed on the host's clock, into
 * a ring of about 0.7 s.  OpenBGI_AudioRead takes them out of it: up to
 * `frames` frames into `out`, from any thread, one reader at a time
 * (typically the sound device's callback), from OpenBGI_New until
 * OpenBGI_Delete, which the reader must not outlive.  It returns how many
 * frames there were; the rest of `out` is silence.  It never waits.
 *
 * OpenBGI_SetAudioLead says how much the host's output needs mixed ahead
 * of what it read: at least the most it reads at once plus the longest
 * time it lets pass between two calls of the engine.  The engine keeps
 * between `ms` and 2 `ms` of sound in the ring and follows the pace of
 * the reads, so an output whose clock runs a little faster or slower
 * than the host's neither starves nor fills up; a host that waits longer
 * between two frames (a box of its own on the screen while it answers a
 * request) calls OpenBGI_RunAudio meanwhile.  At 0, the default, nobody plays the
 * sound: the engine mixes for the time that passed and drops what is not
 * read, so the fades, the loops and the scripts' "is it still playing"
 * keep their time.  A host that stops reading for a while sets 0 for that
 * time, or its sounds stand still.  The lead is the host's word about its
 * output and holds until it says another: OpenBGI_Stop and a new
 * OpenBGI_Start leave it as it is, and so does a game the launcher
 * starts in its own run's place.  OpenBGI_RunAudio and
 * OpenBGI_SetAudioLead are called on the engine's thread, between frames;
 * the lead is at most 300 ms.  While a request is unanswered,
 * OpenBGI_RunAudio also serves the sound library's fade timer, as the
 * original's modal boxes did. */
#define OPENBGI_AUDIO_RATE 44100
OPENBGI_API int OpenBGI_AudioRead(OpenBGI_t* o, int16_t* out, int frames);
OPENBGI_API void OpenBGI_RunAudio(OpenBGI_t* o);
OPENBGI_API void OpenBGI_SetAudioLead(OpenBGI_t* o, int ms);

/* Once OpenBGI_Start or OpenBGI_RunFrame returned OPENBGI_DONE, or at any
 * time between two frames to end a run that is still going: tear down
 * what OpenBGI_Start brought up.  Requests still unanswered are dropped
 * (the host must have closed their windows).  The program a script asked
 * to be started as it ends ("80 E1") is not started - the engine starts
 * none - and the box that says so is a request made after the teardown,
 * to be answered before OpenBGI_Delete or the next OpenBGI_Start.  A stopped engine holds no memory of its run - its
 * picture is gone as well, so the last frame's `pixels` must not be read
 * any more - and the host's sound output reads silence from it.  A
 * second stop does nothing.  Returns the process exit code: 0 for a game
 * run, a tool's result otherwise. */
OPENBGI_API int OpenBGI_Stop(OpenBGI_t* o);

#endif // OPENBGI_H_
