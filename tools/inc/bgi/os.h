/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os.h - the operating-system layer
 *
 * Everything the engine needs from the host lives behind this interface:
 * files and directories, time, threads and locks, the main window and its
 * input, the display surface, fonts, audio output, message boxes and the
 * handful of shell/registry services the "80 Fx" installer instructions use.
 *
 * Three back ends exist:
 *   src/os/win32/ (*.c)  - the original behaviour (GDI, waveOut, DirectShow, registry)
 *                          through the wide-character API: strings are converted
 *                          from and to Shift-JIS, whatever the system's code page
 *   src/os/posix/ (*.c)  - Linux: X11 window, FreeType fonts, ALSA, a registry file
 *   src/os/wasm/  (*.c)  - the browser: a canvas, the files over a websocket (docs/wasm.md)
 * plus src/os/os_common.c for code shared by all of them.
 *
 * Conventions:
 *   * Paths are passed as the engine builds them: Shift-JIS, with '\\' as
 *     separator.  The POSIX back end converts separators (and, optionally,
 *     the encoding) internally; nothing above this layer needs to care.
 *   * Functions that mirror a Win32 API keep its return convention so that
 *     callers that follow the original's code stay readable (e.g.
 *     OS_FileAttrs() returns OS_INVALID_ATTRS on failure like
 *     GetFileAttributes).  Unless
 *     a comment says otherwise, an `int` result is 1 for success and 0 for
 *     failure.
 *   * Handles are opaque pointers; OS_INVALID_FILE is the "closed" value.
 *   * Text the engine hands this layer (titles, labels, the characters to
 *     draw) is Shift-JIS unless OS_SetTextEncoding switched it to UTF-8.
 *   * The engine runs on one thread.  The audio fill callback of
 *     OS_AudioStart runs on the back end's audio thread, so the locks,
 *     the atomics and the clock (OS_TicksMs) must work from any thread;
 *     everything else is only called from the main thread.
 *
 * A port to another system implements every function below (the stubs of
 * src/os/posix/ show what a service without a counterpart may do: report
 * failure) and raises the OsEventHandlers_t callbacks from
 * OS_PumpMessages().  src/os/os_common.c holds the pieces that are the
 * same everywhere (wildcards, the Shift-JIS lead-byte test, the registry
 * file format, the UTF-8 helpers).
 */
#ifndef BGI_OS_H_
#define BGI_OS_H_

#include "bgi/common.h"

// -------------------------------------------------------------------------
// Process / start-up
// -------------------------------------------------------------------------
/* Once, before anything else: the back end sets up what its other services
 * need (character converters, the configuration directory, COM on Win32);
 * 1 on success.  OS_Shutdown undoes it at the end of the run, after the
 * window and the audio output are gone. */
int OS_Init(void);
void OS_Shutdown(void);

/* The game directory as a Windows path with a trailing separator: what
 * the original's GetModuleFileName gives without the file name, the
 * executable living in the game's folder.  It is the engine's base
 * directory: every relative path the scripts use is resolved under it,
 * and the drive checks key on its drive letter.  The POSIX back end
 * presents it as "C:\" and maps that to a directory of the host: the
 * current directory at start-up, or what OS_SetGameDir chose. */
void OS_ExeDir(char* buf, size_t n);
/* The reimplementation's own files - games.json, the launcher's record of
 * the games - live beside the binary and in directories the user picks,
 * which are paths of the host rather than of the game: "native" paths,
 * in the host's own form (UTF-8 on POSIX; on Windows the same Shift-JIS
 * Windows paths the engine uses, which the back end converts to UTF-16 -
 * a directory code page 932 cannot spell appears under a substitute
 * spelling the back end maps back, see src/os/win32/wide.c).  OS_BinaryDir
 * is the directory of the running executable (a trailing separator
 * included); OS_SetGameDir makes a native directory the game directory
 * (OS_ExeDir from then on, and the current directory): 1 when it exists.
 * The two converters turn a native string into UTF-8 and back (the
 * identity on POSIX).  OS_NativeOpen is fopen for a native path;
 * OS_ReadNativeFile reads a whole file into a malloc'ed buffer with a NUL
 * after the data (NULL when it cannot be read); OS_WriteNativeFile
 * creates or replaces one (1 when written). */
void OS_BinaryDir(char* buf, size_t n);
void OS_SetArgv0(const char* argv0);  // the program name of main(), before OS_Init (OS_BinaryDir's fallback; a no-op where unneeded)
void OS_GameDir(char* buf, size_t n); // the game directory as a native path with a trailing separator
int OS_SetGameDir(const char* dir);
void OS_NativeToUtf8(const char* native, char* out, size_t n);
void OS_Utf8ToNative(const char* utf8, char* out, size_t n);
FILE* OS_NativeOpen(const char* path, const char* mode);
uint8_t* OS_ReadNativeFile(const char* path, uint32_t* size);
int OS_WriteNativeFile(const char* path, const void* data, uint32_t size);
/* More of the native file system, for the command-line tools that work on
 * directories of the host (bgidump, bgipack; the engine itself has no use
 * for it): OS_NativeStat tells what a path is; OS_NativeMkdir creates a
 * directory (1 when it exists afterwards; the parent has to exist);
 * OS_NativeRemove deletes a file (1 when it is gone); OS_NativeList calls
 * `fn` for every entry of a directory but "." and ".." - the name alone, in
 * no particular order - and returns the number of entries, -1 when the
 * directory cannot be read; OS_NativeSeek positions a stream opened with
 * OS_NativeOpen at a 64-bit offset from its start (1 on success), which
 * fseek cannot do where long is 32 bits wide; OS_NativeAbsolute gives the
 * full path of a path that may be relative to the current directory (the
 * path itself when the system cannot say).  OS_NativeArgs replaces
 * main's arguments by native strings: a no-op on POSIX; on Windows they
 * are taken again from the wide command line, so that a path the system's
 * code page cannot spell still arrives intact.  OS_NATIVE_SEP is the
 * separator to build native paths with. */
#define OS_NATIVE_NONE 0 // OS_NativeStat: nothing of that name
#define OS_NATIVE_FILE 1
#define OS_NATIVE_DIR  2
typedef struct OsNativeStat
{
	uint64_t size; // a file's size, bytes
	int64_t mtime; // its last modification, milliseconds since 1970-01-01 UTC
} OsNativeStat_t;
typedef void (*OsNativeListFn)(void* ctx, const char* name, int isDir);
int OS_NativeStat(const char* path, OsNativeStat_t* out); // OS_NATIVE_*; `out` may be NULL
int OS_NativeMkdir(const char* path);
int OS_NativeRemove(const char* path);
int OS_NativeList(const char* dir, OsNativeListFn fn, void* ctx);
int OS_NativeSeek(FILE* f, uint64_t pos);
void OS_NativeAbsolute(const char* path, char* out, size_t n);
/* Run fn(0, ctx) .. fn(n - 1, ctx) and return how many of them returned
 * non-zero: in n child processes at once where the system can fork (each
 * child has the parent's memory as it was and its own afterwards, so the
 * functions hand their results back through files), one after the other in
 * this process elsewhere.  OS_WorkerLimit is the n worth asking for: the
 * number of processors where the workers run in parallel, 1 where they do
 * not. */
int OS_WorkerLimit(void);
int OS_RunWorkers(int n, int (*fn)(int index, void* ctx), void* ctx);
void OS_NativeArgs(int* argc, char*** argv);
#if defined(_WIN32)
#define OS_NATIVE_SEP '\\'
#else
#define OS_NATIVE_SEP '/'
#endif
/* Shift-JIS (CP932) to UTF-8, for the reimplementation's own text files;
 * `out` (n bytes) receives a NUL-terminated string, the result is its
 * length.  The back end supplies the code page 932 table (iconv,
 * MultiByteToWideChar); OsCommon_SjisToUtf8 does the rest. */
size_t OS_SjisToUtf8(const char* sjis, char* out, size_t n);
// the Unicode scalar of a two-byte Shift-JIS code (the code page 932 table); U+FFFD when unmapped
uint32_t OS_SjisToCodePoint(uint16_t sjis);
// UTF-8 to Shift-JIS ("81 20" of 1.653 on); a character without a code becomes '?'; the length written
size_t OS_Utf8ToSjis(const char* utf8, char* out, size_t n);
/* "81 00" of 1.653 on: the script text is UTF-8 (1) rather than Shift-JIS
 * (0).  Text the engine hands this layer - window titles, message boxes,
 * the characters OS_FontRender draws - is then in that encoding; the
 * engine's own messages stay Shift-JIS, so the back ends take each
 * well-formed UTF-8 sequence as it is and convert the rest. */
void OS_SetTextEncoding(int utf8);
int OS_SetCurrentDir(const char* dir); // SetCurrentDirectory; 1 on success

/* The engine refuses to start twice: a named mutex (CreateMutex) guards
 * the process.  *alreadyExists is set to 1 when another instance holds
 * the name; the handle is returned either way and given back with
 * OS_InstanceMutexRelease, which also accepts NULL. */
typedef struct OsMutex OsMutex_t;
OsMutex_t* OS_InstanceMutexCreate(const char* name, int* alreadyExists);
void OS_InstanceMutexRelease(OsMutex_t* m);

// -------------------------------------------------------------------------
// Files
// -------------------------------------------------------------------------
/* A file handle (CreateFile).  Files are at most 4 GB: sizes and positions
 * are 32-bit, as in the original. */
typedef struct OsFile OsFile_t;
#define OS_INVALID_FILE ((OsFile_t*)0)

OsFile_t* OS_FileOpenRead(const char* path);                     // GENERIC_READ, OPEN_EXISTING; NULL on failure
OsFile_t* OS_FileCreate(const char* path);                       // GENERIC_WRITE, CREATE_ALWAYS (truncates); NULL on failure
void OS_FileClose(OsFile_t* f);                                  // accepts NULL
uint32_t OS_FileRead(OsFile_t* f, void* buf, uint32_t n);        // bytes read, 0 on error or at the end
uint32_t OS_FileWrite(OsFile_t* f, const void* buf, uint32_t n); // bytes written
int OS_FileSeek(OsFile_t* f, uint32_t pos);                      // from the start; 1 = ok
uint32_t OS_FileSize(OsFile_t* f);                               // bytes
// the last-write time as a 64-bit FILETIME (100 ns units since 1601) in two dwords (lo, hi)
int OS_FileGetTime(OsFile_t* f, uint32_t* lo, uint32_t* hi);
int OS_FileSetTime(OsFile_t* f, uint32_t lo, uint32_t hi);

// the FILE_ATTRIBUTE_* bits the engine looks at
#define OS_INVALID_ATTRS  0xFFFFFFFFu // the file does not exist (INVALID_FILE_ATTRIBUTES)
#define OS_ATTR_READONLY  0x01u
#define OS_ATTR_HIDDEN    0x02u
#define OS_ATTR_SYSTEM    0x04u
#define OS_ATTR_DIRECTORY 0x10u
#define OS_ATTR_ARCHIVE   0x20u
#define OS_ATTR_NORMAL    0x80u                                      // no other attribute is set
uint32_t OS_FileAttrs(const char* path);                             // GetFileAttributes: the bits above, OS_INVALID_ATTRS on failure
int OS_FileSetAttrs(const char* path, uint32_t attrs);               // SetFileAttributes (the read-only bit at least)
int OS_FileDelete(const char* path);                                 // DeleteFile
int OS_FileMove(const char* from, const char* to);                   // MoveFile
int OS_FileCopy(const char* from, const char* to, int failIfExists); // CopyFile: 0 when `to` exists and failIfExists is set
int OS_DirCreate(const char* path);                                  // CreateDirectory: 0 when it exists already
int OS_DirRemove(const char* path);                                  // RemoveDirectory: the directory must be empty

/* Directory enumeration (FindFirstFile / FindNextFile): `pattern` is a
 * path whose file part may hold the wildcards '*' and '?'
 * (OsCommon_WildcardMatch says how they match); the directory part is
 * taken as it is.  Each match fills an OsFindData_t. */
typedef struct OsFind OsFind_t;
typedef struct OsFindData
{
	uint32_t attrs; // OS_ATTR_* bits of the entry
	char name[260]; // the file name alone (no directory), NUL-terminated
} OsFindData_t;
OsFind_t* OS_FindFirst(const char* pattern, OsFindData_t* out); // the first match; NULL = none (nothing to close)
int OS_FindNext(OsFind_t* h, OsFindData_t* out);                // the next match; 0 = done
void OS_FindClose(OsFind_t* h);                                 // accepts NULL

/* Drives (src/core/paths.c, the installer's CD search).  The engine
 * enumerates them to find the CD-ROM that holds the game data and to
 * check that the drive of its own directory is ready.  On POSIX only a
 * single pseudo drive "C:" exists and is "fixed". */
#define OS_DRIVE_UNKNOWN   0 // the GetDriveType values
#define OS_DRIVE_NO_ROOT   1
#define OS_DRIVE_REMOVABLE 2
#define OS_DRIVE_FIXED     3
#define OS_DRIVE_REMOTE    4
#define OS_DRIVE_CDROM     5
#define OS_DRIVE_RAMDISK   6
uint32_t OS_DriveType(const char* root); // GetDriveType of a root such as "A:\"
// GetLogicalDriveStrings: the drive roots ("A:\") NUL-separated and double-NUL terminated; the length without the final NUL
uint32_t OS_LogicalDrives(char* buf, uint32_t n);
// 1 when a volume is mounted and readable at root ("X:\"): GetVolumeInformation succeeds
int OS_VolumeReady(const char* root);
// 1 when the removable drive holds a medium (IOCTL_STORAGE_CHECK_VERIFY on NT); the drive letter alone, 'A' ..
int OS_MediaPresent(char driveLetter);

// -------------------------------------------------------------------------
// Time
// -------------------------------------------------------------------------
// milliseconds since an arbitrary start, wrapping at 2^32 (timeGetTime / GetTickCount); any thread
uint32_t OS_TicksMs(void);
void OS_SleepMs(uint32_t ms); // Sleep
/* timeBeginPeriod(the device's minimum) / timeEndPeriod around the whole
 * run: the engine asks for the finest timer resolution the system has */
void OS_TimerBeginPeriod(void);
void OS_TimerEndPeriod(void);
typedef struct OsLocalTime
{
	uint16_t year, month, dayOfWeek, day, hour, minute, second, millis; // GetLocalTime: month 1..12, dayOfWeek 0 = Sunday
} OsLocalTime_t;
void OS_LocalTime(OsLocalTime_t* t);
/* The main loop's idle step: a sleep of about half a millisecond that
 * gives the system a chance to run without losing a frame (the original
 * waits on a waitable timer; Sleep(1) stands in on Windows 9x).  OS_IdleInit
 * creates whatever the sleep needs, OS_IdleShutdown frees it. */
void OS_IdleInit(void);
void OS_IdleShutdown(void);
void OS_IdleSleep(void);
/* The 1.535+ variant with a duration: 1 ms means the same half-millisecond
 * timer wait as OS_IdleSleep; any other value sets the timer to that many
 * ms but waits at most 10 ms for it (so a longer idle sleep is capped at
 * 10 ms on the timer path). */
void OS_IdleSleepMs(int ms);

// -------------------------------------------------------------------------
// Threads and locks
// -------------------------------------------------------------------------
// a CRITICAL_SECTION: recursive, the thread that holds it may enter again
typedef struct OsLock OsLock_t;
OsLock_t* OS_LockCreate(void);
void OS_LockDestroy(OsLock_t* l); // accepts NULL
void OS_LockEnter(OsLock_t* l);
void OS_LockLeave(OsLock_t* l);

// a thread running fn(arg) until it returns (CreateThread)
typedef struct OsThread OsThread_t;
typedef void (*OsThreadFn_t)(void* arg);
#define OS_PRIO_NORMAL  0                                             // THREAD_PRIORITY_NORMAL
#define OS_PRIO_HIGHEST 2                                             // THREAD_PRIORITY_HIGHEST
OsThread_t* OS_ThreadStart(OsThreadFn_t fn, void* arg, int priority); // NULL when the thread could not be created
void OS_ThreadJoin(OsThread_t* t);                                    // wait for the thread, free it; accepts NULL
int OS_CpuCount(void);                                                // processors online, at least 1
int32_t OS_AtomicInc(volatile int32_t* p);                            // InterlockedIncrement: the new value
int32_t OS_AtomicDec(volatile int32_t* p);                            // InterlockedDecrement: the new value

// -------------------------------------------------------------------------
// Memory status, machine identity
// -------------------------------------------------------------------------
// GlobalMemoryStatus: the memory in use as a percentage, the rest in bytes (capped at 4 GB)
typedef struct OsMemStatus
{
	uint32_t loadPercent, totalPhys, availPhys, totalPageFile, availPageFile,
		totalVirtual, availVirtual;
} OsMemStatus_t;
void OS_MemoryStatus(OsMemStatus_t* m);   // "80 0D" reports totalPhys and availPhys
int OS_ComputerName(char* buf, size_t n); // GetComputerName into buf (n bytes); the installer's machine key ("80 EE", "80 EF")
int OS_UserName(char* buf, size_t n);     // GetUserName
/* CPUID and the time stamp counter for src/sys/cpu.c (the original reads
 * them inline).  OS_Cpuid returns 0 when the processor has no CPUID
 * instruction (the EFLAGS.ID test) or the leaf is above the highest one
 * supported; a platform without x86 CPUID returns 0 from both. */
int OS_Cpuid(uint32_t leaf, uint32_t out[4]); // eax, ebx, ecx, edx
uint64_t OS_ReadTsc(void);                    // RDTSC (after a serialising CPUID), 0 if unavailable
/* QueryPerformanceCounter / QueryPerformanceFrequency ("80 05" of 1.69
 * build 472 on): the counter in nanoseconds; 0 when there is no counter */
int OS_PerfCounterNs(int64_t* out);

// -------------------------------------------------------------------------
// Window, display, input
// -------------------------------------------------------------------------
/* The engine owns one main window of 800x600 client pixels (WS_POPUP |
 * WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX).  It can be windowed or full
 * screen, and is created hidden; the event handlers must be installed
 * (OS_SetEventHandlers) before it is created, because `created` is raised
 * from OS_WindowCreate.  The title is shown as OsCommon_MainTitle spells
 * it. */
typedef struct OsWindowDesc
{
	const char* title; // the game's title (Shift-JIS)
	int x, y;          // window position (outer rectangle, screen pixels)
	int width, height; // client size in pixels
} OsWindowDesc_t;
int OS_WindowCreate(const OsWindowDesc_t* d);          // 1 ok; raises `created`
void OS_WindowDestroy(void);                           // DestroyWindow; raises `destroyed` before it returns
int OS_WindowAlive(void);                              // 1 between OS_WindowCreate and OS_WindowDestroy
void OS_WindowShow(int show);                          // ShowWindow(SW_SHOW / SW_HIDE) + UpdateWindow ("80 64")
void OS_WindowSetTitle(const char* t);                 // SetWindowText with OsCommon_MainTitle applied ("80 66"); NULL clears the game's part
void OS_WindowMove(int x, int y);                      // outer rectangle origin, screen pixels
void OS_WindowGetRect(int* x, int* y, int* w, int* h); // GetWindowRect: outer rectangle, screen coords
int OS_WindowMinimized(void);                          // IsIconic
int OS_WindowActive(void);                             // has the focus (GetActiveWindow / WM_ACTIVATE)
void OS_WindowMinimize(void);                          // ShowWindow(SW_MINIMIZE) ("80 65")
void OS_WindowClose(void);                             // like posting WM_CLOSE: `close_request` from the next pump
void OS_ScreenSize(int* w, int* h);                    // the desktop in pixels (GetSystemMetrics SM_CXSCREEN / SM_CYSCREEN)
/* The window frame: fw / fh are the total width / height added to the
 * client area; OS_FrameTop() is the caption plus the top border (0 on
 * platforms where the engine does not manage the frame).  The engine
 * uses them to place the window so that the client area lands where the
 * scripts asked. */
void OS_FrameMetrics(int* fw, int* fh);
int OS_FrameTop(void);
// show / hide the IME status window (WM_IME_CONTROL IMC_OPENSTATUSWINDOW / IMC_CLOSESTATUSWINDOW): hidden while the game runs
void OS_ImeStatus(int open);

/* Display modes (src/sys/display.c).  Full screen is a borderless window
 * covering a display mode of w x h pixels (the OS layer may scale instead
 * of switching the mode); windowed mode restores the desktop and sizes the
 * client area.  The engine first switches the mode, then restyles the
 * window: OS_DisplaySetMode(1, ..) (trying the depths in turn) and
 * OS_WindowSetFullscreen for full screen, OS_DisplayRestore and
 * OS_WindowSetWindowed back; it then re-applies the cursor visibility
 * (OS_CursorShow), since a mode switch resets it on Windows. */
int OS_DisplaySetMode(int fullscreen, int w, int h, int bpp);      // ChangeDisplaySettings to w x h at bpp bits, or back to the desktop; 1 ok
void OS_DisplayRestore(void);                                      // the desktop mode again (ChangeDisplaySettings(NULL, 0))
void OS_WindowSetFullscreen(int modeW, int modeH);                 // the window borderless over the mode
void OS_WindowSetWindowed(int x, int y, int clientW, int clientH); // the framed window at the outer origin (x, y)
/* The picture the engine draws and addresses is w x h ("81 64" of 1.529
 * on lets the scripts size the window independently of it).  Called after
 * OS_WindowSetWindowed; when the client area is of another size the back
 * end shows the picture scaled into it, centred with its aspect kept, and
 * maps the mouse the other way, so the engine keeps working in picture
 * coordinates.  A client area of the picture's size is shown 1:1. */
void OS_WindowSetPicture(int w, int h);

/* The back buffer is plain memory owned by the graphics manager (the
 * original uses a top-down DIB section; see Gfx_SetupScreen).  An OsSurface
 * describes such a buffer so the OS layer can copy parts of it to the
 * window.  `pixels` is the first visible row, `pitch` the byte distance
 * between rows, `bpp` 16 or 32. */
typedef struct OsSurface
{
	void* pixels;      // top-left visible pixel
	int pitch;         // bytes per row
	int width, height; // visible size in pixels
	int bpp;           // 16 (RGB555) or 32 (0x00RRGGBB)
} OsSurface_t;

/* A device context handle as the window system hands it out (HDC on Win32;
 * the POSIX back end hands out a token naming the window).  NULL means
 * "the main window".  The engine takes one for a present and releases it
 * right after; a child window's comes from OS_ChildGetDc. */
typedef void OsDc_t;
OsDc_t* OS_WindowGetDc(void); // GetDC(hWnd); release when done; NULL without a window
void OS_WindowReleaseDc(OsDc_t* dc);
// BitBlt(dc, dstX, dstY, w, h, src, srcX, srcY, SRCCOPY): w x h pixels of the surface to the window, unscaled
void OS_BlitToWindow(OsDc_t* dc, int dstX, int dstY, int w, int h,
	const OsSurface_t* src, int srcX, int srcY);
/* StretchDIBits(dc, dstX, dstY, dstW, dstH, 0, 0, src->width, src->height,
 * .., SRCCOPY) of a whole surface (Window_DcBlit).  With `clip`
 * ({left, top, right, bottom}, right / bottom exclusive) the output is
 * clipped to that rectangle and stretched in HALFTONE mode; NULL leaves
 * the context as it is. */
void OS_StretchToWindow(OsDc_t* dc, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* src,
	const int* clip);
void OS_WindowFillBlack(void); // the whole client area black (the `paint` handler before the engine is up)

/* Events are delivered to the engine's callbacks (src/sys/window.c) from
 * OS_PumpMessages(), on the main thread, one message per call.  Each entry
 * stands for the window message it is named after; a back end raises the
 * ones it can and leaves the rest alone (every pointer may be NULL and is
 * checked before the call).  Keys are Windows virtual-key codes, mouse
 * positions client pixels (in full screen: pixels of the display mode). */
typedef struct OsEventHandlers
{
	void (*key_down)(int vk, int repeat);                                // WM_KEYDOWN; repeat = the key was already down (auto-repeat)
	void (*key_up)(int vk);                                              // WM_KEYUP
	void (*mouse_move)(int x, int y);                                    // WM_MOUSEMOVE (the engine polls the cursor; the debugger uses this)
	void (*mouse_button)(int button, int down, int x, int y);            // WM_?BUTTONDOWN / UP: 0 L 1 R 2 M 3 X1 4 X2
	void (*mouse_wheel)(int delta);                                      // WM_MOUSEWHEEL: WHEEL_DELTA (120) units, + = away from the user
	void (*activate)(int active);                                        // WM_ACTIVATE: the window gained (1) or lost (0) the focus
	void (*close_request)(void);                                         // WM_CLOSE (the close box, Alt+F4, OS_WindowClose); the engine decides
	void (*destroyed)(void);                                             // WM_DESTROY
	void (*paint)(void);                                                 // WM_PAINT: repaint the whole window
	void (*drop_file)(const char* path);                                 // WM_DROPFILES (after OS_DragAccept(1)): one file's path
	void (*ipc_string)(const char* s);                                   // second-instance boot path (0x9000)
	void (*child_paint)(int index, OsDc_t* dc);                          // a child window's WM_PAINT: present its back buffer
	void (*raw_message)(uint32_t msg, uint32_t wParam, uint32_t lParam); // every other message, for the "80 54" message waits
	void (*syskey)(int vk);                                              // Alt+key and F10 (WM_SYSKEYDOWN)
	void (*quit)(void);                                                  // WM_QUIT (OS_PostQuit): the engine throws out of its loop
	void (*created)(void);                                               // WM_CREATE
	void (*sized)(int minimized);                                        // WM_SIZE: minimised (1) or restored (0)
	void (*double_click)(int button);                                    // WM_?BUTTONDBLCLK, raised before the press itself
	void (*display_change)(void);                                        // WM_DISPLAYCHANGE
	void (*mci_notify)(int status);                                      // MM_MCINOTIFY (1 = successful)
	void (*child_destroyed)(int index);                                  // a child window's WM_DESTROY
	void (*edit_paint)(void);                                            // the text entry's WM_PAINT (before the control draws)
	void (*movie_event)(int completed);                                  // WM_APP from the movie graph (see OS_MovieOpen)
	// the debugger's window (OS_DebugWindowOpen): its own keys, mouse and close box
	void (*debug_key)(int vk, int down, int ch);             // ch: the character typed (UTF-8 code point), 0 for none
	void (*debug_mouse)(int x, int y, int button, int down); // button -1: a move; 0 L 1 R 2 M
	void (*debug_wheel)(int delta);                          // WHEEL_DELTA units
	void (*debug_close)(void);
} OsEventHandlers_t;
void OS_SetEventHandlers(const OsEventHandlers_t* h); // copied; before OS_WindowCreate
int OS_PumpMessages(void);                            // PeekMessage + dispatch of one message; 1 if there was one
void OS_PostQuit(void);                               // PostQuitMessage: `quit` from a later pump
void OS_DragAccept(int accept);                       // DragAcceptFiles: whether `drop_file` is raised

/* Cursor.  "80 67" selects a shape 0..4 from the original's table of five
 * HCURSORs: 0 the arrow, 1 the resource cursor 0x6A, 2..4 are
 * NULL entries that leave the class cursor (the arrow) in place.  The
 * shape is applied when the system asks (WM_SETCURSOR); OS_CURSOR_NONE is
 * the back end's own "no cursor" value, never selected by scripts. */
#define OS_CURSOR_ARROW  0
#define OS_CURSOR_CUSTOM 1
#define OS_CURSOR_NONE   -1
void OS_CursorSetShape(int shape);
void OS_CursorShow(int show);         // ShowCursor counter semantics
void OS_CursorGetPos(int* x, int* y); // screen coordinates
void OS_CursorSetPos(int x, int y);
void OS_ScreenToClient(int* x, int* y);
void OS_ClientToScreen(int* x, int* y);
int OS_MouseButtonsSwapped(void);        // SwapMouseButton system setting
int OS_KeyState(int vk);                 // GetAsyncKeyState & 0x8000
void OS_KeyboardState(uint8_t out[256]); // GetKeyboardState ("81 11" of 1.69/472 on): bit 7 = down

/* Child windows ("B0 1x", src/sys/child.c): up to 8 pop-up windows owned
 * by the main window (WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
 * class "BGI - Child window").  The engine keeps a back buffer per window
 * and presents it from the `child_paint` event; `child_destroyed` tells it
 * the window is gone so it frees the buffer.  The window procedure refuses
 * WM_CLOSE unless the engine is closing the window itself and forwards key
 * presses (WM_KEYDOWN / WM_KEYUP) to the main window. */
typedef void OsChild_t;
// created hidden; (x, y) is the outer origin in screen coordinates, outerW x outerH the outer size
OsChild_t* OS_ChildCreate(int index, const char* title, int x, int y, int outerW, int outerH);
void OS_ChildClose(OsChild_t* c);                                      // SendMessage(WM_CLOSE): destroys it (child_destroyed follows)
void OS_ChildShow(OsChild_t* c, int show);                             // ShowWindow(SW_SHOWNA / SW_HIDE)
void OS_ChildSetTitle(OsChild_t* c, const char* title);                // SetWindowText
void OS_ChildMove(OsChild_t* c, int x, int y, int outerW, int outerH); // MoveWindow(.., repaint)
void OS_ChildGetPos(OsChild_t* c, int* x, int* y);                     // GetWindowRect: outer origin, screen coordinates
OsDc_t* OS_ChildGetDc(OsChild_t* c);                                   // GetDC; release when done
void OS_ChildReleaseDc(OsChild_t* c, OsDc_t* dc);

/* The text entry ("B0 2x", src/sys/edit.c): one single-line EDIT control
 * (ES_CENTER | ES_AUTOHSCROLL | ES_NOHIDESEL) over the main window, created
 * hidden and 1 x 1 with the text MSG_EDIT_DEFAULT_TEXT together with the
 * window, and subclassed by the original: Tab and Enter select its text
 * and give the focus back to the main window, and WM_PAINT first raises
 * `edit_paint` so the engine can present the back buffer under it - the
 * control draws with a transparent background (WM_CTLCOLOREDIT:
 * NULL_BRUSH, TRANSPARENT, the colour of OS_EditSetColour).  A back end
 * without an edit control draws the field itself (src/os/posix/edit.c). */
void OS_EditShow(int show);                   // ShowWindow(SW_SHOW / SW_HIDE) + WM_SETFONT (the font when shown, none when hidden)
void OS_EditMove(int x, int y, int w, int h); // MoveWindow in client coordinates, repaint
/* replace the font: CreateFontA(size, size / 2, 0, 0, FW_THIN, 0, 0, 0,
 * SHIFTJIS_CHARSET, OUT_TT_PRECIS, 0, PROOF_QUALITY, FIXED_PITCH, face) and
 * WM_SETFONT with redraw */
void OS_EditSetFont(const char* face, int size);
void OS_EditLimitText(int maxChars);     // EM_LIMITTEXT
void OS_EditSetText(const char* t);      // SetWindowText
void OS_EditSelectAll(void);             // EM_SETSEL 0, -1
void OS_EditFocus(int toEdit);           // SetFocus(the control / the main window)
int OS_EditGetText(char* buf, size_t n); // GetWindowText; the length copied
void OS_EditSetColour(uint32_t rgb);     // text colour as 0x00RRGGBB
void OS_EditRefresh(void);               // InvalidateRect + UpdateWindow
void OS_EditDestroy(void);               // remove the subclass and the font
void OS_EditRejectAscii(int reject);     // 1.69/472 on ("B0 29"): single-byte printable characters are swallowed
int OS_EditImeOpen(void);                // 1.69/472 on ("B0 23"): the IME open state the control was given

/* The script's dialogs (resources 0x71, 0x72 and 0x77 of the original,
 * centred on the screen as top-most windows; src/sys/dialog.c).  Each
 * returns the dialog result: 1 when confirmed, 0 when cancelled or closed.
 * Text buffers are 0x100 bytes; a `maxLen` <= 0 means unlimited.
 *   Input1 : one edit field (id 0x3f9) with a title, initial text and limit;
 *            OK (0x3fa) / cancel (0x3fb)
 *   Input2 : two labelled edit fields (0x3fc, 0x3fd; labels 0x400, 0x401);
 *            OK (0x3fe) / cancel (0x3ff); the focus goes to the first field
 *            without an initial text
 *   Profile: four edit fields (surname 0x407, given name 0x410, nickname
 *            0x413, first-person pronoun 0x412, 10 bytes each) and two
 *            combo boxes (month 0x409 listing 1..12, day 0x40a listing
 *            1..31, cut to the month's days - 29 for February - when the
 *            month changes); `month` and `day` are the 0-based list
 *            selections (CB_SETCURSEL in, CB_GETCURSEL out).  OK only; the
 *            OK handler rejects a field with a single-byte (half-width)
 *            character with the message MSG_DLG_HALFWIDTH (caption
 *            MSG_DLG_INPUT_ERROR) and keeps the dialog open; the focus
 *            starts on the OK button. */
int OS_DialogInput1(const char* title, const char* initial, int maxLen, char* out);
int OS_DialogInput2(const char* title, const char* label1, const char* initial1, int maxLen1, char* out1,
	const char* label2, const char* initial2, int maxLen2, char* out2);
int OS_DialogProfile(char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month, int32_t* day);

/* The launcher's "Select game" dialog (src/sys/launcher.c), shown when
 * the engine starts outside a game directory: a list of the games
 * games.json records, a directory (typed, browsed with OS_BrowseFolder,
 * or taken from the list), the state of that directory in a line of
 * text, and under "Advanced" the engine profile (one of `profiles`) and
 * extra command-line options.  The launcher owns the structure: the
 * dialog calls `onSelect` when a list entry is chosen and `onDirectory`
 * when the directory changes (both update `dir`, `status`, `launchable`
 * and `profile`), and shows what they set.  Launch is accepted only
 * while `launchable` is set (else `refused` is shown and the dialog stays
 * open).  Returns 1 for Launch, 0 for Quit or the close box, -1 when no
 * display is available.  Texts are Shift-JIS but `dir`, which is native. */
#define OS_LAUNCHER_DIR_MAX     0x400
#define OS_LAUNCHER_OPTIONS_MAX 0x200
typedef struct OsLauncher OsLauncher_t;
struct OsLauncher
{
	const char* const* names; // the list: the games' names
	int count;
	int selected;                          // the list selection, -1 for none
	char dir[OS_LAUNCHER_DIR_MAX];         // the directory (native)
	char status[0x200];                    // what the directory holds ("Heptagram (profile 1.553)", "Unknown game", ..)
	int launchable;                        // 1 when the directory holds a game
	char profile[32];                      // the profile to run with (Advanced)
	char options[OS_LAUNCHER_OPTIONS_MAX]; // extra options (Advanced)
	const char* const* profiles;           // the profile names to choose from
	int profileCount;
	int advanced;        // 1 when the Advanced fields are shown (in and out)
	const char* refused; // the message shown when Launch is pressed without a game
	void (*onSelect)(OsLauncher_t* l, int index);
	void (*onDirectory)(OsLauncher_t* l);
};
int OS_LauncherDialog(OsLauncher_t* l);

/* The debugger's window (src/dbg/): a second top-level window of w x h
 * client pixels with a 32-bit surface the debugger presents whole.  Its
 * input reaches the engine through the debug_* handlers above.  A back
 * end without one returns 0 from OS_DebugWindowOpen and the debugger
 * draws over the main window instead.  The X11 and Win32 back ends have
 * one; the WebAssembly build has no debugger and does not define these. */
#if !defined(BGI_WASM)
int OS_DebugWindowOpen(int w, int h, const char* title);
void OS_DebugWindowClose(void);
int OS_DebugWindowAlive(void);
void OS_DebugWindowPresent(const OsSurface_t* s); // the whole surface at 1:1
#endif

// -------------------------------------------------------------------------
// Fonts
// -------------------------------------------------------------------------
/* The original renders every glyph with GDI: CreateFontA (SHIFTJIS_CHARSET,
 * OUT_TT_PRECIS, PROOF_QUALITY, FIXED_PITCH) at a supersampled height,
 * TextOutA into an 8-bit DIB whose palette runs from white (0) to black
 * (255), and then box-filters the result itself (src/gfx/font.c).  The OS
 * layer only has to deliver that supersampled ink coverage.
 *   height : cell height in pixels (CreateFontA nHeight: ascent + descent)
 *   width  : average character width in pixels (nWidth): the width of a
 *            half-width character; a full-width glyph is twice as wide
 *   bold   : FW_BOLD instead of FW_THIN */
typedef struct OsFont OsFont_t;
// "B0 C2" of 1.69/472 on: make a font file available to OS_FontOpen (AddFontResource); 1 ok
int OS_FontAddFile(const char* path);
// "B0 C3" of 1.494 on: a font file read from an archive (AddFontMemResourceEx); 1 ok
int OS_FontAddMemory(const void* data, uint32_t size);
/* 1 when a face of exactly this (Shift-JIS) family name is installed; the
 * fallback faces of "B0 C7" apply when it is not */
int OS_FontFaceExists(const char* face);
/* "B0 C4" of 1.588 on / "B0 C5" of 1.653 on: the installed face names of a
 * charset (0x80 Shift-JIS, 0 ANSI, 0x86 Big5, 0x88 GB2312, as the Windows
 * charset numbers), each written to `buf` as a NUL-terminated string in
 * the text encoding when `buf` is not NULL, with the bytes they take
 * altogether in *outBytes; the number of faces */
int OS_FontEnumFaces(char* buf, int charset, int* outBytes);
// the GDI font of the glyph rasteriser: lfHeight, lfWidth, FW_BOLD / FW_THIN, lfItalic
OsFont_t* OS_FontOpen(const char* face, int height, int width, int bold, int italic);
void OS_FontClose(OsFont_t* f);
/* Clear the w*h 8-bit buffer (pitch bytes per row) and draw the character
 * (its `len` bytes in the text encoding: 1 or 2 of Shift-JIS, 1 .. 4 of
 * UTF-8) at its top-left corner: 0 = blank, 255 = ink. */
void OS_FontRender(OsFont_t* f, const char* sjis, int len, uint8_t* buf, int pitch, int w, int h);
/* The monochrome variant used by the GDI text instruction (src/gfx/monofont.c).
 * The original creates its font with CreateFontA(size, size / 2, 0, 0,
 * FW_NORMAL / FW_BOLD, 0, 0, 0, SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS, 0,
 * DRAFT_QUALITY, FIXED_PITCH | FF_ROMAN, face) and draws into a
 * 1-bit DIB.  OS_FontRenderMono clears the w*h bitmap (pitch bytes per row,
 * MSB first) and draws the character at its top-left corner: a set bit is
 * ink. */
OsFont_t* OS_FontOpenMono(const char* face, int size, int bold);
void OS_FontRenderMono(OsFont_t* f, const char* sjis, int len, uint8_t* bits, int pitch, int w, int h);
/* The advance of a character in pixels (GetTextExtentPoint32; the glyph's
 * advance under FreeType; measureText on a page): the debugger sizes its
 * text cell with it, and the right-to-left layout of the OpenBGI
 * extensions places joining letters by it (src/gfx/text/layout_bidi.c). */
int OS_FontCharAdvance(OsFont_t* f, const char* sjis, int len);

// -------------------------------------------------------------------------
// Audio
// -------------------------------------------------------------------------
/* The sound library of the original ("Wave Master") mixes into DirectSound
 * secondary buffers.  The reimplementation mixes in software and hands
 * 16-bit stereo PCM to the OS layer through a pull callback: the back end
 * calls fill(user, samples, frames) from its audio thread whenever it
 * needs `frames` interleaved stereo frames (2 * frames int16_t values),
 * and the mixer must fill every one of them.  The period is the back
 * end's choice (about 20 ms). */
typedef void (*OsAudioFill_t)(void* user, int16_t* samples, int frames);
int OS_AudioStart(int rate, OsAudioFill_t fill, void* user); // open the output at `rate` Hz and start pulling; 1 ok
void OS_AudioStop(void);                                     // stop pulling (no callback runs after it returns) and close the output
int OS_AudioAvailable(void);                                 // "DirectSound 8 present": 1 when OS_AudioStart may be tried
/* CD audio through MCI (mciSendCommand) for src/snd/sound.c: best effort,
 * a platform without CD audio returns 0 (failure) from every call.  The
 * device id is opaque, 0 = none. */
uint32_t OS_MciOpenCdAudio(const char* element);                      // MCI_OPEN "cdaudio" (+ MCI_OPEN_ELEMENT "X:" when given)
int OS_MciSetTimeFormatTmsf(uint32_t dev);                            // MCI_SET MCI_SET_TIME_FORMAT MCI_FORMAT_TMSF; 1 ok
int OS_MciClose(uint32_t dev);                                        // MCI_CLOSE
int OS_MciStatus(uint32_t dev, uint32_t item, uint32_t* out);         // MCI_STATUS MCI_STATUS_ITEM; 1 ok
int OS_MciPlay(uint32_t dev, uint32_t from, uint32_t to, int notify); // MCI_PLAY MCI_FROM | MCI_TO (| MCI_NOTIFY); 1 ok
int OS_MciStop(uint32_t dev);                                         // MCI_STOP
#define OS_MCI_STATUS_MODE    4
#define OS_MCI_STATUS_TRACKS  7
#define OS_MCI_MODE_NOT_READY 0x20c // .. 0x212: stop, play, record, seek, pause, open
// PlaySound(path, module, SND_FILENAME | SND_NODEFAULT | SND_ASYNC | SND_NOWAIT); 1 when accepted
int OS_PlaySoundFile(const char* path);

/* Movies ("90 F0" .. "90 F3", src/gfx/movie.c).  The original builds a
 * DirectShow filter graph (CoCreateInstance CLSID_FilterGraph +
 * IGraphBuilder::RenderFile) and shows the video in a child window of the
 * main window (IVideoWindow: put_Owner, WS_CHILD | WS_CLIPSIBLINGS,
 * SetWindowPosition); graph events arrive through WM_APP
 * (IMediaEventEx::SetNotifyWindow); a click on the video window reaches
 * the main window as WM_MOUSEACTIVATE, which the original's window
 * procedure turns into a button down / up pair of its own.
 * The OS layer drains the events on WM_APP and raises `movie_event` with
 * whether an EC_COMPLETE was among them. */
#define OS_MOVIE_OK                 1
#define OS_MOVIE_FAILED             0 // any other error
#define OS_MOVIE_NO_AUDIO_DEVICE    2 // VFW_E_NO_AUDIO_HARDWARE
#define OS_MOVIE_VIDEO_NOT_RENDERED 3 // VFW_S_VIDEO_NOT_RENDERED
#define OS_MOVIE_AUDIO_NOT_RENDERED 4 // VFW_S_AUDIO_NOT_RENDERED
int OS_MovieOpen(const char* path);   // build the graph for the file: one of the codes above (everything is released again on OS_MOVIE_FAILED)
/* show the video as a child at the client rectangle, start it and report
 * its length in ms (the stop position converted to media time); 1 ok */
int OS_MovieStart(int x, int y, int w, int h, int32_t volume, int32_t* lengthMs);
void OS_MovieStop(void);             // stop, hide the video window, release the graph
int OS_MovieIsRunning(void);         // IMediaSeeking: current position < stop position
void OS_MovieSetVolume(int32_t vol); // IBasicAudio::put_Volume: -10000 (silent) .. 0; ignored without a graph

/* A helper program with its standard output piped to the engine: the
 * movie player of src/sys/moviefx.c runs ffmpeg and ffprobe this way
 * where the native player cannot play a file (always, on POSIX).  argv
 * is NULL-terminated; argv[0] names the program - a bare name is looked
 * for next to the engine's binary (OS_BinaryDir) first, then on the
 * PATH.  The strings are native (UTF-8 on POSIX, the engine's own on
 * Windows); OS_PathNative turns an engine path into one.  The program's
 * standard error is discarded.  OS_PipeProcRead blocks until some bytes
 * arrive (the count), 0 at the program's end, -1 on an error.
 * OS_PipeProcStop ends the program (killed when it still runs) and
 * releases everything; it accepts NULL. */
typedef struct OsPipeProc OsPipeProc_t;
OsPipeProc_t* OS_PipeProcStart(const char* const* argv);
int OS_PipeProcRead(OsPipeProc_t* p, void* buf, int n);
void OS_PipeProcStop(OsPipeProc_t* p);
void OS_PathNative(const char* path, char* out, size_t n); // an engine path as the native file system spells it

// -------------------------------------------------------------------------
// Dialogs, shell, registry
// -------------------------------------------------------------------------
// the MessageBox flags the engine uses (the MB_* values) and the button ids it returns (ID*)
#define OS_MB_OK           0x0000 // buttons (flags & 0xf)
#define OS_MB_OKCANCEL     0x0001
#define OS_MB_YESNO        0x0004
#define OS_MB_RETRYCANCEL  0x0005
#define OS_MB_ICONHAND     0x0010 // icon (flags & 0xf0)
#define OS_MB_ICONQUESTION 0x0020
#define OS_MB_ICONWARNING  0x0030
#define OS_MB_ICONINFO     0x0040
#define OS_MB_DEFBUTTON2   0x0100 // the second button is the default
#define OS_MB_SYSTEMMODAL  0x1000
#define OS_IDOK            1
#define OS_IDCANCEL        2
#define OS_IDRETRY         4
#define OS_IDYES           6
#define OS_IDNO            7
// MessageBox over the main window: modal; the id of the button pressed (the close box counts as cancel)
int OS_MessageBox(const char* text, const char* caption, uint32_t flags);
/* GetOpenFileName (save 0) / GetSaveFileName (save 1) for "80 3B":
 * `filter` is one "<description> <pattern>" pair separated by its last
 * space (the original turns that space into the NUL of lpstrFilter),
 * `defExt` the default extension without the dot, `path` (n bytes) the
 * initial file name in and the chosen one out; 1 when a file was chosen */
int OS_FileDialog(int save, char* path, size_t n, const char* filter, const char* defExt, const char* initialDir, const char* title);
/* SHBrowseForFolder for "81 3A" and the installer: file-system folders
 * only, `initial` selected at the start (NULL for none); 1 when a folder
 * was chosen (path receives it) */
int OS_BrowseFolder(char* path, size_t n, const char* title, const char* initial);
int OS_ShellExecute(const char* file, const char* params, const char* dir); // ShellExecute "open", SW_SHOWNORMAL; 1 when it started
int OS_ClipboardSetText(const char* text);                                  // SetClipboardData(CF_TEXT) ("7E" of 1.69 build 472 on); 1 = done
/* the desktop wallpaper ("B0 F0"): HKCU\control panel\desktop
 * WallpaperStyle = stretch ? "2" : "0", TileWallpaper = tile ? "1" : "0",
 * then SystemParametersInfo(SPI_SETDESKWALLPAPER, path, UPDATEINIFILE |
 * SENDCHANGE); returns its result */
int OS_SetWallpaper(const char* path, int stretch, int tile);

/* Processes (src/sys/process.c: "80 E0" .. "80 E3", the registration and
 * uninstaller runs): CreateProcess with a command line.  OS_ProcessWaitIdle is
 * WaitForInputIdle; OS_ProcessWait(ms) returns 1 once the process has
 * ended, 0 when it is still running after `ms`; OS_ProcessClose frees the
 * handle without ending the process. */
typedef struct OsProcess OsProcess_t;
OsProcess_t* OS_ProcessStart(const char* cmdLine); // NULL when it could not be started
void OS_ProcessWaitIdle(OsProcess_t* p);
int OS_ProcessWait(OsProcess_t* p, uint32_t ms);
void OS_ProcessClose(OsProcess_t* p);
int OS_NamedMutexExists(const char* name); // 1 when OpenMutex of the name succeeds: another program holds it

// IShellLink: a shortcut file at `linkPath` to `target`, nothing else set, as the original does
int OS_CreateShortcut(const char* linkPath, const char* target);
/* 1.494 on: the same with a command line for the target (IShellLink::SetArguments);
 * NULL or "" for none */
int OS_CreateShortcutArgs(const char* linkPath, const char* target, const char* args);
/* The registry (src/sys/install/: the installed folder, the file
 * association, the uninstaller entry): key names start with "HKLM\",
 * "HKCU\" or "HKCR\" and continue with the path below the hive (a name
 * without a prefix is under HKCU); only REG_SZ values are read and
 * written.  A back end without a registry keeps them in a file
 * (OsCommon_RegFile*). */
int OS_RegReadString(const char* key, const char* value, char* buf, size_t n); // 1 = a REG_SZ was read into buf (n bytes)
int OS_RegCreateKey(const char* key, const char* cls);                         // RegCreateKeyEx with the class; 1 = exists now
int OS_RegWriteString(const char* key, const char* value, const char* s);      // creates the key; `value` NULL is the default value
int OS_RegDeleteKey(const char* key);                                          // RegDeleteKey: the key with its values
// WM_SETTINGCHANGE + SHChangeNotify(SHCNE_ASSOCCHANGED): tell the shell the file association changed
void OS_ShellNotifyAssocChanged(void);

// machine identity for the installer's GetMachineKey (src/sys/install/machine.c)
uint32_t OS_CpuSignature(void); // CPUID.1:EAX, 0 without CPUID
/* GetVersionEx: platform 0 = Win32s, 1 = Windows 9x, 2 = NT; 0 on failure.
 * The engine takes the NT paths (IOCTLs, the registry) for platform 2, so
 * a non-Windows back end reports 2. */
int OS_Version(int* platform, int* major, int* minor);
/* "81 0C" of 1.529 on: the full OSVERSIONINFO - major, minor, build,
 * platform and the service pack string (up to 128 bytes) */
void OS_VersionEx(uint32_t out[4], char* csd, size_t n);
/* "81 02" of 1.662 on: the user's UI language as a Windows LANGID
 * (GetUserDefaultUILanguage: primary language in the low 10 bits, 0x11
 * Japanese, 0x09 English, 0x04 Chinese; the sublanguage above) */
int OS_UiLanguage(void);
// "81 0D": GlobalMemoryStatusEx, the physical memory in bytes (64 bit)
void OS_MemoryStatusEx(uint64_t* totalPhys, uint64_t* availPhys);
/* "81 0B": the display adapter (D3D adapter identifier of the original):
 * its description and driver version (four words, most significant first);
 * 1 when known */
int OS_DisplayAdapter(char* desc, size_t n, uint32_t version[4]);
/* SHGetSpecialFolderLocation(csidl) as a path into buf (n bytes), for the
 * selectors of GetSpecialFolder() other than the Windows directory, which
 * OS_WindowsDir (GetWindowsDirectory) gives; 1 = ok */
int OS_SpecialFolder(int csidl, char* buf, size_t n);
int OS_WindowsDir(char* buf, size_t n);

/* the resource dialogs of the installer (ids INSTALL_DLG_*, install.h);
 * ctx is the matching Install*Dlg structure, which the dialog fills in;
 * returns the dialog result as install.h describes it for each (the
 * options dialog 1 / 0, the choice dialog the button chosen); a back end
 * without the dialogs returns 0 */
int OS_InstallerDialog(int id, void* ctx);

/* Inter-process: hand a string to the running instance whose main window
 * has class `windowClass` (a named file mapping and the 0x9000 message,
 * which the receiver raises as `ipc_string`); 1 when such a window was
 * found and the message delivered */
int OS_IpcSendToRunning(const char* windowClass, const char* data);

#endif // BGI_OS_H_
