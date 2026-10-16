/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * engine.h - engine-wide globals and the start-up / shutdown entry points
 *            (src/sys/engine.c; Vm_ResetSubsystems in src/vm/sched.c)
 *
 * Engine_Main is the whole program: it parses the command line, runs
 * Engine_Init, boots the script and loops until the machine stops, then
 * shuts down and launches the program "80 E1" asked for, if any.
 */
#ifndef BGI_ENGINE_H_
#define BGI_ENGINE_H_

#include "bgi/common.h"

/* The product name of the reference build (the literal that
 * "80 E8" copies out); every build has its own, see Engine_ProductName()
 * in version.h for the one of the detected game. */
#define PRODUCT_NAME_DEFAULT "Tayutama"

// command line: the second argument may be one of two fixed strings
extern int gLauncherMode; // "Execute as a launcher.": no sound device is opened
extern int gNoMutex;      // "Do not use mutex.": no single-instance mutex
extern int gBootDefault;  // no boot path was given: the game directory is where the engine runs
extern int gDiscMarker;   // --disc-marker: the disc marker file (a file named like the product) is reported to the scripts

// process state
extern int gEngineUp;     // between a successful Engine_Init and Engine_Shutdown
extern int gWindowAlive;  // set by WM_CREATE, cleared by WM_DESTROY: the window can be drawn to
extern int gWindowExists; // the window has been created and not yet destroyed (gHWnd != NULL in the original)

// program to launch after shutdown ("80 E1"): argument 0, the file, argument 2; NULL when none
extern char *gExitArg0, *gExitFile, *gExitArg2;

/* "<boot path> [option]": the path names the directory, the archive or the
 * program file to boot from ("." for the executable's directory), the
 * option one of the two strings above */
void Boot_ParseCmdLine(const char* cmdLine);
// the games.json in use (read at start-up; the launcher writes its records back to it)
const char* Engine_GamesJsonPath(void);
/* The game chooser (src/sys/launcher.c): the dialog of OS_LauncherDialog
 * over the games table.  On Launch the chosen directory is the game
 * directory (OS_SetGameDir), the game is recorded in `gamesPath`, and the
 * profile ("" to detect it) and the extra options chosen are copied out.
 * Returns 1 to launch, 0 to quit, -1 when no dialog could be shown. */
int Launcher_Run(const char* gamesPath, char* profile, size_t profileSize, char* options, size_t optionsSize);
int Engine_Init(void); // 0 = failure (an error box was shown)
void Engine_Shutdown(void);
int Engine_Main(const char* cmdLine); // WinMain without the OS glue; the process exit code

// the subsystem reset executed before every boot (the definition of a script's fresh state)
void Vm_ResetSubsystems(void);

#endif // BGI_ENGINE_H_
