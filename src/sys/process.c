/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * process.c - what a game asks of other programs ("80 E0", "80 E1",
 *             "80 E2", "80 E3") and the audio availability check of the
 *             start-up; declared in bgi/install.h
 *
 * The original starts programs for the scripts - a registration tool,
 * the uninstaller, the game a launcher hands over to (CreateProcess) -
 * and opens files and links with the program the system has for them
 * (ShellExecute "open").  OpenBGI starts no program for a game, on any
 * system: such a request is refused, said on stderr, and the script
 * gets the answer the original gives when the program could not be
 * started and the player gave up (0), without the boxes that come before
 * it there.  Of the second kind a web link and a document are opened,
 * since that is what the games mean by it (the maker's site, the
 * manual); anything else - a program, a shortcut, a script - is refused
 * the same way.
 */
#include "bgi/install.h"
#include "bgi/host.h"
#include "bgi/fs.h"
#include "bgi/sjis.h"
#include "bgi/core.h"

#include <stdio.h>
#include <string.h>

// a text of the scripts' for a line on stderr
static void Shown(OpenBGI_t* o, const char* text, char* out, size_t n)
{
	Sjis_ToUtf8(text ? text : "", out, n, o->enc.textUtf8);
}

int Program_Refuse(OpenBGI_t* o, const char* dir, const char* file)
{
	char name[0x400], shown[0x800];
	if(dir)
		snprintf(name, sizeof name, "%s\\%s", dir, file ? file : "");
	else
		snprintf(name, sizeof name, "%s%s", o->files.baseDir, file ? file : "");
	Shown(o, name, shown, sizeof shown);
	fprintf(stderr, "openbgi: the game asked to start the program \"%s\": not started (OpenBGI starts no programs for a game)\n", shown);
	return 0;
}

// the same letters, whatever their case (ASCII)
static int SameNoCase(const char* a, const char* b, size_t n)
{
	size_t i;
	for(i = 0; i < n; i++)
	{
		char x = a[i], y = b[i];
		if(x >= 'A' && x <= 'Z')
			x = (char)(x + 32);
		if(y >= 'A' && y <= 'Z')
			y = (char)(y + 32);
		if(x != y)
			return 0;
		if(!x)
			break;
	}
	return 1;
}

int ShellOpen_IsLink(const char* path)
{
	return SameNoCase(path, "http://", 7) || SameNoCase(path, "https://", 8);
}

int ShellOpen_IsDocument(OpenBGI_t* o, const char* path)
{
	// what a desktop shows and does not run: text, pages, pictures
	static const char* const kKinds[] = {".txt", ".htm", ".html", ".pdf", ".rtf", ".png", ".jpg", ".jpeg", ".bmp", ".gif"};
	const char* dot = NULL;
	size_t i;
	if((path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/'))
		return 0; // another machine's file
	for(i = 0; path[i]; i++)
	{
		if(!o->enc.textUtf8 && Sjis_IsLead((uint8_t)path[i]) && path[i + 1])
			i++; // (a trail byte is no separator and no dot)
		else if(path[i] == ':' && !(i == 1 && ((path[0] | 32) >= 'a' && (path[0] | 32) <= 'z')))
			return 0; // no path of a file: a link of some kind, a stream of a file
		else if(path[i] == '\\' || path[i] == '/')
			dot = NULL;
		else if(path[i] == '.')
			dot = path + i;
	}
	for(i = 0; dot && i < sizeof kKinds / sizeof kKinds[0]; i++)
		if(strlen(dot) == strlen(kKinds[i]) && SameNoCase(dot, kKinds[i], strlen(dot)))
			return 1;
	return 0;
}

int ShellOpen(OpenBGI_t* o, const char* path)
{
	char local[0x400], shown[0x800];
	if(!path)
		return 0;
	if(ShellOpen_IsLink(path))
		return Host_OpenDocument(o, path);
	if(!ShellOpen_IsDocument(o, path))
	{
		Shown(o, path, shown, sizeof shown);
		fprintf(stderr, "openbgi: the game asked to open \"%s\": not opened (only web links and documents are, no programs)\n", shown);
		return 0;
	}
	Fs_LocalPath(o, path, local, sizeof local); // a file only a game's archive holds is extracted for the program that shows it
	return Host_OpenDocument(o, local);
}

// Whether audio output is available (the start-up refuses to run without it, except in launcher mode).
int DSound8_IsAvailable(OpenBGI_t* o)
{
	return Host_AudioAvailable(o);
}
