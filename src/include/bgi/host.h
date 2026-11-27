/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * host.h - the host's services as the engine calls them: the tables of
 *          OpenBGI_Host_t (openbgi.h) behind functions that take the
 *          engine (src/core/host.c: the files and the registry;
 *          src/core/hostsys.c: the machine and the shell;
 *          src/core/hostmedia.c: CD audio and the movies); not part of
 *          the original engine
 *
 * Each function calls the member of the host's table, or - where the host
 * has no such table or member - does what a missing file or service
 * would: it fails (NULL, 0, OPENBGI_INVALID_ATTRS, an empty string), and
 * a missing machine fact is the ordinary machine's of openbgi.h.  The
 * conventions are openbgi.h's.  Nothing else in the engine reads those
 * tables.
 */
#ifndef BGI_HOST_H_
#define BGI_HOST_H_

#include "bgi/common.h"
#include "openbgi.h"

/* The structs that begin with their `size` (openbgi.h): the host's may
 * be shorter than the engine's, built with an older header as it may be.
 * Host_Take makes the engine's whole copy `own` of the host's `from`:
 * what the host has of it and zero from there on, the size the engine's;
 * 0 - and all of it zero - when the host left the size out.  Host_Give
 * hands the host the engine's whole `from`: as much as the host's `to`
 * holds, whose size stays the host's. */
int Host_Take(void* own, size_t ownSize, const void* from);
void Host_Give(void* to, const void* from, size_t fromSize);

// a stream of the host's (Host_FileOpen, Host_NativeOpen); NULL is none
typedef struct HostFile HostFile_t;
// a directory listing of the host's (Host_FindFirst)
typedef struct HostFind HostFind_t;

// ---- the game's files, by the game's paths ----------------------------------------------------
HostFile_t* Host_FileOpen(OpenBGI_t* o, const char* path, int mode); // OPENBGI_OPEN_*
void Host_FileClose(OpenBGI_t* o, HostFile_t* f);                    // accepts NULL; a native stream too
uint32_t Host_FileRead(OpenBGI_t* o, HostFile_t* f, void* buf, uint32_t n);
uint32_t Host_FileWrite(OpenBGI_t* o, HostFile_t* f, const void* buf, uint32_t n);
int Host_FileSeek(OpenBGI_t* o, HostFile_t* f, uint64_t pos);
uint64_t Host_FileSize(OpenBGI_t* o, HostFile_t* f);
int Host_FileGetTime(OpenBGI_t* o, HostFile_t* f, uint32_t* lo, uint32_t* hi);
int Host_FileSetTime(OpenBGI_t* o, HostFile_t* f, uint32_t lo, uint32_t hi);
uint32_t Host_FileAttrs(OpenBGI_t* o, const char* path);
int Host_FileSetAttrs(OpenBGI_t* o, const char* path, uint32_t attrs);
int Host_FileDelete(OpenBGI_t* o, const char* path);
int Host_FileMove(OpenBGI_t* o, const char* from, const char* to);
int Host_FileCopy(OpenBGI_t* o, const char* from, const char* to, int failIfExists);
int Host_DirCreate(OpenBGI_t* o, const char* path);
int Host_DirRemove(OpenBGI_t* o, const char* path);
HostFind_t* Host_FindFirst(OpenBGI_t* o, const char* pattern, OpenBGI_FindData_t* out);
int Host_FindNext(OpenBGI_t* o, HostFind_t* h, OpenBGI_FindData_t* out);
void Host_FindClose(OpenBGI_t* o, HostFind_t* h); // accepts NULL
int Host_SetCurrentDir(OpenBGI_t* o, const char* dir);
void Host_ExeDir(OpenBGI_t* o, char* buf, size_t n);
uint32_t Host_DriveType(OpenBGI_t* o, const char* root);          // OPENBGI_DRIVE_UNKNOWN without the service
uint32_t Host_LogicalDrives(OpenBGI_t* o, char* buf, uint32_t n); // 0 (an empty list) without the service
int Host_VolumeReady(OpenBGI_t* o, const char* root);
int Host_MediaPresent(OpenBGI_t* o, char drive);
int Host_SpecialFolder(OpenBGI_t* o, int csidl, char* buf, size_t n);
int Host_WindowsDir(OpenBGI_t* o, char* buf, size_t n);

// ---- native paths ---------------------------------------------------------------------------
char Host_NativeSep(OpenBGI_t* o); // the separator to join native paths with: '/' without the service
int Host_NativeSjis(OpenBGI_t* o); // native paths are Shift-JIS (a lead byte's trail byte is no separator)
/* where the last component of a native path starts: after its last
 * separator ('/' or Host_NativeSep, as Host_NativeSjis says), "" when it
 * ends in one, the path itself when it has none */
const char* Host_NativeFilePart(OpenBGI_t* o, const char* path);
HostFile_t* Host_NativeOpen(OpenBGI_t* o, const char* path, int mode);          // OPENBGI_OPEN_READ or _CREATE
int Host_NativeStat(OpenBGI_t* o, const char* path, OpenBGI_NativeStat_t* out); // OPENBGI_NATIVE_*
int Host_NativeWritable(OpenBGI_t* o, const char* path);                        // 1 when the file there can be written to (a host that cannot say: 1)
uint32_t Host_NativeRoots(OpenBGI_t* o, char* buf, uint32_t n);                 // the folders a browser starts from (OpenBGI_Files_t.nativeRoots); 0 without them
int Host_NativeMkdir(OpenBGI_t* o, const char* path);
int Host_NativeRemove(OpenBGI_t* o, const char* path);
int Host_NativeList(OpenBGI_t* o, const char* dir, void (*fn)(void* ctx, const char* name, int isDir), void* ctx);
void Host_NativeAbsolute(OpenBGI_t* o, const char* path, char* out, size_t n);
void Host_NativeToUtf8(OpenBGI_t* o, const char* native, char* out, size_t n);
void Host_Utf8ToNative(OpenBGI_t* o, const char* utf8, char* out, size_t n);
/* A whole native file in a malloc'ed buffer with a NUL after the data
 * (free() it), *size bytes; NULL (and *size 0) when it cannot be read.  Writing one
 * creates or replaces it; 1 when all of it was written. */
uint8_t* Host_ReadNativeFile(OpenBGI_t* o, const char* path, uint32_t* size);
int Host_WriteNativeFile(OpenBGI_t* o, const char* path, const void* data, uint32_t size);
void Host_GameDir(OpenBGI_t* o, char* buf, size_t n);
int Host_SetGameDir(OpenBGI_t* o, const char* dir);
void Host_UserDataDir(OpenBGI_t* o, char* buf, size_t n);
void Host_BinaryDir(OpenBGI_t* o, char* buf, size_t n);

// ---- the registry -------------------------------------------------------------------------------
int Host_RegRead(OpenBGI_t* o, const char* key, const char* value, char* buf, size_t n);
int Host_RegCreateKey(OpenBGI_t* o, const char* key, const char* cls);
int Host_RegWrite(OpenBGI_t* o, const char* key, const char* value, const char* s);
int Host_RegDeleteKey(OpenBGI_t* o, const char* key);

// ---- the text ---------------------------------------------------------------------------------
void Host_TextEncoding(OpenBGI_t* o, int utf8); // "81 00": the host told the scripts' encoding

// ---- the machine ------------------------------------------------------------------------------
int Host_HasCpuid(OpenBGI_t* o);                                   // the host reports an x86 processor
int Host_Cpuid(OpenBGI_t* o, uint32_t leaf, uint32_t out[4]);      // 0 without CPUID or the leaf
uint64_t Host_ReadTsc(OpenBGI_t* o);                               // 0 without a counter
uint32_t Host_CpuSignature(OpenBGI_t* o);                          // CPUID.1:EAX, 0 without CPUID
int Host_CpuCount(OpenBGI_t* o);                                   // at least 1
void Host_MemoryStatus(OpenBGI_t* o, OpenBGI_MemoryStatus_t* out); // GlobalMemoryStatus
void Host_MemoryStatusEx(OpenBGI_t* o, uint64_t* totalPhys, uint64_t* availPhys);
uint64_t Host_ProcessMemory(OpenBGI_t* o); // the bytes of memory the process holds now; 0 when the host does not say
void Host_ComputerName(OpenBGI_t* o, char* buf, size_t n);
void Host_UserName(OpenBGI_t* o, char* buf, size_t n);
void Host_Version(OpenBGI_t* o, uint32_t out[4], char* csd, size_t n); // major, minor, build, platform; the service pack
int Host_UiLanguage(OpenBGI_t* o);                                     // a LANGID
int Host_KnowsUiLanguage(OpenBGI_t* o);                                // the host says it (else the value is the original's home, Japanese)
void Host_DisplayAdapter(OpenBGI_t* o, char* desc, size_t n, uint32_t version[4]);
int Host_AudioAvailable(OpenBGI_t* o); // DirectSound 8

// ---- the shell --------------------------------------------------------------------------------
void* Host_InstanceLock(OpenBGI_t* o, const char* name, int* alreadyExists);
void Host_InstanceUnlock(OpenBGI_t* o, void* lock); // accepts NULL
int Host_SendToRunning(OpenBGI_t* o, const char* windowClass, const char* data);
int Host_OpenDocument(OpenBGI_t* o, const char* target); // a web link or a document with the desktop's program for it; 0 without the service
int Host_ClipboardSetText(OpenBGI_t* o, const char* text);
int Host_SetWallpaper(OpenBGI_t* o, const char* path, int stretch, int tile);
int Host_CreateShortcut(OpenBGI_t* o, const char* linkPath, const char* target, const char* args); // args NULL: none
void Host_NotifyAssocChanged(OpenBGI_t* o);

// ---- CD audio ---------------------------------------------------------------------------------
uint32_t Host_CdOpen(OpenBGI_t* o, const char* element); // a device id, 0 none
int Host_CdSetTimeFormatTmsf(OpenBGI_t* o, uint32_t device);
int Host_CdStatus(OpenBGI_t* o, uint32_t device, uint32_t item, uint32_t* out);
int Host_CdPlay(OpenBGI_t* o, uint32_t device, uint32_t from, uint32_t to, int notify);
int Host_CdStop(OpenBGI_t* o, uint32_t device);
int Host_CdClose(OpenBGI_t* o, uint32_t device);

// ---- the movies -------------------------------------------------------------------------------
int Host_HasPlayer(OpenBGI_t* o);                   // the host has a movie player of its own
int Host_MovieOpen(OpenBGI_t* o, const char* path); // OPENBGI_MOVIE_*: FAILED without a player
int Host_MovieStart(OpenBGI_t* o, int x, int y, int w, int h, int32_t volume, int32_t* lengthMs);
void Host_MovieStop(OpenBGI_t* o);
int Host_MovieRunning(OpenBGI_t* o);
void Host_MovieSetVolume(OpenBGI_t* o, int32_t volume);
int Host_HasDecoder(OpenBGI_t* o); // the host has a movie decoder
// NULL when it cannot; `file` is the movie's, read through the engine, `path` its name for the host's messages
void* Host_DecoderOpen(OpenBGI_t* o, const OpenBGI_MovieFile_t* file, const char* path, int w, int h, int audio, OpenBGI_MovieInfo_t* info);
int32_t Host_DecoderFrames(OpenBGI_t* o, void* m, int* ended);
int Host_DecoderTake(OpenBGI_t* o, void* m, int32_t index, uint8_t* out);
int Host_DecoderSamples(OpenBGI_t* o, void* m, int16_t* out, int frames, int* ended);
void Host_DecoderClose(OpenBGI_t* o, void* m); // accepts NULL

#endif // BGI_HOST_H_
