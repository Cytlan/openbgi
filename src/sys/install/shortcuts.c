/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * shortcuts.c - the desktop and start-menu shortcuts of an installation
 *               ("80 F3", "80 F6", "80 F7", "81 F7"; src/include/bgi/install.h)
 *
 * The links themselves are made by the host (Host_CreateShortcut, an
 * IShellLink on Windows); this file places them: on the desktop, or in a
 * folder of the start menu's "Programs" that is created on the way and
 * removed again when the link cannot be written.  A `name` is the link's
 * file name as the script gives it, a `target` the full path of the file
 * the link opens.
 */
#include "install_internal.h"
#include "bgi/host.h"
#include "bgi/fs.h"
#include "bgi/core.h"

// a link file at `linkPath` that opens `target`; 1 when made
static int ShellLink_Create(OpenBGI_t* o, const char* linkPath, const char* target)
{
	return Host_CreateShortcut(o, linkPath, target, NULL);
}

/* "80 F7": one link, "<Programs>\<folder>\<name>" -> target, or
 * "<Desktop>\<name>" when folder is NULL.  The folder is created when it
 * is missing and removed again when the link cannot be made.  1 when the
 * link was made. */
int CreateShortcut(OpenBGI_t* o, const char* folder, const char* name, const char* target)
{
	char base[0x104], dir[0x104], link[0x104];
	int ok;
	if(folder)
	{
		DirNode_t* root = DirTree_New();
		GetSpecialFolder(o, base, 2);
		sprintf(dir, "%s\\%s", base, folder);
		ok = 0;
		if(DirTree_Build(o, root, dir))
		{
			sprintf(link, "%s\\%s", dir, name);
			ok = ShellLink_Create(o, link, target);
			if(!ok)
				DirTree_Remove(o, root);
		}
		DirTree_Free(root);
		return ok;
	}
	GetSpecialFolder(o, base, 1);
	sprintf(link, "%s\\%s", base, name);
	return ShellLink_Create(o, link, target);
}

/* "81 F7" of 1.492 on: CreateShortcut with a command line `args` for the
 * target (the user-data window makes a desktop shortcut this way that
 * resumes the game); 1 when the link was made. */
int CreateShortcutArgs(OpenBGI_t* o, const char* folder, const char* name, const char* target, const char* args)
{
	char base[0x104], dir[0x104], link[0x104];
	int ok;
	if(folder)
	{
		DirNode_t* root = DirTree_New();
		GetSpecialFolder(o, base, 2);
		sprintf(dir, "%s\\%s", base, folder);
		ok = 0;
		if(DirTree_Build(o, root, dir))
		{
			sprintf(link, "%s\\%s", dir, name);
			ok = Host_CreateShortcut(o, link, target, args);
			if(!ok)
				DirTree_Remove(o, root);
		}
		DirTree_Free(root);
		return ok;
	}
	GetSpecialFolder(o, base, 1);
	sprintf(link, "%s\\%s", base, name);
	return Host_CreateShortcut(o, link, target, args);
}

/* "80 F3": the links of an installation: "<dir>\<exe>" as <name> on the
 * desktop (when desktop != 0) and, when startMenu != 0, the folder
 * "<Programs>\<folder>" with <name> -> "<dir>\<exe>" and <name2> ->
 * "<dir>\<exe2>".  When one of the start-menu links fails, the links made
 * so far (the desktop one included) and the folder are removed again.
 * Returns 1 when the start-menu part succeeded or was not asked for; a
 * failed desktop link alone is not reported. */
int CreateShortcuts(OpenBGI_t* o, const char* dir, const char* exe, const char* name, const char* exe2, const char* name2,
	const char* folder, int startMenu, int desktop)
{
	char desktopDir[0x104], programs[0x104], link[0x104], target[0x104], folderDir[0x104];
	DirNode_t* root;
	int result = 1;

	GetSpecialFolder(o, desktopDir, 1);
	GetSpecialFolder(o, programs, 2);
	o->install.madeList.head = NULL; // the installer's list is reused for the links made here

	if(desktop)
	{
		sprintf(link, "%s\\%s", desktopDir, name);
		sprintf(target, "%s\\%s", dir, exe);
		if(ShellLink_Create(o, link, target))
			MadeList_Add(&o->install.madeList, name, link);
	}
	root = DirTree_New();
	if(startMenu)
	{
		result = 0;
		sprintf(folderDir, "%s\\%s", programs, folder);
		if(DirTree_Build(o, root, folderDir))
		{
			sprintf(link, "%s\\%s", folderDir, name);
			sprintf(target, "%s\\%s", dir, exe);
			if(ShellLink_Create(o, link, target))
			{
				MadeList_Add(&o->install.madeList, name, link);
				sprintf(link, "%s\\%s", folderDir, name2);
				sprintf(target, "%s\\%s", dir, exe2);
				if(ShellLink_Create(o, link, target))
				{
					MadeList_Add(&o->install.madeList, name2, link);
					result = 1;
				}
			}
		}
		if(!result)
		{
			MadeList_DeleteFiles(o, &o->install.madeList);
			DirTree_Remove(o, root);
		}
	}
	MadeList_Free(&o->install.madeList);
	DirTree_Free(root);
	return result;
}

/* "80 F6": delete "<Desktop>\<name>", "<Programs>\<folder>\<name>" and
 * "<Programs>\<folder>\<name2>", then the folder itself when removeFolder
 * != 0 (which only works once it is empty).  Missing files are ignored. */
void DeleteShortcuts(OpenBGI_t* o, const char* name, const char* name2, const char* folder, int removeFolder)
{
	char desktopDir[0x104], programs[0x104], path[0x104];
	GetSpecialFolder(o, desktopDir, 1);
	GetSpecialFolder(o, programs, 2);
	sprintf(path, "%s\\%s", desktopDir, name);
	Fs_FileDelete(o, path);
	sprintf(path, "%s\\%s\\%s", programs, folder, name);
	Fs_FileDelete(o, path);
	sprintf(path, "%s\\%s\\%s", programs, folder, name2);
	Fs_FileDelete(o, path);
	if(removeFolder)
	{
		sprintf(path, "%s\\%s", programs, folder);
		Fs_DirRemove(o, path);
	}
}
