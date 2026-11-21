/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fs.h - the engine's files: the game directory as a folder or as a zip
 *        archive (src/fs/fs.c); not part of the original engine
 *
 * The engine reaches its files through Fs_*, which take the paths and keep
 * the conventions of the host's file services (openbgi.h, "The files"):
 * the game's paths in the scripts' encoding with '\\', relative ones under
 * the current directory, Win32 return values.  With the game in a folder
 * they are the host's calls (bgi/host.h).
 *
 * A game can also be played from a zip archive (vol.h), which then stands
 * for the game directory: Fs_MountArchive puts it under the host's game
 * directory, which is the game's own folder in the user's data
 * (Fs_GameFolder).  The two make one directory:
 *
 *   * a path is looked up in the folder first, then in the archive, so
 *     whatever the engine wrote - or the player put there - wins;
 *   * everything written goes to the folder: a file of the archive is
 *     copied there first when it is opened for writing with its contents
 *     kept (Fs_FileOpenWrite) or its attributes change, and the folders a
 *     new file needs are made there;
 *   * deleting, moving or removing something of the archive hides it from
 *     then on: the folder keeps a list of what is gone
 *     (.openbgi/deleted.txt);
 *   * a listing (Fs_FindFirst) merges both;
 *   * files that only the archive holds read in place when they are stored,
 *     from memory once decompressed when they are deflated, and - deflated
 *     and larger than 32 MB - from a copy extracted once into
 *     .openbgi/cache/ of the folder.  A file something outside the
 *     engine has to open by its name (the system's own movie player, a
 *     file another program opens) gets the same copy (Fs_LocalPath); the
 *     movie decoder reads through this layer and needs none.
 *
 * The folder ".openbgi" at the top of the game folder is this layer's own
 * and is left out of listings.  The game directory's paths are the ones
 * under the host's exeDir as it was when the archive was mounted, and the
 * relative paths under a current directory set through Fs_SetCurrentDir.
 */
#ifndef BGI_FS_H_
#define BGI_FS_H_

#include "bgi/common.h"
#include "openbgi.h"

typedef struct FsFile FsFile_t;
typedef struct FsFind FsFind_t;

// the host's file services (openbgi.h) over the folder and the archive
FsFile_t* Fs_FileOpenRead(OpenBGI_t* o, const char* path);
FsFile_t* Fs_FileCreate(OpenBGI_t* o, const char* path);
FsFile_t* Fs_FileOpenWrite(OpenBGI_t* o, const char* path);
void Fs_FileClose(FsFile_t* f); // accepts NULL
uint32_t Fs_FileRead(FsFile_t* f, void* buf, uint32_t n);
uint32_t Fs_FileWrite(FsFile_t* f, const void* buf, uint32_t n); // 0 for a file of the archive (opened for reading)
int Fs_FileSeek(FsFile_t* f, uint32_t pos);
uint32_t Fs_FileSize(FsFile_t* f);
int Fs_FileGetTime(FsFile_t* f, uint32_t* lo, uint32_t* hi);
int Fs_FileSetTime(FsFile_t* f, uint32_t lo, uint32_t hi);
uint32_t Fs_FileAttrs(OpenBGI_t* o, const char* path); // a file of the archive is OPENBGI_ATTR_ARCHIVE, a folder OPENBGI_ATTR_DIRECTORY
int Fs_FileSetAttrs(OpenBGI_t* o, const char* path, uint32_t attrs);
int Fs_FileDelete(OpenBGI_t* o, const char* path);
int Fs_FileMove(OpenBGI_t* o, const char* from, const char* to);
int Fs_FileCopy(OpenBGI_t* o, const char* from, const char* to, int failIfExists);
int Fs_DirCreate(OpenBGI_t* o, const char* path);
int Fs_DirRemove(OpenBGI_t* o, const char* path);
int Fs_SetCurrentDir(OpenBGI_t* o, const char* dir);
FsFind_t* Fs_FindFirst(OpenBGI_t* o, const char* pattern, OpenBGI_FindData_t* out);
int Fs_FindNext(FsFind_t* h, OpenBGI_FindData_t* out);
void Fs_FindClose(FsFind_t* h);

/* A small text file of the engine's own beside the game: <name> in the
 * folder ".openbgi" of the game folder, where what the game writes goes
 * as well.  Read whole into a malloc'ed string (free() it; NULL when
 * there is none), or written whole (1 when done). */
char* Fs_OwnRead(OpenBGI_t* o, const char* name);
int Fs_OwnWrite(OpenBGI_t* o, const char* name, const char* text);
/* A whole file in a malloc'ed buffer with a NUL after the data (free()
 * it), *size bytes; NULL when it cannot be read. */
uint8_t* Fs_LoadFile(OpenBGI_t* o, const char* path, uint32_t* size);
/* The path of a file for a service of the host's that opens it by name:
 * the path itself, or for a file only the archive holds the path of its
 * extracted copy.  0 when there is no such file (`out` is then the path as
 * well). */
int Fs_LocalPath(OpenBGI_t* o, const char* path, char* out, size_t n);

/* Whether a native path is an archive a game can be played from: a file
 * that starts with the signature of a zip archive, or a disc image (ISO
 * 9660 in 2048-byte sectors; vol.h). */
int Fs_IsArchive(OpenBGI_t* o, const char* nativePath);
/* The folder of the game in the archive at a native path, a native path
 * with a trailing separator, made when missing: "games/<name>/" in the
 * user's data (the host's userDataDir, or $BGI_DATA when it is set),
 * <name> being the archive's file name without ".zip" or ".iso", or the
 * name of a folder played as an archive.  0 when it cannot be made. */
int Fs_GameFolder(OpenBGI_t* o, const char* nativeZip, char* out, size_t n);
/* Mount the archive at a native path over the game directory, which the
 * caller made the game folder (the host's setGameDir); it becomes the current
 * directory.  0 when it is no zip archive or disc image that can be read.
 * A native path that names a folder mounts that folder the same way: its
 * files are read in place and what the game writes goes to the game
 * folder (the command line's --overlay). */
int Fs_MountArchive(OpenBGI_t* o, const char* nativeZip);
/* Lay the zip archive, disc image or folder at a native path over the
 * mounted archive, as a patch (the command line's --patch=): its files
 * are the game's from then on, in place of the archive's of the same
 * path, and its other files are there as well; its root is the game's
 * directory (vol.h, Vol_Overlay).  What the game folder holds still
 * wins.  The files it has, or -1 when nothing is mounted or it cannot be
 * read. */
int Fs_MountPatch(OpenBGI_t* o, const char* nativePath);
void Fs_Unmount(OpenBGI_t* o);
int Fs_Mounted(OpenBGI_t* o);             // 1 while an archive is mounted
const char* Fs_MountedPath(OpenBGI_t* o); // its native path ("" when none is)

// the layer's state: the mounted archive and the game folder over it (o->files.fs)
#define FS_PATH 0x400 // bytes of a path buffer
typedef struct FsState
{
	struct Vol* vol;      // the mounted archive, NULL with the game in a folder
	char exeDir[FS_PATH]; // the host's exeDir at the mount: the game directory as the engine's paths begin
	char folder[FS_PATH]; // its gameDir at the mount: the game folder's native path (trailing separator)
	char cwd[FS_PATH];    // the current directory as a key
	int cwdOutside;       // the current directory is not in the game directory
	char** deleted;       // the keys of what is gone from the archive, sorted
	uint32_t nDeleted, capDeleted;
} FsState_t;

#endif // BGI_FS_H_
