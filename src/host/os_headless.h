/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_headless.h - what the test host (src/host/headless/main.c) needs of
 *                 its back end beyond src/host/os.h: the clock its machine
 *                 runs on (src/host/headless/headless.c).  Its input it
 *                 hands the engine itself (OpenBGI_Key and its kin).
 *                 Nothing above the OS layer includes this.
 */
#ifndef BGI_OS_HEADLESS_H_
#define BGI_OS_HEADLESS_H_

#include "bgi/common.h"

/* The machine's clock in milliseconds, as the test host advances it: the
 * time stamp counter (OS_ReadTsc) counts it at 2 GHz. */
void OsHeadless_SetNow(uint32_t ms);
uint32_t OsHeadless_Now(void);

#endif // BGI_OS_HEADLESS_H_
