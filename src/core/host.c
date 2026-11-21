/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * host.c - the host's services as the engine calls them (src/include/bgi/host.h)
 *
 * Thin calls of the tables in OpenBGI_Host_t (openbgi.h), with the
 * failure a missing table or member stands for.  The whole native files
 * are read and written here, over the host's streams.
 */
#include "bgi/host.h"
#include "bgi/core.h"
#include "bgi/sjis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int Host_Take(void* own, size_t ownSize, const void* from)
{
	uint32_t n;
	memset(own, 0, ownSize);
	memcpy(&n, from, sizeof n);
	if(n < sizeof n)
		return 0;
	memcpy(own, from, n < ownSize ? n : ownSize);
	n = (uint32_t)ownSize;
	memcpy(own, &n, sizeof n);
	return 1;
}

void Host_Give(void* to, const void* from, size_t fromSize)
{
	uint32_t n;
	memcpy(&n, to, sizeof n);
	if(n <= sizeof n)
		return;
	if(n > fromSize)
	{ // (a host built with a newer header: what this engine does not know of is zero)
		memset((char*)to + fromSize, 0, n - fromSize);
		n = (uint32_t)fromSize;
	}
	memcpy((char*)to + sizeof n, (const char*)from + sizeof n, n - sizeof n);
}

// the files' table, NULL without one
static const OpenBGI_Files_t* Files(OpenBGI_t* o)
{
	return o->host.files;
}

static const OpenBGI_Registry_t* Registry(OpenBGI_t* o)
{
	return o->host.registry;
}

// an empty string into a buffer (when it has room for one)
static void Empty(char* buf, size_t n)
{
	if(n)
		buf[0] = 0;
}

// ---- the game's files ---------------------------------------------------------------------------

HostFile_t* Host_FileOpen(OpenBGI_t* o, const char* path, int mode)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->open ? (HostFile_t*)f->open(o->host.user, path, mode) : NULL;
}

void Host_FileClose(OpenBGI_t* o, HostFile_t* h)
{
	const OpenBGI_Files_t* f = Files(o);
	if(h && f && f->close)
		f->close(o->host.user, h);
}

uint32_t Host_FileRead(OpenBGI_t* o, HostFile_t* h, void* buf, uint32_t n)
{
	const OpenBGI_Files_t* f = Files(o);
	return h && f && f->read ? f->read(o->host.user, h, buf, n) : 0;
}

uint32_t Host_FileWrite(OpenBGI_t* o, HostFile_t* h, const void* buf, uint32_t n)
{
	const OpenBGI_Files_t* f = Files(o);
	return h && f && f->write ? f->write(o->host.user, h, buf, n) : 0;
}

int Host_FileSeek(OpenBGI_t* o, HostFile_t* h, uint64_t pos)
{
	const OpenBGI_Files_t* f = Files(o);
	return h && f && f->seek ? f->seek(o->host.user, h, pos) : 0;
}

uint64_t Host_FileSize(OpenBGI_t* o, HostFile_t* h)
{
	const OpenBGI_Files_t* f = Files(o);
	return h && f && f->length ? f->length(o->host.user, h) : 0;
}

int Host_FileGetTime(OpenBGI_t* o, HostFile_t* h, uint32_t* lo, uint32_t* hi)
{
	const OpenBGI_Files_t* f = Files(o);
	return h && f && f->getTime ? f->getTime(o->host.user, h, lo, hi) : 0;
}

int Host_FileSetTime(OpenBGI_t* o, HostFile_t* h, uint32_t lo, uint32_t hi)
{
	const OpenBGI_Files_t* f = Files(o);
	return h && f && f->setTime ? f->setTime(o->host.user, h, lo, hi) : 0;
}

uint32_t Host_FileAttrs(OpenBGI_t* o, const char* path)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->attrs ? f->attrs(o->host.user, path) : OPENBGI_INVALID_ATTRS;
}

int Host_FileSetAttrs(OpenBGI_t* o, const char* path, uint32_t attrs)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->setAttrs ? f->setAttrs(o->host.user, path, attrs) : 0;
}

int Host_FileDelete(OpenBGI_t* o, const char* path)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->remove ? f->remove(o->host.user, path) : 0;
}

int Host_FileMove(OpenBGI_t* o, const char* from, const char* to)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->move ? f->move(o->host.user, from, to) : 0;
}

int Host_FileCopy(OpenBGI_t* o, const char* from, const char* to, int failIfExists)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->copy ? f->copy(o->host.user, from, to, failIfExists) : 0;
}

int Host_DirCreate(OpenBGI_t* o, const char* path)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->makeDir ? f->makeDir(o->host.user, path) : 0;
}

int Host_DirRemove(OpenBGI_t* o, const char* path)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->removeDir ? f->removeDir(o->host.user, path) : 0;
}

HostFind_t* Host_FindFirst(OpenBGI_t* o, const char* pattern, OpenBGI_FindData_t* out)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->findFirst ? (HostFind_t*)f->findFirst(o->host.user, pattern, out) : NULL;
}

int Host_FindNext(OpenBGI_t* o, HostFind_t* h, OpenBGI_FindData_t* out)
{
	const OpenBGI_Files_t* f = Files(o);
	return h && f && f->findNext ? f->findNext(o->host.user, h, out) : 0;
}

void Host_FindClose(OpenBGI_t* o, HostFind_t* h)
{
	const OpenBGI_Files_t* f = Files(o);
	if(h && f && f->findClose)
		f->findClose(o->host.user, h);
}

int Host_SetCurrentDir(OpenBGI_t* o, const char* dir)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->setCurrentDir ? f->setCurrentDir(o->host.user, dir) : 0;
}

void Host_ExeDir(OpenBGI_t* o, char* buf, size_t n)
{
	const OpenBGI_Files_t* f = Files(o);
	Empty(buf, n);
	if(f && f->exeDir)
		f->exeDir(o->host.user, buf, n);
}

uint32_t Host_DriveType(OpenBGI_t* o, const char* root)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->driveType ? f->driveType(o->host.user, root) : OPENBGI_DRIVE_UNKNOWN;
}

uint32_t Host_LogicalDrives(OpenBGI_t* o, char* buf, uint32_t n)
{
	const OpenBGI_Files_t* f = Files(o);
	if(n >= 2)
		buf[0] = buf[1] = 0;
	return f && f->logicalDrives ? f->logicalDrives(o->host.user, buf, n) : 0;
}

int Host_VolumeReady(OpenBGI_t* o, const char* root)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->volumeReady ? f->volumeReady(o->host.user, root) : 0;
}

int Host_MediaPresent(OpenBGI_t* o, char drive)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->mediaPresent ? f->mediaPresent(o->host.user, drive) : 0;
}

int Host_SpecialFolder(OpenBGI_t* o, int csidl, char* buf, size_t n)
{
	const OpenBGI_Files_t* f = Files(o);
	Empty(buf, n);
	return f && f->specialFolder ? f->specialFolder(o->host.user, csidl, buf, n) : 0;
}

int Host_WindowsDir(OpenBGI_t* o, char* buf, size_t n)
{
	const OpenBGI_Files_t* f = Files(o);
	Empty(buf, n);
	return f && f->windowsDir ? f->windowsDir(o->host.user, buf, n) : 0;
}

// ---- native paths -------------------------------------------------------------------------------

char Host_NativeSep(OpenBGI_t* o)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->nativeSep ? (char)f->nativeSep : '/';
}

int Host_NativeSjis(OpenBGI_t* o)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->nativeSjis;
}

const char* Host_NativeFilePart(OpenBGI_t* o, const char* path)
{
	char sep = Host_NativeSep(o);
	int sjis = Host_NativeSjis(o);
	const char* p = path;
	const char* part = path;
	while(*p)
	{
		if(*p == '/' || *p == sep)
			part = p + 1;
		p += sjis && Sjis_IsLead((uint8_t)*p) && p[1] ? 2 : 1;
	}
	return part;
}

HostFile_t* Host_NativeOpen(OpenBGI_t* o, const char* path, int mode)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->nativeOpen ? (HostFile_t*)f->nativeOpen(o->host.user, path, mode) : NULL;
}

int Host_NativeStat(OpenBGI_t* o, const char* path, OpenBGI_NativeStat_t* out)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->nativeStat ? f->nativeStat(o->host.user, path, out) : OPENBGI_NATIVE_NONE;
}

int Host_NativeWritable(OpenBGI_t* o, const char* path)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->nativeWritable ? f->nativeWritable(o->host.user, path) : 1;
}

int Host_NativeMkdir(OpenBGI_t* o, const char* path)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->nativeMkdir ? f->nativeMkdir(o->host.user, path) : 0;
}

int Host_NativeRemove(OpenBGI_t* o, const char* path)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->nativeRemove ? f->nativeRemove(o->host.user, path) : 0;
}

int Host_NativeList(OpenBGI_t* o, const char* dir, void (*fn)(void* ctx, const char* name, int isDir), void* ctx)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->nativeList ? f->nativeList(o->host.user, dir, fn, ctx) : -1;
}

void Host_NativeAbsolute(OpenBGI_t* o, const char* path, char* out, size_t n)
{
	const OpenBGI_Files_t* f = Files(o);
	if(f && f->nativeAbsolute)
		f->nativeAbsolute(o->host.user, path, out, n);
	else if(n)
		snprintf(out, n, "%s", path);
}

void Host_NativeToUtf8(OpenBGI_t* o, const char* native, char* out, size_t n)
{
	const OpenBGI_Files_t* f = Files(o);
	if(f && f->toUtf8)
		f->toUtf8(o->host.user, native, out, n);
	else if(n)
		snprintf(out, n, "%s", native);
}

void Host_Utf8ToNative(OpenBGI_t* o, const char* utf8, char* out, size_t n)
{
	const OpenBGI_Files_t* f = Files(o);
	if(f && f->fromUtf8)
		f->fromUtf8(o->host.user, utf8, out, n);
	else if(n)
		snprintf(out, n, "%s", utf8);
}

uint8_t* Host_ReadNativeFile(OpenBGI_t* o, const char* path, uint32_t* size)
{
	HostFile_t* f = Host_NativeOpen(o, path, OPENBGI_OPEN_READ);
	uint64_t len;
	uint8_t* data;
	*size = 0;
	if(!f)
		return NULL;
	len = Host_FileSize(o, f);
	data = len < 0xffffffffu ? (uint8_t*)malloc((size_t)len + 1) : NULL;
	if(data && len && Host_FileRead(o, f, data, (uint32_t)len) != (uint32_t)len)
	{
		free(data);
		data = NULL;
	}
	Host_FileClose(o, f);
	if(!data)
		return NULL;
	data[len] = 0;
	*size = (uint32_t)len;
	return data;
}

int Host_WriteNativeFile(OpenBGI_t* o, const char* path, const void* data, uint32_t size)
{
	HostFile_t* f = Host_NativeOpen(o, path, OPENBGI_OPEN_CREATE);
	int ok;
	if(!f)
		return 0;
	ok = Host_FileWrite(o, f, data, size) == size;
	Host_FileClose(o, f);
	return ok;
}

void Host_GameDir(OpenBGI_t* o, char* buf, size_t n)
{
	const OpenBGI_Files_t* f = Files(o);
	Empty(buf, n);
	if(f && f->gameDir)
		f->gameDir(o->host.user, buf, n);
}

int Host_SetGameDir(OpenBGI_t* o, const char* dir)
{
	const OpenBGI_Files_t* f = Files(o);
	return f && f->setGameDir ? f->setGameDir(o->host.user, dir) : 0;
}

void Host_UserDataDir(OpenBGI_t* o, char* buf, size_t n)
{
	const OpenBGI_Files_t* f = Files(o);
	Empty(buf, n);
	if(f && f->userDataDir)
		f->userDataDir(o->host.user, buf, n);
}

void Host_BinaryDir(OpenBGI_t* o, char* buf, size_t n)
{
	const OpenBGI_Files_t* f = Files(o);
	Empty(buf, n);
	if(f && f->binaryDir)
		f->binaryDir(o->host.user, buf, n);
}

// ---- the registry -------------------------------------------------------------------------------

int Host_RegRead(OpenBGI_t* o, const char* key, const char* value, char* buf, size_t n)
{
	const OpenBGI_Registry_t* r = Registry(o);
	return r && r->read ? r->read(o->host.user, key, value, buf, n) : 0;
}

int Host_RegCreateKey(OpenBGI_t* o, const char* key, const char* cls)
{
	const OpenBGI_Registry_t* r = Registry(o);
	return r && r->createKey ? r->createKey(o->host.user, key, cls) : 0;
}

int Host_RegWrite(OpenBGI_t* o, const char* key, const char* value, const char* s)
{
	const OpenBGI_Registry_t* r = Registry(o);
	return r && r->write ? r->write(o->host.user, key, value, s) : 0;
}

int Host_RegDeleteKey(OpenBGI_t* o, const char* key)
{
	const OpenBGI_Registry_t* r = Registry(o);
	return r && r->deleteKey ? r->deleteKey(o->host.user, key) : 0;
}

// ---- the text -----------------------------------------------------------------------------------

void Host_TextEncoding(OpenBGI_t* o, int utf8)
{
	if(o->host.textEncoding)
		o->host.textEncoding(o->host.user, utf8 != 0);
}
