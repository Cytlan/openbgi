/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * main.c - the entry point on Windows
 *
 * WinMain's own command line is in the system's code page, which may not
 * be able to spell a path; the back end converts the process's wide one
 * instead (OsWin32_CommandLine: without the program name, in the engine's
 * encoding).  The engine parses it itself (OpenBGI_Start strips the
 * reimplementation's "--" options, Boot_ParseCmdLine understands the
 * original's switches), and the host of src/host/os_host.c runs it
 * (OS_RunMain).
 *
 * A start that is for one of the tools - "openbgi dump ...", or the
 * program under a tool's name - never gets that far (tools/tools.h); a
 * tool sees the arguments the C library made of the command line.  The
 * build links this twice: the program for the desktop, which has no
 * console, and the same as a console program ("openbgi.com"), which is
 * the one a command line finds first and the one the tools' output is
 * seen from.
 */
#include "host/os_win32.h" // windows.h, and the back end's command line
#include "host/os.h"
#include "host/os_common.h"
#include "host/os_gl.h"
#ifdef BGI_TOOLS
#include "tools/tools.h"
#endif

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int show)
{
	char cmd[0x1000];
	BGI_UNUSED(hInst);
	BGI_UNUSED(hPrev);
	BGI_UNUSED(cmdLine);
	BGI_UNUSED(show);
	OsWin32_DeclareDpiAware(); // before any window: the program follows the screen's scale itself
#ifdef BGI_TOOLS
	{
		int status;
		if(Tools_Run(__argc, __argv, &status))
			return status;
	}
#endif
	OsWin32_CommandLine(cmd, sizeof cmd);
#ifdef BGI_LAUNCHER
	OS_UseLauncher(gOsLauncherSources, gOsLauncherSourcesSize); // started without a game: the launcher (docs/dev/launcher.md)
#endif
#ifdef BGI_ICON
	OS_UseIcon(gOsIcon, gOsIconSize); // the window's icon (docs/dev/building.md)
#endif
#ifdef BGI_SHADERS
	OS_UseShaders(gOsShaders, gOsShadersSize); // the shader files of the OpenGL output's filters (docs/dev/os_layer.md)
#endif
	return OS_RunMain(cmd);
}
