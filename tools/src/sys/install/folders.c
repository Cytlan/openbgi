/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * folders.c - special folders, path validation, the directory trees and
 *             the lists of created files of the installer
 *
 * The special folders and Path_IsValid are part of the public interface
 * (inc/bgi/install.h); the directory trees (every directory an installation
 * creates, for the rollback) and the lists of created files are the
 * bookkeeping of install_internal.h that shortcuts.c, copy.c, uninstaller.c
 * and dialogs.c share.
 */
#include "install_internal.h"
#include "bgi/file.h"
#include "bgi/strutil.h"
#include "bgi/os.h"

/* "80 3A": a shell folder into buf (0x104 bytes): 0 the Windows directory
 * (also "80 FB"), 1 the desktop, 2 the start menu's "Programs", 3 "My
 * Documents".  1, or 0 for an unknown selector or a shell failure. */
int GetSpecialFolder(char* buf, int which)
{
	switch(which)
	{
		case 0: OS_WindowsDir(buf, 0x104); return 1;
		case 1: return OS_SpecialFolder(0x10, buf, 0x104); // CSIDL_DESKTOPDIRECTORY
		case 2: return OS_SpecialFolder(2, buf, 0x104);    // CSIDL_PROGRAMS
		case 3: return OS_SpecialFolder(5, buf, 0x104);    // CSIDL_PERSONAL
		default: return 0;
	}
}

/* "80 FA": read "<Windows dir>\<name>" into buf.  The file must hold a
 * path ending in "\" and a NUL; the backslash is removed.  1 when it did,
 * 0 for no name, no file or other contents (buf then holds whatever was
 * read). */
int ReadWinDirFile(char* buf, const char* name)
{
	char dir[0x104], path[0x208];
	File_t f;
	int ok = 0;
	if(!name)
		return 0;
	GetSpecialFolder(dir, 0);
	sprintf(path, "%s\\%s", dir, name);
	File_Ctor(&f);
	if(File_OpenRead(&f, path))
	{
		uint32_t size = File_Size(&f);
		File_Read(&f, buf, size);
		File_Close(&f);
		if(size >= 2 && buf[size - 2] == '\\' && buf[size - 1] == 0)
		{
			buf[size - 2] = 0;
			ok = 1;
		}
	}
	File_Dtor(&f);
	return ok;
}

/* Whether `path` is an absolute Windows path the installer accepts:
 * "x:\..." (the drive letter in any case) with no doubled backslash and no
 * second colon.  The path may be up to 0x103 bytes. */
int Path_IsValid(const char* path)
{
	char tmp[0x104];
	const char* p;
	strcpy(tmp, path);
	SjisStrLwr(tmp);
	if(tmp[0] < 'a' || tmp[0] > 'z' || tmp[1] != ':' || tmp[2] != '\\')
		return 0;
	for(p = tmp + 2; *p; p++)
	{
		if(*p == '\\')
		{
			if(p[1] == '\\')
				return 0;
		}
		else if(*p == ':')
		{
			return 0;
		}
	}
	return 1;
}

// ---- directory trees -------------------------------------------------------------------

// a node without children; with a NULL path it is the root of a new tree
DirNode_t* DirTree_New(void)
{
	DirNode_t* n = (DirNode_t*)BGI_Alloc(sizeof(DirNode_t));
	n->path = NULL;
	n->child = n->next = NULL;
	return n;
}

/* Make sure `path` exists, walking it prefix by prefix ("x:\", "x:\a",
 * "x:\a\b" ...): an existing directory is descended into (to its node when
 * the tree created it, else the current node stays); a missing one is
 * created and gets a node under the current one, so that DirTree_Remove
 * can take it away again.  Returns 0 when a prefix exists but is not a
 * directory, when the drive root itself is missing or when a directory
 * cannot be created; the directories made so far stay in the tree. */
int DirTree_Build(DirNode_t* root, const char* path)
{
	char prefix[0x104];
	DirNode_t* cur = root;
	int n;
	for(n = 0; PathPrefix(prefix, path, n); n++)
	{
		uint32_t attrs = OS_FileAttrs(prefix);
		if(attrs != OS_INVALID_ATTRS)
		{
			DirNode_t* c;
			if(!(attrs & OS_ATTR_DIRECTORY))
				return 0;
			for(c = cur->child; c; c = c->next)
			{
				if(strcmp(c->path, prefix) == 0)
				{
					cur = c;
					break;
				}
			}
		}
		else
		{
			DirNode_t* node;
			if(n <= 0 || !OS_DirCreate(prefix)) // prefix 0 is the drive root, which cannot be created
				return 0;
			node = DirTree_New();
			node->path = BGI_Strdup(prefix);
			if(!cur->child)
			{
				cur->child = node;
			}
			else
			{
				DirNode_t* c = cur->child;
				while(c->next)
					c = c->next;
				c->next = node;
			}
			cur = node;
		}
	}
	return 1;
}

// DirTree_Build of "<dir>\<name>" for every name of the NULL-terminated list; 1 when all were made (or names is NULL)
int DirTree_BuildList(DirNode_t* root, const char* dir, char** names)
{
	char path[0x104];
	int ok = 1;
	if(!names)
		return 1;
	for(; *names && ok; names++)
	{
		sprintf(path, "%s\\%s", dir, *names);
		ok = DirTree_Build(root, path);
	}
	return ok;
}

/* Remove the created directories of the tree at `n` (its node, its
 * siblings and everything below them), each one after its children; 1
 * when every removal went.  Once a removal fails nothing further is
 * removed: the parents are not empty then. */
int DirTree_Remove(DirNode_t* n)
{
	int ok = 1;
	if(!n)
		return 1;
	if(n->child)
		ok = DirTree_Remove(n->child) != 0;
	if(n->next)
		ok = ok && DirTree_Remove(n->next);
	if(!n->path)
		return ok;
	return ok && OS_DirRemove(n->path);
}

// free the nodes below and including `n`
void DirTree_Free(DirNode_t* n)
{
	if(!n)
		return;
	DirTree_Free(n->child);
	DirTree_Free(n->next);
	BGI_Free(n->path);
	BGI_Free(n);
}

// ---- lists of created files ------------------------------------------------------------

// prepend an entry (copies of `name` and `path`) to the list
void MadeList_Add(MadeList_t* l, const char* name, const char* path)
{
	MadeFile_t* m = (MadeFile_t*)BGI_Alloc(sizeof(MadeFile_t));
	m->name = BGI_Strdup(name);
	m->path = BGI_Strdup(path);
	m->next = l->head;
	l->head = m;
}

// delete every file of the list, newest first (the rollback); failures are ignored
void MadeList_DeleteFiles(const MadeList_t* l)
{
	const MadeFile_t* m;
	for(m = l->head; m; m = m->next)
		OS_FileDelete(m->path);
}

// free the entries; the list is empty afterwards
void MadeList_Free(MadeList_t* l)
{
	MadeFile_t *m = l->head, *nx;
	while(m)
	{
		nx = m->next;
		BGI_Free(m->name);
		BGI_Free(m->path);
		BGI_Free(m);
		m = nx;
	}
	l->head = NULL;
}
