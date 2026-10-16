/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dialogs.c - the script's dialogs for the POSIX back end (inc/bgi/os.h):
 *             the original's dialog resources 0x71 (one text field),
 *             0x72 (two labelled fields) and 0x77 (the player's name and
 *             birthday), built on the dialog toolkit; they return 0
 *             (cancelled) without a display.  Also the folder browser
 *             (a list of subdirectories on the same toolkit, in place of
 *             SHBrowseForFolder) and the launcher's game chooser.  The
 *             file dialogs are not provided.
 *
 * Each dialog is laid out here to resemble its resource; the results go
 * back through the 0x100-byte buffers of os.h only when the dialog was
 * confirmed (button id 1).
 */
#include "ui_internal.h"

#include <dirent.h>
#include <limits.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define DLG_FIELD_W 320 // pixels: the text fields of the input dialogs

// dialog 0x71: a caption, one field, OK / cancel
int OS_DialogInput1(const char* title, const char* initial, int maxLen, char* out)
{
	UiDialog_t d;
	int r, y = UI_MARGIN;
	if(!X11_Open())
		return 0;
	memset(&d, 0, sizeof d);
	d.w = DLG_FIELD_W + 2 * UI_MARGIN;
	Ui_FieldAdd(&d, UI_MARGIN, y, DLG_FIELD_W, initial, maxLen);
	d.fields[0].f.selAll = 0; // the original leaves the caret at the end (EM_SETSEL len, len)
	y += UI_FIELD_H + UI_MARGIN;
	Ui_ButtonAdd(&d, d.w - UI_MARGIN - 2 * UI_BTN_W - 8, y, "OK", 1);
	Ui_ButtonAdd(&d, d.w - UI_MARGIN - UI_BTN_W, y, "Cancel", 0);
	d.h = y + UI_BTN_H + UI_MARGIN;
	d.defButton = 0;
	d.escButton = 1;
	d.focus = 0;
	d.result = 0;
	r = Ui_Run(&d, title ? title : MSG_DLG_COMMENT_INPUT); // a NULL title selects the default caption, as the original does
	if(r == 1)
		snprintf(out, 0x100, "%s", d.fields[0].f.text);
	return r == 1;
}

// dialog 0x72: two labelled fields, OK / cancel
int OS_DialogInput2(const char* title, const char* label1, const char* initial1, int maxLen1, char* out1,
	const char* label2, const char* initial2, int maxLen2, char* out2)
{
	UiDialog_t d;
	int r, y = UI_MARGIN;
	if(!X11_Open())
		return 0;
	memset(&d, 0, sizeof d);
	d.w = DLG_FIELD_W + 2 * UI_MARGIN;
	Ui_LabelAdd(&d, UI_MARGIN, y, label1 ? label1 : MSG_DLG_STRING1); // NULL pointers select the defaults
	y += UI_FONT_H + 4;
	Ui_FieldAdd(&d, UI_MARGIN, y, DLG_FIELD_W, initial1, maxLen1);
	y += UI_FIELD_H + 8;
	Ui_LabelAdd(&d, UI_MARGIN, y, label2 ? label2 : MSG_DLG_STRING2);
	y += UI_FONT_H + 4;
	Ui_FieldAdd(&d, UI_MARGIN, y, DLG_FIELD_W, initial2, maxLen2);
	y += UI_FIELD_H + UI_MARGIN;
	Ui_ButtonAdd(&d, d.w - UI_MARGIN - 2 * UI_BTN_W - 8, y, "OK", 1);
	Ui_ButtonAdd(&d, d.w - UI_MARGIN - UI_BTN_W, y, "Cancel", 0);
	d.h = y + UI_BTN_H + UI_MARGIN;
	d.defButton = 0;
	d.escButton = 1;
	// the focus goes to the first field without an initial text (a NULL pointer)
	d.focus = !initial1 ? 0 : !initial2 ? 1
										: 0;
	d.result = 0;
	r = Ui_Run(&d, title ? title : MSG_DLG_DATA_INPUT);
	if(r == 1)
	{
		snprintf(out1, 0x100, "%s", d.fields[0].f.text);
		snprintf(out2, 0x100, "%s", d.fields[1].f.text);
	}
	return r == 1;
}

// ---- dialog 0x77: the player's name and birthday ------------------------------------------------

static const int kDaysOfMonth[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; // February always 29, as the original's list

typedef struct ProfileCtx
{
	int month, day; // field indexes of the two pickers
} ProfileCtx_t;

// every character of a name field must be double-byte (full width); an empty field passes
static int AllDoubleByte(const char* s)
{
	while(*s)
	{
		if(!OsCommon_SjisIsLead((uint8_t)*s) || !s[1])
			return 0;
		s += 2;
	}
	return 1;
}

/* the OK button: a name field with a single-byte character is rejected
 * with the message MSG_DLG_HALFWIDTH naming the field (caption
 * MSG_DLG_INPUT_ERROR) and the dialog stays open */
static int ProfileOk(UiDialog_t* d, int id, void* ctx)
{
	static const char* const kNames[4] = {MSG_DLG_SURNAME, MSG_DLG_GIVEN_NAME, MSG_DLG_NICKNAME, MSG_DLG_PRONOUN};
	int i;
	if(id != 1)
		return 1;
	for(i = 0; i < 4; i++)
	{
		if(!AllDoubleByte(d->fields[i].f.text))
		{
			char msg[0x200];
			snprintf(msg, sizeof msg, MSG_DLG_HALFWIDTH, kNames[i]);
			OS_MessageBox(msg, MSG_DLG_INPUT_ERROR, OS_MB_ICONHAND);
			return 0;
		}
	}
	return 1;
}

// the month picker changed: the day range follows (the original's CBN_SELENDOK handler of combo box 0x409)
static void ProfileChange(UiDialog_t* d, int field, void* ctx)
{
	ProfileCtx_t* pc = (ProfileCtx_t*)ctx;
	if(field == pc->month)
	{
		int m = atoi(d->fields[pc->month].f.text);
		UiFieldW_t* day = &d->fields[pc->day];
		if(m >= 1 && m <= 12)
		{
			day->max = kDaysOfMonth[m - 1];
			if(atoi(day->f.text) > day->max)
			{ // the original resets a day past the new range to the first
				snprintf(day->f.text, sizeof day->f.text, "1");
				day->f.caret = 1;
			}
		}
	}
}

/* dialog 0x77: four name fields of 10 bytes and the birthday as two
 * number pickers (the resource's combo boxes), OK only.  The name
 * buffers hold the initial texts in and the results out; month and day
 * are 0-based in and out, as the combo box selections of the original. */
int OS_DialogProfile(char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month, int32_t* day)
{
	UiDialog_t d;
	ProfileCtx_t pc;
	int y = UI_MARGIN, labelW, fieldX, r;
	char num[16];
	if(!X11_Open())
		return 0;
	memset(&d, 0, sizeof d);
	labelW = Ui_TextWidth(MSG_DLG_NICKNAME) + 16; // the nickname label sets the width of the label column
	fieldX = UI_MARGIN + labelW;
	d.w = fieldX + 200 + UI_MARGIN;

	// surname / given name on one row, like the resource
	Ui_LabelAdd(&d, UI_MARGIN, y + 3, MSG_DLG_SURNAME);
	Ui_FieldAdd(&d, fieldX, y, 90, surname, 10);
	Ui_LabelAdd(&d, fieldX + 100, y + 3, MSG_DLG_GIVEN_NAME);
	Ui_FieldAdd(&d, fieldX + 100 + Ui_TextWidth(MSG_DLG_GIVEN_NAME) + 8, y, 90 - Ui_TextWidth(MSG_DLG_GIVEN_NAME) + 2, givenName, 10);
	y += UI_FIELD_H + 8;
	Ui_LabelAdd(&d, UI_MARGIN, y + 3, MSG_DLG_NICKNAME);
	Ui_FieldAdd(&d, fieldX, y, 200, nickname, 10);
	y += UI_FIELD_H + 8;
	Ui_LabelAdd(&d, UI_MARGIN, y + 3, MSG_DLG_PRONOUN);
	Ui_FieldAdd(&d, fieldX, y, 200, pronoun, 10);
	y += UI_FIELD_H + 8;
	Ui_LabelAdd(&d, UI_MARGIN, y + 3, MSG_DLG_BIRTHDAY);
	snprintf(num, sizeof num, "%d", (int)(*month) + 1); // the engine holds 0-based selections
	pc.month = Ui_FieldAdd(&d, fieldX, y, 44, num, 2);
	d.fields[pc.month].numeric = 1;
	d.fields[pc.month].min = 1;
	d.fields[pc.month].max = 12;
	Ui_LabelAdd(&d, fieldX + 50, y + 3, MSG_DLG_MONTH);
	snprintf(num, sizeof num, "%d", (int)(*day) + 1);
	pc.day = Ui_FieldAdd(&d, fieldX + 50 + Ui_TextWidth(MSG_DLG_MONTH) + 12, y, 44, num, 2);
	d.fields[pc.day].numeric = 1;
	d.fields[pc.day].min = 1;
	d.fields[pc.day].max = 31; // 1..31 until the month changes, as the resource lists them
	Ui_LabelAdd(&d, fieldX + 50 + Ui_TextWidth(MSG_DLG_MONTH) + 12 + 50, y + 3, MSG_DLG_DAY);
	y += UI_FIELD_H + UI_MARGIN;
	Ui_ButtonAdd(&d, (d.w - UI_BTN_W) / 2, y, "OK", 1);
	d.h = y + UI_BTN_H + UI_MARGIN;
	d.defButton = 0;
	d.escButton = -1;    // OK only: the dialog cannot be cancelled
	d.focus = d.nFields; // SetFocus(GetDlgItem(hwnd, IDOK))
	d.onButton = ProfileOk;
	d.onChange = ProfileChange;
	d.ctx = &pc;
	d.result = 0;
	r = Ui_Run(&d, MSG_DLG_PROFILE_TITLE);
	if(r != 1)
		return 0;
	snprintf(surname, 0x100, "%s", d.fields[0].f.text);
	snprintf(givenName, 0x100, "%s", d.fields[1].f.text);
	snprintf(nickname, 0x100, "%s", d.fields[2].f.text);
	snprintf(pronoun, 0x100, "%s", d.fields[3].f.text);
	{
		int m = atoi(d.fields[pc.month].f.text), dd = atoi(d.fields[pc.day].f.text);
		if(m < 1)
			m = 1;
		if(dd < 1)
			dd = 1;
		*month = m - 1;
		*day = dd - 1;
	}
	return 1;
}

// the file dialogs are not provided by this back end: "nothing chosen"
int OS_FileDialog(int save, char* path, size_t n, const char* filter, const char* defExt, const char* initialDir, const char* title)
{
	return 0;
}

// ---- the folder browser ---------------------------------------------------------------------------

#define BROWSE_MAX  512 // entries listed of one directory
#define BROWSE_ROWS 12
#define BROWSE_W    420

/* the state of the browser: the directory shown, its subdirectories as
 * the list's rows (UTF-8 names, and their Shift-JIS spellings for the
 * list) */
typedef struct Browse
{
	char dir[PATH_MAX];            // the directory shown (native, no trailing '/', "/" for the root)
	char names[BROWSE_MAX][0x100]; // its subdirectories, UTF-8
	char shown[BROWSE_MAX][0x100]; // the same as Shift-JIS, what the list draws
	const char* rows[BROWSE_MAX];
	int count;
	int pathLabel; // the label that shows the directory
	int list;
} Browse_t;

static int CompareNames(const void* a, const void* b)
{
	return strcasecmp((const char*)a, (const char*)b);
}

// list the subdirectories of b->dir (hidden ones left out, ".." first unless at the root)
static void BrowseFill(UiDialog_t* d, Browse_t* b)
{
	DIR* dp = opendir(b->dir);
	struct dirent* e;
	int i, start = 0;
	b->count = 0;
	if(strcmp(b->dir, "/") != 0)
	{
		strcpy(b->names[0], "..");
		b->count = start = 1;
	}
	if(dp)
	{
		while((e = readdir(dp)) != NULL && b->count < BROWSE_MAX)
		{
			char full[PATH_MAX * 2];
			struct stat st;
			if(e->d_name[0] == '.')
				continue;
			snprintf(full, sizeof full, "%s/%s", b->dir, e->d_name);
			if(stat(full, &st) == 0 && S_ISDIR(st.st_mode))
				snprintf(b->names[b->count++], sizeof b->names[0], "%s", e->d_name);
		}
		closedir(dp);
		qsort(b->names[start], (size_t)(b->count - start), sizeof b->names[0], CompareNames);
	}
	for(i = 0; i < b->count; i++)
	{
		OS_Utf8ToSjis(b->names[i], b->shown[i], sizeof b->shown[i]);
		b->rows[i] = b->shown[i];
	}
	if(d)
	{
		char shownDir[0x200];
		UiList_t* l = &d->lists[b->list];
		l->count = b->count;
		l->selected = b->count ? 0 : -1;
		l->top = 0;
		OS_Utf8ToSjis(b->dir, shownDir, sizeof shownDir);
		Ui_LabelSet(d, b->pathLabel, shownDir);
		d->focus = d->nFields + d->nButtons + b->list;
	}
}

// enter the selected row (".." goes up); the dialog stays open
static void BrowseEnter(UiDialog_t* d, Browse_t* b)
{
	int sel = d->lists[b->list].selected;
	char next[PATH_MAX];
	if(sel < 0 || sel >= b->count)
		return;
	if(strcmp(b->names[sel], "..") == 0)
	{
		char* slash;
		strcpy(next, b->dir);
		slash = strrchr(next, '/');
		if(!slash)
			return;
		if(slash == next)
			slash[1] = 0;
		else
			*slash = 0;
	}
	else
		snprintf(next, sizeof next, "%s%s%s", b->dir, strcmp(b->dir, "/") == 0 ? "" : "/", b->names[sel]);
	snprintf(b->dir, sizeof b->dir, "%s", next);
	BrowseFill(d, b);
}

// 1 = Select ends the dialog; 2 = Open enters the selected folder and keeps it open
static int BrowseButton(UiDialog_t* d, int id, void* ctx)
{
	if(id == 2)
	{
		BrowseEnter(d, (Browse_t*)ctx);
		return 0;
	}
	return 1;
}

/* the folder browser: the directory as a label, its subdirectories as a
 * list (Open or a double click enters one, ".." goes up), Select takes
 * the directory shown.  `initial` and the result are native (UTF-8)
 * paths; 1 when a folder was chosen. */
int OS_BrowseFolder(char* path, size_t n, const char* title, const char* initial)
{
	UiDialog_t d;
	Browse_t* b;
	struct stat st;
	int r, y = UI_MARGIN;
	if(!X11_Open())
		return 0;
	b = (Browse_t*)calloc(1, sizeof *b);
	if(!b)
		return 0;
	if(initial && *initial && stat(initial, &st) == 0 && S_ISDIR(st.st_mode) && realpath(initial, b->dir))
		;
	else if(!getcwd(b->dir, sizeof b->dir))
		strcpy(b->dir, "/");
	if(strlen(b->dir) > 1 && b->dir[strlen(b->dir) - 1] == '/')
		b->dir[strlen(b->dir) - 1] = 0;
	memset(&d, 0, sizeof d);
	d.w = BROWSE_W + 2 * UI_MARGIN;
	b->pathLabel = Ui_LabelAdd(&d, UI_MARGIN, y, "");
	y += UI_FONT_H + 6;
	b->list = Ui_ListAdd(&d, UI_MARGIN, y, BROWSE_W, BROWSE_ROWS, b->rows, 0, -1);
	y += d.lists[b->list].h + UI_MARGIN;
	Ui_ButtonAdd(&d, UI_MARGIN, y, MSG_BROWSE_OPEN, 2);
	Ui_ButtonAdd(&d, d.w - UI_MARGIN - 2 * UI_BTN_W - 8, y, MSG_BROWSE_SELECT, 1);
	Ui_ButtonAdd(&d, d.w - UI_MARGIN - UI_BTN_W, y, MSG_BROWSE_CANCEL, 0);
	d.h = y + UI_BTN_H + UI_MARGIN;
	d.defButton = 0; // Enter and a double click open the folder
	d.escButton = 2;
	d.onButton = BrowseButton;
	d.ctx = b;
	BrowseFill(&d, b);
	r = Ui_Run(&d, title && *title ? title : MSG_BROWSE_TITLE);
	if(r == 1)
		snprintf(path, n, "%s", b->dir);
	free(b);
	return r == 1;
}

// ---- the launcher's dialog ------------------------------------------------------------------------

#define LAUNCH_W    520 // the controls' width
#define LAUNCH_ROWS 8
#define ID_LAUNCH   1
#define ID_QUIT     0
#define ID_BROWSE   3
#define ID_ADVANCED 4

// what the dialog keeps beside the OsLauncher_t: the indices of its controls
typedef struct LaunchCtx
{
	OsLauncher_t* l;
	int list, dirField, status, profileField, optionsField;
	char dirSjis[0x100]; // the directory field's text as last shown, to tell a change
} LaunchCtx_t;

// the directory and the status line of the launcher into the controls
static void LaunchShow(UiDialog_t* d, LaunchCtx_t* c)
{
	OS_Utf8ToSjis(c->l->dir, c->dirSjis, sizeof c->dirSjis); // native paths are UTF-8 here
	X11Field_SetText(&d->fields[c->dirField].f, c->dirSjis);
	Ui_LabelSet(d, c->status, c->l->status);
	if(c->profileField >= 0)
		X11Field_SetText(&d->fields[c->profileField].f, c->l->profile);
	d->redraw = 1;
}

static void LaunchListSelect(UiDialog_t* d, int list, void* ctx)
{
	LaunchCtx_t* c = (LaunchCtx_t*)ctx;
	int sel = d->lists[list].selected;
	c->l->selected = sel;
	if(sel >= 0 && c->l->onSelect)
		c->l->onSelect(c->l, sel);
	LaunchShow(d, c);
}

// a field changed: the directory typed is probed, the profile and the options are taken as they are
static void LaunchChange(UiDialog_t* d, int field, void* ctx)
{
	LaunchCtx_t* c = (LaunchCtx_t*)ctx;
	if(field == c->dirField)
	{
		if(strcmp(d->fields[field].f.text, c->dirSjis) == 0)
			return;
		snprintf(c->dirSjis, sizeof c->dirSjis, "%s", d->fields[field].f.text);
		OS_SjisToUtf8(c->dirSjis, c->l->dir, sizeof c->l->dir);
		c->l->selected = -1;
		d->lists[c->list].selected = -1;
		if(c->l->onDirectory)
			c->l->onDirectory(c->l);
		Ui_LabelSet(d, c->status, c->l->status);
		if(c->profileField >= 0)
			X11Field_SetText(&d->fields[c->profileField].f, c->l->profile);
		d->redraw = 1;
	}
	else if(field == c->profileField)
		snprintf(c->l->profile, sizeof c->l->profile, "%s", d->fields[field].f.text);
	else if(field == c->optionsField)
		snprintf(c->l->options, sizeof c->l->options, "%s", d->fields[field].f.text);
}

// Browse keeps the dialog open; Launch closes it only with a game; Advanced and Quit close it
static int LaunchButton(UiDialog_t* d, int id, void* ctx)
{
	LaunchCtx_t* c = (LaunchCtx_t*)ctx;
	if(id == ID_BROWSE)
	{
		char chosen[OS_LAUNCHER_DIR_MAX];
		if(OS_BrowseFolder(chosen, sizeof chosen, MSG_BROWSE_TITLE, c->l->dir))
		{
			snprintf(c->l->dir, sizeof c->l->dir, "%s", chosen);
			c->l->selected = -1;
			d->lists[c->list].selected = -1;
			if(c->l->onDirectory)
				c->l->onDirectory(c->l);
			LaunchShow(d, c);
		}
		return 0;
	}
	if(id == ID_LAUNCH && !c->l->launchable)
	{
		OS_MessageBox(c->l->refused ? c->l->refused : MSG_LAUNCH_REFUSED, MSG_LAUNCH_TITLE, OS_MB_ICONWARNING);
		return 0;
	}
	return 1;
}

/* the dialog: the games list, the directory field with its Browse
 * button, the status line, and - once Advanced was pressed, which reopens
 * the dialog - the profile picker and the options field; Launch is the
 * default button, Quit the escape */
int OS_LauncherDialog(OsLauncher_t* l)
{
	LaunchCtx_t c;
	int r;
	if(!X11_Open())
		return -1;
	for(;;)
	{
		UiDialog_t d;
		int y = UI_MARGIN, fieldW = LAUNCH_W - UI_BTN_W - 8;
		memset(&d, 0, sizeof d);
		memset(&c, 0, sizeof c);
		c.l = l;
		c.profileField = c.optionsField = -1;
		d.w = LAUNCH_W + 2 * UI_MARGIN;
		Ui_LabelAdd(&d, UI_MARGIN, y, MSG_LAUNCH_GAMES);
		y += UI_FONT_H + 4;
		c.list = Ui_ListAdd(&d, UI_MARGIN, y, LAUNCH_W, LAUNCH_ROWS, l->names, l->count, l->selected);
		y += d.lists[c.list].h + 8;
		Ui_LabelAdd(&d, UI_MARGIN, y, MSG_LAUNCH_DIRECTORY);
		y += UI_FONT_H + 4;
		c.dirField = Ui_FieldAdd(&d, UI_MARGIN, y, fieldW, "", 0);
		Ui_ButtonAdd(&d, UI_MARGIN + fieldW + 8, y - 1, MSG_LAUNCH_BROWSE, ID_BROWSE);
		y += UI_FIELD_H + 6;
		c.status = Ui_LabelAdd(&d, UI_MARGIN, y, "");
		y += UI_FONT_H + 8;
		if(l->advanced)
		{
			int labelW = Ui_TextWidth(MSG_LAUNCH_PROFILE), labelW2 = Ui_TextWidth(MSG_LAUNCH_OPTIONS), x;
			x = UI_MARGIN + (labelW > labelW2 ? labelW : labelW2) + 8;
			Ui_LabelAdd(&d, UI_MARGIN, y + 3, MSG_LAUNCH_PROFILE);
			c.profileField = Ui_FieldAdd(&d, x, y, 160, l->profile, 31);
			d.fields[c.profileField].choices = l->profiles;
			d.fields[c.profileField].nChoices = l->profileCount;
			y += UI_FIELD_H + 6;
			Ui_LabelAdd(&d, UI_MARGIN, y + 3, MSG_LAUNCH_OPTIONS);
			c.optionsField = Ui_FieldAdd(&d, x, y, LAUNCH_W - (x - UI_MARGIN), l->options, 0xff);
			y += UI_FIELD_H + 8;
		}
		Ui_ButtonAdd(&d, UI_MARGIN, y, MSG_LAUNCH_ADVANCED, ID_ADVANCED);
		Ui_ButtonAdd(&d, d.w - UI_MARGIN - 2 * UI_BTN_W - 8, y, MSG_LAUNCH_LAUNCH, ID_LAUNCH);
		Ui_ButtonAdd(&d, d.w - UI_MARGIN - UI_BTN_W, y, MSG_LAUNCH_QUIT, ID_QUIT);
		d.h = y + UI_BTN_H + UI_MARGIN;
		d.defButton = 2;
		d.escButton = 3;
		d.focus = d.nFields + d.nButtons + c.list; // the list
		d.onButton = LaunchButton;
		d.onChange = LaunchChange;
		d.onListSelect = LaunchListSelect;
		d.ctx = &c;
		LaunchShow(&d, &c);
		r = Ui_Run(&d, MSG_LAUNCH_TITLE);
		if(r == ID_ADVANCED)
		{
			l->advanced = !l->advanced;
			continue;
		}
		return r == ID_LAUNCH;
	}
}
