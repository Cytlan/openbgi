/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * install_internal.h - what the installer's files share (src/sys/install/)
 *
 * The public interface is src/include/bgi/install.h; this header carries the
 * helpers one installer file provides for another: the directory trees
 * and lists of created files (folders.c), the hash list (hvl.c), the
 * installation context the copy engine (copy.c), the uninstaller
 * bookkeeping (uninstaller.c) and the dialogs (dialogs.c) work on.  The
 * state of the one installation that can run at a time is o->install:
 * Install_Begin and Install_Step (dialogs.c) fill it before they ask for
 * the progress dialog, whose window calls Install_CopyAll (copy.c).
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
int DirTree_Build(OpenBGI_t* o, DirNode_t* root, const char* path);
// DirTree_Build of "<dir>\<name>" for every name of a NULL-terminated list (or NULL); 1 when all were made
int DirTree_BuildList(OpenBGI_t* o, DirNode_t* root, const char* dir, char** names);
// remove the created directories, deepest first; 1 when every removal went
int DirTree_Remove(OpenBGI_t* o, DirNode_t* n);
void DirTree_Free(DirNode_t* n); // free the tree (the directories stay)

void MadeList_Add(MadeList_t* l, const char* name, const char* path); // prepend an entry (copies of both strings)
void MadeList_DeleteFiles(OpenBGI_t* o, const MadeList_t* l);         // delete every file of the list (the rollback)
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
uint32_t Hvl_Load(OpenBGI_t* o, int* outCount, uint8_t** outRecs, const char* path);

// ---- copy.c: the installation context and the copy engine ----------------------------

// "x:" of every CD-ROM drive and of the engine's own drive, 0x104 bytes per entry; the count
int CandidateDrives(OpenBGI_t* o, char* out);

// ---- uninstaller.c ----------------------------------------------------------------------

/* write uninst.lst of a fresh installation (the uninstaller, the list,
 * the engine's own files and save files marked as kept, every installed
 * file, every created sub-directory marked '$'); 1 when written */
int Uninst_WriteList(OpenBGI_t* o, MadeList_t* made, const char* uninstExe, int saveCount, const char* saveFmt, char** subdirs,
	const InstallCtx_t* ctx);
/* copy the uninstaller from the disc into the installation directory and
 * register it with Windows' "Add / Remove Programs": 1 done, 0 found but
 * not copied (an error box), -1 no disc holds it */
int Uninst_TryInstall(OpenBGI_t* o, const InstallCtx_t* ctx, const char* product, const char* uninstExe);

// ---- dialogs.c ----------------------------------------------------------------------------

// a box over a dialog through its callback, with the engine's treatment around it
int Inst_Box(OpenBGI_t* o, InstallBoxFn_t fn, void* ui, const char* text, const char* caption, uint32_t flags);

#endif // BGI_SYS_INSTALL_INTERNAL_H_
