/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sysctl.h - a stand-in for the part of <sys/sysctl.h> of macOS the POSIX
 * back end uses (sysctlbyname and the swap usage, src/host/posix/machine.c)
 * for `make check-macos`; never used by the real build.
 */
#ifndef MACSTUB_SYSCTL_H_
#define MACSTUB_SYSCTL_H_

#include <stddef.h>
#include <stdint.h>

struct xsw_usage
{
	uint64_t xsu_total;
	uint64_t xsu_avail;
	uint64_t xsu_used;
	uint32_t xsu_pagesize;
	int xsu_encrypted;
};

int sysctlbyname(const char* name, void* oldp, size_t* oldlenp, void* newp, size_t newlen);

#endif // MACSTUB_SYSCTL_H_
