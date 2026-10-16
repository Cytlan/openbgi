/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * paths.c - where the engine looks for its files: the base directory, the
 *           alternative (disc) directory, the boot program, the drives,
 *           the search list, and the path helpers (inc/bgi/file.h)
 *
 * Every file name the scripts use is resolved against the base directory
 * (the game's own, normally the executable's) and then the alternative
 * directory, which "80 3F" points at the drive holding the game's disc.
 * Both are kept with a trailing backslash so that PathJoin is a plain
 * concatenation.  Paths are Windows paths ('\' separated, "x:" drive)
 * throughout; the OS layer translates them.
 */
#include "bgi/file.h"
#include "bgi/error.h"
#include "bgi/strutil.h"
#include "bgi/msg.h"
#include "bgi/os.h"

char gBaseDir[0x104];
char gAltDir[0x104];
char gBootArc[0x104] = "system.arc";
char gBootFile[0x104] = "ipl._bp";
int gFileSearchOn;

// the disc prompt of "80 3F", kept for the retry prompts of the loader
static char gInstallName[0x104];
static char gInstallMsg[0x104];
static char gInstallTitle[0x104];

// dir (with its trailing separator) + name into dst
void PathJoin(char* dst, const char* dir, const char* name)
{
	sprintf(dst, "%s%s", dir, name);
}

/* Set the base directory (NULL: the executable's), stored with a
 * trailing separator and made the current directory; the alternative
 * directory is cleared. */
void SetBaseDir(const char* dir)
{
	if(!dir)
	{
		OS_ExeDir(gBaseDir, 0x104);
	}
	else if(dir[strlen(dir) - 1] == '\\')
	{
		strcpy(gBaseDir, dir);
	}
	else
	{
		sprintf(gBaseDir, "%s\\", dir);
	}
	gAltDir[0] = 0;
	OS_SetCurrentDir(gBaseDir);
}

void SetBootName(const char* arc, const char* file)
{
	strcpy(gBootArc, arc);
	strcpy(gBootFile, file);
}

void GetBootProgram(char* arc, char* file)
{
	strcpy(arc, gBootArc);
	strcpy(file, gBootFile);
}

/* Decide the base directory and the boot archive from the command line's
 * path (also "80 6B", the reboot): "." keeps the executable's directory
 * and system.arc; a path that does not exist is split into a directory
 * and an archive name; a directory becomes the base; a file is the
 * archive in its directory.  `file` is the boot program within the
 * archive. */
void SetBootProgram(const char* path, const char* file)
{
	uint32_t attr;
	SetBaseDir(NULL);
	SetBootName("system.arc", file);
	if(strcmp(path, ".") == 0)
		return;
	attr = OS_FileAttrs(path);
	if(attr == OS_INVALID_ATTRS)
	{
		char drive[260], dir[260], fname[260], ext[260], a[260], b[260];
		SplitPath(path, drive, dir, fname, ext);
		sprintf(a, "%s%s", drive, dir);
		sprintf(b, "%s%s", fname, ext);
		SetBaseDir(a);
		SetBootName(b, file);
	}
	else if(attr & OS_ATTR_DIRECTORY)
	{
		SetBaseDir(path);
	}
	else
	{
		char copy[260], *slash;
		strcpy(copy, path);
		slash = strrchr(copy, '\\');
		if(slash)
		{
			*slash = 0;
			SetBaseDir(copy);
			SetBootName(slash + 1, file);
		}
		else
		{
			SetBootName(path, file);
		}
	}
}

// "80 3D": the base directory (which 0) or the alternative one (1) into dst; 0 when there is none
int GetBasePath(char* dst, int which)
{
	if(which == 0)
	{
		strcpy(dst, gBaseDir);
		return 1;
	}
	if(which == 1 && gAltDir[0])
	{
		strcpy(dst, gAltDir);
		return 1;
	}
	return 0;
}

// ---- drives ---------------------------------------------------------------------------------

static uint32_t gDriveType[26]; // OS_DRIVE_* per letter, from Drives_Scan
static int gDriveNotFixed[26];  // 1 unless the drive is a fixed disk: its medium is checked by DriveReady

// Record the type of every drive letter (once at start-up).
void Drives_Scan(void)
{
	char root[4] = "A:\\";
	int i;
	for(i = 0; i < 26; i++)
	{
		root[0] = (char)('A' + i);
		gDriveType[i] = OS_DriveType(root);
		gDriveNotFixed[i] = gDriveType[i] != OS_DRIVE_FIXED;
	}
}

// the scanned type of a drive letter (either case); OS_DRIVE_UNKNOWN for anything else
uint32_t Drive_TypeOf(char letter)
{
	int i = (letter | 0x20) - 'a';
	return (unsigned)i < 26 ? gDriveType[i] : OS_DRIVE_UNKNOWN;
}

/* Whether the drive of a path has its medium in: a fixed disk always
 * does, any other drive is asked (the medium, then the volume).  A path
 * without a drive letter is never ready. */
int DriveReady(const char* path)
{
	char buf[0x104];
	char d;
	strcpy(buf, path);
	SjisStrLwr(buf);
	d = buf[0];
	if(d < 'a' || d > 'z' || buf[1] != ':')
		return 0;
	if(!gDriveNotFixed[d - 'a'])
		return 1;
	if(OS_MediaPresent(d))
		return 1;
	{
		char root[4];
		sprintf(root, "%c:\\", d);
		return OS_VolumeReady(root);
	}
}

/* The first n + 1 elements of a '\'-separated path ("c:\a\b", 1 ->
 * "c:\a").  Only the root keeps its trailing backslash ("c:\", 0 ->
 * "c:\").  0 when the path has no element n (dst is not terminated then);
 * a negative n gives "" and 1. */
int PathPrefix(char* dst, const char* src, int n)
{
	const char* p = src;
	char* d = dst;
	int more = n > 0;
	if(n >= 0)
	{
		while(*p)
		{
			char c = *p;
			if(c == '\\' && p[1])
				n--;
			*d++ = c;
			p++;
			if(!*p)
				n--;
			if(n < 0)
				break;
		}
	}
	if(n >= 0)
		return 0;
	if(more && *p)
		d[-1] = 0; // drop the separator that ended element n
	else
		*d = 0;
	return 1;
}

// The path without its last element ("c:\a\b" -> "c:\a", "a\b" -> "a\", "b" -> "").
void PathDirPart(char* dst, const char* path)
{
	int n = 0;
	while(PathPrefix(dst, path, n))
		n++;
	PathPrefix(dst, path, n - 2);
}

/* Look for `name` on every CD-ROM drive; when found, the drive root plus
 * the name's directory part becomes outDir.  Polls 100 times with 50 ms
 * pauses, then asks the user to insert the disc (OK retries, Cancel asks
 * whether to abort) when askAbort is set; without it one round of polling
 * is all.  1 when found. */
static int FindMediaDrive(char* outDir, const char* name, const char* msg, const char* title, int askAbort)
{
	char drives[0x400];
	const char* cd[64];
	int ncd = 0, found = 0, keepGoing = 1;
	char file[0x104], path[0x104];
	uint32_t n = OS_LogicalDrives(drives, sizeof drives);
	const char* p = drives;

	if(n && *p)
	{
		while(*p) // the roots, "A:\", NUL separated, NUL NUL terminated
		{
			if(OS_DriveType(p) == OS_DRIVE_CDROM && ncd < 64)
				cd[ncd++] = p;
			p += strlen(p) + 1;
		}
	}
	PathDirPart(file, name);

	while(keepGoing)
	{
		int tries;
		for(tries = 0; tries < 100 && !found; tries++)
		{
			int i;
			for(i = 0; i < ncd; i++)
			{
				if(!DriveReady(cd[i]))
					continue;
				PathJoin(path, cd[i], name);
				if(OS_FileAttrs(path) != OS_INVALID_ATTRS)
				{
					sprintf(outDir, "%s%s", cd[i], file);
					found = 1;
					keepGoing = 0;
					break;
				}
			}
			if(!found)
				OS_SleepMs(50);
		}
		if(found)
			break;
		if(!askAbort)
			break;
		if(MsgBox(msg, title, OS_MB_OKCANCEL | OS_MB_ICONINFO) != OS_IDOK)
		{
			if(MsgBox(MSG_ASK_ABORT, title, OS_MB_YESNO | OS_MB_ICONQUESTION | OS_MB_DEFBUTTON2) == OS_IDYES)
				break;
		}
	}
	return found;
}

/* "80 3F": make the disc holding `name` the alternative directory, with
 * the prompt (`msg`, `title`) to show when it is missing, kept for the
 * loader's retry prompts; 1 when the disc was found. */
int SetInstallPaths(const char* name, const char* msg, const char* title, int askAbort)
{
	strcpy(gInstallName, name);
	strcpy(gInstallMsg, msg);
	strcpy(gInstallTitle, title);
	return FindMediaDrive(gAltDir, name, msg, title, askAbort);
}

/* The retry prompt of the loader when a file is missing: with an
 * alternative directory (a disc) the user may insert it and retry, the
 * box showing the strings stored by "80 3F"; with none the error `msg` is
 * fatal.  Cancel followed by "abort? yes" throws.  Returns after a 200 ms
 * pause when the user retries. */
void RetryPrompt(const char* msg)
{
	if(!gAltDir[0])
	{
		ThrowScriptError(msg);
		return;
	}
	if(MsgBox(gInstallTitle, gInstallMsg, OS_MB_OKCANCEL | OS_MB_ICONINFO) == OS_IDCANCEL)
	{
		if(MsgBox(MSG_ASK_ABORT, gInstallMsg, OS_MB_YESNO | OS_MB_ICONQUESTION | OS_MB_DEFBUTTON2) == OS_IDYES)
			BGI_ThrowInt(BGI_SCRIPT_ERROR_VALUE);
	}
	OS_SleepMs(200);
}

// ---- the search list ("80 36" / "80 37") ----------------------------------------------------

SearchNode_t* gSearchList;

// "80 36": turn the search list on or off (the list itself is kept)
void Set_FileSearch(int on)
{
	gFileSearchOn = on;
}

// drop every registered sub-directory (a reboot and the shutdown)
void FileSearch_Clear(void)
{
	SearchNode_t *n = gSearchList, *nx;
	while(n)
	{
		nx = n->next;
		BGI_Free(n->name);
		BGI_Free(n);
		n = nx;
	}
	gSearchList = NULL;
}

// "80 37": register a sub-directory to try after a directory itself (the newest registration first).
void PushSearchName(const char* name)
{
	SearchNode_t* n = (SearchNode_t*)BGI_Alloc(sizeof(SearchNode_t));
	n->name = BGI_Strdup(name);
	n->next = gSearchList;
	gSearchList = n;
}
