/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_machine.c - the OS layer's machine and shell as the host's tables
 *                (OpenBGI_Machine_t and OpenBGI_Shell_t of openbgi.h,
 *                for OS_Host; declared in host/os_common.h)
 *
 * The back ends keep their functions (OS_MemoryStatus, OS_Cpuid,
 * OS_OpenDocument, OS_ClipboardSetText, ...); the tables hand them to the
 * engine as they are.  CPUID and the time stamp counter are there only
 * on an x86 processor: elsewhere the host reports none, and the engine
 * skips its processor checks.  The test host's counter runs on its
 * own clock (OS_ReadTsc of the headless build).
 */
#include "host/os.h"
#include "host/os_common.h"

// ---- the machine ------------------------------------------------------------------------------

#if defined(OS_HAVE_CPUID)
static int MachineCpuid(void* user, uint32_t leaf, uint32_t out[4])
{
	BGI_UNUSED(user);
	return OS_Cpuid(leaf, out);
}

static uint64_t MachineReadTsc(void* user)
{
	BGI_UNUSED(user);
	return OS_ReadTsc();
}
#define MACHINE_CPUID MachineCpuid
#define MACHINE_TSC   MachineReadTsc
#else
#define MACHINE_CPUID NULL
#define MACHINE_TSC   NULL
#endif

static int MachineCpuCount(void* user)
{
	BGI_UNUSED(user);
	return OS_CpuCount();
}

static void MachineMemoryStatus(void* user, OpenBGI_MemoryStatus_t* out)
{
	BGI_UNUSED(user);
	OS_MemoryStatus(out);
}

static void MachineMemoryStatusEx(void* user, uint64_t* totalPhys, uint64_t* availPhys)
{
	BGI_UNUSED(user);
	OS_MemoryStatusEx(totalPhys, availPhys);
}

static int MachineComputerName(void* user, char* buf, size_t n)
{
	BGI_UNUSED(user);
	return OS_ComputerName(buf, n);
}

static int MachineUserName(void* user, char* buf, size_t n)
{
	BGI_UNUSED(user);
	return OS_UserName(buf, n);
}

static void MachineVersion(void* user, uint32_t out[4], char* csd, size_t n)
{
	BGI_UNUSED(user);
	OS_VersionEx(out, csd, n);
}

static int MachineUiLanguage(void* user)
{
	BGI_UNUSED(user);
	return OS_UiLanguage();
}

static int MachineDisplayAdapter(void* user, char* desc, size_t n, uint32_t version[4])
{
	BGI_UNUSED(user);
	return OS_DisplayAdapter(desc, n, version);
}

static int MachineAudioAvailable(void* user)
{
	BGI_UNUSED(user);
	return OS_AudioAvailable();
}

static uint64_t MachineProcessMemory(void* user)
{
	BGI_UNUSED(user);
	return OS_ProcessMemory();
}

static const OpenBGI_Machine_t kMachine = {
	sizeof(OpenBGI_Machine_t),
	MACHINE_CPUID,
	MACHINE_TSC,
	MachineCpuCount,
	MachineMemoryStatus,
	MachineMemoryStatusEx,
	MachineComputerName,
	MachineUserName,
	MachineVersion,
	MachineUiLanguage,
	MachineDisplayAdapter,
	MachineAudioAvailable,
	MachineProcessMemory,
};

const OpenBGI_Machine_t* OsCommon_Machine(void)
{
	return &kMachine;
}

// ---- the shell --------------------------------------------------------------------------------

static void* ShellInstanceLock(void* user, const char* name, int* alreadyExists)
{
	BGI_UNUSED(user);
	return OS_InstanceMutexCreate(name, alreadyExists);
}

static void ShellInstanceUnlock(void* user, void* lock)
{
	BGI_UNUSED(user);
	OS_InstanceMutexRelease((OsMutex_t*)lock);
}

static int ShellSendToRunning(void* user, const char* windowClass, const char* data)
{
	BGI_UNUSED(user);
	return OS_IpcSendToRunning(windowClass, data);
}

static int ShellOpenDocument(void* user, const char* target)
{
	BGI_UNUSED(user);
	return OS_OpenDocument(target);
}

static int ShellClipboardSetText(void* user, const char* text)
{
	BGI_UNUSED(user);
	return OS_ClipboardSetText(text);
}

static int ShellSetWallpaper(void* user, const char* path, int stretch, int tile)
{
	BGI_UNUSED(user);
	return OS_SetWallpaper(path, stretch, tile);
}

static int ShellCreateShortcut(void* user, const char* linkPath, const char* target, const char* args)
{
	BGI_UNUSED(user);
	return OS_CreateShortcut(linkPath, target, args);
}

static void ShellNotifyAssocChanged(void* user)
{
	BGI_UNUSED(user);
	OS_ShellNotifyAssocChanged();
}

static const OpenBGI_Shell_t kShell = {
	sizeof(OpenBGI_Shell_t),
	ShellInstanceLock,
	ShellInstanceUnlock,
	ShellSendToRunning,
	ShellOpenDocument,
	ShellClipboardSetText,
	ShellSetWallpaper,
	ShellCreateShortcut,
	ShellNotifyAssocChanged,
};

const OpenBGI_Shell_t* OsCommon_Shell(void)
{
	return &kShell;
}
