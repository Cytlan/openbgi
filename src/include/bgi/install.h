/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * install.h - the installer / launcher services behind "80 E0 .. 80 FE":
 * running external programs, the registration check, shortcuts, the
 * registry, file associations and the resource dialogs of the installer
 *
 * All of this is Windows-specific by nature.  The copy engine, the
 * bookkeeping (uninst.lst, the hash list BGI.hvl, created directories and
 * files) and the decisions of the dialog procedures are reimplemented here
 * exactly; the windows themselves - message boxes, the folder browser, the
 * three resource dialogs - are the host's: the requests
 * OPENBGI_ASK_INSTALLER (openbgi.h) carry one of the Install*Dlg
 * structures below.  The POSIX back end answers them on the console.
 *
 * The implementation is split over src/sys/install/ (machine.c, folders.c,
 * shortcuts.c, registry.c, uninstlist.c, hvl.c, copy.c, uninstaller.c,
 * dialogs.c; their shared internals are in install_internal.h there) and,
 * for the programs and the audio check, src/sys/process.c.  Paths are
 * Windows paths with '\' separators in 0x104-byte buffers throughout.
 */
#ifndef BGI_INSTALL_H_
#define BGI_INSTALL_H_

#include "bgi/common.h"
#include "bgi/request.h" // AskRetry_t

// ---- external programs (src/sys/process.c) -------------------------------
/* "80 E0", "80 E1" and "80 E2" ask for "<dir>\<file>" ("<base dir><file>"
 * when dir is NULL) to be started as a new process: the registration
 * tool, the uninstaller, another game.  OpenBGI starts no program for a
 * game: Program_Refuse says so on stderr and returns what the script is
 * told, 0 - the program was not started.  ("80 E1" still ends the run, as
 * the script asked.) */
int Program_Refuse(OpenBGI_t* o, const char* dir, const char* file);
/* "80 E3", ShellExecute "open": a web link (http, https) and a document
 * (ShellOpen_IsDocument: text, a page, a PDF, a picture, by the name's
 * ending) are opened with the program the desktop has for them - the
 * host's (OpenBGI_Shell_t.openDocument) -, a document only a game's
 * archive holds from an extracted copy; the host's result.  Anything
 * else - a program, a shortcut, a script, a link of another kind - is
 * refused: said on stderr, 0. */
int ShellOpen(OpenBGI_t* o, const char* path);
int ShellOpen_IsLink(const char* path);
int ShellOpen_IsDocument(OpenBGI_t* o, const char* path);

// ---- registration / machine identity (machine.c) ----------------------------
int CheckRegistration(OpenBGI_t* o);                 // "80 EC": 1 when comap.dat holds the machine key, or with --no-disc-check
int ReadComapDat(OpenBGI_t* o, void* buf);           // "80 ED": the first 0x404 bytes of "comap.dat" into buf; 1 when the file opened
uint32_t GetMachineKey(OpenBGI_t* o);                // "80 EE": CPU signature, Windows version, first letters of the user and computer names
uint32_t GetMachineKeyXor(OpenBGI_t* o, uint32_t x); // "80 EF": the machine key XOR x

// ---- folders (folders.c) ------------------------------------------------------
/* "80 3A" (and "80 FB" for 0): which 0 = Windows directory, 1 = desktop
 * directory (CSIDL 0x10), 2 = start menu "Programs" (CSIDL 2), 3 = "My
 * Documents" (CSIDL 5), into buf (0x104 bytes).  Returns 1, or 0 for an
 * unknown selector / a shell failure. */
int GetSpecialFolder(OpenBGI_t* o, char* buf, int which);
/* "80 FA": read "<Windows dir>\<name>" into buf; the file must hold a
 * path ending in "\" and a NUL - the backslash is removed.  1 = ok. */
int ReadWinDirFile(OpenBGI_t* o, char* buf, const char* name);

// ---- registry (registry.c; HKLM\Software\<company>\<product>) -----------------
int RegGetInstalledFolder(OpenBGI_t* o, char* buf, const char* company, const char* product);       // "80 F8": the InstalledFolder value into buf (0x104 bytes); 1 when present
int RegSetInstalledFolder(OpenBGI_t* o, const char* company, const char* product, const char* dir); // write dir as the InstalledFolder value; always 1
int RegDeleteSoftwareKey(OpenBGI_t* o, const char* company, const char* product);                   // "80 F9": delete the key; 1 when deleted
/* "80 FC": HKCR\.<ext> = progid, HKCR\<progid> = desc, \DefaultIcon = icon,
 * \Shell\Open\Command = command; then the shell is told.  1 when every key
 * was written. */
int RegisterFileAssoc(OpenBGI_t* o, const char* ext, const char* progid, const char* desc,
	const char* icon, const char* command);

// ---- shortcuts (shortcuts.c; IShellLink on Windows) -----------------------------
/* "80 F7": "<Programs>\<folder>\<name>" -> target, or "<Desktop>\<name>"
 * when folder is NULL; the folder is created (and removed again when the
 * link cannot be made).  1 = ok. */
int CreateShortcut(OpenBGI_t* o, const char* folder, const char* name, const char* target);
int CreateShortcutArgs(OpenBGI_t* o, const char* folder, const char* name, const char* target, const char* args); // "81 F7" of 1.492 on: with a command line for the target
/* "80 F3": the links of an installation: "<dir>\<exe>" as <name> on the
 * desktop (when desktop != 0) and, when startMenu != 0, "<Programs>\<folder>"
 * with <name> -> "<dir>\<exe>" and <name2> -> "<dir>\<exe2>".  Everything is
 * undone when one of the start-menu links fails.  Returns 1 when the start
 * menu part succeeded (or was not asked for). */
int CreateShortcuts(OpenBGI_t* o, const char* dir, const char* exe, const char* name,
	const char* exe2, const char* name2, const char* folder,
	int startMenu, int desktop);
/* "80 F6": deletes "<Desktop>\<name>", "<Programs>\<folder>\<name>" and
 * "<Programs>\<folder>\<name2>", then the folder when removeFolder != 0 */
void DeleteShortcuts(OpenBGI_t* o, const char* name, const char* name2, const char* folder, int removeFolder);

// ---- uninst.lst (uninstlist.c) -------------------------------------------------
/* "80 F4": delete every file named in "<dir>\uninst.lst" except lines
 * starting with '@', the list itself and the names in keep (NULL-terminated,
 * or NULL).  Returns 1 when the list could be opened. */
int UninstListProcess(OpenBGI_t* o, const char* dir, char** keep);
/* "80 F5": append the names (NULL-terminated) that are not yet in the list.
 * Returns 1 when the list was rewritten. */
int UninstListAppend(OpenBGI_t* o, const char* dir, char** names);

// ---- the three resource dialogs (dialogs.c) --------------------------------------
// the dialog template ids of OPENBGI_ASK_INSTALLER
#define INSTALL_DLG_OPTIONS  0x6C // folder + two check boxes (InstallOptionsDlg_t)
#define INSTALL_DLG_CHOICE   0x6D // text + two radio buttons (InstallChoiceDlg_t)
#define INSTALL_DLG_CHOICE3  0x6E // text + three radio buttons (InstallChoiceDlg_t)
#define INSTALL_DLG_PROGRESS 0x6F // the copy progress (InstallProgressDlg_t)

/* The dialogs are requests to the host (OPENBGI_ASK_INSTALLER, openbgi.h)
 * whose `ctx` is the Install*Dlg_t of the template.  They are not part of
 * the interface a host of the library is built with: only a host built
 * with these sources shows them (the Win32 one does); every other
 * answers 0, which is a cancel for each of them.  Two of them run some
 * of the engine's logic while they are up (the options dialog's buttons,
 * the progress dialog's copy); the message boxes and the pauses of that
 * logic are the dialog's own, through the callbacks the host fills in
 * before it calls it (the engine gives the boxes its usual treatment
 * around them).  Without a box callback a box is answered as by a user
 * who gives up (Yes to "abort?", Cancel otherwise); without a pause the
 * logic does not pause. */
typedef int (*InstallBoxFn_t)(void* ui, const char* text, const char* caption, uint32_t flags); // a message box over the dialog: the button, OPENBGI_ID*

/* 0x6C: control 0x3E8 edit = path, 0x3E9 browse, 0x3EA / 0x3EB check boxes,
 * 0x3EC OK (caption okCaption), 0x3ED cancel.  The dialog's result is 1 for
 * OK, 0 for cancel. */
typedef struct InstallOptionsDlg
{
	char path[0x108];      /* in = initial path, out = chosen; the
	                        * original terminates at [0x104] (its global has room) */
	int check1;            // in = initial state, out = final state of check box 0x3EA
	int check2;            // the same for 0x3EB
	const char* subdir;    // product folder appended to a root
	const char* okCaption; // caption of the OK button, or NULL for the template's
	OpenBGI_t* o;          // the engine the buttons' logic below runs in
	// filled by the host for that logic:
	void* ui;
	InstallBoxFn_t msgBox;
	int (*browseFolder)(void* ui, char* path, size_t n, const char* title, const char* initial); // SHBrowseForFolder over the dialog: 1 chosen
} InstallOptionsDlg_t;
// the logic behind the dialog's buttons, for the host's dialog:
int InstallOptions_Browse(OpenBGI_t* o, InstallOptionsDlg_t* d, char* edit);       // 0x3E9: 1 = edit updated with the chosen folder
int InstallOptions_Accept(OpenBGI_t* o, InstallOptionsDlg_t* d, const char* edit); // 0x3EC: 1 = close with 1 (d->path holds the path)

/* 0x6D / 0x6E: 0x3EE text, radio buttons 0x3EF (0x6E only) / 0x3F0 / 0x3F1,
 * 0x3F2 extra button (shown when showExtra), 0x3F3 OK, 0x3F4 cancel.
 * Result: -1 cancelled, 0..2 the chosen radio button, 3 extra button or
 * no button chosen.  The request is answered with the result and one, so
 * that 0 is the cancel here too. */
typedef struct InstallChoiceDlg
{
	const char* text;   // the text (0x3EE)
	const char* radio0; // caption of 0x3EF; NULL selects the two-button template
	const char* radio1; // caption of 0x3F0
	const char* radio2; // caption of 0x3F1
	int disabled;       /* radio button to disable (0..2, else none)
	                     * radio1 is checked initially when this is 2, else radio2 */
	int showExtra;      // show the extra button 0x3F2
} InstallChoiceDlg_t;

/* 0x6F: 0x3F5 "installing <file>", 0x3F6 per-file bar, 0x3F7 total bar,
 * 0x3F8 cancel button (hidden unless cancelAllowed).  The host creates
 * the window, fills the callbacks and `ui`, and calls Install_CopyAll(),
 * whose result (1 every file copied, 0 failed or cancelled) it returns as
 * the dialog's. */
typedef struct InstallProgressDlg
{
	int fileCount;                                             // range of the total bar
	int cancelAllowed;                                         // show the cancel button
	void* ui;                                                  // the host's window, passed to the callbacks
	void (*setFile)(void* ui, int index, const char* caption); // new file: 0x3F5 text, 0x3F6 reset
	void (*setRange)(void* ui, int blocks);                    // 0x3F6 range (the file's 64 KB blocks)
	void (*setPos)(void* ui, int block);                       // 0x3F6 position
	void (*setTotal)(void* ui, int done);                      // 0x3F7 position (files done)
	int (*alive)(void* ui);                                    // pumps the window; 0 once the user confirmed the abort
	InstallBoxFn_t msgBox;                                     // a box over the window (the disc prompts, the errors)
	void (*pause)(void* ui, uint32_t ms);                      // the copy's pause between two looks for a disc (50 ms)
	OpenBGI_t* o;                                              // the engine Install_CopyAll runs in
} InstallProgressDlg_t;
int Install_CopyAll(OpenBGI_t* o, InstallProgressDlg_t* d); // copy.c: the copy of every file, driving the dialog; 1 done, 0 failed / cancelled

/* The VM-level entry points, each a request and what follows its answer.
 * "80 F0", the options dialog: Install_AskOptions fills `d` (which must
 * last as long as the request) and makes the request; once answered,
 * Install_OptionsDone writes the chosen path to outPath and the boxes to
 * outCheck1 / 2 on OK (1) and leaves them on a cancel (0). */
uint32_t Install_AskOptions(OpenBGI_t* o, InstallOptionsDlg_t* d, const char* initialPath, int check1, int check2, const char* subdir,
	const char* okCaption);
void Install_OptionsDone(OpenBGI_t* o, InstallOptionsDlg_t* d, int answer, char* outPath, int32_t* outCheck1, int32_t* outCheck2);
/* "80 F1", the choice dialog (`d` lasts as long as the request); its
 * answer is -1 cancelled, 0..2 the button, 3 the extra button or none. */
uint32_t Install_AskChoice(OpenBGI_t* o, InstallChoiceDlg_t* d, const char* text, const char* radio0, const char* radio1,
	const char* radio2, int disabled, int showExtra);
/*
 * "80 F2": the installation itself, a step per frame (InstallRun_t).
 *   installDir   where to install (created)
 *   subdirs      sub-directories to create below it (NULL-terminated, or NULL)
 *   files        the files to copy (NULL-terminated), each "<disc dir>\<file>"
 *                relative to the directory the engine runs from
 *   discCounts   number of files on each disc (files are listed disc by disc)
 *   discMarkers  a file that identifies disc i ("<disc dir>\<marker>")
 *   discMsgs     the "insert disc i" message
 *   saveCount    save slots listed in uninst.lst, named saveFmt (with %d) or
 *                SaveFileName() when saveFmt is NULL
 *   company, product   HKLM\Software\<company>\<product>\InstalledFolder
 *   uninstExe    uninstaller copied from the disc and registered under
 *                Software\Microsoft\Windows\CurrentVersion\Uninstall\<product>
 *   insertMsg    message shown when the disc with the uninstaller is missing
 *   cancelAllowed  the progress dialog shows its cancel button
 * Install_Begin keeps the arguments (they must last until the end),
 * Install_Step returns 0 while the installation goes on and 1 once it is
 * over: `result` is 1 when everything succeeded; otherwise everything
 * created was removed again and it is 0.  Install_Abandon ends one that
 * is not over (as a failure) when its instruction goes away.
 */
typedef struct InstallRun
{
	int state;
	const char* installDir;
	char** subdirs;
	char** files;
	int saveCount;
	const char *saveFmt, *company, *product, *uninstExe, *insertMsg;
	struct DirNode* tree;     // the directories made
	char srcRel[0x104];       // the engine's directory below its drive root (the disc mirrors it)
	InstallProgressDlg_t dlg; // the progress dialog's request
	uint32_t req;
	AskRetry_t ask; // the prompt for the uninstaller's disc
	int pausing;    // the 200 ms after it
	uint32_t until; // their end (host clock)
	int giveUp;     // the user gave the uninstaller's disc up
	int result;
	uint32_t idleMs; // the wait the step asks for
} InstallRun_t;
void Install_Begin(OpenBGI_t* o, InstallRun_t* r, const char* installDir, char** subdirs, char** files, const int* discCounts,
	char** discMarkers, char** discMsgs, int saveCount, const char* saveFmt, const char* company, const char* product,
	const char* uninstExe, const char* insertMsg, int cancelAllowed);
int Install_Step(OpenBGI_t* o, InstallRun_t* r);
void Install_Abandon(OpenBGI_t* o, InstallRun_t* r);

// ---- misc -----------------------------------------------------------------
int DSound8_IsAvailable(OpenBGI_t* o);            // "80 FE" and the start-up: whether audio output is available (CoCreateInstance(CLSID_DirectSound8) in the original)
int Path_IsValid(OpenBGI_t* o, const char* path); // folders.c: "x:\..." without "\\" or a second ':'

// the BGI.hvl file hash of a whole file ("80 E9" of 1.69 build 472 on) into the 8 bytes at out; 0 = cannot open
int HvHash_File(OpenBGI_t* o, uint8_t* out, const char* path);
void HvHash_UpdateBuf(uint8_t* state, const void* data, uint32_t n); // "81 E9": continue the hash in the 8-byte state over a buffer

// ---- the types of the installer's state (o->install, bgi/core.h) -----------------------

// the files and shortcuts an installation created, newest first, for the rollback and uninst.lst
typedef struct MadeFile
{
	char* name; // the file / link name as the script gave it
	char* path; // the full path that was created
	struct MadeFile* next;
} MadeFile_t;

typedef struct MadeList
{
	MadeFile_t* head; // the newest entry; NULL for an empty list
} MadeList_t;

// what the dialog, the copy engine and the uninstaller bookkeeping share
typedef struct InstallCtx
{
	const char* installDir; // where the files go
	const char* srcRel;     // the engine's directory without its drive and final '\' (the disc mirrors it)
	int count;              // files to copy
	char** files;           // their names, "<disc dir>\<file>", disc by disc
} InstallCtx_t;

#endif // BGI_INSTALL_H_
