/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * registry.c - the registry keys of an installation and the file
 *              associations ("80 F8", "80 F9", "80 FC"; inc/bgi/install.h)
 *
 * The keys are written through the OS layer's registry functions, which
 * take a path of the form "HKLM\Software\..." (the POSIX back end keeps
 * them in a settings file).  The installation's own key is
 * HKLM\Software\<company>\<product>, whose value "InstalledFolder" holds
 * the installation directory.
 */
#include "install_internal.h"
#include "bgi/os.h"

// "80 F8": HKLM\Software\<company>\<product>\InstalledFolder into buf (0x104 bytes); 1 when present
int RegGetInstalledFolder(char* buf, const char* company, const char* product)
{
	char key[0x1000];
	if(!company || !product)
		return 0;
	sprintf(key, "HKLM\\%s\\%s\\%s", "Software", company, product);
	return OS_RegReadString(key, "InstalledFolder", buf, 0x104);
}

// write `dir` as HKLM\Software\<company>\<product>\InstalledFolder (the end of an installation); always 1
int RegSetInstalledFolder(const char* company, const char* product, const char* dir)
{
	char key[0x1000];
	sprintf(key, "HKLM\\%s\\%s\\%s", "Software", company, product);
	OS_RegWriteString(key, "InstalledFolder", dir);
	return 1;
}

// "80 F9": delete the key HKLM\Software\<company>\<product>; 1 when deleted
int RegDeleteSoftwareKey(const char* company, const char* product)
{
	char key[0x1000];
	if(!company || !product)
		return 0;
	sprintf(key, "HKLM\\%s\\%s\\%s", "Software", company, product);
	return OS_RegDeleteKey(key) ? 1 : 0;
}

/* "80 FC": associate file extension `ext` (without the dot) with the game:
 * HKCR\.<ext> = progid, HKCR\<progid> = desc, its DefaultIcon = icon and
 * its Shell\Open\Command = command; then the shell is told that the
 * associations changed.  1 when every key was written, 0 at the first
 * failure (the keys written before it stay). */
int RegisterFileAssoc(const char* ext, const char* progid, const char* desc, const char* icon, const char* command)
{
	char key[0x1000];
	sprintf(key, "HKCR\\.%s", ext);
	if(!OS_RegWriteString(key, NULL, progid))
		return 0;
	sprintf(key, "HKCR\\%s", progid);
	if(!OS_RegWriteString(key, NULL, desc))
		return 0;
	sprintf(key, "HKCR\\%s\\DefaultIcon", progid);
	if(!OS_RegCreateKey(key, "DefaultIcon") || !OS_RegWriteString(key, NULL, icon))
		return 0;
	sprintf(key, "HKCR\\%s\\Shell", progid);
	if(!OS_RegCreateKey(key, "Shell"))
		return 0;
	sprintf(key, "HKCR\\%s\\Shell\\Open", progid);
	if(!OS_RegCreateKey(key, "Open"))
		return 0;
	sprintf(key, "HKCR\\%s\\Shell\\Open\\Command", progid);
	if(!OS_RegCreateKey(key, "Command") || !OS_RegWriteString(key, NULL, command))
		return 0;
	OS_ShellNotifyAssocChanged();
	return 1;
}
