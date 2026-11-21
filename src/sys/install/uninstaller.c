/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * uninstaller.c - what a fresh installation leaves for its removal: the
 *                 initial uninst.lst and the uninstaller program
 *                 (install_internal.h; both are steps of Install_Step)
 */
#include "install_internal.h"
#include "bgi/file.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/host.h"

/* Write "<install dir>\uninst.lst" (the format is described in
 * uninstlist.c).  A new list starts with the uninstaller, the list itself,
 * BGI.dat, BGIError.txt and comap.dat, then BGI.gdb and the `saveCount`
 * save files marked as kept ('@'; named by `saveFmt` with the slot number,
 * or by SaveFileName when saveFmt is NULL); an existing list is extended
 * instead.  Then come the files of `made` and the `subdirs` (NULL or
 * NULL-terminated) as '$' lines, each unless the list mentions it already.
 * The list is added to `made`.  1 when written; a failure shows an error
 * box and returns 0. */
int Uninst_WriteList(OpenBGI_t* o, MadeList_t* made, const char* uninstExe, int saveCount, const char* saveFmt, char** subdirs,
	const InstallCtx_t* ctx)
{
	char path[0x104], name[0x104];
	char *buf = (char*)BGI_Alloc(0x100000), *end; // 1 MB, the list's size limit
	File_t f;
	int i, ok = 0;
	const MadeFile_t* m;

	sprintf(path, "%s\\%s", ctx->installDir, UNINST_LIST);
	File_Ctor(&f);
	if(File_OpenRead(o, &f, path))
	{
		uint32_t size = File_Size(&f);
		File_Read(&f, buf, size);
		File_Close(&f);
		end = buf + size - 1; // the stored NUL: the new lines overwrite it
	}
	else
	{
		sprintf(buf, "%s\n%s\n%s\n%s\n%s\n@%s\n", uninstExe, UNINST_LIST, "BGI.dat", "BGIError.txt", "comap.dat",
			"BGI.gdb");
		end = buf + strlen(buf);
		for(i = 0; i < saveCount; i++)
		{
			if(saveFmt)
				sprintf(name, saveFmt, i);
			else
				SaveFileName(name, i);
			sprintf(end, "@%s\n", name);
			end += strlen(end);
		}
	}
	// "mentioned" means the name occurs anywhere in the list, also as part of a longer name
	for(m = made->head; m; m = m->next)
	{
		if(!strstr(buf, m->name))
		{
			sprintf(end, "%s\n", m->name);
			end += strlen(end);
		}
	}
	if(subdirs)
	{
		for(; *subdirs; subdirs++)
		{
			sprintf(name, "$%s", *subdirs);
			if(!strstr(buf, name))
			{
				sprintf(end, "%s\n", name);
				end += strlen(end);
			}
		}
	}
	if(File_Create(o, &f, path))
	{
		uint32_t len = (uint32_t)strlen(buf) + 1;
		ok = File_Write(&f, buf, len) == len;
		File_Close(&f);
	}
	BGI_Free(buf);
	if(!ok)
		MsgBox(o, MSG_INST_UNINST_LIST_FAILED, MSG_ERROR_CAPTION, OPENBGI_MB_ICONHAND);
	MadeList_Add(made, UNINST_LIST, path); // the list is rolled back with the files
	File_Dtor(&f);
	return ok;
}

/* Copy <uninstExe> from the disc (its directory mirrors the engine's, see
 * InstallCtx_t) into the installation directory and register it as
 * HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\<product>
 * (DisplayName and UninstallString) for "Add / Remove Programs".  1 when
 * done; a copy that fails shows an error box and returns 0; -1 when no
 * ready drive has the file (Install_Step asks for the disc and tries
 * again). */
int Uninst_TryInstall(OpenBGI_t* o, const InstallCtx_t* ctx, const char* product, const char* uninstExe)
{
	char dstPath[0x104], srcPath[0x104], drives[0x104 * 26];
	File_t src, dst;
	int result = 0, n, i, found = 0;
	uint32_t size;
	uint8_t* buf;

	sprintf(dstPath, "%s\\%s", ctx->installDir, uninstExe);
	n = CandidateDrives(o, drives);
	// the first ready drive that has the file decides, even when it is a directory there
	for(i = 0; i < n; i++)
	{
		uint32_t attrs;
		sprintf(srcPath, "%s\\%s\\%s", drives + i * 0x104, ctx->srcRel, uninstExe);
		if(!DriveReady(o, srcPath))
			continue;
		attrs = Fs_FileAttrs(o, srcPath);
		if(attrs == OPENBGI_INVALID_ATTRS)
			continue;
		found = !(attrs & OPENBGI_ATTR_DIRECTORY);
		break;
	}
	if(!found)
		return -1;
	File_Ctor(&src);
	File_Ctor(&dst);
	File_OpenRead(o, &src, srcPath); // the file is read whole; the open is not checked
	size = File_Size(&src);
	buf = (uint8_t*)BGI_Alloc(size ? size : 1);
	File_Read(&src, buf, size);
	File_Close(&src);
	if(!File_Create(o, &dst, dstPath))
	{
		MsgBox(o, MSG_INST_UNINST_COPY_FAILED, MSG_ERROR_CAPTION, OPENBGI_MB_ICONHAND);
	}
	else
	{
		uint32_t written = File_Write(&dst, buf, size);
		File_Close(&dst);
		if(written != size)
		{
			Fs_FileDelete(o, dstPath);
			MsgBox(o, MSG_INST_UNINST_COPY_FAILED, MSG_ERROR_CAPTION, OPENBGI_MB_ICONHAND);
		}
		else
		{
			char key[0x240];
			sprintf(key, "HKLM\\%s\\%s", "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", product);
			Host_RegCreateKey(o, key, product);
			Host_RegWrite(o, key, "DisplayName", product);
			Host_RegWrite(o, key, "UninstallString", dstPath);
			result = 1;
		}
	}
	BGI_Free(buf);
	File_Dtor(&src);
	File_Dtor(&dst);
	return result;
}
