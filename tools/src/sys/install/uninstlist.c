/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * uninstlist.c - the list of installed files, "uninst.lst" (inc/bgi/install.h)
 *
 * One file name per line, in the installation directory; a line starting
 * with '@' names a file the uninstaller keeps (the save files, the
 * database), one starting with '$' a created sub-directory.  The file is
 * stored with a terminating NUL.  The scripts use it to remove an
 * installation ("80 F4") and to extend it ("80 F5"); the installer writes
 * the initial list (uninstaller.c).
 */
#include "install_internal.h"
#include "bgi/file.h"
#include "bgi/os.h"

/* "80 F4": delete every file named in "<dir>\uninst.lst" except the kept
 * ('@') lines, the list itself and the names in `keep` (a NULL-terminated
 * list, or NULL).  A '$' line is handed to the file deletion as it is,
 * '$' included, so it matches nothing: the sub-directories stay.  1 when
 * the list could be opened. */
int UninstListProcess(const char* dir, char** keep)
{
	char path[0x104], line[0x104];
	File_t f;
	int opened;

	sprintf(path, "%s\\%s", dir, UNINST_LIST);
	File_Ctor(&f);
	opened = File_OpenRead(&f, path);
	if(opened)
	{
		uint32_t size = File_Size(&f);
		char *buf = (char*)BGI_Alloc(size + 1), *p;
		File_Read(&f, buf, size);
		File_Close(&f);
		buf[size] = 0;
		for(p = buf; *p;)
		{
			char* q = line;
			while(*p && *p != '\n')
				*q++ = *p++;
			*q = 0;
			if(*p)
				p++;
			if(line[0] == '@')
				continue;
			if(memcmp(line, UNINST_LIST, 11) == 0) // the 10 characters and the NUL: the whole line
				continue;
			if(keep)
			{
				char** k = keep;
				while(*k && strcmp(line, *k) != 0)
					k++;
				if(*k)
					continue;
			}
			sprintf(path, "%s\\%s", dir, line);
			OS_FileDelete(path);
		}
		BGI_Free(buf);
	}
	File_Dtor(&f);
	return opened;
}

/* "80 F5": append the names (a NULL-terminated list) that
 * "<dir>\uninst.lst" does not mention yet and write the list back; 1 when
 * the list was rewritten, 0 when it could not be opened or written.  A
 * name counts as mentioned when it occurs anywhere in the list, also as
 * part of a longer name. */
int UninstListAppend(const char* dir, char** names)
{
	char path[0x104];
	File_t f;
	int ok = 0;

	File_Ctor(&f);
	sprintf(path, "%s\\%s", dir, UNINST_LIST);
	if(File_OpenRead(&f, path))
	{
		char* buf = (char*)BGI_Alloc(0x100000); // 1 MB, the list's size limit
		uint32_t size = File_Size(&f);
		char* end;
		File_Read(&f, buf, size);
		File_Close(&f);
		end = buf + size - 1; // the stored NUL: the new lines overwrite it
		for(; *names; names++)
		{
			if(!strstr(buf, *names))
			{
				sprintf(end, "%s\n", *names);
				end += strlen(end);
			}
		}
		if(File_Create(&f, path))
		{
			uint32_t len = (uint32_t)strlen(buf) + 1;
			ok = File_Write(&f, buf, len) == len;
			File_Close(&f);
		}
		BGI_Free(buf);
	}
	File_Dtor(&f);
	return ok;
}
