/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * scnfile.c - the scenario container: parsing and writing the
 *             BurikoCompiledScriptVer1.00 layout and the headerless one
 *             of the 1.66 / 1.69 games (interface in scn.h)
 *
 * The file is: the 28-byte magic; at 0x1c the size of the rest of the
 * header; the import count and the NUL-terminated import names; the
 * export count and (name, offset) pairs; 1 to 16 bytes of zero padding
 * up to a multiple of 16; the token stream; the string table.  Offsets in tokens
 * (strings, code addresses) count from the start of the token stream, so
 * the string table begins at codeLen * 4.  The 1.66 / 1.69 games (Itsusora,
 * Tayutama, TayutamaHD) have no header: the token stream starts at offset
 * 0 and the strings follow it, so the offsets are file offsets; such a
 * file has no imports or exports (the scenes call the functions of their
 * `main` by address).  The 16-bit scenes of the 1.64 games are another
 * format altogether (scn16.c); Scn_Load recognizes one and keeps it as
 * its image, so that the tools handle all three alike.
 */
#include "scn_internal.h"

static uint32_t Rd32(const uint8_t* p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void Wr32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

int Scn_OperandCount(uint32_t token)
{
	switch(token)
	{
		case SCN_PUSH:
		case SCN_ADDR:
		case SCN_LOCAL:
		case SCN_STR:
		case SCN_LOAD:
		case SCN_STORE:
		case SCN_STORE2:
		case SCN_JCC:
		case SCN_ARGS: return 1;
		case SCN_LINE: return 2;
		case SCN_SRCINFO: return 3;
		default: return 0;
	}
}

static int Fail(char* err, size_t errSize, const char* msg)
{
	if(err && errSize)
		snprintf(err, errSize, "%s", msg);
	return 0;
}

// read a NUL-terminated name of at most SCN_NAME_MAX - 1 bytes at *p (before `end`) into `out`
static int ReadName(const uint8_t** p, const uint8_t* end, char* out)
{
	const uint8_t* s = *p;
	size_t n = 0;
	while(s + n < end && s[n])
		n++;
	if(s + n >= end || n >= SCN_NAME_MAX)
		return 0;
	memcpy(out, s, n + 1);
	*p = s + n + 1;
	return 1;
}

int Scn_Load(const uint8_t* data, uint32_t size, ScnFile_t* out, char* err, size_t errSize)
{
	const uint8_t* p;
	const uint8_t* end = data + size;
	uint32_t hdr, n, i, base, strStart, q;
	memset(out, 0, sizeof *out);
	if(size >= 4 && (size < 0x24 || memcmp(data, SCN_MAGIC, 28) != 0))
	{ /* no magic: the headerless layout, which starts with the frame prologue
	   * of its first function (`push_fp; push N; add; set_fp`; the whole of it,
	   * since the 16-bit scenes of the 1.64 games may start with the bytes of
	   * a push_fp token too) */
		if(size < 20 || Rd32(data) != SCN_PUSHFP || Rd32(data + 4) != SCN_PUSH || Rd32(data + 12) != SCN_ADD ||
			Rd32(data + 16) != SCN_SETFP)
		{
			if(!Scn16_Check(data, size))
				return Fail(err, errSize, "not a BurikoCompiledScriptVer1.00 file");
			// a 16-bit scene of the 1.64 games: kept whole, scn16.c reads it
			out->image = (uint8_t*)malloc(size + 1);
			memcpy(out->image, data, size);
			out->image[size] = 0;
			out->imageSize = size;
			return 1;
		}
		out->raw = 1;
		out->exports = (ScnExport_t*)calloc(1, sizeof *out->exports);
		base = 0;
	}
	else if(size < 4)
		return Fail(err, errSize, "not a BurikoCompiledScriptVer1.00 file");
	else
	{
		hdr = Rd32(data + 0x1c);
		base = 0x1c + hdr;
		if(hdr < 8 || base > size || (base & 3))
			return Fail(err, errSize, "bad header size");
		p = data + 0x20;
		n = Rd32(p);
		p += 4;
		if(n > SCN_IMPORTS)
			return Fail(err, errSize, "too many imports");
		for(i = 0; i < n; i++)
			if(!ReadName(&p, data + base, out->imports[i]))
				return Fail(err, errSize, "bad import list");
		out->importCount = (int)n;
		if(p + 4 > data + base)
			return Fail(err, errSize, "bad export count");
		n = Rd32(p);
		p += 4;
		if(n > 0x10000)
			return Fail(err, errSize, "too many exports");
		out->exports = (ScnExport_t*)calloc(n + 1, sizeof *out->exports);
		for(i = 0; i < n; i++)
		{
			if(!ReadName(&p, data + base, out->exports[i].name) || p + 4 > data + base)
			{
				Scn_Free(out);
				return Fail(err, errSize, "bad export list");
			}
			out->exports[i].off = Rd32(p);
			p += 4;
		}
		out->exportCount = (int)n;
		for(; p < data + base; p++)
			if(*p)
			{
				Scn_Free(out);
				return Fail(err, errSize, "non-zero header padding");
			}
	}
	out->base = base;
	/* the token stream: walk it, noting the lowest string offset any token
	 * refers to; the stream ends there (the strings follow the code) */
	strStart = size - base;
	q = base;
	while(q + 4 <= size && q - base < strStart)
	{
		uint32_t t = Rd32(data + q);
		int k = Scn_OperandCount(t);
		uint32_t ref = 0;
		int hasRef = 0;
		if(q + 4 + 4 * (uint32_t)k > size)
		{
			Scn_Free(out);
			return Fail(err, errSize, "a token runs past the end of the file");
		}
		if(t == SCN_STR || t == SCN_LINE)
		{
			ref = Rd32(data + q + 4);
			hasRef = 1;
		}
		else if(t == SCN_SRCINFO)
		{
			ref = Rd32(data + q + 12);
			hasRef = 1;
		}
		if(hasRef && ref < strStart)
			strStart = ref;
		q += 4 + 4 * (uint32_t)k;
	}
	if(q - base != strStart)
	{
		Scn_Free(out);
		return Fail(err, errSize, "the token stream does not end where the strings begin");
	}
	out->codeLen = strStart / 4;
	out->code = (uint32_t*)calloc(out->codeLen + 1, sizeof *out->code);
	for(i = 0; i < out->codeLen; i++)
		out->code[i] = Rd32(data + base + 4 * i);
	out->stringsLen = size - base - strStart;
	out->strings = (uint8_t*)calloc(out->stringsLen + 1, 1);
	memcpy(out->strings, data + base + strStart, out->stringsLen);
	if(out->stringsLen && out->strings[out->stringsLen - 1] != 0)
	{
		Scn_Free(out);
		return Fail(err, errSize, "the string table does not end with a NUL");
	}
	(void)end;
	return 1;
}

uint8_t* Scn_Save(const ScnFile_t* f, uint32_t* size)
{
	uint32_t hdr = 0x24, base, total, i;
	uint8_t* out;
	uint8_t* p;
	if(f->image)
	{
		out = (uint8_t*)malloc(f->imageSize + 1);
		memcpy(out, f->image, f->imageSize);
		out[f->imageSize] = 0;
		*size = f->imageSize;
		return out;
	}
	if(f->raw)
	{ // the stream and the strings, nothing else
		total = f->codeLen * 4 + f->stringsLen;
		out = (uint8_t*)calloc(total + 1, 1);
		for(i = 0; i < f->codeLen; i++)
			Wr32(out + 4 * i, f->code[i]);
		memcpy(out + f->codeLen * 4, f->strings, f->stringsLen);
		*size = total;
		return out;
	}
	for(i = 0; i < (uint32_t)f->importCount; i++)
		hdr += (uint32_t)strlen(f->imports[i]) + 1;
	hdr += 4;
	for(i = 0; i < (uint32_t)f->exportCount; i++)
		hdr += (uint32_t)strlen(f->exports[i].name) + 1 + 4;
	base = hdr + (16 - hdr % 16); // 1..16 bytes of padding: an aligned header still gets 16
	total = base + f->codeLen * 4 + f->stringsLen;
	out = (uint8_t*)calloc(total + 1, 1);
	memcpy(out, SCN_MAGIC, 28);
	Wr32(out + 0x1c, base - 0x1c);
	Wr32(out + 0x20, (uint32_t)f->importCount);
	p = out + 0x24;
	for(i = 0; i < (uint32_t)f->importCount; i++)
	{
		size_t n = strlen(f->imports[i]) + 1;
		memcpy(p, f->imports[i], n);
		p += n;
	}
	Wr32(p, (uint32_t)f->exportCount);
	p += 4;
	for(i = 0; i < (uint32_t)f->exportCount; i++)
	{
		size_t n = strlen(f->exports[i].name) + 1;
		memcpy(p, f->exports[i].name, n);
		p += n;
		Wr32(p, f->exports[i].off);
		p += 4;
	}
	for(i = 0; i < f->codeLen; i++)
		Wr32(out + base + 4 * i, f->code[i]);
	if(f->stringsLen)
		memcpy(out + base + f->codeLen * 4, f->strings, f->stringsLen);
	*size = total;
	return out;
}

void Scn_Free(ScnFile_t* f)
{
	free(f->exports);
	free(f->code);
	free(f->strings);
	free(f->image);
	memset(f, 0, sizeof *f);
}

const char* Scn_String(const ScnFile_t* f, uint32_t off)
{
	uint32_t s = f->codeLen * 4;
	if(off < s || off >= s + f->stringsLen)
		return NULL;
	return (const char*)f->strings + (off - s);
}

// the parameter count of the function at dword k, -1 when no prologue is there
static int ParamsAt(const ScnFile_t* f, uint32_t k)
{
	uint32_t n = 0;
	// the prologue: push_fp; push F; add; set_fp; (local n; store2 s)*
	if(k + 5 > f->codeLen || f->code[k] != SCN_PUSHFP || f->code[k + 1] != SCN_PUSH || f->code[k + 3] != SCN_ADD || f->code[k + 4] != SCN_SETFP)
		return -1;
	k += 5;
	while(k + 4 <= f->codeLen && f->code[k] == SCN_LOCAL && f->code[k + 2] == SCN_STORE2)
	{
		n++;
		k += 4;
	}
	return (int)n;
}

static void AddFunction(ScnFuncInfo_t** list, int* count, const char* name, int params)
{
	ScnFuncInfo_t* fi;
	*list = (ScnFuncInfo_t*)realloc(*list, ((size_t)*count + 1) * sizeof **list);
	fi = &(*list)[*count];
	snprintf(fi->name, sizeof fi->name, "%s", name);
	fi->params = params;
	(*count)++;
}

void Scn_ListFunctions(const ScnFile_t* f, ScnFuncInfo_t** list, int* count)
{
	int i;
	if(f->raw)
	{ // no exports: the functions start at 0 and after every `ret`, and the scenes call them by address
		uint32_t k = 0;
		while(k < f->codeLen)
		{
			int params = ParamsAt(f, k);
			char name[32];
			if(params >= 0)
			{
				snprintf(name, sizeof name, "@0x%x", (unsigned)(k * 4));
				AddFunction(list, count, name, params);
			}
			// to the token after the next ret
			for(; k < f->codeLen; k += 1 + (uint32_t)Scn_OperandCount(f->code[k]))
				if(f->code[k] == SCN_RET)
				{
					k++;
					break;
				}
		}
		return;
	}
	for(i = 0; i < f->exportCount; i++)
	{
		int params = (f->exports[i].off & 3) ? -1 : ParamsAt(f, f->exports[i].off / 4);
		if(params >= 0)
			AddFunction(list, count, f->exports[i].name, params);
	}
}
