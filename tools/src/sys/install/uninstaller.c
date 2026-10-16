/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * uninstaller.c - what a fresh installation leaves for its removal: the
 *                 initial uninst.lst and the uninstaller program
 *                 (install_internal.h; both are steps of Install_RunDialog)
 */
#include "install_internal.h"
#include "bgi/file.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/os.h"

/* Write "<install dir>\uninst.lst" (the format is described in
 * uninstlist.c).  A new list starts with the uninstaller, the list itself,
 * BGI.dat, BGIError.txt and comap.dat, then BGI.gdb and the `saveCount`
 * save files marked as kept ('@'; named by `saveFmt` with the slot number,
 * or by SaveFileName when saveFmt is NULL); an existing list is extended
 * instead.  Then come the files of `made` and the `subdirs` (NULL or
 * NULL-terminated) as '$' lines, each unless the list mentions it already.
 * The list is added to `made`.  1 when written; a failure shows an error
 * box and returns 0. */
int Uninst_WriteList(MadeList_t* made, const char* uninstExe, int saveCount, const char* saveFmt, char** subdirs,
	const InstallCtx_t* ctx)
{
	char path[0x104], name[0x104];
	char *buf = (char*)BGI_Alloc(0x100000), *end; // 1 MB, the list's size limit
	File_t f;
	int i, ok = 0;
	const MadeFile_t* m;

	sprintf(path, "%s\\%s", ctx->installDir, UNINST_LIST);
	File_Ctor(&f);
	if(File_OpenRead(&f, path))
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
	if(File_Create(&f, path))
	{
		uint32_t len = (uint32_t)strlen(buf) + 1;
		ok = File_Write(&f, buf, len) == len;
		File_Close(&f);
	}
	BGI_Free(buf);
	if(!ok)
		MsgBox(MSG_INST_UNINST_LIST_FAILED, MSG_ERROR_CAPTION, OS_MB_ICONHAND);
	MadeList_Add(made, UNINST_LIST, path); // the list is rolled back with the files
	File_Dtor(&f);
	return ok;
}

/* Copy <uninstExe> from the disc (its directory mirrors the engine's, see
 * InstallCtx_t) into the installation directory, asking for the disc with
 * `insertMsg` until it is found or the user gives up, and register it as
 * HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\<product>
 * (DisplayName and UninstallString) for "Add / Remove Programs".  1 when
 * done; a copy that fails shows an error box and returns 0. */
int Uninst_Install(const InstallCtx_t* ctx, const char* product, const char* uninstExe, const char* insertMsg)
{
	char dstPath[0x104], srcPath[0x104], drives[0x104 * 26];
	File_t src, dst;
	int result = 0, keepLooking = 1;

	File_Ctor(&src);
	File_Ctor(&dst);
	sprintf(dstPath, "%s\\%s", ctx->installDir, uninstExe);
	while(keepLooking)
	{
		int n = CandidateDrives(drives), i, found = 0;
		// the first ready drive that has the file decides, even when it is a directory there
		for(i = 0; i < n; i++)
		{
			uint32_t attrs;
			sprintf(srcPath, "%s\\%s\\%s", drives + i * 0x104, ctx->srcRel, uninstExe);
			if(!DriveReady(srcPath))
				continue;
			attrs = OS_FileAttrs(srcPath);
			if(attrs == OS_INVALID_ATTRS)
				continue;
			found = !(attrs & OS_ATTR_DIRECTORY);
			break;
		}
		if(found)
		{
			uint32_t size;
			uint8_t* buf;
			File_OpenRead(&src, srcPath); // the file is read whole; the open is not checked
			size = File_Size(&src);
			buf = (uint8_t*)BGI_Alloc(size ? size : 1);
			File_Read(&src, buf, size);
			File_Close(&src);
			if(!File_Create(&dst, dstPath))
			{
				MsgBox(MSG_INST_UNINST_COPY_FAILED, MSG_ERROR_CAPTION, OS_MB_ICONHAND);
			}
			else
			{
				uint32_t written = File_Write(&dst, buf, size);
				File_Close(&dst);
				if(written != size)
				{
					OS_FileDelete(dstPath);
					MsgBox(MSG_INST_UNINST_COPY_FAILED, MSG_ERROR_CAPTION, OS_MB_ICONHAND);
				}
				else
				{
					char key[0x240];
					sprintf(key, "HKLM\\%s\\%s", "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", product);
					OS_RegCreateKey(key, product);
					OS_RegWriteString(key, "DisplayName", product);
					OS_RegWriteString(key, "UninstallString", dstPath);
					result = 1;
				}
			}
			BGI_Free(buf);
			break; // found: done, whether the copy succeeded or not
		}
		// not found: ask for the disc; Cancel asks whether to give up the installation
		if(MsgBox(insertMsg, MSG_NOTICE, OS_MB_OKCANCEL | OS_MB_ICONINFO) != OS_IDOK)
		{
			if(MsgBox(MSG_ASK_ABORT, MSG_CONFIRM, OS_MB_YESNO | OS_MB_ICONQUESTION | OS_MB_DEFBUTTON2) == OS_IDYES)
				keepLooking = 0;
		}
		OS_SleepMs(200);
	}
	File_Dtor(&src);
	File_Dtor(&dst);
	return result;
}
