/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * machine.c - what the POSIX back end knows about the machine and the
 *             desktop (src/host/os.h): the processor, memory, user and
 *             computer names the installer records, the Windows version
 *             the engine is told, the performance counter, and a
 *             document or a web link handed to the desktop's opener;
 *             the wallpaper and shortcut services report failure
 */
#include "posix_internal.h"
#include "bgi/sjis.h"

#include <fcntl.h>
#include <strings.h>
#include <sys/wait.h>
#ifdef BGI_HEADLESS
#include "host/os_headless.h"
#endif

#if defined(BGI_HAIKU)
#include <OS.h>
#elif defined(BGI_MACOS)
#include <mach/mach.h>
#include <sys/sysctl.h>
#else
#include <sys/sysinfo.h>
#endif

// ---- machine -----------------------------------------------------------------------------------

/* the physical memory and the swap space in bytes, in all and free: from
 * sysinfo (Linux, Android), the kernel's page counts (Haiku) or sysctl and
 * the Mach VM statistics (macOS, where free memory counts the inactive
 * pages too); 0 when the system does not tell */
static int MemoryFigures(uint64_t* totalPhys, uint64_t* freePhys, uint64_t* totalSwap, uint64_t* freeSwap)
{
#if defined(BGI_MACOS)
	uint64_t mem = 0;
	size_t len = sizeof mem;
	vm_statistics64_data_t vs;
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	struct xsw_usage swap;
	if(sysctlbyname("hw.memsize", &mem, &len, NULL, 0) != 0)
		return 0;
	*totalPhys = mem;
	*freePhys = 0;
	if(host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &count) == KERN_SUCCESS)
		*freePhys = ((uint64_t)vs.free_count + vs.inactive_count) * (uint64_t)sysconf(_SC_PAGESIZE);
	*totalSwap = *freeSwap = 0;
	len = sizeof swap;
	if(sysctlbyname("vm.swapusage", &swap, &len, NULL, 0) == 0)
	{
		*totalSwap = swap.xsu_total;
		*freeSwap = swap.xsu_avail;
	}
#elif defined(BGI_HAIKU)
	system_info si;
	if(get_system_info(&si) != B_OK)
		return 0;
	*totalPhys = si.max_pages * (uint64_t)B_PAGE_SIZE;
	*freePhys = (si.max_pages - si.used_pages) * (uint64_t)B_PAGE_SIZE;
	*totalSwap = si.max_swap_pages * (uint64_t)B_PAGE_SIZE;
	*freeSwap = si.free_swap_pages * (uint64_t)B_PAGE_SIZE;
#else
	struct sysinfo si;
	if(sysinfo(&si) != 0)
		return 0;
	*totalPhys = (uint64_t)si.totalram * si.mem_unit;
	*freePhys = (uint64_t)si.freeram * si.mem_unit;
	*totalSwap = (uint64_t)si.totalswap * si.mem_unit;
	*freeSwap = (uint64_t)si.freeswap * si.mem_unit;
#endif
	return 1;
}

// GlobalMemoryStatus, each figure cut to 32 bits; the virtual space is a 32-bit process's 2 GB
void OS_MemoryStatus(OpenBGI_MemoryStatus_t* m)
{
	uint64_t totalPhys, freePhys, totalSwap, freeSwap;
	memset(m, 0, sizeof *m);
	if(!MemoryFigures(&totalPhys, &freePhys, &totalSwap, &freeSwap))
		return;
	m->totalPhys = (uint32_t)BGI_MIN(totalPhys, 0xffffffffu);
	m->availPhys = (uint32_t)BGI_MIN(freePhys, 0xffffffffu);
	m->totalPageFile = (uint32_t)BGI_MIN(totalSwap, 0xffffffffu);
	m->availPageFile = (uint32_t)BGI_MIN(freeSwap, 0xffffffffu);
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

#if defined(OS_HAVE_CPUID)
// CPUID through the compiler's intrinsic; the leaf's range (basic / extended) is checked against the highest supported
int OS_Cpuid(uint32_t leaf, uint32_t out[4])
{
	unsigned a, b, c, d;
	unsigned max = __get_cpuid_max(leaf & 0x80000000u, NULL);
	if(max == 0 || leaf > max)
		return 0;
	__cpuid_count(leaf, 0, a, b, c, d); // (the first of a leaf that has several: leaf 4's level 1 data cache)
	out[0] = a;
	out[1] = b;
	out[2] = c;
	out[3] = d;
	return 1;
}

// RDTSC after a serialising CPUID, as the original's CPU speed measurement does
uint64_t OS_ReadTsc(void)
{
#if defined(BGI_HEADLESS)
	return (uint64_t)OsHeadless_Now() * 2000000u; // the test host's 2 GHz machine on its own clock
#else
	unsigned a, b, c, d;
	__cpuid(0, a, b, c, d); // the original serialises with CPUID first
	return __rdtsc();
#endif
}
#endif // OS_HAVE_CPUID

// the monotonic clock in nanoseconds stands in for the performance counter
int OS_PerfCounterNs(int64_t* out)
{
	struct timespec ts;
	if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	*out = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
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

// Windows XP (NT 5.1, build 2600) with Service Pack 3, so that the engine takes its NT code paths (IOCTLs, the registry)
void OS_VersionEx(uint32_t out[4], char* csd, size_t n)
{
	out[0] = 5;
	out[1] = 1;
	out[2] = 2600;
	out[3] = 2; // VER_PLATFORM_WIN32_NT
	if(n)
		snprintf(csd, n, "Service Pack 3");
}

// GlobalMemoryStatusEx: the physical memory in full
void OS_MemoryStatusEx(uint64_t* totalPhys, uint64_t* availPhys)
{
	uint64_t totalSwap, freeSwap;
	if(!MemoryFigures(totalPhys, availPhys, &totalSwap, &freeSwap))
		*totalPhys = *availPhys = 0;
}

/* What the process holds of the memory: its resident pages, from the
 * kernel's /proc/self/statm (Linux, Android).  macOS and Haiku do not say
 * here yet. */
uint64_t OS_ProcessMemory(void)
{
#if defined(BGI_MACOS) || defined(BGI_HAIKU)
	return 0;
#else
	unsigned long total = 0, resident = 0;
	FILE* f = fopen("/proc/self/statm", "r");
	int got;
	if(!f)
		return 0;
	got = fscanf(f, "%lu %lu", &total, &resident);
	fclose(f);
	return got == 2 ? (uint64_t)resident * (uint64_t)sysconf(_SC_PAGESIZE) : 0;
#endif
}

// "X11 display" (Android: "no display") with a zero driver version
int OS_DisplayAdapter(char* desc, size_t n, uint32_t version[4])
{
	// no Direct3D here: the X server (on a Mac, Metal) is the "adapter"
	if(n)
#if defined(BGI_ANDROID)
		snprintf(desc, n, "no display");
#elif defined(BGI_MACOS)
		snprintf(desc, n, "Metal display");
#else
		snprintf(desc, n, "X11 display");
#endif
	version[0] = version[1] = version[2] = version[3] = 0;
	return 1;
}

// ---- the desktop -------------------------------------------------------------------------------

/* the desktop's opener of documents and links: xdg-open, or the open of
 * Haiku and macOS (which hand the file to its preferred application);
 * Android has none a command-line program can use */
#if defined(BGI_HAIKU) || defined(BGI_MACOS)
#define DOCUMENT_OPENER "open"
#elif !defined(BGI_ANDROID)
#define DOCUMENT_OPENER "xdg-open"
#endif

/* A web link or a document to the desktop's opener, as its one argument
 * (no shell reads it): a link as it is, a file by its path here, which
 * never begins like an option.  The opener is left to itself, a
 * grandchild that init inherits; 1 when it could be started.  The test
 * host opens nothing and says what it would have. */
int OS_OpenDocument(const char* target)
{
#if defined(BGI_HEADLESS)
	char utf[0x1000];
	OsCommon_SjisToUtf8(target, utf, sizeof utf);
	fprintf(stderr, "headless: a document to open: %s\n", utf);
	return 1;
#elif defined(DOCUMENT_OPENER)
	char arg[PATH_MAX + 2];
	int status = 0;
	pid_t pid;
	if(strncasecmp(target, "http://", 7) == 0 || strncasecmp(target, "https://", 8) == 0)
		OsCommon_SjisToUtf8(target, arg, sizeof arg);
	else
	{
		char path[PATH_MAX];
		OsPosix_Path(target, path, sizeof path);
		snprintf(arg, sizeof arg, "%s%s", path[0] == '/' ? "" : "./", path);
	}
	pid = fork();
	if(pid < 0)
		return 0;
	if(pid == 0)
	{ // the child starts the opener and ends at once, so that nobody has to wait for the opener
		pid_t opener = fork();
		if(opener == 0)
		{
			int null = open("/dev/null", O_RDWR);
			if(null >= 0)
			{
				dup2(null, 0);
				dup2(null, 1);
				dup2(null, 2);
			}
			execlp(DOCUMENT_OPENER, DOCUMENT_OPENER, arg, (char*)NULL);
			_exit(127);
		}
		_exit(opener < 0 ? 1 : 0);
	}
	while(waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
	BGI_UNUSED(target);
	return 0;
#endif
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
int OS_CreateShortcut(const char* linkPath, const char* target, const char* args)
{
	BGI_UNUSED(linkPath);
	BGI_UNUSED(target);
	BGI_UNUSED(args);
	return 0; // no shortcuts on this platform; the scripts take the failure
}
