/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * database.c - the global database file "BGI.gdb" (inc/bgi/sysobj/database.h)
 *
 * Decoded layout of the file (SDC-encoded on disk):
 *
 *     +000000  window left, top (screen coordinates of the window rectangle)
 *     +000008  the first 0x400 bytes of global memory
 *     +000408  the system area (0x40000 bytes; 0x100000 from 1.616 on, which
 *              moves the sections below up by 0xC0000)
 *     +040408  the name table (GNAME_COUNT * GNAME_SIZE)
 *     +060408  GDB_MAXGROUPS flag-group records {name[0x18], bits, dataOffset}
 *     +070408  the concatenated flag bytes, dataOffset relative to here
 *
 * 1.616 on write a differently sectioned file in the original; this
 * layout, widened to their system area, is what the reimplementation
 * reads and writes for every build.
 */
#include "bgi/sysobj/database.h"
#include "bgi/sysobj/names.h"
#include "bgi/sysobj/flags.h"
#include "bgi/sysobj/strtables.h"
#include "bgi/codec.h"
#include "bgi/file.h"
#include "bgi/error.h"
#include "bgi/display.h"
#include "bgi/sys.h"
#include "bgi/msg.h"
#include "bgi/os.h"
#include "bgi/version.h"

#define GDB_FILE      "BGI.gdb"
#define GDB_GLOBALS   0x008                        // offset of the global-memory prefix (after the window position)
#define GDB_SYSAREA   0x408                        // offset of the system area
#define GDB_NAMES     (GDB_SYSAREA + gSysAreaSize) // offset of the name table
#define GDB_FLAGS     (GDB_NAMES + 0x20000)        // offset of the flag-group records
#define GDB_FLAGDATA  (GDB_FLAGS + 0x10000)        // offset of the flag bytes
#define GDB_MAXGROUPS 0x800                        // flag-group records in the file (and groups the manager holds)
#define GDB_WORK      0x2000000                    // the work buffer (the decoded file never exceeds it)

typedef struct GdbFlagRec // one flag-group record of the file
{
	char name[0x18];     // the group's name, NUL-terminated
	uint32_t bits;       // flags in the group; 0 marks an unused record
	uint32_t dataOffset; // of the bits, from the start of the flag bytes
} GdbFlagRec_t;

/* Where the database lives: next to the game until 1.69 build 444; from
 * build 451 on in the directory "80 39" set (gAltDir), which is the user's
 * data folder in those games. */
static void GdbPath(char* out)
{
	if(gEngine->gen >= GEN_1_69_451)
		sprintf(out, "%s%s", gAltDir, GDB_FILE);
	else
		strcpy(out, GDB_FILE);
}

/* Read the encoded file into a fresh buffer (*size bytes); NULL and size
 * 0 when there is none.  The pre-451 builds go through the file set's
 * loader, the later ones through the file layer directly (the loader only
 * joins a bare name with its own directories). */
static uint8_t* GdbReadFile(uint32_t* size)
{
	char path[0x210];
	uint8_t* file;
	GdbPath(path);
	if(gEngine->gen < GEN_1_69_451)
	{
		*size = GetFileSize_(NULL, GDB_FILE);
		if(*size == 0)
			return NULL;
		file = (uint8_t*)BGI_Alloc(*size);
		LoadFile(file, NULL, GDB_FILE);
		return file;
	}
	else
	{
		File_t f;
		File_Ctor(&f);
		if(!File_OpenRead(&f, path))
		{
			File_Dtor(&f);
			*size = 0;
			return NULL;
		}
		*size = File_Size(&f);
		file = (uint8_t*)BGI_Alloc(*size ? *size : 1);
		*size = File_Read(&f, file, *size);
		File_Close(&f);
		File_Dtor(&f);
		return file;
	}
}

/* Assemble the decoded image from the live state, encode it and write the
 * file.  When the encoder fails the raw image goes into BGIError.txt and
 * an error box is shown; the (empty) file is still written.  1 when the
 * file was written completely. */
int GlobalDB_Save(void)
{
	uint8_t* db = (uint8_t*)BGI_Alloc(GDB_WORK);
	uint8_t* enc;
	GdbFlagRec_t* rec = (GdbFlagRec_t*)(db + GDB_FLAGS);
	uint32_t dataOff = 0, total, encSize, written, i;
	int wx, wy, ww, wh;

	OS_WindowGetRect(&wx, &wy, &ww, &wh); // the window rectangle's left / top
	memcpy(db + 0, &wx, 4);
	memcpy(db + 4, &wy, 4);
	memcpy(db + GDB_GLOBALS, gGlobalMem, 0x400);
	memcpy(db + GDB_SYSAREA, gSysArea, gSysAreaSize);
	GName_ToSlots((char(*)[GNAME_SIZE])(db + GDB_NAMES));

	for(i = 0; i < GDB_MAXGROUPS; i++, rec++)
	{
		const FlagGroup_t* g = i < FlagMgr_Capacity(gFlagMgr) ? FlagMgr_Group(gFlagMgr, i) : NULL;
		memset(rec, 0, sizeof *rec);
		if(g && g->bits != 0)
		{
			uint32_t n = FlagBytes(g->bits);
			strcpy(rec->name, g->name);
			rec->bits = g->bits;
			rec->dataOffset = dataOff;
			memcpy(db + GDB_FLAGDATA + dataOff, g->data, n);
			dataOff += n;
		}
	}
	total = GDB_FLAGDATA + dataOff;

	enc = (uint8_t*)BGI_Alloc(total * 2);
	encSize = SdcEncode(enc, db, total);
	if(encSize == 0)
	{
		WriteTextFile("BGIError.txt", db, total);
		ShowErrorBox(MSG_SDC_ENCODE_FAILED);
	}
	{
		char path[0x210];
		GdbPath(path);
		written = (uint32_t)WriteTextFile(path, enc, encSize);
	}
	BGI_Free(enc);
	BGI_Free(db);
	return written == encSize;
}

/* Read the file and restore the global memory prefix, the system area,
 * the names and the flag groups from it.  The decoded size must cover the
 * flag records and match the flag bytes they describe, else the file
 * counts as damaged (what was decoded so far stays applied). */
uint32_t GlobalDB_Load(void)
{
	uint32_t size;
	uint8_t *db, *file = GdbReadFile(&size);
	uint32_t n, dataTotal = 0, result = 0x80000002u;
	const GdbFlagRec_t* rec;
	int i;

	if(size == 0)
	{
		BGI_Free(file);
		return 0x80000001u;
	}
	db = (uint8_t*)BGI_Alloc(GDB_WORK);
	n = SdcDecode(db, file);
	BGI_Free(file);

	if(n >= GDB_FLAGDATA)
	{
		memcpy(gGlobalMem, db + GDB_GLOBALS, 0x400);
		memcpy(gSysArea, db + GDB_SYSAREA, gSysAreaSize);
		GName_FromSlots((const char(*)[GNAME_SIZE])(db + GDB_NAMES));
		FlagMgr_Clear(gFlagMgr);
		rec = (const GdbFlagRec_t*)(db + GDB_FLAGS);
		for(i = 0; i < GDB_MAXGROUPS; i++, rec++)
		{
			if(rec->bits != 0)
			{
				FlagMgr_RegisterWithData(gFlagMgr, rec->name, rec->bits, db + GDB_FLAGDATA + rec->dataOffset);
				dataTotal += FlagBytes(rec->bits);
			}
		}
		if(n == GDB_FLAGDATA + dataTotal)
			result = 0;
	}
	BGI_Free(db);
	return result;
}

/* The saved window position, accepted only while a window of the
 * picture size plus its frame still fits on the screen from there. */
int SavedWindowPos_Load(int* x, int* y)
{
	uint32_t size;
	uint8_t *db, *file = GdbReadFile(&size);
	uint32_t n;
	int ok = 0;

	if(size == 0)
	{
		BGI_Free(file);
		return 0;
	}
	db = (uint8_t*)BGI_Alloc(GDB_WORK);
	n = SdcDecode(db, file);
	BGI_Free(file);
	if(n >= GDB_FLAGDATA)
	{
		int sw, sh, fw, fh, px, py, maxX, maxY, winW, winH;
		OS_ScreenSize(&sw, &sh);
		OS_FrameMetrics(&fw, &fh);
		Display_GetPictureSize(&winW, &winH);
		maxX = sw - winW - fw;
		maxY = sh - winH - fh;
		memcpy(&px, db + 0, 4);
		memcpy(&py, db + 4, 4);
		if(px >= 0 && px <= maxX && py >= 0 && py <= maxY)
		{
			*x = px;
			*y = py;
			ok = 1;
		}
	}
	BGI_Free(db);
	return ok;
}

// Clear the system area, create the flag manager, empty the names (both storages, names.h).
void SysArea_Init(void)
{
	memset(gSysArea, 0, gSysAreaSize);
	gFlagMgr = FlagMgr_New(GDB_MAXGROUPS);
	memset(gNameTable, 0, sizeof gNameTable);
	StrTab_Remove(GNAME_TABLE);
}

void SysArea_Shutdown(void)
{
	if(gFlagMgr)
	{
		FlagMgr_Delete(gFlagMgr);
		gFlagMgr = NULL;
	}
}
