/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * copy.c - the copy engine of the installation (install_internal.h; the
 *          progress dialog's entry point Install_CopyAll is in
 *          src/include/bgi/install.h)
 *
 * One job per file: find the disc that holds it (the candidate drives are
 * polled for the disc's marker file, then the user is asked for the
 * disc), skip it when the installed copy is not older, else copy it
 * through "inst.tmp" in 64 KB blocks, verify it against BGI.hvl when it
 * is listed there (up to four attempts) and move it into place.
 *
 * The original copies every file on a thread of its own and drives the
 * progress dialog with posted messages; here the copy runs on the calling
 * thread inside Install_CopyAll and the dialog is updated through the
 * callbacks of InstallProgressDlg_t, in the same sequence of file
 * operations, prompts and results.  Its message boxes and its pauses are
 * the dialog's too (InstallProgressDlg_t.msgBox, .pause).
 */
#include "install_internal.h"
#include "bgi/file.h"
#include "bgi/strutil.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/host.h"
#include "bgi/core.h"

// the state of the running installation, set up by Install_Step (install_internal.h)

// the disc a file index belongs to, from the per-disc counts
static int DiscOfFile(OpenBGI_t* o, int index)
{
	int disc = 0;
	int rem = index - o->install.discCounts[0];
	while(rem >= 0)
	{
		disc++;
		rem -= o->install.discCounts[disc];
	}
	return disc;
}

/* The drives a disc may be in: "x:" of every CD-ROM drive and of the drive
 * the engine runs from (o->files.baseDir), 0x104 bytes per entry in `out`
 * (room for 26); the count. */
int CandidateDrives(OpenBGI_t* o, char* out)
{
	char drives[0x400], base[0x104], root[0x104];
	const char* p;
	int n = 0;
	Host_LogicalDrives(o, drives, sizeof drives);
	strcpy(base, o->files.baseDir);
	SjisStrLwr(o, base);
	for(p = drives; *p; p += strlen(p) + 1)
	{
		int take = Host_DriveType(o, p) == OPENBGI_DRIVE_CDROM;
		if(!take)
		{
			strcpy(root, p);
			SjisStrLwr(o, root);
			take = root[0] == base[0] && root[1] == base[1];
		}
		if(take)
		{
			sprintf(out + n * 0x104, "%c%c", p[0], p[1]);
			n++;
		}
	}
	return n;
}

typedef struct CopyJob // one file to copy
{
	char* name;   // the file as the script listed it ("<disc dir>\<file>")
	char* dst;    // "<install dir>\<name>"
	char* src;    // "<src rel>\<name>" (below the drive root, the drive to be found)
	char* marker; // "<src rel>\<disc marker>": the file that identifies the right disc
	char* msg;    // the "insert disc" message of that disc
	int index;    // the file's index in the list
} CopyJob_t;

static void CopyJob_Free(CopyJob_t* j)
{
	BGI_Free(j->name);
	BGI_Free(j->dst);
	BGI_Free(j->src);
	BGI_Free(j->marker);
	BGI_Free(j->msg);
	BGI_Free(j);
}

// the job for file `index` of the context (its paths built, its disc looked up), NULL past the end of the list
static CopyJob_t* CopyJob_New(OpenBGI_t* o, const InstallCtx_t* ctx, int index)
{
	CopyJob_t* j;
	char buf[0x104];
	int disc;
	if(index >= ctx->count)
		return NULL;
	j = (CopyJob_t*)BGI_Alloc(sizeof(CopyJob_t));
	disc = DiscOfFile(o, index);
	j->name = BGI_Strdup(ctx->files[index]);
	sprintf(buf, "%s\\%s", ctx->installDir, ctx->files[index]);
	j->dst = BGI_Strdup(buf);
	sprintf(buf, "%s\\%s", ctx->srcRel, ctx->files[index]);
	j->src = BGI_Strdup(buf);
	sprintf(buf, "%s\\%s", ctx->srcRel, o->install.discMarkers[disc]);
	j->marker = BGI_Strdup(buf);
	j->msg = BGI_Strdup(o->install.discMsgs[disc]);
	j->index = index;
	return j;
}

// pump the dialog through its `alive` callback and note when the user cancelled; 1 while the dialog is open
static int Dlg_Alive(OpenBGI_t* o, InstallProgressDlg_t* d)
{
	if(d->alive && !d->alive(d->ui))
		o->install.dlgAlive = 0;
	return o->install.dlgAlive;
}

/* the "insert the disc" prompt (OK / Cancel; Cancel asks whether to abort
 * the installation); 1 when the user wants another try, 0 to give up (also
 * when the dialog is gone) */
static int CopyJob_PromptDisc(OpenBGI_t* o, const CopyJob_t* j, InstallProgressDlg_t* d)
{
	int retry = 0;
	o->install.promptOpen = 1;
	if(Dlg_Alive(o, d))
	{
		retry = 1;
		if(Inst_Box(o, d->msgBox, d->ui, j->msg, MSG_NOTICE, OPENBGI_MB_OKCANCEL | OPENBGI_MB_ICONINFO) != OPENBGI_IDOK)
		{
			if(Inst_Box(o, d->msgBox, d->ui, MSG_ASK_ABORT, MSG_CONFIRM, OPENBGI_MB_YESNO | OPENBGI_MB_ICONQUESTION | OPENBGI_MB_DEFBUTTON2) == OPENBGI_IDYES)
				retry = 0;
		}
	}
	o->install.promptOpen = 0;
	return retry;
}

/* Copy one file, reporting through the dialog's callbacks.  1 when the file
 * is in place (copied, or already present and not older than the source),
 * 0 on failure (the source was not found and the user gave up, the
 * temporary file could not be created, a read or write error, four
 * attempts with a wrong hash) or when the user cancelled the dialog. */
static int CopyJob_Run(OpenBGI_t* o, const CopyJob_t* j, InstallProgressDlg_t* d)
{
	char dir[0x104], tmpPath[0x104], srcPath[0x104], drives[0x104 * 26];
	File_t src, dst;
	uint32_t srcTime[2] = {0, 0}, dstTime[2];
	int ok = 0, found = 0, keepLooking = 1, tries;
	uint8_t* buf = NULL;

	File_Ctor(&src);
	File_Ctor(&dst);
	PathDirPart(dir, j->dst);
	sprintf(tmpPath, "%s\\%s", dir, "inst.tmp");                 // the copy goes through this file in the destination directory
	Fs_FileSetAttrs(o, tmpPath, Fs_FileAttrs(o, tmpPath) & ~3u); // a leftover is made writable and unhidden (attributes 1 and 2)

	// find the source: poll the candidate drives for up to 5 seconds (100 x 50 ms), then ask for the disc
	while(keepLooking && !found)
	{
		int ndrives = CandidateDrives(o, drives), attempt;
		for(attempt = 0; attempt < 100 && !found; attempt++)
		{
			int i;
			for(i = 0; i < ndrives; i++)
			{
				char path[0x104];
				sprintf(path, "%s\\%s", drives + i * 0x104, j->marker);
				if(!DriveReady(o, path) || Fs_FileAttrs(o, path) == OPENBGI_INVALID_ATTRS) // the right disc has its marker
					continue;
				sprintf(srcPath, "%s\\%s", drives + i * 0x104, j->src);
				if(File_OpenRead(o, &src, srcPath))
				{
					Fs_FileGetTime(src.h, &srcTime[0], &srcTime[1]); // the time stamp (lo, hi) for the age test and the copy
					File_Close(&src);
					found = 1;
					break;
				}
			}
			if(!found && d->pause)
				d->pause(d->ui, 50);
		}
		if(!found)
			keepLooking = CopyJob_PromptDisc(o, j, d);
	}
	if(!found)
		goto done;

	// an installed copy that is not empty and not older than the source is kept
	if(File_OpenRead(o, &dst, j->dst))
	{
		uint32_t size = File_Size(&dst);
		Fs_FileGetTime(dst.h, &dstTime[0], &dstTime[1]);
		File_Close(&dst);
		if(size && (dstTime[1] > srcTime[1] || (dstTime[1] == srcTime[1] && dstTime[0] >= srcTime[0])))
		{
			ok = 1;
			goto done;
		}
	}

	// up to four attempts: a copy whose hash does not match its BGI.hvl record is redone
	for(tries = 0; tries < 4; tries++)
	{
		uint32_t size, remaining, blocks, block = 0;
		HvHash_t hash = {0, 0, 0, 0, 0};
		int retry = 0;

		ok = 0;
		if(!File_Create(o, &dst, tmpPath))
		{
			if(Dlg_Alive(o, d))
				Inst_Box(o, d->msgBox, d->ui, MSG_INST_CREATE_FAILED, MSG_ERROR_CAPTION, OPENBGI_MB_ICONHAND);
			break;
		}
		File_OpenRead(o, &src, srcPath); // the open that succeeded above is not checked again
		size = remaining = File_Size(&src);
		buf = (uint8_t*)BGI_Alloc(0x10000);
		blocks = (size + 0xffffu) >> 16; // the per-file bar counts 64 KB blocks
		if(Dlg_Alive(o, d) && d->setRange)
			d->setRange(d->ui, (int)blocks);
		while(remaining)
		{
			uint32_t n = remaining < 0x10000u ? remaining : 0x10000u;
			if(!Dlg_Alive(o, d)) // a cancel stops the copy; `remaining` stays non-zero and the file is not kept
				break;
			if(File_Read(&src, buf, n) != n)
			{
				if(Dlg_Alive(o, d))
					Inst_Box(o, d->msgBox, d->ui, MSG_INST_READ_ERROR, MSG_ERROR_CAPTION, OPENBGI_MB_ICONHAND);
				break;
			}
			HvHash_Update(&hash, buf, n);
			if(File_Write(&dst, buf, n) != n)
			{
				if(Dlg_Alive(o, d))
					Inst_Box(o, d->msgBox, d->ui, MSG_INST_WRITE_ERROR, MSG_ERROR_CAPTION, OPENBGI_MB_ICONHAND);
				break;
			}
			if(Dlg_Alive(o, d) && d->setPos)
				d->setPos(d->ui, (int)block++);
			remaining -= n;
		}
		if(remaining == 0)
		{
			// verify against BGI.hvl when the file is listed there (names compared in lower case);
			// an unlisted file passes
			ok = 1;
			if(o->install.hvlCount > 0)
			{
				char lname[0x104], rname[0x104];
				int i;
				strcpy(lname, j->name);
				SjisStrLwr(o, lname);
				for(i = 0; i < o->install.hvlCount; i++)
				{
					const uint8_t* rec = o->install.hvl + i * 0x40;
					memcpy(rname, rec, 0x38);
					rname[0x38] = 0;
					SjisStrLwr(o, rname);
					if(strcmp(lname, rname) == 0)
					{
						ok = memcmp(rec + 0x38, &hash, 8) == 0; // the record holds the 8-byte state as laid out in memory
						retry = !ok;
						break;
					}
				}
			}
			if(ok)
				Fs_FileSetTime(dst.h, srcTime[0], srcTime[1]); // the copy keeps the source's time stamp
		}
		BGI_Free(buf);
		buf = NULL;
		File_Close(&src);
		File_Close(&dst);
		if(ok)
		{
			// replace the installed file: a read-only or hidden one is made deletable first
			Fs_FileSetAttrs(o, j->dst, Fs_FileAttrs(o, j->dst) & ~3u);
			Fs_FileDelete(o, j->dst);
			Fs_FileMove(o, tmpPath, j->dst);
		}
		if(!retry)
			break;
		if(tries == 3 && Dlg_Alive(o, d)) // the last attempt failed its verification too
		{
			char text[0x104 + 0x40];
			sprintf(text, MSG_INST_FILE_BROKEN, j->name);
			Inst_Box(o, d->msgBox, d->ui, text, MSG_ERROR_CAPTION, OPENBGI_MB_ICONHAND);
		}
	}

done:
	Fs_FileDelete(o, tmpPath); // whatever is left of the temporary file
	File_Dtor(&src);
	File_Dtor(&dst);
	return ok;
}

/* The progress dialog's work, called by the host's dialog once the window
 * exists: one job after the other, the dialog told about each file
 * (`setFile`, `setTotal`), until the list is exhausted (1), a copy failed
 * or the user cancelled (0); the result becomes the dialog's.  Every file
 * is entered in o->install.madeList before its copy, so a failed one is
 * rolled back too. */
int Install_CopyAll(OpenBGI_t* o, InstallProgressDlg_t* d)
{
	int index;
	o->install.dlgAlive = 1;
	o->install.promptOpen = 0;
	for(index = 0;; index++)
	{
		CopyJob_t* j = CopyJob_New(o, &o->install.inst, index);
		int ok;
		if(!j)
			return 1;
		MadeList_Add(&o->install.madeList, j->name, j->dst); // for the rollback and uninst.lst
		if(d->setFile)
		{
			char caption[0x104 + 0x20];
			sprintf(caption, MSG_INST_INSTALLING, j->name);
			d->setFile(d->ui, index, caption);
		}
		if(d->setTotal)
			d->setTotal(d->ui, index);
		ok = CopyJob_Run(o, j, d);
		CopyJob_Free(j);
		if(!ok || !Dlg_Alive(o, d))
			return 0;
		if(d->setTotal)
			d->setTotal(d->ui, index + 1);
	}
}
