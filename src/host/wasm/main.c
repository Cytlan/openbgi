/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * main.c - the entry point in the browser (and in the wasm-sim build)
 *
 * The page hands the game's arguments to main() (Module.arguments,
 * tools/wasm/shell.html); they are joined into one command line as the
 * original's WinMain gets it (OS_JoinArgs) and the host of
 * src/host/os_host.c runs it (OS_RunMain).  The requests the engine makes
 * are answered by the back end's dialogs (src/host/wasm/window.c).
 */
#include "host/os.h"

int main(int argc, char** argv)
{
	char cmdLine[0x1000];
	OS_JoinArgs(argc, argv, cmdLine, sizeof cmdLine);
	OS_SetArgv0(argv[0]);
	return OS_RunMain(cmdLine);
}
