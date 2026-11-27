/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * engine.h - start-up and shutdown inside the engine (src/sys/engine.c;
 *            Vm_ResetSubsystems in src/vm/sched.c)
 *
 * The host starts and stops a run with OpenBGI_Start and OpenBGI_Stop
 * (openbgi.h): they parse the command line, run Engine_Init and boot the
 * script, and afterwards run Engine_Shutdown and launch the program "80
 * E1" asked for, if any.
 */
#ifndef BGI_ENGINE_H_
#define BGI_ENGINE_H_

#include "bgi/common.h"

/* The product name of the reference build (the literal that
 * "80 E8" copies out); every build has its own, see Engine_ProductName()
 * in version.h for the one of the detected game. */
#define PRODUCT_NAME_DEFAULT "Tayutama"

/* "<boot path> [option]": the path names the directory, the archive or the
 * program file to boot from ("." for the executable's directory), the
 * option "Execute as a launcher." (o->engine.launcherMode) or "Do not use
 * mutex." (o->engine.noMutex) */
void Boot_ParseCmdLine(OpenBGI_t* o, const char* cmdLine);
// the options of the engine's own command line ("--..."), as --help prints them
void Engine_PrintUsage(FILE* out);
/* Find and read openbgi.json (o->engine.gamesPath is the one in use
 * from then on): --games= / BGI_GAMES name it, and that file is used as
 * it is; otherwise the game directory's own comes first, then the one
 * beside the binary (a program that carries its settings with it), then
 * the one of the user's data folder - and when there is none, the
 * built-in defaults are written to the user's data folder, or beside the
 * binary when that folder is not there or cannot be written to (kept in
 * memory when neither can), and used.  Of the three that are looked for,
 * a file that cannot be written is passed over: settings that cannot be
 * kept are no use - unless the file says "staticConfig": true at its top,
 * which is for a read-only place: it is used then, and never written
 * (o->engine.gamesStatic; so is a named file that says it), and what
 * changes is gone with the run. */
void Engine_LoadGames(OpenBGI_t* o);
/* Write the games table and the launcher's settings back to it; when
 * that place cannot be written to and the file was not named on the
 * command line, to the other of the two places a file is made in - the
 * user's data folder, the place beside the binary -, which is the one in
 * use from then on.  Nothing is written for a file that says
 * "staticConfig": true.  1 when it was written. */
int Engine_SaveGames(OpenBGI_t* o);
/* The game chooser (src/sys/gamechooser.c): a request for the host's dialog
 * (OPENBGI_ASK_GAME_CHOOSER) over the games table (o->engine.chooser holds it
 * while it is up; its callbacks probe the directories the user picks).
 * GameChooser_End takes the answer: on Launch (1) the chosen directory is
 * the game directory (the host's setGameDir), the game is recorded in
 * `gamesPath`, and the profile ("" to detect it) and the extra options
 * chosen are copied out.  It returns 1 to launch, 0 to quit, -1 when no
 * dialog could be shown. */
uint32_t GameChooser_Begin(OpenBGI_t* o);
int GameChooser_End(OpenBGI_t* o, int answer, const char* gamesPath, char* profile, size_t profileSize, char* options, size_t optionsSize);
int Engine_Init(OpenBGI_t* o); // 0 = failure (an error box was shown)
void Engine_Shutdown(OpenBGI_t* o);
int Engine_Idle(OpenBGI_t* o); // no run is up: none was started, or the last one was stopped (OpenBGI_Stop)
// a run of a program the engine carries itself, on an engine never started (engine.c; the settings over a game, settings.h)
int Engine_StartBuiltin(OpenBGI_t* o, const uint8_t* program, uint32_t size);
/* What a stopped engine still holds besides its requests - the picture,
 * the fonts, what the scripts registered of them, the input layers, the
 * games table - goes (OpenBGI_Stop's last step; src/core/openbgi.c). */
void Engine_FreeRest(OpenBGI_t* o);
/* An engine that ran before is as OpenBGI_New made it, for the same
 * host: OpenBGI_Start's first step on one.  The host's tables as the
 * engine took them, the pads the host said are there and its touch scale
 * stay. */
void Engine_Renew(OpenBGI_t* o);

// how far OpenBGI_Start came, which is what OpenBGI_Stop tears down (o->engine.stage)
#define ENGINE_STAGE_NONE 0 // nothing to tear down (a tool option, or a check failed before the engine came up)
#define ENGINE_STAGE_UP   1 // the debugger and Engine_Init ran (Engine_Init may have failed)
#define ENGINE_STAGE_VM   2 // the VM started

// the subsystem reset executed before every boot (the definition of a script's fresh state)
void Vm_ResetSubsystems(OpenBGI_t* o);

#endif // BGI_ENGINE_H_
