/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * posix_internal.h - the common includes and the shared state of the
 *                    system files of the POSIX back end (paths.c,
 *                    process.c, files.c, time.c, thread.c, machine.c,
 *                    registry.c); the window files have x11_internal.h
 *
 * The three directories below are set once by OS_Init (process.c) and
 * read by everything else: the game directory is the root of the one
 * drive "C:" this back end presents, the configuration directory holds
 * the registry file and the instance lock files.
 *
 * The same files build for Haiku (BGI_HAIKU, PLATFORM=haiku in the
 * Makefile): where Linux and Haiku differ - the memory figures, the
 * binary's path, the settings and font directories, the sound output, the
 * document opener - the Haiku side is selected here and in the files
 * concerned, and the services only the native kits offer are in
 * src/host/haiku/ (src/host/os_haiku.h).
 */
#ifndef BGI_OS_POSIX_INTERNAL_H
#define BGI_OS_POSIX_INTERNAL_H

#include "host/os.h"
#include "host/os_common.h"
#include "host/os_posix.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
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
extern char gPosixExeDir[PATH_MAX]; // the game directory with a trailing '/': the one drive this layer presents
#ifdef BGI_LIBRETRO
extern char gPosixCurDir[PATH_MAX]; // the game's current directory: a libretro core leaves the process's to its frontend
#endif
extern char gPosixConfigDir[PATH_MAX]; // ~/.config/bgi (Haiku: ~/config/settings/bgi)
extern char gPosixRegFile[PATH_MAX];   // the registry emulation's file (registry.c)

#endif
