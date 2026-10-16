/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * machine.c - what the POSIX back end knows about the machine and other
 *             processes (inc/bgi/os.h): the processor, memory, user and
 *             computer names the installer records, the Windows version
 *             the engine is told, the performance counter, and the
 *             process and shell services (running a program, opening a
 *             document); the wallpaper and shortcut services report
 *             failure
 */
#include "posix_internal.h"

// ---- machine -----------------------------------------------------------------------------------

// GlobalMemoryStatus from sysinfo, each figure cut to 32 bits; the virtual space is a 32-bit process's 2 GB
void OS_MemoryStatus(OsMemStatus_t* m)
{
	struct sysinfo si;
	memset(m, 0, sizeof *m);
	if(sysinfo(&si) != 0)
		return;
	m->totalPhys = (uint32_t)BGI_MIN((uint64_t)si.totalram * si.mem_unit, 0xffffffffu);
	m->availPhys = (uint32_t)BGI_MIN((uint64_t)si.freeram * si.mem_unit, 0xffffffffu);
	m->totalPageFile = (uint32_t)BGI_MIN((uint64_t)si.totalswap * si.mem_unit, 0xffffffffu);
	m->availPageFile = (uint32_t)BGI_MIN((uint64_t)si.freeswap * si.mem_unit, 0xffffffffu);
	m->totalVirtual = 0x7ffe0000u; // the 2 GB user space of a 32-bit process
	m->availVirtual = m->totalVirtual;
	m->loadPercent = m->totalPhys ? (uint32_t)(100 - (uint64_t)m->availPhys * 100 / m->totalPhys) : 0;
}

// the host name
int OS_ComputerName(char* buf, size_t n)
{
	return gethostname(buf, n) == 0;
}

// $USER, else the password entry of the real user id, else "user"
int OS_UserName(char* buf, size_t n)
{
	const char* u = getenv("USER");
	if(!u)
	{
		struct passwd* pw = getpwuid(getuid());
		u = pw ? pw->pw_name : "user";
	}
	snprintf(buf, n, "%s", u);
	return 1;
}

// CPUID through the compiler's intrinsic on x86; the leaf's range (basic / extended) is checked against the highest supported
int OS_Cpuid(uint32_t leaf, uint32_t out[4])
{
#if defined(BGI_X86)
	unsigned a, b, c, d;
	unsigned max = __get_cpuid_max(leaf & 0x80000000u, NULL);
	if(max == 0 || leaf > max)
		return 0;
	__cpuid(leaf, a, b, c, d);
	out[0] = a;
	out[1] = b;
	out[2] = c;
	out[3] = d;
	return 1;
#else
	BGI_UNUSED(leaf);
	BGI_UNUSED(out);
	return 0;
#endif
}

// RDTSC after a serialising CPUID, as the original's CPU speed measurement does; 0 off x86
uint64_t OS_ReadTsc(void)
{
#if defined(BGI_X86)
	unsigned a, b, c, d;
	__cpuid(0, a, b, c, d); // the original serialises with CPUID first
	return __rdtsc();
#else
	return 0;
#endif
}

// the monotonic clock in nanoseconds stands in for the performance counter
int OS_PerfCounterNs(int64_t* out)
{
	struct timespec ts;
	if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	*out = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
	return 1;
}

// CPUID.1:EAX (family, model, stepping), 0 without CPUID
uint32_t OS_CpuSignature(void)
{
	uint32_t r[4];
	return OS_Cpuid(1, r) ? r[0] : 0;
}

// Windows XP (NT 5.1) is reported, so that the engine takes its NT code paths
int OS_Version(int* platform, int* major, int* minor)
{
	*platform = 2; // VER_PLATFORM_WIN32_NT: the engine takes the NT paths (IOCTLs, registry)
	*major = 5;
	*minor = 1;
	return 1;
}

/* the LANGID of the locale in LC_ALL / LC_MESSAGES / LANG: the primary
 * language from the two-letter code (the ones the games ask about, else
 * English), the sublanguage 1 (the default) */
int OS_UiLanguage(void)
{
	static const struct
	{
		const char* code;
		int primary;
	} kLangs[] = {{"ja", 0x11}, {"en", 0x09}, {"zh", 0x04}, {"ko", 0x12}, {"de", 0x07}, {"fr", 0x0c}, {"es", 0x0a},
		{"it", 0x10}, {"ru", 0x19}, {"pt", 0x16}};
	const char* env = getenv("LC_ALL");
	size_t i;
	if(!env || !*env)
		env = getenv("LC_MESSAGES");
	if(!env || !*env)
		env = getenv("LANG");
	if(env)
		for(i = 0; i < sizeof kLangs / sizeof kLangs[0]; i++)
			if(strncmp(env, kLangs[i].code, 2) == 0 && (env[2] == 0 || env[2] == '_' || env[2] == '.' || env[2] == '@'))
				return 0x400 | kLangs[i].primary;
	return 0x409;
}

// the same as XP Service Pack 3, build 2600
void OS_VersionEx(uint32_t out[4], char* csd, size_t n)
{
	int platform, major, minor;
	OS_Version(&platform, &major, &minor);
	out[0] = (uint32_t)major;
	out[1] = (uint32_t)minor;
	out[2] = 2600; // the build of the version OS_Version reports (XP SP3)
	out[3] = (uint32_t)platform;
	if(n)
		snprintf(csd, n, "Service Pack 3");
}

// GlobalMemoryStatusEx: the physical memory from sysinfo in full
void OS_MemoryStatusEx(uint64_t* totalPhys, uint64_t* availPhys)
{
	struct sysinfo si;
	*totalPhys = *availPhys = 0;
	if(sysinfo(&si) != 0)
		return;
	*totalPhys = (uint64_t)si.totalram * si.mem_unit;
	*availPhys = (uint64_t)si.freeram * si.mem_unit;
}

// "X11 display" with a zero driver version
int OS_DisplayAdapter(char* desc, size_t n, uint32_t version[4])
{
	// no Direct3D here: the X server is the "adapter"
	if(n)
		snprintf(desc, n, "X11 display");
	version[0] = version[1] = version[2] = version[3] = 0;
	return 1;
}

// ---- processes ---------------------------------------------------------------------------------

struct OsProcess
{
	pid_t pid;        // the child
	int done, status; // reaped by OS_ProcessWait, and its wait status
};

// the command line (Shift-JIS, converted) through "sh -c"; NULL when fork fails (a failed exec ends the child with 127)
OsProcess_t* OS_ProcessStart(const char* cmdLine)
{
	OsProcess_t* p = (OsProcess_t*)calloc(1, sizeof *p);
	char utf[0x1000];
	OsPosix_SjisToUtf8(cmdLine, utf, sizeof utf);
	p->pid = fork();
	if(p->pid == 0)
	{
		execl("/bin/sh", "sh", "-c", utf, (char*)NULL);
		_exit(127);
	}
	if(p->pid < 0)
	{
		free(p);
		return NULL;
	}
	return p;
}

void OS_ProcessWaitIdle(OsProcess_t* p)
{
	BGI_UNUSED(p); // WaitForInputIdle has no equivalent; the process is running
}

// poll waitpid every 10 ms until the child has ended (1) or `ms` have passed (0); a waitpid error counts as ended
int OS_ProcessWait(OsProcess_t* p, uint32_t ms)
{
	uint32_t end = OS_TicksMs() + ms;
	if(p->done)
		return 1;
	for(;;)
	{
		int r = waitpid(p->pid, &p->status, WNOHANG);
		if(r == p->pid || r < 0)
		{
			p->done = 1;
			return 1;
		}
		if((int32_t)(OS_TicksMs() - end) >= 0)
			return 0;
		OS_SleepMs(10);
	}
}

// free the handle; a child that was never waited for stays a zombie until the engine exits
void OS_ProcessClose(OsProcess_t* p)
{
	free(p);
}

// ShellExecute "open": xdg-open on the converted path, detached; `dir` is ignored
int OS_ShellExecute(const char* file, const char* params, const char* dir)
{
	char cmd[0x1000], f[PATH_MAX];
	OsProcess_t* p;
	BGI_UNUSED(dir);
	OsPosix_Path(file, f, sizeof f);
	snprintf(cmd, sizeof cmd, "xdg-open '%s' %s >/dev/null 2>&1", f, params ? params : "");
	p = OS_ProcessStart(cmd);
	if(!p)
		return 0;
	OS_ProcessClose(p);
	return 1;
}

// "B0 F0": no portable desktop wallpaper service; failure
int OS_SetWallpaper(const char* path, int stretch, int tile)
{
	BGI_UNUSED(path);
	BGI_UNUSED(stretch);
	BGI_UNUSED(tile);
	return 0; // no portable desktop wallpaper service
}

// no shortcuts on this platform; the scripts take the failure
int OS_CreateShortcut(const char* linkPath, const char* target)
{
	BGI_UNUSED(linkPath);
	BGI_UNUSED(target);
	return 0;
}

int OS_CreateShortcutArgs(const char* linkPath, const char* target, const char* args)
{
	BGI_UNUSED(linkPath);
	BGI_UNUSED(target);
	BGI_UNUSED(args);
	return 0; // no shortcuts on this platform; the scripts take the failure
}
