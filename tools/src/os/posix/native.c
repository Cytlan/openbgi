/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * native.c - the POSIX back end's native file system services for the
 *            command-line tools (inc/bgi/os.h: OS_NativeStat,
 *            OS_NativeMkdir, OS_NativeRemove, OS_NativeList,
 *            OS_NativeSeek, OS_NativeAbsolute, OS_NativeArgs) and
 *            its worker processes (OS_WorkerLimit, OS_RunWorkers)
 *
 * A native path is a POSIX path in UTF-8 here, so these are the system
 * calls themselves.  The workers are forked children.
 */
#include "posix_internal.h"

int OS_NativeStat(const char* path, OsNativeStat_t* out)
{
	struct stat st;
	if(stat(path, &st) != 0)
		return OS_NATIVE_NONE;
	if(out)
	{
		out->size = (uint64_t)st.st_size;
		out->mtime = (int64_t)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000;
	}
	return S_ISDIR(st.st_mode) ? OS_NATIVE_DIR : OS_NATIVE_FILE;
}

int OS_NativeMkdir(const char* path)
{
	if(mkdir(path, 0777) == 0)
		return 1;
	return OS_NativeStat(path, NULL) == OS_NATIVE_DIR;
}

int OS_NativeRemove(const char* path)
{
	return unlink(path) == 0 || OS_NativeStat(path, NULL) == OS_NATIVE_NONE;
}

int OS_NativeList(const char* dir, OsNativeListFn fn, void* ctx)
{
	DIR* d = opendir(dir[0] ? dir : ".");
	struct dirent* e;
	int count = 0;
	if(!d)
		return -1;
	while((e = readdir(d)) != NULL)
	{
		char path[PATH_MAX];
		if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		snprintf(path, sizeof path, "%s/%s", dir[0] ? dir : ".", e->d_name);
		fn(ctx, e->d_name, OS_NativeStat(path, NULL) == OS_NATIVE_DIR);
		count++;
	}
	closedir(d);
	return count;
}

int OS_NativeSeek(FILE* f, uint64_t pos)
{
	return fseeko(f, (off_t)pos, SEEK_SET) == 0;
}

void OS_NativeAbsolute(const char* path, char* out, size_t n)
{
	char cwd[PATH_MAX];
	// the current directory in front of a relative path; "." and ".." components stay as they are
	if(path[0] == '/' || !getcwd(cwd, sizeof cwd))
		snprintf(out, n, "%s", path);
	else if(strcmp(path, ".") == 0 || !path[0])
		snprintf(out, n, "%s", cwd);
	else
		snprintf(out, n, "%s/%s", cwd, path[0] == '.' && path[1] == '/' ? path + 2 : path);
}

int OS_WorkerLimit(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return n < 1 ? 1 : n > 64 ? 64
							  : (int)n;
}

int OS_RunWorkers(int n, int (*fn)(int index, void* ctx), void* ctx)
{
	pid_t* pids;
	int i, failed = 0;
	if(n <= 1)
		return n == 1 ? fn(0, ctx) != 0 : 0;
	pids = (pid_t*)calloc((size_t)n, sizeof *pids);
	fflush(NULL); // nothing buffered may be written twice, by the parent and by a child
	for(i = 0; i < n; i++)
	{
		pids[i] = pids ? fork() : -1;
		if(pids && pids[i] == 0)
		{
			int rc = fn(i, ctx);
			fflush(NULL);
			_exit(rc ? 1 : 0); // without the parent's exit handlers
		}
		if(!pids || pids[i] < 0)
		{ // no more processes: this share runs here
			failed += fn(i, ctx) != 0;
			fflush(NULL);
		}
	}
	for(i = 0; pids && i < n; i++)
	{
		int status = 0;
		if(pids[i] > 0 && (waitpid(pids[i], &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0))
			failed++;
	}
	free(pids);
	return failed;
}

void OS_NativeArgs(int* argc, char*** argv) // main's arguments are native already
{
	BGI_UNUSED(argc);
	BGI_UNUSED(argv);
}
