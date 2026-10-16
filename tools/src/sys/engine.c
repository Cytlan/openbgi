/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * engine.c - start-up and shutdown (Engine_Main, Engine_Init,
 *            Engine_CreateCore, Engine_Shutdown) and the command line;
 *            interface in bgi/engine.h
 *
 * Engine_Main is the original's WinMain without the Windows glue: the
 * single-instance mutex, the CPU check and the COM initialisation live in
 * the OS layer, the sequence of steps is the original's.  Before that the
 * reimplementation's own "--" options are taken off the command line
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
#include "bgi/os.h"

int gLauncherMode;                       // "Execute as a launcher.": no sound, no saved window position
int gNoMutex;                            // "Do not use mutex.": several instances may run
int gDiscMarker;                         // --disc-marker: "80 34" reports the disc marker file
int gBootDefault;                        // no boot path on the command line: the game directory is where the engine runs
int gEngineUp;                           // between Engine_Init's success and Engine_Shutdown
char *gExitArg0, *gExitFile, *gExitArg2; // "80 E1": the program to start after the shutdown (dir, file, message)

// ---- command line --------------------------------------------------------

// the number of spaces at s
static int CmdLine_SkipSpaces(const char* s)
{
	int n = 0;
	while(s[n] == ' ')
		n++;
	return n;
}

/* copy one token to dst; returns the characters consumed.  A token in
 * double quotes may contain spaces; an unterminated quote yields 0 (which
 * ends the scan). */
static int CmdLine_Token(char* dst, const char* s)
{
	int n = 0;
	if(*s == '"')
	{
		s++;
		for(;;)
		{
			char c = *s;
			if(c == '\n' || c == 0)
			{
				*dst = 0;
				return 0;
			}
			if(c == '"')
			{
				*dst = 0;
				// an empty "" never advances in the original either: it ends the scan
				return n > 0 ? n + 2 : 0;
			}
			*dst++ = c;
			s++;
			n++;
		}
	}
	while(*s != ' ' && *s != '\n' && *s != 0)
	{
		*dst++ = *s++;
		n++;
	}
	*dst = 0;
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

/* The original's command line, "<boot path> [option]": the path names the
 * directory, the archive or the program file to boot from ("." and
 * ipl._bp without one); the option is "Execute as a launcher." or "Do not
 * use mutex.", anything else is ignored. */
void Boot_ParseCmdLine(const char* cmdLine)
{
	char a[0x104], b[0x104];
	char* bufs[3] = {a, b, NULL};
	int n = CmdLine_Split(bufs, cmdLine);
	gBootDefault = n == 0 || strcmp(a, ".") == 0; // where the engine runs, as the original's "." means
	if(n == 0)
	{
		SetBootProgram(".", "ipl._bp");
		return;
	}
	SetBootProgram(a, "ipl._bp");
	if(n == 2)
	{
		gLauncherMode = strcmp(b, "Execute as a launcher.") == 0;
		gNoMutex = strcmp(b, "Do not use mutex.") == 0;
	}
}

// ---- initialisation ------------------------------------------------------

static void Engine_DetectVersion(void);

// the global memory ("80 70") and the system area ("80 82" / "80 83"); always 1
static int Mem_Init(void)
{
	SetGlobalMemSize(4); // 0x10000 bytes
	// the system area of "80 82 / 83": 0x40000 bytes up to 1.599, 0x100000 from 1.616;
	// the larger block is always allocated, the size the scripts see follows the profile
	gSysAreaSize = 0x40000;
	gSysArea = (uint8_t*)BGI_Calloc(0x100000);
	return 1;
}

// the system area size of the generation (after the profile is known)
static void Mem_SetSysAreaSize(void)
{
	gSysAreaSize = gEngine->gen >= GEN_1_616 ? 0x100000 : 0x40000;
}

/* The worker pool, the compositor, the bitmap manager and the archive
 * manager.  The band size of the compositor (in pixels) follows the CPU
 * caches: L2 size in KB * 64; an L1 data cache of 64 KB or more overrides
 * that with L1 KB * 76.8, a smaller one on a multi-processor machine with
 * 0x2ee0; 0xffffffff when no cache size is known. */
static void Engine_CreateCore(void)
{
	uint32_t band = 0xffffffffu;
	int cpus = OS_CpuCount();
	if(gSysInfo.l2)
		band = ((gSysInfo.l2 & 0xffffu) << 8) >> 2;
	if(gSysInfo.l1)
	{
		uint32_t l1 = gSysInfo.l1 & 0xffffu;
		if(l1 >= 0x40)
			band = (l1 * 3 * 256) / 10;
		else if(cpus > 1)
			band = 0x2ee0;
	}
	Gfx_Create(band, cpus); // gGfx, gBmpMgr
	gArcMgr = FileSet_New();
}

/* Bring the engine up in the original's order: lock, graphics core,
 * timers, the window, memory, the initial display mode, the system area,
 * sound (not in launcher mode), then the engine profile and everything
 * that depends on it.  0 when start-up failed (an error box was shown). */
int Engine_Init(void)
{
	EngineLock_Init();
	Engine_CreateCore();
	Timer_Begin();
	Idle_Init();
	if(!MainWindow_Create())
		return 0;
	// the original loads the arrow and its resource cursor here; the OS layer's window has them
	if(!Mem_Init())
		return 0;
	SetDisplayMode(2, 1, 0); // the initial mode: size index 2 (800x600), 32-bit pixels, windowed
	SysArea_Init();
	if(!gLauncherMode && !Sound_Init())
		return 0;
	DynMem_Init();
	Engine_DetectVersion();
	Mem_SetSysAreaSize();
	Display_InitSizeTable(); // the generation's picture sizes (index 2 is 800x600 in all of them)
	Vm_TablesInit();         // (static data in the original)
	UserOp_Init();
	Loader_Start();
	Child_InitTable();
	Ptcl_InitGlobals();
	Blit_InitMmxTables();
	gEngineUp = 1;
	return 1;
}

// Tear everything down in the original's order (the window is already gone by then).
void Engine_Shutdown(void)
{
	gEngineUp = 0;
	Sound_CdClose();
	Ptcl_AutoClear();
	Timer_End();
	Idle_Shutdown();
	Movie_Stop();
	BmSeq_FreeAll(); // the bitmap sequences of 1.69 build 472 on
	if(gFullscreen)
		OS_DisplayRestore();
	Panel_DeleteAll();
	BGI_Free(gGlobalMem);
	gGlobalMem = NULL;
	BGI_Free(gSysArea);
	gSysArea = NULL;
	Gfx_Destroy(); // Gfx, BmpMgr, ArcMgr, pools
	SysArea_Shutdown();
	Sound_Shutdown();
	DynMem_FreeAll();
	UserOp_Clear();
	History_Reset(0);
	Ring_DeleteAll();
	StrTab_Clear(0);
	Dict_DeleteAll();
	OS_EditDestroy();
	Child_DestroyAll();
	Ptcl_FreeGlobals();
	Knob_UnregisterAll();
	Knob_ReleaseAll();
	Input_ResetLayers(); // the layers are cleared and the base layer re-pushed
	WinMsgWait_Clear();
	MsgQueue_Clear();
	Target_Clear();
	Watch_Clear();
	Loader_Stop();
	EngineLock_Delete();
	FileSearch_Clear();
}

// ---- the reimplementation's own command-line options ---------------------------
//
// The original takes "<boot path> [option]" only.  Options starting with
// "--" are ours and are removed before the rest is handed to
// Boot_ParseCmdLine:
//   --engine=NAME      select the engine profile (docs/versions.md) instead
//                      of detecting the game; BGI_ENGINE=NAME does the same
//   --games=PATH       the games.json to detect the game with (default: the
//                      game directory's own, else the one next to the
//                      executable, written there when missing; BGI_GAMES=PATH)
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
//   --debug            open the debugger (docs/debugger.md) with the game;
//                      --debug=pause holds the threads until F5 (F12 in the
//                      game's window opens and closes it at any time)
//   --disc-marker      let the scripts see the disc marker file - the file
//                      named after the product that the discs carry; every
//                      boot program quits at once when it finds one next to
//                      the game, so it is hidden from "80 34" by default
static char gOptEngine[32];     // --engine=
static char gOptGames[0x104];   // --games=
static int gOptList;            // --list-engines
static int gOptListMissing;     // --list-missing: the opcodes of the profile without a handler
static char gOptListArc[0x200]; // --list=
static char gOptExtract[0x200]; // --extract=
static char gOptEntry[0x104];   // --entry=
static char gOptOut[0x200];     // --out=
static int gOptExtractMode;     // EXTRACT_DECODE / RAW / CONVERT
static int gOptDebug;           // --debug: 1 open the debugger, 2 open it paused

/* Take the "--" options out of the command line (they are blanked in
 * place) into the gOpt* variables; an unknown option is reported on
 * stderr and ignored.  BGI_LANG is applied first so that --lang= wins. */
static void Engine_ParseOptions(char* cmd)
{
	char* p = cmd;
	if(getenv("BGI_LANG") && !Msg_SetLanguage(getenv("BGI_LANG")))
		fprintf(stderr, "bgi: unknown language in BGI_LANG (en or ja)\n");
	while(*p)
	{
		char* start;
		char* end;
		while(*p == ' ' || *p == '\t')
			p++;
		start = p;
		while(*p && *p != ' ' && *p != '\t')
			p++;
		end = p;
		if(end - start < 2 || start[0] != '-' || start[1] != '-')
			continue;
		*end = 0;
		if(strncmp(start, "--engine=", 9) == 0)
			snprintf(gOptEngine, sizeof gOptEngine, "%s", start + 9);
		else if(strncmp(start, "--games=", 8) == 0)
			snprintf(gOptGames, sizeof gOptGames, "%s", start + 8);
		else if(strncmp(start, "--enable=", 9) == 0 || strncmp(start, "--disable=", 10) == 0)
		{
			int enable = start[2] == 'e';
			if(!Engine_OverrideOpcode(start + (enable ? 9 : 10), enable))
				fprintf(stderr, "bgi: bad opcode in %s (expected FAMILY:HEX, e.g. 80:63)\n", start);
		}
		else if(strncmp(start, "--lang=", 7) == 0)
		{
			if(!Msg_SetLanguage(start + 7))
				fprintf(stderr, "bgi: unknown language in %s (en or ja)\n", start);
		}
		else if(strcmp(start, "--list-engines") == 0)
			gOptList = 1;
		else if(strcmp(start, "--list-missing") == 0)
			gOptListMissing = 1;
		else if(strcmp(start, "--debug") == 0 || strcmp(start, "--debug=pause") == 0)
			gOptDebug = start[7] ? 2 : 1;
		else if(strcmp(start, "--disc-marker") == 0)
			gDiscMarker = 1;
		else if(strncmp(start, "--list=", 7) == 0)
			snprintf(gOptListArc, sizeof gOptListArc, "%s", start + 7);
		else if(strncmp(start, "--extract=", 10) == 0)
			snprintf(gOptExtract, sizeof gOptExtract, "%s", start + 10);
		else if(strncmp(start, "--entry=", 8) == 0)
			snprintf(gOptEntry, sizeof gOptEntry, "%s", start + 8);
		else if(strncmp(start, "--out=", 6) == 0)
			snprintf(gOptOut, sizeof gOptOut, "%s", start + 6);
		else if(strcmp(start, "--raw") == 0)
			gOptExtractMode = EXTRACT_RAW;
		else if(strcmp(start, "--convert") == 0)
			gOptExtractMode = EXTRACT_CONVERT;
		else
			fprintf(stderr, "bgi: unknown option %s\n", start);
		// blank the option out of the command line the original parser sees
		memset(start, ' ', (size_t)(end - start));
		*end = ' ';
		p = end + 1;
	}
}

static char gGamesPath[0x408]; // the games.json in use (Engine_LoadGames)

const char* Engine_GamesJsonPath(void)
{
	return gGamesPath;
}

/* Find and read games.json: --games= / BGI_GAMES name it; otherwise the
 * game directory's own comes first, then the one beside the binary - and
 * when there is none there either, the built-in defaults are written to
 * that place (quietly kept in memory when the directory cannot be
 * written to, as from a read-only location) and used. */
static void Engine_LoadGames(void)
{
	char path[0x408];
	Engine_ResetGames();
	if(gOptGames[0] || getenv("BGI_GAMES"))
	{
		snprintf(gGamesPath, sizeof gGamesPath, "%s", gOptGames[0] ? gOptGames : getenv("BGI_GAMES"));
		if(!Engine_LoadGamesJson(gGamesPath))
			fprintf(stderr, "bgi: cannot read %s: the built-in profiles and games are used\n", gGamesPath);
		return;
	}
	OS_GameDir(path, sizeof path);
	strncat(path, "games.json", sizeof path - strlen(path) - 1);
	if(Engine_LoadGamesJson(path))
	{
		snprintf(gGamesPath, sizeof gGamesPath, "%s", path);
		return;
	}
	OS_BinaryDir(path, sizeof path);
	strncat(path, "games.json", sizeof path - strlen(path) - 1);
	snprintf(gGamesPath, sizeof gGamesPath, "%s", path);
	if(Engine_LoadGamesJson(path))
		return;
	Engine_ResetGames(); // a file that failed to parse may have replaced nothing or everything: start clean
	Engine_SaveGamesJson(path);
}

/* Choose the engine profile: the boot program (ipl._bp) is read and
 * matched against the games table, which names the profile and the
 * product name; an explicit --engine / BGI_ENGINE (or the launcher's
 * choice) replaces the profile.  Runs once the archive manager exists and
 * before the opcode tables are built.  Every failure keeps the default
 * profile and says so on stderr. */
static void Engine_DetectVersion(void)
{
	const char* forced = gOptEngine[0] ? gOptEngine : getenv("BGI_ENGINE");
	char arc[0x104], file[0x104];
	uint8_t* buf;
	uint32_t n = 0;
	BgiTryFrame_t frame;

	GetBootProgram(arc, file);
	buf = (uint8_t*)BGI_Alloc(0x2000000); // a 32 MB scratch buffer, as the engine's own file-size probe uses
	if(BGI_TRY(frame))
	{
		n = LoadFile(buf, strcmp(arc, ".") == 0 ? NULL : arc, file);
		BGI_END_TRY(frame);
	}
	else
	{
		n = 0; // the boot program is missing: the VM reports it properly later
	}
	if(n)
	{ // the game's entry names its profile and its product name; a forced profile replaces the former only
		const char* prof = Engine_DetectGame(buf, n);
		if(prof && !(forced && *forced))
			fprintf(stderr, "bgi: engine profile %s selected for this game\n", prof);
		else if(!prof && !(forced && *forced))
			fprintf(stderr, "bgi: game not in %s: assuming engine %s\n", gGamesPath, gEngine->name);
	}
	BGI_Free(buf);
	if(forced && *forced && !Engine_SelectProfile(forced))
		fprintf(stderr, "bgi: unknown engine profile \"%s\" (see --list-engines), using %s\n", forced, gEngine->name);
}

/* Started outside a game directory - no boot path given and neither
 * system.arc nor ipl._bp where the engine runs: ask which game to run.
 * The chosen directory becomes the game directory, the profile and the
 * options chosen under "Advanced" are applied as if given on the command
 * line.  Returns 1 to go on, 0 to leave (Quit), -1 when there was no
 * display to ask on (said on stderr). */
static int Engine_ChooseGame(void)
{
	char profile[32], options[OS_LAUNCHER_OPTIONS_MAX];
	int r;
	if(!gBootDefault || OS_FileAttrs("system.arc") != OS_INVALID_ATTRS || OS_FileAttrs("ipl._bp") != OS_INVALID_ATTRS)
		return 1;
	r = Launcher_Run(gGamesPath, profile, sizeof profile, options, sizeof options);
	if(r < 0)
	{
		fprintf(stderr, "bgi: no ipl._bp or system.arc here and no display to ask on: run from a game's directory or pass its path\n");
		return -1;
	}
	if(r == 0)
		return 0;
	SetBootProgram(".", "ipl._bp"); // the base directory follows the new game directory
	Engine_LoadGames();             // which may carry a games.json of its own
	if(profile[0])
		snprintf(gOptEngine, sizeof gOptEngine, "%s", profile);
	if(options[0])
		Engine_ParseOptions(options);
	return 1;
}

/*
 * WinMain: the whole life of the process.  The tool options (--list-*,
 * --extract=) run and return before anything else; otherwise the
 * single-instance mutex is taken (a running instance is handed the boot
 * path and this one leaves), the processor and audio checks run, the
 * engine is brought up, the VM runs until the scripts end or the window
 * closes, everything is torn down and the "80 E1" program, if any, is
 * started.  Returns the process exit code: 0 for a game run, the tool's
 * result for --list= and 1 for a failed --extract=.
 */
int Engine_Main(const char* cmdLine)
{
	OsMutex_t* mutex = NULL;
	int exists = 0;
	char cmd[0x400];

	snprintf(cmd, sizeof cmd, "%s", cmdLine ? cmdLine : "");
	Engine_ParseOptions(cmd);
	if(gOptList)
	{
		Engine_ListProfiles(stdout);
		return 0;
	}
	if(gOptListMissing)
	{
		if(gOptEngine[0] && !Engine_SelectProfile(gOptEngine))
			fprintf(stderr, "bgi: unknown engine profile \"%s\"\n", gOptEngine);
		Vm_TablesInit();
		Vm_ListMissing(stdout);
		return 0;
	}
	if(gOptListArc[0])
		return Extract_List(gOptListArc, stdout);
	if(gOptExtract[0])
	{
		int n;
		if(!gOptOut[0])
		{ // the archive's name without its directory and extension
			const char* base = gOptExtract;
			const char* p;
			char* dot;
			for(p = gOptExtract; *p; p++)
				if(*p == '\\' || *p == '/')
					base = p + 1;
			snprintf(gOptOut, sizeof gOptOut, "%s", base);
			dot = strrchr(gOptOut, '.');
			if(dot && dot != gOptOut)
				*dot = 0;
		}
		n = Extract_Archive(gOptExtract, gOptEntry[0] ? gOptEntry : NULL, gOptOut, gOptExtractMode, stderr);
		if(n < 0)
			return 1;
		fprintf(stderr, "bgi: %d entr%s written to %s\n", n, n == 1 ? "y" : "ies", gOptOut);
		return 0;
	}
	Boot_ParseCmdLine(cmd);
	Engine_LoadGames();
	{
		int r = Engine_ChooseGame();
		if(r <= 0)
			return r < 0 ? 1 : 0;
	}
	if(!gNoMutex)
	{
		mutex = OS_InstanceMutexCreate("Buriko General Interpreter for Tayutama is executing.", &exists);
		if(exists)
		{
			// hand the boot path to the running instance and leave (not for the default system.arc)
			char arc[0x104], file[0x104], path[0x208];
			GetBootProgram(arc, file);
			if(strcmp(arc, "system.arc") != 0)
			{
				PathJoin(path, gBaseDir, arc);
				OS_IpcSendToRunning("BGI - Main window", path);
			}
			return 0;
		}
	}
#if !defined(BGI_WASM) // no CPUID on a page: the processor checks are skipped there
	if(!Cpu_Detect())
	{
		OS_MessageBox(MSG_CPU_UNSUPPORTED, "Error!!", OS_MB_ICONHAND | OS_MB_SYSTEMMODAL);
		return 0;
	}
	if(!Cpu_HasCmov() || !Cpu_HasMmx())
	{
		OS_MessageBox(MSG_CPU_NO_INSN, "Error!!", OS_MB_ICONHAND | OS_MB_SYSTEMMODAL);
		return 0;
	}
#endif
	if(!gLauncherMode && !DSound8_IsAvailable())
	{
		OS_MessageBox(MSG_NO_DIRECTX, "Error!!", OS_MB_ICONHAND | OS_MB_SYSTEMMODAL);
		return 0;
	}
	Drives_Scan();
	Mouse_ProbeSwap();
	Dbg_Init();
	if(gOptDebug)
		Dbg_RequestOpen(gOptDebug == 2);
	if(Engine_Init())
	{
		OS_ImeStatus(0); // no input method while the game runs (ImmAssociateContext / IMC_CLOSESTATUSWINDOW)
		VmMain();
		OS_ImeStatus(1);
	}
	Dbg_Shutdown();
	Engine_Shutdown();
	if(mutex)
		OS_InstanceMutexRelease(mutex);

	// "80 E1": start the program the script asked for
	if(gExitFile)
	{
		RunProgram(gExitArg0, gExitFile, gExitArg2, 0, 0, 0, 0);
		BGI_Free(gExitFile);
		BGI_Free(gExitArg0);
		BGI_Free(gExitArg2);
		gExitFile = gExitArg0 = gExitArg2 = NULL;
	}
	return 0;
}
