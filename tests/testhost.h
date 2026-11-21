/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/testhost.h - the host services the tests use beyond the engine's
 *                    OS layer, the same on POSIX and on Windows (where the
 *                    tests of a MinGW build run, under wine on Linux): a
 *                    scratch directory in the system's temporary
 *                    directory, the current directory, an environment
 *                    variable, removing a directory tree and a short pause
 *
 * The functions are static: each test includes what it uses.  Paths are
 * native ('/' works on Windows as well) and in the encoding the OS layer's
 * OS_Native* functions take.
 */
#ifndef BGI_TESTHOST_H_
#define BGI_TESTHOST_H_

#include "host/os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>  // _mkdir, _chdir, _rmdir
#include <process.h> // _getpid
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// the system's temporary directory: $TEMP / $TMP on Windows, else /tmp
static inline const char* TestHost_TempRoot(void)
{
#ifdef _WIN32
	const char* t = getenv("TEMP");
	if(!t || !*t)
		t = getenv("TMP");
	return t && *t ? t : ".";
#else
	return "/tmp";
#endif
}

/* "<temporary directory>/<name>_<process id>_<time>" into out: a scratch
 * directory of this run (the time, because wine hands out the same few
 * process ids again and a failed run keeps its directory) */
static inline void TestHost_ScratchName(char* out, size_t n, const char* name)
{
#ifdef _WIN32
	long pid = (long)_getpid();
#else
	long pid = (long)getpid();
#endif
	snprintf(out, n, "%s/%s_%ld_%ld", TestHost_TempRoot(), name, pid, (long)time(NULL));
}

// make a directory; 0 on success, as mkdir
static inline int TestHost_Mkdir(const char* path)
{
#ifdef _WIN32
	return _mkdir(path);
#else
	return mkdir(path, 0755);
#endif
}

// change the current directory; 0 on success, as chdir
static inline int TestHost_Chdir(const char* path)
{
#ifdef _WIN32
	return _chdir(path);
#else
	return chdir(path);
#endif
}

// set an environment variable for this process (and what it starts)
static inline void TestHost_SetEnv(const char* name, const char* value)
{
#ifdef _WIN32
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

// remove a directory with everything in it (the OS layer lists it; OS_Init first)
static inline void TestHost_RemoveTreeEntry(void* ctx, const char* name, int isDir);
static inline void TestHost_RemoveTree(const char* path)
{
	OS_NativeList(path, TestHost_RemoveTreeEntry, (void*)path);
#ifdef _WIN32
	_rmdir(path);
#else
	remove(path);
#endif
}

static inline void TestHost_RemoveTreeEntry(void* ctx, const char* name, int isDir)
{
	char path[0x800];
	snprintf(path, sizeof path, "%s/%s", (const char*)ctx, name);
	if(isDir)
		TestHost_RemoveTree(path);
	else
		remove(path);
}

#endif // BGI_TESTHOST_H_
