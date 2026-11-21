/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * process.c - the POSIX back end's process services (src/host/os.h):
 *             start-up and shutdown, the executable's directory, the
 *             current directory, the instance mutex and the named mutex
 *             test, and the exit path of a termination signal
 *
 * The game directory is the current directory at start-up (or what the
 * launcher chose through OS_SetGameDir) and stands for the drive "C:";
 * the configuration directory holds the registry file and the lock files
 * that stand in for the named mutexes: ~/.config/bgi, on Haiku "bgi" in
 * the user's settings directory (~/config/settings), on macOS in
 * ~/Library/Application Support, on Android (the command line, for now)
 * in $TMPDIR.  The binary's own directory (OS_BinaryDir, where
 * openbgi.json lives) is found through /proc/self/exe (on Haiku the team's
 * application image, on macOS the executable path dyld knows) or the
 * program name.
 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700 // realpath, which POSIX has only with XSI
#endif
#include "posix_internal.h"

#ifdef BGI_HAIKU
#include <FindDirectory.h>
#include <image.h>
#endif
#ifdef BGI_MACOS
#include <mach-o/dyld.h>
#endif

char gPosixExeDir[PATH_MAX]; // the game directory with a trailing '/': the one drive this layer presents
#ifdef BGI_LIBRETRO
char gPosixCurDir[PATH_MAX]; // the game's current directory: a libretro core leaves the process's to its frontend
#endif
char gPosixConfigDir[PATH_MAX]; // ~/.config/bgi (Haiku, macOS, Android: see above)
char gPosixRegFile[PATH_MAX];   // the registry emulation's file (registry.txt in the configuration directory)
static char gArgv0[PATH_MAX];   // the program name main() was called with, for OS_BinaryDir when /proc has no answer

void OS_SetArgv0(const char* argv0)
{
	snprintf(gArgv0, sizeof gArgv0, "%s", argv0 ? argv0 : "");
}

/* SIGTERM / SIGINT: the first one is the window's close button (so an
 * unattended run stopped by `timeout` still ends through the engine's
 * own shutdown and writes BGI_OPSTAT), the second one ends the process */
static void OnSignal(int sig)
{
	static volatile sig_atomic_t count;
	BGI_UNUSED(sig);
	if(count++)
		_exit(1);
	OsPosix_RequestClose();
}

// the signal handlers, the directories, the character converters; always 1
int OS_Init(void)
{
	const char* home = getenv("HOME");
	signal(SIGTERM, OnSignal);
	signal(SIGINT, OnSignal);
	/* The original lives in the game's directory and makes that its base
	 * directory (GetModuleFileName); here the binary is usually somewhere
	 * else, so the game directory is the current directory - run the
	 * engine from inside the game folder.  "C:\" maps to it. */
	if(!getcwd(gPosixExeDir, sizeof gPosixExeDir - 2))
		strcpy(gPosixExeDir, ".");
	strcat(gPosixExeDir, "/");
#if defined(BGI_HAIKU)
	{
		char settings[PATH_MAX];
		if(find_directory(B_USER_SETTINGS_DIRECTORY, -1, true, settings, (int32)sizeof settings) == B_OK)
			snprintf(gPosixConfigDir, sizeof gPosixConfigDir, "%s/bgi", settings);
		else
			snprintf(gPosixConfigDir, sizeof gPosixConfigDir, "%s/config/settings/bgi", home ? home : ".");
	}
#elif defined(BGI_MACOS)
	snprintf(gPosixConfigDir, sizeof gPosixConfigDir, "%s/Library/Application Support/bgi", home ? home : ".");
#elif defined(BGI_HEADLESS)
	// the test host keeps no state between runs: the directory it runs in holds it
	BGI_UNUSED(home);
	snprintf(gPosixConfigDir, sizeof gPosixConfigDir, "%s.headless", gPosixExeDir);
#elif defined(BGI_ANDROID)
	{ // the shell's writable scratch directory, until the app hands over its own files directory
		const char* tmp = getenv("TMPDIR");
		BGI_UNUSED(home);
		snprintf(gPosixConfigDir, sizeof gPosixConfigDir, "%s/bgi", tmp && *tmp ? tmp : "/data/local/tmp");
	}
#else
	snprintf(gPosixConfigDir, sizeof gPosixConfigDir, "%s/.config/bgi", home ? home : ".");
#endif
	mkdir(gPosixConfigDir, 0755);
	snprintf(gPosixRegFile, sizeof gPosixRegFile, "%s/registry.txt", gPosixConfigDir);
	return 1;
}

// release the font libraries and the character converters
void OS_Shutdown(void)
{
	OsPosix_FontShutdown();
}

// always "C:\": see below
void OS_ExeDir(char* buf, size_t n)
{
	/* The engine wants a Windows path with a drive letter: the drive checks
	 * (DriveReady, the CD prompts) key on it.  The game directory is the root
	 * of the one drive this layer presents, so the executable lives in "C:\"
	 * and OsPosix_Path maps that prefix back to gPosixExeDir. */
	snprintf(buf, n, "C:\\");
}

/* the path of the running binary into `path` (PATH_MAX bytes, not
 * terminated); its length, <= 0 when the system does not tell */
static ssize_t SelfPath(char* path)
{
#if defined(BGI_MACOS)
	char raw[PATH_MAX];
	uint32_t size = sizeof raw;
	if(_NSGetExecutablePath(raw, &size) != 0 || !realpath(raw, path))
		return 0;
	return (ssize_t)strlen(path);
#elif defined(BGI_HAIKU)
	image_info info;
	int32 cookie = 0;
	while(get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK)
		if(info.type == B_APP_IMAGE)
		{
			size_t len = strlen(info.name);
			if(len >= PATH_MAX)
				return 0;
			memcpy(path, info.name, len);
			return (ssize_t)len;
		}
	return 0;
#else
	return readlink("/proc/self/exe", path, PATH_MAX - 1);
#endif
}

/* the directory of the binary: from /proc/self/exe (Linux, Android), the
 * application image (Haiku) or dyld (macOS), else from the program name
 * when it has a directory part, else the current directory */
void OS_BinaryDir(char* buf, size_t n)
{
	char path[PATH_MAX];
	ssize_t len = SelfPath(path);
	char* slash;
	if(len <= 0)
	{
		if(gArgv0[0] && strchr(gArgv0, '/') && realpath(gArgv0, path))
			len = (ssize_t)strlen(path);
		else
			len = 0;
	}
	path[len] = 0;
	slash = strrchr(path, '/');
	if(len == 0 || !slash)
	{
		if(!getcwd(path, sizeof path - 2))
			strcpy(path, ".");
		strcat(path, "/");
	}
	else
		slash[1] = 0;
	snprintf(buf, n, "%s", path);
}

void OS_GameDir(char* buf, size_t n)
{
	snprintf(buf, n, "%s", gPosixExeDir);
}

// a directory and its parents made where missing
static void MakeDirs(const char* path)
{
	char buf[PATH_MAX];
	char* p;
	snprintf(buf, sizeof buf, "%s", path);
	for(p = buf + 1; *p; p++)
		if(*p == '/')
		{
			*p = 0;
			mkdir(buf, 0755);
			*p = '/';
		}
	mkdir(buf, 0755);
}

void OS_UserDataDir(char* buf, size_t n)
{
	const char* home = getenv("HOME");
	char path[PATH_MAX];
	struct stat st;
	if(!home || !*home)
		home = ".";
#if defined(BGI_HAIKU)
	{
		char settings[PATH_MAX];
		if(find_directory(B_USER_SETTINGS_DIRECTORY, -1, true, settings, (int32)sizeof settings) == B_OK)
			snprintf(path, sizeof path, "%s/OpenBGI", settings);
		else
			snprintf(path, sizeof path, "%s/config/settings/OpenBGI", home);
	}
#elif defined(BGI_MACOS)
	snprintf(path, sizeof path, "%s/Library/Application Support/OpenBGI", home);
#elif defined(BGI_ANDROID)
	{
		const char* tmp = getenv("TMPDIR");
		snprintf(path, sizeof path, "%s/openbgi", tmp && *tmp ? tmp : "/data/local/tmp");
	}
#else
	{
		const char* xdg = getenv("XDG_DATA_HOME");
		if(xdg && xdg[0] == '/') // a relative one is to be ignored
			snprintf(path, sizeof path, "%s/openbgi", xdg);
		else
			snprintf(path, sizeof path, "%s/.local/share/openbgi", home);
	}
#endif
	MakeDirs(path);
	if(stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
		path[0] = 0;
	snprintf(buf, n, "%s%s", path, path[0] ? "/" : "");
}

// the game directory moves: "C:\" maps to `dir` from now on, which also becomes the current directory
int OS_SetGameDir(const char* dir)
{
	struct stat st;
	size_t len;
	if(!dir || !*dir || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
		return 0;
#ifdef BGI_LIBRETRO
	{ // the process's current directory is the frontend's: the game's is kept here (gPosixCurDir)
		char abs[PATH_MAX];
		snprintf(gPosixExeDir, sizeof gPosixExeDir - 2, "%s", realpath(dir, abs) ? abs : dir);
		snprintf(gPosixCurDir, sizeof gPosixCurDir, "%s", gPosixExeDir);
	}
#else
	if(chdir(dir) != 0)
		return 0;
	if(!getcwd(gPosixExeDir, sizeof gPosixExeDir - 2))
		snprintf(gPosixExeDir, sizeof gPosixExeDir - 2, "%s", dir);
#endif
	len = strlen(gPosixExeDir);
	if(len == 0 || gPosixExeDir[len - 1] != '/')
		strcat(gPosixExeDir, "/");
	return 1;
}

// native paths are UTF-8 here already
void OS_NativeToUtf8(const char* native, char* out, size_t n)
{
	snprintf(out, n, "%s", native);
}

void OS_Utf8ToNative(const char* utf8, char* out, size_t n)
{
	snprintf(out, n, "%s", utf8);
}

FILE* OS_NativeOpen(const char* path, const char* mode) // a native path is a POSIX path
{
	return fopen(path, mode);
}

uint8_t* OS_ReadNativeFile(const char* path, uint32_t* size)
{
	FILE* f = fopen(path, "rb");
	long len;
	uint8_t* buf;
	*size = 0;
	if(!f)
		return NULL;
	if(fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
	{
		fclose(f);
		return NULL;
	}
	buf = (uint8_t*)malloc((size_t)len + 1);
	if(!buf || fread(buf, 1, (size_t)len, f) != (size_t)len)
	{
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	buf[len] = 0;
	*size = (uint32_t)len;
	return buf;
}

int OS_WriteNativeFile(const char* path, const void* data, uint32_t size)
{
	FILE* f = fopen(path, "wb");
	int ok;
	if(!f)
		return 0;
	ok = fwrite(data, 1, size, f) == size;
	ok = fclose(f) == 0 && ok;
	return ok;
}

// chdir to the converted path
int OS_SetCurrentDir(const char* dir)
{
	char p[PATH_MAX];
	OsPosix_Path(dir, p, sizeof p);
#ifdef BGI_LIBRETRO
	{ // (the game's current directory, not the process's)
		struct stat st;
		char abs[PATH_MAX];
		if(stat(p, &st) != 0 || !S_ISDIR(st.st_mode) || !realpath(p, abs))
			return 0;
		snprintf(gPosixCurDir, sizeof gPosixCurDir, "%s", abs);
		return 1;
	}
#else
	return chdir(p) == 0;
#endif
}

// the instance mutex: a lock file held with flock
struct OsMutex
{
	int fd;              // the open lock file, -1 when the lock was not taken
	char path[PATH_MAX]; // its path
};

/* a lock file under the config directory stands in for the named mutex:
 * "<config dir>/<name with everything but letters and digits as '_'>.lock" */
static void MutexPath(const char* name, char* out, size_t n)
{
	size_t i, o;
	o = (size_t)snprintf(out, n, "%s/", gPosixConfigDir);
	for(i = 0; name[i] && o + 6 < n; i++)
		out[o++] = (name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') ||
				(name[i] >= '0' && name[i] <= '9')
			? name[i]
			: '_';
	strcpy(out + o, ".lock");
}

/* create the lock file and take its exclusive lock without waiting;
 * *alreadyExists when another process holds it (the lock is released
 * with the process, so a crashed instance leaves nothing behind) */
OsMutex_t* OS_InstanceMutexCreate(const char* name, int* alreadyExists)
{
	OsMutex_t* m = (OsMutex_t*)calloc(1, sizeof *m);
	MutexPath(name, m->path, sizeof m->path);
	m->fd = open(m->path, O_RDWR | O_CREAT, 0644);
	*alreadyExists = 0;
	if(m->fd >= 0 && flock(m->fd, LOCK_EX | LOCK_NB) != 0)
	{
		*alreadyExists = errno == EWOULDBLOCK;
		close(m->fd);
		m->fd = -1;
	}
	return m;
}

// unlock and close the file (it stays on disk), free the handle
void OS_InstanceMutexRelease(OsMutex_t* m)
{
	if(!m)
		return;
	if(m->fd >= 0)
	{
		flock(m->fd, LOCK_UN);
		close(m->fd);
	}
	free(m);
}
