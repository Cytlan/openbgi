/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * file.h - files, archives, paths, the loader and the save files
 *
 * Three layers, implemented in src/fs/{file,arc,paths,loadfile,loader,
 * savefile}.c:
 *
 *   File      a thin wrapper over the engine's file handle (fs.h)
 *   FileSet   one "PackFile" / "BURIKO ARC20" archive (index + data),
 *             chained into the archive manager list o->files.arcMgr;
 *             FileSetList is the subclass registered by "80 38" that maps
 *             one pseudo archive name to a list of archive files
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
#include "bgi/fs.h"
#include "bgi/request.h" // AskRetry_t

//  --- File ---------------------------------------------------
typedef struct File
{
	FsFile_t* h; // handle, NULL when closed
} File_t;
void File_Ctor(File_t* f);
void File_Dtor(File_t* f);                                    // closes the file
int File_OpenRead(OpenBGI_t* o, File_t* f, const char* path); // 1 ok; 0 when already open or it cannot be opened
int File_Create(OpenBGI_t* o, File_t* f, const char* path);   // create or truncate; 1 ok, 0 as above
void File_Close(File_t* f);                                   // no-op when closed
uint32_t File_Read(File_t* f, void* buf, uint32_t n);         // bytes read
uint32_t File_Write(File_t* f, const void* buf, uint32_t n);
int File_Seek(File_t* f, uint32_t pos); // from the start; 1 ok
uint32_t File_Size(File_t* f);

// ---- PackFile / BURIKO ARC20 archives ------------------------------------
#define PACKFILE_MAGIC "PackFile    " // 12 bytes
#define ARC20_MAGIC    "BURIKO ARC20" // 12 bytes, 1.573 on (docs/dev/versions.md)

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
	void (*destroy)(OpenBGI_t* o, FileSet_t* s);                                                                 // the virtual destructor: frees s and the chain after it
	int (*contains)(OpenBGI_t* o, FileSet_t* s, const char* name);                                               // 1 when the archive has the entry
	int (*arcNameFor)(OpenBGI_t* o, FileSet_t* s, char* dst, const char* name);                                  // the archive file holding the entry into dst; 1 when found
	uint32_t (*readRange)(OpenBGI_t* o, FileSet_t* s, void* dst, const char* name, uint32_t off, uint32_t size); // bytes read, or ARC_ERR_*
	uint32_t (*sizeOf)(OpenBGI_t* o, FileSet_t* s, const char* name);                                            // the stored size, or ARC_ERR_NOT_FOUND
	int (*matches)(OpenBGI_t* o, FileSet_t* s, const char* arcPath);                                             // 1 when this node is the archive `arcPath`
	uint32_t (*reset)(OpenBGI_t* o, FileSet_t* s, const char* arcPath);                                          // 1.573 on: drop the index of `arcPath` here or further down the chain; 0, or ARC_ERR_OPEN
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

/* The archive manager is the chain starting at o->files.arcMgr: a node
 * per archive that has been opened (its index stays loaded), appended on
 * first use.  The ArcMgr_* functions walk the chain for the node matching
 * `arc` (a path, or the pseudo name of a registered list) and apply the
 * operation; `name` is the entry.  The chain owns its nodes. */
FileSet_t* FileSet_New(void);                                                                                                       // an empty, unloaded node
void FileSet_Delete(OpenBGI_t* o, FileSet_t* s);                                                                                    // destroy through the vtable, with the chain after it; NULL is allowed
int ArcMgr_Exists(OpenBGI_t* o, FileSet_t* mgr, const char* arc, const char* name);                                                 // 1 when the entry exists
int ArcMgr_ArcNameFor(OpenBGI_t* o, FileSet_t* mgr, char* dst, const char* arc, const char* name);                                  // the archive file holding the entry into dst (NULL: existence only); 1 when found
uint32_t ArcMgr_Read(OpenBGI_t* o, FileSet_t* mgr, void* dst, const char* arc, const char* name);                                   // the whole entry; bytes, or ARC_ERR_*
uint32_t ArcMgr_ReadRange(OpenBGI_t* o, FileSet_t* mgr, void* dst, const char* arc, const char* name, uint32_t off, uint32_t size); // size 0: the whole entry; dst NULL: the size only
uint32_t ArcMgr_SizeOf(OpenBGI_t* o, FileSet_t* mgr, const char* arc, const char* name);                                            // the stored size, or ARC_ERR_NOT_FOUND / ARC_ERR_OPEN
int ArcMgr_Register(FileSet_t* mgr, FileSet_t* other);                                                                              // append; 1 when taken over, 0 when a node of that name exists
uint32_t ArcMgr_Reset(OpenBGI_t* o, FileSet_t* mgr, const char* arc);                                                               // "81 3C": the archive's index is read again on its next use; 0, or ARC_ERR_OPEN when no node is or can become it

FileSet_t* FileSetList_New(void);                                      // an empty list archive
int FileSetList_SetName(OpenBGI_t* o, FileSet_t* s, const char* name); // the pseudo archive name; 0 for NULL
int FileSetList_SetList(OpenBGI_t* o, FileSet_t* s, char** names);     // NULL-terminated, copied; 0 for NULL or empty

//  --- the file cache, enabled by "90 03" ------------------
/* an LRU cache of file images keyed by (arc, name): a file larger than
 * half the capacity is not stored */
typedef struct FileCache FileCache_t;
FileCache_t* FileCache_New(uint32_t capacity); // capacity in bytes
void FileCache_Delete(FileCache_t* c);
int FileCache_Add(OpenBGI_t* o, FileCache_t* c, const char* arc, const char* name, const void* data, uint32_t size); // 1 stored, 0 too large or empty
int FileCache_Lookup(OpenBGI_t* o, FileCache_t* c, void* dst, uint32_t* size, const char* arc, const char* name);    // 1 on a hit (data copied to dst)

/* the loader's optional file cache (created by "90 03"): the
 * bitmap loader consults it before reading an archive */
void Loader_SetCacheSize(OpenBGI_t* o, uint32_t capacity); // 0 = none; replaces the current cache
void Loader_FreeCache(OpenBGI_t* o);
int Loader_CacheAdd(OpenBGI_t* o, const char* arc, const char* name, const void* data, uint32_t size); // 0 without a cache
int Loader_CacheLookup(OpenBGI_t* o, void* dst, uint32_t* size, const char* arc, const char* name);    // 0 without a cache

// ---- paths and the boot program -----------------------------------------
typedef struct SearchNode // the "80 37" search sub-directories, newest first; tried after the directory itself
{
	char* name; // relative to the directory being searched
	struct SearchNode* next;
} SearchNode_t;

void SetBaseDir(OpenBGI_t* o, const char* dir);                                           // NULL: the executable's; also clears o->files.altDir and sets the current directory
void SetBootName(OpenBGI_t* o, const char* arc, const char* file);                        // o->files.bootArc / o->files.bootFile
void GetBootProgram(OpenBGI_t* o, char* arc, char* file);                                 // copies of o->files.bootArc / o->files.bootFile
void SetBootProgram(OpenBGI_t* o, const char* path, const char* file);                    // base directory and boot archive from a command-line path
void PathJoin(char* dst, const char* dir, const char* name);                              // dir + name (dir carries its separator)
int PathPrefix(char* dst, const char* src, int n);                                        // first n+1 elements; 0 when there are fewer
void PathDirPart(char* dst, const char* path);                                            // all but the last element
int GetBasePath(OpenBGI_t* o, char* dst, int which);                                      // "80 3D": 0 base, 1 alternative directory; 0 when there is none
void SetInstallPaths(OpenBGI_t* o, const char* name, const char* msg, const char* title); // "80 3F": the disc's marker file and its prompt, for MediaSearch and RetryPrompt
void Set_FileSearch(OpenBGI_t* o, int on);                                                // "80 36"
void FileSearch_Clear(OpenBGI_t* o);                                                      // drop the search list
void PushSearchName(OpenBGI_t* o, const char* name);                                      // "80 37": add a sub-directory, tried before the older ones
void Drives_Scan(OpenBGI_t* o);                                                           // record every drive letter's type (start-up)
uint32_t Drive_TypeOf(OpenBGI_t* o, char letter);                                         // the scanned OPENBGI_DRIVE_* type of 'a'..'z' / 'A'..'Z'
int DriveReady(OpenBGI_t* o, const char* path);                                           // 1 when the drive of `path` has its medium in

/* "81 3F": the engine's own program file.  The original tells a program
 * the path of the executable it runs in; here that is a file of the
 * engine's own making, "BGI.exe" in the game's directory, which the file
 * layer serves from memory from then on whatever the folder holds: the
 * same PROGRAM_IMAGE_SIZE bytes for every game.  What a program makes of
 * them is followed (follow.h). */
#define PROGRAM_IMAGE_SIZE 1493
void ProgramPath(OpenBGI_t* o, char* dst);                       // the path into dst (0x104 bytes); the file exists from now on
int Program_Is(OpenBGI_t* o, const char* arc, const char* name); // whether a loader instruction's file is that file
int Program_IsPath(OpenBGI_t* o, const char* path);              // whether a full path names it
const uint8_t* Program_Image(OpenBGI_t* o);                      // its PROGRAM_IMAGE_SIZE bytes

/* "80 3C": the prompt until file `name` is in the base directory
 * (loadfile.c), a step per frame: 0 while a box is up, 1 there, -1 the
 * user gave up.  The strings must last as long as the prompt. */
typedef struct DiskPrompt
{
	const char* name;
	const char* title;
	const char* msg;
	AskRetry_t ask;
} DiskPrompt_t;
int DiskPrompt_Step(OpenBGI_t* o, DiskPrompt_t* d);

/* "80 3F": the search for the disc holding the file SetInstallPaths kept
 * (paths.c), a step per frame.  MediaSearch_Step returns 0 while it goes
 * on (a box is up, or `idleMs` until the next try), 1 found (the disc is
 * the alternative directory), -1 not found. */
typedef struct MediaSearch
{
	char drives[0x400]; // the drive roots
	const char* cd[64]; // the CD-ROM drives among them
	int ncd;
	char file[0x104]; // the directory part of the marker's name
	int askAbort;     // ask for the disc after a round of tries; else one round is all
	int tries;        // of the round
	uint32_t next;    // when the next try is due (host clock)
	int asking;       // the boxes are up
	AskRetry_t ask;
	uint32_t idleMs; // the wait the step asks for
} MediaSearch_t;
void MediaSearch_Begin(OpenBGI_t* o, MediaSearch_t* m, int askAbort);
int MediaSearch_Step(OpenBGI_t* o, MediaSearch_t* m);

//  --- the background loader (src/fs/loader.c) ------------
/* the jobs of the loader thread; the status word is polled by the wait
 * objects ("90 10", "A0 20", "90 F4"): a file job writes 0 while pending,
 * then the loaded size or -1; a sound job writes -1 while pending, then 0
 * or an 0x8xxxxxxx error */
void Loader_QueueFile(OpenBGI_t* o, uint32_t* status, void* buf, const char* arc, const char* name);
/* not in the original: a file job that found its disc missing waits, with
 * the file jobs after it (loader.c).  Loader_Service, once per frame,
 * takes the answers of its boxes and tries again; Loader_Cancel drops the
 * job of a status word whose wait goes away first. */
void Loader_Service(OpenBGI_t* o);
void Loader_Cancel(OpenBGI_t* o, const uint32_t* status);
void Loader_QueueSe(OpenBGI_t* o, uint32_t* status, int slot, const void* desc, int fadeInMs, double gain, double rate);
void Loader_Start(OpenBGI_t* o);
void Loader_Stop(OpenBGI_t* o);

// ---- loading ----------------------------------------------------------
/* buf receives the whole file, decompressed when it is DSC or
 * CompressedBG.  Returns the size in buf.  Prompts for the disc while the
 * file is missing; throws when the user gives up (or without a disc). */
uint32_t LoadFile(OpenBGI_t* o, void* buf, const char* arc, const char* name);
/* "80 31" up to 1.69 build 444, "81 30" of 1.529 on: part of a file as
 * stored; 0 ok, 1 not found, 2 the range reaches past the end, 3 the size
 * exceeds the file, -1 short read */
int LoadFileRange(OpenBGI_t* o, void* buf, const char* arc, const char* name, uint32_t off, uint32_t size);
/* "80 31" of 1.69 build 472 on: the range of the
 * DECODED file (DSC / CompressedBG; off = size = 0 is everything, and with
 * a CompressedBG only reports the size); the raw range moved to "81 30".
 * 0 ok, 1 not found, 2 end beyond the data, 3 size 0 or beyond, 5 decode
 * failure, 6 larger than the limit (40 MB in build 472, 48 MB in 1.492 and 1.494, 64 MB from 1.529) */
int LoadFileRangeDecoded(OpenBGI_t* o, void* buf, const char* arc, const char* name, uint32_t off, uint32_t size);
/* a range of a loose file from a directory, honouring the search list
 * (off = size = 0: all of it; buf NULL: the size only); the codes of
 * LoadFileRange, *outSize receives the byte count */
int ReadLooseFile(OpenBGI_t* o, void* buf, uint32_t* outSize, const char* dir, const char* name, uint32_t off, uint32_t size);
uint32_t ReadWholeFile(OpenBGI_t* o, void* buf, const char* dir, const char* name); // bytes read, 0 when not readable
uint32_t ReadFileAt(OpenBGI_t* o, void* buf, const char* name);                     // base dir, then alt dir
int WriteTextFile(OpenBGI_t* o, const char* name, const void* buf, uint32_t len);   // create in the base dir (or an absolute path); bytes written
uint32_t GetFileSize_(OpenBGI_t* o, const char* arc, const char* name);             // "80 35": the loaded (DSC-decoded) size, by reading; 0 not found
uint32_t GetFileSizeFast(OpenBGI_t* o, const char* arc, const char* name);          // "81 35" (1.588 on): the stored size without reading; 0 not found
int FileExists(OpenBGI_t* o, const char* arc, const char* name);                    // "80 34"
int Dir_Exists(OpenBGI_t* o, const char* dir, const char* name);                    // dir + name is a file (search list honoured)
int FindMoviePath(OpenBGI_t* o, char* dst, const char* name);                       // "90 F0": the full path of a loose file; 1 found
int ReloadArchive(OpenBGI_t* o, const char* name);                                  // "81 3C" (1.573 on): ArcMgr_Reset of the archive in the base (then the alternative) directory; 0 found, 1 not
int Sys_RegisterFilelist(OpenBGI_t* o, const char* name, char** list);              // "80 38": a pseudo archive for a NULL-terminated list; 1 ok
// "80 3B" / "81 38": the file dialogs as requests (loadfile.c): 0 and *req, or the code for a bad argument
int FileDialog_Ask(OpenBGI_t* o, uint32_t* req, char* filter, char* path, const char* desc, const char* ext, const char* title,
	const char* initialDir, int mode); // 4 bad mode
int FileDialogN_Ask(OpenBGI_t* o, uint32_t* req, char* path, int count, const char* const* descs, const char* const* pats,
	const char* title, const char* initialDir, int kind);                                               // 4 bad kind, 7 no filter
int FindFiles(OpenBGI_t* o, char** out, const char* pattern, int recurse, const char* prefix, int max); // "80 24" / "80 25": the count; out NULL counts only

void RetryPrompt(OpenBGI_t* o, const char* msg);                                   // the loader's "insert the disc" prompt; throws when there is no disc or the user aborts
uint32_t Arc_Read(OpenBGI_t* o, void* dst, const char* arcPath, const char* name); // ArcMgr_Read on o->files.arcMgr after the name-length check
int Arc_Exists(OpenBGI_t* o, const char* arcPath, const char* name);               // ArcMgr_Exists on o->files.arcMgr after the name-length check

//  --- save files ----------------------------------------

void SaveFileName(char* dst, int slot);                     // "BGI%.4d.cad"
int SaveWrite(OpenBGI_t* o, int slot, const char* comment); // "80 78": 1 when written completely
int SaveLoad(OpenBGI_t* o, int slot);                       // "80 79": 0 ok, 1 missing, 2 wrong size, 3 corrupt
int SaveCheck(OpenBGI_t* o, int slot);                      // "80 7B": the codes of SaveLoad, without loading
int SaveReadHeader(OpenBGI_t* o, int slot, void* dst);      // "80 7A": the 64-byte header; 0 ok, 1 missing, 2 wrong size
void Set_SaveEncode(OpenBGI_t* o, int on);

#endif // BGI_FILE_H_
