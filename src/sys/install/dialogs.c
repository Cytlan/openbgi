/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dialogs.c - the installer's dialogs ("80 F0", "80 F1") and the
 *             installation itself ("80 F2"); src/include/bgi/install.h
 *
 * The windows are the host's (requests with one of the Install*Dlg_t
 * descriptions of src/include/bgi/install.h); this file holds the decisions
 * behind their buttons and the sequence of an installation:
 * create the directories, copy the files (copy.c), write uninst.lst and
 * the uninstaller (uninstaller.c), register the installed folder - and
 * undo everything when a step fails.
 */
#include "install_internal.h"
#include "bgi/file.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/sys.h"
#include "bgi/request.h"
#include "bgi/core.h"

/* A box the dialog shows over itself for the logic it runs (install.h),
 * with the treatment the engine gives its boxes around it. */
int Inst_Box(OpenBGI_t* o, InstallBoxFn_t fn, void* ui, const char* text, const char* caption, uint32_t flags)
{
	int prev, r;
	if(!fn) // a dialog without boxes: the answer of a user who gives up
		return (flags & 0xf) == OPENBGI_MB_YESNO ? OPENBGI_IDYES : OPENBGI_IDCANCEL;
	prev = MsgBox_Begin(o, text, caption);
	r = fn(ui, text, caption, flags);
	MsgBox_End(o, prev);
	return r;
}

/* The browse button (0x3E9) of the options dialog: let the user pick a
 * folder, check it (an invalid one shows an error box) and append the
 * product directory `d->subdir`; the result goes to `edit`.  1 when the
 * edit control is to be updated, 0 when the browser was cancelled or the
 * folder is unusable. */
int InstallOptions_Browse(OpenBGI_t* o, InstallOptionsDlg_t* d, char* edit)
{
	char chosen[0x104];
	size_t len;
	if(!d->browseFolder || !d->browseFolder(d->ui, chosen, sizeof chosen, MSG_INST_SELECT_FOLDER, NULL)) // no initial selection, as in the original
		return 0;
	if(!Path_IsValid(o, chosen))
	{
		Inst_Box(o, d->msgBox, d->ui, MSG_INST_BAD_FOLDER, MSG_ERROR_CAPTION, OPENBGI_MB_ICONWARNING);
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
int InstallOptions_Accept(OpenBGI_t* o, InstallOptionsDlg_t* d, const char* edit)
{
	char root[8];
	DirNode_t* tree;
	int ok;

	strncpy(d->path, edit, 0x104);
	d->path[0x104] = 0;
	if(!Path_IsValid(o, d->path) || strlen(d->path) >= 0xe4)
	{
		Inst_Box(o, d->msgBox, d->ui, MSG_INST_BAD_PATH, MSG_ERROR_CAPTION, OPENBGI_MB_ICONWARNING);
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
	ok = DirTree_Build(o, tree, d->path);
	DirTree_Remove(o, tree);
	DirTree_Free(tree);
	if(!ok)
	{
		Inst_Box(o, d->msgBox, d->ui, MSG_INST_BAD_PATH, MSG_ERROR_CAPTION, OPENBGI_MB_ICONWARNING);
		return 0;
	}
	return 1;
}

static uint32_t Install_Ask(OpenBGI_t* o, int dialog, void* ctx)
{
	OpenBGI_Request_t q;
	memset(&q, 0, sizeof q);
	q.kind = OPENBGI_ASK_INSTALLER;
	q.u.installer.dialog = dialog;
	q.u.installer.ctx = ctx;
	return Request_Post(o, &q, REQUEST_TREAT_NONE);
}

/* "80 F0", the options dialog: the installation path (initialPath, with
 * `subdir` as the product folder and `okCaption` on the OK button) and two
 * check boxes with their initial states.  On OK the chosen path goes to
 * outPath, the boxes to outCheck1 / 2 (script memory, possibly
 * unaligned); on a cancel the outputs are untouched. */
uint32_t Install_AskOptions(OpenBGI_t* o, InstallOptionsDlg_t* d, const char* initialPath, int check1, int check2, const char* subdir,
	const char* okCaption)
{
	memset(d, 0, sizeof *d);
	strcpy(o->install.dlgPath, initialPath);
	strcpy(d->path, o->install.dlgPath);
	d->check1 = check1;
	d->check2 = check2;
	d->subdir = subdir;
	d->okCaption = okCaption;
	d->o = o;
	return Install_Ask(o, INSTALL_DLG_OPTIONS, d);
}

void Install_OptionsDone(OpenBGI_t* o, InstallOptionsDlg_t* d, int answer, char* outPath, int32_t* outCheck1, int32_t* outCheck2)
{
	if(!answer)
		return;
	strcpy(o->install.dlgPath, d->path);
	strcpy(outPath, o->install.dlgPath);
	memcpy(outCheck1, &d->check1, 4); // script memory: may be unaligned
	memcpy(outCheck2, &d->check2, 4);
}

/* "80 F1", the choice dialog: a text and two or three radio buttons
 * (radio0 NULL selects the two-button form), button `disabled` (0..2)
 * greyed out, the extra button shown when `showExtra` is set.  The
 * request's answer is 0 cancelled (or declined by a host without the
 * dialog), 1 .. 3 the chosen button, 4 the extra button or no choice: one
 * more than the instruction's result. */
uint32_t Install_AskChoice(OpenBGI_t* o, InstallChoiceDlg_t* d, const char* text, const char* radio0, const char* radio1,
	const char* radio2, int disabled, int showExtra)
{
	memset(d, 0, sizeof *d);
	d->text = text;
	d->radio0 = radio0;
	d->radio1 = radio1;
	d->radio2 = radio2;
	d->disabled = disabled;
	d->showExtra = showExtra;
	return Install_Ask(o, radio0 ? INSTALL_DLG_CHOICE3 : INSTALL_DLG_CHOICE, d);
}

/* "80 F2", the installation itself; the arguments are described in
 * src/include/bgi/install.h.  In order: the installation directory and its
 * sub-directories are created, BGI.hvl is loaded from the engine's
 * directory (no verification when it is missing), the progress dialog runs
 * the copy (Install_CopyAll), uninst.lst and the uninstaller are written
 * and the installed folder is registered.  The result is 1 when
 * everything succeeded; otherwise every file and directory created is
 * removed again and it is 0.  The original does it all inside the
 * instruction; here the dialog is a request, the disc prompt for the
 * uninstaller boxes, and the steps in between run once they are answered. */
#define INST_DIRS      0 // make the directories, ask for the progress dialog
#define INST_COPIED    1 // the dialog's answer: the copy is over
#define INST_UNINST    2 // look for the uninstaller on a disc
#define INST_UNINST_OK 3 // it is installed: register the folder
#define INST_END       4 // the rollback when it failed, the cleanup
#define INST_OVER      5

void Install_Begin(OpenBGI_t* o, InstallRun_t* r, const char* installDir, char** subdirs, char** files, const int* discCounts,
	char** discMarkers, char** discMsgs, int saveCount, const char* saveFmt, const char* company, const char* product,
	const char* uninstExe, const char* insertMsg, int cancelAllowed)
{
	memset(r, 0, sizeof *r);
	r->installDir = installDir;
	r->subdirs = subdirs;
	r->files = files;
	r->saveCount = saveCount;
	r->saveFmt = saveFmt;
	r->company = company;
	r->product = product;
	r->uninstExe = uninstExe;
	r->insertMsg = insertMsg;
	r->dlg.cancelAllowed = cancelAllowed;
	strcpy(o->install.dlgPath, installDir);
	o->install.discCounts = discCounts;
	o->install.discMarkers = discMarkers;
	o->install.discMsgs = discMsgs;
	r->state = INST_DIRS;
}

// the directories, BGI.hvl and the progress dialog's request; 0 when a directory could not be made (the run is over)
static int Install_Start(OpenBGI_t* o, InstallRun_t* r)
{
	size_t len;
	int n;
	char hvlPath[0x104];
	r->tree = DirTree_New();
	if(!DirTree_Build(o, r->tree, r->installDir))
		return 0;
	if(!DirTree_BuildList(o, r->tree, r->installDir, r->subdirs))
		return 0; // a sub-directory could not be made: the directories made so far go (the file list holds nothing yet)
	o->install.madeList.head = NULL;

	// the engine's directory relative to its drive root ("D:\game\" -> "game"): the disc mirrors it
	len = strlen(o->files.baseDir);
	if((int)len - 4 > 0)
	{
		memcpy(r->srcRel, o->files.baseDir + 3, len - 4);
		r->srcRel[len - 4] = 0;
	}
	else
	{
		r->srcRel[0] = 0;
	}
	for(n = 0; r->files[n]; n++)
		;
	o->install.inst.installDir = r->installDir;
	o->install.inst.srcRel = r->srcRel;
	o->install.inst.count = n;
	o->install.inst.files = r->files;
	o->install.hvlCount = 0;
	o->install.hvl = NULL;
	sprintf(hvlPath, "%s%s", o->files.baseDir, "BGI.hvl");       // o->files.baseDir ends in '\'
	Hvl_Load(o, &o->install.hvlCount, &o->install.hvl, hvlPath); // a missing or bad list leaves the count at 0: no verification

	// the progress dialog (the host fills in the callbacks and the ui pointer); its answer is the copy's result
	r->dlg.fileCount = n;
	r->dlg.o = o;
	r->req = Install_Ask(o, INSTALL_DLG_PROGRESS, &r->dlg);
	return 1;
}

// the rollback of a failed run (every file made, then every directory made) and the cleanup
static void Install_Finish(OpenBGI_t* o, InstallRun_t* r)
{
	if(o->install.hvl)
	{
		BGI_Free(o->install.hvl);
		o->install.hvl = NULL;
	}
	if(r->tree)
	{
		if(!r->result)
		{
			MadeList_DeleteFiles(o, &o->install.madeList);
			DirTree_Remove(o, r->tree);
		}
		DirTree_Free(r->tree);
		r->tree = NULL;
	}
	MadeList_Free(&o->install.madeList);
	r->state = INST_OVER;
}

int Install_Step(OpenBGI_t* o, InstallRun_t* r)
{
	int answer;
	r->idleMs = 0;
	while(!Request_Pending(o)) // a box a step showed is answered before the next step, as the original's boxes are
	{
		switch(r->state)
		{
			case INST_DIRS:
				r->state = Install_Start(o, r) ? INST_COPIED : INST_END;
				break;
			case INST_COPIED:
				if(!Request_Take(o, r->req, &answer))
					return 0;
				r->req = 0;
				r->state = INST_END;
				if(answer && Uninst_WriteList(o, &o->install.madeList, r->uninstExe, r->saveCount, r->saveFmt, r->subdirs, &o->install.inst))
					r->state = INST_UNINST;
				break;
			case INST_UNINST:
			{
				int32_t left = (int32_t)(r->until - Clock_HostMs(o));
				int a;
				if(r->pausing && left > 0)
				{
					r->idleMs = (uint32_t)left; // the 200 ms after the prompt
					return 0;
				}
				r->pausing = 0;
				if(r->giveUp)
				{
					r->state = INST_END;
					break;
				}
				if(r->ask.state == 0)
				{ // no box is up: look for the disc
					int u = Uninst_TryInstall(o, &o->install.inst, r->product, r->uninstExe);
					if(u >= 0)
					{
						r->state = u ? INST_UNINST_OK : INST_END; // found: done, whether the copy succeeded or not
						break;
					}
				}
				// not found: ask for the disc; Cancel asks whether to give up the installation
				a = AskRetry_Step(o, &r->ask, r->insertMsg, MSG_NOTICE, MSG_CONFIRM, 0);
				if(a == ASK_WAIT)
					return 0;
				r->giveUp = a == ASK_ABORT;
				r->pausing = 1;
				r->until = Clock_HostMs(o) + 200;
				break;
			}
			case INST_UNINST_OK:
				r->result = RegSetInstalledFolder(o, r->company, r->product, r->installDir);
				r->state = INST_END;
				break;
			case INST_END: Install_Finish(o, r); return 1;
			default: return 1;
		}
	}
	return 0;
}

void Install_Abandon(OpenBGI_t* o, InstallRun_t* r)
{
	if(r->state == INST_OVER)
		return;
	Request_Forget(o, r->req);
	Request_Forget(o, r->ask.req);
	r->result = 0;
	Install_Finish(o, r);
}
