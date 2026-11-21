/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dialogs.m - the macOS back end's message boxes and pickers
 *             (src/host/os.h)
 *
 * A message box is the system's alert, a file or a folder is chosen in
 * the system's panels; each is modal, with the modal tick kept going
 * (app.m).  The scripts' dialogs - the input fields, the player's
 * profile - are not here: the engine draws them in the frame
 * (OS_HostCaps has no OPENBGI_CAP_DIALOGS), and the calls for them answer
 * "cancelled" for a run that asks for the host's all the same
 * (--dialogs=host).  There is no game chooser either: the launcher is the
 * way to choose a game.
 */
#import "macos_internal.h"

// the buttons of a box: their labels and the ids they answer, the first one the default
static int Buttons(uint32_t flags, const char* label[2], int ids[2])
{
	switch(flags & 0xf)
	{
		case OPENBGI_MB_OKCANCEL:
			label[0] = "OK", ids[0] = OPENBGI_IDOK;
			label[1] = "Cancel", ids[1] = OPENBGI_IDCANCEL;
			return 2;
		case OPENBGI_MB_YESNO:
			label[0] = "Yes", ids[0] = OPENBGI_IDYES;
			label[1] = "No", ids[1] = OPENBGI_IDNO;
			return 2;
		case OPENBGI_MB_RETRYCANCEL:
			label[0] = "Retry", ids[0] = OPENBGI_IDRETRY;
			label[1] = "Cancel", ids[1] = OPENBGI_IDCANCEL;
			return 2;
		default:
			label[0] = "OK", ids[0] = OPENBGI_IDOK;
			return 1;
	}
}

// MessageBox: the id of the button pressed
int OS_MessageBox(const char* text, const char* caption, uint32_t flags)
{
	const char* label[2] = {NULL, NULL};
	int ids[2] = {0, 0}, n = Buttons(flags, label, ids), def = (flags & OPENBGI_MB_DEFBUTTON2) && n > 1 ? 1 : 0, result;
	if(getenv("BGI_MSGBOX_STDERR"))
	{ // asked to, for unattended runs: say it on stderr, take the default button
		char utf8[0x800];
		OsCommon_SjisToUtf8(caption ? caption : "", utf8, sizeof utf8);
		fprintf(stderr, "[%s] ", utf8);
		OsCommon_SjisToUtf8(text ? text : "", utf8, sizeof utf8);
		fprintf(stderr, "%s\n", utf8);
		return ids[def];
	}
	Mac_AppInit();
	@autoreleasepool
	{
		NSAlert* alert = [[NSAlert alloc] init];
		NSString* title = Mac_Text(caption);
		NSModalResponse r;
		int i;
		alert.messageText = title.length > 0 ? title : @"OpenBGI";
		alert.informativeText = Mac_Text(text);
		switch(flags & 0xf0)
		{
			case OPENBGI_MB_ICONHAND: alert.alertStyle = NSAlertStyleCritical; break;
			case OPENBGI_MB_ICONWARNING: alert.alertStyle = NSAlertStyleWarning; break;
			default: alert.alertStyle = NSAlertStyleInformational; break;
		}
		for(i = 0; i < n; i++)
		{
			NSButton* b = [alert addButtonWithTitle:[NSString stringWithUTF8String:label[i]]];
			// Return presses the default button, Escape the last one of two
			b.keyEquivalent = i == def ? @"\r" : (n > 1 && i == n - 1 ? @"\033" : @"");
		}
		[NSApp activateIgnoringOtherApps:YES];
		r = [alert runModal];
		result = r == NSAlertSecondButtonReturn && n > 1 ? ids[1] : ids[0];
	}
	return result;
}

// ---- the scripts' dialogs: the engine's, in the frame --------------------------------------------

int OS_DialogInput1(const char* title, const char* initial, int maxLen, int digits, char* out)
{
	BGI_UNUSED(title);
	BGI_UNUSED(initial);
	BGI_UNUSED(maxLen);
	BGI_UNUSED(digits);
	BGI_UNUSED(out);
	return 0;
}

int OS_DialogInput2(const char* title, const char* label1, const char* initial1, int maxLen1, int digits1, char* out1,
	const char* label2, const char* initial2, int maxLen2, int digits2, char* out2, int compact)
{
	BGI_UNUSED(title);
	BGI_UNUSED(label1);
	BGI_UNUSED(initial1);
	BGI_UNUSED(maxLen1);
	BGI_UNUSED(digits1);
	BGI_UNUSED(out1);
	BGI_UNUSED(label2);
	BGI_UNUSED(initial2);
	BGI_UNUSED(maxLen2);
	BGI_UNUSED(digits2);
	BGI_UNUSED(out2);
	BGI_UNUSED(compact);
	return 0;
}

int OS_DialogProfile(char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month, int32_t* day)
{
	BGI_UNUSED(surname);
	BGI_UNUSED(givenName);
	BGI_UNUSED(nickname);
	BGI_UNUSED(pronoun);
	BGI_UNUSED(month);
	BGI_UNUSED(day);
	return 0;
}

// no dialog to choose a game with: -1, "no display for it" (the launcher chooses)
int OS_GameChooserDialog(OpenBGI_GameChooser_t* l)
{
	BGI_UNUSED(l);
	return -1;
}

// ---- files and folders ---------------------------------------------------------------------------

/* The extensions of a filter's pattern ("*.txt;*.dat") into `types`; a
 * pattern that takes every file ("*.*", "*") empties the list for good:
 * 0 then. */
static int AddExtensions(NSMutableArray<NSString*>* types, const char* pattern)
{
	NSArray<NSString*>* parts = [Mac_Text(pattern) componentsSeparatedByString:@";"];
	for(NSString* part in parts)
	{
		NSString* p = [part stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
		NSString* ext = p.pathExtension;
		if(p.length == 0)
			continue;
		if(ext.length == 0 || [ext isEqualToString:@"*"])
			return 0;
		[types addObject:ext];
	}
	return 1;
}

// GetOpenFileName / GetSaveFileName with a list of filters; 1 when a file was chosen
int OS_FileDialogN(int save, char* path, size_t n, const char* const* descs, const char* const* pats, int count,
	const char* defExt, const char* initialDir, const char* title)
{
	int chosen = 0;
	BGI_UNUSED(descs);
	Mac_AppInit();
	@autoreleasepool
	{
		NSSavePanel* panel = save ? [NSSavePanel savePanel] : [NSOpenPanel openPanel];
		NSMutableArray<NSString*>* types = [NSMutableArray array];
		NSString* name = Mac_Text(OsCommon_FilePart(path ? path : ""));
		int i, all = 0;
		for(i = 0; i < count && !all; i++)
			if(pats && pats[i] && !AddExtensions(types, pats[i]))
				all = 1;
		if(!all && types.count == 0 && defExt && *defExt)
			[types addObject:Mac_Text(defExt)];
		if(!all && types.count > 0)
			panel.allowedFileTypes = types;
		if(title && *title)
			panel.message = Mac_Text(title);
		if(initialDir && *initialDir)
		{
			char dir[PATH_MAX];
			NSString* native;
			OsPosix_Path(initialDir, dir, sizeof dir);
			native = [NSString stringWithUTF8String:dir];
			if(native)
				panel.directoryURL = [NSURL fileURLWithPath:native isDirectory:YES];
		}
		if(save && name.length > 0)
			panel.nameFieldStringValue = name;
		[NSApp activateIgnoringOtherApps:YES];
		if([panel runModal] == NSModalResponseOK && panel.URL && panel.URL.path)
		{
			Mac_FromString(panel.URL.path, path, n);
			chosen = 1;
		}
	}
	return chosen;
}

/* the same with one filter, "<description> <pattern>" with its last space
 * between the two */
int OS_FileDialog(int save, char* path, size_t n, const char* filter, const char* defExt, const char* initialDir, const char* title)
{
	const char* pattern = filter ? strrchr(filter, ' ') : NULL;
	pattern = pattern ? pattern + 1 : filter;
	return OS_FileDialogN(save, path, n, NULL, &pattern, pattern ? 1 : 0, defExt, initialDir, title);
}

/* SHBrowseForFolder: a folder in the system's panel.  `initial` and the
 * result are native (UTF-8) paths; 1 when a folder was chosen. */
int OS_BrowseFolder(char* path, size_t n, const char* title, const char* initial)
{
	int chosen = 0;
	Mac_AppInit();
	@autoreleasepool
	{
		NSOpenPanel* panel = [NSOpenPanel openPanel];
		NSString* start = initial && *initial ? [NSString stringWithUTF8String:initial] : nil;
		panel.canChooseFiles = NO;
		panel.canChooseDirectories = YES;
		panel.canCreateDirectories = YES;
		panel.allowsMultipleSelection = NO;
		if(title && *title)
			panel.message = Mac_Text(title);
		if(start)
			panel.directoryURL = [NSURL fileURLWithPath:start isDirectory:YES];
		[NSApp activateIgnoringOtherApps:YES];
		if([panel runModal] == NSModalResponseOK && panel.URL && panel.URL.fileSystemRepresentation)
		{
			snprintf(path, n, "%s", panel.URL.fileSystemRepresentation);
			chosen = 1;
		}
	}
	return chosen;
}
