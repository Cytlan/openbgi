/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * engine.c - start-up and shutdown (OpenBGI_Start, OpenBGI_Stop,
 *            Engine_Init, Engine_CreateCore, Engine_Shutdown) and the
 *            command line; interface in bgi/engine.h and openbgi.h
 *
 * OpenBGI_Start and OpenBGI_Stop are the original's WinMain without the
 * Windows glue and without its main loop, which is the host's (openbgi.h):
 * the single-instance mutex and the processor's CPUID are the host's
 * (bgi/host.h), the COM initialisation is the OS layer's, the sequence of
 * steps is the original's.  Before that
 * the reimplementation's own "--" options are taken off the command line
 * (Engine_ParseOptions); what remains is parsed as the original does it
 * (Boot_ParseCmdLine).
 */
#include "bgi/engine.h"
#include "bgi/extract.h"
#include "bgi/dbg.h"
#include "bgi/vm.h"
#include "bgi/wait.h"
#include "bgi/file.h"
#include "bgi/sys.h"
#include "bgi/version.h"
#include "bgi/sysobj.h"
#include "bgi/input.h"
#include "bgi/display.h"
#include "bgi/sound.h"
#include "bgi/gfx.h"
#include "bgi/gfx/bmseq.h"
#include "bgi/panel.h"
#include "bgi/install.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/host.h"
#include "bgi/fs.h"
#include "bgi/detect.h"
#include "bgi/release.h"
#include "openbgi.h"
#include "bgi/request.h"
#include "bgi/inframe.h"
#include "bgi/launcher.h"
#include "bgi/core.h"

// ---- command line --------------------------------------------------------

// the number of spaces at s
static int CmdLine_SkipSpaces(const char* s)
{
	int n = 0;
	while(s[n] == ' ')
		n++;
	return n;
}

#define CMDLINE_TOKEN 0x400 // bytes of a token's buffer: what is longer is cut

/* copy one token to dst (CMDLINE_TOKEN bytes); returns the characters
 * consumed.  A token in double quotes may contain spaces; an unterminated
 * quote yields 0 (which ends the scan). */
static int CmdLine_Token(char* dst, const char* s)
{
	int n = 0, k = 0;
	if(*s == '"')
	{
		s++;
		for(;;)
		{
			char c = *s;
			if(c == '\n' || c == 0)
			{
				dst[k] = 0;
				return 0;
			}
			if(c == '"')
			{
				dst[k] = 0;
				// an empty "" never advances in the original either: it ends the scan
				return n > 0 ? n + 2 : 0;
			}
			if(k + 1 < CMDLINE_TOKEN)
				dst[k++] = c;
			s++;
			n++;
		}
	}
	while(*s != ' ' && *s != '\n' && *s != 0)
	{
		if(k + 1 < CMDLINE_TOKEN)
			dst[k++] = *s;
		s++;
		n++;
	}
	dst[k] = 0;
	return n;
}

// the tokens of s into the NULL-terminated list of buffers; returns how many were filled
static int CmdLine_Split(char** bufs, const char* s)
{
	int count = 0;
	while(*bufs)
	{
		int n;
		s += CmdLine_SkipSpaces(s);
		n = CmdLine_Token(*bufs, s);
		if(n <= 0)
			break;
		s += n;
		bufs++;
		count++;
	}
	return count;
}

/* The boot path is the host's own path, which cannot be the base
 * directory as it stands where paths have no drive letter: the engine's
 * file checks ("80 34" and the search list, the sound and movie lookups)
 * pass only a path on a drive that is ready (DriveReady), so a game run
 * by its path would find no loose file and no archive outside the boot
 * archive.  The directory the path names - the path itself, or the one
 * that holds the archive or program file it names - becomes the game
 * directory instead (Host_SetGameDir: the drive the host presents as
 * "C:\"), and `rest` is what is booted from there ("." for a
 * directory).  A zip archive or a disc image is a game directory of its
 * own: its game folder in the user's data becomes the host's game
 * directory and the archive is mounted over it (fs.h); with --overlay a
 * folder is played the same way, read as an archive and never written
 * into, and so it is with a patch (--patch=FILE, a zip archive or a
 * folder of files that take the place of the game's: Fs_MountPatch),
 * which is laid over whatever was mounted.  A folder that cannot be
 * written to is played so without being asked (Boot_EnterFolder): the
 * game needs a place for its settings and saved games.  0 when the path
 * names no existing directory; it is then used as the original uses it. */

// the archive - or the folder - at the native path `full` mounted over its game folder in the user's data; 1 when it is
static int Boot_Mount(OpenBGI_t* o, const char* path, const char* full)
{
	char folder[0x400];
	int i;
	if(!Fs_GameFolder(o, full, folder, sizeof folder) || !Host_SetGameDir(o, folder))
	{
		fprintf(stderr, "openbgi: %s: no folder for what the game writes\n", path);
		return 0;
	}
	if(!Fs_MountArchive(o, full))
	{
		fprintf(stderr, "openbgi: %s: not a zip archive, disc image or folder that can be read\n", path);
		return 0;
	}
	if(!o->engine.optPreview)
		fprintf(stderr, "openbgi: playing %s; what the game writes goes to %s\n", path, folder);
	for(i = 0; i < o->engine.optPatchCount; i++)
	{ // the patches over it, the later over the earlier
		char patch[0x400];
		int files;
		Host_NativeAbsolute(o, o->engine.optPatch[i], patch, sizeof patch);
		files = Fs_MountPatch(o, patch);
		if(files < 0)
			fprintf(stderr, "openbgi: %s: not a zip archive or a folder that can be read; played without it\n", o->engine.optPatch[i]);
		else if(!o->engine.optPreview)
			fprintf(stderr, "openbgi: patch %s: %d file%s over the game's\n", o->engine.optPatch[i], files, files == 1 ? "" : "s");
	}
	return 1;
}

// a game folder is played as an archive is: --overlay, or a patch to lay over it
static int Boot_Overlaid(OpenBGI_t* o)
{
	return o->engine.optOverlay || o->engine.optPreview || o->engine.optPatchCount > 0;
}

/* 1 when the folder at the native path `dir` can be written to: a file is
 * made in it and taken away again.  (A game's own check comes too late:
 * the scripts of 1.66 on end the run when they cannot make their data
 * folder, the older ones play on and keep nothing.) */
static int Boot_Writable(OpenBGI_t* o, const char* dir)
{
	char probe[0x440];
	char sep[2] = {Host_NativeSep(o), 0};
	HostFile_t* f;
	if((size_t)snprintf(probe, sizeof probe, "%s%s.openbgi-write-test", dir, *dir && *Host_NativeFilePart(o, dir) ? sep : "") >= sizeof probe)
		return 1; // (too long to try: as before)
	f = Host_NativeOpen(o, probe, OPENBGI_OPEN_CREATE);
	if(!f)
		return 0;
	Host_FileClose(o, f);
	Host_NativeRemove(o, probe);
	return 1;
}

/* The game's folder at the native path `full` becomes the place the game
 * runs in.  One that cannot be written to - a disc, a share, somebody
 * else's files - is played as an archive is, so that the game has a
 * folder to keep its settings and saved games in. */
static int Boot_EnterFolder(OpenBGI_t* o, const char* path, const char* full)
{
	if(Boot_Overlaid(o))
		return Boot_Mount(o, path, full);
	if(!Boot_Writable(o, full))
	{
		fprintf(stderr, "openbgi: %s cannot be written to\n", path);
		if(Boot_Mount(o, path, full))
			return 1;
	}
	return Host_SetGameDir(o, full);
}

static int Boot_EnterGameDir(OpenBGI_t* o, const char* path, char* rest, size_t n)
{
	char full[0x400];
	size_t file;
	Host_NativeAbsolute(o, path, full, sizeof full);
	if(Host_NativeStat(o, full, NULL) == OPENBGI_NATIVE_DIR)
	{
		snprintf(rest, n, ".");
		return Boot_EnterFolder(o, path, full);
	}
	if(Host_NativeStat(o, full, NULL) == OPENBGI_NATIVE_FILE && Fs_IsArchive(o, full))
	{
		snprintf(rest, n, ".");
		return Boot_Mount(o, path, full);
	}
	file = (size_t)(Host_NativeFilePart(o, full) - full); // where the name after the directory starts
	if(!file || !full[file])
		return 0;
	snprintf(rest, n, "%s", full + file);
	full[file] = 0;
	if(Host_NativeStat(o, full, NULL) != OPENBGI_NATIVE_DIR)
		return 0;
	return Boot_EnterFolder(o, path, full);
}

/* The game where the engine itself runs (no path, or "."), when that
 * folder cannot be written to: played as an archive is, as a folder
 * named on the command line would be.  1 when it was mounted. */
static int Boot_DefaultFolder(OpenBGI_t* o)
{
	char dir[0x400], file[0x440];
	char sep[2] = {Host_NativeSep(o), 0};
	const char* join;
	int game = 0, i;
	static const char* const kBoot[2] = {"system.arc", "ipl._bp"};
	Host_GameDir(o, dir, sizeof dir);
	if(!dir[0])
		return 0;
	join = *Host_NativeFilePart(o, dir) ? sep : "";
	for(i = 0; i < 2; i++)
		if((size_t)snprintf(file, sizeof file, "%s%s%s", dir, join, kBoot[i]) < sizeof file && Host_NativeStat(o, file, NULL) == OPENBGI_NATIVE_FILE)
			game = 1;
	if(!game || Boot_Writable(o, dir))
		return 0;
	fprintf(stderr, "openbgi: %s cannot be written to\n", dir);
	if(Boot_Mount(o, dir, dir))
		return 1;
	Host_SetGameDir(o, dir); // (as it was)
	return 0;
}

/* The original's command line, "<boot path> [option]": the path names the
 * directory, the archive or the program file to boot from ("." and
 * ipl._bp without one); the option is "Execute as a launcher." or "Do not
 * use mutex.", anything else is ignored. */
void Boot_ParseCmdLine(OpenBGI_t* o, const char* cmdLine)
{
	char a[CMDLINE_TOKEN], b[CMDLINE_TOKEN], rest[CMDLINE_TOKEN];
	char* bufs[3] = {a, b, NULL};
	int n = CmdLine_Split(bufs, cmdLine);
	o->engine.bootDefault = n == 0 || strcmp(a, ".") == 0; // where the engine runs, as the original's "." means
	if(o->engine.bootDefault && !Boot_DefaultFolder(o) && o->engine.optPatchCount)
		fprintf(stderr, "openbgi: --patch= needs the game's path on the command line; played without\n");
	if(n == 0)
	{
		SetBootProgram(o, ".", "ipl._bp");
		return;
	}
	if(!o->engine.bootDefault && Boot_EnterGameDir(o, a, rest, sizeof rest))
		SetBootProgram(o, rest, "ipl._bp");
	else
	{
		a[0x103] = 0; // (a path as the original takes it, MAX_PATH at most)
		SetBootProgram(o, a, "ipl._bp");
	}
	if(n == 2)
	{
		o->engine.launcherMode = strcmp(b, "Execute as a launcher.") == 0;
		o->engine.noMutex = strcmp(b, "Do not use mutex.") == 0;
	}
}

// ---- initialisation ------------------------------------------------------

static void Engine_DetectVersion(OpenBGI_t* o);

// the global memory ("80 70") and the system area ("80 82" / "80 83"); always 1
static int Mem_Init(OpenBGI_t* o)
{
	SetGlobalMemSize(o, 4); // 0x10000 bytes
	// the system area of "80 82 / 83": 0x40000 bytes up to 1.599, 0x100000 from 1.616;
	// the larger block is always allocated, the size the scripts see follows the profile
	o->mem.sysAreaSize = 0x40000;
	o->mem.sysArea = (uint8_t*)BGI_Calloc(0x100000);
	return 1;
}

// the system area size of the generation (after the profile is known)
static void Mem_SetSysAreaSize(OpenBGI_t* o)
{
	o->mem.sysAreaSize = o->engine.profile->gen >= GEN_1_616 ? 0x100000 : 0x40000;
}

/* The worker pool, the compositor, the bitmap manager and the archive
 * manager.  The band size of the compositor (in pixels) follows the CPU
 * caches: L2 size in KB * 64; an L1 data cache of 64 KB or more overrides
 * that with L1 KB * 76.8, a smaller one on a multi-processor machine with
 * 0x2ee0; 0xffffffff when no cache size is known. */
static void Engine_CreateCore(OpenBGI_t* o)
{
	uint32_t band = 0xffffffffu;
	int cpus = Host_CpuCount(o);
	if(o->util.sysInfo.l2)
		band = ((o->util.sysInfo.l2 & 0xffffu) << 8) >> 2;
	if(o->util.sysInfo.l1)
	{
		uint32_t l1 = o->util.sysInfo.l1 & 0xffffu;
		if(l1 >= 0x40)
			band = (l1 * 3 * 256) / 10;
		else if(cpus > 1)
			band = 0x2ee0;
	}
	Gfx_Create(o, band, cpus); // o->gfx.gfx, o->gfx.bmpMgr
	o->files.arcMgr = FileSet_New();
}

/* Bring the engine up in the original's order: graphics core, the
 * window, memory, the initial display mode, the system area, sound (not
 * in launcher mode), then the engine profile and everything that depends
 * on it.  The timer resolution and the idle wait, which the original sets
 * up between the graphics core and the window, are the host's.  0 when
 * start-up failed (an error box was shown). */
int Engine_Init(OpenBGI_t* o)
{
	Engine_CreateCore(o);
	if(!MainWindow_Create(o))
		return 0;
	// the original loads the arrow and its resource cursor here; the host's window has them
	if(!Mem_Init(o))
		return 0;
	SetDisplayMode(o, 2, 1, 0); // the initial mode: size index 2 (800x600), 32-bit pixels, windowed
	SysArea_Init(o);
	if(!o->engine.launcherMode && !Sound_Init(o))
		return 0;
	DynMem_Init(o);
	if(!o->engine.launcherMode)
		Follow_Start(o);
	Engine_DetectVersion(o);
	Mem_SetSysAreaSize(o);
	Display_InitSizeTable(o); // the generation's picture sizes (index 2 is 800x600 in all of them)
	Vm_TablesInit(o);         // (static data in the original)
	UserOp_Init(o);
	Loader_Start(o);
	Child_InitTable(o);
	Ptcl_InitGlobals(o);
	Blit_InitMmxTables();
	o->engine.engineUp = 1;
	return 1;
}

// Tear everything down in the original's order (the window is already gone by then).
void Engine_Shutdown(OpenBGI_t* o)
{
	if(o->vm.follow.live)
		Follow_Keep(o);
	o->engine.engineUp = 0;
	Sound_CdClose(o);
	Ptcl_AutoClear(o);
	Movie_Stop(o);
	BmSeq_FreeAll(o); // the bitmap sequences of 1.69 build 472 on
	if(o->display.fullscreen)
		MainWin_RestoreMode(o);
	Panel_DeleteAll(o);
	Follow_Released(o, NULL, 0);
	BGI_Free(o->mem.globalMem);
	o->mem.globalMem = NULL;
	BGI_Free(o->mem.sysArea);
	o->mem.sysArea = NULL;
	Gfx_Destroy(o); // Gfx, BmpMgr, ArcMgr, pools
	SysArea_Shutdown(o);
	Sound_Shutdown(o);
	FileAcc_CloseAll(o); // the files "81 28" left open
	DynMem_FreeAll(o);
	UserOp_Clear(o);
	History_Reset(o, 0);
	Ring_DeleteAll(o);
	StrTab_Clear(o, 0);
	Dict_DeleteAll(o);
	Edit_Destroy(o);
	Child_DestroyAll(o);
	Ptcl_FreeGlobals(o);
	Knob_UnregisterAll(o);
	Knob_ReleaseAll(o);
	Input_ResetLayers(o); // the layers are cleared and the base layer re-pushed
	WinMsgWait_Clear(o);
	MsgQueue_Clear(o);
	Target_Clear(o);
	Watch_Clear(o);
	Loader_Stop(o);
	FileSearch_Clear(o);
	Error_FreeKept(o); // the error message "81 6A" kept (1.547 on)
}

// ---- the reimplementation's own command-line options ---------------------------
//
// The original takes "<boot path> [option]" only.  Options starting with
// "--" are ours and are removed before the rest is handed to
// Boot_ParseCmdLine:
//   --engine=NAME      select the engine profile (docs/dev/versions.md) instead
//                      of detecting the game; BGI_ENGINE=NAME does the same
//   --productname=NAME the product name the game's scripts ask for ("80 E8")
//                      instead of the one its programs check for or its
//                      openbgi.json entry gives
//   --games=PATH       the openbgi.json to detect the game with (default:
//                      the game directory's own, else the one next to the
//                      executable, written there when missing; BGI_GAMES=PATH)
//   --detect           print how the game is identified - the product name,
//                      the hash of its programs, how every engine build fits
//                      its opcodes - and exit
//   --version          print the OpenBGI version and exit
//   --help             print the options (Engine_PrintUsage) and exit
//   --enable=FAM:OP    make an opcode available regardless of the profile
//   --disable=FAM:OP   remove one ("80:63", "main:7E")
//   --list-engines     print the profiles and exit
//   --list-missing     print the opcodes the selected profile defines that
//                      have no handler yet, and exit (after --engine=)
//   --lang=en|ja       the language of the engine's messages (English by
//                      default, "ja" the originals); BGI_LANG=ja the same
//   --list=ARCHIVE     print the entries of an archive and what they are, and exit
//   --extract=ARCHIVE  write the entries into a directory (--out=DIR, default
//                      the archive's name without its extension) and exit:
//                      decoded from DSC as the engine sees them; --raw the
//                      stored bytes; --convert images as .bmp, sounds as .wav,
//                      BF_Movie frames as .bmp; --entry=NAME one entry only
//   --debug            open the debugger (docs/modding/debugger.md) with the game;
//                      --debug=pause holds the threads until F5 (F12 in the
//                      game's window opens and closes it at any time)
//   --disc-marker      let the scripts see the disc marker file - the file
//                      named after the product that the discs carry; every
//                      boot program quits at once when it finds one next to
//                      the game, so it is hidden from "80 34" by default
//   --no-disc-check    the game counts as registered ("80 EC") without a
//                      comap.dat made for this machine - the games' own
//                      registration program (reg.exe) wants the disc and
//                      Windows, and an unregistered game's boot program
//                      asks for the disc until it is given up
//   --dialogs=frame    draw the message boxes and the scripts' dialogs in
//                      the game's picture (bgi/inframe.h); --dialogs=host
//                      leaves them to the host's windows.  By default the
//                      host's when it has them (OPENBGI_CAP_DIALOGS),
//                      but in the picture while a pad has the turn
//   --overlay          play a game's folder without writing into it: what
//                      the game writes goes to its folder in the user's
//                      data, as for a zip archive or a disc image, and is
//                      read from there first (a folder on a medium that
//                      cannot be written, or one to keep as it is)
//   --preview          a run nobody sees, as a host makes it for the
//                      launcher's first picture of a game
//                      (OpenBGI_PreviewNext): as --overlay, without the
//                      notes of what is played from where
//   --preview=none     the launcher has no first pictures made of the
//                      games that were never played ("preview": "none"
//                      among the launcher's settings says the same for
//                      every start)
//   --patch=FILE       lay a zip archive (or a folder) of files over the
//                      game: its files take the place of the game's of
//                      the same path, its root being the game's folder;
//                      several times for several, the later over the
//                      earlier.  A game's folder is played as with
//                      --overlay then
//   --stand-in-font=gothic|mincho  the design of the built-in font that
//                      stands in for a font that is not there, whatever
//                      the font's name says (=auto: by its name)
//   --own-pad-keys=off the keys a game gives a pad's controls itself
//                      ("81 1B") are ignored and OpenBGI maps every
//                      control; =on takes them, whatever openbgi.json's
//                      "ownPadKeys" says
//   --child-windows=frame  draw the child windows of "B0 1x" in the game's
//                      picture though the host has windows of its own
//                      for them (OpenBGI_Host_t.childWindows); =host has
//                      the host's windows though the game's window is
//                      fitted to its screen, where they are drawn in the
//                      picture otherwise
//   --launcher=dialog  started without a game, ask for one with the host's
//                      "Select game" dialog instead of running the game
//                      launcher (bgi/launcher.h); =program the launcher

void Engine_PrintUsage(FILE* out)
{
	fputs("usage: openbgi [OPTIONS] [GAME [the game's own arguments]]\n"
		  "\n"
		  "GAME is a game's folder, a zip archive or a disc image (.iso) of one, or a file in its folder.\n"
		  "Without it the game of the current folder is played, and where there is none the launcher is shown.\n"
		  "\n"
		  "  --engine=NAME          the engine profile, instead of detecting the game (--list-engines)\n"
		  "  --games=FILE           the openbgi.json to know the game by\n"
		  "  --productname=NAME     the product name the game's scripts ask for\n"
		  "  --overlay              play a folder without writing into it (saves go to the user's data)\n"
		  "  --patch=FILE           lay a zip archive or a folder of files over the game; may be repeated\n"
		  "  --lang=en|ja           the language of OpenBGI's own messages\n"
		  "  --launcher=dialog      without a game, ask with the system's dialog (=program: the launcher)\n"
		  "  --preview=none         the launcher makes no first pictures of games that were never played\n"
		  "  --dialogs=frame|host   message boxes drawn in the game's picture, or as windows\n"
		  "  --movies=decoder|player  movies by the built-in decoder, or by the system's player first\n"
		  "  --stand-in-font=gothic|mincho|auto  the built-in font that stands in for a missing one\n"
		  "  --own-pad-keys=on|off  take or ignore the keys a game gives a pad's controls itself\n"
		  "  --child-windows=frame|host  child windows drawn in the game's picture, or windows of their own\n"
		  "  --disc-marker          let the scripts see the disc's marker file\n"
		  "  --no-disc-check        the game counts as registered\n"
		  "  --enable=FAMILY:OP     make an opcode available whatever the profile says (--disable= removes one)\n"
		  "  --debug[=pause]        open the debugger with the game\n"
		  "\n"
		  "  --detect               print how the game is identified, and exit\n"
		  "  --list-engines         print the engine profiles, and exit\n"
		  "  --list-missing         print the opcodes of the profile that have no handler, and exit\n"
		  "  --list=ARCHIVE         print the entries of an archive, and exit\n"
		  "  --extract=ARCHIVE      write the entries of an archive into a folder, and exit\n"
		  "                         (--out=DIR, --entry=NAME, --raw, --convert)\n"
		  "  --version              print the version, and exit\n"
		  "  --help                 print this, and exit\n",
		out);
}

// 1 when the command line has the option `name` as a word of its own
static int Engine_HasOption(const char* cmd, const char* name)
{
	size_t n = strlen(name);
	const char* p;
	for(p = strstr(cmd, name); p; p = strstr(p + 1, name))
		if((p == cmd || p[-1] == ' ' || p[-1] == '\t') && (p[n] == 0 || p[n] == ' ' || p[n] == '\t'))
			return 1;
	return 0;
}

/* Take the "--" options out of the command line (they are blanked in
 * place) into the o->engine.opt* fields; an unknown option is reported
 * on stderr and ignored.  BGI_LANG is applied first so that --lang=
 * wins.  A token runs to the next space or tab outside double quotes,
 * and the quotes are not part of an option's text, so that a value may
 * hold spaces (--games="VN/my openbgi.json") and a quoted path is a
 * single token that is left alone. */
static void Engine_ParseOptions(OpenBGI_t* o, char* cmd)
{
	char* p = cmd;
	/* The language of the messages is one for the process.  A run out of
	 * sight (--preview) goes on beside the engine whose language it is,
	 * on a thread of its own: it takes the language as it finds it. */
	int ownLanguage = !Engine_HasOption(cmd, "--preview");
	// the language of the messages: Japanese on a Japanese system, English elsewhere - unless BGI_LANG or --lang= say
	if(ownLanguage && Host_KnowsUiLanguage(o))
		Msg_SetLanguage((Host_UiLanguage(o) & 0x3ff) == 0x11 ? "ja" : "en");
	if(ownLanguage && getenv("BGI_LANG") && !Msg_SetLanguage(getenv("BGI_LANG")))
		fprintf(stderr, "openbgi: unknown language in BGI_LANG (en or ja)\n");
	while(*p)
	{
		char opt[0x400];
		char* start;
		size_t k = 0;
		int quoted = 0;
		while(*p == ' ' || *p == '\t')
			p++;
		if(!*p)
			break;
		start = p;
		for(; *p && (quoted || (*p != ' ' && *p != '\t')); p++)
		{
			if(*p == '"')
				quoted = !quoted;
			else if(k + 1 < sizeof opt)
				opt[k++] = *p;
		}
		opt[k] = 0;
		if(k < 2 || opt[0] != '-' || opt[1] != '-')
			continue;
		if(strncmp(opt, "--engine=", 9) == 0)
			snprintf(o->engine.optEngine, sizeof o->engine.optEngine, "%s", opt + 9);
		else if(strncmp(opt, "--productname=", 14) == 0)
			snprintf(o->engine.optProduct, sizeof o->engine.optProduct, "%s", opt + 14);
		else if(strncmp(opt, "--games=", 8) == 0)
			snprintf(o->engine.optGames, sizeof o->engine.optGames, "%s", opt + 8);
		else if(strncmp(opt, "--enable=", 9) == 0 || strncmp(opt, "--disable=", 10) == 0)
		{
			int enable = opt[2] == 'e';
			if(!Engine_OverrideOpcode(o, opt + (enable ? 9 : 10), enable))
				fprintf(stderr, "openbgi: bad opcode in %s (expected FAMILY:HEX, e.g. 80:63)\n", opt);
		}
		else if(strncmp(opt, "--lang=", 7) == 0)
		{
			if(ownLanguage && !Msg_SetLanguage(opt + 7))
				fprintf(stderr, "openbgi: unknown language in %s (en or ja)\n", opt);
		}
		else if(strcmp(opt, "--list-engines") == 0)
			o->engine.optList = 1;
		else if(strcmp(opt, "--detect") == 0)
			o->engine.optDetect = 1;
		else if(strcmp(opt, "--version") == 0)
			o->engine.optVersion = 1;
		else if(strcmp(opt, "--help") == 0)
			o->engine.optHelp = 1;
		else if(strcmp(opt, "--list-missing") == 0)
			o->engine.optListMissing = 1;
		else if(strcmp(opt, "--debug") == 0 || strcmp(opt, "--debug=pause") == 0)
			o->engine.optDebug = opt[7] ? 2 : 1;
		else if(strcmp(opt, "--disc-marker") == 0)
			o->engine.discMarker = 1;
		else if(strcmp(opt, "--no-disc-check") == 0)
			o->engine.noDiscCheck = 1;
		else if(strcmp(opt, "--dialogs=frame") == 0 || strcmp(opt, "--dialogs=host") == 0)
			o->engine.optDialogs = opt[10] == 'f' ? 1 : 2;
		else if(strcmp(opt, "--child-windows=frame") == 0 || strcmp(opt, "--child-windows=host") == 0)
			o->engine.optChildFrame = opt[16] == 'f' ? 1 : 2;
		else if(strcmp(opt, "--movies=decoder") == 0 || strcmp(opt, "--movies=player") == 0)
			o->engine.optMovies = opt[9] == 'd' ? 1 : 2;
		else if(strcmp(opt, "--overlay") == 0)
			o->engine.optOverlay = 1;
		else if(strcmp(opt, "--preview") == 0)
			o->engine.optPreview = 1;
		else if(strcmp(opt, "--preview=none") == 0)
			o->engine.optPreviewNone = 1;
		else if(strcmp(opt, "--own-pad-keys=on") == 0 || strcmp(opt, "--own-pad-keys=off") == 0)
			o->engine.optOwnPadKeys = opt[16] == 'n' ? 1 : 2;
		else if(strncmp(opt, "--patch=", 8) == 0)
		{
			if(!opt[8] || o->engine.optPatchCount >= ENGINE_PATCHES)
				fprintf(stderr, "openbgi: %s: a file or a folder is needed, %d patches at most\n", opt, ENGINE_PATCHES);
			else
				snprintf(o->engine.optPatch[o->engine.optPatchCount++], sizeof o->engine.optPatch[0], "%s", opt + 8);
		}
		else if(strcmp(opt, "--launcher=dialog") == 0 || strcmp(opt, "--launcher=program") == 0)
			o->engine.optLauncherDialog = opt[11] == 'd';
		else if(strcmp(opt, "--stand-in-font=gothic") == 0 || strcmp(opt, "--stand-in-font=mincho") == 0 || strcmp(opt, "--stand-in-font=auto") == 0)
			o->engine.optStandIn = opt[16] == 'g' ? 1 : opt[16] == 'm' ? 2
																	   : 0;
		else if(strncmp(opt, "--list=", 7) == 0)
			snprintf(o->engine.optListArc, sizeof o->engine.optListArc, "%s", opt + 7);
		else if(strncmp(opt, "--extract=", 10) == 0)
			snprintf(o->engine.optExtract, sizeof o->engine.optExtract, "%s", opt + 10);
		else if(strncmp(opt, "--entry=", 8) == 0)
			snprintf(o->engine.optEntry, sizeof o->engine.optEntry, "%s", opt + 8);
		else if(strncmp(opt, "--out=", 6) == 0)
			snprintf(o->engine.optOut, sizeof o->engine.optOut, "%s", opt + 6);
		else if(strcmp(opt, "--raw") == 0)
			o->engine.optExtractMode = EXTRACT_RAW;
		else if(strcmp(opt, "--convert") == 0)
			o->engine.optExtractMode = EXTRACT_CONVERT;
		else
			fprintf(stderr, "openbgi: unknown option %s\n", opt);
		// blank the option out of the command line the original parser sees
		memset(start, ' ', (size_t)(p - start));
	}
}

// the openbgi.json of the user's data folder into `path`; 0 when there is no such folder
static int Engine_UserGamesPath(OpenBGI_t* o, char* path, size_t n)
{
	Host_UserDataDir(o, path, n);
	if(!path[0])
		return 0;
	strncat(path, "openbgi.json", n - strlen(path) - 1);
	return 1;
}

// the openbgi.json beside the binary into `path` (in the current directory where the host names no such place)
static void Engine_BinaryGamesPath(OpenBGI_t* o, char* path, size_t n)
{
	Host_BinaryDir(o, path, n);
	strncat(path, "openbgi.json", n - strlen(path) - 1);
}

int Engine_SaveGames(OpenBGI_t* o)
{
	char path[0x408];
	if(!o->engine.gamesPath[0] || o->engine.gamesStatic)
		return 0;
	if(Engine_SaveGamesJson(o, o->engine.gamesPath))
		return 1;
	if(o->engine.optGames[0] || getenv("BGI_GAMES"))
		return 0;
	/* A place that cannot be written to: the other of the two from now on -
	 * the user's data folder for a file beside a program installed for
	 * everyone, the place beside the program where the user has no data
	 * folder that takes a file. */
	if(Engine_UserGamesPath(o, path, sizeof path) && strcmp(path, o->engine.gamesPath) != 0 && Engine_SaveGamesJson(o, path))
	{
		snprintf(o->engine.gamesPath, sizeof o->engine.gamesPath, "%s", path);
		return 1;
	}
	Engine_BinaryGamesPath(o, path, sizeof path);
	if(strcmp(path, o->engine.gamesPath) != 0 && Engine_SaveGamesJson(o, path))
	{
		snprintf(o->engine.gamesPath, sizeof o->engine.gamesPath, "%s", path);
		return 1;
	}
	return 0;
}

/* A settings file that is there and cannot be used - a slip of the hand
 * in an editor - is not to be lost when the engine writes its own in that
 * place: a copy is kept beside it, as "<name>.bad". */
static void Engine_KeepBadGames(OpenBGI_t* o, const char* path)
{
	char bad[0x410];
	uint32_t size = 0;
	uint8_t* text = Host_ReadNativeFile(o, path, &size);
	if(!text)
		return; // no such file: nothing to keep
	snprintf(bad, sizeof bad, "%s.bad", path);
	if(Host_WriteNativeFile(o, bad, text, size))
		fprintf(stderr, "openbgi: %s cannot be used; it is kept as %s\n", path, bad);
	free(text);
}

/* One of the places openbgi.json is looked for by itself: 1 when the file
 * there is the one to use, 0 when there is none that can be read, -1 when
 * it is left alone - a file that cannot be written and does not say
 * "staticConfig": true (a word about it goes to stderr). */
static int Engine_TakeGames(OpenBGI_t* o, const char* path)
{
	if(!Engine_LoadGamesJson(o, path))
		return 0;
	if(o->engine.gamesStatic || Host_NativeWritable(o, path))
		return 1;
	fprintf(stderr, "openbgi: %s cannot be written and does not say \"staticConfig\": true: it is not used\n", path);
	Engine_ResetGames(o);
	return -1;
}

void Engine_LoadGames(OpenBGI_t* o)
{
	char path[0x408];
	int r;
	Engine_ResetGames(o);
	if(o->engine.optGames[0] || getenv("BGI_GAMES"))
	{ // the file that was asked for, as it is
		snprintf(o->engine.gamesPath, sizeof o->engine.gamesPath, "%s", o->engine.optGames[0] ? o->engine.optGames : getenv("BGI_GAMES"));
		if(!Engine_LoadGamesJson(o, o->engine.gamesPath))
		{
			fprintf(stderr, "openbgi: cannot read %s: the built-in profiles and games are used\n", o->engine.gamesPath);
			Engine_ResetGames(o);
			Engine_KeepBadGames(o, o->engine.gamesPath);
		}
		return;
	}
	Host_GameDir(o, path, sizeof path);
	strncat(path, "openbgi.json", sizeof path - strlen(path) - 1);
	if(Engine_TakeGames(o, path) > 0)
	{
		snprintf(o->engine.gamesPath, sizeof o->engine.gamesPath, "%s", path);
		return;
	}
	Engine_BinaryGamesPath(o, path, sizeof path);
	snprintf(o->engine.gamesPath, sizeof o->engine.gamesPath, "%s", path); // (where the defaults go without a user's data folder)
	r = Engine_TakeGames(o, path);
	if(r > 0)
		return;
	if(r == 0)
		Engine_KeepBadGames(o, path); // (a later write may fall back to this place)
	if(Engine_UserGamesPath(o, path, sizeof path))
	{
		snprintf(o->engine.gamesPath, sizeof o->engine.gamesPath, "%s", path);
		r = Engine_TakeGames(o, path);
		if(r > 0)
			return;
		if(r == 0)
			Engine_KeepBadGames(o, path);
	}
	Engine_ResetGames(o); // a file that failed to parse may have replaced nothing or everything: start clean
	Engine_SaveGames(o);
}

// the programs of the base directory's sysprg.arc into the evidence (none when there is no such archive)
static void Engine_AddSysprg(OpenBGI_t* o, DetectSet_t* set)
{
	char path[0x208];
	PathJoin(path, o->files.baseDir, "sysprg.arc");
	DetectSet_AddGameArchive(o, set, path);
}

/* Choose the engine profile and the product name from the game's own
 * programs (detect.h): the boot program as the engine will load it and
 * the programs of sysprg.arc, against the games table.  An explicit
 * --engine / BGI_ENGINE (or the launcher's choice) replaces the profile,
 * --productname= the product name.  Runs once the archive manager exists
 * and before the opcode tables are built; what was decided is said on
 * stderr. */
static void Engine_DetectVersion(OpenBGI_t* o)
{
	const char* forced = o->engine.optEngine[0] ? o->engine.optEngine : getenv("BGI_ENGINE");
	int isForced = forced && *forced;
	char arc[0x104], file[0x104];
	uint8_t* buf;
	uint32_t n = 0;
	BgiTryFrame_t frame;
	DetectSet_t set;
	DetectResult_t r;

	GetBootProgram(o, arc, file);
	buf = (uint8_t*)BGI_Alloc(LoadFile_BufferSize(o)); // a scratch buffer of the load limit, as the engine's own file-size probe uses
	if(BGI_TRY(o, frame))
	{
		n = LoadFile(o, buf, strcmp(arc, ".") == 0 ? NULL : arc, file);
		BGI_END_TRY(o, frame);
	}
	else
	{
		n = 0; // the boot program is missing: the VM reports it properly later
	}
	DetectSet_Init(&set);
	if(n)
		DetectSet_Add(o, &set, file, buf, n, 1);
	BGI_Free(buf);
	Engine_AddSysprg(o, &set);
	if(!set.count)
		fprintf(stderr, "openbgi: no programs to identify the game by: assuming engine %s\n", o->engine.profile->name);
	else if(Detect_Game(o, &set, isForced || o->engine.optPreview ? NULL : stderr, "openbgi: ", &r) != DETECT_NONE && !isForced)
	{
		if(Engine_SelectProfile(o, r.profile))
		{
			if(!o->engine.optPreview) // (a run nobody sees says nothing of what goes as expected)
				fprintf(stderr, "openbgi: engine profile %s selected\n", r.profile);
		}
		else
			fprintf(stderr, "openbgi: %s: unknown engine profile \"%s\": assuming engine %s\n", o->engine.gamesPath, r.profile, o->engine.profile->name);
	}
	if(set.count)
	{ // the product name: what the programs check for, else what the entry gives
		EngineGame_t g;
		if(r.product[0])
			Engine_SetProductName(o, r.product);
		else if(r.entry >= 0 && Engine_GameAt(o, r.entry, &g) && g.product)
			Engine_SetProductName(o, g.product);
	}
	// the input settings of openbgi.json: those for every game, then those of the entry that identified this one
	InputMap_Configure(o, Engine_InputAll(o), set.count ? Engine_GameInput(o, r.entry) : NULL);
	DetectSet_Free(&set);
	if(isForced && !Engine_SelectProfile(o, forced))
		fprintf(stderr, "openbgi: unknown engine profile \"%s\" (see --list-engines), using %s\n", forced, o->engine.profile->name);
	if(o->engine.optProduct[0])
	{ // --productname= replaces the product name found
		Engine_SetProductName(o, o->engine.optProduct);
		fprintf(stderr, "openbgi: product name \"%s\"\n", o->engine.optProduct);
	}
}

/* --detect: the report of the detection for the game directory - what
 * the engine would decide, then how every engine build fits the opcodes
 * of the programs - on stdout.  Returns the exit code: 0 when a profile
 * was found, 1 when there were no programs. */
static int Engine_PrintDetection(OpenBGI_t* o)
{
	char dir[0x400];
	DetectSet_t set;
	DetectResult_t r;
	int how;
	if(Fs_Mounted(o))
		snprintf(dir, sizeof dir, "%s", Fs_MountedPath(o));
	else
		Host_GameDir(o, dir, sizeof dir);
	DetectSet_Init(&set);
	DetectSet_LoadGame(o, &set, o->files.baseDir);
	printf("%s: %s\n", OPENBGI_TITLE, dir);
	if(!set.count)
	{
		printf("no ipl._bp, system.arc or sysprg.arc here\n");
		return 1;
	}
	printf("%d programs%s, games table %s\n", set.count, set.haveIpl ? " (ipl._bp first)" : " (no ipl._bp)", o->engine.gamesPath);
	how = Detect_Game(o, &set, stdout, "", &r);
	if(how != DETECT_NONE)
	{
		EngineGame_t g;
		const char* product = r.product[0] ? r.product : NULL;
		if(!product && r.entry >= 0 && Engine_GameAt(o, r.entry, &g))
			product = g.product;
		if(product)
			printf("=> engine profile %s, product name \"%s\"\n", r.profile, product);
		else
			printf("=> engine profile %s, product name unknown (--productname= gives it)\n", r.profile);
	}
	printf("how the engine builds fit the opcodes:\n");
	Detect_Explain(&set, stdout);
	DetectSet_Free(&set);
	return how == DETECT_NONE;
}

// ---- the start, over the first frames ----------------------------------------------
//
// OpenBGI_Start does what needs no answer and no time; the rest of the
// original's WinMain up to its main loop is taken a step at a time by
// the frames that follow (Engine_StartStep): the game chooser is a
// request, the processor's clock is measured over a second of frames,
// and every failure is a box that is answered before the run ends.

#define START_END      0 // nothing runs: not started, or the run is over (OpenBGI_Stop next, once the boxes are answered)
#define START_CHOOSE   1 // ask which game to run, when none is given
#define START_CHOSEN   2 // the chooser's answer
#define START_MUTEX    3 // the single-instance mutex, the processor checks
#define START_CPU      4 // the clock measurement runs
#define START_ENGINE   5 // the audio check, the engine's start-up, the boot
#define START_RUN      6 // the game runs: Vm_Frame
#define START_SOURCES  7 // the launcher asked whether its changed sources are kept
#define START_LAUNCHER 8 // the launcher is compiled and booted

/* Started outside a game directory - no boot path given and neither
 * system.arc nor ipl._bp where the engine runs: ask which game to run. */
static int Engine_NeedsChooser(OpenBGI_t* o)
{
	return o->engine.bootDefault && Fs_FileAttrs(o, "system.arc") == OPENBGI_INVALID_ATTRS && Fs_FileAttrs(o, "ipl._bp") == OPENBGI_INVALID_ATTRS;
}

/* The chooser's answer: the chosen directory becomes the game directory,
 * the profile and the options chosen under "Advanced" are applied as if
 * given on the command line.  Returns 1 to go on, 0 to leave (Quit), -1
 * when there was no display to ask on (said on stderr). */
static int Engine_GameChosen(OpenBGI_t* o, int answer)
{
	char profile[32], options[OPENBGI_GAME_CHOOSER_OPTIONS_MAX];
	int r = GameChooser_End(o, answer, o->engine.gamesPath, profile, sizeof profile, options, sizeof options);
	if(r < 0)
	{
		fprintf(stderr, "openbgi: no ipl._bp or system.arc here and no display to ask on: run from a game's directory or pass its path\n");
		return -1;
	}
	if(r == 0)
		return 0;
	SetBootProgram(o, ".", "ipl._bp"); // the base directory follows the new game directory
	Engine_LoadGames(o);               // which may carry a openbgi.json of its own
	if(profile[0])
		snprintf(o->engine.optEngine, sizeof o->engine.optEngine, "%s", profile);
	if(options[0])
		Engine_ParseOptions(o, options);
	return 1;
}

// the run is over (before the game ran, or after): OpenBGI_Stop returns `code`
static void Engine_End(OpenBGI_t* o, int code)
{
	o->engine.exitCode = code;
	o->engine.phase = START_END;
}

// a start-up check failed: its box (the original's direct MessageBox), then the end
static void Engine_Refuse(OpenBGI_t* o, const char* text)
{
	MsgBox_Plain(o, text, "Error!!", OPENBGI_MB_ICONHAND | OPENBGI_MB_SYSTEMMODAL);
	Engine_End(o, 0);
}

/* The steps of the start, until one waits for an answer or for time, or
 * the game runs or the run is over. */
static void Engine_StartStep(OpenBGI_t* o, OpenBGI_Frame_t* f)
{
	int exists = 0, answer;
	while(!Request_Pending(o))
	{
		switch(o->engine.phase)
		{
			case START_CHOOSE:
				if(!Engine_NeedsChooser(o))
					o->engine.phase = START_MUTEX;
				else if(Launcher_Wanted(o)) // the launcher: its sources are made ready, which may come with a question
					o->engine.phase = Launcher_Prepare(o) ? START_SOURCES : START_LAUNCHER;
				else
				{
					o->engine.chooserReq = GameChooser_Begin(o);
					o->engine.phase = START_CHOSEN;
				}
				break;
			case START_SOURCES:
				if(!Request_Take(o, o->launcher.askReq, &answer))
					return;
				Launcher_Answer(o, answer == OPENBGI_IDYES);
				o->engine.phase = START_LAUNCHER;
				break;
			case START_LAUNCHER:
				if(Launcher_Boot(o))
				{ // the launcher runs in place of a game, with the settings of its own directory (a file that was named is read already)
					if(!o->engine.optGames[0] && !getenv("BGI_GAMES"))
						Engine_LoadGames(o);
					o->engine.phase = START_MUTEX;
				}
				else
				{
					o->engine.chooserReq = GameChooser_Begin(o);
					o->engine.phase = START_CHOSEN;
				}
				break;
			case START_CHOSEN:
			{
				int r;
				if(!Request_Take(o, o->engine.chooserReq, &answer))
					return;
				o->engine.chooserReq = 0;
				r = Engine_GameChosen(o, answer);
				if(r <= 0)
					Engine_End(o, r < 0 ? 1 : 0);
				else
					o->engine.phase = START_MUTEX;
				break;
			}
			case START_MUTEX:
				if(!o->engine.noMutex)
				{
					o->engine.mutex = Host_InstanceLock(o, "OpenBGI Lock", &exists);
					if(exists)
					{
						// hand the boot path to the running instance and leave (not for the default system.arc)
						char arc[0x104], file[0x104], path[0x208];
						GetBootProgram(o, arc, file);
						if(strcmp(arc, "system.arc") != 0)
						{
							PathJoin(path, o->files.baseDir, arc);
							Host_SendToRunning(o, "BGI - Main window", path);
						}
						Engine_End(o, 0);
						break;
					}
				}
				if(!Host_HasCpuid(o))
				{ // no x86 processor to report (a page, another architecture): the processor checks are skipped
					o->engine.phase = START_ENGINE;
					break;
				}
				if(!Cpu_Detect(o))
				{
					Engine_Refuse(o, MSG_CPU_UNSUPPORTED);
					break;
				}
				o->engine.phase = START_CPU;
				break;
			case START_CPU:
				if(!Cpu_MeasureStep(o))
				{
					f->idleMs = 1; // the original measures in 1 ms sleeps
					return;
				}
				if(!Cpu_HasCmov(o) || !Cpu_HasMmx(o))
					Engine_Refuse(o, MSG_CPU_NO_INSN);
				else
					o->engine.phase = START_ENGINE;
				break;
			case START_ENGINE:
				if(!o->engine.launcherMode && !DSound8_IsAvailable(o))
				{
					Engine_Refuse(o, MSG_NO_DIRECTX);
					break;
				}
				Drives_Scan(o); // (the original probes SwapMouseButton here; the host reports the buttons swapped already)
				Dbg_Init(o);
				if(o->engine.optDebug)
					Dbg_RequestOpen(o, o->engine.optDebug == 2);
				o->engine.stage = ENGINE_STAGE_UP;
				if(!Engine_Init(o))
				{
					Engine_End(o, 0);
					break;
				}
				MainWin_ImeStatus(o, 0); // no input method while the game runs (ImmAssociateContext / IMC_CLOSESTATUSWINDOW)
				o->engine.stage = ENGINE_STAGE_VM;
				o->engine.exitCode = 0;
				o->engine.phase = START_RUN;
				Vm_Start(o);
				return;
			default: return; // START_RUN, START_END
		}
	}
}

// what OpenBGI_Start and OpenBGI_RunFrame report: frames follow while the game runs or a box of its end is up
static int Engine_Result(OpenBGI_t* o)
{
	return o->engine.phase == START_END && !Request_Pending(o) ? OPENBGI_DONE : OPENBGI_RUNNING;
}

// a run that ends in OpenBGI_Start: OpenBGI_Stop returns `code`
static int Engine_Done(OpenBGI_t* o, int code)
{
	Engine_End(o, code);
	return Engine_Result(o);
}

void OpenBGI_SetDrawing(OpenBGI_t* o, int on)
{
	Present_SetDrawing(o, on);
}

int OpenBGI_RunFrame(OpenBGI_t* o, OpenBGI_Frame_t* out)
{
	OpenBGI_Frame_t frame, *f = &frame; // whole here; the host gets what its own holds of it
	memset(f, 0, sizeof *f);
	Perf_Begin(o);       // the frame's start, for the performance display (while it is on)
	Screen_NextFrame(o); // the last frame's pixels are the host's to read no longer
	SndOut_Run(o);       // the sound up to now: the voices are where the scripts expect them
	if((o->engine.phase != START_END && !Request_Pending(o)) || Inframe_Up(o))
		InputMap_Frame(o);  // what the touch screen and the pads do to the mouse and the keys now: a pass's, or a dialog's drawn in the frame
	if(!Request_Pending(o)) // while one is, the engine holds still
	{
		if(o->engine.phase == START_RUN)
		{
			if(!Vm_Frame(o, f))
			{
				if(o->launcher.relaunch)
					Launcher_Relaunch(o); // the launcher started a game: its run begins in this one's place
				else
					Engine_End(o, o->engine.exitCode);
			}
		}
		else if(o->engine.phase != START_END)
			Engine_StartStep(o, f);
	}
	if(o->engine.phase == START_END && !Request_Pending(o) && Launcher_Returns(o))
		Launcher_Return(o); // a game the launcher started is over, its last box answered: the launcher's run begins in its place
	f->textInput = 0;       // (until the entry or a dialog takes text)
	memset(&f->textRect, 0, sizeof f->textRect);
	Edit_Frame(o, f);       // the text entry drawn in the frame
	Child_Frame(o, f);      // the child windows drawn in the frame
	Inframe_Frame(o, f);    // the dialog drawn in the frame
	Osk_Shown(o, f);        // the on-screen keyboard, and whether a text field has the keys
	Pad_Shown(o);           // the pointer drawn for a pad
	SndOut_Run(o);          // the time the frame took, with the sounds it started
	Screen_TakeFrame(o, f); // the window's picture and what changed in it since the last frame
	Settings_Frame(o, f);   // the settings over it, while they are up: a frame of their own engine
	Perf_End(o, f);         // the engine's work in the frame ends here for the performance display, which the layer shows
	Overlay_Frame(o, f);    // the layer over it, for a host that said its surface: the settings' picture, that display and the on-screen controls
	if(o->engine.phase == START_RUN)
		Launcher_Frame(o, f); // a picture of a game the launcher started, now and then
	f->expose = Present_Expose(o);
	Pointer_TakeFrame(o, f); // where the frame moved the pointer
	Host_Give(out, f, sizeof *f);
	return Engine_Result(o);
}

/* A run of a program the engine carries itself - the launcher's,
 * compiled (Launcher_Program) - on an engine that was never started:
 * no command line, no game directory, no settings file, no lock, no
 * sound, the launcher's profile.  The program is the boot program, and
 * stays its giver's.  What OpenBGI_Start returns. */
int Engine_StartBuiltin(OpenBGI_t* o, const uint8_t* program, uint32_t size)
{
	OpenBGI_Frame_t f;
	if(o->engine.ran || !program || !size)
		return OPENBGI_DONE;
	o->engine.ran = 1;
	o->files.builtinName = LAUNCHER_PROGRAM;
	o->files.builtinData = program;
	o->files.builtinSize = size;
	SetBootProgram(o, ".", LAUNCHER_PROGRAM);
	snprintf(o->engine.optEngine, sizeof o->engine.optEngine, "%s", LAUNCHER_PROFILE);
	o->engine.launcherMode = 1;
	o->engine.noMutex = 1;
	o->engine.phase = START_ENGINE;
	memset(&f, 0, sizeof f);
	Engine_StartStep(o, &f);
	return Engine_Result(o);
}

/*
 * WinMain up to its main loop.  The tool options (--list-*, --extract=,
 * --detect, --version) run and finish here; otherwise the game chooser
 * (when no game is given), the single-instance mutex (a running instance
 * is handed the boot path and this one leaves), the processor and audio
 * checks, the engine's start-up and the VM's boot follow over the first
 * frames.  The exit code OpenBGI_Stop returns is 0 for a game run, the
 * tool's result for --list= and --detect, and 1 for a failed --extract=
 * or no game to run.
 */
int Engine_Idle(OpenBGI_t* o)
{
	return o->engine.stage == ENGINE_STAGE_NONE && o->engine.phase == START_END;
}

int OpenBGI_Start(OpenBGI_t* o, const char* cmdLine)
{
	char cmd[0x2000];

	if(o->engine.ran)
	{ // not its first run: nothing of the one before remains
		if(!Engine_Idle(o))
			OpenBGI_Stop(o);
		Engine_Renew(o);
	}
	o->engine.ran = 1;
	snprintf(cmd, sizeof cmd, "%s", cmdLine ? cmdLine : "");
	BGI_Free(o->engine.startCmd);
	o->engine.startCmd = BGI_Strdup(cmd);
	Host_GameDir(o, o->engine.startDir, sizeof o->engine.startDir);
	o->launcher.hostQuit = 0; // (a quit the host asked for was the run's before this one)
	Engine_ParseOptions(o, cmd);
	if(o->engine.optVersion)
	{
		printf("%s (https://openbgi.net)\n", OPENBGI_TITLE);
		return Engine_Done(o, 0);
	}
	if(o->engine.optHelp)
	{
		Engine_PrintUsage(stdout);
		return Engine_Done(o, 0);
	}
	if(o->engine.optList)
	{
		Engine_ListProfiles(o, stdout);
		return Engine_Done(o, 0);
	}
	if(o->engine.optListMissing)
	{
		if(o->engine.optEngine[0] && !Engine_SelectProfile(o, o->engine.optEngine))
			fprintf(stderr, "openbgi: unknown engine profile \"%s\"\n", o->engine.optEngine);
		Vm_TablesInit(o);
		Vm_ListMissing(o, stdout);
		return Engine_Done(o, 0);
	}
	if(o->engine.optListArc[0])
		return Engine_Done(o, Extract_List(o, o->engine.optListArc, stdout));
	if(o->engine.optExtract[0])
	{
		int n;
		if(!o->engine.optOut[0])
		{ // the archive's name without its directory and extension
			const char* base = o->engine.optExtract;
			const char* p;
			char* dot;
			for(p = o->engine.optExtract; *p; p++)
				if(*p == '\\' || *p == '/')
					base = p + 1;
			snprintf(o->engine.optOut, sizeof o->engine.optOut, "%s", base);
			dot = strrchr(o->engine.optOut, '.');
			if(dot && dot != o->engine.optOut)
				*dot = 0;
		}
		n = Extract_Archive(o, o->engine.optExtract, o->engine.optEntry[0] ? o->engine.optEntry : NULL, o->engine.optOut, o->engine.optExtractMode, stderr);
		if(n < 0)
			return Engine_Done(o, 1);
		fprintf(stderr, "openbgi: %d entr%s written to %s\n", n, n == 1 ? "y" : "ies", o->engine.optOut);
		return Engine_Done(o, 0);
	}
	Boot_ParseCmdLine(o, cmd);
	Engine_LoadGames(o);
	if(o->engine.optDetect)
		return Engine_Done(o, Engine_PrintDetection(o));
	o->engine.phase = START_CHOOSE;
	{
		OpenBGI_Frame_t f;
		Engine_StartStep(o, &f);
	}
	return Engine_Result(o);
}

/* The rest of WinMain after its main loop: the VM's, the debugger's and
 * the engine's shutdown for as far as the start came, the mutex, and the
 * program "80 E1" asked for.  Requests not answered by now are dropped; a
 * box saying that program did not start is a request made after the
 * shutdown, which the host answers before OpenBGI_Delete.  Where the
 * original leaves memory to the end of its process, everything is freed
 * here: a stopped engine holds nothing of its run. */
int OpenBGI_Stop(OpenBGI_t* o)
{
	Settings_Close(o); // the settings over this run go with it
	if(o->engine.chooser)
		GameChooser_End(o, 0, NULL, NULL, 0, NULL, 0); // the chooser was not answered
	// the dialog in the frame goes; boxes from now on are the host's
	Inframe_Free(o);
	Perf_Stop(o); // the performance display's characters and picture
	InputMap_Stop(o);
	Request_Clear(o);
	o->engine.phase = START_END;
	if(o->engine.stage >= ENGINE_STAGE_VM)
	{
		Vm_Stop(o);
		MainWin_ImeStatus(o, 1);
	}
	if(o->engine.stage >= ENGINE_STAGE_UP)
	{
		Dbg_Shutdown(o);
		Engine_Shutdown(o);
	}
	o->engine.stage = ENGINE_STAGE_NONE;
	Screen_NextFrame(o); // (the last frame's pixels, kept for the host until here)
	if(o->engine.mutex)
	{
		Host_InstanceUnlock(o, o->engine.mutex);
		o->engine.mutex = NULL;
	}

	// "80 E1": the program the script asked to be started now is not (bgi/install.h)
	if(o->engine.exitFile)
	{
		Program_Refuse(o, o->engine.exitArg0, o->engine.exitFile);
		BGI_Free(o->engine.exitFile);
		BGI_Free(o->engine.exitArg0);
		o->engine.exitFile = o->engine.exitArg0 = NULL;
	}
	Launcher_RunEnded(o); // the folders the launcher read; the play time and the picture of a game it started
	Engine_FreeRest(o);   // a stopped engine keeps nothing but a box still to answer
	return o->engine.exitCode;
}
