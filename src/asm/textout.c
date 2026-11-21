/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * textout.c - the disassembler's text to a stream or into memory
 *             (AsmOut_t of asmout.h)
 */
#include "bgi/asmout.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

// room for n more bytes and the NUL; 0 (and `failed`) when there is none
static int Reserve(AsmOut_t* w, size_t n)
{
	size_t cap;
	char* p;
	if(w->failed)
		return 0;
	if(w->len + n + 1 <= w->cap)
		return 1;
	cap = w->cap ? w->cap : 0x1000;
	while(cap < w->len + n + 1)
		cap *= 2;
	p = (char*)realloc(w->text, cap);
	if(!p)
	{
		w->failed = 1;
		return 0;
	}
	w->text = p;
	w->cap = cap;
	return 1;
}

void AsmOut_Write(AsmOut_t* w, const char* s, size_t n)
{
	if(w->fp)
	{
		fwrite(s, 1, n, w->fp);
		return;
	}
	if(!Reserve(w, n))
		return;
	memcpy(w->text + w->len, s, n);
	w->len += n;
	w->text[w->len] = 0;
}

void AsmOut_Puts(AsmOut_t* w, const char* s)
{
	AsmOut_Write(w, s, strlen(s));
}

void AsmOut_Putc(AsmOut_t* w, int c)
{
	char ch = (char)c;
	AsmOut_Write(w, &ch, 1);
}

void AsmOut_VPrintf(AsmOut_t* w, const char* fmt, va_list ap)
{
	char buf[0x200];
	va_list again;
	int n;
	if(w->fp)
	{
		vfprintf(w->fp, fmt, ap);
		return;
	}
	va_copy(again, ap);
	n = vsnprintf(buf, sizeof buf, fmt, ap);
	if(n >= 0 && (size_t)n < sizeof buf)
		AsmOut_Write(w, buf, (size_t)n);
	else if(n >= 0 && Reserve(w, (size_t)n))
	{ // longer than the buffer: formatted again, straight into the text
		vsnprintf(w->text + w->len, (size_t)n + 1, fmt, again);
		w->len += (size_t)n;
	}
	va_end(again);
}

void AsmOut_Printf(AsmOut_t* w, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	AsmOut_VPrintf(w, fmt, ap);
	va_end(ap);
}
