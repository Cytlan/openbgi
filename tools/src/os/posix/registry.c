/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * registry.c - the registry emulation of the POSIX back end
 *              (inc/bgi/os.h): the values the installer and the engine
 *              keep under HKEY_LOCAL_MACHINE / HKEY_CURRENT_USER live in
 *              ~/.config/bgi/registry.txt, one "key\\value=data" line
 *              each (the format is OsCommon_RegFile*'s).  Also here: the
 *              shell folders, and the stubs of the installer dialogs and
 *              the second-instance message, which have no counterpart.
 */
#include "posix_internal.h"

int OS_RegReadString(const char* key, const char* value, char* buf, size_t n)
{
	return OsCommon_RegFileRead(gPosixRegFile, key, value, buf, n);
}

// a key line without a value registers the key; the class is not kept
int OS_RegCreateKey(const char* key, const char* cls)
{
	BGI_UNUSED(cls);
	return OsCommon_RegFileWrite(gPosixRegFile, key, NULL, "");
}

int OS_RegWriteString(const char* key, const char* value, const char* s)
{
	return OsCommon_RegFileWrite(gPosixRegFile, key, value, s);
}

int OS_RegDeleteKey(const char* key)
{
	return OsCommon_RegFileDeleteKey(gPosixRegFile, key);
}

// no shell to tell about the file association
void OS_ShellNotifyAssocChanged(void)
{
}

// the shell folders under $HOME: the desktop, the programs menu and the documents; anything else is the home itself
int OS_SpecialFolder(int csidl, char* buf, size_t n)
{
	const char* home = getenv("HOME");
	if(!home)
		home = ".";
	switch(csidl)
	{
		case 0x10: snprintf(buf, n, "%s/Desktop", home); break;                // CSIDL_DESKTOPDIRECTORY
		case 2: snprintf(buf, n, "%s/.local/share/applications", home); break; // CSIDL_PROGRAMS
		case 5: snprintf(buf, n, "%s/Documents", home); break;                 // CSIDL_PERSONAL
		default: snprintf(buf, n, "%s", home); break;
	}
	return 1;
}

// the configuration directory stands in for the Windows directory
int OS_WindowsDir(char* buf, size_t n)
{
	snprintf(buf, n, "%s", gPosixConfigDir);
	return 1;
}

int OS_InstallerDialog(int id, void* ctx)
{
	BGI_UNUSED(id);
	BGI_UNUSED(ctx);
	return 0; // the installer's resource dialogs are not available here: "cancelled"
}

// no second instance can be reached: the boot path is not handed over
int OS_IpcSendToRunning(const char* windowClass, const char* data)
{
	BGI_UNUSED(windowClass);
	BGI_UNUSED(data);
	return 0;
}

const char* OsPosix_ConfigDir(void)
{
	return gPosixConfigDir;
}
