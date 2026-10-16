/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/games.c - unit test of the engine profiles and games.json
 *                 (src/core/version.c, declared in inc/bgi/version.h);
 *                 built as bin/test_games by `make test`
 *
 * The groups of tests, in the order they run:
 *
 *   defaults  - the built-in profiles and games, and the default
 *               games.json the engine writes: it reads back to the same
 *               lists
 *   profiles  - a games.json "profiles" list replaces the table: a
 *               profile may rename a generation and change single
 *               parameters, the others follow the generation's built-in
 *               values; an unknown generation is skipped; the built-in
 *               profiles the list lacks come after it
 *   games     - the game entries: match, product, the launcher's
 *               directory / name / options, the detection of a boot
 *               program's strings (Shift-JIS converted), the first entry
 *               winning, the recording of launched games; the built-in
 *               games the file lacks come after its own
 *   errors    - files that are not a games table leave the lists alone
 *
 * The files are written to and read from the scratch directory of the
 * test run (BGI_TEST_TMP, else the current directory).
 */
#include "bgi/version.h"
#include "bgi/os.h"

#include <stdio.h>

static int gFails; // the number of failed checks so far
#define CHECK(c)                                                  \
	do                                                            \
	{                                                             \
		if(!(c))                                                  \
		{                                                         \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			gFails++;                                             \
		}                                                         \
	} while(0)

static char gPath[0x400]; // the scratch file
static int gDefaultGames; // the number of built-in game entries

static void WriteText(const char* text)
{
	CHECK(OS_WriteNativeFile(gPath, text, (uint32_t)strlen(text)));
}

static const EngineProfile_t* ProfileNamed(const char* name)
{
	int i;
	for(i = 0; i < Engine_ProfileCount(); i++)
		if(strcmp(Engine_ProfileAt(i)->name, name) == 0)
			return Engine_ProfileAt(i);
	return NULL;
}

static void TestDefaults(void)
{
	size_t len;
	char* text;
	EngineGame_t g;
	int n;
	Engine_ResetGames();
	CHECK(Engine_ProfileCount() == GEN_COUNT);
	CHECK(ProfileNamed("1.69/444") != NULL && ProfileNamed("1.69/444")->gen == GEN_1_69_444);
	CHECK(ProfileNamed("1.669") != NULL && ProfileNamed("1.669")->tagShift == 26 && ProfileNamed("1.669")->bootCode == 0x800000);
	CHECK(Engine_SelectProfile("1.553") && gEngine->gen == GEN_1_553 && gEngine->compat == 169);
	CHECK(!Engine_SelectProfile("9.99"));
	n = gDefaultGames = Engine_GameCount();
	CHECK(n >= 23);
	CHECK(ProfileNamed("1.69/451") != NULL && ProfileNamed("1.69/451")->gen == GEN_1_69_451 && ProfileNamed("1.69/451")->tagShift == 25);
	CHECK(Engine_GameAt(0, &g) && strcmp(g.match, "NurseryRhymeTrial") == 0 && strcmp(g.product, "NurseryRhyme") == 0);
	CHECK(Engine_GameAt(1, &g) && strcmp(g.product, g.match) == 0 && strcmp(g.name, g.match) == 0 && g.directory == NULL);
	CHECK(!Engine_GameAt(n, &g));
	// the default file reads back to the same lists
	text = Engine_FormatGamesJson(&len);
	CHECK(text != NULL && len > 1000 && strstr(text, "\"profiles\"") != NULL && strstr(text, "\"games\"") != NULL);
	CHECK(strstr(text, "\"name\": \"1.69/444\", \"generation\": \"1.69/444\"") != NULL);
	CHECK(strstr(text, "\"match\": \"HanairoHeptagram\", \"profile\": \"1.553\"") != NULL);
	free(text);
	CHECK(Engine_SaveGamesJson(gPath));
	Engine_SelectProfile("1.58");
	CHECK(Engine_LoadGamesJson(gPath) == n);
	CHECK(Engine_ProfileCount() == GEN_COUNT && strcmp(gEngine->name, "1.58") == 0); // the selection survives a reload
	CHECK(ProfileNamed("1.64") != NULL && ProfileNamed("1.64")->dynCount == 192 && ProfileNamed("1.64")->heapLimit == 0x1000000);
	CHECK(Engine_GameAt(n - 1, &g) && strcmp(g.match, "HarukaAonoHanayomeni") == 0 && strcmp(g.profile, "1.669") == 0);
}

static void TestProfiles(void)
{
	const EngineProfile_t* p;
	WriteText("{ \"profiles\": [\n"
			  "  { \"name\": \"mine\", \"generation\": \"1.553\", \"heapLimit\": 1234 },\n"
			  "  { \"name\": \"1.669\" },\n"
			  "  { \"name\": \"bad\", \"generation\": \"7.0\" },\n"
			  "  { \"generation\": \"1.64\" }\n"
			  "], \"games\": [ { \"match\": \"X\", \"profile\": \"mine\" } ] }\n");
	Engine_ResetGames();
	// the file's entries come first, then the built-in games it lacks (all of them here)
	CHECK(Engine_LoadGamesJson(gPath) == 1 + gDefaultGames);
	CHECK(Engine_ProfileCount() == GEN_COUNT + 1); // "mine" and "1.669" of the file, then the other built-in profiles
	p = ProfileNamed("mine");
	CHECK(p != NULL && p->gen == GEN_1_553 && p->heapLimit == 1234 && p->tagShift == 26 && p->dynCount == 0 && p->bootCode == 0x80000);
	p = Engine_ProfileAt(0);
	CHECK(p != NULL && strcmp(p->name, "mine") == 0 && strcmp(Engine_ProfileAt(1)->name, "1.669") == 0);
	p = ProfileNamed("1.669");
	CHECK(p != NULL && p->gen == GEN_1_669 && p->bootCode == 0x800000);
	CHECK(ProfileNamed("1.58") != NULL && ProfileNamed("1.58")->gen == GEN_1_58 && ProfileNamed("bad") == NULL);
	CHECK(Engine_SelectProfile("mine") && gEngine->heapLimit == 1234);
	CHECK(Engine_ProfileOfGen(GEN_1_553)->heapLimit == 0x4000000); // the built-in table is what the tools see
	// a reload keeps the selected profile by name when the new list has it, else falls back
	CHECK(Engine_LoadGamesJson(gPath) == 1 + gDefaultGames && strcmp(gEngine->name, "mine") == 0);
	WriteText("{ \"profiles\": [ { \"name\": \"other\", \"generation\": \"1.58\" } ], \"games\": [] }\n");
	CHECK(Engine_LoadGamesJson(gPath) == gDefaultGames); // no game entries of its own: the built-in ones
	CHECK(Engine_ProfileCount() == GEN_COUNT + 1 && strcmp(gEngine->name, "other") == 0);
	// an empty profiles list keeps the built-in table
	Engine_ResetGames();
	WriteText("{ \"profiles\": [ { \"name\": \"bad\", \"generation\": \"none\" } ], \"games\": [] }\n");
	Engine_LoadGamesJson(gPath);
	CHECK(Engine_ProfileCount() == GEN_COUNT);
}

static void TestGames(void)
{
	EngineGame_t g;
	int i, nAll;
	// Shift-JIS "タユタマ【体験版】" as a boot program would hold it, among other strings
	static const uint8_t ipl[] = "\x01\x02junk\0Tayutama\0\x83^\x83\x86\x83^\x83}\x81y\x91\xcc\x8c\xb1\x94\xc5\x81z\0rest\0";
	WriteText("[\n"
			  "  { \"match\": \"Tayutama\", \"profile\": \"1.69/444\" },\n"
			  "  { \"match\": \"\xe3\x82\xbf\xe3\x83\xa6\xe3\x82\xbf\xe3\x83\x9e\xe3\x80\x90\xe4\xbd\x93\xe9\xa8\x93\xe7\x89\x88\xe3\x80\x91\", "
			  "\"profile\": \"1.66\", \"product\": \"Trial\", \"name\": \"The trial\" },\n"
			  "  { \"directory\": \"/games/x\", \"name\": \"Some game\", \"profile\": \"1.553\", \"options\": \"--debug\" },\n"
			  "  { \"match\": \"NoProfile\" },\n"
			  "  { \"match\": \"Alias\", \"profile\": \"1.64\", \"directory\": \"/games/a\" }\n"
			  "]\n");
	Engine_ResetGames();
	// the entry without a profile or a directory is skipped; two of the file's matches are built-in games, the other built-in games follow
	nAll = 4 + gDefaultGames - 2;
	CHECK(Engine_LoadGamesJson(gPath) == nAll);
	CHECK(Engine_GameAt(4, &g) && g.match != NULL && strcmp(g.match, "NurseryRhymeTrial") == 0);
	CHECK(Engine_GameAt(1, &g) && strcmp(g.product, "Trial") == 0 && strcmp(g.name, "The trial") == 0 && g.directory == NULL);
	CHECK(Engine_GameAt(2, &g) && g.match == NULL && strcmp(g.name, "Some game") == 0 && strcmp(g.directory, "/games/x") == 0 &&
		strcmp(g.options, "--debug") == 0 && strcmp(g.profile, "1.553") == 0);
	CHECK(Engine_GameAt(3, &g) && strcmp(g.name, "Alias") == 0 && strcmp(g.directory, "/games/a") == 0);
	// detection: both strings of the boot program match; the first entry in file order wins
	CHECK(Engine_FindGame(ipl, sizeof ipl) == 0);
	Engine_SelectProfile("1.669");
	CHECK(Engine_DetectGame(ipl, sizeof ipl) != NULL && strcmp(gEngine->name, "1.69/444") == 0);
	CHECK(strcmp(Engine_ProductName(), "Tayutama") == 0);
	CHECK(Engine_FindGame((const uint8_t*)"nothing\0here\0", 13) == -1);
	CHECK(Engine_FindGame((const uint8_t*)"Alias\0", 6) == 3);
	// recording what the launcher learnt: an existing entry gains a directory, a new one is appended
	i = Engine_RecordGame(0, NULL, "/games/tayutama", "1.69/444", NULL);
	CHECK(i == 0 && Engine_GameAt(0, &g) && strcmp(g.directory, "/games/tayutama") == 0 && g.name != NULL && strcmp(g.name, "Tayutama") == 0);
	i = Engine_RecordGame(-1, "New", "/games/new", "1.64", "--lang=ja");
	CHECK(i == nAll && Engine_GameCount() == nAll + 1);
	CHECK(Engine_GameAt(nAll, &g) && g.match == NULL && strcmp(g.name, "New") == 0 && strcmp(g.profile, "1.64") == 0 &&
		strcmp(g.options, "--lang=ja") == 0);
	i = Engine_RecordGame(0, "Tayutama", NULL, NULL, ""); // a name equal to the product is not kept, empty options are dropped
	CHECK(i == 0 && Engine_GameAt(0, &g) && strcmp(g.name, "Tayutama") == 0 && g.options == NULL);
	// the round trip through the file keeps all of it
	CHECK(Engine_SaveGamesJson(gPath));
	Engine_ResetGames();
	CHECK(Engine_LoadGamesJson(gPath) == nAll + 1);
	CHECK(Engine_GameAt(0, &g) && strcmp(g.directory, "/games/tayutama") == 0 && g.options == NULL);
	CHECK(Engine_GameAt(1, &g) && strcmp(g.name, "The trial") == 0 && strcmp(g.product, "Trial") == 0);
	CHECK(Engine_GameAt(nAll, &g) && strcmp(g.options, "--lang=ja") == 0);
	{ // the quoting of the writer: a name with a quote, a backslash and a newline
		Engine_RecordGame(-1, "q\"b\\n\n", "C:\\games\\x", "1.64", NULL);
		CHECK(Engine_SaveGamesJson(gPath));
		Engine_ResetGames();
		CHECK(Engine_LoadGamesJson(gPath) == nAll + 2);
		CHECK(Engine_GameAt(nAll + 1, &g) && strcmp(g.name, "q\"b\\n\n") == 0 && strcmp(g.directory, "C:\\games\\x") == 0);
	}
	{ // a file of an older build, without the retail Tayutama and its profile: both are there after the load
		static const uint8_t retail[] = "x\0\x83^\x83\x86\x83^\x83}\0";
		WriteText("{ \"profiles\": [ { \"name\": \"1.69/444\" } ], \"games\": [ { \"match\": \"Tayutama\", \"profile\": \"1.69/444\" } ] }\n");
		Engine_ResetGames();
		CHECK(Engine_LoadGamesJson(gPath) == gDefaultGames);
		CHECK(Engine_DetectGame(retail, sizeof retail) != NULL && strcmp(gEngine->name, "1.69/451") == 0 && gEngine->gen == GEN_1_69_451);
		CHECK(strcmp(Engine_ProductName(), "Tayutama") == 0);
	}
}

static void TestErrors(void)
{
	int before;
	Engine_ResetGames();
	before = Engine_GameCount();
	WriteText("{ \"games\": 5 }\n");
	CHECK(Engine_LoadGamesJson(gPath) == 0 && Engine_GameCount() == before);
	WriteText("[ { \"match\": \"A\", \"profile\": \"1.64\" ");
	CHECK(Engine_LoadGamesJson(gPath) == 0 && Engine_GameCount() == before);
	CHECK(Engine_LoadGamesJson("/nonexistent/dir/games.json") == 0 && Engine_GameCount() == before);
	CHECK(!Engine_SaveGamesJson("/nonexistent/dir/games.json")); // a failure to write is reported, not fatal
}

int main(void)
{
	const char* tmp = getenv("BGI_TEST_TMP");
	if(!OS_Init())
		return 1;
	snprintf(gPath, sizeof gPath, "%s/bgi_test_games.json", tmp ? tmp : ".");
	printf("defaults\n");
	TestDefaults();
	printf("profiles\n");
	TestProfiles();
	printf("games\n");
	TestGames();
	printf("errors\n");
	TestErrors();
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
