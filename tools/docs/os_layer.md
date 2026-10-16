# The OS layer

`inc/bgi/os.h` is the whole interface between the engine and the host.
Nothing above it includes a platform header; `src/os/os_common.c` holds the
platform-independent helpers (DOS wildcards, the Shift-JIS lead-byte test,
the registry text file, Shift-JIS to UTF-8), `src/os/posix/*.c` the Linux
back end, `src/os/win32/*.c` the Windows one and `src/os/wasm/*.c` the
browser's (a canvas and the files over a websocket; `docs/wasm.md`
describes it).  The Makefile picks the directory from `PLATFORM` (`make` /
`make win` / `make wasm`).

Conventions: paths arrive as the engine builds them (Shift-JIS, `\`
separators, optionally a drive); functions mirroring a Win32 API keep its
return convention (`OS_FileAttrs` returns `OS_INVALID_ATTRS`, message boxes
return `OS_IDOK` ...); the engine's virtual-key codes, window messages and
dialog resource ids are those of Windows because the scripts and the
engine's own code use them.

## What the engine asks for

| area | functions | original |
|------|-----------|----------|
| process | `OS_Init/Shutdown`, `OS_ExeDir`, the instance mutex; the "native" paths of games.json and the launcher (`OS_BinaryDir`, `OS_GameDir`, `OS_NativeOpen`, ..) | GetModuleFileName, CreateMutex |
| files | `OS_File*`, attributes, copy / move / delete, directories, `OS_Find*`, drives, media | CreateFile, FindFirstFile, GetDriveType, IOCTL_STORAGE_CHECK_VERIFY |
| time | `OS_TicksMs`, `OS_SleepMs`, `OS_LocalTime`, the 0.5 ms idle sleep | timeGetTime, waitable timers |
| threads | locks, threads with a priority, atomics | CRITICAL_SECTION, CreateThread |
| machine | memory status, names, CPUID / TSC for `src/sys/cpu.c` | GlobalMemoryStatus, cpuid |
| window | create / show / move / title / minimise / close, screen size, frame metrics, display modes, full screen, the picture size inside a larger or smaller client (`OS_WindowSetPicture`, the "81 64" window size of 1.529 on), DC blits of the back buffer, events, cursor, key state | the main window, DirectDraw mode switch, BitBlt / StretchDIBits |
| child windows | `OS_Child*` | the "B0 1x" pop-ups |
| text entry | `OS_Edit*` | the subclassed EDIT control |
| dialogs | `OS_MessageBox`, `OS_DialogInput1/2/Profile`, file / folder pickers, the installer dialogs | MessageBox, resources 0x71 / 0x72 / 0x77 |
| fonts | `OS_FontOpen/Close/Render/RenderMono`, `OS_FontCharAdvance` (the debugger's cell) | CreateFont + TextOut into an 8-bit / 1-bit DIB, GetTextExtentPoint32 |
| debugger | `OS_DebugWindowOpen/Close/Alive/Present` and the `debug_*` events: a second top-level window for `src/dbg/` (not in the WebAssembly build; a back end returning 0 gets the overlay) | none |
| audio | `OS_AudioStart/Stop/Available` (a pull callback), MCI, PlaySound | DirectSound, mciSendCommand |
| movies | `OS_Movie*` | the DirectShow graph |
| child processes | `OS_PipeProcStart/Read/Stop`, `OS_PathNative`: a program started with its standard output on a pipe, for the ffmpeg movie player (`src/sys/moviefx.c`) | CreateProcess |
| the tools' file system | `OS_NativeStat`, `OS_NativeMkdir`, `OS_NativeRemove`, `OS_NativeList`, `OS_NativeSeek`, `OS_NativeAbsolute`, `OS_NativeArgs`: directories of the host for `bgidump` / `bgipack` (`docs/dump.md`), by native path; `OS_WorkerLimit`, `OS_RunWorkers`: a job shared out among forked processes (one after the other on Windows).  `src/os/*/native.c` | none |
| shell | ShellExecute, wallpaper, processes, shortcuts, registry, special folders, IPC to a running instance | |

Events reach the engine through `OsEventHandlers_t` from `OS_PumpMessages()`
(one message per call, like the original's PeekMessage loop); key events
carry Windows virtual keys, mouse events client coordinates, and the
"system" keys (Alt+key, F10) go to `syskey` the way WM_SYSKEYDOWN did.

## The POSIX back end

`src/os/posix/` is split by area; `posix_internal.h`, `x11_internal.h`
and `ui_internal.h` are its own headers, `inc/bgi/os_posix.h` and
`inc/bgi/os_x11.h` what the files share with each other.

**Paths, files and the machine** (`paths.c`, `files.c`, `process.c`,
`registry.c`, `machine.c`, `sjis.c`, `time.c`, `thread.c`).  The game
directory is reported as `C:\` (`OS_ExeDir`; the original takes its own
directory, this binary lives elsewhere and takes the current directory at
start-up, so the engine is run from inside the game folder), so every
path the engine builds carries a drive letter - the drive-ready checks of
the file layer key on it, and the scripts' scan for `data0Nxxx.arc`
members goes through them; on the way back a drive letter maps to the
game directory, `\` becomes `/`, and every component that does not exist
as spelled is matched case-insensitively (the game data was written for a
case-insensitive file system).  Shift-JIS is converted with iconv
(CP932).  The registry is a text file of `key\value=data` lines in
`~/.config/bgi/registry.txt`; the instance mutex is a lock file there.
Only one pseudo drive `C:\` exists and is "fixed"; CD-ROM checks fail.
Processes use `sh -c`, ShellExecute `xdg-open`, wallpaper and shortcuts
report failure.  Locks and threads are pthreads; the idle sleep is a
nanosleep.

**X11** (`x11.c`, `window.c`, `events.c`, `child.c`, `edit.c`, `field.c`,
`dbgwin.c`).  The main window is a fixed-size top-level; full screen sets
`_NET_WM_STATE_FULLSCREEN` and scales the display mode the engine asked
for onto the screen, centred, translating every coordinate the engine
sees (cursor, mouse events, `OS_WindowGetRect`) back into the mode space;
the same mapping serves a windowed client of another size than the
picture (`OS_WindowSetPicture`, the "81 64" window size), where the
picture is scaled into the client.
Blits convert the 16-bit RGB555 or 32-bit back buffer rows to the
visual's layout and `XPutImage` them; `OS_StretchToWindow` filters
bilinearly when the engine asked for HALFTONE.  Keys are mapped from
keysyms to virtual keys; auto-repeat is detected from the paired
release / press; a second click of the same button within 400 ms is a
double click.  Child windows are transient top-levels; the text entry is
drawn by the back end itself over the picture (the engine's `edit_paint`
present is captured into a buffer, the text is drawn on it with FreeType
through the text field of `field.c`, and the result put on the window),
with the X input method for Japanese input where one is configured.  The
debugger's window (`dbgwin.c`) is a second top-level with the resource
name `bgi-debug` (the game's is `bgi`).  Not reproduced: file drag and
drop (XDND) and the IME status window.

**Message boxes and dialogs** (`uikit.c`, `msgbox.c`, `dialogs.c`).
Message boxes and the three script dialogs are drawn from scratch in the
classic grey look, run as modal loops that keep the engine's windows
repainting but drop their input.  The profile dialog replaces the two
combo boxes by number pickers (up / down keys) and keeps the half-width
check and the "OK only" behaviour of the resource.  Without a display a
message box goes to stderr and returns its default button; setting
`BGI_MSGBOX_STDERR` in the environment forces that even with a display,
for unattended runs.  The file dialogs and the installer dialogs are not
provided.

**Fonts** (`font.c`) - FreeType.  A face name is resolved with fontconfig
when it was available at build time (lang `ja`, serif for names
containing 明朝 / "Mincho"), else by searching the usual font directories
for well-known Japanese fonts; `BGI_FONT_GOTHIC`, `BGI_FONT_MINCHO` or
`BGI_FONT` name a file to use instead.  GDI's cell metrics are
reproduced from the OS/2 table (usWinAscent + usWinDescent = the
requested height, the baseline at the ascent) and the width request
means "a full-width glyph is twice this wide".  `OS_FontRender`
rasterises in monochrome because the original draws into a palette DIB
where GDI never antialiases; the engine's own oversampling does the
smoothing.  Bold on a face without a bold style is a one-pixel
horizontal emboldening, like GDI's overstrike.

**Audio** (`audio.c`) - ALSA when `pkg-config alsa` succeeds at build time
(20 ms periods on `default`), else a silent thread that pulls the mixer
at the same pace.  MCI and PlaySound report failure (no CD audio, no
`.wav` jingle); `OS_MovieOpen` answers `OS_MOVIE_FAILED`, on which the
engine's movie player (`src/gfx/movie.c`) falls back to `src/sys/moviefx.c`:
`ffprobe` for the length and frame rate, one `ffmpeg` child decoding the
video to raw BGRA frames at the picture size and another the sound to
16-bit stereo at 44100 Hz, both read through `OS_PipeProcStart` /
`OS_PipeProcRead` (`pipeproc.c`: posix_spawn with the output on a pipe, a
bare program name tried next to the binary first); the frames are shown
on the engine's clock through `Window_DcBlit`, the sound goes into the
Wave Master mixer through its external tap (`Wm_SetExternalMix`).  Without
the programs the movie is skipped and "90 F0" returns 0, as before.

## The Win32 back end

`src/os/win32/` is the original's windowing code with the engine's
decisions taken out.  `make check-win` checks it on a machine without
MinGW against the stub headers in `tools/win32stub/` (`docs/building.md`):
misspelt identifiers, wrong structure members, and a string of the wrong
width given to a wide-character call.

**Strings** (`wide.c`, declared in `inc/bgi/os_win32.h`).  The engine's
strings are Shift-JIS, and the original hands them to the ANSI (A) entry
points of Windows, which read them in the system's code page - right on
a Japanese system only; elsewhere file names are not found, titles and
messages come out as mojibake and the fonts are not found by their
Japanese names.  This back end calls no A function.  Every string is
converted to UTF-16 with code page 932 and goes to the W entry point
(`Win32_ToWide`), and what Windows hands back is converted the other
way, so the game behaves on any system as it does on a Japanese one:

* text - titles, message boxes, dialog labels, the text entry - follows
  the text encoding (`OS_SetTextEncoding`): Shift-JIS, or in UTF-8 text
  mode every well-formed UTF-8 sequence as it is and the rest as
  Shift-JIS; what the player types comes back in the same encoding
  (`Win32_TextFromWide`), cut at a character boundary when a buffer is
  short;
* the windows are Unicode windows (RegisterClassExW, DefWindowProcW,
  PeekMessageW / DispatchMessageW), the dialogs too
  (DialogBoxIndirectParamW);
* an EDIT control of a Unicode window counts UTF-16 units where the
  original's counts bytes, so the limits the scripts set (EM_LIMITTEXT)
  are kept in bytes by cutting the text after each change
  (`Win32_EditClamp`);
* a font's face name is converted before CreateFontW looks it up, and a
  character is converted with code page 932 - what TextOutA does for a
  SHIFTJIS_CHARSET font - and drawn with TextOutW;
* paths Windows reports (the executable's directory, a chosen file or
  folder, a dropped file, the special folders, registry values, the
  names of a directory listing) are converted to Shift-JIS component by
  component (`Win32_PathFromWide`).  A name code page 932 cannot spell
  (`C:\Users\Bjørn\...`) is written with `_` for each character it
  lacks, and for an absolute path the directory up to that component is
  remembered as a *path alias*: `Win32_ToWide` puts the real name back
  wherever that spelling appears (in either case, with either
  separator, for the directory and everything below it).  So a game
  installed under such a directory runs: the engine works with
  `C:\Users\Bj_rn\Game\`, Windows is asked for the real path.  The
  table holds 64 directories; a file inside a directory the engine can
  spell whose own name it cannot is listed with `_` but cannot be
  opened;
* the reimplementation's "native" paths (games.json, the launcher's
  directory) are these same paths on Windows; `OS_NativeToUtf8` writes
  the real name into games.json and `OS_Utf8ToNative` gives it its
  alias again; `OS_NativeOpen` is `_wfopen`;
* the command line is taken from GetCommandLineW and converted argument
  by argument (`Win32_CmdLineFromWide`), an option's value
  (`--games=...`) and a quoted path as paths of their own;
* the string a second instance hands to the running one stays the
  engine's bytes, as the original sends them, followed in the same file
  mapping by the path in UTF-16 for a receiver of this build, since an
  alias means nothing to another process.

Windows 9x has no wide-character API, so this back end runs on the
NT line of Windows only (the build targets Windows XP and later).
`tests/win32wide.c` runs the conversions, the aliases
and the command line on the build host (`make test`), with the two code
page functions mocked through iconv.

**Process, files and the machine** (`process.c`, `files.c`, `time.c`,
`thread.c`, `machine.c`, `registry.c`) - CreateFile / FindFirstFile /
GetDriveType and friends on the engine's paths (converted, see above),
timeGetTime + timeBeginPeriod, the waitable idle timer (NT only, 0.5 ms;
Sleep(1) on Windows 9x), CRITICAL_SECTIONs, GlobalMemoryStatus, CPUID /
RDTSC through the compiler's intrinsics, GetVersionEx, CreateProcess /
WaitForInputIdle, ShellExecute, the wallpaper registry keys +
SystemParametersInfo, IShellLink shortcuts, the registry helpers with
their `HKLM\` / `HKCU\` / `HKCR\` prefixes, SHChangeNotify,
SHGetSpecialFolderLocation, and the inter-process string of a second
instance: a file mapping `FMO%.8xForBGI` whose key and length travel in
message 0x9000 (`BGI_WM_IPC`), as the original does it.

**The window** (`window.c`, `wndproc.c`, `child.c`, `edit.c`,
`dbgwin.c`) - the classes `BGI - Main window` (CS_OWNDC | CS_DBLCLKS, icon
0x65, the arrow) and `BGI - Child window` (style 0x3020), the main window
with style 0x80ca0000 and the hidden 1 x 1 EDIT created with it and
subclassed (Tab / Enter select all and give the focus back, WM_PAINT first
raises `edit_paint`, WM_IME_NOTIFY invalidates).  The window procedure
(`wndproc.c`) follows the original's message for message: every message
not dispatched by the engine's own key / button handlers goes through
`raw_message` (the original hands every one to its message-wait
dispatcher); WM_CLOSE / WM_DESTROY / WM_CREATE / WM_SIZE / WM_ACTIVATE /
WM_PAINT raise their events; WM_SETCURSOR sets the table entry of the
"80 67" shape and falls through to DefWindowProc as the original does
(which re-applies the class cursor over the client area);
WM_MOUSEACTIVATE over the movie window replays the click as a button
down / up pair; WM_SYSKEYDOWN / WM_SYSKEYUP never reach DefWindowProc (no
menu), F10 counts as a key; WM_SYSCOMMAND refuses SC_SCREENSAVE /
SC_MONITORPOWER; WM_MENUCHAR returns MNC_CLOSE; WM_CTLCOLOREDIT makes the
text entry transparent in its colour; the button messages give the window
the focus and raise `mouse_button` (double clicks raise `double_click`
first and then act as a press); WM_DROPFILES, MM_MCINOTIFY, WM_APP (the
movie graph's events are drained here) and 0x9000 complete the set.  The
display mode uses ChangeDisplaySettings where the original called
IDirectDraw::SetDisplayMode; full screen is `SetWindowLong(GWL_STYLE,
WS_POPUP | WS_VISIBLE)` + `SetWindowPos(0, 0, modeW, modeH,
SWP_FRAMECHANGED)`, windowed restores style 0x90ca0000.  Presentation is
StretchDIBits of the engine's memory surface with a top-down BI_RGB
header (16-bit = RGB 5-5-5); `OS_StretchToWindow` with a clip rectangle
is the original's child blit (CreateRectRgn, HALFTONE, SetBrushOrgEx,
everything restored afterwards).  Child windows and the EDIT control are
the calls listed in os.h.  The icon (0x65) and the cursor (0x6A) are
resources of the original executable and are simply absent unless a
resource script provides them.

**Dialogs** (`dlgtemplate.c`, `dialogs.c`, `instdlg.c`, `pickers.c`) -
MessageBoxA on the main window; the dialogs 0x71, 0x72, 0x77 and the
installer's 0x6C .. 0x6F assembled as DLGTEMPLATEs in memory from the
layouts of the original's resources (dialog units, ids and styles as
stored there; strings from msg.h; 9 pt "ＭＳ Ｐゴシック" (MS PGothic), 10
pt for 0x77) and run with DialogBoxIndirectParamA; the dialog procedures
follow the original's, with the installer's decisions delegated to
`InstallOptions_Browse` / `InstallOptions_Accept` / `Install_CopyAll` in
`src/sys/install/`.  The progress dialog is modeless and pumped from the
copy loop's `alive` callback (the original copied on a second thread and
posted 0x8000 .. 0x8004 to the dialog).  GetOpenFileName /
GetSaveFileName with the original's flags and filter handling,
SHBrowseForFolder with its centring callback.

**Fonts** (`font.c`) - CreateFontA with the original's parameters
(FW_THIN / FW_BOLD, SHIFTJIS_CHARSET, OUT_TT_PRECIS, PROOF_QUALITY,
FIXED_PITCH for the text renderer; FW_NORMAL / FW_BOLD, DRAFT_QUALITY,
FIXED_PITCH | FF_ROMAN for the GDI text instruction), TextOutA into an
8-bit DIB whose palette runs white (0) to black (255), or into a 1-bit
DIB where a set bit is ink.  The DIB is kept per font and grown on
demand.

**Audio** (`audio.c`) - waveOut (four 20 ms buffers, a refill thread)
pulling the software mixer; the original's DirectSound 8 presence test as
`OS_AudioAvailable`; mciSendCommand for the CD audio primitives (MCI_OPEN
"cdaudio" [+ element], MCI_SET TMSF, MCI_PLAY with FROM / TO [/ NOTIFY to
the main window], MCI_STATUS, MCI_STOP, MCI_CLOSE); PlaySoundA; and the
original's DirectShow graph: the filter graph with
IGraphBuilder::RenderFile (its three special results mapped to
`OS_MOVIE_*`), IVideoWindow as a WS_CHILD | WS_CLIPSIBLINGS child of the
main window at the picture rectangle, IMediaEventEx::SetNotifyWindow with
WM_APP, IMediaSeeking for the length (stop position converted to media
time, / 10000) and the running test, IBasicAudio for the volume, and the
release order of the original's stop routine.  A file the graph cannot
render falls back to the ffmpeg player of the POSIX back end when
`ffmpeg.exe` / `ffprobe.exe` are found (`pipeproc.c`: CreateProcessW
with an inheritable pipe as the standard output, the arguments quoted
the way the C runtime parses them).

**The window size** - a windowed client of another size than the
picture ("81 64" of 1.529 on, `OS_WindowSetPicture`) is handled as the
full-screen mapping: the picture is StretchDIBits'ed into the client
with black bars where the aspect differs, and the cursor positions,
mouse events and `OS_ScreenToClient` / `OS_ClientToScreen` are mapped
between the two spaces.
