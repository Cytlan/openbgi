/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os.h - the operating-system layer under the hosts (included as
 *        "host/os.h": the hosts, the tools and the tests have src/ on
 *        their include path, the core does not)
 *
 * Everything the hosts need from the system lives behind this interface:
 * files and directories, time, threads, the main window and its input,
 * the display surface, the fonts, audio output, message boxes, the
 * machine's facts, other programs and the handful of shell/registry
 * services the "80 Fx" installer instructions use.
 *
 * Three back ends exist:
 *   src/host/win32/ (*.c) - the original behaviour (GDI, waveOut, DirectShow, registry)
 *                           through the wide-character API: strings are converted
 *                           from and to Shift-JIS, whatever the system's code page
 *   src/host/posix/ (*.c) - Linux: X11 window, font files for FreeType, ALSA, a registry file
 *   src/host/wasm/  (*.c) - the browser: a canvas, the files over a websocket (docs/dev/wasm.md)
 * plus src/host/os_common.c for code shared by all of them, the tables of
 * the functions below for the host (openbgi.h; host/os_common.h):
 * src/host/os_files.c the files and the registry, os_window.c the windows,
 * os_machine.c the machine and the shell, os_media.c CD audio and the
 * movie player, os_ffmpeg.c the movie decoder - and src/host/os_host.c, the
 * services an engine is made with (OS_Host).  The engine reaches all it
 * needs only through those (openbgi.h); the tools (src/tools/: the
 * commands of the program, the dump and pack module) call this layer as
 * hosts do.
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
 *     OS_AudioStart runs on the back end's audio thread (it only reads the
 *     engine's sound out, OpenBGI_AudioRead), the movie decoder's pipe
 *     readers run on threads of their own (OS_PipeProcRead, OS_SleepMs);
 *     everything else is only called from the main thread.
 *
 * A port to another system implements every function below (the stubs of
 * src/host/posix/ show what a service without a counterpart may do: report
 * failure) and raises the OsHostHandlers_t callbacks of OS_SetHostHandlers
 * from OS_PumpMessages().  src/host/os_common.c holds the pieces that are the
 * same everywhere (the registry file format, the main window's title, the
 * conversion in the text encoding); the wildcards and the Shift-JIS and
 * UTF-8 primitives are the engine's (bgi/sjis.h).
 */
#ifndef BGI_OS_H_
#define BGI_OS_H_

#include "bgi/common.h"
#include "openbgi.h" // OpenBGI_Host_t

// the platform: BGI_WIN32 for the Windows back end, BGI_POSIX for the others (the Makefile also names it)
#if defined(_WIN32)
#define BGI_WIN32 1
#else
#define BGI_POSIX 1
#endif

// -------------------------------------------------------------------------
// Process / start-up
// -------------------------------------------------------------------------
/* The host of an engine on this layer (src/host/os_host.c; openbgi.h).
 * OS_Host: the services the engine is made with, the clocks OS_TicksMs,
 * OS_PerfCounterNs and OS_LocalTime, the fonts (OS_FontFiles,
 * OS_FontRenderer) and what the back end does itself (OS_HostCaps).
 * OS_AnswerRequests answers every
 * request the engine has made with this layer's windows (OS_MessageBox,
 * OS_Dialog*, the pickers, OS_GameChooserDialog, OS_InstallerDialog), each
 * modal, the engine's sound kept going meanwhile (OS_SetModalTick).
 * OS_ShowFrame blits what changed in a frame's picture to the main
 * window (OS_BlitToWindow) and has the text entry repaint itself over it;
 * it turns the input method on while the frame takes typed text
 * (OS_TextInput), and keeps the frame to paint the window again from it
 * (the input handlers' `paint` and `entryPaint`) until the next frame.
 * OS_RunEngine runs the game the command line names - the start, the
 * sound output (OS_AudioStart) reading the engine's sound, the frames
 * with the requests answered, their pictures shown, the window's
 * messages pumped and the idle wait in between, the stop - and returns
 * the exit code;
 * OS_RunMain is the whole program around it (the engine, OS_Init,
 * OS_Shutdown) for the platforms' entry points (src/host/<platform>/main.c),
 * and OS_JoinArgs makes main()'s arguments one command line. */
const OpenBGI_Host_t* OS_Host(void);
void OS_AnswerRequests(OpenBGI_t* o);
void OS_ShowFrame(const OpenBGI_Frame_t* f);
int OS_RunEngine(OpenBGI_t* o, const char* cmdLine);
int OS_RunMain(const char* cmdLine);
/* The launcher's packed sources for the engine OS_RunMain makes
 * (OpenBGI_SetLauncher, openbgi.h): the bytes stay where they are.  The
 * programs' entry points name the pack the build carries (gOsLauncherSources,
 * host/os_common.h) before OS_RunMain. */
void OS_UseLauncher(const void* sources, uint32_t size);
// the same for the program's own icon, an .ico file (gOsIcon, host/os_common.h; OpenBGI_SetIcon)
void OS_UseIcon(const void* ico, uint32_t size);
void OS_JoinArgs(int argc, char** argv, char* out, size_t n);
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
/* The reimplementation's own files - openbgi.json, the launcher's record of
 * the games - live beside the binary and in directories the user picks,
 * which are paths of the host rather than of the game: "native" paths,
 * in the host's own form (UTF-8 on POSIX; on Windows the same Shift-JIS
 * Windows paths the engine uses, which the back end converts to UTF-16 -
 * a directory code page 932 cannot spell appears under a substitute
 * spelling the back end maps back, see src/host/win32/wide.c).  OS_BinaryDir
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
/* The folder of the user's own data where the platform keeps it, as a
 * native path with a trailing separator, made when missing ("" when there
 * is none): the games played from zip archives keep what they write
 * there (fs.h).  $XDG_DATA_HOME/openbgi/ (~/.local/share/openbgi/) on
 * Linux, ~/Library/Application Support/OpenBGI/ on macOS,
 * ~/config/settings/OpenBGI/ on Haiku, %APPDATA%\OpenBGI\ on Windows,
 * $TMPDIR/openbgi/ from Android's command line. */
void OS_UserDataDir(char* buf, size_t n);
void OS_NativeToUtf8(const char* native, char* out, size_t n);
void OS_Utf8ToNative(const char* utf8, char* out, size_t n);
FILE* OS_NativeOpen(const char* path, const char* mode);
uint8_t* OS_ReadNativeFile(const char* path, uint32_t* size);
int OS_WriteNativeFile(const char* path, const void* data, uint32_t size);
/* More of the native file system, for the host's table of files
 * (src/host/os_files.c: the zip games' folders, the launcher's probe) and
 * the command-line tools that work on directories of the host (`openbgi
 * dump`, `openbgi pack`): OS_NativeStat tells what a path is; OS_NativeMkdir creates a
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
#define OS_NATIVE_NONE OPENBGI_NATIVE_NONE // OS_NativeStat: nothing of that name
#define OS_NATIVE_FILE OPENBGI_NATIVE_FILE
#define OS_NATIVE_DIR  OPENBGI_NATIVE_DIR
typedef OpenBGI_NativeStat_t OsNativeStat_t; // the size, the last modification
typedef void (*OsNativeListFn)(void* ctx, const char* name, int isDir);
int OS_NativeStat(const char* path, OsNativeStat_t* out); // OS_NATIVE_*; `out` may be NULL
int OS_NativeWritable(const char* path);                  // 1 when the file there can be opened for writing; it is not changed
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
/* "81 00" of 1.653 on: the script text is UTF-8 (1) rather than Shift-JIS
 * (0).  Text the engine hands this layer - window titles, message boxes,
 * the characters of the font renderer - is then in that encoding; the
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
OsFile_t* OS_FileOpenWrite(const char* path);                    // GENERIC_WRITE, OPEN_ALWAYS (keeps the contents, starts at 0); NULL on failure
void OS_FileClose(OsFile_t* f);                                  // accepts NULL
uint32_t OS_FileRead(OsFile_t* f, void* buf, uint32_t n);        // bytes read, 0 on error or at the end
uint32_t OS_FileWrite(OsFile_t* f, const void* buf, uint32_t n); // bytes written
int OS_FileSeek(OsFile_t* f, uint32_t pos);                      // from the start; 1 = ok
uint32_t OS_FileSize(OsFile_t* f);                               // bytes
// the last-write time as a 64-bit FILETIME (100 ns units since 1601) in two dwords (lo, hi)
int OS_FileGetTime(OsFile_t* f, uint32_t* lo, uint32_t* hi);
int OS_FileSetTime(OsFile_t* f, uint32_t lo, uint32_t hi);

// the FILE_ATTRIBUTE_* bits the engine looks at (OPENBGI_ATTR_* of openbgi.h)
#define OS_INVALID_ATTRS  OPENBGI_INVALID_ATTRS // the file does not exist (INVALID_FILE_ATTRIBUTES)
#define OS_ATTR_READONLY  OPENBGI_ATTR_READONLY
#define OS_ATTR_HIDDEN    OPENBGI_ATTR_HIDDEN
#define OS_ATTR_SYSTEM    OPENBGI_ATTR_SYSTEM
#define OS_ATTR_DIRECTORY OPENBGI_ATTR_DIRECTORY
#define OS_ATTR_ARCHIVE   OPENBGI_ATTR_ARCHIVE
#define OS_ATTR_NORMAL    OPENBGI_ATTR_NORMAL                        // no other attribute is set
uint32_t OS_FileAttrs(const char* path);                             // GetFileAttributes: the bits above, OS_INVALID_ATTRS on failure
int OS_FileSetAttrs(const char* path, uint32_t attrs);               // SetFileAttributes (the read-only bit at least)
int OS_FileDelete(const char* path);                                 // DeleteFile
int OS_FileMove(const char* from, const char* to);                   // MoveFile
int OS_FileCopy(const char* from, const char* to, int failIfExists); // CopyFile: 0 when `to` exists and failIfExists is set
int OS_DirCreate(const char* path);                                  // CreateDirectory: 0 when it exists already
int OS_DirRemove(const char* path);                                  // RemoveDirectory: the directory must be empty

/* Directory enumeration (FindFirstFile / FindNextFile): `pattern` is a
 * path whose file part may hold the wildcards '*' and '?'
 * (Sjis_WildcardMatch says how they match); the directory part is
 * taken as it is.  Each match fills an OsFindData_t. */
typedef struct OsFind OsFind_t;
typedef OpenBGI_FindData_t OsFindData_t;                        // the entry's OS_ATTR_* bits and its name alone
OsFind_t* OS_FindFirst(const char* pattern, OsFindData_t* out); // the first match; NULL = none (nothing to close)
int OS_FindNext(OsFind_t* h, OsFindData_t* out);                // the next match; 0 = done
void OS_FindClose(OsFind_t* h);                                 // accepts NULL

/* Drives (src/fs/paths.c, the installer's CD search).  The engine
 * enumerates them to find the CD-ROM that holds the game data and to
 * check that the drive of its own directory is ready.  On POSIX only a
 * single pseudo drive "C:" exists and is "fixed". */
#define OS_DRIVE_UNKNOWN   OPENBGI_DRIVE_UNKNOWN // the GetDriveType values
#define OS_DRIVE_NO_ROOT   OPENBGI_DRIVE_NO_ROOT
#define OS_DRIVE_REMOVABLE OPENBGI_DRIVE_REMOVABLE
#define OS_DRIVE_FIXED     OPENBGI_DRIVE_FIXED
#define OS_DRIVE_REMOTE    OPENBGI_DRIVE_REMOTE
#define OS_DRIVE_CDROM     OPENBGI_DRIVE_CDROM
#define OS_DRIVE_RAMDISK   OPENBGI_DRIVE_RAMDISK
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
/* The host loop's idle wait between frames (OpenBGI_Frame_t.idleMs 0): a
 * sleep of about half a millisecond that gives the system a chance to run
 * without losing a frame (the original waits on a waitable timer; Sleep(1)
 * stands in on Windows 9x).  OS_IdleInit creates whatever the sleep needs,
 * OS_IdleShutdown frees it. */
void OS_IdleInit(void);
void OS_IdleShutdown(void);
void OS_IdleSleep(void);
/* The 1.535+ variant with a duration: 1 ms means the same half-millisecond
 * timer wait as OS_IdleSleep; any other value sets the timer to that many
 * ms but waits at most 10 ms for it (so a longer idle sleep is capped at
 * 10 ms on the timer path). */
void OS_IdleSleepMs(int ms);

// -------------------------------------------------------------------------
// Threads
// -------------------------------------------------------------------------
#if !defined(BGI_WASM)
// a thread running fn(arg) until it returns (CreateThread); not on a page
typedef struct OsThread OsThread_t;
typedef void (*OsThreadFn_t)(void* arg);
#define OS_PRIO_LOW     (-1)                                          // THREAD_PRIORITY_BELOW_NORMAL: work that may wait for the rest (where the system lets a thread step back)
#define OS_PRIO_NORMAL  0                                             // THREAD_PRIORITY_NORMAL
#define OS_PRIO_HIGHEST 2                                             // THREAD_PRIORITY_HIGHEST
OsThread_t* OS_ThreadStart(OsThreadFn_t fn, void* arg, int priority); // NULL when the thread could not be created
void OS_ThreadJoin(OsThread_t* t);                                    // wait for the thread, free it; accepts NULL
#endif
int OS_CpuCount(void); // processors online, at least 1

// -------------------------------------------------------------------------
// Memory status, machine identity
// -------------------------------------------------------------------------
// GlobalMemoryStatus (OpenBGI_MemoryStatus_t of openbgi.h): the memory in use as a percentage, the rest in bytes (capped at 4 GB)
void OS_MemoryStatus(OpenBGI_MemoryStatus_t* m);
int OS_ComputerName(char* buf, size_t n); // GetComputerName into buf (n bytes); the installer's machine key ("80 EE", "80 EF")
int OS_UserName(char* buf, size_t n);     // GetUserName
/* CPUID and the time stamp counter of an x86 processor, for the host's
 * machine (src/host/os_machine.c; the original reads them inline), where
 * OS_HAVE_CPUID says so: not on other processors, nor on a page.
 * OS_Cpuid returns 0 when the processor has no CPUID instruction (the
 * EFLAGS.ID test) or the leaf is above the highest one supported. */
#if !defined(BGI_WASM) && (defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64))
#define OS_HAVE_CPUID 1
#endif
#if defined(OS_HAVE_CPUID)
int OS_Cpuid(uint32_t leaf, uint32_t out[4]); // eax, ebx, ecx, edx
uint64_t OS_ReadTsc(void);                    // RDTSC (after a serialising CPUID), 0 if unavailable
#endif
/* QueryPerformanceCounter / QueryPerformanceFrequency ("80 05" of 1.69
 * build 472 on): the counter in nanoseconds; 0 when there is no counter */
int OS_PerfCounterNs(int64_t* out);

// -------------------------------------------------------------------------
// Window, display, input
// -------------------------------------------------------------------------
/* The main window, for the host's table of it (OpenBGI_Window_t of
 * openbgi.h, made of these by src/host/os_window.c; the headless back end
 * has none, and its engines keep the window nobody sees): 800x600 client
 * pixels at first (WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX),
 * windowed or full screen, created hidden.  The title is shown as
 * OsCommon_MainTitle spells it.  Its input and events go to the host's
 * handlers (OsHostHandlers_t, below). */
typedef struct OsWindowDesc
{
	const char* title; // the game's title (Shift-JIS)
	int x, y;          // window position (outer rectangle, screen pixels)
	int width, height; // client size in pixels
} OsWindowDesc_t;
int OS_WindowCreate(const OsWindowDesc_t* d);          // 1 ok
void OS_WindowDestroy(void);                           // DestroyWindow
void OS_WindowShow(int show);                          // ShowWindow(SW_SHOW / SW_HIDE) + UpdateWindow ("80 64")
void OS_WindowSetTitle(const char* t);                 // SetWindowText with OsCommon_MainTitle applied ("80 66"); NULL clears the game's part
void OS_WindowMove(int x, int y);                      // outer rectangle origin, screen pixels
void OS_WindowGetRect(int* x, int* y, int* w, int* h); // GetWindowRect: outer rectangle, screen coords
int OS_WindowMinimized(void);                          // IsIconic
void OS_WindowMinimize(void);                          // ShowWindow(SW_MINIMIZE) ("80 65")
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
/* The engine takes typed text (OpenBGI_Frame_t.textInput): the input
 * method is on for the main window, placed at the field (x, y, w, h, the
 * picture's pixels); off again with on = 0.  The text comes through the
 * `text` input handler. */
void OS_TextInput(int on, int x, int y, int w, int h);
/* What the back end does itself, OPENBGI_CAP_* of openbgi.h: _DIALOGS
 * when its OS_MessageBox and OS_Dialog* show windows of their own (or
 * answer without asking, as a headless one does) */
uint32_t OS_HostCaps(void);

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
/* "81 66": the window has a sizing border (and may be maximised), or is
 * of a fixed size again.  When the player changes the client area's size
 * the `resized` handler says so; the picture goes on being shown scaled
 * into it. */
void OS_WindowSetResizable(int on);
/* A picture shown at another size than its own is scaled by a rule:
 * OPENBGI_SCALE_BILINEAR (the start) or OPENBGI_SCALE_BICUBIC, which the
 * engine names when a game chooses (OpenBGI_Window_t.setScaling).
 * OS_WindowScales tells whether the back end follows it (1) or scales in
 * a way of its own (0: the call does nothing). */
void OS_WindowSetScaling(int rule);
int OS_WindowScales(void);
/* The window's icon: one picture at `count` sizes, the largest first
 * (OpenBGI_Icon_t of openbgi.h), kept until the next call; the window to
 * come takes it when there is none yet.  OS_WindowHasIcons tells whether
 * the back end can show one (0: the call does nothing). */
void OS_WindowSetIcon(const OpenBGI_Icon_t* icons, int count);
int OS_WindowHasIcons(void);
/* The window's size on the screen (OpenBGI_Window_t.dpi, workArea,
 * setAspect).  OS_WindowFits tells whether the back end takes part (1):
 * it then reports the scale of the window's screen in dots per inch (96:
 * none) and the part of that screen a window may take, shows a picture
 * of another size than the client area scaled at any time, and tells the
 * `resized` handler of every size the player gives the window, whose
 * sizing border the engine then asks for.  OS_WindowSetAspect: while the
 * player sizes the window its client area keeps the proportions w : h
 * (0, 0: any).  The `screenChanged` handler is told when the scale, the
 * work area or the window's frame becomes another.  A back end that does
 * not take part (0) keeps its three functions empty. */
int OS_WindowFits(void);
int OS_WindowDpi(void);
void OS_WindowWorkArea(int* x, int* y, int* w, int* h);
void OS_WindowSetAspect(int w, int h);
/* Filters of the back end's own beyond the three rules
 * (OpenBGI_Window_t.filterName): the name of number n, NULL for none.
 * OS_WindowSetScaling takes one as OPENBGI_SCALE_OWN + n. */
const char* OS_WindowFilterName(int n);
/* What was blitted to the main window since the last call is to be seen
 * now.  A back end that shows every blit at once does nothing here; one
 * that draws the whole picture anew for every change (through a graphics
 * processor) does it once for a frame's blits. */
void OS_WindowPresent(void);

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
 * "the main window".  The host (os_host.c) takes one to show a frame and
 * releases it right after. */
typedef void OsDc_t;
OsDc_t* OS_WindowGetDc(void); // GetDC(hWnd); release when done; NULL without a window
void OS_WindowReleaseDc(OsDc_t* dc);
// BitBlt(dc, dstX, dstY, w, h, src, srcX, srcY, SRCCOPY): w x h pixels of the surface to the window, unscaled
void OS_BlitToWindow(OsDc_t* dc, int dstX, int dstY, int w, int h,
	const OsSurface_t* src, int srcX, int srcY);
void OS_WindowFillBlack(void); // the whole client area black (an uncovered window while there is no picture to show)

int OS_PumpMessages(void);      // PeekMessage + dispatch of one message to the host's handlers; 1 if there was one
void OS_DragAccept(int accept); // DragAcceptFiles: whether `dropFile` is raised

/* What the main window and the debugger's window report, for the host
 * (src/host/os_host.c), which hands it to the engine with the calls of
 * openbgi.h whose names and conventions these follow ("The input", "The
 * window's events": OpenBGI_Key, OpenBGI_Close, ...) and paints from the
 * last frame's picture: raised from OS_PumpMessages, on the main thread,
 * in the order the messages came, with `user` as given; a back end raises
 * what it can (every member may be NULL).  Keys are Windows virtual-key
 * codes (a key the text entry takes with OPENBGI_KEY_TAKEN); positions
 * are pixels of the picture (client pixels mapped through the scaling of
 * a window of another size; in full screen, pixels of the display mode).
 * No input and no close is raised while a dialog of this layer is modal;
 * the repaints are, as a window under a dialog still paints. */
typedef struct OsHostHandlers
{
	void (*key)(void* user, int vk, int down, int flags);                      // WM_KEYDOWN / UP, WM_SYSKEYDOWN / UP: OPENBGI_KEY_REPEAT, OPENBGI_KEY_SYSTEM
	void (*pointer)(void* user, int x, int y);                                 // WM_MOUSEMOVE (where a back end has it; the host also asks, OS_CursorGetPos)
	void (*button)(void* user, int button, int down, int x, int y, int flags); // WM_?BUTTONDOWN / UP: OPENBGI_BUTTON_*; OPENBGI_CLICK_DOUBLE for WM_?BUTTONDBLCLK
	void (*wheel)(void* user, int delta);                                      // WM_MOUSEWHEEL: WHEEL_DELTA (120) units, + = away from the user
	void (*focus)(void* user, int active);                                     // WM_ACTIVATE: the window gained (1) or lost (0) the focus
	void (*text)(void* user, const char* utf8);                                // WM_CHAR: the text a key typed (the input method's too), UTF-8, no control characters
	void (*touch)(void* user, int id, int phase, int x, int y);                // a contact of a touch screen (OPENBGI_TOUCH_*), the picture's pixels; where a back end has one
	void (*paint)(void* user);                                                 // WM_PAINT: the window was uncovered, all of it is to be shown again
	void (*entryPaint)(void* user, int x, int y, int w, int h);                // the text entry's WM_PAINT: the picture under it again first
	// the window's events
	void (*close)(void* user);                                                   // WM_CLOSE: the close box, the window manager's close
	void (*minimized)(void* user, int minimized);                                // WM_SIZE: minimised (1) or restored (0)
	void (*resized)(void* user, int w, int h);                                   // WM_SIZE of a window with a sizing border (OS_WindowSetResizable): its client size now
	void (*displayChanged)(void* user);                                          // WM_DISPLAYCHANGE
	void (*screenChanged)(void* user);                                           // the screen's scale, its work area or the window's frame is another (OS_WindowFits)
	void (*modal)(void* user);                                                   // about every 20 ms while a loop of the system's has the thread (the player drags the window or its border): the sound is kept going
	void (*dropFile)(void* user, const char* path);                              // a file dropped on the window (WM_DROPFILES, XDND; after OS_DragAccept(1)) by its native path, once for each; a second instance's boot path (0x9000)
	void (*message)(void* user, uint32_t msg, uint32_t wParam, uint32_t lParam); // every other message, for the "80 54" message waits
	void (*cdNotify)(void* user, int status);                                    // MM_MCINOTIFY (1 = successful)
	void (*movieEvent)(void* user, int completed);                               // WM_APP from the movie graph (see OS_MovieOpen)
	void (*quit)(void* user);                                                    // WM_QUIT, SIGTERM / SIGINT: the run ends
	// the debugger's window (OS_DebugWindowOpen)
	void (*debugKey)(void* user, int vk, int down, int ch);             // ch: the character typed (a code point), 0 for none
	void (*debugMouse)(void* user, int x, int y, int button, int down); // button -1: a move; 0 L 1 R 2 M
	void (*debugWheel)(void* user, int delta);                          // WHEEL_DELTA units
	void (*debugClose)(void* user);
	void* user;
} OsHostHandlers_t;
void OS_SetHostHandlers(const OsHostHandlers_t* h); // copied; NULL: none

/* The pads (docs/dev/input.md).  OS_PadPoll reads what the pads did since the
 * last call and answers a bit per port (0 .. 3) whose state changed;
 * OS_PadState is a port's state then, NULL when no pad is there (any
 * more).  The host hands them to the engine after each frame
 * (OpenBGI_Pad).  A back end without pads answers 0.  OS_PadShutdown lets
 * go of the devices. */
uint32_t OS_PadPoll(void);
const OpenBGI_PadState_t* OS_PadState(int port);
void OS_PadShutdown(void);

/* Cursor.  "80 67" selects a shape 0..4 from the original's table of five
 * HCURSORs: 0 the arrow, 1 the resource cursor 0x6A, 2..4 are
 * NULL entries that leave the class cursor (the arrow) in place.  The
 * shape is applied when the system asks (WM_SETCURSOR); OS_CURSOR_NONE is
 * the back end's own "no cursor" value, never selected by scripts. */
#define OS_CURSOR_ARROW  0
#define OS_CURSOR_CUSTOM 1
#define OS_CURSOR_NONE   -1
void OS_CursorSetShape(int shape);
void OS_CursorShow(int show); // ShowCursor counter semantics
/* The system's pointer, for the host: where it is (GetCursorPos, screen
 * coordinates), there to be put (SetCursorPos), and the conversions to
 * and from the picture's pixels of the main window (ScreenToClient /
 * ClientToScreen, through the scaling of a window of another size; in
 * full screen screen and client coordinates are the display mode's
 * pixels).  The engine asks none of these: the host tells it where the
 * pointer is (OpenBGI_Pointer) and moves it where a frame says
 * (OpenBGI_Frame_t.pointerMoved). */
void OS_CursorGetPos(int* x, int* y);
void OS_CursorSetPos(int x, int y);
void OS_ScreenToClient(int* x, int* y);
void OS_ClientToScreen(int* x, int* y);

/* Child windows ("B0 1x", src/sys/child.c): a back end with pop-up
 * windows of the system's hands them over as the host's
 * (OpenBGI_ChildWindows_t of openbgi.h, OpenBGI_Host_t.childWindows): up
 * to 8 windows owned by the main window (WS_POPUP | WS_CAPTION |
 * WS_SYSMENU | WS_MINIMIZEBOX, class "BGI - Child window") that paint
 * themselves from the pixels last presented, refuse WM_CLOSE unless the
 * engine closes them, and forward the keys typed into them (WM_KEYDOWN /
 * WM_KEYUP) to the main window, but for Ctrl+C with a copy text.  NULL
 * from a back end without: the engine draws them in the frame. */
const OpenBGI_ChildWindows_t* OS_ChildWindows(void);

/* The text entry ("B0 2x", src/sys/edit.c): a back end with an edit
 * control of the system's own hands it over as the host's
 * (OpenBGI_TextEntry_t of openbgi.h, OpenBGI_Host_t.textEntry): one EDIT
 * control over the main window, created hidden and 1 x 1 with the text
 * MSG_EDIT_DEFAULT_TEXT together with the window (ES_CENTER |
 * ES_AUTOHSCROLL | ES_NOHIDESEL) and made again in the style a later
 * build's setup asks for (setStyle), subclassed as by the original: Tab
 * and Enter select its text and give the focus back to the main window
 * or hide it, as the style says, and WM_PAINT first raises the input
 * handlers' `entryPaint` so the host can put the picture under it back -
 * the control draws with a transparent background.  The keys it takes go to the input
 * handlers' `key` with OPENBGI_KEY_TAKEN.  NULL from a back end without
 * one: the engine draws the entry in the frame. */
const OpenBGI_TextEntry_t* OS_TextEntry(void);

/* The script's dialogs (resources 0x71, 0x72 and 0x77 of the original,
 * centred on the screen as top-most windows; src/sys/dialog.c).  Each
 * returns the dialog result: 1 when confirmed, 0 when cancelled or closed.
 * Text buffers are 0x100 bytes; a `maxLen` <= 0 means unlimited.
 *   Input1 : one edit field (id 0x3f9) with a title, initial text and limit;
 *            OK (0x3fa) / cancel (0x3fb)
 *   Input2 : two labelled edit fields (0x3fc, 0x3fd; labels 0x400, 0x401);
 *            OK (0x3fe) / cancel (0x3ff); the focus goes to the first field
 *            without an initial text; `compact` ("B0 87", 1.573 on) selects
 *            resource 0x7E, the two fields side by side
 *   A `digits` field (1.573 on) takes only digits as they are typed, and
 *            a '-' while it is empty; pasted text is not filtered.
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
int OS_DialogInput1(const char* title, const char* initial, int maxLen, int digits, char* out);
int OS_DialogInput2(const char* title, const char* label1, const char* initial1, int maxLen1, int digits1, char* out1,
	const char* label2, const char* initial2, int maxLen2, int digits2, char* out2, int compact);
int OS_DialogProfile(char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month, int32_t* day);

/* The game chooser (OpenBGI_GameChooser_t of openbgi.h, src/sys/gamechooser.c)
 * as a modal "Select game" dialog, its directory browsed with
 * OS_BrowseFolder: 1 for Launch, 0 for Quit or the close box, -1 when no
 * display is available. */
int OS_GameChooserDialog(OpenBGI_GameChooser_t* l);

/* The debugger's window (src/dbg/), for the host's table of it
 * (OpenBGI_DebugWindow_t, src/host/os_window.c): a second top-level window
 * of w x h client pixels with a 32-bit surface the debugger presents
 * whole.  Its input goes to the host's debug* handlers above, its close
 * box as debugClose.  The X11 and Win32 back ends have one; the headless
 * and the WebAssembly back ends have none and do not define these. */
#if !defined(BGI_WASM) && !defined(BGI_HEADLESS) && !defined(BGI_ANDROID)
int OS_DebugWindowOpen(int w, int h, const char* title);
void OS_DebugWindowClose(void);
void OS_DebugWindowPresent(const OsSurface_t* s); // the whole surface at 1:1
#endif

// -------------------------------------------------------------------------
// Fonts
// -------------------------------------------------------------------------
/* The fonts are the host's to give the engine (openbgi.h, "The fonts"):
 * each back end has one of two kinds, and NULL for the other.
 *
 * OS_FontFiles: the system's font files for the engine's own FreeType
 * rasteriser (POSIX: fontconfig, else the usual font directories, with
 * BGI_FONT_GOTHIC / BGI_FONT_MINCHO / BGI_FONT naming a file that wins).
 * OS_FontRenderer: a renderer of the system's own that the engine draws
 * with instead (Windows: GDI, as the original; a page: its canvas). */
const OpenBGI_FontFiles_t* OS_FontFiles(void);
const OpenBGI_FontRenderer_t* OS_FontRenderer(void);

// -------------------------------------------------------------------------
// Audio
// -------------------------------------------------------------------------
/* The sound output of the host (src/host/os_host.c), which plays the
 * engine's sound (OpenBGI_AudioRead, openbgi.h): the back end calls
 * fill(user, samples, frames) whenever it needs `frames` interleaved
 * 16-bit stereo frames (2 * frames int16_t values), from a thread of its
 * own or - on a page - from the engine's thread between two of its calls.
 * fill writes all of them and returns how many it had: the rest is
 * silence it wrote.  The period is the back end's choice (about 20 ms).
 * Without a sound device the back end still calls fill at the same pace
 * and drops the frames, so the output never stalls.  OS_AudioAvailable is
 * the original's start-up check (DirectSound 8 is installed), which the
 * engine makes before it starts its sound library. */
typedef int (*OsAudioFill_t)(void* user, int16_t* samples, int frames);
int OS_AudioStart(int rate, OsAudioFill_t fill, void* user); // open the output at `rate` Hz and start pulling; 1 ok
void OS_AudioStop(void);                                     // stop pulling (no callback runs after it returns) and close the output
int OS_AudioAvailable(void);                                 // "DirectSound 8 present": 1 when the engine may start its sound
#if defined(BGI_WIN32)
/* The Windows back end's CD audio and movie player, for the host's tables
 * (OpenBGI_CdAudio_t, OpenBGI_MoviePlayer_t; src/host/os_media.c); the
 * other back ends have neither.  CD audio through MCI (mciSendCommand):
 * the device id is opaque, 0 = none. */
uint32_t OS_MciOpenCdAudio(const char* element);                      // MCI_OPEN "cdaudio" (+ MCI_OPEN_ELEMENT "X:" when given)
int OS_MciSetTimeFormatTmsf(uint32_t dev);                            // MCI_SET MCI_SET_TIME_FORMAT MCI_FORMAT_TMSF; 1 ok
int OS_MciClose(uint32_t dev);                                        // MCI_CLOSE
int OS_MciStatus(uint32_t dev, uint32_t item, uint32_t* out);         // MCI_STATUS MCI_STATUS_ITEM; 1 ok
int OS_MciPlay(uint32_t dev, uint32_t from, uint32_t to, int notify); // MCI_PLAY MCI_FROM | MCI_TO (| MCI_NOTIFY); 1 ok
int OS_MciStop(uint32_t dev);                                         // MCI_STOP

/* Movies ("90 F0" .. "90 F3").  The original builds a
 * DirectShow filter graph (CoCreateInstance CLSID_FilterGraph +
 * IGraphBuilder::RenderFile) and shows the video in a child window of the
 * main window (IVideoWindow: put_Owner, WS_CHILD | WS_CLIPSIBLINGS,
 * SetWindowPosition); graph events arrive through WM_APP
 * (IMediaEventEx::SetNotifyWindow); a click on the video window reaches
 * the main window as WM_MOUSEACTIVATE, which the original's window
 * procedure turns into a button down / up pair of its own.
 * The OS layer drains the events on WM_APP and raises `movieEvent` with
 * whether an EC_COMPLETE was among them. */
int OS_MovieOpen(const char* path); // build the graph for the file: OPENBGI_MOVIE_* of openbgi.h (everything is released again on OPENBGI_MOVIE_FAILED)
/* show the video as a child at the client rectangle, start it and report
 * its length in ms (the stop position converted to media time); 1 ok */
int OS_MovieStart(int x, int y, int w, int h, int32_t volume, int32_t* lengthMs);
void OS_MovieStop(void);             // stop, hide the video window, release the graph
int OS_MovieIsRunning(void);         // IMediaSeeking: current position < stop position
void OS_MovieSetVolume(int32_t vol); // IBasicAudio::put_Volume: -10000 (silent) .. 0; ignored without a graph
#endif

#if !defined(BGI_WASM)
/* A helper program with its standard output piped to the OS layer (not
 * on a page): the tests make their movies with the ffmpeg program this
 * way; the engine itself starts none.  argv
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
#endif

// -------------------------------------------------------------------------
// Dialogs, shell, registry
// -------------------------------------------------------------------------
// MessageBox over the main window: modal; the flags OPENBGI_MB_*, the id of the button pressed OPENBGI_ID* (openbgi.h; the close box counts as cancel)
int OS_MessageBox(const char* text, const char* caption, uint32_t flags);
/* While `tick` is set, the modal loops of this layer's windows - the
 * boxes, the dialogs, the pickers - call tick(user) about every 20 ms, on
 * the main thread, between two of their events: the host keeps the
 * engine's sound going under a box (OpenBGI_RunAudio), as DirectSound
 * played on under the original's.  NULL stops it.  A back end whose
 * boxes stop the thread altogether (a page's) calls nothing. */
void OS_SetModalTick(void (*tick)(void* user), void* user);
/* GetOpenFileName (save 0) / GetSaveFileName (save 1) for "80 3B":
 * `filter` is one "<description> <pattern>" pair separated by its last
 * space (the original turns that space into the NUL of lpstrFilter),
 * `defExt` the default extension without the dot, `path` (n bytes) the
 * initial file name in and the chosen one out; 1 when a file was chosen */
int OS_FileDialog(int save, char* path, size_t n, const char* filter, const char* defExt, const char* initialDir, const char* title);
/* the same for "81 38" with `count` description / pattern pairs (the
 * filter list of the dialog, the first one selected), in the newer style
 * with the places bar */
int OS_FileDialogN(int save, char* path, size_t n, const char* const* descs, const char* const* pats, int count,
	const char* defExt, const char* initialDir, const char* title);
/* SHBrowseForFolder for "81 3A" and the installer: file-system folders
 * only, `initial` selected at the start (NULL for none); 1 when a folder
 * was chosen (path receives it) */
int OS_BrowseFolder(char* path, size_t n, const char* title, const char* initial);
/* a web link (http, https) or a document shown with the program the
 * desktop has for it ("80 E3": the engine asks for nothing else); the
 * target goes to that program as one argument, through no command line.
 * 1 when it was handed over */
int OS_OpenDocument(const char* target);
int OS_ClipboardSetText(const char* text); // SetClipboardData(CF_TEXT) ("7E" of 1.69 build 472 on); 1 = done
/* the desktop wallpaper ("B0 F0"): HKCU\control panel\desktop
 * WallpaperStyle = stretch ? "2" : "0", TileWallpaper = tile ? "1" : "0",
 * then SystemParametersInfo(SPI_SETDESKWALLPAPER, path, UPDATEINIFILE |
 * SENDCHANGE); returns its result */
int OS_SetWallpaper(const char* path, int stretch, int tile);


/* IShellLink: a shortcut file at `linkPath` to `target`, with the command
 * line `args` for it (IShellLink::SetArguments, 1.492 on), nothing else
 * set, as the original does; NULL for no command line */
int OS_CreateShortcut(const char* linkPath, const char* target, const char* args);
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

/* GetVersionEx: the full OSVERSIONINFO - major, minor, build, platform
 * (0 Win32s, 1 Windows 9x, 2 NT) and the service pack string (up to 128
 * bytes).  The engine takes the NT paths (IOCTLs, the registry) for
 * platform 2, so a non-Windows back end reports 2. */
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
 * returns the request's answer as install.h describes it for each (the
 * options dialog 1 / 0, the choice dialog the button chosen and one); a
 * back end without the dialogs returns 0, a cancel for each */
int OS_InstallerDialog(int id, void* ctx);

/* Inter-process: hand a string to the running instance whose main window
 * has class `windowClass` (a named file mapping and the 0x9000 message,
 * which the receiver raises as `dropFile`); 1 when such a window was
 * found and the message delivered */
int OS_IpcSendToRunning(const char* windowClass, const char* data);

#endif // BGI_OS_H_
