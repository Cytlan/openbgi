/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * core.h - the engine's state: OpenBGI_t, one struct per engine
 *             (src/core/openbgi.c); not part of the original engine
 *
 * Everything the engine keeps between calls is a field of OpenBGI_t, so
 * that several engines can share a process, an engine can be dropped and
 * started again from nothing, and its state can be saved one day.  Every
 * function that reads or changes that state, itself or through the
 * functions it calls, takes the engine as its first parameter, `OpenBGI_t*
 * o`: opcode handlers, methods and callbacks as well.  There is no other
 * way to an engine.
 *
 * The fields are grouped by the part of the engine they belong to
 * (o->text, o->vm, o->snd, ...).  A field is named after the original's
 * global it reproduces, without the `g`: gBaseDir is o->files.baseDir.
 *
 * What stays global is what no engine owns: constant tables, tables
 * computed once from nothing (the CRC and deflate code tables, the JPEG
 * cosines, the scaler weights), the language of OpenBGI's own messages
 * (gMsgEnglish, the host's choice) and the hosts' own state.
 *
 * A host makes, runs and frees an engine through openbgi.h and needs
 * nothing from this header.
 */
#ifndef BGI_CORE_H_
#define BGI_CORE_H_

#include "bgi/common.h"
#include "openbgi.h"
#include "bgi/error.h"
#include "bgi/file.h"
#include "bgi/fs.h"
#include "bgi/fontfile.h"
#include "bgi/sys.h"
#include "bgi/version.h"
#include "bgi/vm.h"
#include "bgi/follow.h"
#include "bgi/wait.h"
#include "bgi/sysobj/flags.h"
#include "bgi/sysobj/locks.h"
#include "bgi/sysobj/names.h"
#include "bgi/display.h"
#include "bgi/input.h"
#include "bgi/edit.h"
#include "bgi/install.h"
#include "bgi/gfx.h"
#include "bgi/gfx/bitmap.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/gfx/compositor.h"
#include "bgi/gfx/dispobj.h"
#include "bgi/gfx/screens.h"
#include "bgi/gfx/text.h"
#include "bgi/sound.h"
#include "bgi/snd/wavemaster.h"
#include "bgi/snd/sndout.h"
#include "bgi/dbg.h"
#include "bgi/request.h"
#include "bgi/launcher.h"
#include "bgi/overlay.h"
#include "bgi/settings.h"
#include "bgi/perf.h"

struct OpenBGI
{
	/* the host's services (openbgi.h), from OpenBGI_New: the engine's own
	 * copies, whole whatever the host's sizes of them (Host_Take) - `host`
	 * points at the tables here */
	OpenBGI_Host_t host;
	struct HostTables
	{
		OpenBGI_FontFiles_t fontFiles;
		OpenBGI_FontRenderer_t fontRenderer;
		OpenBGI_TextEntry_t textEntry;
		OpenBGI_ChildWindows_t childWindows;
		OpenBGI_Files_t files;
		OpenBGI_Registry_t registry;
		OpenBGI_Window_t window;
		OpenBGI_DebugWindow_t debugWindow;
		OpenBGI_Machine_t machine;
		OpenBGI_Shell_t shell;
		OpenBGI_CdAudio_t cdAudio;
		OpenBGI_MoviePlayer_t moviePlayer;
		OpenBGI_Threads_t threads;
		OpenBGI_MovieDecoder_t movieDecoder;
	} hostTables;

	// what the engine asks the host (src/sys/request.c)
	struct
	{
		Request_t items[REQUEST_MAX]; // the queue's slots
		uint32_t serial;              // the id the last request got
		int pending;                  // requests made and not yet answered: the engine holds still
	} req;

	// the error handling: the try frames, the kept errors (src/core/error.c)
	struct
	{
		BgiTryFrame_t* tryTop; // the innermost active frame
		int keepErrors;        // "81 6A"
		char* keptError;       // the first error's message while keeping; NULL when none
	} err;

	// the clock, the timer, the random generator, the scrambler, the CPU (src/core/sysutil.c, src/sys/cpu.c)
	struct
	{
		// src/core/sysutil.c
		uint32_t clockBase;    // subtracted from the system clock
		uint32_t clockFrozen;  // the value while stopped; 0 while running (a clock stopped at 0 runs on)
		uint32_t clockLast;    // the system clock at the previous reading (1.573 on)
		uint32_t clockMaxStep; // the longest step that counts as time passing
		int clockStopInactive; // "81 68": stop while the window is inactive
		int clockStopped;      // 1.547 on: a stop is in effect (the next one is not counted)
		uint32_t randSeed;     // the CRT's holdrand, initially 1
		uint32_t hashKey;      // the scrambler's state; it starts as the bytes "90\0\0", i.e. 0x3039
		// src/sys/cpu.c
		SysInfo_t sysInfo;
		int cpuMeasuring;  // the clock measurement runs (over the frames of the start)
		uint32_t cpuUntil; // its end (GetTicks)
		uint64_t cpuTsc0;  // the time stamp counter at its start
		uint32_t cpuKept;  // what it gave in the run before this one (Engine_Renew): this run takes it and does not measure; 0 for an engine's first run
	} util;

	// the text encoding of the scripts (src/core/strutil.c)
	struct
	{
		int textUtf8; // the mode: 1 in UTF-8 mode
		// kKinsoku in the current code space, rebuilt when the encoding changes
		uint32_t kinsokuCodes[64];
		int kinsokuCount;
		int kinsokuMode; // the textUtf8 it was built for, -1 not yet
	} enc;

	// the global memory, the system area and the dynamic blocks (src/core/dynmem.c)
	struct
	{
		uint8_t* globalMem;                   // the global memory block (tag 0)
		uint32_t globalMemSize;               // its size in bytes
		uint8_t* sysArea;                     // the system area of "80 82" / "80 83" (0x100000 bytes allocated)
		uint32_t sysAreaSize;                 // the size the scripts see: 0x40000 up to 1.599, 0x100000 from 1.616
		uint8_t* dynBlocks[VM_DYN_SLOTS_MAX]; // one block per tag from dynFirst (48 slots in 1.69)
		uint32_t dynSizes[VM_DYN_SLOTS_MAX];  // .. and the bytes each has
		int dynReady;                         // DynMem_Init has run
		DynPool_t dynPools[5];
	} mem;

	// paths, the file layer, archives, loading and saving (src/fs/, src/sys/fileacc.c)
	struct
	{
		// src/fs/paths.c
		char baseDir[0x104];  // the game's directory, with trailing '\'
		char altDir[0x104];   // disc directory ("80 3F", with trailing '\') or ""
		char bootArc[0x104];  // the archive of the boot program, "system.arc"
		char bootFile[0x104]; // the boot program, "ipl._bp"
		// "81 3F": the engine's own program file (file.h)
		char programPath[0x104];                  // its path; "" until a program asks for it
		uint8_t programImage[PROGRAM_IMAGE_SIZE]; // its bytes
		// "80 36" and "80 37"
		int fileSearchOn;              // "80 36": loose files are also looked for in the search sub-directories
		struct SearchNode* searchList; // "80 37": the sub-directories tried after a directory, the newest first
		// the disc prompt of "80 3F", kept for the retry prompts of the loader
		char installName[0x104];
		char installMsg[0x104];
		char installTitle[0x104];
		uint32_t driveType[26]; // OPENBGI_DRIVE_* per letter, from Drives_Scan
		int driveNotFixed[26];  // 1 unless the drive is a fixed disk: its medium is checked by DriveReady
		// src/fs/fs.c
		FsState_t fs;
		const char* builtinName;    // a file the engine carries itself, as a file of the game directory (the launcher's program); NULL for none
		const uint8_t* builtinData; // its bytes (not the engine's to free)
		uint32_t builtinSize;
		// src/fs/arc.c
		FileSet_t* arcMgr;      // the head of the archive chain, created at start-up
		FileCache_t* fileCache; // NULL until "90 03" creates it
		/* the engine's own cache in front of LoadFile: packed archive
		 * files as they were unpacked, so that a file a program loads
		 * again and again is unpacked once (LoadFile_FreeCache) */
		FileCache_t* unpackCache; // NULL until the first load from an archive, and with --unpack-cache=0
		int unpackCacheOff;       // there is none: it was asked for and is not to be
		int packed;               // the load under way unpacked its file
		uint32_t unpackHits;      // loads the cache answered (for the tests)
		// src/fs/loadfile.c
		char emptyPath[8]; // zero and only ever read: the empty path prefix of FindFiles, the seed byte of Window_DrawText's tag buffer
		// src/fs/loader.c
		int loaderRun;            // between Loader_Start and Loader_Stop
		int inLoad;               // a loader job runs: a disc prompt is the loader's (RetryPrompt)
		struct LoadDefer* defer;  // a background load that takes the unpacking on itself (LoadFile_Unpack); NULL: LoadFile unpacks
		struct LoadJob* loadJobs; // the jobs waiting behind a disc prompt, the one that asked first
		Retry_t loadRetry;        // that prompt
		// src/fs/savefile.c
		int saveEncode; // "80 74": scramble the save files (the default)
		// src/sys/fileacc.c
		struct FileAccNode* fileAcc; // the open files, the newest first
		uint32_t fileAccLastId;      // the last handle number given out (the first is 1)
	} files;

	// the engine profile, the games table, the command line and the life cycle (src/core/version.c, src/sys/engine.c)
	struct
	{
		// src/core/version.c
		const EngineProfile_t* profile; // the selected profile, never NULL
		// the profiles in use: the built-in ones, or those of openbgi.json (the names live in profileNames)
		EngineProfile_t profiles[ENGINE_PROFILES_MAX];
		char profileNames[ENGINE_PROFILES_MAX][ENGINE_PROFILE_NAME_MAX];
		int profileCount;
		struct GameEntry* games; // the entries of openbgi.json in file order
		int gameCount;
		struct InputSettings* inputAll;    // the file's top-level "input" object (bgi/inputcfg.h), NULL for none
		struct CfgSet* launcherCfg;        // the file's top-level "launcher" object: the launcher's settings, NULL for none
		int moviesPlayer;                  // the file's "movies": "player" - the host's own movie player before its decoder
		int gamesStatic;                   // the file's "staticConfig": true - it is used as it is and never written
		char product[0x100];               // the product name of the running game
		int8_t override[OPFAM_COUNT][256]; // 0 none, 1 disabled, 2 enabled
		// src/sys/engine.c
		int launcherMode;                     // "Execute as a launcher.": no sound device is opened
		int noMutex;                          // "Do not use mutex.": no single-instance mutex
		int discMarker;                       // --disc-marker: the disc marker file (a file named like the product) is reported to the scripts
		int noDiscCheck;                      // --no-disc-check: "80 EC" counts the game as registered without comap.dat
		int bootDefault;                      // no boot path was given: the game directory is where the engine runs
		int engineUp;                         // between a successful Engine_Init and Engine_Shutdown
		int ran;                              // OpenBGI_Start was called: the next one renews the engine first
		int stage;                            // ENGINE_STAGE_*: how far OpenBGI_Start came
		int phase;                            // the step of the start the next frame takes (engine.c)
		struct GameChooser* chooser;          // the game chooser while it is up (gamechooser.c)
		uint32_t chooserReq;                  // its request
		int exitCode;                         // what OpenBGI_Stop returns
		void* mutex;                          // the host's single-instance lock (Host_InstanceLock), NULL when not taken
		char *exitArg0, *exitFile;            // "80 E1": the program the script asked to be started after the shutdown (dir, file)
		char optEngine[32];                   // --engine=
		char optProduct[0x100];               // --productname=
		char optGames[0x104];                 // --games=
		char* startCmd;                       // the command line of this start as the host gave it (BGI_Free; the launcher's goes with the game it starts)
		char startDir[0x408];                 // .. and the game directory it began in, a native path (what stands in a game's place is looked for there)
		int optList;                          // --list-engines
		int optListMissing;                   // --list-missing: the opcodes of the profile without a handler
		char optListArc[0x200];               // --list=
		char optExtract[0x200];               // --extract=
		char optEntry[0x104];                 // --entry=
		char optOut[0x200];                   // --out=
		int optExtractMode;                   // EXTRACT_DECODE / RAW / CONVERT
		int optDebug;                         // --debug: 1 open the debugger, 2 open it paused
		int optDetect;                        // --detect: the detection report
		int optVersion;                       // --version
		int optUnpackCache;                   // --unpack-cache=MB: the megabytes plus one (0: not given - UNPACK_CACHE_MB, file.h)
		int optThreads;                       // --threads=N: the threads a frame is painted on (0: not given - BGI_THREADS, else one for each processor)
		int optHelp;                          // --help
		int optDialogs;                       // --dialogs=: 0 as the host has them (OPENBGI_CAP_DIALOGS), 1 in the frame, 2 the host's
		int optChildFrame;                    // --child-windows=: 1 frame (in the frame, though the host has its own), 2 host (the host's, though the window is fitted to its screen)
		int optMovies;                        // --movies=: 0 as openbgi.json says, 1 the host's decoder, 2 its player first
		int optOverlay;                       // --overlay: a game folder is played as an archive is, without writing into it
		int optPreview;                       // --preview: a run nobody sees, for the launcher's first picture (as --overlay, and without the notes)
		int optPreviewNone;                   // --preview=none: the launcher has no first pictures made
		int optOwnPadKeys;                    // --own-pad-keys=: 0 as openbgi.json says, 1 on, 2 off
		char optPatch[ENGINE_PATCHES][0x400]; // --patch=, in the order given
		int optPatchCount;
		int optStandIn;        // --stand-in-font=: 0 by the face's name, 1 gothic, 2 mincho
		int optLauncherDialog; // --launcher=dialog: a start without a game asks the host for one, though there is a launcher
		char gamesPath[0x408]; // the openbgi.json in use (Engine_LoadGames)
	} engine;

	// the launcher: its program, and what is kept of the game it started over the engine's restart (src/sys/launcher.c)
	LauncherState_t launcher;

	// the screen layer and the on-screen controls drawn in it (src/sys/overlay.c)
	OverlayState_t overlay;

	// the settings over a running game: the engine that runs them, or - in that engine - the game's (src/sys/settings.c)
	SettingsState_t settings;

	// the performance display: what the run measures of itself, and the panel that shows it (src/sys/perf.c)
	PerfState_t perf;
	struct Async* async; // the helpers for work beside the engine's thread, once there was any (bgi/async.h); kept over a restart

	// the string encoding of the assembler, the compilers and the decompilers (src/asm/strlit.c)
	struct
	{
		int asmUtf8;        // the literals are kept in UTF-8 right now
		int asmUtf8Default; // what a file starts with (the -u option)
	} tools;

	// the script machine: threads, the scheduler, the opcode tables (src/vm/)
	struct
	{
		// the debugger's hooks the scheduler reads (src/dbg/dbg.c; constant without the debugger)
		int dbgHooked;         // 1 while Dbg_OnInsn must be called (breakpoints or tracing)
		uint32_t dbgInsnLimit; // the instructions a turn may execute: 0x100000, 1 while stepping
		// src/vm/thread.c
		struct Thread* rootThread; // list head, never runs; its id is 0
		uint32_t threadSerial;     // the id the next thread gets
		// src/vm/sched.c
		uint32_t switchThreadId;        // target of scheduler code 3 ("80 5E")
		int exclusiveOn;                // 1 while only exclusiveThread gets turns ("80 5D")
		struct Thread* exclusiveThread; // that thread
		SchedFlags_t sched;             // the flags the main loop's catch blocks modify
		int frameOpen;                  // a frame ran its pass; the next frame finishes it first (Vm_EndFrame)
		int runEnded;                   // the run is over: the list emptied without a reboot, or a boot failed
		// the pass that waits (not in the original, whose waits were inside the instruction)
		struct Thread* holder;   // the thread whose WaitStep holds the engine: no other thread runs
		struct Thread* resumeAt; // the thread the pass goes on with; NULL when no pass waits
		int servicesDue;         // the pass is over, its services wait for a box to be answered
		int inPoll;              // a wait is polled (a disc prompt there cannot start its instruction over)
		int booting;             // the boot program loads (its disc prompt is the boot's)
		Retry_t bootRetry;       // the boot's disc prompt
		void* temp[VM_TEMP_MAX]; // Vm_TempAlloc's blocks
		int tempCount;
		// src/vm/follow.c
		Follow_t follow; // the memory that holds what a program made of the engine's own program file
		// src/vm/ops_base.c
		VmHandler_t optable7F[256];   // the families of later builds (docs/dev/versions.md): "7F xx", 1.667 on
		VmHandler_t optable81[256];   // "81 xx", 1.69 build 472 on (ops_sys81.c)
		VmHandler_t optableD0[256];   // "D0 xx", 1.535 on
		VmHandler_t optableE0[256];   // "E0 xx", 1.588 on
		VmHandler_t optableEF[256];   // "EF xx", the OpenBGI extensions (ops_obgi.c): every generation
		VmHandler_t optablemain[256]; // the base instructions and the family dispatchers
		// src/vm/ops_sys.c
		VmHandler_t optable80[256]; // system control
		// src/vm/ops_gfx0.c
		VmHandler_t optable90[256]; // graphics 0
		// src/vm/ops_gfx1.c
		VmHandler_t optable91[256]; // graphics 1
		// src/vm/ops_gfx2.c
		VmHandler_t optable92[256]; // graphics 2
		// src/vm/ops_snd.c
		VmHandler_t optableA0[256]; // sound
		// src/vm/ops_ext0.c
		VmHandler_t optableB0[256]; // extension 0
		// src/vm/ops_ext1.c
		VmHandler_t optableC0[256]; // extension 1
		// src/vm/userop.c
		struct UserOp* userOps[USEROP_SLOTS]; // the registrations, NULL for a free slot
		// src/vm/ops_base.c
		struct WatchNode* watchList; // the registered regions; Watch_Clear frees them
	} vm;

	// what single instructions keep (src/vm/ops_*.c)
	struct
	{
		// src/vm/ops_sys.c
		int discMarkerSaid; // a hidden disc marker was reported on stderr (once)
		// src/vm/ops_sys81.c
		int32_t uiLanguage;                     // "81 02" / "81 03": the language of the user interface
		int windowPosLock;                      // "81 65": the window may not be moved
		int resizableWindow;                    // "81 66": the window has a sizing border
		uint32_t touchMaxPoints, touchDistance; // "81 16": the touch tracking asked for (stored only)
		int printScreenTaken;                   // "81 69": the Print Screen key is taken (stored only)
		int adapterOfMonitor;                   // "81 62": present through the adapter of the window's monitor
		// src/vm/ops_gfx0.c
		uint32_t syncLoadMs;    // the length of the window in ms; 0 = no window set
		uint32_t syncLoadUntil; // the tick the open window closes at; 0 = not open
		// src/vm/ops_gfx1.c
		struct FaceAdjust* faceAdjust; // "91 0E": the list of corrections, one entry per face
		int presentAlways;             // "91 00": present without a pending request (stored only)
		int sizeLimitSelect;           // "91 09": which of two stored size limits applies (stored only)
		int propFontAdvance;           // "91 0D": no half-width advance adjustment for variable-pitch faces (stored only)
		// src/vm/ops_ext1.c
		int ptclAnimMode; // the mode "C0 1F" selected, read by "C0 18"
	} ops;

	// the tracing aids (src/vm/trace.c)
	struct
	{
		int vmTraceOn;                                  // any of them is on: the scheduler calls the hooks
		int vmTrace;                                    // print (or record) every instruction: BGI_TRACE, BGI_TRACE_LAST or BGI_TRACE_OPS is set
		const char* vmTraceProg;                        // the comma-separated programs of BGI_TRACE, NULL for all
		uint32_t vmTraceFrom;                           // milliseconds after the first instruction before printing starts
		uint8_t vmTraceOps[OPFAM_COUNT][256];           // [family][opcode] = 1 to trace it
		int vmTraceOpsOn;                               // BGI_TRACE_OPS named at least one instruction: only those are printed
		int vmTraceOpsPending;                          // a traced instruction is executing: print its result after it
		struct TraceEntry* traceRing;                   // BGI_TRACE_LAST entries, NULL without it
		int traceRingSize, traceRingPos, traceRingFull; // capacity, next slot to write, 1 once it wrapped
		const char* vmStatFile;                         // the output file, NULL without BGI_OPSTAT
		uint32_t vmStat[8][256];
		uint32_t traceT0; // the tick of the first traced instruction (BGI_TRACE_FROM counts from it)
	} trace;

	// the wait objects (src/wait/)
	struct
	{
		// src/wait/wait.c
		int waitBlocking;    // 0 = every wait ends at once ("80 50")
		int presentOnWait;   // a dirty wait requests a present ("90 0A")
		int presentFull;     // ... of the whole picture rather than the dirty rectangles
		uint32_t waitSerial; // the next serial handed out by Wait_Ctor
		int32_t waitCounted; // waits with work outstanding on another OS thread
		// src/wait/wait_icon.c
		int selectRequireActive; // "90 AF": selections take input only while the engine window is active
		// src/wait/wait_menu.c
		int menuRequireActive;        // "90 AF": the menu takes input only while the engine window is active
		uint32_t menuBlinkMs;         // "90 A5": the blink interval in ms
		uint32_t menuCursorColour[2]; //          phase; 0 hides the cursor in that phase
		int menuWheelOn;              // "90 A6": the wheel moves the pointer
		int menuWheelCurve;           // to the next entry with this
		int menuWheelDuration;        // MouseMove_Start curve, duration
		int menuWheelFps;             // and rate
	} wait;

	// the script-visible system objects (src/sysobj/)
	struct
	{
		// src/sysobj/flags.c
		FlagMgr_t* flagMgr; // the engine's flag groups (created by SysArea_Init, database.h)
		// src/sysobj/names.c
		char nameTable[GNAME_COUNT_MAX][GNAME_SIZE]; // the fixed name table of the builds before 1.616 (unused, but kept zeroed, by the later ones)
		// src/sysobj/locks.c
		SemMgr_t* semMgr;
		// src/sysobj/dicts.c
		struct DictNode* dictList; // every dictionary, newest first
		uint32_t dictSerial;       // the next id
		// src/sysobj/events.c
		struct MsgNode* msgHead; // the oldest event, NULL when the queue is empty
		struct MsgNode* msgTail; // the newest event, NULL when the queue is empty
		// src/sysobj/history.c
		struct HistNode* histOldest; // the two ends of the list, NULL when empty
		struct HistNode* histNewest;
		uint32_t histMax;   // records allowed (History_Reset)
		uint32_t histCount; // records in the list
		// src/sysobj/rings.c
		struct Ring* ringList; // every ring, newest first
		uint32_t ringSerial;   // the last id handed out
		// src/sysobj/strgroups.c
		struct StrGroup* strGroups; // every group, newest first
		// src/sysobj/strtables.c
		struct StrTab* strTabList; // every table, oldest first
		// src/sysobj/targets.c
		struct Target* targetHead; // every target, newest first
		uint32_t targetCount;      // registrations since the last clear (the next index)
	} sysobj;

	// the display mode and the main window (src/sys/display.c, window.c)
	struct
	{
		// src/sys/display.c
		int fullscreen;      // the current mode is full screen
		int sizeIndex;       // the picture size: an index into the size table
		int pixelMode;       // 0 = 16-bit, 1 = 32-bit pixels
		int displayEnabled;  // "90 01": nothing is presented while 0
		int screenWidths[8]; // the picture sizes by size index (Display_InitSizeTable, "81 60")
		int screenHeights[8];
		int frameW, frameH;           // what the window manager adds around the client area, pixels
		int screenW, screenH;         // the screen size (halved for two monitors side by side)
		int aspectClass;              // 0 4:3 and others, 1 5:4, 2 16:10, 3 5:3, 4 1024x600, 5 16:9
		int posMode;                  // "80 63": letter-box into the next larger full-screen mode
		int inSetDisplayMode;         // SetDisplayMode is running (its window changes must not re-enter it)
		int styleKeysOn;              // "80 62": the style keys are active
		uint32_t styleKeys[16];       // the keys that toggle windowed / full screen, 0-terminated
		int windowSizeW, windowSizeH; // "81 64" (1.529 on): the client size of the window, 0 for the picture size
		int vsyncWait;                // "80 6E" (1.69/472 on): wait for the vertical blank before the flip (stored only)
		int fullscreenFit;            // "81 63" (1.529 on): 0 stretched, 1 keeping the aspect ratio, 2 unscaled (stored only)
		int renderW, renderH;         // "81 6C": the picture size the 1.669 renderer scales to (stored only)
		int bicubic;                  // "81 6F" (1.616 on): the bicubic scaler is on (the original's pixel shader)
		// src/sys/window.c
		int windowActive;         // the window has the focus
		int windowRestored;       // the window is not minimised
		int minimizedByScript;    // "80 65" minimised the window; restoring it resumes the sound
		int closeMode;            // "80 68": 1 closing destroys the window, 0 posts event 2 and lets the script decide
		int windowAlive;          // set by WM_CREATE, cleared by WM_DESTROY: the window can be drawn to
		int windowExists;         // the window has been created and not yet destroyed (gHWnd != NULL in the original)
		int knobDragL, knobDragR; // a button press went to a knob: its release does not reach the input state
	} display;

	// the main window as the engine asks for it (src/sys/mainwin.c) and the messages it posts itself (window.c)
	struct
	{
		int alive, shown; // made and not yet destroyed; shown
		int x, y, w, h;   // where it is: the outer origin, the client size (what the window nobody sees answers)
		int focus[8];     // the window nobody sees: the focus changes its shows and hides made, for the next pumps
		int focusCount;   // ... how many wait
		int closePosted;  // "80 69" / Alt+F4 posted WM_CLOSE: the next OpenBGI_PumpEvents delivers it
		int quitPosted;   // WM_QUIT (the window is gone, OpenBGI_Quit): the next OpenBGI_PumpEvents stops the machine
		/* the view (mainwin.c): the window's real size on a host that says
		 * its screen's scale and room (OpenBGI_Window_t.workArea); x, y, w,
		 * h above stay what the scripts asked for */
		int fullscreen;         // the window covers the screen (MainWin_SetFullscreen)
		int viewDpi;            // the screen's scale in dots per inch as last read (0: not yet; 96: none)
		int realW, realH;       // the client area the host's window was given, or the player gave it
		int userW, userH;       // .. as the player sized it (0: not)
		int userVW, userVH;     // .. and the client size the scripts had asked for then: when they ask for another, the player's goes
		int aspectW, aspectH;   // the proportions the host was last told to keep (0: any)
		int gameRule;           // the scaling rule the game asked for (OPENBGI_SCALE_BILINEAR / _BICUBIC)
		int setSize;            // the settings (MainWin_ViewApply): the window's size, VIEW_SIZE_*
		int setFilter;          // .. the filter, VIEW_FILTER_*
		char setFilterName[32]; // .. by its name when it is one of the host's own (it may not be there in this run: kept all the same)
		int setHigh;            // .. and its better, slower kind
		int setWhole;           // .. by whole steps alone
	} window;

	// presenting the picture (src/sys/present.c)
	struct
	{
		int presentPending;       // a present was requested
		int presentPendingFull;   // ... of the whole picture
		int presentAllowed;       // Present_Allow: 0 while the screen quake presents on its own
		uint32_t frameInterval;   // ms between presents
		uint32_t nextPresentTime; // the tick of the next present (on the interval grid)
		uint32_t frameCount;      // frames presented; nothing reads it
		int profileOn;            // "80 06": the render-time profiler runs
		int64_t profileBudget;    // 96 % of a display period, in nanoseconds
		int64_t profileTotal;     // the render time of all passes, nanoseconds
		uint32_t profilePasses;   // passes timed
		uint32_t profileSlow;     // passes that took at least the budget
		int64_t profileStart;     // the performance counter at the start of the pass
		int idleSleepMs;          // "80 52": ms; 0 = the idle wait
		Rect_t dirtyRects[0x400]; // Present_Dirty's rectangles; the compositor holds at most 0x400
		int drawOff;              // OpenBGI_SetDrawing(0): a present paints nothing
		int drawStale;            // .. and one has: the back buffer is not what the objects show
	} present;

	// the window's picture: what the main window shows, handed to the host (src/sys/screen.c)
	struct
	{
		uint8_t* mem;                          // the pixels, NULL without a picture
		Bmp_t pic;                             // a view of mem
		int changes;                           // the rectangles in `changed` since the last frame; -1 all of it
		Rect_t changed[OPENBGI_FRAME_CHANGES]; // inclusive, picture coordinates
		struct
		{
			const uint32_t* pix;    // 0x00RRGGBB (0xAARRGGBB with alpha), NULL while the slot is free
			int pitch, x, y, w, h;  // pixels: its rows, and its place in the picture
			int alpha;              // composited over the picture rather than put on it
		} overlay[SCREEN_OVERLAYS]; // drawn over the picture in the frames (Screen_Overlay)
		uint8_t* shown;             // the picture with the overlays, what the frames hand over while one is up; NULL otherwise
		size_t shownSize;           // its bytes
		const uint8_t* handed;      // the pixels the last frame handed to the host, which may read them until the next frame
		uint8_t* kept;              // .. and those pixels once the engine has dropped them in between: freed with the next frame (Screen_NextFrame)
	} screen;

	// the dialogs drawn in the frame (src/sys/inframe.c)
	struct
	{
		struct Inframe* up; // the dialog that is up and its state, NULL while none is
		int stopped;        // OpenBGI_Stop ran: the boxes are the host's from then on
	} inframe;

	// window message waits and dropped files (src/sys/winmsg.c)
	struct
	{
		WinMsgRec_t* winMsgList; // one record per subscribed (thread, message)
		int dropEnabled;         // "80 6C": the window accepts dropped files
		char droppedFile[0x104]; // the last dropped path (MAX_PATH bytes); empty when none
	} winmsg;

	// keys, mouse buttons, input layers and knobs (src/sys/input.c, knobreg.c)
	struct
	{
		// src/sys/input.c
		int inputEnable;                    // "80 10": the layers report nothing while 0
		uint32_t stdKeys[STDKEY_COUNT][16]; // 0-terminated lists
		uint32_t skipKeys[16];              // the skip keys (bit 31), 0-terminated
		uint32_t wheelUpKeys[3];            // group 0xc3: up + wheel up
		uint32_t wheelDownKeys[3];          // group 0xc4: down + wheel down
		uint32_t padKeys[PAD_CONTROLS];     // the virtual key each control taps, 0 none
		KeyRec_t keyState[256];             // by virtual key
		int skipKeyEnable;                  // "80 14": the skip keys count
		int skipLatch;                      // "80 15": the skip function is held on without a key
		int skipFlushed;                    // set by Input_Flush: a held skip key does not count until released
		uint32_t inputSerial;               // bumped on every input message ("80 13")
		int swapButtons;                    // "80 1E": 1 exchanges the left and right buttons
		int keysWhenInactive;               // "81 14": key states are read while the window is inactive
		uint32_t tweenKeyMask;              // "81 1F": the standard keys the tween waits break on (with 0x180)
		struct InputLayer* mouseLayers;     // highest key first
		struct InputLayer* keyLayers;       // highest key first
		struct FocusNode* focusList;        // highest key first
		// src/sys/knobreg.c
		int knobGlobal;            // "90 DD": the wheel goes to the captured knob instead of the wheel keys
		KnobReg_t knobHead;        // a dummy element whose `next` is the list of registered knobs, by layer key
		KnobReg_t* knobDrag;       // the knob being dragged, NULL when none
		int knobDragX, knobDragY;  // last mouse position seen by the drag (back-buffer coordinates)
		KnobCapture_t captureHead; // (its `next` is the list)
		// src/sys/window.c: what the host reported (OpenBGI_Key, OpenBGI_Pointer, OpenBGI_Button)
		uint8_t keyboard[256];  // by virtual key, as GetKeyboardState: bit 7 down, bit 0 flipped by every press
		int pointerX, pointerY; // the pointer, in pixels of the frame's picture
		int pointerMoved;       // the engine moved it since the last frame (OpenBGI_Frame_t.pointerMoved)
		int hostFocus;          // the host's window has the focus (OpenBGI_Focus)
		int childFocus;         // one of the child windows the engine draws is the active window instead
		// src/sys/inputmap.c
		InputProbe_t probes[INPUT_PROBES]; // what the scripts tested the pointer with lately (Input_NoteProbe)
		// src/sys/steps.c, touch.c, pad.c, osk.c: the input mapper
		InputSteps_t steps;       // the mouse and key steps still to take
		TouchState_t touch;       // the touches
		PadMap_t pad;             // the pads, the focus and the drawn pointer
		Osk_t osk;                // the on-screen keyboard
		int device;               // INPUT_DEV_*: what the player used last
		InputHotspot_t* hotspots; // the game's hotspots (InputMap_Configure), NULL for none
		int hotspotCount;
		const struct InputSettings *cfgAll, *cfgGame; // the settings file's layers in use (InputMap_Configure),
		struct InputSettings* cfgHost;                // .. and the host's over them (OpenBGI_InputSettings), NULL for none,
		struct InputSettings* cfgHostBase;            // .. with what the host says under them (its "defaults"), NULL for none
		int oskAsked;                                 // the on-screen keyboard comes only when asked for ("onScreenKeyboard": false)
		int settingsKey;                              // the virtual key that opens the display settings ("settingsKey"; F9), 0 for none
		int settingsHeld;                             // .. is down, and its release is not the game's either
	} input;

	// the mouse cursor (src/sys/cursor.c)
	struct
	{
		int cursorShape;      // 0..4, "80 67"
		int mvActive;         // a move is in progress
		int mvStart[2];       // start (back-buffer coordinates)
		int mvDelta[2];       // target - start
		int mvPos[2];         // last position set
		int mvCurve;          // 1 = cosine ease, else linear
		uint32_t mvStep;      // steps done
		uint32_t mvSteps;     // steps in all
		uint32_t mvDuration;  // ms
		uint32_t mvStartTick; // GetTicks at the start
		uint32_t mvNextTick;  // when the next step is due
		int mvCancelOnMove;   // stop when the user moves the mouse
		int hideEnabled;      // auto-hide is on
		int hideShown;        // the cursor is currently shown by the auto-hide
		int hideHeld;         // the auto-hide's hide is on the host's count
		uint32_t hideDelay;   // ms of rest before the cursor is hidden
		uint32_t hideAt;      // the tick at which it will be hidden
		int hideLast[2];      // the position last seen (0x80000000: none yet)
		int cursorShown;      // the engine's own ShowCursor state
		uint32_t curSprite;   // sprite id, 0 = none
		int curDx, curDy;     // the sprite's offset from the cursor position, pixels
		int curLast[2];       // the cursor position the sprite was last placed at
	} cursor;

	// the button panels and procs (src/sys/objproc.c)
	struct
	{
		struct Gfx* panelGfx;         // the graphics manager the panels draw through
		struct BmpMgr* panelBmpMgr;   // the bitmap manager the button bitmap numbers refer to
		uint32_t procSerial;          // the last id handed out
		int procPresent;              // a dirty proc asks for a present
		uint32_t procCount;           // procs registered since the last Panel_DeleteAll
		struct ProcNode* procHead;    // the most recently registered proc first
		int panelPollMode;            // "80 AF" (1.573 on): 0 the panels are polled before the input, 1 at the end of the pass
		uint32_t panelKeyMaps[4][24]; // "91 BF": the user-defined key maps 4 .. 7, one action per input bit
		int panelKeyMapsStale;        // the maps have not been cleared yet
		int panelRequireActive;       // "90 AF": panels take input only while the window is active
	} panel;

	// the child windows (src/sys/child.c)
	struct
	{
		ChildWindow_t children[CHILD_SLOTS]; // by the scripts' child index
		int childTableUp;                    // the table has been initialised and not yet torn down
		// the windows drawn in the frame
		int active;             // the window clicked last (Ctrl+C is its), -1 none
		int drag, dragX, dragY; // the window dragged by its caption (-1 none), the pointer's offset from its origin
		int pressed;            // the buttons pressed on a window (a bit each): their releases are its too
		int zNext;              // the next stacking order
		int dirty;              // the windows are to be placed and drawn again
		int picW, picH;         // the picture size they were placed for
		struct Typeface* face;  // the captions' font, made on first use
		struct UiFont* font;    // its UiFont_t (made with it)
	} child;

	// the text entry (src/sys/edit.c)
	struct
	{
		char editText[EDIT_TEXT_MAX]; // empty until "B0 26" (the control itself is created with MSG_EDIT_DEFAULT_TEXT)
		Rect_t editRect;              // the client rectangle of the last Edit_Setup, inclusive
		uint32_t editColour;          // COLORREF (0x00BBGGRR)
		int editWidthPct;             // "B0 22": the glyph width of the font in percent of the normal
		int rejectAscii;              // "B0 29": single-byte printable characters are refused
		int closeOnEnter;             // "B0 28": Enter hides the control as well
		int align;                    // "B0 2A": 0 left, 1 centred, 2 right
		int made;                     // 1.69/472 on: a control exists, from a setup until "B0 21" or the window's end
		int flags;                    // OPENBGI_ENTRY_* of the last setup (and "B0 28" since)
		int own;                      // the engine draws the entry although the host has one: a pad had the turn (Edit_Setup, Edit_ToFrame)
		int fontNo, size, maxLen;     // the font, its size and the text limit of the last setup
		struct EditFrame* frame;      // the entry the engine draws in the frame (without a host's), made on its first use
	} edit;

	// the installer (src/sys/install/)
	struct
	{
		InstallCtx_t inst;       // the running installation (Install_Step)
		MadeList_t madeList;     // the files created so far
		char** discMarkers;      // per disc: the file that identifies it
		char** discMsgs;         // per disc: the "insert disc" message
		const int* discCounts;   // per disc: the files on it (the files are listed disc by disc)
		int hvlCount;            // records of BGI.hvl loaded for this installation; 0 = no verification
		uint8_t* hvl;            // the records (0x40 bytes each)
		char dlgPath[0x104 + 4]; // the path the dialogs edit (the original's global has room past 0x104)
		int registrationSaid;    // CheckRegistration (machine.c) told its outcome on stderr
		int dlgAlive;            // the progress dialog is open (the user has not cancelled)
		int promptOpen;          // a copy job shows a message box (kept as state; nothing reads it here)
	} install;

	// the graphics manager, bitmaps and display objects (src/gfx/)
	struct
	{
		// src/gfx/mgr/gfxmgr.c
		Gfx_t* gfx;               // the manager (dispGfx below is the same pointer)
		BmpMgr_t* gfxBmpMgr;      // the bitmap manager the wrappers look bitmaps up in (Gfx_SetBmpMgrPtr)
		WorkerPool_t* workerPool; // the compositor's threads
		// src/gfx/bmpmgr.c
		BmpMgr_t* bmpMgr; // the one manager of the engine
		// src/gfx/bitmap.c
		int screenMode;                    // pixel mode of the back buffer
		int seeThrough;                    // "EF 31": a 32-bit screen is made with alpha (ARGB32), and no background is drawn
		int bmpOption;                     // "90 0F": the key colour put ARGB32 pictures were composited over, 0 = none
		int blitSse, blitSse2, blitTuning; // the CPU switches of the original's blitter selection
		int blitTablesReady;               // Blit_InitTables was called
		// src/gfx/dispobj.c
		Gfx_t* dispGfx;              // the graphics manager (set by DispObj_SetGfxPtr)
		BmpMgr_t* dispBmpMgr;        // the bitmap manager (set by DispObj_SetBmpMgrPtr)
		int globalEffectValue;       // "90 08": stored only
		int32_t dispGlobalOffset[2]; // 1.616 on: "91 06", added to the draw position of the objects that follow it
		// src/gfx/effector.c
		struct EffNode* effectors; // every live effector, newest first
		// src/gfx/window/window.c
		int windowsShown; // visibility switch for all windows ("90 0C")
		int windowsLevel; // transparency added to all windows ("90 0C")
		// src/gfx/window/window_doc.c
		uint32_t docSerial; // a number per document ever created: tells a wait that its document was replaced
		// src/gfx/tonecurve.c
		struct ToneCurve* toneCurves;
	} gfx;

	// fonts (src/gfx/font.c, bmptext.c, typeface.c)
	struct
	{
		// src/gfx/font.c
		int aaShift;    // sum of the oversampled block >> this
		int aaScale;    // oversampling factor per axis
		int aaRowShift; // log2(aaScale)
		// src/gfx/typeface.c
		struct FtFonts* ft;            // the engine's FreeType library, NULL until the first font (and without FreeType)
		int ftTried;                   // it was made, or could not be
		struct Typeface* typefaces;    // the open typefaces
		struct MemFamily* memFamilies; // the families of the fonts handed over as data
		// src/gfx/bmptext.c
		struct FontName* fontNames;
		int fontNameCount; // the next number to hand out
		// the fonts built into the binary: per class (bgi/embfonts.h: gothic, Mincho and their proportional
		// forms) the family of the first one the system took, as UTF-8 and as Shift-JIS (face names are in
		// the scripts' encoding); "" when none
		char embeddedUtf8[4][FONTFILE_NAME_MAX];
		char embeddedSjis[4][FONTFILE_NAME_MAX];
		int embeddedReady;
		struct FontFile* fontFiles;
		struct FontFallback* fontFallbacks;
	} font;

	// the text engine (src/gfx/text/)
	struct
	{
		// src/gfx/text/text.c
		Gfx_t* textGfx;
		BmpMgr_t* textBmpMgr;
		Bmp_t glyphSheet;                     // a row of equal cells, one per private code
		int32_t glyphCount;                   // cells in the sheet (0: no sheet)
		int32_t glyphCellW;                   // width of one cell in pixels
		int32_t textItemCount;                // "waiting" cursor animation frames
		Bmp_t* textItems;                     // textItemCount screen bitmaps
		int32_t cursorInterval;               // ms between cursor frames ("90 99")
		int32_t charInterval;                 // ms between characters ("90 94")
		int32_t scrollSteps, scrollInterval;  // a line scroll: steps and ms per step ("90 95")
		int32_t fadeSteps, fadeInterval;      // a page-break fade: steps and ms per step ("90 96")
		int32_t textStyle[5];                 // {shadowOn, dx%, dy%, shadowColour, density 0..0x100} ("90 9C" / "90 9D")
		int32_t cursorPosMode;                // 1 = the cursor picture at the fixed position ("90 9A")
		int32_t cursorX, cursorY;             // that position, in text-layer pixels
		int32_t startDelayOn, startDelayMs;   // "90 9B": the end-of-text wait starts after `ms`
		int32_t autoAdvanceOn, autoAdvanceMs; // "90 97": the end-of-text wait ends by itself after `ms`
		int32_t textInputFinishes;            // "90 9F": input finishes the output instead of hurrying it
		int32_t textLayerMode;                // "90 91": 0 key 2, 1 fixed layer, 2 window priority
		int32_t textLayerNo;                  // the fixed layer of mode 1 (< 0x1000)
		RubyDict_t rubyDict;                  // the script's dictionary ("91 94" / "91 96")
		int32_t rubyCharDelay;                // reveal delay added per character of the rich output (steps)
		int32_t rubyFadeSteps;                // steps of a character's fade-in
		int32_t rubySizePct;                  // ruby size in percent of the font
		int32_t charPitch;                    // extra pixels per non-proportional character
		int32_t hangBrackets;                 // hang a leading bracket into the margin
		int32_t textIndent;                   // line-start indent when ruby is on (the margin "91 98" leaves for readings), pixels
		int32_t textFunc[TEXT_FUNC_MAX];      // [0] = function 0, [1 + n] = function 0x80000000 + n
		int32_t textLastPos[2];               // "92 9B" 0x100: the pen after the last layout into a bitmap
		uint32_t textLastColour;              // "92 9B" 0x101: the colour in effect at its end
		int32_t textScaleRecip;               // "91 99": 16.16 reciprocal of the divisor
		// src/gfx/text/ruby.c
		char rubyFace[0x100];                                // empty = the base font's face
		int32_t rubySizeFixed, rubyWidthPct, rubyDx, rubyDy; // size and width (0 = derived), offset in pixels
		int32_t rubyBold, rubyShadow;                        // "92 97" (1.616 on): -1 = the base font's bold / the style's shadow colour
		// src/gfx/text/markup.c
		TextLink_t textLinks[TEXT_LINKS_MAX];
		int32_t textLinkCount;
		char linkFace[0x34];                                  // empty = the text's face
		int32_t linkSize, linkWidthPct, linkBold, linkItalic; // 0 = the text's
		int32_t linkColour;                                   // -1 = the text's
		// src/gfx/text/textext.c
		int32_t textDirection;     // "EF 01": 0 the original left-to-right layout, 1 right-to-left paragraphs, 2 left-to-right paragraphs that may hold right-to-left text
		int32_t textWordWrap;      // "EF 03": lines break between words in the generations before 1.492 too
		int32_t textScroll;        // "EF 08": message text that does not fit its window scrolls (window.h: the text document)
		int32_t textScrollMs;      // the time a scroll of one line takes, ms
		TextArrow_t textArrows[2]; // [0] up, [1] down
		// src/gfx/text/glyphs.c
		Bmp_t charImages[0x100]; // the "92 98" pictures, by code & 0xff; pixels NULL = none
	} text;

	// movies (src/gfx/movie.c, src/sys/moviefx.c)
	struct
	{
		// src/gfx/movie.c
		int moviePlaying;    // a movie is running (the window paints black, no style toggle)
		int movieGraph;      // a graph exists
		int movieLoaded;     // a file is rendered in it
		int movieFx;         // the host's decoder runs the movie instead of its player
		int32_t movieVolume; // IBasicAudio units, -10000 .. 0; applied to every movie started
		// src/sys/moviefx.c
		struct MovieFx* fx; // the running movie, NULL for none
	} movie;

	// the BF_Movie sequences (src/gfx/bmseq.c)
	struct
	{
		struct BmSeq* list; // every loaded movie
		int32_t counter;    // the last number handed out (numbers start at 1)
		int deferFlag;      // "80 53": the next decode goes through a wait object
	} bmseq;

	// the particle patterns and screens (src/gfx/particle/)
	struct
	{
		// src/gfx/particle/engine.c
		struct PtclAuto* ptclAutoHead; // the registered screens, in order of registration
		// src/gfx/particle/patterns.c
		Bmp_t patternBmpB[PTCL_PATTERNS][PTCL_SCALES]; // type 1: the scaled copies per pattern
		PatternB_t patternB[PTCL_PATTERNS];
		Bmp_t patternBmpA[PTCL_PATTERNS][PTCL_SCALES]; // type 0
		PatternA_t patternA[PTCL_PATTERNS];
		int patternsReady;                           // Ptcl_InitGlobals ran
		PatternAnim_t patternAnim[2][PTCL_PATTERNS]; // [type][pattern]
		/* 1.636 on: the "alternate image" of a type 1 pattern ("C0 1A"), a second animation mixed into the
		 * picture by a level of 0 .. 0x100 ("C0 1B"; 0 the picture alone, 0x100 the alternate frame alone) */
		PatternAnim_t patternAlt[PTCL_PATTERNS];
		int32_t patternAltLevel[PTCL_PATTERNS]; // and their mix levels
		Bmp_t altMix, altTmp;                   // the picture mixed with the alternate image (one particle at a time), and the centred copy
		BmpMgr_t* ptclBmpMgr;                   // set at start-up by both setters; never read (the patterns use o->gfx.bmpMgr)
	} ptcl;

	// the rain effect (src/gfx/rain.c, rain_screen.c)
	struct
	{
		// src/gfx/rain.c
		uint32_t effectFrameMs; // milliseconds between effect frames (20 fps by default)
		uint32_t effectNext;    // when the next frame is due
		// src/gfx/rain_screen.c
		int effectsEnabled; // the switch of "C0 4F": 0 hides every rain screen
	} rain;

	// sound: the sound manager and the Wave Master library (src/snd/)
	struct
	{
		// src/snd/sound.c
		int soundOff;                           // muted by Sound_StopAll (minimise)
		uint32_t bgmMasterVolume[BGM_CHANNELS]; // 0 .. 0x80 per channel
		uint32_t seMasterVolume[SE_SLOTS];      // 0 .. 0x80 per slot
		uint8_t seDesc[SE_SLOTS][SE_DESC_SIZE]; // the loaded effects' descriptors
		uint32_t cdDevice;                      // MCI device id, 0 = closed
		int cdTrack;                            // the track last started (replayed on MM_MCINOTIFY)
		// src/snd/wm/wavemaster.c
		int sndDebug;     // BGI_SND_DEBUG: report music opens and the pump's restarts on stderr (SND_DEBUG)
		int audioRunning; // Wm_Init succeeded: the mixer runs
		WmRecord_t bgm[WM_BGM_CHANNELS];
		WmRecord_t se[WM_SE_SLOTS];
		Voice_t bgmVoices[WM_BGM_CHANNELS];
		Voice_t seVoices[WM_SE_SLOTS];
		uint32_t wmFlags;    // bit 0 initialised, bit 1 timer running
		uint32_t wmNextTick; // when the 20 ms timer is next due (ms)
		// src/snd/wm/voice.c
		WmExternalMix_t externalMix; // the external source (Wm_SetExternalMix)
		void* externalMixUser;
		int32_t mixAcc[4096 * 2]; // Wm_Mix's sums, 4096 stereo frames
		// src/snd/wm/wave.c
		Voice_t waveVoice; // "A0 C0": the wave of Wm_PlayWave
		// src/snd/sndout.c
		SndOut_t out; // the ring the host drains (OpenBGI_AudioRead)
		// src/snd/bw/vorbis.c
		int noDecoderSaid; // the build without a decoder said so on stderr (once)
	} snd;

#if !defined(BGI_NO_DEBUGGER)
	// the debugger (src/dbg/); not in a build without it (bgi/dbg.h)
	struct
	{
		// src/dbg/dbg.c
		DbgLogLine_t dbgLog[DBG_LOG_LINES]; // the log, a ring
		int dbgLogCount, dbgLogNext;        // lines in it; the slot the next line takes
		uint32_t dbgThreadId;               // the thread the views look at (its id; the pointer is looked up each frame)
		int dbgPaused;                      // the threads are held (Dbg_GateRun)
		int dbgTrace;                       // instruction trace into the log
		DbgBreak_t dbgBreaks[DBG_MAX_BREAKS];
		int dbgBreakCount;
		int open, overlay, wantOpen;        // the window is open / draws over the game / --debug asked for it
		int view;                           // the current tab
		int step;                           // 0 none, 1 a pass, 2 an instruction
		uint32_t lastDraw;                  // the host's clock (Clock_HostMs) at the last redraw
		int mouseX, mouseY;                 // the last mouse position seen (for the wheel)
		uint32_t passCount;                 // passes of the main loop (the status line)
		uint32_t skipThread, skipOff;       // the breakpoint to pass over once (the one just hit)
		DbgEvent_t events[DBG_EVENT_QUEUE]; // the queued input of the debugger's window
		int eventHead, eventCount;
		// src/dbg/dbgdraw.c
		DbgSurf_t dbgSurf;                               // the debugger's picture
		int dbgCellW;                                    // the text cell's width (DBG_CELL_W): 8 with a half-width font, else the font's advance
		struct Typeface* font;                           // the debugger's font, NULL when none could be opened
		uint8_t ascii[128][DBG_CELL_H * DBG_CELL_PITCH]; // one bit per pixel, MSB first
		int asciiReady;                                  // ascii is filled
		DbgWideGlyph_t wide[DBG_WIDE_CACHE];
		int wideNext; // the slot the next glyph takes
		// src/dbg/dbg_listing.c
		DbgListing_t listings[DBG_MAX_LISTINGS];
		int listingNext; // the slot the next listing takes
		// src/dbg/views/threads.c
		DbgList_t threadList;           // the thread list
		DbgListingPane_t threadListing; // the selected thread's listing
		struct Thread* threadRows[256]; // the threads behind the rows, rebuilt every draw
		int threadsFocus;               // 0 the thread list, 1 the listing
		// src/dbg/views/programs.c
		DbgList_t moduleList;              // the module list
		int programsFocus;                 // 0 the module list, 1 the listing
		DbgListingPane_t programListing;   // the selected module's listing
		struct ModuleNode* moduleRows[64]; // the modules behind the rows, rebuilt every draw
		// src/dbg/views/bitmaps.c
		DbgList_t bmpList; // the slot list
		int bmpRows[4096]; // the slot numbers behind the rows, rebuilt every draw
		// src/dbg/views/objects.c
		DbgList_t objList;          // the object list
		DispObj_t* objRows[1024];   // the objects behind the rows, rebuilt every draw
		const char* objClass[1024]; // the class each row was collected from
		// src/dbg/views/memory.c
		DbgList_t areaList; // the area list
		uint32_t memTop;    // the first byte shown (clamped to the area when drawing)
		// src/dbg/views/log.c
		DbgList_t logList; // the lines
		int logFollow;     // keep the newest line in view
		// src/dbg/views/targets.c
		DbgList_t targetList;             // the targets
		OpenBGI_Target_t targetRows[256]; // what a click reaches, rebuilt every draw (OpenBGI_Targets)
		int targetPicX, targetPicY;       // where the picture was drawn
		double targetPicScale;            // and its scale; 0 when it was not
	} dbg;
#endif
};

#endif // BGI_CORE_H_
