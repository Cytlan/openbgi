/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dialogs.c - the installer's dialogs ("80 F0", "80 F1") and the
 *             installation itself ("80 F2"); inc/bgi/install.h
 *
 * The windows are the OS layer's (OS_InstallerDialog with one of the
 * Install*Dlg_t descriptions of inc/bgi/install.h); this file holds the
 * decisions behind their buttons and the sequence of an installation:
 * create the directories, copy the files (copy.c), write uninst.lst and
 * the uninstaller (uninstaller.c), register the installed folder - and
 * undo everything when a step fails.
 */
#include "install_internal.h"
#include "bgi/file.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/os.h"

/* The browse button (0x3E9) of the options dialog: let the user pick a
 * folder, check it (an invalid one shows an error box) and append the
 * product directory `d->subdir`; the result goes to `edit`.  1 when the
 * edit control is to be updated, 0 when the browser was cancelled or the
 * folder is unusable. */
int InstallOptions_Browse(InstallOptionsDlg_t* d, char* edit)
{
	char chosen[0x104];
	size_t len;
	if(!OS_BrowseFolder(chosen, sizeof chosen, MSG_INST_SELECT_FOLDER, NULL)) // no initial selection, as in the original
		return 0;
	if(!Path_IsValid(chosen))
	{
		MsgBox(MSG_INST_BAD_FOLDER, MSG_ERROR_CAPTION, OS_MB_ICONWARNING);
		return 0;
	}
	len = strlen(chosen);
	if(len && chosen[len - 1] == '\\')
		chosen[len - 1] = 0;
	sprintf(edit, "%s\\%s", chosen, d->subdir);
	return 1;
}

/* The OK button (0x3EC) of the options dialog: take the path from the
 * edit control into `d->path`, validate it (an invalid or over-long one
 * shows an error box), append the product folder to a bare drive, create
 * the directory to see that it can be made and remove it again.  1 when
 * the dialog may close with the path, 0 to keep it open. */
int InstallOptions_Accept(InstallOptionsDlg_t* d, const char* edit)
{
	char root[8];
	DirNode_t* tree;
	int ok;

	strncpy(d->path, edit, 0x104);
	d->path[0x104] = 0;
	if(!Path_IsValid(d->path) || strlen(d->path) >= 0xe4)
	{
		MsgBox(MSG_INST_BAD_PATH, MSG_ERROR_CAPTION, OS_MB_ICONWARNING);
		return 0;
	}
	sprintf(root, "%c:\\", d->path[0]);
	if(strcmp(root, d->path) == 0) // a bare drive: the product folder is appended
	{
		char tmp[0x104];
		sprintf(tmp, "%s%s", d->path, d->subdir);
		strcpy(d->path, tmp);
	}
	// a trial creation: the directories are made and taken away again
	tree = DirTree_New();
	ok = DirTree_Build(tree, d->path);
	DirTree_Remove(tree);
	DirTree_Free(tree);
	if(!ok)
	{
		MsgBox(MSG_INST_BAD_PATH, MSG_ERROR_CAPTION, OS_MB_ICONWARNING);
		return 0;
	}
	return 1;
}

/* "80 F0", the options dialog: the installation path (initialPath, with
 * `subdir` as the product folder and `okCaption` on the OK button) and two
 * check boxes with their initial states.  The chosen path goes to outPath,
 * the boxes to outCheck1 / 2 (script memory, possibly unaligned); the
 * dialog's result (1 OK, 0 cancelled) is returned and the outputs are
 * untouched on a cancel. */
int Install_DialogA(char* outPath, int32_t* outCheck1, int32_t* outCheck2, const char* initialPath, int check1,
	int check2, const char* subdir, const char* okCaption)
{
	InstallOptionsDlg_t d;
	int r;
	strcpy(gDlgPath, initialPath);
	strcpy(d.path, gDlgPath);
	d.check1 = check1;
	d.check2 = check2;
	d.subdir = subdir;
	d.okCaption = okCaption;
	r = OS_InstallerDialog(INSTALL_DLG_OPTIONS, &d);
	if(r)
	{
		strcpy(gDlgPath, d.path);
		strcpy(outPath, gDlgPath);
		memcpy(outCheck1, &d.check1, 4); // script memory: may be unaligned
		memcpy(outCheck2, &d.check2, 4);
	}
	return r;
}

/* "80 F1", the choice dialog: a text and two or three radio buttons
 * (radio0 NULL selects the two-button form), button `disabled` (0..2)
 * greyed out, the extra button shown when `showExtra` is set.  Returns -1
 * cancelled, 0 .. 2 the chosen button, 3 the extra button or no choice. */
int Install_DialogB(const char* text, const char* radio0, const char* radio1, const char* radio2, int disabled,
	int showExtra)
{
	InstallChoiceDlg_t d;
	d.text = text;
	d.radio0 = radio0;
	d.radio1 = radio1;
	d.radio2 = radio2;
	d.disabled = disabled;
	d.showExtra = showExtra;
	return OS_InstallerDialog(radio0 ? INSTALL_DLG_CHOICE3 : INSTALL_DLG_CHOICE, &d);
}

/* "80 F2", the installation itself; the arguments are described in
 * inc/bgi/install.h.  In order: the installation directory and its
 * sub-directories are created, BGI.hvl is loaded from the engine's
 * directory (no verification when it is missing), the progress dialog runs
 * the copy (Install_CopyAll), uninst.lst and the uninstaller are written
 * and the installed folder is registered.  Returns 1 when everything
 * succeeded; otherwise every file and directory created is removed again
 * and 0 is returned. */
int Install_RunDialog(const char* installDir, char** subdirs, char** files, int discCount, const int* discCounts,
	char** discMarkers, char** discMsgs, int saveCount, const char* saveFmt, const char* company,
	const char* product, const char* uninstExe, const char* insertMsg, int cancelAllowed)
{
	char srcRel[0x104], hvlPath[0x104];
	DirNode_t* tree;
	int result = 0, n;
	size_t len;
	InstallProgressDlg_t dlg;

	(void)discCount; // the per-disc counts are walked by file index instead (DiscOfFile in copy.c)
	strcpy(gDlgPath, installDir);
	gDiscCounts = discCounts;
	gDiscMarkers = discMarkers;
	gDiscMsgs = discMsgs;

	tree = DirTree_New();
	if(!DirTree_Build(tree, installDir))
	{
		DirTree_Remove(tree);
		DirTree_Free(tree);
		return 0;
	}
	if(DirTree_BuildList(tree, installDir, subdirs))
	{
		gMadeList.head = NULL;

		// the engine's directory relative to its drive root ("D:\game\" -> "game"): the disc mirrors it
		len = strlen(gBaseDir);
		if((int)len - 4 > 0)
		{
			memcpy(srcRel, gBaseDir + 3, len - 4);
			srcRel[len - 4] = 0;
		}
		else
		{
			srcRel[0] = 0;
		}
		for(n = 0; files[n]; n++)
			;
		gInst.installDir = installDir;
		gInst.srcRel = srcRel;
		gInst.count = n;
		gInst.files = files;
		gHvlCount = 0;
		gHvl = NULL;
		sprintf(hvlPath, "%s%s", gBaseDir, "BGI.hvl"); // gBaseDir ends in '\'
		Hvl_Load(&gHvlCount, &gHvl, hvlPath);          // a missing or bad list leaves the count at 0: no verification

		// the progress dialog (the OS layer fills in the callbacks and the ui pointer); its result is the copy's
		memset(&dlg, 0, sizeof dlg);
		dlg.fileCount = n;
		dlg.cancelAllowed = cancelAllowed;
		if(OS_InstallerDialog(INSTALL_DLG_PROGRESS, &dlg))
		{
			if(Uninst_WriteList(&gMadeList, uninstExe, saveCount, saveFmt, subdirs, &gInst) &&
				Uninst_Install(&gInst, product, uninstExe, insertMsg))
			{
				result = RegSetInstalledFolder(company, product, installDir);
			}
		}
		if(gHvl)
		{
			BGI_Free(gHvl);
			gHvl = NULL;
		}
		if(!result) // the rollback: every file made, then every directory made
		{
			MadeList_DeleteFiles(&gMadeList);
			DirTree_Remove(tree);
		}
		MadeList_Free(&gMadeList);
	}
	else
	{
		// a sub-directory could not be made: the directories made so far go (the file
		// list holds nothing of this installation yet; its users leave it empty)
		MadeList_DeleteFiles(&gMadeList);
		DirTree_Remove(tree);
	}
	DirTree_Free(tree);
	return result;
}
