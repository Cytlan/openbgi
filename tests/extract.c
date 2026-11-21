/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/extract.c - unit test of the --list / --extract options
 *                   (src/fs/extract.c, declared in src/include/bgi/extract.h);
 *                   built as test_extract by `make test`
 *
 * A PackFile with a raw image, a PCM sound and a program is written into a
 * scratch directory in the temporary directory (made the current
 * directory, so that the OS layer's relative paths land there), and the
 * test runs in three steps:
 *
 *   list    - Extract_List describes the three entries (type, size and
 *             the image / sound parameters) and fails on a missing archive
 *   decode  - Extract_Archive in EXTRACT_DECODE writes every entry byte
 *             for byte; EXTRACT_RAW of one entry finds it regardless of
 *             case, and a missing entry name returns -1
 *   convert - EXTRACT_CONVERT turns the image into a bottom-up 32-bit .bmp
 *             and the sound into a 16-bit mono .wav whose headers and
 *             sample values are checked; the program is left as its bytes
 *   reload  - the archive manager keeps an index it has read; replacing
 *             the archive file is seen only after ArcMgr_Reset ("81 3C"),
 *             which answers ARC_ERR_OPEN for an archive that is not there;
 *             and a font entry that is not in an archive is no error for
 *             "B0 C3" (FontNames_AddFileFromArc answers 0)
 *   load    - from 1.69 build 472 on a DSC entry that is larger as stored
 *             than decoded loads into a buffer of the size GetFileSize_
 *             gives without a byte written past it, loose and from an
 *             archive; "80 31" reads ranges of the decoded data; a whole
 *             24-bit CompressedBG is decoded into the buffer, its size
 *             counting 4 bytes a pixel; "90 C0" shapes empty bitmaps
 *             after a CompressedBG and a raw image (mode, base size) and
 *             refuses a BMP and a DSC stream
 *
 * A failure means the listing text, the output names or the converted
 * formats changed.  The scratch directory is removed at the end.
 */
#include "bgi/extract.h"
#include "bgi/core.h"
#include "bgi/codec.h"
#include "bgi/file.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/gfx/bmpops.h"
#include "host/os.h"
#include "bgi/version.h"

#include "testhost.h"

#include <stdio.h>

#include "check.h"

// store a 32-bit value little-endian
static void Put32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

// write a PackFile holding the one entry `name` with `size` bytes of `data`
static void WriteOneEntryArc(const char* path, const char* name, const void* data, uint32_t size)
{
	uint8_t hdr[16 + 32];
	FILE* f = fopen(path, "wb");
	memset(hdr, 0, sizeof hdr);
	memcpy(hdr, "PackFile    ", 12);
	Put32(hdr + 12, 1);
	memcpy(hdr + 16, name, strlen(name));
	Put32(hdr + 16 + 16, 0);
	Put32(hdr + 16 + 20, size);
	fwrite(hdr, 1, sizeof hdr, f);
	fwrite(data, 1, size, f);
	fclose(f);
}

// the size of a file in bytes, -1 when it does not exist
static long FileSize(const char* path)
{
	OsNativeStat_t st;
	return OS_NativeStat(path, &st) == OS_NATIVE_FILE ? (long)st.size : -1;
}

/* read a whole file into a malloc'd buffer with its size in *n; NULL when it cannot be
 * opened, *n = -1 on a short read */
static uint8_t* ReadAll(const char* path, long* n)
{
	FILE* f = fopen(path, "rb");
	uint8_t* b;
	*n = FileSize(path);
	if(!f || *n < 0)
		return NULL;
	b = (uint8_t*)malloc((size_t)*n + 1);
	if(fread(b, 1, (size_t)*n, f) != (size_t)*n)
		*n = -1;
	fclose(f);
	return b;
}

/* build the archive, then list, decode and convert it; exit status 1 when any check
 * failed */
int main(void)
{
	char dir[0x100];
	uint8_t img[16 + 2 * 2 * 4], snd[0x40 + 100 * 2], prg[16 + 4];
	uint8_t arc[16 + 3 * 32 + sizeof img + sizeof snd + sizeof prg];
	uint32_t p, i;
	FILE* f;
	FILE* tmp;
	char text[0x800];
	long n;
	uint8_t* b;

	OpenBGI_t* o = OpenBGI_New(OS_Host()); // the engine whose code the test runs
	if(!o)
		return 1;
	TestHost_ScratchName(dir, sizeof dir, "bgi_extract");
	TestHost_Mkdir(dir);
	if(TestHost_Chdir(dir) != 0)
		return 1;
	OS_Init();

	// a 2x2 32-bit image: the 16-byte raw image header (width, height, bpp) + BGRA pixels
	memset(img, 0, sizeof img);
	img[0] = 2;
	img[2] = 2;
	img[4] = 32;
	for(i = 0; i < 4; i++)
	{
		img[16 + i * 4] = (uint8_t)(0x10 * i);            // B
		img[16 + i * 4 + 1] = 0x80;                       // G
		img[16 + i * 4 + 2] = (uint8_t)(0xff - 0x10 * i); // R
		img[16 + i * 4 + 3] = 0xff;
	}
	// a BW PCM sound of 100 frames at 8000 Hz, mono: the 0x40-byte header (size, magic,
	// data size, frames, rate, channels, codec) and a ramp of samples
	memset(snd, 0, sizeof snd);
	snd[0] = 0x40;
	memcpy(snd + 4, "bw  ", 4);
	Put32(snd + 8, 200);
	Put32(snd + 12, 100);
	Put32(snd + 16, 8000);
	Put32(snd + 20, 1);
	Put32(snd + 0x30, 1); // PCM
	for(i = 0; i < 100; i++)
	{
		int16_t v = (int16_t)(i * 100);
		memcpy(snd + 0x40 + i * 2, &v, 2);
	}
	// a program: the 16-byte header (code offset 0x10, 4 bytes of code) and "push_i8 0;
	// ret"
	memset(prg, 0, sizeof prg);
	prg[0] = 0x10;
	prg[4] = 4;
	prg[16] = 0x00;
	prg[17] = 0x00;
	prg[18] = 0x17;
	prg[19] = 0x00;
	// the archive: "PackFile    " + count, three 32-byte entries (name, offset from the
	// end of the index, size), the data
	memset(arc, 0, sizeof arc);
	memcpy(arc, "PackFile    ", 12);
	Put32(arc + 12, 3);
	p = 0;
	memcpy(arc + 16, "pic", 3);
	Put32(arc + 16 + 16, p);
	Put32(arc + 16 + 20, sizeof img);
	p += sizeof img;
	memcpy(arc + 48, "snd", 3);
	Put32(arc + 48 + 16, p);
	Put32(arc + 48 + 20, sizeof snd);
	p += sizeof snd;
	memcpy(arc + 80, "prog._bp", 8);
	Put32(arc + 80 + 16, p);
	Put32(arc + 80 + 20, sizeof prg);
	memcpy(arc + 112, img, sizeof img);
	memcpy(arc + 112 + sizeof img, snd, sizeof snd);
	memcpy(arc + 112 + sizeof img + sizeof snd, prg, sizeof prg);
	f = fopen("test.arc", "wb");
	CHECK(f && fwrite(arc, 1, sizeof arc, f) == sizeof arc);
	if(f)
		fclose(f);

	// the listing names each entry's kind and parameters; a missing archive is an error
	printf("list\n");
	tmp = tmpfile();
	CHECK(Extract_List(o, "test.arc", tmp) == 0);
	n = ftell(tmp);
	fseek(tmp, 0, SEEK_SET);
	text[fread(text, 1, sizeof text - 1, tmp)] = 0;
	fclose(tmp);
	CHECK(strstr(text, "3 entries (PackFile)") != NULL);
	CHECK(strstr(text, "image 2x2 32 bpp") != NULL);
	CHECK(strstr(text, "BW PCM 8000 Hz x 1, 100 frames") != NULL);
	CHECK(strstr(text, "program, 4 bytes of code area") != NULL);
	CHECK(Extract_List(o, "nothing.arc", tmp = tmpfile()) != 0);
	fclose(tmp);

	// EXTRACT_DECODE writes every entry as the engine would see it (identical here:
	// nothing is DSC-compressed)
	printf("decode\n");
	CHECK(Extract_Archive(o, "test.arc", NULL, "out", EXTRACT_DECODE, stderr) == 3);
	CHECK(FileSize("out/pic") == (long)sizeof img);
	CHECK(FileSize("out/snd") == (long)sizeof snd);
	CHECK(FileSize("out/prog._bp") == (long)sizeof prg);
	b = ReadAll("out/prog._bp", &n);
	CHECK(b && n == (long)sizeof prg && memcmp(b, prg, sizeof prg) == 0);
	free(b);
	CHECK(Extract_Archive(o, "test.arc", "PROG._BP", "one", EXTRACT_RAW, stderr) == 1); // names match regardless of case
	CHECK(FileSize("one/prog._bp") == (long)sizeof prg);
	CHECK(Extract_Archive(o, "test.arc", "missing", "one", EXTRACT_RAW, tmp = tmpfile()) == -1); // an entry that does not exist
	fclose(tmp);

	// EXTRACT_CONVERT: the image as a .bmp, the sound as a .wav, the program as its bytes
	printf("convert\n");
	CHECK(Extract_Archive(o, "test.arc", NULL, "conv", EXTRACT_CONVERT, stderr) == 3);
	b = ReadAll("conv/pic.bmp", &n);
	CHECK(b && n == 14 + 40 + 2 * 2 * 4 && b[0] == 'B' && b[1] == 'M'); // the file header, a BITMAPINFOHEADER and the pixels
	if(b && n == 70)
	{
		CHECK(b[18] == 2 && b[22] == 2 && b[28] == 32); // width, height, bits per pixel
		// bottom-up: the first stored row is the image's second row (pixels 2, 3), the
		// second row pixels 0, 1
		CHECK(b[54] == 0x20 && b[56] == 0xff - 0x20 && b[62] == 0x00 && b[64] == 0xff);
	}
	free(b);
	b = ReadAll("conv/snd.wav", &n);
	CHECK(b && n == 44 + 200 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WAVEfmt ", 8) == 0); // a 44-byte header and the 200 bytes of samples
	if(b && n == 244)
	{
		int16_t v;
		CHECK(b[22] == 1 && b[24] == 0x40 && b[25] == 0x1f); // mono, 8000 Hz
		memcpy(&v, b + 44 + 10 * 2, 2);
		CHECK(v == 1000); // sample 10 of the ramp
	}
	free(b);
	CHECK(FileSize("conv/prog._bp") == (long)sizeof prg);

	printf("reload\n");
	{
		FileSet_t* mgr = FileSet_New();
		char buf[16];
		WriteOneEntryArc("r.arc", "a.txt", "one", 3);
		memset(buf, 0, sizeof buf);
		CHECK(ArcMgr_Read(o, mgr, buf, "r.arc", "a.txt") == 3 && memcmp(buf, "one", 3) == 0);
		WriteOneEntryArc("r.arc", "a.txt", "second", 6);
		CHECK(ArcMgr_SizeOf(o, mgr, "r.arc", "a.txt") == 3); // the old index
		CHECK(ArcMgr_Reset(o, mgr, "r.arc") == 0);
		CHECK(ArcMgr_Read(o, mgr, buf, "r.arc", "a.txt") == 6 && memcmp(buf, "second", 6) == 0);
		CHECK(ArcMgr_Reset(o, mgr, "missing.arc") == ARC_ERR_OPEN);
		CHECK(ArcMgr_Read(o, mgr, buf, "r.arc", "a.txt") == 6); // the dropped node took its archive back
		FileSet_Delete(o, mgr);
		SetBaseDir(o, dir);
		o->files.arcMgr = FileSet_New(); // what the engine's start-up makes
		CHECK(FontNames_AddFileFromArc(o, "r.arc", "nope.otf") == 0);
		CHECK(FontNames_AddFileFromArc(o, "missing.arc", "nope.otf") == 0);
		CHECK(ReloadArchive(o, "r.arc") == 0 && ReloadArchive(o, "missing.arc") == 1);
		FileSet_Delete(o, o->files.arcMgr);
		o->files.arcMgr = NULL;
	}

	printf("load\n");
	{
		static const char plain[] = "0123456789abcdefghijklmnopqrstuvwxyzABCD"; // 40 bytes that DSC makes larger
		uint8_t pic[16 + 2 * 2 * 3], out[16 + 2 * 2 * 4 + 16], guard[40 + 16];
		uint32_t dscSize, cbgSize;
		uint8_t* dsc = BGI_DscEncode(plain, 40, 0x1234, &dscSize);
		uint8_t* cbg;
		CHECK(Engine_SelectProfile(o, "1.636"));
		SetBaseDir(o, NULL); // "C:\", the scratch directory: the loose readers want a drive
		o->files.arcMgr = FileSet_New();
		CHECK(dscSize > 40);
		WriteOneEntryArc("d.arc", "d.txt", dsc, dscSize);
		f = fopen("l.txt", "wb");
		fwrite(dsc, 1, dscSize, f);
		fclose(f);
		// the size and the load agree on the decoded size, and nothing is written past it
		CHECK(GetFileSize_(o, "d.arc", "d.txt") == 40 && GetFileSize_(o, NULL, "l.txt") == 40);
		memset(guard, 0xcc, sizeof guard);
		CHECK(LoadFile(o, guard, "d.arc", "d.txt") == 40 && memcmp(guard, plain, 40) == 0);
		for(i = 40; i < sizeof guard; i++)
			CHECK(guard[i] == 0xcc);
		memset(guard, 0xcc, sizeof guard);
		CHECK(LoadFile(o, guard, NULL, "l.txt") == 40 && memcmp(guard, plain, 40) == 0 && guard[40] == 0xcc);
		// "80 31": ranges of the decoded data
		memset(guard, 0, sizeof guard);
		CHECK(LoadFileRangeDecoded(o, guard, "d.arc", "d.txt", 10, 5) == 0 && memcmp(guard, plain + 10, 5) == 0);
		CHECK(LoadFileRangeDecoded(o, guard, "d.arc", "d.txt", 36, 5) == 2);
		CHECK(LoadFileRangeDecoded(o, guard, "d.arc", "d.txt", 0, 41) == 3);
		CHECK(LoadFileRangeDecoded(o, guard, "d.arc", "nope.txt", 0, 0) == 1);
		// a 24-bit CompressedBG: the size counts 4 bytes a pixel, the whole file decodes straight into the buffer
		memset(pic, 0, sizeof pic);
		pic[0] = 2;
		pic[2] = 2;
		pic[4] = 24;
		for(i = 16; i < sizeof pic; i++)
			pic[i] = (uint8_t)(i * 7);
		cbg = BGI_CbgEncode(pic, 0x55, &cbgSize);
		WriteOneEntryArc("c.arc", "c.cbg", cbg, cbgSize);
		CHECK(GetFileSize_(o, "c.arc", "c.cbg") == 16 + 2 * 2 * 4);
		memset(out, 0xcc, sizeof out);
		CHECK(LoadFileRangeDecoded(o, out, "c.arc", "c.cbg", 0, 0) == 0 && memcmp(out, pic, sizeof pic) == 0);
		memset(out, 0xcc, sizeof out);
		CHECK(LoadFile(o, out, "c.arc", "c.cbg") == 16 + 2 * 2 * 4 && memcmp(out, pic, sizeof pic) == 0 && out[32] == 0xcc);
		memset(out, 0, sizeof out);
		CHECK(LoadFileRangeDecoded(o, out, "c.arc", "c.cbg", 16, 6) == 0 && memcmp(out, pic + 16, 6) == 0);
		// "90 C0": a zero-filled bitmap shaped like the picture file, from its stored header
		{
			uint8_t raw[16 + 3 * 4 * 4]; // at least the 0x30 bytes the header read takes
			Bmp_t info;
			memset(raw, 0x5a, sizeof raw);
			memset(raw, 0, 16);
			raw[0] = 3;
			raw[2] = 4;
			raw[4] = 32;
			raw[8] = 5;    // a height field
			raw[0xa] = 1;  // with a base size
			raw[0xc] = 30; // of 30 x 20
			raw[0xe] = 20;
			WriteOneEntryArc("r2.arc", "raw", raw, sizeof raw);
			WriteOneEntryArc("b.arc", "bmp", "BM\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 48);
			o->gfx.bmpMgr = BmpMgr_New(4);
			{ // a slot has no base size until a picture declares one
				int32_t base[2] = {7, 7};
				CHECK(BmpMgr_GetBaseSize(o->gfx.bmpMgr, base, 0) && base[0] == -1 && base[1] == -1);
				CHECK(!BmpMgr_GetBaseSize(o->gfx.bmpMgr, base, 4));
			}
			CHECK(BmpOp_CreateLikeFile(o, 0, "c.arc", "c.cbg") == 0);
			CHECK(BmpMgr_GetInfo(o->gfx.bmpMgr, &info, 0) && info.w == 2 && info.h == 2 && info.mode == PM_RGB32);
			CHECK(info.pixels[0] == 0 && info.pixels[info.pitch + 7] == 0);
			CHECK(BmpOp_CreateLikeFile(o, 1, "r2.arc", "raw") == 0);
			CHECK(BmpMgr_GetInfo(o->gfx.bmpMgr, &info, 1) && info.w == 3 && info.h == 4 && info.mode == PM_HEIGHT);
			{
				int32_t base[2];
				CHECK(BmpMgr_GetBaseSize(o->gfx.bmpMgr, base, 1) && base[0] == 30 && base[1] == 20);
				// a picture that declares none leaves the slot's as it is: none in a fresh slot ..
				CHECK(BmpMgr_GetBaseSize(o->gfx.bmpMgr, base, 0) && base[0] == -1 && base[1] == -1);
				// .. and none after the slot's bitmap was replaced, which releases the old one
				CHECK(BmpOp_CreateLikeFile(o, 1, "c.arc", "c.cbg") == 0);
				CHECK(BmpMgr_GetBaseSize(o->gfx.bmpMgr, base, 1) && base[0] == -1 && base[1] == -1);
				CHECK(BmpOp_CreateLikeFile(o, 1, "r2.arc", "raw") == 0);
				CHECK(BmpMgr_FreeSlot(o->gfx.bmpMgr, 1) && BmpMgr_GetBaseSize(o->gfx.bmpMgr, base, 1) && base[0] == -1 && base[1] == -1);
				CHECK(BmpOp_CreateLikeFile(o, 1, "r2.arc", "raw") == 0);
			}
			CHECK(BmpOp_CreateLikeFile(o, 2, "b.arc", "bmp") == (int)BMP_ERR_NOT_BG);
			CHECK(BmpOp_CreateLikeFile(o, 2, "d.arc", "d.txt") == (int)BMP_ERR_NOT_BG); // the DSC stream as stored
			CHECK(BmpOp_CreateLikeFile(o, 2, "c.arc", "nope") == -1);
			BmpMgr_Delete(o->gfx.bmpMgr);
			o->gfx.bmpMgr = NULL;
		}
		// the stored model of 1.69 build 444: a DSC stream is decoded in place
		CHECK(Engine_SelectProfile(o, "1.69/444"));
		memset(guard, 0, sizeof guard);
		CHECK(GetFileSize_(o, "d.arc", "d.txt") == 40);
		BGI_Free(dsc);
		BGI_Free(cbg);
		FileSet_Delete(o, o->files.arcMgr);
		o->files.arcMgr = NULL;
	}

	if(TestHost_Chdir(TestHost_TempRoot()) == 0) // clean up
		TestHost_RemoveTree(dir);
	OS_Shutdown();
	if(gFails)
	{
		printf("%d failure(s)\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
