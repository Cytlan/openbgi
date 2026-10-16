/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * main.c - the process entry point
 *
 * The original is WinMain; Engine_Main in engine.c (bgi/engine.h) is that
 * function without the Windows glue.  This file only turns the host's
 * entry point into a call of Engine_Main with the command line as one
 * string, which the engine parses itself (Engine_Main strips the
 * reimplementation's "--" options, Boot_ParseCmdLine understands the
 * original's switches, e.g. "Execute as a launcher.").
 */
#include "bgi/engine.h"
#include "bgi/os.h"

#if defined(BGI_WIN32)
#include "bgi/os_win32.h" // windows.h, and the back end's command line

/* the Windows entry point.  WinMain's own command line is in the system's
 * code page, which may not be able to spell a path; the back end converts
 * the process's wide one instead (OsWin32_CommandLine: without the
 * program name, in the engine's encoding). */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int show)
{
	char cmd[0x1000];
	int r;
	BGI_UNUSED(hInst);
	BGI_UNUSED(hPrev);
	BGI_UNUSED(cmdLine);
	BGI_UNUSED(show);
	if(!OS_Init())
		return -1;
	OsWin32_CommandLine(cmd, sizeof cmd);
	r = Engine_Main(cmd);
	OS_Shutdown();
	return r;
}

#else

// the POSIX entry point: the arguments are rejoined into one command line
int main(int argc, char** argv)
{
	// the arguments joined by single spaces, as GetCommandLine would give them without the program name;
	// arguments that do not fit the buffer are dropped
	char cmdLine[0x1000];
	size_t len = 0;
	int i, r;
	cmdLine[0] = 0;
	for(i = 1; i < argc; i++)
	{
		size_t n = strlen(argv[i]);
		if(len + n + 2 >= sizeof cmdLine)
			break;
		if(len)
			cmdLine[len++] = ' ';
		memcpy(cmdLine + len, argv[i], n + 1);
		len += n;
	}
	OS_SetArgv0(argv[0]);
	if(!OS_Init())
		return -1;
	r = Engine_Main(cmdLine);
	OS_Shutdown();
	return r;
}

#endif
