/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * main.c - the entry point on POSIX systems (Linux, macOS, Android, Haiku)
 *
 * The original is WinMain, which gets its command line as one string.
 * Here the arguments are joined into one (OS_JoinArgs), which the engine
 * parses itself (OpenBGI_Start strips the reimplementation's "--" options,
 * Boot_ParseCmdLine understands the original's switches, e.g. "Execute as
 * a launcher."), and the host of src/host/os_host.c runs it (OS_RunMain).
 *
 * A start that is for one of the tools - "openbgi dump ...", or the
 * program under a tool's name - never gets that far (tools/tools.h).
 */
#include "host/os.h"
#include "host/os_common.h"
#include "host/os_gl.h"
#ifdef BGI_TOOLS
#include "tools/tools.h"
#endif

int main(int argc, char** argv)
{
	char cmdLine[0x1000];
	OS_SetArgv0(argv[0]);
#ifdef BGI_TOOLS
	{
		int status;
		if(Tools_Run(argc, argv, &status))
			return status;
	}
#endif
	OS_JoinArgs(argc, argv, cmdLine, sizeof cmdLine);
#ifdef BGI_LAUNCHER
	OS_UseLauncher(gOsLauncherSources, gOsLauncherSourcesSize); // started without a game: the launcher (docs/dev/launcher.md)
#endif
#ifdef BGI_ICON
	OS_UseIcon(gOsIcon, gOsIconSize); // the window's icon (docs/dev/building.md)
#endif
#ifdef BGI_SHADERS
	OS_UseShaders(gOsShaders, gOsShadersSize); // the shader files of the OpenGL output's filters (docs/dev/os_layer.md)
#endif
	return OS_RunMain(cmdLine);
}
