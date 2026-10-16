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
 * three resource dialogs - are provided by the OS layer through
 * OS_InstallerDialog(), which receives one of the Install*Dlg structures
 * below.  The POSIX back end answers them on the console.
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

// ---- external programs (src/sys/process.c) -------------------------------
/*
 * Start "<dir>\<file>" (or "<base dir><file>" when dir is NULL) as a new
 * process.  While the file's drive is not ready or the process cannot be
 * created, `msg` is shown ("Insert the disc"): with allowRetry the box is
 * OK/Cancel and Cancel asks whether to abort, without it the box is
 * informational and the call fails.  With `wait` the engine waits for the
 * process (hiding the main window first when hideWindow is set) and, when
 * waitUninstaller is set, also until the uninstaller's mutex is gone.
 * Returns 1 when the process was started.  "80 E0" calls it with wait and
 * allowRetry, "80 E2" with hideWindow, wait and waitUninstaller, "80 E1"
 * after the shutdown with none of them.
 */
int RunProgram(const char* dir, const char* file, const char* msg,
	int hideWindow, int wait, int allowRetry, int waitUninstaller);
int ShellOpen(const char* path); // "80 E3": open a file or URL with its associated program (ShellExecute "open"); the OS layer's result

// ---- registration / machine identity (machine.c) ----------------------------
int CheckRegistration(void);           // "80 EC": 1 when comap.dat holds the machine key (reg.exe is run to write it first)
int ReadComapDat(void* buf);           // "80 ED": the first 0x404 bytes of "comap.dat" into buf; 1 when the file opened
uint32_t GetMachineKey(void);          // "80 EE": CPU signature, Windows version, first letters of the user and computer names
uint32_t GetMachineKeyXor(uint32_t x); // "80 EF": the machine key XOR x

// ---- folders (folders.c) ------------------------------------------------------
/* "80 3A" (and "80 FB" for 0): which 0 = Windows directory, 1 = desktop
 * directory (CSIDL 0x10), 2 = start menu "Programs" (CSIDL 2), 3 = "My
 * Documents" (CSIDL 5), into buf (0x104 bytes).  Returns 1, or 0 for an
 * unknown selector / a shell failure. */
int GetSpecialFolder(char* buf, int which);
/* "80 FA": read "<Windows dir>\<name>" into buf; the file must hold a
 * path ending in "\" and a NUL - the backslash is removed.  1 = ok. */
int ReadWinDirFile(char* buf, const char* name);

// ---- registry (registry.c; HKLM\Software\<company>\<product>) -----------------
int RegGetInstalledFolder(char* buf, const char* company, const char* product);       // "80 F8": the InstalledFolder value into buf (0x104 bytes); 1 when present
int RegSetInstalledFolder(const char* company, const char* product, const char* dir); // write dir as the InstalledFolder value; always 1
int RegDeleteSoftwareKey(const char* company, const char* product);                   // "80 F9": delete the key; 1 when deleted
/* "80 FC": HKCR\.<ext> = progid, HKCR\<progid> = desc, \DefaultIcon = icon,
 * \Shell\Open\Command = command; then the shell is told.  1 when every key
 * was written. */
int RegisterFileAssoc(const char* ext, const char* progid, const char* desc,
	const char* icon, const char* command);

// ---- shortcuts (shortcuts.c; IShellLink on Windows) -----------------------------
/* "80 F7": "<Programs>\<folder>\<name>" -> target, or "<Desktop>\<name>"
 * when folder is NULL; the folder is created (and removed again when the
 * link cannot be made).  1 = ok. */
int CreateShortcut(const char* folder, const char* name, const char* target);
int CreateShortcutArgs(const char* folder, const char* name, const char* target, const char* args); // "81 F7" of 1.494 on: with a command line for the target
/* "80 F3": the links of an installation: "<dir>\<exe>" as <name> on the
 * desktop (when desktop != 0) and, when startMenu != 0, "<Programs>\<folder>"
 * with <name> -> "<dir>\<exe>" and <name2> -> "<dir>\<exe2>".  Everything is
 * undone when one of the start-menu links fails.  Returns 1 when the start
 * menu part succeeded (or was not asked for). */
int CreateShortcuts(const char* dir, const char* exe, const char* name,
	const char* exe2, const char* name2, const char* folder,
	int startMenu, int desktop);
/* "80 F6": deletes "<Desktop>\<name>", "<Programs>\<folder>\<name>" and
 * "<Programs>\<folder>\<name2>", then the folder when removeFolder != 0 */
void DeleteShortcuts(const char* name, const char* name2, const char* folder, int removeFolder);

// ---- uninst.lst (uninstlist.c) -------------------------------------------------
/* "80 F4": delete every file named in "<dir>\uninst.lst" except lines
 * starting with '@', the list itself and the names in keep (NULL-terminated,
 * or NULL).  Returns 1 when the list could be opened. */
int UninstListProcess(const char* dir, char** keep);
/* "80 F5": append the names (NULL-terminated) that are not yet in the list.
 * Returns 1 when the list was rewritten. */
int UninstListAppend(const char* dir, char** names);

// ---- the three resource dialogs (dialogs.c) --------------------------------------
// the dialog template ids OS_InstallerDialog receives
#define INSTALL_DLG_OPTIONS  0x6C // folder + two check boxes (InstallOptionsDlg_t)
#define INSTALL_DLG_CHOICE   0x6D // text + two radio buttons (InstallChoiceDlg_t)
#define INSTALL_DLG_CHOICE3  0x6E // text + three radio buttons (InstallChoiceDlg_t)
#define INSTALL_DLG_PROGRESS 0x6F // the copy progress (InstallProgressDlg_t)

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
} InstallOptionsDlg_t;
// the logic behind the dialog's buttons, for the OS layer:
int InstallOptions_Browse(InstallOptionsDlg_t* d, char* edit);       // 0x3E9: 1 = edit updated with the chosen folder
int InstallOptions_Accept(InstallOptionsDlg_t* d, const char* edit); // 0x3EC: 1 = close with 1 (d->path holds the path)

/* 0x6D / 0x6E: 0x3EE text, radio buttons 0x3EF (0x6E only) / 0x3F0 / 0x3F1,
 * 0x3F2 extra button (shown when showExtra), 0x3F3 OK, 0x3F4 cancel.
 * Result: -1 cancelled, 0..2 the chosen radio button, 3 extra button or
 * no button chosen. */
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
 * 0x3F8 cancel button (hidden unless cancelAllowed).  The OS layer creates
 * the window, fills the callbacks and `ui`, and calls Install_CopyAll(),
 * whose result (1 every file copied, 0 failed or cancelled) it returns as
 * the dialog's. */
typedef struct InstallProgressDlg
{
	int fileCount;                                             // range of the total bar
	int cancelAllowed;                                         // show the cancel button
	void* ui;                                                  // the OS layer's window, passed to the callbacks
	void (*setFile)(void* ui, int index, const char* caption); // new file: 0x3F5 text, 0x3F6 reset
	void (*setRange)(void* ui, int blocks);                    // 0x3F6 range (the file's 64 KB blocks)
	void (*setPos)(void* ui, int block);                       // 0x3F6 position
	void (*setTotal)(void* ui, int done);                      // 0x3F7 position (files done)
	int (*alive)(void* ui);                                    // pumps the window; 0 once the user confirmed the abort
} InstallProgressDlg_t;
int Install_CopyAll(InstallProgressDlg_t* d); // copy.c: the copy of every file, driving the dialog; 1 done, 0 failed / cancelled

// the VM-level entry points
int Install_DialogA(char* outPath, int32_t* outCheck1, int32_t* outCheck2, const char* initialPath,
	int check1, int check2, const char* subdir, const char* okCaption); // "80 F0": the options dialog; 1 OK (outputs written), 0 cancel
int Install_DialogB(const char* text, const char* radio0, const char* radio1, const char* radio2,
	int disabled, int showExtra); // "80 F1": the choice dialog; -1 cancelled, 0..2 the button, 3 the extra button or none
/*
 * "80 F2": the installation itself.
 *   installDir   where to install (created)
 *   subdirs      sub-directories to create below it (NULL-terminated, or NULL)
 *   files        the files to copy (NULL-terminated), each "<disc dir>\<file>"
 *                relative to the directory the engine runs from
 *   discCount    number of discs (not used: the per-disc counts are walked
 *                by file index)
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
 * Returns 1 when everything succeeded; otherwise everything created is
 * removed again and 0 is returned.
 */
int Install_RunDialog(const char* installDir, char** subdirs, char** files, int discCount,
	const int* discCounts, char** discMarkers, char** discMsgs,
	int saveCount, const char* saveFmt, const char* company,
	const char* product, const char* uninstExe, const char* insertMsg,
	int cancelAllowed);

// ---- misc -----------------------------------------------------------------
int DSound8_IsAvailable(void);      // "80 FE" and the start-up: whether audio output is available (CoCreateInstance(CLSID_DirectSound8) in the original)
int Path_IsValid(const char* path); // folders.c: "x:\..." without "\\" or a second ':'

// the BGI.hvl file hash of a whole file ("80 E9" of 1.69 build 472 on) into the 8 bytes at out; 0 = cannot open
int HvHash_File(uint8_t* out, const char* path);
void HvHash_UpdateBuf(uint8_t* state, const void* data, uint32_t n); // "81 E9": continue the hash in the 8-byte state over a buffer

#endif // BGI_INSTALL_H_
