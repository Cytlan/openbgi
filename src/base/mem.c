/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * mem.c - the allocation helpers (src/include/bgi/common.h)
 */
#include "bgi/common.h"

// malloc that never returns NULL: the process is aborted when memory runs out (n = 0 gives a 1-byte block)
void* BGI_Alloc(size_t n)
{
	void* p = malloc(n ? n : 1);
	if(!p)
	{
		fprintf(stderr, "openbgi: out of memory (%lu bytes)\n", (unsigned long)n);
		abort();
	}
	return p;
}

// BGI_Alloc, zero filled (calloc: a large block can come zeroed from the system without being written)
void* BGI_Calloc(size_t n)
{
	void* p = calloc(n ? n : 1, 1);
	if(!p)
	{
		fprintf(stderr, "openbgi: out of memory (%lu bytes)\n", (unsigned long)n);
		abort();
	}
	return p;
}

void BGI_Free(void* p)
{
	free(p);
}

char* BGI_Strdup(const char* s)
{
	size_t n = strlen(s) + 1;
	char* d = (char*)BGI_Alloc(n);
	memcpy(d, s, n);
	return d;
}
