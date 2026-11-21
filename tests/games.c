/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/games.c - unit test of the engine profiles and openbgi.json
 *                 (src/core/version.c, declared in src/include/bgi/version.h);
 *                 built as test_games by `make test`
 *
 * The groups of tests, in the order they run:
 *
 *   defaults  - the built-in profiles and games, and the default
 *               openbgi.json the engine writes: it reads back to the same
 *               lists
 *   profiles  - an openbgi.json "profiles" list replaces the table: a
 *               profile may rename a generation and change single
 *               parameters, the others follow the generation's built-in
 *               values; an unknown generation is skipped; the built-in
 *               profiles the list lacks come after it
 *   games     - the game entries: product, profile, hash, the launcher's
 *               directory / name / options, the recording of launched
 *               games; the built-in games the file lacks come after its
 *               own, and an entry of the file for a built-in game gains
 *               the built-in hash
 *   settings  - what the launcher keeps (Engine_CfgGet, Engine_CfgSet):
 *               its own values and lists, the lists it can only read, a
 *               game's values by its path, and all of it through the file
 *   errors    - files that are not a games table leave the lists alone
 *
 * The files are written to and read from the scratch directory of the
 * test run (BGI_TEST_TMP, else the current directory).
 */
#include "bgi/version.h"
#include "bgi/core.h"
#include "host/os.h"

#include <stdio.h>

#include "check.h"

static char gPath[0x400]; // the scratch file
static int gDefaultGames; // the number of built-in game entries

static void WriteText(const char* text)
{
	CHECK(OS_WriteNativeFile(gPath, text, (uint32_t)strlen(text)));
}

static const EngineProfile_t* ProfileNamed(OpenBGI_t* o, const char* name)
{
	int i;
	for(i = 0; i < Engine_ProfileCount(o); i++)
		if(strcmp(Engine_ProfileAt(o, i)->name, name) == 0)
			return Engine_ProfileAt(o, i);
	return NULL;
}

static void TestDefaults(OpenBGI_t* o)
{
	size_t len;
	char* text;
	EngineGame_t g;
	int n;
	Engine_ResetGames(o);
	CHECK(Engine_ProfileCount(o) == GEN_COUNT);
	// the built-in profiles' names, for a host to offer (OpenBGI_ProfileName)
	for(n = 0; OpenBGI_ProfileName(n); n++)
		CHECK(ProfileNamed(o, OpenBGI_ProfileName(n)) != NULL && (int)ProfileNamed(o, OpenBGI_ProfileName(n))->gen == n);
	CHECK(n == GEN_COUNT && OpenBGI_ProfileName(-1) == NULL && strcmp(OpenBGI_ProfileName(0), "1.58") == 0);
	CHECK(ProfileNamed(o, "1.69/444") != NULL && ProfileNamed(o, "1.69/444")->gen == GEN_1_69_444);
	CHECK(ProfileNamed(o, "1.669") != NULL && ProfileNamed(o, "1.669")->tagShift == 26 && ProfileNamed(o, "1.669")->bootCode == 0x800000);
	CHECK(Engine_SelectProfile(o, "1.553") && o->engine.profile->gen == GEN_1_553 && o->engine.profile->compat == 169);
	CHECK(!Engine_SelectProfile(o, "9.99"));
	n = gDefaultGames = Engine_GameCount(o);
	CHECK(n >= 23);
	CHECK(ProfileNamed(o, "1.69/451") != NULL && ProfileNamed(o, "1.69/451")->gen == GEN_1_69_451 && ProfileNamed(o, "1.69/451")->tagShift == 25);
	// the trial of Sen no Hatou, Tsukisome no Kouki: build 1.636, found by its product name in ipl._bp
	CHECK(ProfileNamed(o, "1.636") != NULL && ProfileNamed(o, "1.636")->gen == GEN_1_636 && ProfileNamed(o, "1.636")->dynCount == 0 &&
		ProfileNamed(o, "1.636")->bootCode == 0x800000 && ProfileNamed(o, "1.616")->bootCode == 0x80000);
	CHECK(Engine_GameAt(o, 0, &g) && strcmp(g.product, "NurseryRhyme") == 0 && strcmp(g.profile, "1.58") == 0 && g.hash == NULL &&
		strcmp(g.name, "NurseryRhyme (trial)") == 0);
	CHECK(Engine_GameAt(o, 1, &g) && strcmp(g.product, "NurseryRhyme") == 0 && strcmp(g.name, g.product) == 0 && g.directory == NULL &&
		g.hash != NULL && strlen(g.hash) == 16);
	CHECK(!Engine_GameAt(o, n, &g));
	// the default file reads back to the same lists
	text = Engine_FormatGamesJson(o, &len);
	CHECK(text != NULL && len > 1000 && strstr(text, "\"profiles\"") != NULL && strstr(text, "\"games\"") != NULL);
	CHECK(strstr(text, "\"name\": \"1.69/444\", \"generation\": \"1.69/444\"") != NULL);
	CHECK(strstr(text, "\"product\": \"HanairoHeptagram\", \"profile\": \"1.553\", \"hash\": \"ec000926aebade15\"") != NULL);
	CHECK(strstr(text, "openbgi.json") != NULL && strstr(text, "\"match\"") == NULL);
	free(text);
	CHECK(Engine_SaveGamesJson(o, gPath));
	Engine_SelectProfile(o, "1.58");
	CHECK(Engine_LoadGamesJson(o, gPath) == n);
	CHECK(!o->engine.moviesPlayer);
	// "movies": "player" is kept when the file is written again, and is the file's: gone with the next one that lacks it
	o->engine.moviesPlayer = 1;
	text = Engine_FormatGamesJson(o, &len);
	CHECK(text != NULL && strstr(text, "\n\t\"movies\": \"player\",\n") != NULL);
	free(text);
	CHECK(Engine_SaveGamesJson(o, gPath));
	o->engine.moviesPlayer = 0;
	CHECK(Engine_LoadGamesJson(o, gPath) == n && o->engine.moviesPlayer == 1);
	o->engine.moviesPlayer = 0;
	text = Engine_FormatGamesJson(o, &len);
	CHECK(text != NULL && strstr(text, "\n\t\"movies\": \"player\",\n") == NULL);
	free(text);
	o->engine.moviesPlayer = 1;
	CHECK(Engine_SaveGamesJson(o, gPath) && Engine_LoadGamesJson(o, gPath) == n && o->engine.moviesPlayer);
	o->engine.moviesPlayer = 0;
	CHECK(Engine_SaveGamesJson(o, gPath) && Engine_LoadGamesJson(o, gPath) == n && !o->engine.moviesPlayer);
	CHECK(Engine_ProfileCount(o) == GEN_COUNT && strcmp(o->engine.profile->name, "1.58") == 0); // the selection survives a reload
	CHECK(ProfileNamed(o, "1.64") != NULL && ProfileNamed(o, "1.64")->dynCount == 192 && ProfileNamed(o, "1.64")->heapLimit == 0x1000000);
	CHECK(Engine_GameAt(o, n - 1, &g) && strcmp(g.product, "HarukaAonoHanayomeni") == 0 && strcmp(g.profile, "1.669") == 0);
}

static void TestProfiles(OpenBGI_t* o)
{
	const EngineProfile_t* p;
	WriteText("{ \"profiles\": [\n"
			  "  { \"name\": \"mine\", \"generation\": \"1.553\", \"heapLimit\": 1234 },\n"
			  "  { \"name\": \"1.669\" },\n"
			  "  { \"name\": \"bad\", \"generation\": \"7.0\" },\n"
			  "  { \"generation\": \"1.64\" }\n"
			  "], \"games\": [ { \"product\": \"X\", \"profile\": \"mine\" } ] }\n");
	Engine_ResetGames(o);
	// the file's entries come first, then the built-in games it lacks (all of them here)
	CHECK(Engine_LoadGamesJson(o, gPath) == 1 + gDefaultGames);
	CHECK(Engine_ProfileCount(o) == GEN_COUNT + 1); // "mine" and "1.669" of the file, then the other built-in profiles
	p = ProfileNamed(o, "mine");
	CHECK(p != NULL && p->gen == GEN_1_553 && p->heapLimit == 1234 && p->tagShift == 26 && p->dynCount == 0 && p->bootCode == 0x80000);
	p = Engine_ProfileAt(o, 0);
	CHECK(p != NULL && strcmp(p->name, "mine") == 0 && strcmp(Engine_ProfileAt(o, 1)->name, "1.669") == 0);
	p = ProfileNamed(o, "1.669");
	CHECK(p != NULL && p->gen == GEN_1_669 && p->bootCode == 0x800000);
	CHECK(ProfileNamed(o, "1.58") != NULL && ProfileNamed(o, "1.58")->gen == GEN_1_58 && ProfileNamed(o, "bad") == NULL);
	CHECK(Engine_SelectProfile(o, "mine") && o->engine.profile->heapLimit == 1234);
	CHECK(Engine_ProfileOfGen(GEN_1_553)->heapLimit == 0x4000000); // the built-in table is what the tools see
	// a reload keeps the selected profile by name when the new list has it, else falls back
	CHECK(Engine_LoadGamesJson(o, gPath) == 1 + gDefaultGames && strcmp(o->engine.profile->name, "mine") == 0);
	WriteText("{ \"profiles\": [ { \"name\": \"other\", \"generation\": \"1.58\" } ], \"games\": [] }\n");
	CHECK(Engine_LoadGamesJson(o, gPath) == gDefaultGames); // no game entries of its own: the built-in ones
	CHECK(Engine_ProfileCount(o) == GEN_COUNT + 1 && strcmp(o->engine.profile->name, "other") == 0);
	// an empty profiles list keeps the built-in table
	Engine_ResetGames(o);
	WriteText("{ \"profiles\": [ { \"name\": \"bad\", \"generation\": \"none\" } ], \"games\": [] }\n");
	Engine_LoadGamesJson(o, gPath);
	CHECK(Engine_ProfileCount(o) == GEN_COUNT);
}

static void TestGames(OpenBGI_t* o)
{
	EngineGame_t g;
	int i, nAll;
	WriteText("[\n"
			  "  { \"product\": \"Tayutama\", \"profile\": \"1.69/444\" },\n"
			  "  { \"product\": \"Trial\", \"profile\": \"1.66\", \"hash\": \"0123456789abcdef\", \"name\": \"The trial\" },\n"
			  "  { \"directory\": \"/games/x\", \"name\": \"Some game\", \"profile\": \"1.553\", \"options\": \"--debug\" },\n"
			  "  { \"product\": \"NoProfile\" },\n"
			  "  { \"match\": \"OldFormat\", \"profile\": \"1.64\" },\n"
			  "  { \"product\": \"NekoTsukuSakura\", \"profile\": \"1.654\", \"directory\": \"/games/a\" }\n"
			  "]\n");
	Engine_ResetGames(o);
	/* the entries without a profile or a directory are skipped (an entry of
	 * the old "match" format has neither product nor directory); the first
	 * entry is the Tayutama trial's built-in one (same product and profile)
	 * and the last Nekotsuku's: the other built-in games follow */
	nAll = 4 + gDefaultGames - 2;
	CHECK(Engine_LoadGamesJson(o, gPath) == nAll);
	CHECK(Engine_GameAt(o, 0, &g) && strcmp(g.product, "Tayutama") == 0 && g.hash != NULL && strcmp(g.hash, "f9ef0e96e6048b82") == 0);
	CHECK(Engine_GameAt(o, 1, &g) && strcmp(g.product, "Trial") == 0 && strcmp(g.name, "The trial") == 0 && g.directory == NULL &&
		strcmp(g.hash, "0123456789abcdef") == 0);
	CHECK(Engine_GameAt(o, 2, &g) && g.product == NULL && strcmp(g.name, "Some game") == 0 && strcmp(g.directory, "/games/x") == 0 &&
		strcmp(g.options, "--debug") == 0 && strcmp(g.profile, "1.553") == 0);
	CHECK(Engine_GameAt(o, 3, &g) && strcmp(g.name, "NekoTsukuSakura") == 0 && strcmp(g.directory, "/games/a") == 0 && g.hash != NULL);
	CHECK(Engine_GameAt(o, 4, &g) && strcmp(g.product, "NurseryRhyme") == 0 && strcmp(g.profile, "1.58") == 0);
	// recording what the launcher learnt: an existing entry gains a directory, a new one is appended
	i = Engine_RecordGame(o, 0, NULL, NULL, NULL, "/games/tayutama", "1.69/444", NULL);
	CHECK(i == 0 && Engine_GameAt(o, 0, &g) && strcmp(g.directory, "/games/tayutama") == 0 && g.name != NULL && strcmp(g.name, "Tayutama") == 0);
	i = Engine_RecordGame(o, -1, "NewGame", "fedcba9876543210", "New", "/games/new", "1.64", "--lang=ja");
	CHECK(i == nAll && Engine_GameCount(o) == nAll + 1);
	CHECK(Engine_GameAt(o, nAll, &g) && strcmp(g.product, "NewGame") == 0 && strcmp(g.hash, "fedcba9876543210") == 0 && strcmp(g.name, "New") == 0 &&
		strcmp(g.profile, "1.64") == 0 && strcmp(g.options, "--lang=ja") == 0);
	i = Engine_RecordGame(o, 0, "Other", "1111111111111111", "Tayutama", NULL, NULL, ""); // a known product and hash stay; a name equal to the product is not kept, empty options are dropped
	CHECK(i == 0 && Engine_GameAt(o, 0, &g) && strcmp(g.name, "Tayutama") == 0 && strcmp(g.product, "Tayutama") == 0 &&
		strcmp(g.hash, "f9ef0e96e6048b82") == 0 && g.options == NULL);
	i = Engine_RecordGame(o, -1, NULL, NULL, "Dir only", "/games/d", NULL, NULL);
	CHECK(Engine_GameAt(o, i, &g) && g.product == NULL && g.hash == NULL && strcmp(g.name, "Dir only") == 0);
	// the round trip through the file keeps all of it
	CHECK(Engine_SaveGamesJson(o, gPath));
	Engine_ResetGames(o);
	CHECK(Engine_LoadGamesJson(o, gPath) == nAll + 2);
	CHECK(Engine_GameAt(o, 0, &g) && strcmp(g.directory, "/games/tayutama") == 0 && g.options == NULL);
	CHECK(Engine_GameAt(o, 1, &g) && strcmp(g.name, "The trial") == 0 && strcmp(g.product, "Trial") == 0);
	CHECK(Engine_GameAt(o, nAll, &g) && strcmp(g.options, "--lang=ja") == 0 && strcmp(g.hash, "fedcba9876543210") == 0);
	CHECK(Engine_GameAt(o, nAll + 1, &g) && g.product == NULL && strcmp(g.directory, "/games/d") == 0);
	{ // the quoting of the writer: a name with a quote, a backslash and a newline
		Engine_RecordGame(o, -1, NULL, NULL, "q\"b\\n\n", "C:\\games\\x", "1.64", NULL);
		CHECK(Engine_SaveGamesJson(o, gPath));
		Engine_ResetGames(o);
		CHECK(Engine_LoadGamesJson(o, gPath) == nAll + 3);
		CHECK(Engine_GameAt(o, nAll + 2, &g) && strcmp(g.name, "q\"b\\n\n") == 0 && strcmp(g.directory, "C:\\games\\x") == 0);
	}
	{ // a file of an older build, without the retail Tayutama and its profile: both are there after the load
		WriteText("{ \"profiles\": [ { \"name\": \"1.69/444\" } ], \"games\": [ { \"product\": \"Tayutama\", \"profile\": \"1.69/444\" } ] }\n");
		Engine_ResetGames(o);
		CHECK(Engine_LoadGamesJson(o, gPath) == gDefaultGames);
		CHECK(Engine_SelectProfile(o, "1.69/451") && o->engine.profile->gen == GEN_1_69_451);
		for(i = 0; i < Engine_GameCount(o); i++)
			if(Engine_GameAt(o, i, &g) && strcmp(g.product, "Tayutama") == 0 && strcmp(g.profile, "1.69/451") == 0)
				break;
		CHECK(i < Engine_GameCount(o));
	}
}

/* The launcher's settings.  A key holds a list of values, most often one:
 * value i is replaced, a value at the list's end is added, a NULL deletes
 * (and the values after it move up).  "" is the launcher's own place, and
 * "profiles" and "games" there are the engine's: read, not written.  A
 * path is a game: its first value makes its entry, whose "product",
 * "hash", "directory" and "input" are not the launcher's to set. */
static void TestSettings(OpenBGI_t* o)
{
	char v[64];
	int n, i, found = 0;
	Engine_ResetGames(o);
	CHECK(Engine_CfgGet(o, "", "look", 0, v, sizeof v) == -1 && v[0] == 0);
	CHECK(Engine_CfgSet(o, "", "look", 0, "waves") && Engine_CfgGet(o, "", "look", 0, v, sizeof v) == 5 && strcmp(v, "waves") == 0);
	CHECK(Engine_CfgSet(o, "", "look", 0, "shapes") && Engine_CfgGet(o, "", "look", 0, v, sizeof v) == 6 && Engine_CfgGet(o, "", "look", 1, v, sizeof v) == -1);
	CHECK(Engine_CfgGet(o, "", "look", 0, v, 4) == 3 && strcmp(v, "sha") == 0); // cut to the room there is
	CHECK(!Engine_CfgSet(o, "", "", 0, "x"));
	// a list
	CHECK(Engine_CfgSet(o, "", "folders", 0, "/a") && Engine_CfgSet(o, "", "folders", 1, "/b") && Engine_CfgSet(o, "", "folders", 2, "/c"));
	CHECK(!Engine_CfgSet(o, "", "folders", 5, "/x")); // not the next place
	CHECK(Engine_CfgGet(o, "", "folders", 1, v, sizeof v) == 2 && strcmp(v, "/b") == 0 && Engine_CfgGet(o, "", "folders", 3, v, sizeof v) == -1);
	CHECK(Engine_CfgSet(o, "", "folders", 1, NULL));
	CHECK(Engine_CfgGet(o, "", "folders", 1, v, sizeof v) == 2 && strcmp(v, "/c") == 0 && Engine_CfgGet(o, "", "folders", 2, v, sizeof v) == -1);
	// the engine's lists
	CHECK(Engine_CfgGet(o, "", "profiles", 0, v, sizeof v) > 0 && strcmp(v, "1.58") == 0 && Engine_CfgGet(o, "", "profiles", GEN_COUNT, v, sizeof v) == -1);
	CHECK(!Engine_CfgSet(o, "", "profiles", 0, "mine") && !Engine_CfgSet(o, "", "games", 0, "/x"));
	CHECK(Engine_CfgGet(o, "", "games", 0, v, sizeof v) == -1); // no entry has a directory yet
	// a game
	n = Engine_GameCount(o);
	CHECK(Engine_CfgGet(o, "/games/x", "name", 0, v, sizeof v) == -1 && Engine_CfgSet(o, "/games/x", "name", 0, NULL) && Engine_GameCount(o) == n);
	CHECK(Engine_CfgSet(o, "/games/x", "name", 0, "The X") && Engine_GameCount(o) == n + 1);
	CHECK(Engine_CfgSet(o, "/games/x", "profile", 0, "1.616") && !Engine_CfgSet(o, "/games/x", "profile", 1, "1.64")); // one value at most
	CHECK(Engine_CfgSet(o, "/games/x", "patches", 0, "/p/one.zip") && Engine_CfgSet(o, "/games/x", "patches", 1, "/p/two"));
	CHECK(Engine_CfgSet(o, "/games/x", "played", 0, "2026-11-18 10:00") && Engine_CfgSet(o, "/games/x", "seconds", 0, "75"));
	CHECK(!Engine_CfgSet(o, "/games/x", "product", 0, "P") && !Engine_CfgSet(o, "/games/x", "directory", 0, "/y") && Engine_GameCount(o) == n + 1);
	CHECK(Engine_CfgGet(o, "", "games", 0, v, sizeof v) == 8 && strcmp(v, "/games/x") == 0);
	// through the file
	CHECK(Engine_SaveGamesJson(o, gPath));
	Engine_ResetGames(o);
	CHECK(Engine_CfgGet(o, "", "look", 0, v, sizeof v) == -1 && Engine_CfgGet(o, "/games/x", "name", 0, v, sizeof v) == -1);
	CHECK(Engine_LoadGamesJson(o, gPath) == n + 1);
	CHECK(Engine_CfgGet(o, "", "look", 0, v, sizeof v) == 6 && strcmp(v, "shapes") == 0);
	CHECK(Engine_CfgGet(o, "", "folders", 0, v, sizeof v) == 2 && strcmp(v, "/a") == 0 && Engine_CfgGet(o, "", "folders", 1, v, sizeof v) == 2 &&
		strcmp(v, "/c") == 0 && Engine_CfgGet(o, "", "folders", 2, v, sizeof v) == -1);
	CHECK(Engine_CfgGet(o, "/games/x", "name", 0, v, sizeof v) == 5 && strcmp(v, "The X") == 0);
	CHECK(Engine_CfgGet(o, "/games/x", "profile", 0, v, sizeof v) == 5 && strcmp(v, "1.616") == 0);
	CHECK(Engine_CfgGet(o, "/games/x", "patches", 1, v, sizeof v) == 6 && strcmp(v, "/p/two") == 0 && Engine_CfgGet(o, "/games/x", "patches", 2, v, sizeof v) == -1);
	CHECK(Engine_CfgGet(o, "/games/x", "seconds", 0, v, sizeof v) == 2 && strcmp(v, "75") == 0);
	for(i = 0; Engine_CfgGet(o, "", "games", i, v, sizeof v) >= 0; i++)
		found += strcmp(v, "/games/x") == 0;
	CHECK(found == 1);
	// a key without values is gone
	CHECK(Engine_CfgSet(o, "/games/x", "patches", 0, NULL) && Engine_CfgSet(o, "/games/x", "patches", 0, NULL) &&
		Engine_CfgGet(o, "/games/x", "patches", 0, v, sizeof v) == -1);
	// a file with nothing but the launcher's settings is a file: the built-in lists serve
	Engine_ResetGames(o);
	WriteText("{ \"launcher\": { \"look\": \"waves\", \"folders\": [\"/a\", \"/b\"] } }\n");
	CHECK(Engine_LoadGamesJson(o, gPath) == gDefaultGames);
	CHECK(Engine_CfgGet(o, "", "look", 0, v, sizeof v) == 5 && Engine_CfgGet(o, "", "folders", 1, v, sizeof v) == 2 && strcmp(v, "/b") == 0);
	// "folders" and "patches" are written as lists also when they hold one value; another key is a plain value then
	Engine_ResetGames(o);
	CHECK(Engine_CfgSet(o, "", "folders", 0, "/only") && Engine_CfgSet(o, "", "sort", 0, "name") && Engine_CfgSet(o, "/games/y", "patches", 0, "/p.zip"));
	CHECK(Engine_SaveGamesJson(o, gPath));
	{
		uint32_t size = 0;
		char* text = (char*)OS_ReadNativeFile(gPath, &size);
		CHECK(text != NULL && strstr(text, "\"folders\": [\"/only\"]") != NULL && strstr(text, "\"patches\": [\"/p.zip\"]") != NULL &&
			strstr(text, "\"sort\": \"name\"") != NULL);
		free(text);
	}
	Engine_ResetGames(o);
}

static void TestErrors(OpenBGI_t* o)
{
	int before;
	Engine_ResetGames(o);
	before = Engine_GameCount(o);
	WriteText("{ \"games\": 5 }\n");
	CHECK(Engine_LoadGamesJson(o, gPath) == 0 && Engine_GameCount(o) == before);
	WriteText("[ { \"product\": \"A\", \"profile\": \"1.64\" ");
	CHECK(Engine_LoadGamesJson(o, gPath) == 0 && Engine_GameCount(o) == before);
	CHECK(Engine_LoadGamesJson(o, "/nonexistent/dir/openbgi.json") == 0 && Engine_GameCount(o) == before);
	CHECK(!Engine_SaveGamesJson(o, "/nonexistent/dir/openbgi.json")); // a failure to write is reported, not fatal
}

int main(void)
{
	const char* tmp = getenv("BGI_TEST_TMP");
	OpenBGI_t* o = OpenBGI_New(OS_Host()); // the engine whose code the test runs
	if(!o)
		return 1;
	if(!OS_Init())
		return 1;
	snprintf(gPath, sizeof gPath, "%s/openbgi_test_openbgi.json", tmp ? tmp : ".");
	printf("defaults\n");
	TestDefaults(o);
	printf("profiles\n");
	TestProfiles(o);
	printf("games\n");
	TestGames(o);
	printf("settings\n");
	TestSettings(o);
	printf("errors\n");
	TestErrors(o);
	remove(gPath);
	OS_Shutdown();
	if(gFails)
	{
		printf("%d check(s) failed\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
