/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dyld.h - a stand-in for <mach-o/dyld.h> of macOS (the executable's
 * path, src/host/posix/process.c) for `make check-macos`; never used by the
 * real build.
 */
#ifndef MACSTUB_DYLD_H_
#define MACSTUB_DYLD_H_

#include <stdint.h>

int _NSGetExecutablePath(char* buf, uint32_t* bufsize);

#endif // MACSTUB_DYLD_H_
