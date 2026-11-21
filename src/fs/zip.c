/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * zip.c - zip archives, for the volumes (src/include/bgi/vol.h): the format alone
 *
 * The format of PKWARE's APPNOTE: the central directory at the end of the
 * file, zip64 for archives and entries past 4 GB, entries stored or
 * deflated (an entry compressed another way, or encrypted, is listed but
 * cannot be read) and checked against their CRC-32 when they are read.
 *
 * Zip_Read reads the end records and the central directory and hands the
 * volume one entry per file, its name a clean path:
 *
 *   * names are UTF-8: as the entry says it stores them (flag bit 11, the
 *     Info-ZIP Unicode path field), else as found when they are well-formed
 *     UTF-8, else taken as Shift-JIS - what archives made on Japanese
 *     Windows hold;
 *   * '\\' is taken as a separator too, "." and empty components are
 *     dropped, a name that leaves the root ("..") is ignored, and so is
 *     everything under "__MACOSX/".
 *
 * The local headers are read only when an entry's bytes are first wanted:
 * their name and extra fields may differ from the central directory's, so
 * the data offset is found there.
 */
#include "vol_internal.h"

#include "bgi/bytes.h"
#include "bgi/deflate.h"
#include "bgi/sjis.h"

#define SIG_LOCAL     0x04034b50u
#define SIG_CENTRAL   0x02014b50u
#define SIG_END       0x06054b50u
#define SIG_END64     0x06064b50u
#define SIG_LOCATOR64 0x07064b50u
#define END_SIZE      22     // the end of central directory record without its comment
#define COMMENT_MAX   0xffff // the longest archive comment

// ---- names -----------------------------------------------------------------------------------------

// whether `n` bytes are well-formed UTF-8 (`ascii` is set to whether they are all ASCII)
static int IsUtf8(const uint8_t* s, size_t n, int* ascii)
{
	size_t i = 0;
	*ascii = 1;
	while(i < n)
	{
		uint8_t c = s[i];
		size_t len, k;
		uint32_t cp;
		if(c < 0x80)
		{
			i++;
			continue;
		}
		*ascii = 0;
		if(c >= 0xc2 && c <= 0xdf)
			len = 2, cp = c & 0x1f;
		else if(c >= 0xe0 && c <= 0xef)
			len = 3, cp = c & 0x0f;
		else if(c >= 0xf0 && c <= 0xf4)
			len = 4, cp = c & 0x07;
		else
			return 0;
		if(i + len > n)
			return 0;
		for(k = 1; k < len; k++)
		{
			if((s[i + k] & 0xc0) != 0x80)
				return 0;
			cp = (cp << 6) | (s[i + k] & 0x3f);
		}
		if((len == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (len == 4 && (cp < 0x10000 || cp > 0x10ffff)))
			return 0;
		i += len;
	}
	return 1;
}

/* a stored name as UTF-8 into out (cap bytes): taken as it is when `utf8`
 * is set or the bytes are UTF-8, else decoded as Shift-JIS (whatever the
 * engine's text mode) */
static void DecodeName(const uint8_t* raw, size_t n, int utf8, char* out, size_t cap)
{
	int ascii;
	size_t i, off = 0;
	if(utf8 || IsUtf8(raw, n, &ascii))
	{
		if(n >= cap)
			n = cap - 1;
		memcpy(out, raw, n);
		out[n] = 0;
		return;
	}
	for(i = 0; i < n;)
	{
		uint8_t c = raw[i];
		uint32_t cp;
		if(c < 0x80)
			cp = c, i++;
		else if(c >= 0xa1 && c <= 0xdf)
			cp = 0xff61 + (c - 0xa1), i++; // the half-width katakana
		else if(Sjis_IsLead(c) && i + 1 < n)
		{
			cp = Sjis_CodePoint((uint16_t)((c << 8) | raw[i + 1]));
			i += 2;
		}
		else
			cp = 0xfffd, i++;
		off = Vol_PutUtf8(out, off, cap, cp);
	}
	out[off] = 0;
}

/* a decoded name as a clean relative path in place: '\\' becomes '/',
 * empty and "." components go, a trailing '/' is dropped (and reported in
 * *isDir); 0 when the name leaves the root or nothing is left */
static int CleanPath(char* s, int* isDir)
{
	char* w = s;
	const char* r = s;
	size_t len = strlen(s);
	*isDir = len && (s[len - 1] == '/' || s[len - 1] == '\\');
	while(*r)
	{
		const char* start;
		size_t n;
		while(*r == '/' || *r == '\\')
			r++;
		start = r;
		while(*r && *r != '/' && *r != '\\')
			r++;
		n = (size_t)(r - start);
		if(n == 0 || (n == 1 && start[0] == '.'))
			continue;
		if(n == 2 && start[0] == '.' && start[1] == '.')
			return 0;
		if(w != s)
			*w++ = '/';
		memmove(w, start, n);
		w += n;
	}
	*w = 0;
	return w != s;
}

// ---- the directory ---------------------------------------------------------------------------------

/* the end records of a file of `fileSize` bytes: the central directory's
 * offset, size and entry count; 0 when the file has no end record or spans
 * several disks */
static int FindDirectory(const VolIn_t* f, uint64_t fileSize, uint64_t* cdOff, uint64_t* cdSize, uint64_t* count)
{
	uint8_t* tail;
	uint64_t start;
	size_t n;
	long i;
	int found = 0;
	if(fileSize < END_SIZE)
		return 0;
	n = (size_t)(fileSize < END_SIZE + COMMENT_MAX ? fileSize : END_SIZE + COMMENT_MAX);
	start = fileSize - n;
	tail = (uint8_t*)malloc(n);
	if(!tail || !VolIn_ReadAt(f, start, tail, n))
	{
		free(tail);
		return 0;
	}
	for(i = (long)(n - END_SIZE); i >= 0; i--)
	{
		const uint8_t* e = tail + i;
		uint64_t at = start + (uint64_t)i;
		uint8_t loc[20], end64[56];
		if(Le32(e) != SIG_END)
			continue;
		if(Le16(e + 4) != 0 || Le16(e + 6) != 0 || Le16(e + 8) != Le16(e + 10))
			break; // several disks
		*count = Le16(e + 10);
		*cdSize = Le32(e + 12);
		*cdOff = Le32(e + 16);
		found = 1;
		// a zip64 locator right before the end record points to the zip64 end record, which has the full values
		if(at >= 20 && VolIn_ReadAt(f, at - 20, loc, 20) && Le32(loc) == SIG_LOCATOR64 && VolIn_ReadAt(f, Le64(loc + 8), end64, 56) &&
			Le32(end64) == SIG_END64)
		{
			if(Le32(end64 + 16) != 0 || Le32(end64 + 20) != 0)
				found = 0; // several disks
			*count = Le64(end64 + 32);
			*cdSize = Le64(end64 + 40);
			*cdOff = Le64(end64 + 48);
		}
		break;
	}
	free(tail);
	return found;
}

/* the extra fields of a central directory record: the zip64 sizes and
 * offset where the record's own fields are saturated, the time of an NTFS
 * or an extended timestamp field, and the UTF-8 name of an Info-ZIP
 * Unicode path field whose check value matches the stored name (copied to
 * `uname`, which stays "" otherwise) */
static void ParseExtra(const uint8_t* x, size_t n, const uint8_t* rawName, size_t rawLen, VolEntry_t* e, int zipU, int zipC,
	int zipL, char* uname, size_t ucap)
{
	size_t i = 0;
	uname[0] = 0;
	while(i + 4 <= n)
	{
		uint32_t id = Le16(x + i), len = Le16(x + i + 2);
		const uint8_t* d = x + i + 4;
		if(i + 4 + len > n)
			break;
		if(id == 0x0001)
		{ // zip64: only the fields whose 32-bit form is saturated, in this order
			size_t k = 0;
			if(zipU && k + 8 <= len)
				e->size = Le64(d + k), k += 8;
			if(zipC && k + 8 <= len)
				e->csize = Le64(d + k), k += 8;
			if(zipL && k + 8 <= len)
				e->local = Le64(d + k), k += 8;
		}
		else if(id == 0x000a && len >= 32 && Le16(d + 4) == 1 && Le16(d + 6) >= 24)
			e->mtime = Le64(d + 8); // NTFS: the first attribute's modification FILETIME
		else if(id == 0x5455 && len >= 5 && (d[0] & 1) && !e->mtime)
			e->mtime = ((uint64_t)Le32(d + 1) + 11644473600ull) * 10000000ull; // Unix seconds (taken as unsigned: past 2038)
		else if(id == 0x7075 && len >= 5 && d[0] == 1 && Le32(d + 1) == BGI_Crc32(0, rawName, rawLen))
		{
			size_t m = len - 5 < ucap - 1 ? len - 5 : ucap - 1;
			int ascii;
			if(IsUtf8(d + 5, len - 5, &ascii))
			{
				memcpy(uname, d + 5, m);
				uname[m] = 0;
			}
		}
		i += 4 + len;
	}
}

// read the central directory into `l`; 0 when it is damaged
static int ReadDirectory(const VolIn_t* f, uint64_t cdOff, uint64_t cdSize, uint64_t count, VolList_t* l)
{
	uint8_t* cd;
	size_t p = 0;
	uint64_t i;
	if(count > VOL_MAX_ENTRIES || cdSize > (uint64_t)count * (46 + 3 * 0xffff) || cdSize > 0x40000000u)
		return 0;
	cd = (uint8_t*)malloc((size_t)cdSize + 1);
	if(!cd || !VolIn_ReadAt(f, cdOff, cd, (size_t)cdSize))
	{
		free(cd);
		return 0;
	}
	for(i = 0; i < count; i++)
	{
		const uint8_t* h = cd + p;
		uint32_t nameLen, extraLen, commentLen, flags;
		char name[VOL_NAME_MAX], uname[VOL_NAME_MAX];
		VolEntry_t e;
		int isDir;
		if(p + 46 > cdSize || Le32(h) != SIG_CENTRAL)
			break;
		nameLen = Le16(h + 28);
		extraLen = Le16(h + 30);
		commentLen = Le16(h + 32);
		if(p + 46 + nameLen + extraLen + commentLen > cdSize)
			break;
		memset(&e, 0, sizeof e);
		flags = Le16(h + 8);
		e.method = (uint16_t)Le16(h + 10);
		e.dosTime = (Le16(h + 14) << 16) | Le16(h + 12);
		e.check = Le32(h + 16);
		e.csize = Le32(h + 20);
		e.size = Le32(h + 24);
		e.local = Le32(h + 42);
		ParseExtra(h + 46 + nameLen, extraLen, h + 46, nameLen, &e, e.size == 0xffffffffu, e.csize == 0xffffffffu,
			e.local == 0xffffffffu, uname, sizeof uname);
		if(uname[0])
			snprintf(name, sizeof name, "%s", uname);
		else
			DecodeName(h + 46, nameLen, (flags & 0x0800) != 0, name, sizeof name);
		e.readable = !(flags & 1) && (e.method == 0 || e.method == 8);
		p += 46 + nameLen + extraLen + commentLen;
		if(!CleanPath(name, &isDir))
			continue;
		if(strncmp(name, "__MACOSX", 8) == 0 && (name[8] == 0 || name[8] == '/'))
			continue; // the resource forks of macOS's archiver
		e.isDir = (uint8_t)isDir;
		if(isDir)
			e.readable = 0;
		e.stored = e.readable && e.method == 0;
		if(!VolList_Add(l, name, &e))
		{
			free(cd);
			return 0;
		}
	}
	free(cd);
	return 1;
}

int Zip_Read(const VolIn_t* f, uint64_t fileSize, VolList_t* l)
{
	uint64_t cdOff = 0, cdSize = 0, count = 0;
	if(!FindDirectory(f, fileSize, &cdOff, &cdSize, &count))
		return 0; // not a zip archive
	return ReadDirectory(f, cdOff, cdSize, count, l) ? 1 : -1;
}

// ---- the bytes of an entry -------------------------------------------------------------------------

// the offset of the entry's kept bytes from its local header (remembered in the entry); 0 when the header is wrong
static uint64_t DataOffset(Vol_t* v, const VolEntry_t* ce)
{
	VolEntry_t* e = (VolEntry_t*)ce; // the cache field of the caller's entry, which is one of those the volume lists
	const VolIn_t* in = Vol_In(v);
	uint8_t h[30];
	if(e->data)
		return e->data;
	if(!in->f || !VolIn_ReadAt(in, e->local, h, sizeof h) || Le32(h) != SIG_LOCAL)
		return 0;
	e->data = e->local + 30 + Le16(h + 26) + Le16(h + 28);
	return e->data;
}

static uint8_t* ZipLoad(Vol_t* v, const VolEntry_t* e, size_t* size)
{
	uint64_t off = DataOffset(v, e);
	uint8_t* raw = off ? VolIn_Load(Vol_In(v), off, e->csize) : NULL;
	uint8_t* out;
	size_t n;
	if(!raw)
		return NULL;
	if(e->method == 0)
	{
		out = raw;
		n = (size_t)e->csize;
	}
	else
	{ // the first allocation has room for the spare byte
		out = Deflate_InflateRaw(raw, (size_t)e->csize, (size_t)e->size + 1, &n);
		free(raw);
	}
	if(!out || n != e->size || BGI_Crc32(0, out, n) != e->check)
	{
		free(out);
		return NULL;
	}
	out[n] = 0;
	*size = n;
	return out;
}

// the state of an extraction: what is left of the stored bytes, the check value so far
typedef struct Extract
{
	const VolIn_t* f;
	uint64_t left; // stored bytes not read yet
	uint64_t done; // bytes written
	uint32_t crc;
	int (*write)(void* ctx, const uint8_t* data, size_t n);
	void* ctx;
} Extract_t;

static size_t ExtractRead(void* ctx, uint8_t* buf, size_t n)
{
	Extract_t* x = (Extract_t*)ctx;
	size_t got;
	if(n > x->left)
		n = (size_t)x->left;
	got = n ? Host_FileRead(x->f->o, x->f->f, buf, (uint32_t)n) : 0;
	x->left -= got;
	return got;
}

static int ExtractWrite(void* ctx, const uint8_t* data, size_t n)
{
	Extract_t* x = (Extract_t*)ctx;
	x->crc = BGI_Crc32(x->crc, data, n);
	x->done += n;
	return x->write(x->ctx, data, n);
}
static int ZipExtract(Vol_t* v, const VolEntry_t* e, VolWrite_t write, void* ctx)
{
	const VolIn_t* in = Vol_In(v);
	Extract_t x;
	uint64_t off = DataOffset(v, e);
	int ok;
	if(!off || !Host_FileSeek(in->o, in->f, off))
		return 0;
	memset(&x, 0, sizeof x);
	x.f = in;
	x.left = e->csize;
	x.write = write;
	x.ctx = ctx;
	if(e->method == 8)
		ok = Deflate_InflateStream(ExtractRead, ExtractWrite, &x);
	else
	{
		uint8_t* buf = (uint8_t*)malloc(0x10000);
		size_t got;
		ok = buf != NULL;
		while(ok && (got = ExtractRead(&x, buf, 0x10000)) > 0)
			ok = ExtractWrite(&x, buf, got);
		ok = ok && x.left == 0;
		free(buf);
	}
	return ok && x.done == e->size && x.crc == e->check;
}

static HostFile_t* ZipOpenStored(Vol_t* v, const VolEntry_t* e, uint64_t* offset)
{
	uint64_t off = e->method == 0 ? DataOffset(v, e) : 0;
	HostFile_t* f = off ? Vol_Reopen(v) : NULL;
	if(f)
		*offset = off;
	return f;
}

const VolOrigin_t kZipOrigin = {ZipLoad, ZipExtract, ZipOpenStored};
