/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * copy.c - the copy engine of the installation (install_internal.h; the
 *          progress dialog's entry point Install_CopyAll is in
 *          inc/bgi/install.h)
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
 * operations, prompts and results.
 */
#include "install_internal.h"
#include "bgi/file.h"
#include "bgi/strutil.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/os.h"

// the state of the running installation, set up by Install_RunDialog (install_internal.h)
InstallCtx_t gInst;
MadeList_t gMadeList;
char** gDiscMarkers;
char** gDiscMsgs;
const int* gDiscCounts;
int gHvlCount;
uint8_t* gHvl;
char gDlgPath[0x104 + 4];

static int gDlgAlive;   // the progress dialog is open (the user has not cancelled)
static int gPromptOpen; // a copy job shows a message box (kept as state; nothing reads it here)

// the disc a file index belongs to, from the per-disc counts
static int DiscOfFile(int index)
{
	int disc = 0;
	int rem = index - gDiscCounts[0];
	while(rem >= 0)
	{
		disc++;
		rem -= gDiscCounts[disc];
	}
	return disc;
}

/* The drives a disc may be in: "x:" of every CD-ROM drive and of the drive
 * the engine runs from (gBaseDir), 0x104 bytes per entry in `out` (room
 * for 26); the count. */
int CandidateDrives(char* out)
{
	char drives[0x400], base[0x104], root[0x104];
	const char* p;
	int n = 0;
	OS_LogicalDrives(drives, sizeof drives);
	strcpy(base, gBaseDir);
	SjisStrLwr(base);
	for(p = drives; *p; p += strlen(p) + 1)
	{
		int take = OS_DriveType(p) == OS_DRIVE_CDROM;
		if(!take)
		{
			strcpy(root, p);
			SjisStrLwr(root);
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
static CopyJob_t* CopyJob_New(const InstallCtx_t* ctx, int index)
{
	CopyJob_t* j;
	char buf[0x104];
	int disc;
	if(index >= ctx->count)
		return NULL;
	j = (CopyJob_t*)BGI_Alloc(sizeof(CopyJob_t));
	disc = DiscOfFile(index);
	j->name = BGI_Strdup(ctx->files[index]);
	sprintf(buf, "%s\\%s", ctx->installDir, ctx->files[index]);
	j->dst = BGI_Strdup(buf);
	sprintf(buf, "%s\\%s", ctx->srcRel, ctx->files[index]);
	j->src = BGI_Strdup(buf);
	sprintf(buf, "%s\\%s", ctx->srcRel, gDiscMarkers[disc]);
	j->marker = BGI_Strdup(buf);
	j->msg = BGI_Strdup(gDiscMsgs[disc]);
	j->index = index;
	return j;
}

// pump the dialog through its `alive` callback and note when the user cancelled; 1 while the dialog is open
static int Dlg_Alive(InstallProgressDlg_t* d)
{
	if(d->alive && !d->alive(d->ui))
		gDlgAlive = 0;
	return gDlgAlive;
}

/* the "insert the disc" prompt (OK / Cancel; Cancel asks whether to abort
 * the installation); 1 when the user wants another try, 0 to give up (also
 * when the dialog is gone) */
static int CopyJob_PromptDisc(const CopyJob_t* j, InstallProgressDlg_t* d)
{
	int retry = 0;
	gPromptOpen = 1;
	if(Dlg_Alive(d))
	{
		retry = 1;
		if(MsgBox(j->msg, MSG_NOTICE, OS_MB_OKCANCEL | OS_MB_ICONINFO) != OS_IDOK)
		{
			if(MsgBox(MSG_ASK_ABORT, MSG_CONFIRM, OS_MB_YESNO | OS_MB_ICONQUESTION | OS_MB_DEFBUTTON2) == OS_IDYES)
				retry = 0;
		}
	}
	gPromptOpen = 0;
	return retry;
}

/* Copy one file, reporting through the dialog's callbacks.  1 when the file
 * is in place (copied, or already present and not older than the source),
 * 0 on failure (the source was not found and the user gave up, the
 * temporary file could not be created, a read or write error, four
 * attempts with a wrong hash) or when the user cancelled the dialog. */
static int CopyJob_Run(const CopyJob_t* j, InstallProgressDlg_t* d)
{
	char dir[0x104], tmpPath[0x104], srcPath[0x104], drives[0x104 * 26];
	File_t src, dst;
	uint32_t srcTime[2] = {0, 0}, dstTime[2];
	int ok = 0, found = 0, keepLooking = 1, tries;
	uint8_t* buf = NULL;

	File_Ctor(&src);
	File_Ctor(&dst);
	PathDirPart(dir, j->dst);
	sprintf(tmpPath, "%s\\%s", dir, "inst.tmp");           // the copy goes through this file in the destination directory
	OS_FileSetAttrs(tmpPath, OS_FileAttrs(tmpPath) & ~3u); // a leftover is made writable and unhidden (attributes 1 and 2)

	// find the source: poll the candidate drives for up to 5 seconds (100 x 50 ms), then ask for the disc
	while(keepLooking && !found)
	{
		int ndrives = CandidateDrives(drives), attempt;
		for(attempt = 0; attempt < 100 && !found; attempt++)
		{
			int i;
			for(i = 0; i < ndrives; i++)
			{
				char path[0x104];
				sprintf(path, "%s\\%s", drives + i * 0x104, j->marker);
				if(!DriveReady(path) || OS_FileAttrs(path) == OS_INVALID_ATTRS) // the right disc has its marker
					continue;
				sprintf(srcPath, "%s\\%s", drives + i * 0x104, j->src);
				if(File_OpenRead(&src, srcPath))
				{
					OS_FileGetTime(src.h, &srcTime[0], &srcTime[1]); // the time stamp (lo, hi) for the age test and the copy
					File_Close(&src);
					found = 1;
					break;
				}
			}
			if(!found)
				OS_SleepMs(50);
		}
		if(!found)
			keepLooking = CopyJob_PromptDisc(j, d);
	}
	if(!found)
		goto done;

	// an installed copy that is not empty and not older than the source is kept
	if(File_OpenRead(&dst, j->dst))
	{
		uint32_t size = File_Size(&dst);
		OS_FileGetTime(dst.h, &dstTime[0], &dstTime[1]);
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
		if(!File_Create(&dst, tmpPath))
		{
			if(Dlg_Alive(d))
				MsgBox(MSG_INST_CREATE_FAILED, MSG_ERROR_CAPTION, OS_MB_ICONHAND);
			break;
		}
		File_OpenRead(&src, srcPath); // the open that succeeded above is not checked again
		size = remaining = File_Size(&src);
		buf = (uint8_t*)BGI_Alloc(0x10000);
		blocks = (size + 0xffffu) >> 16; // the per-file bar counts 64 KB blocks
		if(Dlg_Alive(d) && d->setRange)
			d->setRange(d->ui, (int)blocks);
		while(remaining)
		{
			uint32_t n = remaining < 0x10000u ? remaining : 0x10000u;
			if(!Dlg_Alive(d)) // a cancel stops the copy; `remaining` stays non-zero and the file is not kept
				break;
			if(File_Read(&src, buf, n) != n)
			{
				if(Dlg_Alive(d))
					MsgBox(MSG_INST_READ_ERROR, MSG_ERROR_CAPTION, OS_MB_ICONHAND);
				break;
			}
			HvHash_Update(&hash, buf, n);
			if(File_Write(&dst, buf, n) != n)
			{
				if(Dlg_Alive(d))
					MsgBox(MSG_INST_WRITE_ERROR, MSG_ERROR_CAPTION, OS_MB_ICONHAND);
				break;
			}
			if(Dlg_Alive(d) && d->setPos)
				d->setPos(d->ui, (int)block++);
			remaining -= n;
		}
		if(remaining == 0)
		{
			// verify against BGI.hvl when the file is listed there (names compared in lower case);
			// an unlisted file passes
			ok = 1;
			if(gHvlCount > 0)
			{
				char lname[0x104], rname[0x104];
				int i;
				strcpy(lname, j->name);
				SjisStrLwr(lname);
				for(i = 0; i < gHvlCount; i++)
				{
					const uint8_t* rec = gHvl + i * 0x40;
					memcpy(rname, rec, 0x38);
					rname[0x38] = 0;
					SjisStrLwr(rname);
					if(strcmp(lname, rname) == 0)
					{
						ok = memcmp(rec + 0x38, &hash, 8) == 0; // the record holds the 8-byte state as laid out in memory
						retry = !ok;
						break;
					}
				}
			}
			if(ok)
				OS_FileSetTime(dst.h, srcTime[0], srcTime[1]); // the copy keeps the source's time stamp
		}
		BGI_Free(buf);
		buf = NULL;
		File_Close(&src);
		File_Close(&dst);
		if(ok)
		{
			// replace the installed file: a read-only or hidden one is made deletable first
			OS_FileSetAttrs(j->dst, OS_FileAttrs(j->dst) & ~3u);
			OS_FileDelete(j->dst);
			OS_FileMove(tmpPath, j->dst);
		}
		if(!retry)
			break;
		if(tries == 3 && Dlg_Alive(d)) // the last attempt failed its verification too
		{
			char text[0x104 + 0x40];
			sprintf(text, MSG_INST_FILE_BROKEN, j->name);
			MsgBox(text, MSG_ERROR_CAPTION, OS_MB_ICONHAND);
		}
	}

done:
	OS_FileDelete(tmpPath); // whatever is left of the temporary file
	File_Dtor(&src);
	File_Dtor(&dst);
	return ok;
}

/* The progress dialog's work, called by the OS layer once the window
 * exists: one job after the other, the dialog told about each file
 * (`setFile`, `setTotal`), until the list is exhausted (1), a copy failed
 * or the user cancelled (0); the result becomes the dialog's.  Every file
 * is entered in gMadeList before its copy, so a failed one is rolled back
 * too. */
int Install_CopyAll(InstallProgressDlg_t* d)
{
	int index;
	gDlgAlive = 1;
	gPromptOpen = 0;
	for(index = 0;; index++)
	{
		CopyJob_t* j = CopyJob_New(&gInst, index);
		int ok;
		if(!j)
			return 1;
		MadeList_Add(&gMadeList, j->name, j->dst); // for the rollback and uninst.lst
		if(d->setFile)
		{
			char caption[0x104 + 0x20];
			sprintf(caption, MSG_INST_INSTALLING, j->name);
			d->setFile(d->ui, index, caption);
		}
		if(d->setTotal)
			d->setTotal(d->ui, index);
		ok = CopyJob_Run(j, d);
		CopyJob_Free(j);
		if(!ok || !Dlg_Alive(d))
			return 0;
		if(d->setTotal)
			d->setTotal(d->ui, index + 1);
	}
}
