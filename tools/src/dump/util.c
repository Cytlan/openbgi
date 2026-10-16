/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * util.c - the helpers of the dump and pack tools: the text buffer their
 *          JSON is written with, native paths and whole files, hex
 *          strings and file names (declared in dump_internal.h)
 */
#include "dump_internal.h"

const char* const kDumpKindNames[DUMP_KIND_COUNT] = {"data", "program", "scenario", "image", "sound", "movie", "table", "sdc"};

// ---- text ----------------------------------------------------------------------------------------

void DumpText_AddN(DumpText_t* t, const char* s, size_t n)
{
	if(t->len + n + 1 > t->cap)
	{
		size_t cap = t->cap ? t->cap * 2 : 256;
		char* p;
		while(cap < t->len + n + 1)
			cap *= 2;
		p = (char*)BGI_Alloc(cap);
		if(t->len)
			memcpy(p, t->p, t->len);
		BGI_Free(t->p);
		t->p = p;
		t->cap = cap;
	}
	memcpy(t->p + t->len, s, n);
	t->len += n;
	t->p[t->len] = 0;
}

void DumpText_Add(DumpText_t* t, const char* s)
{
	DumpText_AddN(t, s, strlen(s));
}

void DumpText_Printf(DumpText_t* t, const char* fmt, ...)
{
	char small[512];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(small, sizeof small, fmt, ap);
	va_end(ap);
	if(n < 0)
		return;
	if((size_t)n < sizeof small)
		DumpText_AddN(t, small, (size_t)n);
	else
	{ // longer than the stack buffer: format again into one of the right size
		char* big = (char*)BGI_Alloc((size_t)n + 1);
		va_start(ap, fmt);
		vsnprintf(big, (size_t)n + 1, fmt, ap);
		va_end(ap);
		DumpText_AddN(t, big, (size_t)n);
		BGI_Free(big);
	}
}

// a JSON string: quotes, backslashes and control characters escaped, everything else (UTF-8) as it is
void DumpText_AddString(DumpText_t* t, const char* utf8)
{
	const char* s;
	DumpText_AddN(t, "\"", 1);
	for(s = utf8; *s; s++)
	{
		uint8_t ch = (uint8_t)*s;
		if(ch == '"' || ch == '\\')
		{
			char esc[2];
			esc[0] = '\\';
			esc[1] = (char)ch;
			DumpText_AddN(t, esc, 2);
		}
		else if(ch == '\n')
			DumpText_AddN(t, "\\n", 2);
		else if(ch == '\r')
			DumpText_AddN(t, "\\r", 2);
		else if(ch == '\t')
			DumpText_AddN(t, "\\t", 2);
		else if(ch < 0x20 || ch == 0x7f)
			DumpText_Printf(t, "\\u%04x", ch);
		else
			DumpText_AddN(t, s, 1);
	}
	DumpText_AddN(t, "\"", 1);
}

void DumpText_Free(DumpText_t* t)
{
	BGI_Free(t->p);
	memset(t, 0, sizeof *t);
}

// ---- files ---------------------------------------------------------------------------------------

void DumpFs_Join(char* out, size_t n, const char* dir, const char* rel)
{
	char part[DUMP_PATH_MAX], native[DUMP_PATH_MAX];
	size_t i, len = strlen(dir);
	if(!rel[0])
	{
		snprintf(out, n, "%s", dir);
		return;
	}
	// the separators are exchanged while the name is still UTF-8, where a byte of '/' is always the character
	snprintf(part, sizeof part, "%s", rel);
	for(i = 0; part[i]; i++)
		if(part[i] == '/')
			part[i] = OS_NATIVE_SEP;
	OS_Utf8ToNative(part, native, sizeof native);
	if(len == 0)
		snprintf(out, n, "%s", native);
	else if(OsCommon_FilePart(dir)[0] == 0) // the directory ends in a separator already
		snprintf(out, n, "%s%s", dir, native);
	else
		snprintf(out, n, "%s%c%s", dir, OS_NATIVE_SEP, native);
}

uint8_t* DumpFs_Read(const char* path, uint32_t* size)
{
	OsNativeStat_t st;
	*size = 0;
	if(OS_NativeStat(path, &st) != OS_NATIVE_FILE || st.size >= 0xffffffffu)
		return NULL;
	return OS_ReadNativeFile(path, size);
}

int DumpFs_Write(const char* path, const void* data, size_t size)
{
	FILE* f = OS_NativeOpen(path, "wb");
	int ok;
	if(!f)
		return 0;
	ok = size == 0 || fwrite(data, 1, size, f) == size;
	return fclose(f) == 0 && ok;
}

uint32_t DumpFs_Crc(const char* path, int* ok)
{
	FILE* f = OS_NativeOpen(path, "rb");
	uint8_t* buf;
	uint32_t crc = 0;
	size_t got;
	*ok = 0;
	if(!f)
		return 0;
	buf = (uint8_t*)BGI_Alloc(0x10000);
	while((got = fread(buf, 1, 0x10000, f)) > 0)
		crc = BGI_Crc32(crc, buf, got);
	*ok = !ferror(f);
	fclose(f);
	BGI_Free(buf);
	return crc;
}

// ---- strings -------------------------------------------------------------------------------------

void Dump_Hex(char* out, const uint8_t* data, size_t n)
{
	static const char digits[] = "0123456789abcdef";
	size_t i;
	for(i = 0; i < n; i++)
	{
		out[i * 2] = digits[data[i] >> 4];
		out[i * 2 + 1] = digits[data[i] & 15];
	}
	out[n * 2] = 0;
}

static int HexDigit(char ch)
{
	if(ch >= '0' && ch <= '9')
		return ch - '0';
	if(ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if(ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

int Dump_Unhex(const char* hex, uint8_t* out, size_t cap)
{
	size_t n = strlen(hex), i;
	if(n % 2 || n / 2 > cap)
		return -1;
	for(i = 0; i < n / 2; i++)
	{
		int hi = HexDigit(hex[i * 2]), lo = HexDigit(hex[i * 2 + 1]);
		if(hi < 0 || lo < 0)
			return -1;
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return (int)(n / 2);
}

static int Lower(int ch)
{
	return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
}

int Dump_EqualNoCase(const char* a, const char* b)
{
	for(; *a && *b; a++, b++)
		if(Lower((uint8_t)*a) != Lower((uint8_t)*b))
			return 0;
	return *a == *b;
}

int Dump_EndsWithNoCase(const char* s, const char* suffix)
{
	size_t n = strlen(s), m = strlen(suffix);
	return n >= m && Dump_EqualNoCase(s + n - m, suffix);
}

void Dump_SafeName(const char* utf8, char* out, size_t n)
{
	static const char* const reserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
		"lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
	char stem[16];
	size_t o = 0, i;
	for(i = 0; utf8[i] && o + 2 < n; i++)
	{
		uint8_t ch = (uint8_t)utf8[i];
		out[o++] = (char)((ch < 0x20 || ch == 0x7f || strchr("<>:\"/\\|?*", ch)) ? '_' : ch);
	}
	for(i = o; i > 0 && (out[i - 1] == '.' || out[i - 1] == ' '); i--)
		out[i - 1] = '_'; // a name may not end in a dot or a blank
	if(o == 0)
		out[o++] = '_';
	out[o] = 0;
	// the name up to its first dot is what Windows takes for a device
	for(i = 0; i < sizeof stem - 1 && out[i] && out[i] != '.'; i++)
		stem[i] = out[i];
	stem[i] = 0;
	for(i = 0; i < BGI_COUNTOF(reserved); i++)
		if(Dump_EqualNoCase(stem, reserved[i]) && o + 2 < n)
		{
			memmove(out + 1, out, o + 1);
			out[0] = '_';
			break;
		}
}

int Dump_IsInside(const char* rel)
{
	const char* p = rel;
	if(!rel[0] || rel[0] == '/' || strchr(rel, '\\') || strchr(rel, ':'))
		return 0;
	while(*p)
	{ // component by component
		const char* end = strchr(p, '/');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		if(len == 0 || (len == 2 && p[0] == '.' && p[1] == '.'))
			return 0;
		p += len + (end ? 1 : 0);
	}
	return 1;
}

int Dump_Wanted(const char* name, const char* const* names, int count)
{
	int i;
	if(!names || count == 0)
		return 1;
	for(i = 0; i < count; i++)
		if(Dump_EqualNoCase(name, names[i]))
			return 1;
	return 0;
}
