/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * extract.c - listing and extracting the entries of the game archives;
 *             interface in extract.h
 *
 * The archive is read with arcread.c (a native path, through the host's
 * files); the output goes through the host's files too, by the game's
 * paths (so Shift-JIS entry names come out right on every platform).  KindOf recognises an entry by its header; the converters use
 * the engine's own decoders: CompressedBG (codec.h), the BW sound streams
 * (snd/bw.h), BF_Movie (bfmovie.h).  Messages about single entries go to the
 * `log` stream of the operation, with the entry name converted to UTF-8.
 */
#include "bgi/extract.h"
#include "bgi/bytes.h"
#include "bgi/arcread.h"
#include "bgi/codec.h"
#include "bgi/bfmovie.h"
#include "bgi/snd/bw.h"
#include "bgi/host.h"
#include "bgi/sjis.h"
#include "bgi/core.h"

// ---- what an entry is ----------------------------------------------------------------------

typedef enum EntryKind
{
	KIND_BYTES,    // nothing recognised: written as it is
	KIND_PROGRAM,  // a compiled program (._bp)
	KIND_CBG,      // CompressedBG image
	KIND_RAWIMAGE, // uncompressed image: RawImageHeader + pixels
	KIND_BW,       // BW sound
	KIND_BFMOVIE,  // BF_Movie frame sequence
	KIND_SCENARIO  // BurikoCompiledScriptVer1.00 scenario file
} EntryKind_t;

/* a compiled program: a 16-byte header of the header size (0x10), the size
 * of the code that follows and eight zero bytes */
static int IsProgram(const uint8_t* d, uint32_t n)
{
	uint32_t size;
	if(n < 16 || d[0] != 0x10 || d[1] || d[2] || d[3] || memcmp(d + 8, "\0\0\0\0\0\0\0\0", 8) != 0)
		return 0;
	memcpy(&size, d + 4, 4);
	return size + 16 == n;
}

// an uncompressed image: a plausible RawImageHeader whose pixel data fills the entry exactly
static int IsRawImage(const uint8_t* d, uint32_t n)
{
	RawImageHeader_t h;
	if(n < 16)
		return 0;
	memcpy(&h, d, sizeof h);
	return h.width && h.height && (h.bpp == 8 || h.bpp == 24 || h.bpp == 32) &&
		16u + (uint32_t)h.width * h.height * (h.bpp / 8) == n;
}

/* classify the entry data (after the DSC layer) and describe it in `desc`
 * (dn bytes) for the listing */
static EntryKind_t KindOf(const uint8_t* d, uint32_t n, char* desc, size_t dn)
{
	desc[0] = 0;
	if(n >= 0x30 && BGI_IsCbg(d))
	{
		const CbgHeader_t* h = (const CbgHeader_t*)d;
		snprintf(desc, dn, "CompressedBG %ux%u %u bpp", h->width, h->height, (unsigned)(h->bpp & 0xffff));
		return KIND_CBG;
	}
	// a BW sound starts with its header size (0x40), then the magic
	if(n >= 0x40 && memcmp(d + 4, "bw  ", 4) == 0 && d[0] == 0x40)
	{
		const BwHeader_t* h = (const BwHeader_t*)d;
		static const char* const codecs[4] = {"ADPCM", "PCM", "Huffman", "Vorbis"};
		snprintf(desc, dn, "BW %s %u Hz x %u, %u frames%s", h->codec < 4 ? codecs[h->codec] : "?", (unsigned)h->rate,
			(unsigned)h->channels, (unsigned)h->frames, h->loopFlag ? ", loops" : "");
		return KIND_BW;
	}
	if(n >= 0x40 && BfMovie_Check(d, n))
	{
		const BfMovieHeader_t* h = (const BfMovieHeader_t*)d;
		snprintf(desc, dn, "BF_Movie %ux%u %u bpp, %u frames at %u fps%s", (unsigned)h->width, (unsigned)h->height,
			(unsigned)h->bpp, (unsigned)h->frames, (unsigned)h->fps, h->type ? " (streamed type)" : "");
		return KIND_BFMOVIE;
	}
	if(IsProgram(d, n))
	{
		snprintf(desc, dn, "program, %u bytes of code area", (unsigned)(n - 16));
		return KIND_PROGRAM;
	}
	if(n >= 28 && memcmp(d, "BurikoCompiledScriptVer1.00", 27) == 0)
	{
		snprintf(desc, dn, "compiled scenario");
		return KIND_SCENARIO;
	}
	if(IsRawImage(d, n))
	{
		RawImageHeader_t h;
		memcpy(&h, d, sizeof h);
		snprintf(desc, dn, "image %ux%u %u bpp", h.width, h.height, (unsigned)h.bpp);
		return KIND_RAWIMAGE;
	}
	snprintf(desc, dn, "%u bytes", (unsigned)n);
	return KIND_BYTES;
}

// ---- writers ------------------------------------------------------------------------------------

// create `path` (engine path convention) with the n bytes of data; 1 when all of it was written
static int WriteWhole(OpenBGI_t* o, const char* path, const void* data, uint32_t n)
{
	HostFile_t* f = Host_FileOpen(o, path, OPENBGI_OPEN_CREATE);
	uint32_t w;
	if(!f)
		return 0;
	w = Host_FileWrite(o, f, data, n);
	Host_FileClose(o, f);
	return w == n;
}

/* write a Windows bitmap of w x h pixels of `bpp` bits (8: grey palette, 24:
 * BGR, 32: BGRA) from top-down rows without padding, as the engine keeps
 * its images; 1 when written */
static int WriteBmp(OpenBGI_t* o, const char* path, const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t bpp)
{
	uint32_t rowIn = w * (bpp / 8), rowOut = (rowIn + 3) & ~3u; // BMP rows are padded to 4 bytes
	uint32_t palette = bpp == 8 ? 256 * 4 : 0;
	uint32_t dataSize = rowOut * h, offset = 14 + 40 + palette, total = offset + dataSize;
	uint8_t* bmp = (uint8_t*)BGI_Alloc(total);
	uint8_t* p = bmp;
	uint32_t y, i;
	int ok;
	memset(bmp, 0, total);
	// BITMAPFILEHEADER
	memcpy(p, "BM", 2);
	PutLe32(p + 2, total);
	PutLe32(p + 10, offset);
	p += 14;
	// BITMAPINFOHEADER
	PutLe32(p, 40);
	PutLe32(p + 4, w);
	PutLe32(p + 8, h); // positive: bottom-up rows
	PutLe16(p + 12, 1);
	PutLe16(p + 14, bpp);
	PutLe32(p + 20, dataSize);
	PutLe32(p + 24, 2835); // 72 dpi in pixels per metre
	PutLe32(p + 28, 2835);
	PutLe32(p + 32, bpp == 8 ? 256 : 0);
	p += 40;
	if(bpp == 8)
	{
		// a grey ramp palette
		for(i = 0; i < 256; i++)
		{
			p[i * 4] = p[i * 4 + 1] = p[i * 4 + 2] = (uint8_t)i;
			p[i * 4 + 3] = 0;
		}
		p += palette;
	}
	for(y = 0; y < h; y++)
		memcpy(p + (size_t)(h - 1 - y) * rowOut, pixels + (size_t)y * rowIn, rowIn);
	ok = WriteWhole(o, path, bmp, total);
	BGI_Free(bmp);
	return ok;
}

/* decode the whole BW sound in `data` with the Wave Master stream classes
 * and write it as a 16-bit PCM WAV; 1 when written.  `name` is the entry
 * name for messages on `log`. */
static int WriteWav(OpenBGI_t* o, const char* path, const uint8_t* data, uint32_t n, FILE* log, const char* name)
{
	WmSource_t* src = WmSource_FromMemory(data, n);
	WmStream_t* s;
	int err = 0;
	uint32_t rate, ch, total = 0, cap;
	int16_t* pcm;
	uint8_t* wav;
	int ok;
	if(!src)
		return 0;
	s = WmStream_Open(o, src, 1.0, 0, &err);
	if(!s)
	{
		fprintf(log, "%s: the sound does not open (error 0x%x)\n", name, (unsigned)err);
		return 0;
	}
	rate = WmStream_Rate(s);
	ch = WmStream_Channels(s);
	cap = WmStream_Frames(s) + 1024; // 1024 frames of slack beyond the announced length
	pcm = (int16_t*)BGI_Alloc((size_t)cap * ch * 2);
	for(;;)
	{
		uint32_t got;
		if(total == cap)
			break;
		got = WmStream_Decode(s, pcm + (size_t)total * ch, cap - total > 4096 ? 4096 : cap - total);
		total += got;
		if(got == 0)
			break;
	}
	WmStream_Close(s);
	{
		// the canonical 44-byte RIFF/WAVE header, format 1 (PCM), 16 bits
		uint32_t bytes = total * ch * 2;
		wav = (uint8_t*)BGI_Alloc(44 + bytes);
		memcpy(wav, "RIFF", 4);
		PutLe32(wav + 4, 36 + bytes);
		memcpy(wav + 8, "WAVEfmt ", 8);
		PutLe32(wav + 16, 16);
		PutLe16(wav + 20, 1);
		PutLe16(wav + 22, ch);
		PutLe32(wav + 24, rate);
		PutLe32(wav + 28, rate * ch * 2);
		PutLe16(wav + 32, ch * 2);
		PutLe16(wav + 34, 16);
		memcpy(wav + 36, "data", 4);
		PutLe32(wav + 40, bytes);
		memcpy(wav + 44, pcm, bytes);
		ok = WriteWhole(o, path, wav, 44 + bytes);
		BGI_Free(wav);
	}
	BGI_Free(pcm);
	return ok;
}

/* write every frame of a BF_Movie as <base>_NNN.bmp (32 bpp), decoding the
 * frames in order into one slot as the engine would; 1 when all frames were
 * written.  A movie of an unknown type is refused with a message. */
static int WriteFrames(OpenBGI_t* o, const char* base, const uint8_t* data, uint32_t n, FILE* log, const char* name)
{
	const BfMovieHeader_t* h = (const BfMovieHeader_t*)data;
	uint32_t w = h->width, hh = h->height, i, written = 0;
	uint8_t* slot;
	char path[0x300];
	if(!BfMovie_CanDecode(data))
	{
		fprintf(log, "%s: a BF_Movie of the unknown type 0x%x\n", name, (unsigned)h->type);
		return 0;
	}
	slot = (uint8_t*)BGI_Alloc((size_t)w * hh * 4);
	memset(slot, 0, (size_t)w * hh * 4);
	for(i = 0; i < h->frames; i++)
	{
		if(!BfMovie_DecodeFrame(data, n, i, slot, (int)(w * 4)))
		{
			fprintf(log, "%s: frame %u does not decode\n", name, (unsigned)i);
			break;
		}
		snprintf(path, sizeof path, "%s_%03u.bmp", base, (unsigned)i);
		if(!WriteBmp(o, path, slot, w, hh, 32))
			break;
		written++;
	}
	BGI_Free(slot);
	return written == h->frames;
}

// ---- the operations ------------------------------------------------------------------------------

// the archive named on the command line: a native path, so that it opens whatever its name is spelled in
static ArcRead_t* OpenArchive(OpenBGI_t* o, const char* path)
{
	return ArcRead_OpenNative(o, path);
}

/* --list: a heading with the archive format and entry count, then one line
 * per entry: name (UTF-8), stored size, "DSC: " when it is DSC-compressed,
 * and what the decoded data is.  0 on success, 1 when the archive does not
 * open. */
int Extract_List(OpenBGI_t* o, const char* arcPath, FILE* out)
{
	ArcRead_t* a = OpenArchive(o, arcPath);
	uint32_t i;
	if(!a)
		return 1;
	fprintf(out, "%s: %u entries (%s)\n", arcPath, (unsigned)a->count, a->arc20 ? "BURIKO ARC20" : "PackFile");
	for(i = 0; i < a->count; i++)
	{
		const ArcReadEntry_t* e = &a->entries[i];
		uint32_t size;
		int dsc = 0;
		uint8_t* data = ArcRead_Load(o, a, e, &size, &dsc);
		char utf[0x200], desc[0x100];
		Sjis_ToUtf8(e->name, utf, sizeof utf, o->enc.textUtf8);
		if(!data)
		{
			fprintf(out, "  %-32s %10u  (unreadable)\n", utf, (unsigned)e->size);
			continue;
		}
		KindOf(data, size, desc, sizeof desc);
		fprintf(out, "  %-32s %10u  %s%s\n", utf, (unsigned)e->size, dsc ? "DSC: " : "", desc);
		BGI_Free(data);
	}
	ArcRead_Close(a);
	return 0;
}

/* write one entry into outDir according to `mode` (EXTRACT_*); the output
 * file keeps the entry's name, a converted one gets .bmp / .wav appended or,
 * for a movie, becomes a set of <name>_NNN.bmp.  1 when written, 0 with a
 * message on `log` otherwise. */
static int WriteEntry(OpenBGI_t* o, const ArcRead_t* a, const ArcReadEntry_t* e, const char* outDir, int mode, FILE* log)
{
	uint32_t size;
	int dsc = 0, ok = 0;
	uint8_t* data;
	char path[0x300], desc[0x100], utf[0x200];
	EntryKind_t kind;
	Sjis_ToUtf8(e->name, utf, sizeof utf, o->enc.textUtf8);
	if(mode == EXTRACT_RAW)
	{ // the stored bytes, read back without the DSC layer
		data = (uint8_t*)BGI_Alloc(e->size + 1);
		if(!ArcRead_ReadAt(a, e->offset, data, e->size))
		{
			fprintf(log, "%s: read error\n", utf);
			BGI_Free(data);
			return 0;
		}
		snprintf(path, sizeof path, "%s\\%s", outDir, e->name);
		ok = WriteWhole(o, path, data, e->size);
		BGI_Free(data);
		return ok;
	}
	data = ArcRead_Load(o, a, e, &size, &dsc);
	if(!data)
	{
		fprintf(log, "%s: read error\n", utf);
		return 0;
	}
	kind = KindOf(data, size, desc, sizeof desc);
	if(mode == EXTRACT_CONVERT && kind == KIND_CBG)
	{
		uint32_t n = BGI_CbgOutSize(data);
		uint8_t* img = (uint8_t*)BGI_Alloc(n);
		RawImageHeader_t h;
		if(BGI_CbgDecode(data, img) == 0)
		{
			memcpy(&h, img, sizeof h);
			snprintf(path, sizeof path, "%s\\%s.bmp", outDir, e->name);
			ok = WriteBmp(o, path, img + 16, h.width, h.height, h.bpp);
		}
		else
			fprintf(log, "%s: the CompressedBG does not decode\n", utf);
		BGI_Free(img);
	}
	else if(mode == EXTRACT_CONVERT && kind == KIND_RAWIMAGE)
	{
		RawImageHeader_t h;
		memcpy(&h, data, sizeof h);
		snprintf(path, sizeof path, "%s\\%s.bmp", outDir, e->name);
		ok = WriteBmp(o, path, data + 16, h.width, h.height, h.bpp);
	}
	else if(mode == EXTRACT_CONVERT && kind == KIND_BW)
	{
		snprintf(path, sizeof path, "%s\\%s.wav", outDir, e->name);
		ok = WriteWav(o, path, data, size, log, utf);
	}
	else if(mode == EXTRACT_CONVERT && kind == KIND_BFMOVIE)
	{
		snprintf(path, sizeof path, "%s\\%s", outDir, e->name);
		ok = WriteFrames(o, path, data, size, log, utf);
	}
	else
	{ // EXTRACT_DECODE, and the kinds without a conversion
		snprintf(path, sizeof path, "%s\\%s", outDir, e->name);
		ok = WriteWhole(o, path, data, size);
	}
	if(!ok)
		fprintf(log, "%s: not written\n", utf);
	BGI_Free(data);
	return ok;
}

/* --extract: write the entry `name` (NULL: every entry) into outDir, created
 * when missing.  Returns the number of entries written; -1 when the archive
 * does not open (message on stderr), the directory cannot be created or the
 * named entry does not exist (message on `log`). */
int Extract_Archive(OpenBGI_t* o, const char* arcPath, const char* name, const char* outDir, int mode, FILE* log)
{
	ArcRead_t* a = OpenArchive(o, arcPath);
	uint32_t i;
	int written = 0;
	if(!a)
		return -1;
	if(Host_FileAttrs(o, outDir) == OPENBGI_INVALID_ATTRS && !Host_DirCreate(o, outDir))
	{
		fprintf(log, "%s: cannot create the output directory\n", outDir);
		ArcRead_Close(a);
		return -1;
	}
	if(name)
	{
		const ArcReadEntry_t* e = ArcRead_Find(a, name);
		if(!e)
		{
			fprintf(log, "%s: no entry %s\n", arcPath, name);
			ArcRead_Close(a);
			return -1;
		}
		written = WriteEntry(o, a, e, outDir, mode, log);
	}
	else
	{
		for(i = 0; i < a->count; i++)
			written += WriteEntry(o, a, &a->entries[i], outDir, mode, log);
	}
	ArcRead_Close(a);
	return written;
}
