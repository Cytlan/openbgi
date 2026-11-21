/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * stat.h - the host's <sys/stat.h> with the names of macOS for a file's
 * times, for `make check-macos`; never used by the real build.
 *
 * macOS calls the times of struct stat st_atimespec, st_mtimespec and
 * st_ctimespec, and has no st_atim, st_mtim and st_ctim.  Here the first
 * three stand for the host's fields, and the last three are refused, so
 * that the check stops at a name a Mac does not know.  (st_mtime and its
 * like are on both.)
 */
#ifndef MACSTUB_STAT_H_
#define MACSTUB_STAT_H_

#pragma GCC system_header
#include_next <sys/stat.h>

#define st_atimespec st_atim
#define st_mtimespec st_mtim
#define st_ctimespec st_ctim
#pragma GCC poison st_atim st_mtim st_ctim

#endif // MACSTUB_STAT_H_
