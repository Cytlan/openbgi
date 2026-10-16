/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * file.h - files, archives, paths, the loader and the save files
 *
 * Three layers, implemented in src/core/{file,arc,paths,loadfile,loader,
 * savefile}.c:
 *
 *   File      a thin wrapper over the OS layer's file handle
 *   FileSet   one "PackFile" / "BURIKO ARC20" archive (index + data),
 *             chained into the archive manager list gArcMgr; FileSetList is
 *             the subclass registered by "80 38" that maps one pseudo
 *             archive name to a list of archive files
 *   LoadFile  the search order every load goes through: loose file in the
 *             base directory, archive in the base directory, archive in the
 *             alternative (disc) directory, retry prompt
 *
 * File names are Shift-JIS; archive paths and entry names are lower-cased
 * when the index is read, so archive lookups ignore case.  Entry names are
 * limited to 15 characters (95 with ARC20 archives, 1.573 on).
 */
#ifndef BGI_FILE_H_
#define BGI_FILE_H_

#include "bgi/common.h"
#include "bgi/os.h"

//  --- File ---------------------------------------------------
typedef struct File
{
	OsFile_t* h; // handle, OS_INVALID_FILE when closed
} File_t;
void File_Ctor(File_t* f);
void File_Dtor(File_t* f);                            // closes the file
int File_OpenRead(File_t* f, const char* path);       // 1 ok; 0 when already open or it cannot be opened
int File_Create(File_t* f, const char* path);         // create or truncate; 1 ok, 0 as above
void File_Close(File_t* f);                           // no-op when closed
uint32_t File_Read(File_t* f, void* buf, uint32_t n); // bytes read
uint32_t File_Write(File_t* f, const void* buf, uint32_t n);
int File_Seek(File_t* f, uint32_t pos); // from the start; 1 ok
uint32_t File_Size(File_t* f);

// ---- PackFile / BURIKO ARC20 archives ------------------------------------
#define PACKFILE_MAGIC "PackFile    " // 12 bytes
#define ARC20_MAGIC    "BURIKO ARC20" // 12 bytes, 1.573 on (docs/versions.md)

/* the on-disk index entry of a PackFile; the header is the 12-byte magic
 * and the 32-bit entry count, the entries follow, the data follows them */
typedef struct PackEntry // 32 bytes in a PackFile
{
	char name[16];   // NUL-padded; lower-cased after loading
	uint32_t offset; // from the end of the index
	uint32_t size;   // bytes
	uint32_t reserved[2];
} PackEntry_t;

/* the in-memory index entry of either format: an ARC20 entry is 0x80
 * bytes in the file with the name in its first 0x60 bytes, then the offset
 * and the size as 32-bit values */
typedef struct ArcEntry
{
	char name[0x60]; // lower-cased after loading
	uint32_t offset; // from the end of the index
	uint32_t size;   // bytes
} ArcEntry_t;

// result codes of the archive layer (the reads return the byte count otherwise)
#define ARC_ERR_OPEN        0x80000010u // the archive cannot be opened or is not an archive
#define ARC_ERR_NOT_FOUND   0x80000020u // name not in the archive
#define ARC_ERR_RANGE       0x80000030u // offset + size beyond the entry
#define ARC_ERR_OFFSET      0x80000040u // size larger than the entry
#define ARC_ERR_INDEX_OPEN  0x80000001u // index: cannot open
#define ARC_ERR_INDEX_MAGIC 0x80000002u // index: bad magic

typedef struct FileSet FileSet_t;
typedef struct FileSetVtbl
{
	void (*destroy)(FileSet_t* s);                                                                 // the virtual destructor: frees s and the chain after it
	int (*contains)(FileSet_t* s, const char* name);                                               // 1 when the archive has the entry
	int (*arcNameFor)(FileSet_t* s, char* dst, const char* name);                                  // the archive file holding the entry into dst; 1 when found
	uint32_t (*readRange)(FileSet_t* s, void* dst, const char* name, uint32_t off, uint32_t size); // bytes read, or ARC_ERR_*
	uint32_t (*sizeOf)(FileSet_t* s, const char* name);                                            // the stored size, or ARC_ERR_NOT_FOUND
	int (*matches)(FileSet_t* s, const char* arcPath);                                             // 1 when this node is the archive `arcPath`
} FileSetVtbl_t;

struct FileSet
{
	const FileSetVtbl_t* vt;
	int loaded;          // the index has been read (a list: the name assigned)
	FileSet_t* next;     // next archive in the manager chain
	char name[0x104];    // lower-cased path of the archive (a list: its pseudo name)
	int count;           // entries in the index
	uint32_t dataBase;   // where the data starts: 16 + the index (32 or 0x80 bytes per entry)
	ArcEntry_t* entries; // the index, count entries
	// FileSetList only:
	char dir[0x104];  // directory of the path that matched, the member archives are looked up there
	int listCount;    // member archive files
	char** list;      // their file names (lower case)
	FileSet_t* inner; // private FileSet chain for the members
};

/* The archive manager is the chain starting at gArcMgr: a node per archive
 * that has been opened (its index stays loaded), appended on first use.
 * The ArcMgr_* functions walk the chain for the node matching `arc` (a
 * path, or the pseudo name of a registered list) and apply the operation;
 * `name` is the entry.  The chain owns its nodes. */
FileSet_t* FileSet_New(void);                                                                                         // an empty, unloaded node
void FileSet_Delete(FileSet_t* s);                                                                                    // destroy through the vtable, with the chain after it; NULL is allowed
int ArcMgr_Exists(FileSet_t* mgr, const char* arc, const char* name);                                                 // 1 when the entry exists
int ArcMgr_ArcNameFor(FileSet_t* mgr, char* dst, const char* arc, const char* name);                                  // the archive file holding the entry into dst (NULL: existence only); 1 when found
uint32_t ArcMgr_Read(FileSet_t* mgr, void* dst, const char* arc, const char* name);                                   // the whole entry; bytes, or ARC_ERR_*
uint32_t ArcMgr_ReadRange(FileSet_t* mgr, void* dst, const char* arc, const char* name, uint32_t off, uint32_t size); // size 0: the whole entry; dst NULL: the size only
uint32_t ArcMgr_SizeOf(FileSet_t* mgr, const char* arc, const char* name);                                            // the stored size, or ARC_ERR_NOT_FOUND / ARC_ERR_OPEN
int ArcMgr_Register(FileSet_t* mgr, FileSet_t* other);                                                                // append; 1 when taken over, 0 when a node of that name exists

FileSet_t* FileSetList_New(void);                        // an empty list archive
int FileSetList_SetName(FileSet_t* s, const char* name); // the pseudo archive name; 0 for NULL
int FileSetList_SetList(FileSet_t* s, char** names);     // NULL-terminated, copied; 0 for NULL or empty

extern FileSet_t* gArcMgr; // the head of the archive chain, created at start-up

//  --- the file cache, enabled by "90 03" ------------------
/* an LRU cache of file images keyed by (arc, name): a file larger than
 * half the capacity is not stored */
typedef struct FileCache FileCache_t;
FileCache_t* FileCache_New(uint32_t capacity); // capacity in bytes
void FileCache_Delete(FileCache_t* c);
int FileCache_Add(FileCache_t* c, const char* arc, const char* name, const void* data, uint32_t size); // 1 stored, 0 too large or empty
int FileCache_Lookup(FileCache_t* c, void* dst, uint32_t* size, const char* arc, const char* name);    // 1 on a hit (data copied to dst)

/* the loader's optional file cache (created by "90 03"): the
 * bitmap loader consults it before reading an archive */
void Loader_SetCacheSize(uint32_t capacity); // 0 = none; replaces the current cache
void Loader_FreeCache(void);
int Loader_CacheAdd(const char* arc, const char* name, const void* data, uint32_t size); // 0 without a cache
int Loader_CacheLookup(void* dst, uint32_t* size, const char* arc, const char* name);    // 0 without a cache
extern FileCache_t* gFileCache;                                                          // NULL until "90 03" creates it

// ---- paths and the boot program -----------------------------------------
extern char gBaseDir[0x104];  // the game's directory, with trailing '\'
extern char gAltDir[0x104];   // disc directory ("80 3F", with trailing '\') or ""
extern char gBootArc[0x104];  // the archive of the boot program, "system.arc"
extern char gBootFile[0x104]; // the boot program, "ipl._bp"
extern int gFileSearchOn;     // "80 36": loose files are also looked for in the search sub-directories
typedef struct SearchNode     // the "80 37" search sub-directories, newest first; tried after the directory itself
{
	char* name; // relative to the directory being searched
	struct SearchNode* next;
} SearchNode_t;
extern SearchNode_t* gSearchList;

void SetBaseDir(const char* dir);                                                        // NULL: the executable's; also clears gAltDir and sets the current directory
void SetBootName(const char* arc, const char* file);                                     // gBootArc / gBootFile
void GetBootProgram(char* arc, char* file);                                              // copies of gBootArc / gBootFile
void SetBootProgram(const char* path, const char* file);                                 // base directory and boot archive from a command-line path
void PathJoin(char* dst, const char* dir, const char* name);                             // dir + name (dir carries its separator)
int PathPrefix(char* dst, const char* src, int n);                                       // first n+1 elements; 0 when there are fewer
void PathDirPart(char* dst, const char* path);                                           // all but the last element
int GetBasePath(char* dst, int which);                                                   // "80 3D": 0 base, 1 alternative directory; 0 when there is none
int SetInstallPaths(const char* name, const char* msg, const char* title, int askAbort); // "80 3F": find the disc holding `name`; 1 found
void Set_FileSearch(int on);                                                             // "80 36"
void FileSearch_Clear(void);                                                             // drop the search list
void PushSearchName(const char* name);                                                   // "80 37": add a sub-directory, tried before the older ones
void Drives_Scan(void);                                                                  // record every drive letter's type (start-up)
uint32_t Drive_TypeOf(char letter);                                                      // the scanned OS_DRIVE_* type of 'a'..'z' / 'A'..'Z'
int DriveReady(const char* path);                                                        // 1 when the drive of `path` has its medium in

//  --- the background loader (src/core/loader.c) ------------
/* the jobs of the loader thread; the status word is polled by the wait
 * objects ("90 10", "A0 20", "90 F4"): a file job writes 0 while pending,
 * then the loaded size or -1; a sound job writes -1 while pending, then 0
 * or an 0x8xxxxxxx error */
void Loader_QueueFile(uint32_t* status, void* buf, const char* arc, const char* name);
void Loader_QueueSe(uint32_t* status, int slot, const void* desc, int fadeInMs, double gain);
void Loader_Start(void);
void Loader_Stop(void);

// ---- loading ----------------------------------------------------------
/* buf receives the whole file, decompressed when it is DSC or
 * CompressedBG.  Returns the size in buf.  Prompts for the disc while the
 * file is missing; throws when the user gives up (or without a disc). */
uint32_t LoadFile(void* buf, const char* arc, const char* name);
/* "80 31" up to 1.69 build 444, "81 30" of 1.529 on: part of a file as
 * stored; 0 ok, 1 not found, 2 the range reaches past the end, 3 the size
 * exceeds the file, -1 short read */
int LoadFileRange(void* buf, const char* arc, const char* name, uint32_t off, uint32_t size);
/* "80 31" of 1.69 build 472 on: the range of the
 * DECODED file (DSC / CompressedBG; off = size = 0 is everything, and with
 * a CompressedBG only reports the size); the raw range moved to "81 30".
 * 0 ok, 1 not found, 2 end beyond the data, 3 size 0 or beyond, 5 decode
 * failure, 6 larger than the limit (40 MB in build 472, 64 MB from 1.494) */
int LoadFileRangeDecoded(void* buf, const char* arc, const char* name, uint32_t off, uint32_t size);
/* a range of a loose file from a directory, honouring the search list
 * (off = size = 0: all of it; buf NULL: the size only); the codes of
 * LoadFileRange, *outSize receives the byte count */
int ReadFileEx(void* buf, uint32_t* outSize, const char* dir, const char* name, uint32_t off, uint32_t size);
uint32_t ReadWholeFile(void* buf, const char* dir, const char* name);                                               // bytes read, 0 when not readable
uint32_t ReadFileAt(void* buf, const char* name);                                                                   // base dir, then alt dir
int WriteTextFile(const char* name, const void* buf, uint32_t len);                                                 // create in the base dir (or an absolute path); bytes written
uint32_t GetFileSize_(const char* arc, const char* name);                                                           // "80 35": the loaded (DSC-decoded) size, by reading; 0 not found
uint32_t GetFileSizeFast(const char* arc, const char* name);                                                        // "81 35" (1.588 on): the stored size without reading; 0 not found
int FileExists(const char* arc, const char* name);                                                                  // "80 34"
int Dir_Exists(const char* dir, const char* name);                                                                  // dir + name is a file (search list honoured)
int FindMoviePath(char* dst, const char* name);                                                                     // "90 F0": the full path of a loose file; 1 found
int PromptForDisk(const char* name, const char* title, const char* msg);                                            // "80 3C": prompt until `name` is in the base dir; 0 gave up
int Sys_RegisterFilelist(const char* name, char** list);                                                            // "80 38": a pseudo archive for a NULL-terminated list; 1 ok
int FileDialog(char* path, const char* desc, const char* ext, const char* title, const char* initialDir, int mode); // "80 3B": 0 ok, -1 cancel, 4 bad mode
int FindFiles(char** out, const char* pattern, int recurse, const char* prefix, int max);                           // "80 24" / "80 25": the count; out NULL counts only

/* an 8-byte zero-initialised buffer that is only ever read; it
 * serves as the empty path prefix for FindFiles and as the seed byte of
 * Window_DrawText's tag buffer */
extern char gEmptyPath[8];
void RetryPrompt(const char* msg);                                   // the loader's "insert the disc" prompt; throws when there is no disc or the user aborts
uint32_t Arc_Read(void* dst, const char* arcPath, const char* name); // ArcMgr_Read on gArcMgr after the name-length check
int Arc_Exists(const char* arcPath, const char* name);               // ArcMgr_Exists on gArcMgr after the name-length check

//  --- save files ----------------------------------------
extern int gSaveEncode;                       // "80 74": scramble the save files (the default)
void SaveFileName(char* dst, int slot);       // "BGI%.4d.cad"
int SaveWrite(int slot, const char* comment); // "80 78": 1 when written completely
int SaveLoad(int slot);                       // "80 79": 0 ok, 1 missing, 2 wrong size, 3 corrupt
int SaveCheck(int slot);                      // "80 7B": the codes of SaveLoad, without loading
int SaveReadHeader(int slot, void* dst);      // "80 7A": the 64-byte header; 0 ok, 1 missing, 2 wrong size
void Set_SaveEncode(int on);

#endif // BGI_FILE_H_
