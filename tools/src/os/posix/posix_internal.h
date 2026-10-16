/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * posix_internal.h - the common includes and the shared state of the
 *                    system files of the POSIX back end (sjis.c, paths.c,
 *                    process.c, files.c, time.c, thread.c, machine.c,
 *                    registry.c); the window files have x11_internal.h
 *
 * The three directories below are set once by OS_Init (process.c) and
 * read by everything else: the game directory is the root of the one
 * drive "C:" this back end presents, the configuration directory holds
 * the registry file and the instance lock files.
 */
#ifndef BGI_OS_POSIX_INTERNAL_H
#define BGI_OS_POSIX_INTERNAL_H

#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/os_posix.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <iconv.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <limits.h>
#include <strings.h>

// the CPUID / RDTSC intrinsics of machine.c exist on x86 only
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#include <x86intrin.h>
#define BGI_X86 1
#endif

// set by OS_Init (process.c)
extern char gPosixExeDir[PATH_MAX];    // the game directory with a trailing '/': the one drive this layer presents
extern char gPosixConfigDir[PATH_MAX]; // ~/.config/bgi
extern char gPosixRegFile[PATH_MAX];   // the registry emulation's file (registry.c)

void OsPosix_SjisInit(void);     // open the CP932 converters (sjis.c)
void OsPosix_SjisShutdown(void); // close them

#endif
