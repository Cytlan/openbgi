/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * paths.c - Windows paths on a POSIX file system (inc/bgi/os_posix.h)
 *
 * Paths arrive as the engine builds them (Shift-JIS, '\\' separators,
 * optionally a "C:\" drive).  OsPosix_Path() turns them into UTF-8 POSIX
 * paths under the game directory, and when a path does not exist as
 * spelled it looks for a case-insensitive match component by component,
 * because the game data was written for a case-insensitive file system.
 * Every file service of files.c goes through it, so the engine never
 * sees a POSIX path.
 */
#include "posix_internal.h"

/* one entry of `dir` ("" = the current directory) that matches `name`
 * without regard to ASCII case; `name` is replaced in place by the entry's
 * spelling when a match exists (the first one readdir yields) */
static void MatchCase(const char* dir, char* name)
{
	DIR* d = opendir(dir[0] ? dir : ".");
	struct dirent* e;
	if(!d)
		return;
	while((e = readdir(d)) != NULL)
	{
		if(strcasecmp(e->d_name, name) == 0)
		{
			strcpy(name, e->d_name);
			break;
		}
	}
	closedir(d);
}

/* Shift-JIS Windows path -> UTF-8 POSIX path (into out, n bytes).  A drive
 * letter (any letter) maps to the executable's directory ("C:\" is where
 * the game lives), a leading separator to the file system root; '\\'
 * becomes '/'; every component that does not exist as spelled is replaced
 * by a case-insensitive match when there is one.  A relative path stays
 * relative (to the current directory); an empty one becomes "." or "/". */
void OsPosix_Path(const char* win, char* out, size_t n)
{
	char utf[PATH_MAX * 2];
	char cur[PATH_MAX]; // the POSIX path built so far
	const char* p;
	size_t o = 0;
	int absolute = 0;

	OsPosix_SjisToUtf8(win, utf, sizeof utf);
	p = utf;
	if(((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
	{
		p += 2;
		if(*p == '\\' || *p == '/')
			p++;
		snprintf(cur, sizeof cur, "%s", gPosixExeDir); // the drive root is the game directory
		o = strlen(cur);
		if(o && cur[o - 1] == '/')
			cur[--o] = 0;
		absolute = 1;
	}
	else if(*p == '\\' || *p == '/')
	{
		cur[0] = 0;
		absolute = 1;
		p++;
	}
	else
		cur[0] = 0;
	BGI_UNUSED(absolute);

	while(*p)
	{ // one component at a time: probe it as spelled, then look for another spelling
		char comp[PATH_MAX];
		size_t c = 0;
		struct stat st;
		while(*p && *p != '\\' && *p != '/' && c + 1 < sizeof comp)
			comp[c++] = *p++;
		comp[c] = 0;
		while(*p == '\\' || *p == '/')
			p++;
		if(c == 0)
			continue;
		if(strcmp(comp, ".") != 0 && strcmp(comp, "..") != 0)
		{
			char probe[PATH_MAX];
			const char* dir = cur[0] ? cur : absolute ? "/"
													  : ".";
			snprintf(probe, sizeof probe, "%s%s%s", cur, cur[0] || absolute ? "/" : "", comp);
			if(stat(probe, &st) != 0)
				MatchCase(dir, comp);
		}
		snprintf(cur + strlen(cur), sizeof cur - strlen(cur), "%s%s", cur[0] || absolute ? "/" : "", comp);
	}
	if(!cur[0])
		strcpy(cur, absolute ? "/" : ".");
	snprintf(out, n, "%s", cur);
}
