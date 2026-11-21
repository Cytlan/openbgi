/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_common.h - helpers shared by the OS back ends (src/host/os_common.c):
 *               the platform-independent parts of the services of os.h,
 *               which each back end calls from its own implementation.
 *               Nothing above the OS layer includes this.
 */
#ifndef BGI_OS_COMMON_H_
#define BGI_OS_COMMON_H_

#include "bgi/common.h"
#include "openbgi.h"

const char* OsCommon_FilePart(const char* path); // after the last '\\' or '/' (Shift-JIS aware)
/* the main window's title as shown: the game's title (Shift-JIS) with
 * " - OpenBGI v1.0" (OPENBGI_TITLE, release.h) appended, the name and
 * version alone for an empty one */
void OsCommon_MainTitle(char* out, size_t n, const char* sjisTitle);

/* the text file behind the POSIX registry emulation ("key\value=data"
 * lines, keys compared without regard to case); the back end names the
 * file and passes the key as the engine spells it ("HKLM\...") */
int OsCommon_RegFileRead(const char* file, const char* key, const char* value, char* buf, size_t n); // 1 = found
int OsCommon_RegFileWrite(const char* file, const char* key, const char* value, const char* data);   // data NULL removes; 1 = written
int OsCommon_RegFileDeleteKey(const char* file, const char* key);                                    // 1 = something was removed

/* Shift-JIS to UTF-8 (Sjis_ToUtf8 of the core, src/include/bgi/sjis.h) in the
 * text encoding of OS_SetTextEncoding: in UTF-8 text mode a well-formed
 * sequence passes through.  The length written. */
size_t OsCommon_SjisToUtf8(const char* sjis, char* out, size_t n);

/* The launcher's program as the build compiled it into the program
 * (make LAUNCHER=1, the default of the desktop programs: launcher/ of the
 * source tree, docs/dev/launcher.md): the bytes of its ._bp file, handed to
 * the engine with OpenBGI_SetLauncher.  Only in a build with it
 * (BGI_LAUNCHER). */
#ifdef BGI_LAUNCHER
extern const unsigned char gOsLauncherSources[];
extern const unsigned int gOsLauncherSourcesSize;
// the shader files the build carries (SHADERS=1; tools/mkpack.py), for OS_UseShaders of os_gl.h
extern const unsigned char gOsShaders[];
extern const unsigned int gOsShadersSize;
#endif

/* The program's icon as the build compiled it in (icons/openbgi.ico, an
 * .ico file; tools/mkicon.py), handed to the engine with OpenBGI_SetIcon.
 * Only in a build with it (BGI_ICON). */
#ifdef BGI_ICON
extern const unsigned char gOsIcon[];
extern const unsigned int gOsIconSize;
#endif

// the text encoding of OS_SetTextEncoding (1 = UTF-8), and the character decoding the font back ends share
extern int gOsTextUtf8;
uint32_t OsCommon_CodePoint(const char* s, int len); // the code point of a `len`-byte character in the text encoding

/* The tables of OpenBGI_Host_t (openbgi.h) made of this layer's
 * services, for OS_Host (src/host/os_files.c): the files (OS_File*,
 * OS_Native* and the rest of "Files" and of the start-up's directories)
 * and the registry (OS_Reg*). */
const OpenBGI_Files_t* OsCommon_Files(void);
const OpenBGI_Registry_t* OsCommon_Registry(void);
/* The native files for the preprocessor's `#include` (bgi/prep.h), for
 * the tools: fills in `read` (OS_ReadNativeFile), `absolute`
 * (OS_NativeAbsolute), `fromUtf8` (OS_Utf8ToNative), `sep`, `sjis` and
 * `foldCase`, and leaves the directories and `ctx` as they are
 * (src/host/os_files.c). */
struct PrepIncludes;
void OsCommon_PrepFiles(struct PrepIncludes* inc);
/* The main window and the debugger's window as the host's tables
 * (src/host/os_window.c): OS_Window*, OS_Display*, the cursor's, ... and
 * OS_DebugWindow*; NULL where the back end has none (the headless one:
 * its engines keep the window nobody sees; the page has no debugger). */
int OsCommon_Dpi(void); // OS_WindowDpi, or what BGI_DPI says instead: for the back end's own windows, which follow the scale as the game's does
const OpenBGI_Window_t* OsCommon_Window(void);
const OpenBGI_DebugWindow_t* OsCommon_DebugWindow(void);
/* The main window's picture at another size, for a back end whose window
 * shows it so (src/host/os_window.c, on os_present.h): rows y .. y + h - 1
 * of the picture `s` changed or are to be shown again, and it is shown as
 * w x h pixels by the rule OPENBGI_SCALE_*.  The value is that picture -
 * w x h pixels of 0x00RRGGBB, a row after the other, valid until the next
 * call - and *outY, *outH are the rows of it that were made anew (the
 * rows to put on the window: all of them when the size or the rule is
 * another than before).  Only those rows are worked out.  NULL when
 * memory ran out. */
const uint32_t* OsCommon_Scaled(const OsSurface_t* s, int y, int h, int w, int hOut, int rule, int* outY, int* outH);
/* The machine and the shell as the host's tables (src/host/os_machine.c):
 * the memory, the names, the version, ... (CPUID and the time stamp
 * counter only on x86), the processes, the mutexes, the clipboard, the
 * shortcuts, ...  CD audio and the movie player (src/host/os_media.c):
 * OS_Mci* and OS_Movie* of the Windows back end, NULL elsewhere.  The
 * movie decoder (src/host/os_ffmpeg.c): the movie library
 * (openbgi_ffmovie.h) over this layer's files, NULL in a build without
 * ffmpeg's libraries (macOS and the page, for now). */
const OpenBGI_Machine_t* OsCommon_Machine(void);
const OpenBGI_Shell_t* OsCommon_Shell(void);
const OpenBGI_CdAudio_t* OsCommon_CdAudio(void);
const OpenBGI_MoviePlayer_t* OsCommon_MoviePlayer(void);
const OpenBGI_MovieDecoder_t* OsCommon_MovieDecoder(void);

/* What is to happen before the files' table changes the game directory
 * (a game is started): `fn` is called first, each time; NULL for nothing.
 * The desktop hosts end the run for a first picture there, which reads
 * its files by the directory that was. */
void OsCommon_BeforeGameDir(void (*fn)(void));

/* The launcher's first pictures and the games' icons
 * (src/host/os_preview.c; OpenBGI_PreviewNext, openbgi.h).  OsPreview_Run
 * runs the game of the command line out of sight, on the caller's
 * thread, with an engine and a host of its own: with Ctrl held and a
 * click now and then until the title's menu is there, then left alone
 * until the picture has settled, the game's time counted as fast as it
 * goes, painting nothing but the pictures it looks at, writing nothing to
 * a disk.  `want` is OPENBGI_PREVIEW_*: with the icon wanted, the icon
 * of the game's own program is taken as the game starts; without a
 * picture wanted the game is not run.  1 when the game was started and
 * *cancel did not become 1 meanwhile: `out` has the picture of the last
 * frame (laid out as an OpenBGI_Frame_t's; NULL when the game showed
 * nothing) and the icon as .ico bytes (NULL for none); free() both.
 *
 * OsPreview_Poll, between two frames of the engine `o`, does that for the
 * launcher of `o` on a thread beside it, one game at a time: it hands
 * over what the run that ended brought and starts the next.  A second
 * thread does the launcher's reading (OpenBGI_JobNext; the host says
 * OPENBGI_CAP_JOBS): the pieces that were read are handed back as they
 * are done, the others when the thread's turn is over; no run is begun
 * while something is read.  OsPreview_Stop ends the run and the reading
 * there are and waits for them; the host calls
 * it before the back end's text encoding or game directory change (a game
 * is started; OsCommon_BeforeGameDir) and before the engine stops.
 * Neither does anything on the page, which has no threads. */
typedef struct OsPreviewResult
{
	uint8_t* pixels;
	int width, height, pitch, bpp;
	uint8_t* icon;
	size_t iconSize;
} OsPreviewResult_t;
int OsPreview_Run(const char* cmdLine, int want, const int* cancel, OsPreviewResult_t* out);
void OsPreview_Poll(OpenBGI_t* o);
void OsPreview_Stop(void);
/* The host a hidden run is made with, by itself (a test looks at its
 * files): its clock stands, what is written through it stays in memory,
 * and OsPreview_HostFree lets go of that.  NULL on the page. */
OpenBGI_Host_t* OsPreview_HostNew(void);
void OsPreview_HostFree(OpenBGI_Host_t* host);

#endif // BGI_OS_COMMON_H_
