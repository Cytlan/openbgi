/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * launcher.c - the game chooser (inc/bgi/engine.h: Launcher_Run)
 *
 * The original lives in the game's folder and boots from there.  This
 * binary lives anywhere, so when it is started outside a game directory
 * (no system.arc and no ipl._bp where it runs, no boot path on the
 * command line) it asks which game to run: the dialog of OS_Launcher-
 * Dialog lists the games games.json records with a directory, takes a
 * directory typed or browsed, and shows what that directory holds - the
 * game's name and profile when its boot program matches an entry of
 * games.json, "Unknown game" when a boot program is there that matches
 * none, or that there is no game.  "Advanced" uncovers the profile (the
 * detected one, or any of the table) and extra command-line options.
 * Launch makes the directory the game directory and records it in
 * games.json - a new entry for an unknown game, the directory added to
 * the matched entry otherwise - so that the list offers it next time.
 */
#include "bgi/engine.h"
#include "bgi/version.h"
#include "bgi/arcread.h"
#include "bgi/codec.h"
#include "bgi/os.h"
#include "bgi/msg.h"

#define LAUNCHER_LIST_MAX 256

// what the dialog's callbacks need beside the OsLauncher_t
typedef struct Launcher
{
	OsLauncher_t l;
	char names[LAUNCHER_LIST_MAX][0x100]; // the list rows: the names of the recorded games, Shift-JIS
	const char* rows[LAUNCHER_LIST_MAX];
	int entry[LAUNCHER_LIST_MAX]; // the games.json entry of each row
	int rowCount;
	const char* profiles[ENGINE_PROFILES_MAX];
	int match;     // the games.json entry the directory's boot program matched, -1 for none
	int fromEntry; // the entry the directory was taken from (the list), -1 when typed or browsed
	char dirUtf8[OS_LAUNCHER_DIR_MAX];
} Launcher_t;

// the boot program of a directory: a loose ipl._bp, else the ipl._bp of its system.arc; NULL when there is none
static uint8_t* LoadBootProgram(const char* dir, uint32_t* size)
{
	char path[OS_LAUNCHER_DIR_MAX + 32];
	uint8_t* data;
	ArcRead_t* a;
	snprintf(path, sizeof path, "%s/ipl._bp", dir);
	data = OS_ReadNativeFile(path, size);
	if(data)
	{
		if(*size >= sizeof(DscHeader_t) && BGI_IsDsc(data))
		{ // a loose file may be compressed like an archive entry
			uint32_t n = BGI_DscOutSize(data);
			uint8_t* out = (uint8_t*)malloc(n + 1);
			*size = BGI_DscDecode(data, out);
			out[*size] = 0;
			free(data);
			return out;
		}
		return data;
	}
	snprintf(path, sizeof path, "%s/system.arc", dir);
	{ // no archive either: nothing to say (the reader below reports a file it cannot read)
		FILE* f = OS_NativeOpen(path, "rb");
		if(!f)
			return NULL;
		a = ArcRead_OpenFile(f, path);
	}
	if(!a)
		return NULL;
	{
		const ArcEntry_t* e = ArcRead_Find(a, "ipl._bp");
		int dsc;
		uint8_t* entry = e ? ArcRead_Load(a, e, size, &dsc) : NULL;
		ArcRead_Close(a);
		if(!entry)
			return NULL;
		data = (uint8_t*)malloc(*size + 1);
		memcpy(data, entry, *size);
		data[*size] = 0;
		BGI_Free(entry);
		return data;
	}
}

/* look at the directory the launcher holds: what game is there (its
 * entry's name and profile), an unknown one, or none; the profile and
 * the options follow the entry */
static void Probe(Launcher_t* L)
{
	OsLauncher_t* l = &L->l;
	uint32_t size;
	uint8_t* ipl;
	char nameSjis[0x100];
	EngineGame_t g;
	L->match = -1;
	l->launchable = 0;
	l->profile[0] = 0;
	if(!l->dir[0])
	{
		snprintf(l->status, sizeof l->status, "%s", MSG_LAUNCH_NO_DIRECTORY);
		return;
	}
	OS_NativeToUtf8(l->dir, L->dirUtf8, sizeof L->dirUtf8);
	ipl = LoadBootProgram(l->dir, &size);
	if(!ipl)
	{
		snprintf(l->status, sizeof l->status, "%s", MSG_LAUNCH_NO_GAME);
		return;
	}
	L->match = Engine_FindGame(ipl, size);
	free(ipl);
	l->launchable = 1;
	if(L->match >= 0 && Engine_GameAt(L->match, &g))
	{
		OS_Utf8ToSjis(g.name, nameSjis, sizeof nameSjis);
		snprintf(l->status, sizeof l->status, MSG_LAUNCH_GAME_FOUND, nameSjis, g.profile ? g.profile : "");
		snprintf(l->profile, sizeof l->profile, "%s", g.profile ? g.profile : "");
		if(L->fromEntry < 0 || L->fromEntry == L->match)
			snprintf(l->options, sizeof l->options, "%s", g.options ? g.options : "");
	}
	else
	{
		snprintf(l->status, sizeof l->status, "%s", MSG_LAUNCH_UNKNOWN);
		if(L->fromEntry >= 0 && Engine_GameAt(L->fromEntry, &g) && g.profile)
			snprintf(l->profile, sizeof l->profile, "%s", g.profile);
	}
}

static void OnDirectory(OsLauncher_t* l)
{
	Launcher_t* L = (Launcher_t*)l;
	L->fromEntry = -1;
	Probe(L);
}

// a row of the list: its entry's directory, options and profile
static void OnSelect(OsLauncher_t* l, int row)
{
	Launcher_t* L = (Launcher_t*)l;
	EngineGame_t g;
	if(row < 0 || row >= L->rowCount || !Engine_GameAt(L->entry[row], &g))
		return;
	L->fromEntry = L->entry[row];
	OS_Utf8ToNative(g.directory ? g.directory : "", l->dir, sizeof l->dir);
	snprintf(l->options, sizeof l->options, "%s", g.options ? g.options : "");
	Probe(L);
	if(!l->profile[0] && g.profile)
		snprintf(l->profile, sizeof l->profile, "%s", g.profile);
}

// the last component of a native path, for the name of an unknown game
static const char* BaseName(const char* dir)
{
	const char* p = dir + strlen(dir);
	while(p > dir && (p[-1] == '/' || p[-1] == '\\'))
		p--;
	while(p > dir && p[-1] != '/' && p[-1] != '\\')
		p--;
	return p;
}

/* record the launched game: the row that already names the directory
 * keeps it; else the matched entry gets the directory when it has none
 * yet, or a row of its own named after the game when it names another
 * (the same game elsewhere); an unknown game gets a row named after its
 * directory.  The profile and the options are recorded as chosen. */
static void Record(Launcher_t* L, const char* gamesPath)
{
	OsLauncher_t* l = &L->l;
	EngineGame_t g;
	int index = -1, i;
	char name[0x100];
	name[0] = 0;
	for(i = 0; i < Engine_GameCount() && index < 0; i++)
		if(Engine_GameAt(i, &g) && g.directory && strcmp(g.directory, L->dirUtf8) == 0)
			index = i;
	if(index < 0 && L->match >= 0 && Engine_GameAt(L->match, &g))
	{
		if(!g.directory)
			index = L->match;
		else
			snprintf(name, sizeof name, "%s", g.name);
	}
	if(index < 0 && !name[0])
	{
		char base[0x100];
		snprintf(base, sizeof base, "%s", BaseName(l->dir));
		OS_NativeToUtf8(base, name, sizeof name);
		if(!name[0])
			snprintf(name, sizeof name, "%s", L->dirUtf8);
	}
	Engine_RecordGame(index, name, L->dirUtf8, l->profile, l->options);
	if(gamesPath && *gamesPath)
		Engine_SaveGamesJson(gamesPath); // kept in memory only when the file cannot be written
}

int Launcher_Run(const char* gamesPath, char* profile, size_t profileSize, char* options, size_t optionsSize)
{
	Launcher_t* L = (Launcher_t*)calloc(1, sizeof *L);
	OsLauncher_t* l;
	int i, r;
	if(!L)
		return 0;
	l = &L->l;
	for(i = 0; i < Engine_GameCount() && L->rowCount < LAUNCHER_LIST_MAX; i++)
	{
		EngineGame_t g;
		if(!Engine_GameAt(i, &g) || !g.directory)
			continue;
		OS_Utf8ToSjis(g.name, L->names[L->rowCount], sizeof L->names[0]);
		L->rows[L->rowCount] = L->names[L->rowCount];
		L->entry[L->rowCount] = i;
		L->rowCount++;
	}
	for(i = 0; i < Engine_ProfileCount() && i < ENGINE_PROFILES_MAX; i++)
		L->profiles[i] = Engine_ProfileAt(i)->name;
	l->names = L->rows;
	l->count = L->rowCount;
	l->selected = -1;
	l->profiles = L->profiles;
	l->profileCount = i;
	l->refused = MSG_LAUNCH_REFUSED;
	l->onSelect = OnSelect;
	l->onDirectory = OnDirectory;
	L->match = L->fromEntry = -1;
	snprintf(l->status, sizeof l->status, "%s", MSG_LAUNCH_NO_DIRECTORY);
	r = OS_LauncherDialog(l);
	if(r == 1)
	{
		if(!OS_SetGameDir(l->dir))
			r = 0;
		else
		{
			Record(L, gamesPath);
			snprintf(profile, profileSize, "%s", l->profile);
			snprintf(options, optionsSize, "%s", l->options);
		}
	}
	free(L);
	return r;
}
