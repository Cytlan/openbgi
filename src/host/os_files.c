/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_files.c - the OS layer's files and registry as the host's tables
 *              (OpenBGI_Files_t and OpenBGI_Registry_t of openbgi.h,
 *              for OS_Host; declared in host/os_common.h)
 *
 * The back ends keep their own file code (OS_File*, OS_Find*, the drives,
 * OS_Native*, OS_Reg*, ...); the tables hand it to the engine as it is.
 * A stream of the tables is either a game file (an OsFile_t: positions
 * are 32-bit, as in the original) or a native one (a stdio stream, sought
 * with OS_NativeSeek).  On the page (BGI_WASM) the native paths are the
 * game's own: a native file is read through the game's files and none is
 * written, as the back end's OS_ReadNativeFile and OS_WriteNativeFile do.
 */
#include "host/os.h"
#include "host/os_common.h"
#include "bgi/prep.h"

#include <stdio.h>
#include <stdlib.h>

// ---- streams --------------------------------------------------------------------------

typedef struct HostStream
{
	OsFile_t* os;  // a game file; NULL for a native one
	FILE* fp;      // a native file; NULL for a game one
	uint64_t size; // fp's size: what it had when opened, or the end of what was written since
	uint64_t pos;  // fp's position
} HostStream_t;

static HostStream_t* NewStream(OsFile_t* os, FILE* fp, uint64_t size)
{
	HostStream_t* s;
	if(!os && !fp)
		return NULL;
	s = (HostStream_t*)calloc(1, sizeof *s);
	if(!s)
	{
		if(os)
			OS_FileClose(os);
		if(fp)
			fclose(fp);
		return NULL;
	}
	s->os = os;
	s->fp = fp;
	s->size = size;
	return s;
}

static void* FilesOpen(void* user, const char* path, int mode)
{
	BGI_UNUSED(user);
	switch(mode)
	{
		case OPENBGI_OPEN_READ: return NewStream(OS_FileOpenRead(path), NULL, 0);
		case OPENBGI_OPEN_CREATE: return NewStream(OS_FileCreate(path), NULL, 0);
		case OPENBGI_OPEN_WRITE: return NewStream(OS_FileOpenWrite(path), NULL, 0);
		default: return NULL;
	}
}

static void FilesClose(void* user, void* f)
{
	HostStream_t* s = (HostStream_t*)f;
	BGI_UNUSED(user);
	if(!s)
		return;
	if(s->os)
		OS_FileClose(s->os);
	if(s->fp)
		fclose(s->fp);
	free(s);
}

static uint32_t FilesRead(void* user, void* f, void* buf, uint32_t n)
{
	HostStream_t* s = (HostStream_t*)f;
	uint32_t got;
	BGI_UNUSED(user);
	if(s->os)
		return OS_FileRead(s->os, buf, n);
	got = (uint32_t)fread(buf, 1, n, s->fp);
	s->pos += got;
	return got;
}

static uint32_t FilesWrite(void* user, void* f, const void* buf, uint32_t n)
{
	HostStream_t* s = (HostStream_t*)f;
	uint32_t put;
	BGI_UNUSED(user);
	if(s->os)
		return OS_FileWrite(s->os, buf, n);
	put = (uint32_t)fwrite(buf, 1, n, s->fp);
	s->pos += put;
	if(s->pos > s->size)
		s->size = s->pos;
	return put;
}

static int FilesSeek(void* user, void* f, uint64_t pos)
{
	HostStream_t* s = (HostStream_t*)f;
	BGI_UNUSED(user);
	if(s->os)
		return pos <= 0xffffffffu && OS_FileSeek(s->os, (uint32_t)pos);
	if(!OS_NativeSeek(s->fp, pos))
		return 0;
	s->pos = pos;
	return 1;
}

static uint64_t FilesSize(void* user, void* f)
{
	HostStream_t* s = (HostStream_t*)f;
	BGI_UNUSED(user);
	return s->os ? OS_FileSize(s->os) : s->size;
}

static int FilesGetTime(void* user, void* f, uint32_t* lo, uint32_t* hi)
{
	HostStream_t* s = (HostStream_t*)f;
	BGI_UNUSED(user);
	return s->os && OS_FileGetTime(s->os, lo, hi);
}

static int FilesSetTime(void* user, void* f, uint32_t lo, uint32_t hi)
{
	HostStream_t* s = (HostStream_t*)f;
	BGI_UNUSED(user);
	return s->os && OS_FileSetTime(s->os, lo, hi);
}

// ---- the game's files by path -------------------------------------------------------------

static uint32_t FilesAttrs(void* user, const char* path)
{
	BGI_UNUSED(user);
	return OS_FileAttrs(path);
}

static int FilesSetAttrs(void* user, const char* path, uint32_t attrs)
{
	BGI_UNUSED(user);
	return OS_FileSetAttrs(path, attrs);
}

static int FilesRemove(void* user, const char* path)
{
	BGI_UNUSED(user);
	return OS_FileDelete(path);
}

static int FilesMove(void* user, const char* from, const char* to)
{
	BGI_UNUSED(user);
	return OS_FileMove(from, to);
}

static int FilesCopy(void* user, const char* from, const char* to, int failIfExists)
{
	BGI_UNUSED(user);
	return OS_FileCopy(from, to, failIfExists);
}

static int FilesMakeDir(void* user, const char* path)
{
	BGI_UNUSED(user);
	return OS_DirCreate(path);
}

static int FilesRemoveDir(void* user, const char* path)
{
	BGI_UNUSED(user);
	return OS_DirRemove(path);
}

static void* FilesFindFirst(void* user, const char* pattern, OpenBGI_FindData_t* out)
{
	BGI_UNUSED(user);
	return OS_FindFirst(pattern, out);
}

static int FilesFindNext(void* user, void* find, OpenBGI_FindData_t* out)
{
	BGI_UNUSED(user);
	return OS_FindNext((OsFind_t*)find, out);
}

static void FilesFindClose(void* user, void* find)
{
	BGI_UNUSED(user);
	OS_FindClose((OsFind_t*)find);
}

static int FilesSetCurrentDir(void* user, const char* dir)
{
	BGI_UNUSED(user);
	return OS_SetCurrentDir(dir);
}

static void FilesExeDir(void* user, char* buf, size_t n)
{
	BGI_UNUSED(user);
	OS_ExeDir(buf, n);
}

static uint32_t FilesDriveType(void* user, const char* root)
{
	BGI_UNUSED(user);
	return OS_DriveType(root);
}

static uint32_t FilesLogicalDrives(void* user, char* buf, uint32_t n)
{
	BGI_UNUSED(user);
	return OS_LogicalDrives(buf, n);
}

static int FilesVolumeReady(void* user, const char* root)
{
	BGI_UNUSED(user);
	return OS_VolumeReady(root);
}

static int FilesMediaPresent(void* user, char drive)
{
	BGI_UNUSED(user);
	return OS_MediaPresent(drive);
}

static int FilesSpecialFolder(void* user, int csidl, char* buf, size_t n)
{
	BGI_UNUSED(user);
	return OS_SpecialFolder(csidl, buf, n);
}

static int FilesWindowsDir(void* user, char* buf, size_t n)
{
	BGI_UNUSED(user);
	return OS_WindowsDir(buf, n);
}

// ---- native paths -------------------------------------------------------------------------

static void* FilesNativeOpen(void* user, const char* path, int mode)
{
	BGI_UNUSED(user);
#if defined(BGI_WASM)
	return mode == OPENBGI_OPEN_READ ? NewStream(OS_FileOpenRead(path), NULL, 0) : NULL;
#else
	if(mode == OPENBGI_OPEN_READ)
	{
		OsNativeStat_t st;
		if(OS_NativeStat(path, &st) != OS_NATIVE_FILE)
			return NULL;
		return NewStream(NULL, OS_NativeOpen(path, "rb"), st.size);
	}
	if(mode == OPENBGI_OPEN_CREATE)
		return NewStream(NULL, OS_NativeOpen(path, "wb"), 0);
	return NULL;
#endif
}

static int FilesNativeStat(void* user, const char* path, OpenBGI_NativeStat_t* out)
{
	BGI_UNUSED(user);
	return OS_NativeStat(path, out);
}

static int FilesNativeWritable(void* user, const char* path)
{
	BGI_UNUSED(user);
	return OS_NativeWritable(path);
}

static int FilesNativeMkdir(void* user, const char* path)
{
	BGI_UNUSED(user);
	return OS_NativeMkdir(path);
}

static int FilesNativeRemove(void* user, const char* path)
{
	BGI_UNUSED(user);
	return OS_NativeRemove(path);
}

static int FilesNativeList(void* user, const char* dir, void (*fn)(void* ctx, const char* name, int isDir), void* ctx)
{
	BGI_UNUSED(user);
	return OS_NativeList(dir, fn, ctx);
}

static void FilesNativeAbsolute(void* user, const char* path, char* out, size_t n)
{
	BGI_UNUSED(user);
	OS_NativeAbsolute(path, out, n);
}

static void FilesToUtf8(void* user, const char* native, char* out, size_t n)
{
	BGI_UNUSED(user);
	OS_NativeToUtf8(native, out, n);
}

static void FilesFromUtf8(void* user, const char* utf8, char* out, size_t n)
{
	BGI_UNUSED(user);
	OS_Utf8ToNative(utf8, out, n);
}

static void FilesGameDir(void* user, char* buf, size_t n)
{
	BGI_UNUSED(user);
	OS_GameDir(buf, n);
}

static void (*gBeforeGameDir)(void); // OsCommon_BeforeGameDir

void OsCommon_BeforeGameDir(void (*fn)(void))
{
	gBeforeGameDir = fn;
}

static int FilesSetGameDir(void* user, const char* dir)
{
	BGI_UNUSED(user);
	if(gBeforeGameDir)
		gBeforeGameDir();
	return OS_SetGameDir(dir);
}

static void FilesUserDataDir(void* user, char* buf, size_t n)
{
	BGI_UNUSED(user);
	OS_UserDataDir(buf, n);
}

static void FilesBinaryDir(void* user, char* buf, size_t n)
{
	BGI_UNUSED(user);
	OS_BinaryDir(buf, n);
}

static const OpenBGI_Files_t kFiles = {
	sizeof(OpenBGI_Files_t),
	FilesOpen,
	FilesClose,
	FilesRead,
	FilesWrite,
	FilesSeek,
	FilesSize,
	FilesGetTime,
	FilesSetTime,
	FilesAttrs,
	FilesSetAttrs,
	FilesRemove,
	FilesMove,
	FilesCopy,
	FilesMakeDir,
	FilesRemoveDir,
	FilesFindFirst,
	FilesFindNext,
	FilesFindClose,
	FilesSetCurrentDir,
	FilesExeDir,
	FilesDriveType,
	FilesLogicalDrives,
	FilesVolumeReady,
	FilesMediaPresent,
	FilesSpecialFolder,
	FilesWindowsDir,
	OS_NATIVE_SEP,
#if defined(_WIN32)
	1, // the Windows back end's native paths are the engine's Shift-JIS ones
#else
	0,
#endif
	FilesNativeOpen,
	FilesNativeStat,
	FilesNativeMkdir,
	FilesNativeRemove,
	FilesNativeList,
	FilesNativeAbsolute,
	FilesToUtf8,
	FilesFromUtf8,
	FilesGameDir,
	FilesSetGameDir,
	FilesUserDataDir,
	FilesBinaryDir,
	FilesNativeWritable,
};

const OpenBGI_Files_t* OsCommon_Files(void)
{
	return &kFiles;
}

// ---- the registry -------------------------------------------------------------------------

static int RegistryRead(void* user, const char* key, const char* value, char* buf, size_t n)
{
	BGI_UNUSED(user);
	return OS_RegReadString(key, value, buf, n);
}

static int RegistryCreateKey(void* user, const char* key, const char* cls)
{
	BGI_UNUSED(user);
	return OS_RegCreateKey(key, cls);
}

static int RegistryWrite(void* user, const char* key, const char* value, const char* s)
{
	BGI_UNUSED(user);
	return OS_RegWriteString(key, value, s);
}

static int RegistryDeleteKey(void* user, const char* key)
{
	BGI_UNUSED(user);
	return OS_RegDeleteKey(key);
}

static const OpenBGI_Registry_t kRegistry = {sizeof(OpenBGI_Registry_t), RegistryRead, RegistryCreateKey, RegistryWrite, RegistryDeleteKey};

const OpenBGI_Registry_t* OsCommon_Registry(void)
{
	return &kRegistry;
}

// ---- the preprocessor's includes ----------------------------------------------------------

static uint8_t* PrepRead(void* ctx, const char* path, uint32_t* size)
{
	BGI_UNUSED(ctx);
	return OS_ReadNativeFile(path, size);
}

static void PrepAbsolute(void* ctx, const char* path, char* out, size_t n)
{
	BGI_UNUSED(ctx);
	OS_NativeAbsolute(path, out, n);
}

static void PrepFromUtf8(void* ctx, const char* utf8, char* out, size_t n)
{
	BGI_UNUSED(ctx);
	OS_Utf8ToNative(utf8, out, n);
}

void OsCommon_PrepFiles(PrepIncludes_t* inc)
{
	inc->read = PrepRead;
	inc->absolute = PrepAbsolute;
	inc->fromUtf8 = PrepFromUtf8;
	inc->sep = OS_NATIVE_SEP;
#if defined(_WIN32)
	inc->sjis = 1; // the Windows back end's native paths are the engine's Shift-JIS ones
	inc->foldCase = 1;
#else
	inc->sjis = 0;
	inc->foldCase = 0;
#endif
}
