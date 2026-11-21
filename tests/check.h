/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/check.h - what every unit test counts its failures with
 *
 * A test is one program: it runs its checks, says which failed, and ends
 * with status 1 when any did (gFails).
 */
#ifndef BGI_TESTS_CHECK_H_
#define BGI_TESTS_CHECK_H_

#include <stdio.h>

static int gFails; // the number of failed checks so far

// evaluate a condition; print it with its location and count a failure when it is false
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

#endif // BGI_TESTS_CHECK_H_
