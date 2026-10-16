/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pipeproc.c - a helper program with a pipe from its standard output
 *              (inc/bgi/os.h: OS_PipeProcStart, OS_PipeProcRead,
 *              OS_PipeProcStop, OS_PathNative)
 *
 * posix_spawn, so that nothing of the engine's threads is copied: the
 * child gets the pipe's write end as its standard output and /dev/null
 * as its standard error.  A bare program name is tried next to the
 * engine's binary first (an ffmpeg dropped beside bgi), then through the
 * PATH as posix_spawnp resolves it.
 */
#include "posix_internal.h"

#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>

extern char** environ;

struct OsPipeProc
{
	pid_t pid;
	int fd; // the read end
};

OsPipeProc_t* OS_PipeProcStart(const char* const* argv)
{
	OsPipeProc_t* p;
	posix_spawn_file_actions_t fa;
	int fds[2], r;
	char prog[PATH_MAX];
	const char* file;
	pid_t pid;
	if(!argv || !argv[0] || !argv[0][0])
		return NULL;
	// a bare name: the copy beside the binary wins over the PATH
	file = argv[0];
	if(!strchr(file, '/'))
	{
		char dir[PATH_MAX];
		OS_BinaryDir(dir, sizeof dir);
		snprintf(prog, sizeof prog, "%s%s", dir, file);
		if(access(prog, X_OK) == 0)
			file = prog;
	}
	if(pipe(fds) != 0)
		return NULL;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addclose(&fa, fds[0]);
	posix_spawn_file_actions_addclose(&fa, fds[1]);
	r = posix_spawnp(&pid, file, &fa, NULL, (char* const*)argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	if(r != 0)
	{
		close(fds[0]);
		return NULL;
	}
	p = (OsPipeProc_t*)calloc(1, sizeof *p);
	p->pid = pid;
	p->fd = fds[0];
	return p;
}

int OS_PipeProcRead(OsPipeProc_t* p, void* buf, int n)
{
	ssize_t r;
	if(!p || n <= 0)
		return -1;
	do
		r = read(p->fd, buf, (size_t)n);
	while(r < 0 && errno == EINTR);
	return r < 0 ? -1 : (int)r;
}

void OS_PipeProcStop(OsPipeProc_t* p)
{
	int status;
	if(!p)
		return;
	close(p->fd);
	if(waitpid(p->pid, &status, WNOHANG) == 0)
	{ // still running: the pipe is gone, so it would die on its next write; do not wait for that
		kill(p->pid, SIGKILL);
		waitpid(p->pid, &status, 0);
	}
	free(p);
}

// the engine's Shift-JIS Windows path as a UTF-8 POSIX path (case-insensitive lookup included)
void OS_PathNative(const char* path, char* out, size_t n)
{
	OsPosix_Path(path, out, n);
}
