/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * process.c - starting other programs from the engine ("80 E0", "80 E1",
 *             "80 E2", "80 E3", the registration and uninstaller runs) and
 *             the audio availability check of the start-up; declared in
 *             bgi/install.h
 *
 * RunProgram is the one routine behind every program start: the
 * instructions and the installer differ only in its flags (whether the
 * engine waits, hides its window, lets the user retry a missing disc).
 */
#include "bgi/install.h"
#include "bgi/file.h"
#include "bgi/error.h"
#include "bgi/display.h"
#include "bgi/engine.h"
#include "bgi/version.h"
#include "bgi/msg.h"
#include "bgi/os.h"

/* Start "<dir>\<file>" (or "<base dir><file>" when dir is NULL) as a new
 * process.  While the file's drive is not ready or the process cannot be
 * created, `msg` is shown ("Insert the disc"): with allowRetry the box is
 * OK / Cancel and Cancel asks whether to abort, without it the box is
 * informational and the call fails; a NULL msg fails silently.  With
 * `wait` the engine waits for the process (hiding the main window first
 * when hideWindow is set, and pumping the window's messages meanwhile)
 * and, when waitUninstaller is set, also until the uninstaller's mutex is
 * gone.  1 when the process was started, 0 when not.
 * "80 E0" calls it with wait and allowRetry, "80 E2" (the uninstaller)
 * with hideWindow, wait and waitUninstaller, "80 E1" after the shutdown
 * with none of them. */
int RunProgram(const char* dir, const char* file, const char* msg, int hideWindow, int wait, int allowRetry,
	int waitUninstaller)
{
	char cmd[0x104];
	OsProcess_t* p;

	if(dir)
		sprintf(cmd, "%s\\%s", dir, file);
	else
		sprintf(cmd, "%s%s", gBaseDir, file);

	for(;;)
	{
		p = DriveReady(cmd) ? OS_ProcessStart(cmd) : NULL;
		if(p)
			break;
		if(!msg)
			return 0;
		if(!allowRetry)
		{
			MsgBox(msg, MSG_NOTICE, OS_MB_ICONINFO);
			return 0;
		}
		if(MsgBox(msg, MSG_NOTICE, OS_MB_OKCANCEL | OS_MB_ICONINFO) != OS_IDOK)
		{
			if(MsgBox(MSG_ASK_ABORT, MSG_CONFIRM, OS_MB_YESNO | OS_MB_ICONQUESTION | OS_MB_DEFBUTTON2) == OS_IDYES)
				return 0;
		}
	}
	if(wait)
	{
		if(hideWindow)
			ShowMainWindow(0);
		OS_ProcessWaitIdle(p);
		while(!OS_ProcessWait(p, 10)) // 10 ms slices, the window stays responsive in between
			PumpMessages();
		OS_ProcessClose(p);
		if(waitUninstaller)
		{
			char mutexName[0x140];
			// the uninstaller holds a mutex named after the product while it runs
			snprintf(mutexName, sizeof mutexName, "Uninstaller for %s is executing.", Engine_ProductName());
			while(OS_NamedMutexExists(mutexName))
				OS_SleepMs(100);
		}
		ShowMainWindow(1);
	}
	else
	{
		OS_ProcessClose(p);
	}
	return 1;
}

// "80 E3": open a file or URL with its associated program (ShellExecute "open"); the OS layer's result
int ShellOpen(const char* path)
{
	return OS_ShellExecute(path, NULL, NULL);
}

// Whether audio output is available (the start-up refuses to run without it, except in launcher mode).
int DSound8_IsAvailable(void)
{
	return OS_AudioAvailable();
}
