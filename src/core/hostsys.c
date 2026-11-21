/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * hostsys.c - the host's machine and shell as the engine calls them
 *             (src/include/bgi/host.h)
 *
 * Thin calls of OpenBGI_Machine_t and OpenBGI_Shell_t (openbgi.h).  A
 * machine fact the host does not give is the ordinary machine's of
 * openbgi.h: one processor without CPUID, 1 GB of memory with half of
 * it free, Windows XP SP3, a Japanese user interface, DirectSound
 * present.  A shell call the host does not take fails.
 */
#include "bgi/host.h"
#include "bgi/core.h"

#include <stdio.h>
#include <string.h>

// the ordinary machine's memory: 1 GB, half of it free, 2 GB of page file (1.5 GB free), a 32-bit process's 2 GB of address space
#define PLAIN_PHYS     0x40000000u
#define PLAIN_PAGEFILE 0x80000000u
#define PLAIN_VIRTUAL  0x7ffe0000u

static const OpenBGI_Machine_t* Machine(OpenBGI_t* o)
{
	return o->host.machine;
}

static const OpenBGI_Shell_t* Shell(OpenBGI_t* o)
{
	return o->host.shell;
}

// ---- the machine ------------------------------------------------------------------------------

int Host_HasCpuid(OpenBGI_t* o)
{
	const OpenBGI_Machine_t* m = Machine(o);
	return m && m->cpuid;
}

int Host_Cpuid(OpenBGI_t* o, uint32_t leaf, uint32_t out[4])
{
	const OpenBGI_Machine_t* m = Machine(o);
	return m && m->cpuid ? m->cpuid(o->host.user, leaf, out) != 0 : 0;
}

uint64_t Host_ReadTsc(OpenBGI_t* o)
{
	const OpenBGI_Machine_t* m = Machine(o);
	return m && m->readTsc ? m->readTsc(o->host.user) : 0;
}

uint32_t Host_CpuSignature(OpenBGI_t* o)
{
	uint32_t r[4];
	return Host_Cpuid(o, 1, r) ? r[0] : 0;
}

int Host_CpuCount(OpenBGI_t* o)
{
	const OpenBGI_Machine_t* m = Machine(o);
	int n = m && m->cpuCount ? m->cpuCount(o->host.user) : 1;
	return n > 0 ? n : 1;
}

void Host_MemoryStatus(OpenBGI_t* o, OpenBGI_MemoryStatus_t* out)
{
	const OpenBGI_Machine_t* m = Machine(o);
	memset(out, 0, sizeof *out);
	if(m && m->memoryStatus)
	{
		m->memoryStatus(o->host.user, out);
		return;
	}
	out->loadPercent = 50;
	out->totalPhys = PLAIN_PHYS;
	out->availPhys = PLAIN_PHYS / 2;
	out->totalPageFile = PLAIN_PAGEFILE;
	out->availPageFile = PLAIN_PAGEFILE / 4 * 3;
	out->totalVirtual = PLAIN_VIRTUAL;
	out->availVirtual = PLAIN_VIRTUAL;
}

void Host_MemoryStatusEx(OpenBGI_t* o, uint64_t* totalPhys, uint64_t* availPhys)
{
	const OpenBGI_Machine_t* m = Machine(o);
	*totalPhys = PLAIN_PHYS;
	*availPhys = PLAIN_PHYS / 2;
	if(m && m->memoryStatusEx)
		m->memoryStatusEx(o->host.user, totalPhys, availPhys);
}

void Host_ComputerName(OpenBGI_t* o, char* buf, size_t n)
{
	const OpenBGI_Machine_t* m = Machine(o);
	if(!n)
		return;
	buf[0] = 0;
	if(m && m->computerName)
		m->computerName(o->host.user, buf, n); // a failure leaves it empty, as GetComputerName's buffer
	else
		snprintf(buf, n, "OPENBGI");
}

void Host_UserName(OpenBGI_t* o, char* buf, size_t n)
{
	const OpenBGI_Machine_t* m = Machine(o);
	if(!n)
		return;
	buf[0] = 0;
	if(m && m->userName)
		m->userName(o->host.user, buf, n);
	else
		snprintf(buf, n, "user");
}

void Host_Version(OpenBGI_t* o, uint32_t out[4], char* csd, size_t n)
{
	const OpenBGI_Machine_t* m = Machine(o);
	if(n)
		csd[0] = 0;
	if(m && m->version)
	{
		m->version(o->host.user, out, csd, n);
		return;
	}
	out[0] = 5; // Windows XP: NT 5.1, build 2600, Service Pack 3
	out[1] = 1;
	out[2] = 2600;
	out[3] = 2; // VER_PLATFORM_WIN32_NT
	if(n)
		snprintf(csd, n, "Service Pack 3");
}

int Host_UiLanguage(OpenBGI_t* o)
{
	const OpenBGI_Machine_t* m = Machine(o);
	return m && m->uiLanguage ? m->uiLanguage(o->host.user) : 0x411;
}

int Host_KnowsUiLanguage(OpenBGI_t* o)
{
	const OpenBGI_Machine_t* m = Machine(o);
	return m && m->uiLanguage;
}

void Host_DisplayAdapter(OpenBGI_t* o, char* desc, size_t n, uint32_t version[4])
{
	const OpenBGI_Machine_t* m = Machine(o);
	if(n)
		desc[0] = 0;
	version[0] = version[1] = version[2] = version[3] = 0;
	if(m && m->displayAdapter)
		m->displayAdapter(o->host.user, desc, n, version);
}

int Host_AudioAvailable(OpenBGI_t* o)
{
	const OpenBGI_Machine_t* m = Machine(o);
	return m && m->audioAvailable ? m->audioAvailable(o->host.user) != 0 : 1;
}

// ---- the shell --------------------------------------------------------------------------------

void* Host_InstanceLock(OpenBGI_t* o, const char* name, int* alreadyExists)
{
	const OpenBGI_Shell_t* s = Shell(o);
	*alreadyExists = 0;
	return s && s->instanceLock ? s->instanceLock(o->host.user, name, alreadyExists) : NULL;
}

void Host_InstanceUnlock(OpenBGI_t* o, void* lock)
{
	const OpenBGI_Shell_t* s = Shell(o);
	if(lock && s && s->instanceUnlock)
		s->instanceUnlock(o->host.user, lock);
}

int Host_SendToRunning(OpenBGI_t* o, const char* windowClass, const char* data)
{
	const OpenBGI_Shell_t* s = Shell(o);
	return s && s->sendToRunning ? s->sendToRunning(o->host.user, windowClass, data) != 0 : 0;
}

int Host_OpenDocument(OpenBGI_t* o, const char* target)
{
	const OpenBGI_Shell_t* s = Shell(o);
	return s && s->openDocument ? s->openDocument(o->host.user, target) != 0 : 0;
}

int Host_ClipboardSetText(OpenBGI_t* o, const char* text)
{
	const OpenBGI_Shell_t* s = Shell(o);
	return s && s->clipboardSetText ? s->clipboardSetText(o->host.user, text) != 0 : 0;
}

int Host_SetWallpaper(OpenBGI_t* o, const char* path, int stretch, int tile)
{
	const OpenBGI_Shell_t* s = Shell(o);
	return s && s->setWallpaper ? s->setWallpaper(o->host.user, path, stretch, tile) : 0; // SystemParametersInfo's result
}

int Host_CreateShortcut(OpenBGI_t* o, const char* linkPath, const char* target, const char* args)
{
	const OpenBGI_Shell_t* s = Shell(o);
	return s && s->createShortcut ? s->createShortcut(o->host.user, linkPath, target, args) != 0 : 0;
}

void Host_NotifyAssocChanged(OpenBGI_t* o)
{
	const OpenBGI_Shell_t* s = Shell(o);
	if(s && s->notifyAssocChanged)
		s->notifyAssocChanged(o->host.user);
}
