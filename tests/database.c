/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/database.c - unit test of the global database BGI.gdb
 *                    (src/sysobj/database.c, src/include/bgi/sysobj/database.h)
 *                    with the flag groups and names it stores; built as
 *                    test_database by `make test`
 *
 * A state of global memory, system area, names and flag groups is saved,
 * spoiled, and loaded again, in each build family's own layout.  The file
 * goes to bin/ (as the data directory), so the test runs from the project
 * root (as `make test` does).
 *
 * The groups of tests, in the order they run:
 *
 *   layout A  - 1.69 build 472: the fixed layout with 0x1000 name slots
 *   layout B  - 1.553: 0x4000 name slots (a name past slot 0x1000 survives),
 *               the flag records after them; and an A file read there
 *   gdb 3.00  - 1.636: the sized sections, names and flag groups beyond the
 *               old limits (a name of 40 characters, 0x900 groups), the
 *               latest group written first, the size field checked; a B
 *               file read there; the widened A of OpenBGI's own earlier
 *               files
 *   merge     - "81 80": names added, groups OR'ed and grown, the global
 *               memory prefix and the system area handed out padded, the
 *               live ones untouched; a file that is not GDB 3.00 refused
 *   flags     - the flag manager's limits until 1.599 and none after
 */
#include "bgi/sysobj/database.h"
#include "bgi/core.h"
#include "bgi/sysobj/flags.h"
#include "bgi/sysobj/names.h"
#include "bgi/sysobj/strtables.h"
#include "bgi/codec.h"
#include "bgi/display.h"
#include "bgi/file.h"
#include "bgi/sys.h"
#include "host/os.h"
#include "bgi/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

#define GDB_PATH "bin/BGI.gdb"

// select a build and start from an empty state of its sizes
static void Fresh(OpenBGI_t* o, const char* profile)
{
	CHECK(Engine_SelectProfile(o, profile));
	o->mem.sysAreaSize = o->engine.profile->gen >= GEN_1_616 ? 0x100000 : 0x40000;
	SysArea_Shutdown(o);
	SysArea_Init(o);
	memset(o->mem.globalMem, 0, o->mem.globalMemSize);
}

// a recognisable state: global memory and system area patterns, names, two groups
static void Fill(OpenBGI_t* o)
{
	uint32_t i;
	for(i = 0; i < 0x400; i++)
		o->mem.globalMem[i] = (uint8_t)(i * 7 + 1);
	for(i = 0; i < o->mem.sysAreaSize; i++)
		o->mem.sysArea[i] = (uint8_t)(i * 13 + (i >> 12));
	CHECK(GName_Add(o, "alpha") && GName_Add(o, "beta"));
	CHECK(GFlag_RegisterGroup(o, "route", 20) && GFlag_RegisterGroup(o, "cg", 300));
	CHECK(GFlag_SetBit(o, "route", 3, 1) == FLAG_OK && GFlag_SetBit(o, "cg", 299, 1) == FLAG_OK);
}

// spoil what Fill made
static void Spoil(OpenBGI_t* o)
{
	memset(o->mem.globalMem, 0xEE, 0x400);
	memset(o->mem.sysArea, 0xEE, o->mem.sysAreaSize);
	FlagMgr_Clear(o->sysobj.flagMgr);
	memset(o->sysobj.nameTable, 0, sizeof o->sysobj.nameTable);
	StrTab_Remove(o, GNAME_TABLE);
}

// whether the state is what Fill made
static int Filled(OpenBGI_t* o)
{
	uint32_t i, v = 0;
	for(i = 0; i < 0x400; i++)
		if(o->mem.globalMem[i] != (uint8_t)(i * 7 + 1))
			return 0;
	for(i = 0; i < o->mem.sysAreaSize; i++)
		if(o->mem.sysArea[i] != (uint8_t)(i * 13 + (i >> 12)))
			return 0;
	if(!GName_Exists(o, "alpha") || !GName_Exists(o, "beta") || GName_Exists(o, "gamma"))
		return 0;
	if(GFlag_GetBit(o, &v, "route", 3) != FLAG_OK || !v || GFlag_GetBit(o, &v, "route", 2) != FLAG_OK || v)
		return 0;
	return GFlag_GetBit(o, &v, "cg", 299) == FLAG_OK && v;
}

// the decoded payload of the file written last (BGI_Free it); NULL when there is none
static uint8_t* Decoded(OpenBGI_t* o, uint32_t* n)
{
	FILE* f = fopen(GDB_PATH, "rb");
	uint8_t* file;
	uint8_t* db;
	long size;
	if(!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	file = (uint8_t*)BGI_Alloc((size_t)size);
	if(fread(file, 1, (size_t)size, f) != (size_t)size)
		size = 0;
	fclose(f);
	db = (uint8_t*)BGI_Alloc(SdcOutSize(file) + 1);
	*n = SdcDecode(o, db, file);
	BGI_Free(file);
	return db;
}

// encode a payload as the database file
static void WriteDecoded(OpenBGI_t* o, const uint8_t* db, uint32_t n)
{
	uint8_t* enc = (uint8_t*)BGI_Alloc(n * 2 + 0x40);
	uint32_t m = SdcEncode(o, enc, db, n);
	FILE* f = fopen(GDB_PATH, "wb");
	if(f)
	{
		fwrite(enc, 1, m, f);
		fclose(f);
	}
	BGI_Free(enc);
}

/* 1.69 build 472 writes layout A (0x1000 name slots, the flag records at 0x60408)
 * and reads it back into the state it saved.  A failure means the fixed
 * layout of the early builds moved. */
static void TestLayoutA(OpenBGI_t* o)
{
	uint8_t* db;
	uint32_t n = 0;
	printf("layout A\n");
	Fresh(o, "1.69/472");
	Fill(o);
	CHECK(GlobalDB_Save(o));
	db = Decoded(o, &n);
	CHECK(db && n == 0x70408 + 3 + 38 && strcmp((char*)db + 0x40408, "alpha") == 0 && strcmp((char*)db + 0x60408, "route") == 0);
	BGI_Free(db);
	Spoil(o);
	CHECK(GlobalDB_Load(o) == 0 && Filled(o));
}

/* 1.553 writes layout B (0x4000 name slots, the flag records at 0xC0408)
 * and reads it back, a name in slot 0x2000 included; a layout A file
 * (1.69's) is recognised by its first flag record and read as well.  A
 * damaged size is 0x80000002.  A failure means the B layout or the
 * detection between A and B changed. */
static void TestLayoutB(OpenBGI_t* o)
{
	uint8_t* db;
	uint32_t n = 0;
	int i;
	char name[16];
	printf("layout B\n");
	Fresh(o, "1.553");
	Fill(o);
	for(i = 2; i < 0x2001; i++)
	{
		sprintf(name, "n%d", i);
		GName_Add(o, name);
	}
	CHECK(GName_Exists(o, "n8000"));
	CHECK(GlobalDB_Save(o));
	db = Decoded(o, &n);
	CHECK(db && n == 0xD0408 + 3 + 38 && strcmp((char*)db + 0x40408 + 0x2000 * 0x20, "n8192") == 0 &&
		strcmp((char*)db + 0xC0408, "route") == 0);
	Spoil(o);
	CHECK(GlobalDB_Load(o) == 0 && Filled(o) && GName_Exists(o, "n8192"));
	if(db)
	{
		WriteDecoded(o, db, n - 1); // one byte short of its flag bytes
		CHECK(GlobalDB_Load(o) == 0x80000002u);
	}
	BGI_Free(db);
	Fresh(o, "1.69/472");
	Fill(o);
	CHECK(GlobalDB_Save(o));
	Fresh(o, "1.553");
	CHECK(GlobalDB_Load(o) == 0);
	CHECK(Filled(o));
}

/* 1.636 writes GDB 3.00: the magic, the size, the global memory prefix and
 * the whole system area in sized sections, then the names and the flag
 * groups, the group registered last first; longer names and more than
 * 0x800 groups are kept.  It reads the file back, refuses a wrong size
 * field, and reads layout B (1.553's file) and the widened layout A
 * OpenBGI wrote for 1.616 on before.  A failure means the GDB 3.00
 * layout, its checks or the reading of the older layouts changed. */
static void TestGdb3(OpenBGI_t* o)
{
	uint8_t* db;
	uint32_t n = 0, v = 0, i;
	char name[64];
	printf("gdb 3.00\n");
	Fresh(o, "1.636");
	Fill(o);
	CHECK(GName_Add(o, "a name much longer than the old slots allow, 40+"));
	CHECK(GFlag_RegisterGroup(o, "a group name longer than twenty-three", 9));
	CHECK(GFlag_SetBit(o, "a group name longer than twenty-three", 8, 1) == FLAG_OK);
	for(i = 0; i < 0x900; i++)
	{
		sprintf(name, "g%u", i);
		CHECK(GFlag_RegisterGroup(o, name, 1 + i % 17));
	}
	CHECK(GlobalDB_Save(o));
	db = Decoded(o, &n);
	CHECK(db && memcmp(db, "BURIKO GDB 3.00", 16) == 0 && n > 0x100430);
	if(db)
	{
		uint32_t off = 0x420 + 4 + 0x100000;
		CHECK(*(uint32_t*)(db + 0x10) == n && *(uint32_t*)(db + 0x1c) == 0x400 && *(uint32_t*)(db + 0x420) == 0x100000);
		CHECK(*(uint32_t*)(db + off) == 3 && strcmp((char*)db + off + 4, "alpha") == 0);
		off += 4 + 6 + 5 + (uint32_t)strlen("a name much longer than the old slots allow, 40+") + 1;
		CHECK(*(uint32_t*)(db + off) == 0x903 && strcmp((char*)db + off + 4, "g2303") == 0); // the latest group first
	}
	Spoil(o);
	CHECK(GlobalDB_Load(o) == 0 && Filled(o) && GName_Exists(o, "a name much longer than the old slots allow, 40+"));
	CHECK(GFlag_GetBit(o, &v, "a group name longer than twenty-three", 8) == FLAG_OK && v);
	CHECK(GFlag_GetBit(o, &v, "g2303", 0) == FLAG_OK && FlagMgr_Count(o->sysobj.flagMgr) == 0x903);
	CHECK(strcmp(FlagMgr_Group(o->sysobj.flagMgr, 0)->name, "route") == 0); // the registration order rebuilt
	if(db)
	{
		*(uint32_t*)(db + 0x10) = n + 1;
		WriteDecoded(o, db, n);
		CHECK(GlobalDB_Load(o) == 0x80000002u);
	}
	BGI_Free(db);

	// a layout B file, as 1.553 wrote it: the system area's rest cleared
	Fresh(o, "1.553");
	Fill(o);
	CHECK(GlobalDB_Save(o));
	Fresh(o, "1.636");
	memset(o->mem.sysArea, 0x55, o->mem.sysAreaSize);
	CHECK(GlobalDB_Load(o) == 0 && GName_Exists(o, "alpha") && o->mem.sysArea[0x40000] == 0 && o->mem.sysArea[0xFFFFF] == 0);
	CHECK(o->mem.sysArea[0x3FFFF] == (uint8_t)(0x3FFFF * 13 + (0x3FFFF >> 12)));
	CHECK(GFlag_GetBit(o, &v, "cg", 299) == FLAG_OK && v);

	// the widened A of OpenBGI's earlier files for 1.616 on
	db = (uint8_t*)BGI_Calloc(0x130408 + 3);
	for(i = 0; i < 0x100000; i++)
		db[0x408 + i] = (uint8_t)(i * 13 + (i >> 12));
	for(i = 0; i < 0x400; i++)
		db[8 + i] = (uint8_t)(i * 7 + 1);
	strcpy((char*)db + 0x100408, "alpha");
	strcpy((char*)db + 0x100428, "beta");
	strcpy((char*)db + 0x120408, "route");
	*(uint32_t*)(db + 0x120408 + 0x18) = 20;
	db[0x130408] = 0x10; // bit 3
	WriteDecoded(o, db, 0x130408 + 3);
	BGI_Free(db);
	Fresh(o, "1.636");
	CHECK(GlobalDB_Load(o) == 0 && GName_Exists(o, "beta") && GFlag_GetBit(o, &v, "route", 3) == FLAG_OK && v);
	CHECK(o->mem.sysArea[0xFFFFF] == (uint8_t)(0xFFFFF * 13 + (0xFFFFF >> 12)));
}

/* "81 80": a GDB 3.00 image (1.636's file) merged into another state -
 * its names join the live ones, a group of its is OR'ed into the live
 * one of that name and grows it, a group only it has is added; its
 * global memory prefix and system area go to the buffers given, the
 * live ones stay; the sizes are always written.  A layout B image is
 * refused.  A failure means the merge or its result codes changed. */
static void TestMerge(OpenBGI_t* o)
{
	FILE* f;
	uint8_t* file = NULL;
	uint8_t* glob = (uint8_t*)BGI_Alloc(0x400);
	uint8_t* sys = (uint8_t*)BGI_Alloc(0x100000);
	long size = 0;
	uint32_t gs = 0, ss = 0, v = 0;
	printf("merge\n");
	Fresh(o, "1.636");
	Fill(o);
	CHECK(GlobalDB_Save(o));
	f = fopen(GDB_PATH, "rb");
	if(f)
	{
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		fseek(f, 0, SEEK_SET);
		file = (uint8_t*)BGI_Alloc((size_t)size);
		if(fread(file, 1, (size_t)size, f) != (size_t)size)
			size = 0;
		fclose(f);
	}
	Fresh(o, "1.636");
	CHECK(GName_Add(o, "gamma"));
	CHECK(GFlag_RegisterGroup(o, "route", 8) && GFlag_SetBit(o, "route", 5, 1) == FLAG_OK);
	memset(o->mem.globalMem, 0x11, 0x400);
	memset(glob, 0x77, 0x400);
	CHECK(file && GlobalDB_MergeImage(o, file, glob, &gs, NULL, &ss) == 0 && gs == 0x400 && ss == 0x100000);
	CHECK(glob[0] == 1 && glob[0x3FF] == (uint8_t)(0x3FF * 7 + 1) && o->mem.globalMem[0] == 0x11);
	CHECK(GName_Exists(o, "alpha") && GName_Exists(o, "beta") && GName_Exists(o, "gamma"));
	CHECK(GFlag_GetBit(o, &v, "route", 5) == FLAG_OK && v && GFlag_GetBit(o, &v, "route", 3) == FLAG_OK && v);
	CHECK(GFlag_GetBit(o, &v, "route", 19) == FLAG_OK && !v); // grown to the image's 20 bits
	CHECK(GFlag_GetBit(o, &v, "cg", 299) == FLAG_OK && v);
	CHECK(file && GlobalDB_MergeImage(o, file, NULL, &gs, sys, &ss) == 0 && sys[0x12345] == (uint8_t)(0x12345 * 13 + (0x12345 >> 12)));
	{
		static const char junk[0x40] = "not an image";
		CHECK(GlobalDB_MergeImage(o, junk, NULL, &gs, NULL, &ss) == 0x80000002u);
	}
	BGI_Free(file);
	Fresh(o, "1.553");
	Fill(o);
	CHECK(GlobalDB_Save(o));
	file = NULL;
	f = fopen(GDB_PATH, "rb");
	if(f)
	{
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		fseek(f, 0, SEEK_SET);
		file = (uint8_t*)BGI_Alloc((size_t)size);
		if(fread(file, 1, (size_t)size, f) != (size_t)size)
			size = 0;
		fclose(f);
	}
	Fresh(o, "1.636");
	CHECK(file && GlobalDB_MergeImage(o, file, NULL, &gs, NULL, &ss) == 0x80000002u);
	BGI_Free(file);
	BGI_Free(glob);
	BGI_Free(sys);
}

/* Until 1.599 a group name has 1 to 23 characters and the manager holds
 * 0x800 groups; from 1.616 on neither limit applies, but a group still
 * needs bits.  A failure means a generation's limits changed. */
static void TestFlags(OpenBGI_t* o)
{
	char name[16];
	int i, ok = 1;
	printf("flags\n");
	Fresh(o, "1.553");
	CHECK(FlagMgr_Register(o, o->sysobj.flagMgr, "", 8) == FLAG_ERR_NAME);
	CHECK(FlagMgr_Register(o, o->sysobj.flagMgr, "123456789012345678901234", 8) == FLAG_ERR_NAME);
	for(i = 0; i < 0x800; i++)
	{
		sprintf(name, "g%d", i);
		if(FlagMgr_Register(o, o->sysobj.flagMgr, name, 8) != FLAG_OK)
			ok = 0;
	}
	CHECK(ok && FlagMgr_Register(o, o->sysobj.flagMgr, "one more", 8) == FLAG_ERR_FULL);
	CHECK(FlagMgr_Register(o, o->sysobj.flagMgr, "g5", 16) == FLAG_OK); // a resize still works
	Fresh(o, "1.636");
	CHECK(FlagMgr_Register(o, o->sysobj.flagMgr, "", 8) == FLAG_OK);
	CHECK(FlagMgr_Register(o, o->sysobj.flagMgr, "123456789012345678901234", 8) == FLAG_OK);
	CHECK(FlagMgr_Register(o, o->sysobj.flagMgr, "x", 0) == FLAG_ERR_BITS);
}

// run every group; exit status 1 when any check failed
int main(void)
{
	OpenBGI_t* o = OpenBGI_New(OS_Host()); // the engine whose code the test runs
	if(!o)
		return 1;
	OS_Init();
	o->display.fullscreen = 1; // (the window rectangle without a window: 0, 0)
	o->mem.globalMem = NULL;
	SetGlobalMemSize(o, 4);
	o->mem.sysArea = (uint8_t*)BGI_Calloc(0x100000);
	strcpy(o->files.altDir, "bin/");
	o->files.baseDir[0] = 0;
	TestLayoutA(o);
	TestLayoutB(o);
	TestGdb3(o);
	TestMerge(o);
	TestFlags(o);
	remove(GDB_PATH);
	SysArea_Shutdown(o);
	OS_Shutdown();
	if(gFails)
	{
		printf("%d failure(s)\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
