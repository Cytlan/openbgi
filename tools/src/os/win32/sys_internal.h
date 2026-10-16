/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sys_internal.h - the common includes of the system files of the Win32
 *                  back end (process.c, files.c, time.c, thread.c,
 *                  machine.c, registry.c): windows.h with the multimedia,
 *                  shell, device-I/O and COM headers they need
 *
 * Nothing is declared here; the files only share these includes and the
 * BGI_X86 switch.  The window files include win32_internal.h instead and
 * the dialog files ui_internal.h.
 */
#ifndef BGI_OS_WIN32_SYS_INTERNAL_H
#define BGI_OS_WIN32_SYS_INTERNAL_H

#include "bgi/os_win32.h" // windows.h and the version macros come from here
#include <mmsystem.h>
#include <shlobj.h>
#include <shellapi.h>
#include <winioctl.h>
#include <objbase.h>

#include "bgi/os.h"
#include "bgi/os_common.h"

// BGI_X86: the compiler offers the CPUID and RDTSC intrinsics machine.c uses; without it OS_Cpuid and OS_ReadTsc report "unavailable"
#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
#include <cpuid.h>
#include <x86intrin.h>
#define BGI_X86 1
#endif

#endif
