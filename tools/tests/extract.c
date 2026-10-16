/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/extract.c - unit test of the --list / --extract options
 *                   (src/core/extract.c, declared in inc/bgi/extract.h);
 *                   built as bin/test_extract by `make test`
 *
 * A PackFile with a raw image, a PCM sound and a program is written into a
 * scratch directory under /tmp (made the current directory, so that the
 * OS layer's relative paths land there), and the test runs in three steps:
 *
 *   list    - Extract_List describes the three entries (type, size and
 *             the image / sound parameters) and fails on a missing archive
 *   decode  - Extract_Archive in EXTRACT_DECODE writes every entry byte
 *             for byte; EXTRACT_RAW of one entry finds it regardless of
 *             case, and a missing entry name returns -1
 *   convert - EXTRACT_CONVERT turns the image into a bottom-up 32-bit .bmp
 *             and the sound into a 16-bit mono .wav whose headers and
 *             sample values are checked; the program is left as its bytes
 *
 * A failure means the listing text, the output names or the converted
 * formats changed.  The scratch directory is removed at the end.
 */
#include "bgi/extract.h"
#include "bgi/os.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

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

// store a 32-bit value little-endian
static void Put32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

// the size of a file in bytes, -1 when it does not exist
static long FileSize(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0 ? (long)st.st_size : -1;
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

	snprintf(dir, sizeof dir, "/tmp/bgi_extract_%ld", (long)getpid());
	mkdir(dir, 0755);
	if(chdir(dir) != 0)
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
	CHECK(Extract_List("test.arc", tmp) == 0);
	n = ftell(tmp);
	fseek(tmp, 0, SEEK_SET);
	text[fread(text, 1, sizeof text - 1, tmp)] = 0;
	fclose(tmp);
	CHECK(strstr(text, "3 entries (PackFile)") != NULL);
	CHECK(strstr(text, "image 2x2 32 bpp") != NULL);
	CHECK(strstr(text, "BW PCM 8000 Hz x 1, 100 frames") != NULL);
	CHECK(strstr(text, "program, 4 bytes of code area") != NULL);
	CHECK(Extract_List("nothing.arc", tmp = tmpfile()) != 0);
	fclose(tmp);

	// EXTRACT_DECODE writes every entry as the engine would see it (identical here:
	// nothing is DSC-compressed)
	printf("decode\n");
	CHECK(Extract_Archive("test.arc", NULL, "out", EXTRACT_DECODE, stderr) == 3);
	CHECK(FileSize("out/pic") == (long)sizeof img);
	CHECK(FileSize("out/snd") == (long)sizeof snd);
	CHECK(FileSize("out/prog._bp") == (long)sizeof prg);
	b = ReadAll("out/prog._bp", &n);
	CHECK(b && n == (long)sizeof prg && memcmp(b, prg, sizeof prg) == 0);
	free(b);
	CHECK(Extract_Archive("test.arc", "PROG._BP", "one", EXTRACT_RAW, stderr) == 1); // names match regardless of case
	CHECK(FileSize("one/prog._bp") == (long)sizeof prg);
	CHECK(Extract_Archive("test.arc", "missing", "one", EXTRACT_RAW, tmp = tmpfile()) == -1); // an entry that does not exist
	fclose(tmp);

	// EXTRACT_CONVERT: the image as a .bmp, the sound as a .wav, the program as its bytes
	printf("convert\n");
	CHECK(Extract_Archive("test.arc", NULL, "conv", EXTRACT_CONVERT, stderr) == 3);
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

	OS_Shutdown();
	{ // clean up
		char cmd[0x200];
		snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
		if(chdir("/") == 0 && system(cmd) != 0)
			printf("  (could not remove %s)\n", dir);
	}
	if(gFails)
	{
		printf("%d failure(s)\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
