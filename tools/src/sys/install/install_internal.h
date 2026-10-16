/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * install_internal.h - what the installer's files share (src/sys/install/)
 *
 * The public interface is inc/bgi/install.h; this header carries the
 * helpers one installer file provides for another: the directory trees
 * and lists of created files (folders.c), the hash list (hvl.c), the
 * installation context the copy engine (copy.c), the uninstaller
 * bookkeeping (uninstaller.c) and the dialogs (dialogs.c) work on.  The
 * globals are the state of the one installation that can run at a time:
 * Install_RunDialog (dialogs.c) fills them before it opens the progress
 * dialog, whose window calls Install_CopyAll (copy.c).
 */
#ifndef BGI_SYS_INSTALL_INTERNAL_H_
#define BGI_SYS_INSTALL_INTERNAL_H_

#include "bgi/install.h"

#define UNINST_LIST "uninst.lst" // the list of installed files, in the installation directory

// ---- folders.c: directory trees and lists of created files ----------------------------

/* Every directory the installer creates is remembered in a tree so that a
 * failed installation can remove them again; existing directories get no
 * node.  A tree starts with a pathless root. */
typedef struct DirNode
{
	char* path;            // the directory's full path; NULL for the root
	struct DirNode* child; // the first directory created below this one
	struct DirNode* next;  // the next sibling
} DirNode_t;

DirNode_t* DirTree_New(void); // an empty tree (its root)
/* make sure `path` exists, creating the missing elements below the tree's
 * root; 0 when an element exists but is not a directory, when the drive
 * root itself is missing or when a directory cannot be created */
int DirTree_Build(DirNode_t* root, const char* path);
// DirTree_Build of "<dir>\<name>" for every name of a NULL-terminated list (or NULL); 1 when all were made
int DirTree_BuildList(DirNode_t* root, const char* dir, char** names);
// remove the created directories, deepest first; 1 when every removal went
int DirTree_Remove(DirNode_t* n);
void DirTree_Free(DirNode_t* n); // free the tree (the directories stay)

// the files and shortcuts an installation created, newest first, for the rollback and uninst.lst
typedef struct MadeFile
{
	char* name; // the file / link name as the script gave it
	char* path; // the full path that was created
	struct MadeFile* next;
} MadeFile_t;

typedef struct MadeList
{
	MadeFile_t* head; // the newest entry; NULL for an empty list
} MadeList_t;

void MadeList_Add(MadeList_t* l, const char* name, const char* path); // prepend an entry (copies of both strings)
void MadeList_DeleteFiles(const MadeList_t* l);                       // delete every file of the list (the rollback)
void MadeList_Free(MadeList_t* l);                                    // free the entries; the list is empty afterwards

// ---- hvl.c: the hash verification list "BGI.hvl" ---------------------------------------

// the running state of the file hash (8 bytes in the records: h, then b4 .. b7)
typedef struct HvHash
{
	uint32_t h;             // h = h * 233 + c over the bytes c
	uint8_t b4, b5, b6, b7; // b4 += (uint8_t)h; b5 ^= (uint8_t)h; b6 += c; b7 ^= c
} HvHash_t;

void HvHash_Update(HvHash_t* s, const uint8_t* p, uint32_t n); // feed n bytes at p into the state
/* load the list: 0x40-byte records {name[0x38], hash[8]} into *outRecs
 * (*outCount of them; the caller frees the records); 0 ok, 0x80000001
 * cannot open, 0x80000002 bad header, 0x80000003 short read (*outCount and
 * *outRecs are left alone on an error) */
uint32_t Hvl_Load(int* outCount, uint8_t** outRecs, const char* path);

// ---- copy.c: the installation context and the copy engine ----------------------------

// what the dialog, the copy engine and the uninstaller bookkeeping share
typedef struct InstallCtx
{
	const char* installDir; // where the files go
	const char* srcRel;     // the engine's directory without its drive and final '\' (the disc mirrors it)
	int count;              // files to copy
	char** files;           // their names, "<disc dir>\<file>", disc by disc
} InstallCtx_t;

extern InstallCtx_t gInst;
extern MadeList_t gMadeList;     // the files created so far
extern char** gDiscMarkers;      // per disc: the file that identifies it
extern char** gDiscMsgs;         // per disc: the "insert disc" message
extern const int* gDiscCounts;   // per disc: the files on it (the files are listed disc by disc)
extern int gHvlCount;            // records of BGI.hvl loaded for this installation; 0 = no verification
extern uint8_t* gHvl;            // the records (0x40 bytes each)
extern char gDlgPath[0x104 + 4]; // the path the dialogs edit (the original's global has room past 0x104)

// "x:" of every CD-ROM drive and of the engine's own drive, 0x104 bytes per entry; the count
int CandidateDrives(char* out);

// ---- uninstaller.c ----------------------------------------------------------------------

/* write uninst.lst of a fresh installation (the uninstaller, the list,
 * the engine's own files and save files marked as kept, every installed
 * file, every created sub-directory marked '$'); 1 when written */
int Uninst_WriteList(MadeList_t* made, const char* uninstExe, int saveCount, const char* saveFmt, char** subdirs,
	const InstallCtx_t* ctx);
/* copy the uninstaller from the disc into the installation directory and
 * register it with Windows' "Add / Remove Programs"; 1 when done */
int Uninst_Install(const InstallCtx_t* ctx, const char* product, const char* uninstExe, const char* insertMsg);

#endif // BGI_SYS_INSTALL_INTERNAL_H_
