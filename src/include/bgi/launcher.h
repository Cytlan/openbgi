/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * launcher.h - the launcher's side of the engine (src/sys/launcher.c);
 *              not part of the original engine
 *
 * The original lives in its game's folder.  OpenBGI lives anywhere, and
 * started without a game it runs the launcher: a Buriko program of
 * its own that lists the player's games and starts one (launcher/ in the
 * source tree, docs/dev/launcher.md).  The program is the host's
 * (OpenBGI_SetLauncher, openbgi.h): the desktop programs carry its
 * sources, a host that is handed its game by something else has none and
 * the engine asks it for a game as before (OPENBGI_ASK_GAME_CHOOSER,
 * src/sys/gamechooser.c).  The sources are written into the user's data
 * folder, where anyone may change them, and compiled there by the engine,
 * which keeps the compiled program until a source changes
 * (src/sys/launcher_src.c).
 *
 * What the program cannot do with the original's instructions it asks of
 * the engine with OpenBGI's own ("EF 2x", docs/modding/obgi.md), which this
 * module carries out: reading a folder of the host, looking at a folder
 * or an archive for a game, starting one, keeping settings.  The paths
 * and names here are UTF-8; the instructions convert from and to the
 * script's text.
 *
 * Starting a game ends the launcher's run and starts the engine again
 * with the game's command line (Launcher_Relaunch), in the same window's
 * place; the game's end starts it once more with the launcher's own
 * (Launcher_Return).  A few things live across that restart and are kept apart from
 * the rest of the engine's state for it (LauncherState_t): which game
 * runs and for how long, so that the launcher's settings can say when it
 * was played and for how long, and a small picture of it, taken now and
 * then, for the launcher to show next time.
 */
#ifndef BGI_LAUNCHER_H_
#define BGI_LAUNCHER_H_

#include "bgi/common.h"
#include "openbgi.h"

#define LAUNCHER_PROGRAM   "launcher._bp"   // the compiled program's name: the boot program of the launcher's run
#define LAUNCHER_PROFILE   "1.669"          // the engine profile it is compiled for
#define LAUNCHER_SOURCES   "launcher"       // the folder of its sources, in the user's data folder
#define LAUNCHER_WRITTEN   "written.txt"    // there: what the engine wrote into the folder
#define LAUNCHER_CACHE     "compiled.cache" // there: the program compiled from the folder's sources, and what it was made from
#define LAUNCHER_CACHE_OWN "builtin.cache"  // .. and the one compiled from the pack's own, for when the folder's do not compile
#define LAUNCHER_BUILD_ID  "build.id"       // a file of a pack that is no source: its bytes say which build of the engine carries the pack
#define LAUNCHER_FILES     32               // files of a pack of sources
#define LAUNCHER_DIRS      8                // folder listings open at once
#define LAUNCHER_DROPS     64               // the dropped paths that wait for the program at most
#define LAUNCHER_PATH      0x400            // bytes of a path
#define LAUNCHER_SHOT_W    640              // the picture kept of a game: at most this size, its aspect kept
#define LAUNCHER_SHOT_H    360
#define LAUNCHER_SHOT_MS   10000      // a picture is taken this often
#define LAUNCHER_SHOT_1ST  4000       // and first after this long
#define LAUNCHER_PICTURES  "pictures" // the folder of the pictures, in the user's data folder
#define LAUNCHER_SAVE_MS   1000       // changed settings are written this long after the last change
#define LAUNCHER_ICONS     "icons"    // the folder of the games' icons, in the user's data folder
#define LAUNCHER_PREVIEWS  256        // games that can wait for a first picture at a time

struct LauncherDir;
typedef struct LauncherSources LauncherSources_t; // a pack's files in memory, while the launcher is booted (launcher_src.c)

/* What stays when the engine starts again (Engine_Renew keeps it; the
 * listings and the command line of a start go with the run). */
typedef struct LauncherState
{
	const uint8_t* pack; // the host's launcher: its sources, packed; NULL for none
	uint32_t packSize;
	const uint8_t* ownIcon; // the host's own icon, an .ico file (OpenBGI_SetIcon); NULL for none
	uint32_t ownIconSize;
	LauncherSources_t* src; // .. unpacked, from Launcher_Prepare to Launcher_Boot
	uint32_t askReq;        // the box that asks whether changed sources are kept or replaced
	uint8_t* compiled;      // the program compiled for this run (BGI_Free)
	uint8_t* kept;          // a copy of the program as the launcher's last run had it, for the runs after it: the settings over a game (Launcher_Program; BGI_Free)
	uint32_t keptSize;
	int cached;                   // .. which was not compiled at this start: it was kept from an earlier one (Launcher_Boot)
	int running;                  // this run is the launcher's
	int launched;                 // this run is a game the launcher started
	char gamePath[LAUNCHER_PATH]; // that game: its path as the launcher knows it (UTF-8)
	char gamesPath[0x408];        // the openbgi.json the launcher kept its settings in
	uint32_t startMs;             // the host's clock when the game's run began
	uint32_t shotMs;              // .. and when the next picture of it is due
	uint8_t* shot;                // the last picture kept of the running game: shotW x shotH pixels, B G R x; NULL for none
	int shotW, shotH;
	char* relaunch;                          // the command line "EF 29" left: the run is replaced at the frame's end
	char* back;                              // the launcher's own command line while a game it started runs: the run after the game's (BGI_Free)
	char backDir[0x408];                     // .. and the game directory that start began in
	int hostQuit;                            // the host asked for the end (OpenBGI_Quit): no run follows this one
	int cfgChanged;                          // "EF 2D" changed the settings since they were written,
	uint32_t cfgSaveMs;                      // .. and the host's clock at which they are written
	struct LauncherDir* dirs[LAUNCHER_DIRS]; // the listings "EF 20" opened
	char* dropped[LAUNCHER_DROPS];           // the paths dropped on the window that the program has not taken yet (UTF-8)
	int droppedCount;
	char gameIcon[40];                 // the launched game's icon: a file's name in LAUNCHER_ICONS, "" for none
	char* previews[LAUNCHER_PREVIEWS]; // the games that wait for a first picture, the next first (UTF-8 paths)
	int previewCount;
	char* previewOut; // the one a host took (OpenBGI_PreviewNext) and has not handed back
	int previewWant;  // .. and what it was taken for: OPENBGI_PREVIEW_*
	uint8_t* runIcon; // the icon of this run's game as OpenBGI_GameIcon found it (BGI_Free), kept for the run
	size_t runIconSize;
	int runIconKnown;         // .. it was looked for
	struct OpenBGI_Job* jobs; // the listings and the looks at games a host does beside the frames, the next first; done ones wait to be asked for
	int jobsOut;              // .. of which the host has so many (OpenBGI_JobNext)
	uint32_t cfgSerial;       // counts the times the settings were written
	char jobGames[0x408];     // an engine that does such work for another: the settings file its tables are of,
	uint32_t jobSerial;       // .. as written this many times,
	int jobTables;            // .. once they were read
	uint64_t* listed;         // the games the program named in this run, by a sum of their paths: each is looked at once
	int listedCount, listedCap;
	uint32_t previewSerial; // counts the first pictures that came in, for the program
} LauncherState_t;

// what Launcher_Probe finds: struct GameInfo of the program (launcher/obgi_launcher.inc)
typedef struct LauncherGame
{
	char name[128];   // the game's name, "" when nothing says it
	char profile[16]; // the profile the detection chose
	char product[64]; // the product name its programs check for, "" when they do not
	int kind;         // LAUNCHER_GAME_*
} LauncherGame_t;

#define LAUNCHER_GAME_FOLDER  1
#define LAUNCHER_GAME_ZIP     2
#define LAUNCHER_GAME_ISO     3

#define LAUNCHER_ENTRY_FILE   1
#define LAUNCHER_ENTRY_FOLDER 2
// Launcher_DirOpen's flags ("EF 20")
#define LAUNCHER_DIR_HIDDEN   1 // hidden entries too
#define LAUNCHER_DIR_GAMES    2 // a search for games: what could be one among the entries is looked at ahead (Launcher_JobFor)
#define LAUNCHER_DIR_DEEPER   4 // .. and the folders among them are listed ahead, each as with LAUNCHER_DIR_GAMES

/* Whether a start without a game boots the launcher: the host handed
 * over its sources and the command line did not ask for the host's
 * dialog.
 *
 * Launcher_Prepare then makes the folder of the sources ready
 * (LAUNCHER_SOURCES in the user's data folder; launcher_src.c says how it is
 * kept up to date).  It returns 1 when it had to ask whether changed
 * sources are kept or replaced by newer ones: the box is
 * o->launcher.askReq, and its answer goes to Launcher_Answer (1 for
 * "replace") before the boot.
 *
 * Launcher_Boot compiles the program - the folder's sources, or the
 * pack's own when those do not compile (which a box says) or there is no
 * folder; a program kept from an earlier compile of the same files by the
 * same engine is taken in the compile's place (o->launcher.cached) - and
 * makes the run the launcher's: its game directory the
 * user's data folder, the boot program, the engine profile, no sound
 * device, no single-instance lock.  1 when the launcher runs, 0 when
 * there is nothing to run or no folder to run in.
 *
 * Launcher_SourcesFree lets go of what the three made. */
int Launcher_Wanted(OpenBGI_t* o);
int Launcher_Prepare(OpenBGI_t* o);
void Launcher_Answer(OpenBGI_t* o, int replace);
int Launcher_Boot(OpenBGI_t* o);
void Launcher_SourcesFree(OpenBGI_t* o);
/* The launcher's compiled program for a run beside this one (the settings
 * over a game, settings.h): the one the launcher's last run of this
 * engine had - compiled from the folder's sources, or kept from an
 * earlier compile - else the pack's own sources compiled now.  NULL when
 * the host handed over no launcher.  The engine's, until it is deleted. */
const uint8_t* Launcher_Program(OpenBGI_t* o, uint32_t* size);

/* The listing of a folder (a UTF-8 native path; "" the computer: the
 * drives where paths have them, the host's roots where it names some -
 * each with its whole path for a name, in the host's order - else the
 * root).  Folders first, then
 * files, each sorted by name without regard to the case of ASCII letters;
 * hidden entries - names that begin with '.' or '$' - only with flag 1,
 * after the others of their kind.  The handle is 1 .. LAUNCHER_DIRS, 0
 * when the folder cannot be read or too many are open. */
int Launcher_DirOpen(OpenBGI_t* o, const char* path, int flags);
int Launcher_TopKnown(OpenBGI_t* o); // the listing of "" is drives or roots: nothing is read for it
int Launcher_DirCount(OpenBGI_t* o, int h);
int Launcher_DirEntry(OpenBGI_t* o, int h, int i, char* dst, size_t size); // LAUNCHER_ENTRY_*, 0 and "" for none
void Launcher_DirClose(OpenBGI_t* o, int h);                               // 0 and unknown handles are ignored
void Launcher_DirCloseAll(OpenBGI_t* o);

/* What a program asks about the host through the settings ("EF 2C" with
 * an empty path): "home", the folder the user's documents are in ("" when
 * the host names none) - where a folder browser starts; "separator", what
 * stands between the names of a path; and "language", the language of the
 * engine's own messages ("en" or "ja"), which the program's texts follow.
 * 1 when `key` is one of them. */
int Launcher_HostValue(OpenBGI_t* o, const char* key, char* dst, size_t size);
/* .. and the lists "roots" and "root_names": the folders the host names
 * for a folder browser to start from, where those above them cannot be
 * listed (OpenBGI_Files_t.nativeRoots), and what each is called for the
 * player.  A root has nothing above it but the listing of "".  0 when
 * `key` is neither, 1 with value `i` in dst, -1 when there is no such
 * value. */
#define LAUNCHER_ROOTS_TEXT 0x1000 // bytes of the host's list at most
int Launcher_RootValue(OpenBGI_t* o, const char* key, int i, char* dst, size_t size);

/* What was dropped on the launcher's window: the setting "dropped" of the
 * path "", which the program reads as a list and empties by deleting
 * what it took.  Launcher_Drop adds a path as the host spells it (no
 * more than LAUNCHER_DROPS wait; nothing outside a launcher's run);
 * Launcher_Dropped is entry `i` in UTF-8, its length, -1 for none;
 * Launcher_DropTake removes entry `i`, 1 when there was one. */
void Launcher_Drop(OpenBGI_t* o, const char* native);
int Launcher_Dropped(OpenBGI_t* o, int i, char* dst, size_t size);
int Launcher_DropTake(OpenBGI_t* o, int i);

/* Whether a game is at `path` - a folder with a boot program (a loose
 * ipl._bp, or system.arc with one), or a zip archive or disc image that
 * holds such a folder - and which: 1 and *out filled, 0 and *out zeroed. */
int Launcher_Probe(OpenBGI_t* o, const char* path, LauncherGame_t* out);

/* Work beside the frames.  A folder's listing and a look at a game read
 * from a drive, which may take seconds (a disk that has to spin up, a
 * share); a host that says OPENBGI_CAP_JOBS does that work for the
 * launcher on a thread of its own (OpenBGI_JobNext, openbgi.h), and the
 * thread of the program that asked waits meanwhile (a wait object, as
 * for a load), while the program's other threads go on.
 *
 * Launcher_JobsOn: this run has such a host.  Launcher_JobFor gives the
 * job that answers a listing (LAUNCHER_JOB_LIST: `flags` as
 * Launcher_DirOpen's) or a look at a game (LAUNCHER_JOB_PROBE): one that
 * was made ahead for the same path - done already, perhaps - or a new
 * one at the end of the queue.  A listing with LAUNCHER_DIR_GAMES, when
 * it comes back, puts the looks at its games into the queue - its
 * archives and disc images, and the folder itself when it has
 * system.arc or ipl._bp - and with LAUNCHER_DIR_DEEPER the listings of
 * its folders, so that the host has the next pieces before the program
 * asks for them one by one; a listing asked for with both flags first
 * drops what was made ahead and never asked for.  Launcher_JobReady: 1
 * when the job is done.  Launcher_JobTake ends a job that is done: a
 * listing's handle (0 when the folder cannot be read or no handle is
 * free) or, for a look, 1 with *out filled when a game is there.
 * Launcher_JobDrop gives up a job that is not wanted any more, done or
 * not. */
#define LAUNCHER_JOB_LIST  1
#define LAUNCHER_JOB_PROBE 2
int Launcher_JobsOn(OpenBGI_t* o);
struct OpenBGI_Job* Launcher_JobFor(OpenBGI_t* o, int kind, const char* path, int flags);
int Launcher_JobReady(const struct OpenBGI_Job* job);
int Launcher_JobTake(OpenBGI_t* o, struct OpenBGI_Job* job, LauncherGame_t* out);
void Launcher_JobDrop(OpenBGI_t* o, struct OpenBGI_Job* job);
struct Wait;
// a job's waiting thread: the wait is told when the job goes with the run (Launcher_JobGone, the wait's)
void Launcher_JobWait(struct OpenBGI_Job* job, struct Wait* w);

/* Start the game at `path` in place of this run: with the profile
 * (--engine=; "" for the detection), the patches (--patch= each, a block
 * of strings ended by an empty one) and more options of the command line.
 * Its settings record when (Engine_CfgSet "played").  1 when the frame's
 * end will replace the run (Launcher_Relaunch), 0 when there is no game
 * there or its path cannot be put on a command line. */
int Launcher_Launch(OpenBGI_t* o, const char* path, const char* profile, const char* patches, const char* options);
int Launcher_Relaunch(OpenBGI_t* o); // the engine's frame: stop this run and start the one Launcher_Launch prepared; what OpenBGI_Start returned
/* The way back.  A game the launcher started does not end the engine's
 * run of frames: when it is over - by its own doing, by the window's
 * close box as the game handles it, or with an error whose box was
 * answered - the launcher runs again, started as it was started first.
 * Launcher_Returns says whether that is due for the run that just ended;
 * Launcher_Return, the engine's frame, stops that run and starts the
 * launcher's (what OpenBGI_Start returned).  Not when the host asked for
 * the end (OpenBGI_Quit: a signal, the session's end): then the run of
 * frames ends as ever.  A game named on the command line was not started
 * by the launcher and ends them too. */
int Launcher_Returns(OpenBGI_t* o);
int Launcher_Return(OpenBGI_t* o);

/* A frame of a game the launcher started: now and then a small copy of
 * its picture is kept (one that shows something).  Launcher_GameEnded,
 * as the run stops, adds its time to the game's settings ("seconds"),
 * writes the picture as a PNG file into the folder LAUNCHER_PICTURES of
 * the user's data folder and puts the file's name into the game's
 * settings ("picture").  The launcher's run has that data folder as its
 * game directory, so its program loads the picture as "pictures\<name>"
 * ("90 C4").
 *
 * In a frame of the launcher's own run Launcher_Frame writes the settings
 * a moment after "EF 2D" changed them (Launcher_SettingsChanged);
 * Launcher_SaveSettings does so at once, as does the run's end. */
void Launcher_Frame(OpenBGI_t* o, const OpenBGI_Frame_t* f);
void Launcher_GameEnded(OpenBGI_t* o);
void Launcher_SettingsChanged(OpenBGI_t* o);
void Launcher_SaveSettings(OpenBGI_t* o);

// what goes with the engine for good (OpenBGI_Delete), and with a run (OpenBGI_Stop)
void Launcher_Free(OpenBGI_t* o);
void Launcher_RunEnded(OpenBGI_t* o);

/* The program lists the game at `path` ("EF 2D" with the key "preview"):
 * once in a run, it joins the games a host is handed (OpenBGI_PreviewNext)
 * when its settings name no icon, or no picture while first pictures are
 * made (Launcher_PreviewsOff says when not) and none was tried in vain.
 * 1 when it was looked at now. */
int Launcher_Listed(OpenBGI_t* o, const char* path);
// the number of first pictures that came in during this run, as text ("EF 2C" with the key "previews")
int Launcher_PreviewCount(OpenBGI_t* o, char* dst, size_t size);
/* The icon of the game at the native path `native` (a folder, a zip
 * archive or a disc image): that of its main program - not the setup,
 * installer and uninstaller beside it -, as the bytes of an .ico file
 * (BGI_Free them), `*size` of them; NULL when there is none. */
uint8_t* Launcher_GameIcon(OpenBGI_t* o, const char* native, size_t* size);
// 1 when no first pictures are made: --preview=none, or "preview": "none" among the launcher's settings
int Launcher_PreviewsOff(OpenBGI_t* o);
/* The icon the window of this run takes, as .ico bytes (BGI_Free them):
 * the launched game's as the launcher kept it, or that of the game being
 * run when nothing was kept; NULL for the host's own, or OpenBGI's. */
uint8_t* Launcher_RunIcon(OpenBGI_t* o, size_t* size);
// the host's own icon as it handed it over (OpenBGI_SetIcon): .ico bytes that stay where they are; NULL for none
const uint8_t* Launcher_OwnIcon(OpenBGI_t* o, size_t* size);

#endif // BGI_LAUNCHER_H_
