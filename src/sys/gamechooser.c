/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * gamechooser.c - the game chooser (src/include/bgi/engine.h: GameChooser_Begin, GameChooser_End)
 *
 * The original lives in the game's folder and boots from there.  This
 * binary lives anywhere, so when it is started outside a game directory
 * (no system.arc and no ipl._bp where it runs, no boot path on the
 * command line) it asks the host to show the game chooser
 * (OPENBGI_ASK_GAME_CHOOSER with an OpenBGI_GameChooser_t, openbgi.h), which
 * lists the games openbgi.json records with a directory, takes a
 * directory typed or browsed, and shows what that directory holds - the
 * game's name and profile when the detection (detect.h) finds its entry,
 * "Unknown game" and the profile its opcodes fit when it finds none, or
 * that there is no game.  "Advanced" uncovers the profile (the detected
 * one, or any of the table) and extra command-line options.  Launch makes
 * the directory the game directory and records it in openbgi.json - a new
 * entry for an unknown game, with its product name and the hash of its
 * programs so that it is found by them next time, the directory added to
 * the matched entry otherwise - so that the list offers it next time.
 */
#include "bgi/engine.h"
#include "bgi/version.h"
#include "bgi/detect.h"
#include "bgi/host.h"
#include "bgi/msg.h"
#include "bgi/release.h"
#include "bgi/sjis.h"
#include "bgi/request.h"
#include "bgi/core.h"

#define GAME_CHOOSER_LIST_MAX 256

// what the dialog's callbacks need beside the OpenBGI_GameChooser_t
typedef struct GameChooser
{
	OpenBGI_GameChooser_t l;
	char names[GAME_CHOOSER_LIST_MAX][0x100]; // the list rows: the names of the recorded games, Shift-JIS
	const char* rows[GAME_CHOOSER_LIST_MAX];
	int entry[GAME_CHOOSER_LIST_MAX]; // the openbgi.json entry of each row
	int rowCount;
	const char* profiles[ENGINE_PROFILES_MAX];
	int match;           // the openbgi.json entry the detection found for the directory, -1 for none
	char product[0x100]; // the product name the directory's programs check for (UTF-8), "" for none
	char hash[17];       // the hash of its programs, "" when there are none
	char title[0x100];   // the dialog's title: "Select game - OpenBGI v1.0"
	int fromEntry;       // the entry the directory was taken from (the list), -1 when typed or browsed
	char dirUtf8[OPENBGI_GAME_CHOOSER_DIR_MAX];
} GameChooser_t;

/* look at the directory the chooser holds: what game is there (its
 * entry's name and profile), an unknown one (the profile its opcodes
 * fit), or none; the profile and the options follow the entry */
static void Probe(OpenBGI_t* o, GameChooser_t* L)
{
	OpenBGI_GameChooser_t* l = &L->l;
	DetectSet_t set;
	DetectResult_t r;
	char nameSjis[0x100];
	EngineGame_t g;
	L->match = -1;
	L->product[0] = L->hash[0] = 0;
	l->launchable = 0;
	l->profile[0] = 0;
	if(!l->dir[0])
	{
		snprintf(l->status, sizeof l->status, "%s", MSG_LAUNCH_NO_DIRECTORY);
		return;
	}
	Host_NativeToUtf8(o, l->dir, L->dirUtf8, sizeof L->dirUtf8);
	DetectSet_Init(&set);
	DetectSet_LoadDirectory(o, &set, l->dir);
	if(!set.haveIpl)
	{ // no boot program: nothing the engine could start
		DetectSet_Free(&set);
		snprintf(l->status, sizeof l->status, "%s", MSG_LAUNCH_NO_GAME);
		return;
	}
	Detect_Game(o, &set, NULL, "", &r);
	DetectSet_Free(&set);
	L->match = r.entry;
	snprintf(L->product, sizeof L->product, "%s", r.product);
	snprintf(L->hash, sizeof L->hash, "%s", r.hash);
	l->launchable = 1;
	if(L->match >= 0 && Engine_GameAt(o, L->match, &g))
	{
		Sjis_FromUtf8(g.name, nameSjis, sizeof nameSjis);
		snprintf(l->status, sizeof l->status, MSG_LAUNCH_GAME_FOUND, nameSjis, r.profile);
		snprintf(l->profile, sizeof l->profile, "%s", r.profile);
		if(L->fromEntry < 0 || L->fromEntry == L->match)
			snprintf(l->options, sizeof l->options, "%s", g.options ? g.options : "");
	}
	else
	{
		if(L->product[0])
			Sjis_FromUtf8(L->product, nameSjis, sizeof nameSjis);
		else
			snprintf(nameSjis, sizeof nameSjis, "%s", MSG_LAUNCH_UNKNOWN);
		if(r.profile[0])
			snprintf(l->status, sizeof l->status, MSG_LAUNCH_UNKNOWN_FIT, nameSjis, r.profile);
		else
			snprintf(l->status, sizeof l->status, "%s", nameSjis);
		snprintf(l->profile, sizeof l->profile, "%s", r.profile);
		if(L->fromEntry >= 0 && Engine_GameAt(o, L->fromEntry, &g) && g.profile)
			snprintf(l->profile, sizeof l->profile, "%s", g.profile); // what was chosen for it before
	}
}

static void OnDirectory(OpenBGI_t* o, OpenBGI_GameChooser_t* l)
{
	GameChooser_t* L = (GameChooser_t*)l;
	L->fromEntry = -1;
	Probe(o, L);
}

// a row of the list: its entry's directory, options and profile
static void OnSelect(OpenBGI_t* o, OpenBGI_GameChooser_t* l, int row)
{
	GameChooser_t* L = (GameChooser_t*)l;
	EngineGame_t g;
	if(row < 0 || row >= L->rowCount || !Engine_GameAt(o, L->entry[row], &g))
		return;
	L->fromEntry = L->entry[row];
	Host_Utf8ToNative(o, g.directory ? g.directory : "", l->dir, sizeof l->dir);
	snprintf(l->options, sizeof l->options, "%s", g.options ? g.options : "");
	Probe(o, L);
	if(!l->profile[0] && g.profile)
		snprintf(l->profile, sizeof l->profile, "%s", g.profile);
}

// the last component of a native directory's path (its separators at the end left out), for the name of an unknown game
static void BaseName(OpenBGI_t* o, const char* dir, char* out, size_t n)
{
	char buf[OPENBGI_GAME_CHOOSER_DIR_MAX];
	size_t len;
	snprintf(buf, sizeof buf, "%s", dir);
	while((len = strlen(buf)) > 1 && !*Host_NativeFilePart(o, buf))
		buf[len - 1] = 0;
	snprintf(out, n, "%s", Host_NativeFilePart(o, buf));
}

/* record the launched game: the row that already names the directory
 * keeps it; else the matched entry gets the directory when it has none
 * yet, or a row of its own named after the game when it names another
 * (the same game elsewhere); an unknown game gets a row named after its
 * product name, else its directory, with the hash of its programs.  The
 * profile and the options are recorded as chosen. */
static void Record(OpenBGI_t* o, GameChooser_t* L, const char* gamesPath)
{
	OpenBGI_GameChooser_t* l = &L->l;
	EngineGame_t g;
	int index = -1, i;
	char name[0x100];
	name[0] = 0;
	for(i = 0; i < Engine_GameCount(o) && index < 0; i++)
		if(Engine_GameAt(o, i, &g) && g.directory && strcmp(g.directory, L->dirUtf8) == 0)
			index = i;
	if(index < 0 && L->match >= 0 && Engine_GameAt(o, L->match, &g))
	{
		if(!g.directory)
			index = L->match;
		else
			snprintf(name, sizeof name, "%s", g.name);
	}
	if(index < 0 && !name[0] && L->product[0])
		snprintf(name, sizeof name, "%s", L->product);
	if(index < 0 && !name[0])
	{
		char base[0x100];
		BaseName(o, l->dir, base, sizeof base);
		Host_NativeToUtf8(o, base, name, sizeof name);
		if(!name[0])
			snprintf(name, sizeof name, "%s", L->dirUtf8);
	}
	Engine_RecordGame(o, index, L->product, L->hash, name, L->dirUtf8, l->profile, l->options);
	if(gamesPath && *gamesPath)
		Engine_SaveGamesJson(o, gamesPath); // kept in memory only when the file cannot be written
}

uint32_t GameChooser_Begin(OpenBGI_t* o)
{
	GameChooser_t* L = (GameChooser_t*)calloc(1, sizeof *L);
	OpenBGI_GameChooser_t* l;
	OpenBGI_Request_t q;
	int i;
	if(!L)
		return 0;
	l = &L->l;
	for(i = 0; i < Engine_GameCount(o) && L->rowCount < GAME_CHOOSER_LIST_MAX; i++)
	{
		EngineGame_t g;
		if(!Engine_GameAt(o, i, &g) || !g.directory)
			continue;
		Sjis_FromUtf8(g.name, L->names[L->rowCount], sizeof L->names[0]);
		L->rows[L->rowCount] = L->names[L->rowCount];
		L->entry[L->rowCount] = i;
		L->rowCount++;
	}
	for(i = 0; i < Engine_ProfileCount(o) && i < ENGINE_PROFILES_MAX; i++)
		L->profiles[i] = Engine_ProfileAt(o, i)->name;
	l->names = L->rows;
	l->count = L->rowCount;
	l->selected = -1;
	l->profiles = L->profiles;
	l->profileCount = i;
	l->refused = MSG_LAUNCH_REFUSED;
	snprintf(L->title, sizeof L->title, "%s - %s", MSG_LAUNCH_TITLE, OPENBGI_TITLE);
	l->title = L->title;
	l->onSelect = OnSelect;
	l->onDirectory = OnDirectory;
	l->o = o;
	L->match = L->fromEntry = -1;
	snprintf(l->status, sizeof l->status, "%s", MSG_LAUNCH_NO_DIRECTORY);
	memset(&q, 0, sizeof q);
	q.kind = OPENBGI_ASK_GAME_CHOOSER;
	q.u.chooser.chooser = l;
	o->engine.chooser = L;
	return Request_Post(o, &q, REQUEST_TREAT_NONE);
}

int GameChooser_End(OpenBGI_t* o, int answer, const char* gamesPath, char* profile, size_t profileSize, char* options, size_t optionsSize)
{
	GameChooser_t* L = o->engine.chooser;
	int r = answer;
	o->engine.chooser = NULL;
	if(!L)
		return 0;
	if(r == 1)
	{
		OpenBGI_GameChooser_t* l = &L->l;
		if(!Host_SetGameDir(o, l->dir))
			r = 0;
		else
		{
			Record(o, L, gamesPath);
			snprintf(profile, profileSize, "%s", l->profile);
			snprintf(options, optionsSize, "%s", l->options);
		}
	}
	free(L);
	return r;
}
