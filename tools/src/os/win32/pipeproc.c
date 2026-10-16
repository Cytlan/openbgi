/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * pipeproc.c - a helper program with a pipe from its standard output
 *              (inc/bgi/os.h: OS_PipeProcStart, OS_PipeProcRead,
 *              OS_PipeProcStop, OS_PathNative)
 *
 * CreateProcessW with an inheritable pipe as the child's standard output
 * and NUL as its standard error, no console window.  The arguments are
 * converted like every other engine string (wide.c) and quoted the way
 * the C runtime's argv parsing expects.  A bare program name is tried
 * next to the engine's binary first (an ffmpeg.exe dropped beside
 * bgi.exe), then as CreateProcess resolves it (the PATH).
 */
#include "win32_internal.h"

struct OsPipeProc
{
	HANDLE process;
	HANDLE read; // the pipe's read end
};

// does the argument need quoting on the command line (empty, or a blank or quote in it)?
static int NeedsQuotes(const WCHAR* arg)
{
	size_t i;
	if(arg[0] == 0)
		return 1;
	for(i = 0; arg[i]; i++)
		if(arg[i] == L' ' || arg[i] == L'\t' || arg[i] == L'"')
			return 1;
	return 0;
}

// append `arg` to the command line, quoted when it needs it (backslashes before a quote doubled)
static void AppendArg(WCHAR* cmd, size_t cap, const WCHAR* arg)
{
	size_t len = wcslen(cmd), i, bs;
	int quote = NeedsQuotes(arg);
	if(len && len + 1 < cap)
		cmd[len++] = L' ';
	if(quote && len + 1 < cap)
		cmd[len++] = L'"';
	for(i = 0, bs = 0; arg[i] && len + 2 < cap; i++)
	{
		if(arg[i] == L'\\')
		{
			bs++;
			cmd[len++] = arg[i];
			continue;
		}
		if(arg[i] == L'"')
		{ // the backslashes before a quote are doubled, then the quote escaped
			size_t k;
			for(k = 0; k < bs && len + 2 < cap; k++)
				cmd[len++] = L'\\';
			cmd[len++] = L'\\';
		}
		bs = 0;
		cmd[len++] = arg[i];
	}
	if(quote)
	{
		size_t k;
		for(k = 0; k < bs && len + 2 < cap; k++)
			cmd[len++] = L'\\';
		if(len + 1 < cap)
			cmd[len++] = L'"';
	}
	cmd[len] = 0;
}

OsPipeProc_t* OS_PipeProcStart(const char* const* argv)
{
	static WCHAR cmd[0x4000];
	SECURITY_ATTRIBUTES sa;
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	HANDLE rd = NULL, wr = NULL, nul;
	OsPipeProc_t* p;
	WCHAR* w;
	int i;
	if(!argv || !argv[0] || !argv[0][0])
		return NULL;
	cmd[0] = 0;
	for(i = 0; argv[i]; i++)
	{
		w = Win32_ToWideDup(argv[i]);
		if(!w)
			return NULL;
		if(i == 0 && !strchr(argv[0], '\\') && !strchr(argv[0], '/'))
		{ // a bare name: the copy beside the binary wins over the PATH
			char dir[0x210], prog[0x220];
			WCHAR* wp;
			OS_BinaryDir(dir, sizeof dir);
			snprintf(prog, sizeof prog, "%s%s", dir, argv[0]);
			wp = Win32_ToWideDup(prog);
			if(wp && GetFileAttributesW(wp) != INVALID_FILE_ATTRIBUTES)
			{
				free(w);
				w = wp;
			}
			else
				free(wp);
		}
		AppendArg(cmd, BGI_COUNTOF(cmd), w);
		free(w);
	}
	memset(&sa, 0, sizeof sa);
	sa.nLength = sizeof sa;
	sa.bInheritHandle = TRUE;
	if(!CreatePipe(&rd, &wr, &sa, 0))
		return NULL;
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0); // only the write end goes to the child
	nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = wr;
	si.hStdError = nul != INVALID_HANDLE_VALUE ? nul : wr;
	memset(&pi, 0, sizeof pi);
	if(!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
	{
		CloseHandle(rd);
		CloseHandle(wr);
		if(nul != INVALID_HANDLE_VALUE)
			CloseHandle(nul);
		return NULL;
	}
	CloseHandle(wr); // the child holds its own copy: the read end sees its end when the child closes
	if(nul != INVALID_HANDLE_VALUE)
		CloseHandle(nul);
	CloseHandle(pi.hThread);
	p = (OsPipeProc_t*)calloc(1, sizeof *p);
	p->process = pi.hProcess;
	p->read = rd;
	return p;
}

int OS_PipeProcRead(OsPipeProc_t* p, void* buf, int n)
{
	DWORD got = 0;
	if(!p || n <= 0)
		return -1;
	if(!ReadFile(p->read, buf, (DWORD)n, &got, NULL))
		return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
	return (int)got;
}

void OS_PipeProcStop(OsPipeProc_t* p)
{
	if(!p)
		return;
	CloseHandle(p->read);
	if(WaitForSingleObject(p->process, 0) == WAIT_TIMEOUT)
		TerminateProcess(p->process, 1);
	CloseHandle(p->process);
	free(p);
}

// an engine path is the native path on Windows (the aliases of wide.c are resolved when it is converted)
void OS_PathNative(const char* path, char* out, size_t n)
{
	snprintf(out, n, "%s", path);
}
