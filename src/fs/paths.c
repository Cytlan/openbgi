/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * paths.c - where the engine looks for its files: the base directory, the
 *           alternative (disc) directory, the boot program, the drives,
 *           the search list, and the path helpers (src/include/bgi/file.h)
 *
 * Every file name the scripts use is resolved against the base directory
 * (the game's own, normally the executable's) and then the alternative
 * directory, which "80 3F" points at the drive holding the game's disc.
 * Both are kept with a trailing backslash so that PathJoin is a plain
 * concatenation.  Paths are Windows paths ('\' separated, "x:" drive)
 * throughout; the host translates them (openbgi.h, "The files").
 */
#include "bgi/file.h"
#include "bgi/error.h"
#include "bgi/strutil.h"
#include "bgi/msg.h"
#include "bgi/host.h"
#include "bgi/sys.h"
#include "bgi/request.h"
#include "bgi/core.h"

// dir (with its trailing separator) + name into dst
void PathJoin(char* dst, const char* dir, const char* name)
{
	sprintf(dst, "%s%s", dir, name);
}

/* Set the base directory (NULL or "": the executable's), stored with a
 * trailing separator and made the current directory; the alternative
 * directory is cleared. */
void SetBaseDir(OpenBGI_t* o, const char* dir)
{
	if(!dir || !dir[0])
	{
		Host_ExeDir(o, o->files.baseDir, 0x104);
	}
	else if(dir[strlen(dir) - 1] == '\\')
	{
		strcpy(o->files.baseDir, dir);
	}
	else
	{
		sprintf(o->files.baseDir, "%s\\", dir);
	}
	o->files.altDir[0] = 0;
	Fs_SetCurrentDir(o, o->files.baseDir);
}

void SetBootName(OpenBGI_t* o, const char* arc, const char* file)
{
	strcpy(o->files.bootArc, arc);
	strcpy(o->files.bootFile, file);
}

void GetBootProgram(OpenBGI_t* o, char* arc, char* file)
{
	strcpy(arc, o->files.bootArc);
	strcpy(file, o->files.bootFile);
}

/* Decide the base directory and the boot archive from the command line's
 * path (also "80 6B", the reboot): "." keeps the executable's directory
 * and system.arc; a path that does not exist is split into a directory
 * and an archive name; a directory becomes the base; a file is the
 * archive in its directory.  `file` is the boot program within the
 * archive. */
void SetBootProgram(OpenBGI_t* o, const char* path, const char* file)
{
	uint32_t attr;
	SetBaseDir(o, NULL);
	SetBootName(o, "system.arc", file);
	if(strcmp(path, ".") == 0)
		return;
	attr = Fs_FileAttrs(o, path);
	if(attr == OPENBGI_INVALID_ATTRS)
	{
		char drive[260], dir[260], fname[260], ext[260], a[260], b[260];
		SplitPath(path, drive, dir, fname, ext);
		sprintf(a, "%s%s", drive, dir);
		sprintf(b, "%s%s", fname, ext);
		SetBaseDir(o, a);
		SetBootName(o, b, file);
	}
	else if(attr & OPENBGI_ATTR_DIRECTORY)
	{
		SetBaseDir(o, path);
	}
	else
	{
		char copy[260], *slash;
		strcpy(copy, path);
		slash = strrchr(copy, '\\');
		if(slash)
		{
			*slash = 0;
			SetBaseDir(o, copy);
			SetBootName(o, slash + 1, file);
		}
		else
		{
			SetBootName(o, path, file);
		}
	}
}

// "80 3D": the base directory (which 0) or the alternative one (1) into dst; 0 when there is none
int GetBasePath(OpenBGI_t* o, char* dst, int which)
{
	if(which == 0)
	{
		strcpy(dst, o->files.baseDir);
		return 1;
	}
	if(which == 1 && o->files.altDir[0])
	{
		strcpy(dst, o->files.altDir);
		return 1;
	}
	return 0;
}

// ---- the engine's own program file ("81 3F", file.h) ----------------------------------

/* The bytes of the file: the same for every game and every run, and of no
 * meaning - a sequence that looks like nothing else a program reads, so
 * that memory which still holds it is told from memory that does not. */
const uint8_t* Program_Image(OpenBGI_t* o)
{
	return o->files.programImage;
}

void ProgramPath(OpenBGI_t* o, char* dst)
{
	if(!o->files.programPath[0])
	{
		char dir[0x104];
		uint32_t x = 0x4F424749u, i;
		for(i = 0; i < PROGRAM_IMAGE_SIZE; i++)
		{
			x ^= x << 13;
			x ^= x >> 17;
			x ^= x << 5;
			o->files.programImage[i] = (uint8_t)(x >> 11);
		}
		Host_ExeDir(o, dir, sizeof dir);
		snprintf(o->files.programPath, sizeof o->files.programPath, "%sBGI.exe", dir);
	}
	strcpy(dst, o->files.programPath);
}

// the letters of ASCII without their case
static int SameName(const char* a, const char* b)
{
	for(; *a && *b; a++, b++)
	{
		int x = (unsigned char)*a, y = (unsigned char)*b;
		if(x >= 'A' && x <= 'Z')
			x += 'a' - 'A';
		if(y >= 'A' && y <= 'Z')
			y += 'a' - 'A';
		if(x != y)
			return 0;
	}
	return *a == *b;
}

int Program_IsPath(OpenBGI_t* o, const char* path)
{
	return o->files.programPath[0] && SameName(path, o->files.programPath);
}

// a loose file as the loader names it: an absolute name as it is, else in the base directory
int Program_Is(OpenBGI_t* o, const char* arc, const char* name)
{
	char path[0x210];
	if(arc || !name || !o->files.programPath[0])
		return 0;
	if(name[0] == '\\' || name[1] == ':')
		return Program_IsPath(o, name);
	snprintf(path, sizeof path, "%s%s", o->files.baseDir, name);
	return Program_IsPath(o, path);
}

// ---- drives ---------------------------------------------------------------------------------

// Record the type of every drive letter (once at start-up).
void Drives_Scan(OpenBGI_t* o)
{
	char root[4] = "A:\\";
	int i;
	for(i = 0; i < 26; i++)
	{
		root[0] = (char)('A' + i);
		o->files.driveType[i] = Host_DriveType(o, root);
		o->files.driveNotFixed[i] = o->files.driveType[i] != OPENBGI_DRIVE_FIXED;
	}
}

// the scanned type of a drive letter (either case); OPENBGI_DRIVE_UNKNOWN for anything else
uint32_t Drive_TypeOf(OpenBGI_t* o, char letter)
{
	int i = (letter | 0x20) - 'a';
	return (unsigned)i < 26 ? o->files.driveType[i] : OPENBGI_DRIVE_UNKNOWN;
}

/* Whether the drive of a path has its medium in: a fixed disk always
 * does, any other drive is asked (the medium, then the volume).  A path
 * without a drive letter is never ready. */
int DriveReady(OpenBGI_t* o, const char* path)
{
	char buf[0x104];
	char d;
	strcpy(buf, path);
	SjisStrLwr(o, buf);
	d = buf[0];
	if(d < 'a' || d > 'z' || buf[1] != ':')
		return 0;
	if(!o->files.driveNotFixed[d - 'a'])
		return 1;
	if(Host_MediaPresent(o, d))
		return 1;
	{
		char root[4];
		sprintf(root, "%c:\\", d);
		return Host_VolumeReady(o, root);
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

/* The search of "80 3F" for the disc holding `name` (MediaSearch_t,
 * bgi/file.h): every CD-ROM drive is tried 100 times, 50 ms apart; then,
 * with askAbort, the user is asked to insert the disc (OK searches again,
 * Cancel asks whether to give up), without it the search is over.  When
 * found, the drive's root plus the name's directory part becomes the
 * alternative directory.  The original searches inside the instruction;
 * here a step of the search is taken per frame. */
void MediaSearch_Begin(OpenBGI_t* o, MediaSearch_t* m, int askAbort)
{
	const char* p = m->drives;
	memset(m, 0, sizeof *m);
	if(Host_LogicalDrives(o, m->drives, sizeof m->drives) && *p)
	{
		while(*p) // the roots, "A:\", NUL separated, NUL NUL terminated
		{
			if(Host_DriveType(o, p) == OPENBGI_DRIVE_CDROM && m->ncd < 64)
				m->cd[m->ncd++] = p;
			p += strlen(p) + 1;
		}
	}
	PathDirPart(m->file, o->files.installName);
	m->askAbort = askAbort;
	m->next = Clock_HostMs(o);
}

// one try over the drives; 1 found
static int MediaSearch_Try(OpenBGI_t* o, MediaSearch_t* m)
{
	char path[0x104];
	int i;
	for(i = 0; i < m->ncd; i++)
	{
		if(!DriveReady(o, m->cd[i]))
			continue;
		PathJoin(path, m->cd[i], o->files.installName);
		if(Fs_FileAttrs(o, path) != OPENBGI_INVALID_ATTRS)
		{
			sprintf(o->files.altDir, "%s%s", m->cd[i], m->file);
			return 1;
		}
	}
	return 0;
}

int MediaSearch_Step(OpenBGI_t* o, MediaSearch_t* m)
{
	for(;;)
	{
		int32_t left;
		if(m->asking)
		{
			int a = AskRetry_Step(o, &m->ask, o->files.installMsg, o->files.installTitle, o->files.installTitle, 0);
			m->idleMs = 0;
			if(a == ASK_WAIT)
				return 0;
			if(a == ASK_ABORT)
				return -1;
			m->asking = 0;
			m->tries = 0;
			m->next = Clock_HostMs(o);
		}
		left = (int32_t)(m->next - Clock_HostMs(o));
		if(left > 0)
		{
			m->idleMs = (uint32_t)left; // the 50 ms between two tries
			return 0;
		}
		if(m->tries == 100)
		{
			if(!m->askAbort)
				return -1;
			m->asking = 1;
			continue;
		}
		if(MediaSearch_Try(o, m))
			return 1;
		m->tries++;
		m->next = Clock_HostMs(o) + 50;
	}
}

/* "80 3F": keep the name of the file that marks the disc and the prompt
 * (`msg`, `title`) to show when it is missing, for the search that
 * follows (MediaSearch_Begin) and the loader's retry prompts. */
void SetInstallPaths(OpenBGI_t* o, const char* name, const char* msg, const char* title)
{
	strcpy(o->files.installName, name);
	strcpy(o->files.installMsg, msg);
	strcpy(o->files.installTitle, title);
}

/* The retry prompt of the loader when a file is missing: with an
 * alternative directory (a disc) the user may insert it and retry, the
 * box showing the strings stored by "80 3F" (Retry_t, bgi/request.h);
 * with none the error `msg` is fatal.  The original asks inside the load
 * and tries again after a 200 ms pause; here the load is left
 * (BGI_RETRY_VALUE) for the one who can wait for the answers to ask and
 * try again: the scheduler starts the instruction over, the boot boots
 * again, the loader queues the job (src/vm/sched.c, src/fs/loader.c).
 * Anywhere else - a wait's poll, the frame's services - there is no one
 * to try again, and the file counts as missing for good. */
void RetryPrompt(OpenBGI_t* o, const char* msg)
{
	if(!o->files.altDir[0])
		ThrowScriptError(o, msg);
	if(o->files.inLoad || o->vm.booting || (o->vm.sched.cur && !o->vm.inPoll))
		BGI_ThrowInt(o, BGI_RETRY_VALUE);
	ThrowScriptError(o, msg);
}

// ---- the search list ("80 36" / "80 37") ----------------------------------------------------

// "80 36": turn the search list on or off (the list itself is kept)
void Set_FileSearch(OpenBGI_t* o, int on)
{
	o->files.fileSearchOn = on;
}

// drop every registered sub-directory (a reboot and the shutdown)
void FileSearch_Clear(OpenBGI_t* o)
{
	SearchNode_t *n = o->files.searchList, *nx;
	while(n)
	{
		nx = n->next;
		BGI_Free(n->name);
		BGI_Free(n);
		n = nx;
	}
	o->files.searchList = NULL;
}

// "80 37": register a sub-directory to try after a directory itself (the newest registration first).
void PushSearchName(OpenBGI_t* o, const char* name)
{
	SearchNode_t* n = (SearchNode_t*)BGI_Alloc(sizeof(SearchNode_t));
	n->name = BGI_Strdup(name);
	n->next = o->files.searchList;
	o->files.searchList = n;
}
