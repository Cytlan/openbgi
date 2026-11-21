/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * version.c - the engine profiles (docs/dev/versions.md), the games table of
 *             openbgi.json and the opcode overrides (src/include/bgi/version.h);
 *             the detection that uses the table is src/core/detect.c.
 *             Not part of the original engine, which is built for one
 *             game at a time.
 *
 * The profiles and the known games are built in (the tables below) and
 * written out as the default openbgi.json; an openbgi.json that is found
 * replaces both lists, so a profile can be adjusted or added and a game
 * recorded without touching the binary.  The launcher (src/sys/
 * launcher.c) adds the entries of the games it started - a directory and
 * a name - and the file is written back from these lists, so an edit by
 * hand should keep to the fields described in the file's own header
 * (comments outside it are not kept).
 */
#include "bgi/version.h"
#include "bgi/json.h"
#include "bgi/inputcfg.h"
#include "bgi/host.h"
#include "bgi/engine.h" // PRODUCT_NAME_DEFAULT
#include "bgi/release.h"
#include "bgi/core.h"

// ---- the profiles --------------------------------------------------------------

/* The tagged-pointer layouts (docs/dev/vm.md, "Tagged pointers"), as the
 * tagShift .. heapLimit members of EngineProfile_t:
 *   A  1.58 .. 1.66          24-bit offsets, tags 0x11 code / 0x10 data / 0x12 heap, dynamic from 0x40
 *   B  1.69 builds 444, 451  25-bit offsets, tags 8 / 9 / 0xA, dynamic 0x10 .. 0x3F
 *   C  1.69 build 472 ..     26-bit offsets, tags 1 / 2 / 3, dynamic 16 .. 63 */
#define LAYOUT_A 24, 0x11, 0x10, 0x12, 0x40, 192, 0x1000000
#define LAYOUT_B 25, 0x08, 0x09, 0x0a, 0x10, 48, 0x2000000
#define LAYOUT_C 26, 0x01, 0x02, 0x03, 0x10, 48, 0x4000000
#define LAYOUT_D 26, 0x01, 0x02, 0x03, 0x10, 0, 0x4000000 // 1.535 on: layout C with the dynamic blocks from size-class pools (dynCount 0, see dynmem.c)

/* the boot thread's code and data areas (the SpawnThread of the boot
 * program): 256 KB each up to build 444, 512 KB of code
 * from build 472, 8 MB of code and 4 MB of data from 1.636 */
#define BOOT_A   0x40000, 0x40000
#define BOOT_B   0x80000, 0x40000
#define BOOT_C   0x800000, 0x400000

// the built-in profiles: one per generation, named after it
static const EngineProfile_t kDefaultProfiles[GEN_COUNT] = {
	// name        generation    compat layout    boot areas
	{"1.58", GEN_1_58, 158, LAYOUT_A, BOOT_A},
	{"1.64", GEN_1_64, 164, LAYOUT_A, BOOT_A},
	{"1.66", GEN_1_66, 166, LAYOUT_A, BOOT_A},
	{"1.69/444", GEN_1_69_444, 169, LAYOUT_B, BOOT_A},
	{"1.69/451", GEN_1_69_451, 169, LAYOUT_B, BOOT_A},
	{"1.69/472", GEN_1_69_472, 169, LAYOUT_C, BOOT_B},
	{"1.492", GEN_1_492, 169, LAYOUT_C, BOOT_B},
	{"1.494", GEN_1_494, 169, LAYOUT_C, BOOT_B},
	{"1.529", GEN_1_529, 169, LAYOUT_C, BOOT_B},
	{"1.535", GEN_1_535, 169, LAYOUT_D, BOOT_B},
	{"1.547", GEN_1_547, 169, LAYOUT_D, BOOT_B},
	{"1.553", GEN_1_553, 169, LAYOUT_D, BOOT_B},
	{"1.573", GEN_1_573, 171, LAYOUT_D, BOOT_B},
	{"1.588", GEN_1_588, 172, LAYOUT_D, BOOT_B},
	{"1.599", GEN_1_599, 172, LAYOUT_D, BOOT_B},
	{"1.616", GEN_1_616, 172, LAYOUT_D, BOOT_B},
	{"1.636", GEN_1_636, 172, LAYOUT_D, BOOT_C},
	{"1.640", GEN_1_640, 172, LAYOUT_D, BOOT_C},
	{"1.653", GEN_1_653, 172, LAYOUT_D, BOOT_C},
	{"1.654", GEN_1_654, 172, LAYOUT_D, BOOT_C},
	{"1.659", GEN_1_659, 172, LAYOUT_D, BOOT_C},
	{"1.662", GEN_1_662, 172, LAYOUT_D, BOOT_C},
	{"1.667", GEN_1_667, 172, LAYOUT_D, BOOT_C},
	{"1.669", GEN_1_669, 172, LAYOUT_D, BOOT_C},
};

const char* OpenBGI_ProfileName(int i)
{
	return i >= 0 && i < GEN_COUNT ? kDefaultProfiles[i].name : NULL;
}

// the built-in profiles become the table in use
static void ProfilesReset(OpenBGI_t* o)
{
	int i;
	for(i = 0; i < GEN_COUNT; i++)
	{
		o->engine.profiles[i] = kDefaultProfiles[i];
		snprintf(o->engine.profileNames[i], sizeof o->engine.profileNames[i], "%s", kDefaultProfiles[i].name);
		o->engine.profiles[i].name = o->engine.profileNames[i];
	}
	o->engine.profileCount = GEN_COUNT;
	o->engine.profile = &o->engine.profiles[GEN_1_69_444];
}

static void ProfilesInit(OpenBGI_t* o)
{
	if(o->engine.profileCount == 0)
		ProfilesReset(o);
}

// select the profile named `name` ("1.69/444"); 1 when found, 0 (nothing changed) otherwise
int Engine_SelectProfile(OpenBGI_t* o, const char* name)
{
	int i;
	ProfilesInit(o);
	for(i = 0; i < o->engine.profileCount; i++)
		if(strcmp(o->engine.profiles[i].name, name) == 0)
		{
			o->engine.profile = &o->engine.profiles[i];
			return 1;
		}
	return 0;
}

const EngineProfile_t* Engine_ProfileOfGen(EngineGen_t gen)
{
	return &kDefaultProfiles[gen];
}

int Engine_ProfileCount(OpenBGI_t* o)
{
	ProfilesInit(o);
	return o->engine.profileCount;
}

const EngineProfile_t* Engine_ProfileAt(OpenBGI_t* o, int i)
{
	ProfilesInit(o);
	return i >= 0 && i < o->engine.profileCount ? &o->engine.profiles[i] : NULL;
}

// one line per profile: name, generation, compatibility level and offset width (--list-engines)
void Engine_ListProfiles(OpenBGI_t* o, FILE* out)
{
	int i;
	ProfilesInit(o);
	for(i = 0; i < o->engine.profileCount; i++)
	{
		const EngineProfile_t* p = &o->engine.profiles[i];
		fprintf(out, "  %-10s compat %d.%02d  %d-bit offsets", p->name, p->compat / 100, p->compat % 100, p->tagShift);
		if(strcmp(p->name, kGenNames[p->gen]) != 0)
			fprintf(out, "  (opcodes of %s)", kGenNames[p->gen]);
		fputc('\n', out);
	}
}

// ---- the games (openbgi.json) ----------------------------------------------------

/* What the launcher keeps (Engine_Cfg*): a key with one string or a
 * list of strings, in the order the keys were first set. */
typedef struct CfgValue
{
	char* key;
	char** v; // the values (`n` of them, never 0)
	int n;
	int list; // written as a list (a key that ever held two values, or was read as one)
} CfgValue_t;

typedef struct CfgSet
{
	CfgValue_t* items;
	int count;
} CfgSet_t;

typedef struct GameEntry
{
	char* product;          // the product name ("80 E8"), NULL for an entry the launcher made of a directory alone
	char* profile;          // the profile name
	char* hash;             // the hash of the game's programs (detect.h), NULL when not recorded
	char* name;             // the name the launcher shows, NULL for `product`
	char* directory;        // where the launcher found it (a native path as UTF-8), NULL when not recorded
	char* options;          // the launcher's extra command-line options, NULL for none
	InputSettings_t* input; // the entry's "input" object (bgi/inputcfg.h), NULL for none
	CfgSet_t more;          // the launcher's other settings of the game: every other member that is a string or a list of strings
} GameEntry_t;

/* The games this build knows: what the default openbgi.json lists.  The
 * product name is what "80 E8" returns and what the boot program of most
 * games compares it with; the hash (detect.h) identifies the games whose
 * programs make no such check, and tells a trial from its full game when
 * both carry the same product name. */
static const struct
{
	const char* product;
	const char* profile;
	const char* hash; // NULL when no copy of the game was at hand
	const char* name; // NULL: the product name
} kDefaultGames[] = {
	{"NurseryRhyme", "1.58", NULL, "NurseryRhyme (trial)"},
	{"NurseryRhyme", "1.64", "6d587fe9b20500c0", NULL},
	{"ItsukaTodokuAnoSorani", "1.66", "cd500a4b0b66f402", NULL},
	// Fortune Arterial (August), build 438: a subset of build 444's opcodes, on its pointer layout
	{"FortuneArterial", "1.69/444", "df6f236a194f009f", NULL},
	{"Tayutama", "1.69/444", "f9ef0e96e6048b82", "Tayutama (trial)"},
	{"Tayutama", "1.69/451", "6f4bf879ae5c1b53", NULL}, // the retail disc
	{"TayutamaHD", "1.69/472", "737b40584e06a11d", NULL},
	// Kira Kira Curtain Call (OVERDRIVE), the English edition
	{"KiraKiraCurtainCallENG", "1.492", "fe03c4188dabd1dc", NULL},
	{"PrismRhythm", "1.494", "0af1bc6609c384d0", NULL},
	{"DiamicDays", "1.529", "48659189fb5b2ee1", NULL},
	{"Gackoh", "1.535", "85449ddcc557e0af", NULL},
	{"Gackoh_HD", "1.547", "718ef05f358f0a6f", NULL},
	{"HanairoHeptagram", "1.553", "ec000926aebade15", NULL},
	{"MagicalCharming!", "1.573", "f39e60570d084c31", NULL},
	{"SekaiToSekaiNoMannakaDe", "1.588", "ae7792666d4eacbc", NULL},
	{"UnmeisenjouNoPhi", "1.599", "25c30f5be421f8fd", NULL},
	{"KodomoNoAsobi", "1.616", "6139b78eeb682f01", NULL},
	{"SenNoHatouTsukisomeNoKouki", "1.636", "65984daa7839388a", NULL},
	{"Tayutama2AS", "1.640", "3dbb76da6baa9730", NULL},
	{"WakabaironoQuartet", "1.653", "356c723797b811b1", NULL},
	{"NekoTsukuSakura", "1.654", "5a7697688ca95d06", NULL},
	// Yoake mae yori Ruri iro na (August), 1.658.2: the Windows 10 release, its PS2 and
	// Moonlight Cradle versions; 1.659's opcodes less "81 01" and its behaviour, so 1.659's profile
	{"YoakemaeyoriRuriironaWin10", "1.659", "f661e3a9fb4a4980", NULL},
	{"YoakemaeyoriRuriironaPS2Win10", "1.659", "cbf86ae9db15c689", NULL},
	{"YoakemaeyoriRuriironaMCWin10", "1.659", "87e9fa900898a306", NULL},
	{"MadoiShirokinoKamikakushi", "1.659", "8feb66b5226d9484", NULL},
	{"Yumahorome", "1.662", "d3aba0d577667236", NULL},
	{"ArcanaAlchemia", "1.667", "8e07d42f4e3df576", NULL},
	{"HarukaAonoHanayomeni", "1.669", "6a3fd794bb81d38f", NULL},
};

const char* Engine_ProductName(OpenBGI_t* o)
{
	return o->engine.product;
}

void Engine_SetProductName(OpenBGI_t* o, const char* name)
{
	strncpy(o->engine.product, name, sizeof o->engine.product - 1);
	o->engine.product[sizeof o->engine.product - 1] = 0;
}

static void CfgFree(CfgSet_t* c)
{
	int i, k;
	for(i = 0; i < c->count; i++)
	{
		for(k = 0; k < c->items[i].n; k++)
			BGI_Free(c->items[i].v[k]);
		BGI_Free(c->items[i].v);
		BGI_Free(c->items[i].key);
	}
	BGI_Free(c->items);
	c->items = NULL;
	c->count = 0;
}

static void GamesClear(OpenBGI_t* o)
{
	int i;
	if(o->engine.launcherCfg)
	{
		CfgFree(o->engine.launcherCfg);
		BGI_Free(o->engine.launcherCfg);
		o->engine.launcherCfg = NULL;
	}
	for(i = 0; i < o->engine.gameCount; i++)
	{
		CfgFree(&o->engine.games[i].more);
		BGI_Free(o->engine.games[i].product);
		BGI_Free(o->engine.games[i].profile);
		BGI_Free(o->engine.games[i].hash);
		BGI_Free(o->engine.games[i].name);
		BGI_Free(o->engine.games[i].directory);
		BGI_Free(o->engine.games[i].options);
		InputSettings_Free(o->engine.games[i].input);
	}
	BGI_Free(o->engine.games);
	o->engine.games = NULL;
	o->engine.gameCount = 0;
	InputSettings_Free(o->engine.inputAll);
	o->engine.inputAll = NULL;
	o->input.cfgAll = o->input.cfgGame = NULL; // (the mapper's hold on them)
	o->engine.moviesPlayer = 0;
	o->engine.gamesStatic = 0;
}

// a copy of a string, NULL for NULL or ""
static char* Dup(const char* s)
{
	return s && *s ? BGI_Strdup(s) : NULL;
}

// append an empty entry; its index
static int GamesAppend(OpenBGI_t* o)
{
	GameEntry_t* grown = (GameEntry_t*)BGI_Calloc(sizeof *o->engine.games * (size_t)(o->engine.gameCount + 1));
	if(o->engine.gameCount)
		memcpy(grown, o->engine.games, sizeof *o->engine.games * (size_t)o->engine.gameCount);
	BGI_Free(o->engine.games);
	o->engine.games = grown;
	return o->engine.gameCount++;
}

void Engine_FreeGames(OpenBGI_t* o)
{
	GamesClear(o);
}

void Engine_ResetGames(OpenBGI_t* o)
{
	size_t i;
	ProfilesReset(o);
	GamesClear(o);
	for(i = 0; i < BGI_COUNTOF(kDefaultGames); i++)
	{
		int k = GamesAppend(o);
		o->engine.games[k].product = BGI_Strdup(kDefaultGames[i].product);
		o->engine.games[k].profile = BGI_Strdup(kDefaultGames[i].profile);
		o->engine.games[k].hash = Dup(kDefaultGames[i].hash);
		o->engine.games[k].name = Dup(kDefaultGames[i].name);
	}
}

int Engine_GameCount(OpenBGI_t* o)
{
	return o->engine.gameCount;
}

int Engine_GameAt(OpenBGI_t* o, int i, EngineGame_t* out)
{
	const GameEntry_t* g;
	if(i < 0 || i >= o->engine.gameCount)
		return 0;
	g = &o->engine.games[i];
	out->product = g->product;
	out->profile = g->profile;
	out->hash = g->hash;
	if(g->name)
		out->name = g->name;
	else if(g->product)
		out->name = g->product;
	else
		out->name = g->directory ? g->directory : "";
	out->directory = g->directory;
	out->options = g->options;
	return 1;
}

const InputSettings_t* Engine_InputAll(OpenBGI_t* o)
{
	return o->engine.inputAll;
}

const InputSettings_t* Engine_GameInput(OpenBGI_t* o, int i)
{
	return i >= 0 && i < o->engine.gameCount ? o->engine.games[i].input : NULL;
}

int Engine_RecordGame(OpenBGI_t* o, int index, const char* product, const char* hash, const char* name, const char* directory, const char* profile,
	const char* options)
{
	GameEntry_t* g;
	if(index < 0 || index >= o->engine.gameCount)
		index = GamesAppend(o);
	g = &o->engine.games[index];
	if(product && *product && !g->product)
		g->product = BGI_Strdup(product);
	if(hash && *hash && !g->hash)
		g->hash = BGI_Strdup(hash);
	if(name && *name)
	{ // the name is kept only where it says more than the product name does
		BGI_Free(g->name);
		g->name = NULL;
		if(!g->product || strcmp(name, g->product) != 0)
			g->name = Dup(name);
	}
	if(directory)
	{
		BGI_Free(g->directory);
		g->directory = Dup(directory);
	}
	if(profile && *profile)
	{
		BGI_Free(g->profile);
		g->profile = BGI_Strdup(profile);
	}
	BGI_Free(g->options);
	g->options = Dup(options);
	return index;
}

// ---- the launcher's settings ------------------------------------------------

static CfgValue_t* CfgFind(const CfgSet_t* c, const char* key)
{
	int i;
	for(i = 0; i < c->count; i++)
		if(strcmp(c->items[i].key, key) == 0)
			return &c->items[i];
	return NULL;
}

// value `i` of `key` becomes `value`, a new last one when `i` is the count, or goes when `value` is NULL; 1 when done
static int CfgPut(CfgSet_t* c, const char* key, int i, const char* value)
{
	CfgValue_t* e = CfgFind(c, key);
	int n = e ? e->n : 0;
	if(i < 0 || i > n || (i == n && !value) || !*key)
		return i == n && !value; // nothing there to delete: done
	if(!value)
	{
		BGI_Free(e->v[i]);
		memmove(&e->v[i], &e->v[i + 1], sizeof *e->v * (size_t)(n - i - 1));
		if(--e->n == 0)
		{
			BGI_Free(e->v);
			BGI_Free(e->key);
			memmove(e, e + 1, sizeof *e * (size_t)(c->count - (int)(e - c->items) - 1));
			c->count--;
		}
		return 1;
	}
	if(!e)
	{
		CfgValue_t* grown = (CfgValue_t*)BGI_Calloc(sizeof *grown * (size_t)(c->count + 1));
		if(c->count)
			memcpy(grown, c->items, sizeof *grown * (size_t)c->count);
		BGI_Free(c->items);
		c->items = grown;
		e = &c->items[c->count++];
		e->key = BGI_Strdup(key);
	}
	if(i == n)
	{
		char** grown = (char**)BGI_Calloc(sizeof *grown * (size_t)(n + 1));
		if(n)
			memcpy(grown, e->v, sizeof *grown * (size_t)n);
		BGI_Free(e->v);
		e->v = grown;
		e->v[e->n++] = BGI_Strdup(value);
		if(e->n > 1)
			e->list = 1;
		return 1;
	}
	BGI_Free(e->v[i]);
	e->v[i] = BGI_Strdup(value);
	return 1;
}

// the members of a JSON object that are strings or lists of strings, but those named in `skip` (NULL-terminated)
static void CfgLoad(CfgSet_t* c, const JsonNode_t* obj, const char* const* skip)
{
	const JsonNode_t* m;
	for(m = obj && obj->type == JSON_OBJECT ? obj->child : NULL; m; m = m->next)
	{
		const char* const* k;
		const JsonNode_t* e;
		CfgValue_t* made;
		if(!m->key || !*m->key)
			continue;
		for(k = skip; k && *k; k++)
			if(strcmp(*k, m->key) == 0)
				break;
		if(k && *k)
			continue;
		if(m->type == JSON_STRING)
			CfgPut(c, m->key, 0, m->str ? m->str : "");
		else if(m->type == JSON_ARRAY)
		{
			int n = 0;
			for(e = m->child; e; e = e->next)
				if(e->type == JSON_STRING)
					CfgPut(c, m->key, n++, e->str ? e->str : "");
			made = CfgFind(c, m->key);
			if(made)
				made->list = 1;
		}
	}
}

static void CopyOut(const char* v, char* dst, size_t size)
{
	size_t n = strlen(v);
	if(!size)
		return;
	if(n >= size)
	{ // cut between two characters of UTF-8
		n = size - 1;
		while(n > 0 && ((uint8_t)v[n] & 0xc0) == 0x80)
			n--;
	}
	memcpy(dst, v, n);
	dst[n] = 0;
}

/* The entry of the game at `path`: the one that has it as its directory
 * and no product name, the launcher's own (`own` 1); or an entry of the
 * detection that recorded the directory as the native dialog does (0).
 * NULL for none. */
static GameEntry_t* GameAtPath(OpenBGI_t* o, const char* path, int own)
{
	int i;
	for(i = 0; i < o->engine.gameCount; i++)
	{
		GameEntry_t* g = &o->engine.games[i];
		if(g->directory && (g->product == NULL) == (own != 0) && strcmp(g->directory, path) == 0)
			return g;
	}
	return NULL;
}

// the string member of an entry a key names: "name", "profile" or "options"; NULL for another key
static char** GameField(GameEntry_t* g, const char* key)
{
	if(strcmp(key, "name") == 0)
		return &g->name;
	if(strcmp(key, "profile") == 0)
		return &g->profile;
	if(strcmp(key, "options") == 0)
		return &g->options;
	return NULL;
}

// the keys of a game's entry that are not the launcher's to set
static int GameKeyTaken(const char* key)
{
	static const char* const kTaken[] = {"product", "hash", "directory", "input"};
	size_t i;
	for(i = 0; i < BGI_COUNTOF(kTaken); i++)
		if(strcmp(key, kTaken[i]) == 0)
			return 1;
	return 0;
}

// value `i` of a key, NULL when there is none; *count its number of values
static const char* CfgLook(OpenBGI_t* o, const char* path, const char* key, int i, int* count)
{
	const CfgValue_t* e = NULL;
	*count = 0;
	if(!*path)
	{
		if(strcmp(key, "profiles") == 0)
		{
			*count = Engine_ProfileCount(o);
			return i >= 0 && i < *count ? Engine_ProfileAt(o, i)->name : NULL;
		}
		if(strcmp(key, "games") == 0)
		{
			const char* found = NULL;
			int k;
			for(k = 0; k < o->engine.gameCount; k++)
				if(o->engine.games[k].directory)
				{
					if(*count == i)
						found = o->engine.games[k].directory;
					(*count)++;
				}
			return found;
		}
		if(o->engine.launcherCfg)
			e = CfgFind(o->engine.launcherCfg, key);
	}
	else
	{
		GameEntry_t* g = GameAtPath(o, path, 1);
		char** field;
		if(!g)
		{ // what the native dialog recorded for the directory: its options
			g = GameAtPath(o, path, 0);
			if(!g || strcmp(key, "options") != 0 || !g->options)
				return NULL;
			*count = 1;
			return i == 0 ? g->options : NULL;
		}
		field = GameField(g, key);
		if(field)
		{
			*count = *field != NULL;
			return i == 0 ? *field : NULL;
		}
		e = CfgFind(&g->more, key);
	}
	if(!e)
		return NULL;
	*count = e->n;
	return i >= 0 && i < e->n ? e->v[i] : NULL;
}

int Engine_CfgGet(OpenBGI_t* o, const char* path, const char* key, int i, char* dst, size_t size)
{
	int count;
	const char* v = CfgLook(o, path, key, i, &count);
	if(size)
		dst[0] = 0;
	if(!v)
		return -1;
	CopyOut(v, dst, size);
	return (int)strlen(dst);
}

int Engine_CfgSet(OpenBGI_t* o, const char* path, const char* key, int i, const char* value)
{
	GameEntry_t* g;
	char** field;
	if(!*key)
		return 0;
	if(!*path)
	{
		if(strcmp(key, "profiles") == 0 || strcmp(key, "games") == 0)
			return 0;
		if(!o->engine.launcherCfg)
			o->engine.launcherCfg = (CfgSet_t*)BGI_Calloc(sizeof *o->engine.launcherCfg);
		return CfgPut(o->engine.launcherCfg, key, i, value);
	}
	if(GameKeyTaken(key))
		return 0;
	g = GameAtPath(o, path, 1);
	if(!g)
	{
		GameEntry_t* old;
		int k;
		if(!value)
			return 1; // nothing to delete
		k = GamesAppend(o);
		g = &o->engine.games[k];
		g->directory = BGI_Strdup(path);
		old = GameAtPath(o, path, 0);
		if(old)
		{ // the directory and what was chosen for it move to the launcher's entry
			g->options = old->options;
			old->options = NULL;
			BGI_Free(old->directory);
			old->directory = NULL;
		}
	}
	field = GameField(g, key);
	if(!field)
		return CfgPut(&g->more, key, i, value);
	if(value ? i != 0 : (i != 0 && i != (*field != NULL)))
		return 0; // one value at most
	if(i == 0)
	{
		BGI_Free(*field);
		*field = value ? BGI_Strdup(value) : NULL;
	}
	return 1;
}

// the generation named `name` (the names of kGenNames), or -1
static int GenByName(const char* name)
{
	int g;
	for(g = 0; g < GEN_COUNT; g++)
		if(strcmp(kGenNames[g], name) == 0)
			return g;
	return -1;
}

/* the "profiles" list: every entry names a profile and its generation (the
 * opcode set; the name itself when it is one); the numbers of the
 * tagged-pointer layout and the boot areas default to the built-in
 * profile of that generation, so an entry may give the few that differ */
static int LoadProfiles(OpenBGI_t* o, const JsonNode_t* list, const char* path)
{
	const JsonNode_t* e;
	int n = 0;
	for(e = list->child; e && n < ENGINE_PROFILES_MAX; e = e->next)
	{
		const char* name = Json_String(e, "name", NULL);
		const char* gen = Json_String(e, "generation", name);
		const EngineProfile_t* base;
		EngineProfile_t* p;
		int g;
		if(!name)
			continue;
		g = gen ? GenByName(gen) : -1;
		if(g < 0)
		{
			fprintf(stderr, "openbgi: %s: profile \"%s\": unknown generation \"%s\" (one of the built-in profiles' names)\n", path,
				name, gen ? gen : "");
			continue;
		}
		base = &kDefaultProfiles[g];
		p = &o->engine.profiles[n];
		snprintf(o->engine.profileNames[n], sizeof o->engine.profileNames[n], "%s", name);
		p->name = o->engine.profileNames[n];
		p->gen = (EngineGen_t)g;
		p->compat = (int)Json_Number(e, "compat", base->compat);
		p->tagShift = (int)Json_Number(e, "tagShift", base->tagShift);
		p->tagCode = (uint32_t)Json_Number(e, "tagCode", base->tagCode);
		p->tagData = (uint32_t)Json_Number(e, "tagData", base->tagData);
		p->tagHeap = (uint32_t)Json_Number(e, "tagHeap", base->tagHeap);
		p->dynFirst = (uint32_t)Json_Number(e, "dynFirst", base->dynFirst);
		p->dynCount = (uint32_t)Json_Number(e, "dynCount", base->dynCount);
		p->heapLimit = (uint32_t)Json_Number(e, "heapLimit", base->heapLimit);
		p->bootCode = (uint32_t)Json_Number(e, "bootCode", base->bootCode);
		p->bootData = (uint32_t)Json_Number(e, "bootData", base->bootData);
		n++;
	}
	return n;
}

/* The built-in profiles and games a file does not have - a file an older
 * build wrote, or one its owner trimmed - are added after the file's
 * own, so that a game this build knows is detected whatever openbgi.json
 * is in use: a profile by its name, a game by its hash or by its product
 * name and profile (such an entry without a hash gets the built-in one).
 * Only the memory tables grow; the file is rewritten when the launcher
 * records a game. */
static void MergeDefaults(OpenBGI_t* o)
{
	size_t i;
	int k;
	for(i = 0; i < GEN_COUNT && o->engine.profileCount < ENGINE_PROFILES_MAX; i++)
	{
		for(k = 0; k < o->engine.profileCount; k++)
			if(strcmp(o->engine.profiles[k].name, kDefaultProfiles[i].name) == 0)
				break;
		if(k < o->engine.profileCount)
			continue;
		k = o->engine.profileCount++;
		o->engine.profiles[k] = kDefaultProfiles[i];
		snprintf(o->engine.profileNames[k], sizeof o->engine.profileNames[k], "%s", kDefaultProfiles[i].name);
		o->engine.profiles[k].name = o->engine.profileNames[k];
	}
	for(i = 0; i < BGI_COUNTOF(kDefaultGames); i++)
	{
		const char* hash = kDefaultGames[i].hash;
		for(k = 0; k < o->engine.gameCount; k++)
		{
			const GameEntry_t* g = &o->engine.games[k];
			if(hash && g->hash && strcmp(g->hash, hash) == 0)
				break; // the same build
			if(g->product && g->profile && strcmp(g->product, kDefaultGames[i].product) == 0 &&
				strcmp(g->profile, kDefaultGames[i].profile) == 0 && (!hash || !g->hash))
				break; // the same game on the same profile
		}
		if(k < o->engine.gameCount)
		{
			if(hash && !o->engine.games[k].hash)
				o->engine.games[k].hash = BGI_Strdup(hash); // what this build knows of it
			continue;
		}
		k = GamesAppend(o);
		o->engine.games[k].product = BGI_Strdup(kDefaultGames[i].product);
		o->engine.games[k].profile = BGI_Strdup(kDefaultGames[i].profile);
		o->engine.games[k].hash = Dup(hash);
		o->engine.games[k].name = Dup(kDefaultGames[i].name);
	}
}

/* Read openbgi.json from `path`: {"profiles": [...], "games": [...]}
 * (either list may be missing; a bare list is a games list).  The
 * profiles replace the built-in table when the list has at least one
 * valid entry; the games replace the entry list (an entry needs "product"
 * with "profile", or "directory"); the built-in profiles and games the
 * file lacks are then appended (MergeDefaults).  Returns the number of game entries, 0
 * when the file cannot be read or is not a list (nothing is changed then;
 * a syntax error is reported on stderr). */
int Engine_LoadGamesJson(OpenBGI_t* o, const char* path)
{
	uint32_t size;
	uint8_t* text;
	JsonNode_t* root;
	const JsonNode_t* e;
	const JsonNode_t* profiles;
	size_t err;
	const char* selected;

	text = Host_ReadNativeFile(o, path, &size);
	if(!text)
		return 0;
	root = Json_Parse((const char*)text, size, &err);
	free(text);
	if(!root)
	{
		fprintf(stderr, "openbgi: %s: JSON syntax error at offset %u\n", path, (unsigned)err);
		return 0;
	}
	// either a bare list of entries or {"profiles": [...], "games": [...]}
	profiles = NULL;
	if(root->type == JSON_OBJECT && (!Json_Get(root, "games") || Json_Get(root, "games")->type == JSON_ARRAY))
	{
		e = Json_Get(root, "games") ? Json_Get(root, "games")->child : NULL;
		profiles = Json_Get(root, "profiles");
	}
	else if(root->type == JSON_ARRAY)
		e = root->child;
	else
	{
		fprintf(stderr, "openbgi: %s: expected a list of {\"product\", \"profile\"} entries\n", path);
		Json_Free(root);
		return 0;
	}
	ProfilesInit(o);
	selected = o->engine.profile->name;
	if(profiles && profiles->type == JSON_ARRAY)
	{
		char keep[ENGINE_PROFILE_NAME_MAX];
		int n;
		snprintf(keep, sizeof keep, "%s", selected);
		n = LoadProfiles(o, profiles, path);
		if(n > 0)
		{
			o->engine.profileCount = n;
			if(!Engine_SelectProfile(o, keep) && !Engine_SelectProfile(o, kDefaultProfiles[GEN_1_69_444].name))
				o->engine.profile = &o->engine.profiles[0];
		}
		else
			fprintf(stderr, "openbgi: %s: no usable profile in \"profiles\": the built-in ones are kept\n", path);
	}
	GamesClear(o);
	for(; e; e = e->next)
	{
		const char* product = Json_String(e, "product", NULL);
		const char* profile = Json_String(e, "profile", NULL);
		const char* directory = Json_String(e, "directory", NULL);
		int k;
		if(!(product && *product && profile) && !(directory && *directory))
			continue;
		k = GamesAppend(o);
		o->engine.games[k].product = Dup(product);
		o->engine.games[k].profile = Dup(profile);
		o->engine.games[k].hash = Dup(Json_String(e, "hash", NULL));
		o->engine.games[k].name = Dup(Json_String(e, "name", NULL));
		o->engine.games[k].directory = Dup(directory);
		o->engine.games[k].options = Dup(Json_String(e, "options", NULL));
		o->engine.games[k].input = InputSettings_Parse(Json_Get(e, "input"), path);
		if(!o->engine.games[k].product)
		{ // the launcher's entry: what else it keeps there
			static const char* const kOwn[] = {"product", "profile", "hash", "name", "directory", "options", "input", NULL};
			CfgLoad(&o->engine.games[k].more, e, kOwn);
		}
	}
	if(root->type == JSON_OBJECT)
	{
		const JsonNode_t* cfg = Json_Get(root, "launcher");
		if(cfg && cfg->type == JSON_OBJECT && cfg->child)
		{
			o->engine.launcherCfg = (CfgSet_t*)BGI_Calloc(sizeof *o->engine.launcherCfg);
			CfgLoad(o->engine.launcherCfg, cfg, NULL);
		}
		o->engine.inputAll = InputSettings_Parse(Json_Get(root, "input"), path);
		o->engine.moviesPlayer = strcmp(Json_String(root, "movies", ""), "player") == 0;
		{
			const JsonNode_t* fixed = Json_Get(root, "staticConfig");
			o->engine.gamesStatic = fixed && fixed->type == JSON_BOOL && fixed->num != 0;
		}
	}
	Json_Free(root);
	MergeDefaults(o);
	return o->engine.gameCount;
}

// ---- writing openbgi.json --------------------------------------------------------

typedef struct Out
{
	char* s;
	size_t len, cap;
} Out_t;

static void OutPut(Out_t* out, const char* text)
{
	size_t n = strlen(text);
	if(out->len + n + 1 > out->cap)
	{
		out->cap = (out->cap + n + 1024) * 2;
		out->s = (char*)realloc(out->s, out->cap);
	}
	memcpy(out->s + out->len, text, n + 1);
	out->len += n;
}

// a JSON string literal: the quotes and the escapes of '"', '\\' and the control characters; UTF-8 passes through
static void OutString(Out_t* out, const char* s)
{
	char buf[8];
	OutPut(out, "\"");
	for(; *s; s++)
	{
		unsigned char c = (unsigned char)*s;
		if(c == '"' || c == '\\')
		{
			buf[0] = '\\';
			buf[1] = (char)c;
			buf[2] = 0;
		}
		else if(c == '\n')
			strcpy(buf, "\\n");
		else if(c == '\r')
			strcpy(buf, "\\r");
		else if(c == '\t')
			strcpy(buf, "\\t");
		else if(c < 0x20)
			sprintf(buf, "\\u%04x", c);
		else
		{
			buf[0] = (char)c;
			buf[1] = 0;
		}
		OutPut(out, buf);
	}
	OutPut(out, "\"");
}

static void OutField(Out_t* out, const char* key, const char* value, int* first)
{
	OutPut(out, *first ? " " : ", ");
	*first = 0;
	OutString(out, key);
	OutPut(out, ": ");
	OutString(out, value);
}

static void OutNumber(Out_t* out, const char* key, uint32_t value, int* first)
{
	char num[32];
	OutPut(out, *first ? " " : ", ");
	*first = 0;
	OutString(out, key);
	sprintf(num, ": %u", (unsigned)value);
	OutPut(out, num);
}

// the settings of a set as members: `"key": "value"` or `"key": ["a", "b"]`, each after `sep`
static void OutCfg(Out_t* out, const CfgSet_t* c, const char* sep, int* first)
{
	int i, k;
	for(i = 0; i < c->count; i++)
	{
		const CfgValue_t* e = &c->items[i];
		OutPut(out, *first ? (sep[0] == ',' ? sep + 1 : sep) : sep);
		*first = 0;
		OutString(out, e->key);
		OutPut(out, ": ");
		if(!e->list && strcmp(e->key, "folders") != 0 && strcmp(e->key, "patches") != 0) // (those two are lists by their nature: also with one value)
			OutString(out, e->v[0]);
		else
		{
			OutPut(out, "[");
			for(k = 0; k < e->n; k++)
			{
				if(k)
					OutPut(out, ", ");
				OutString(out, e->v[k]);
			}
			OutPut(out, "]");
		}
	}
}

static const char kGamesJsonHeader[] =
	"// openbgi.json - the engine profiles and the games OpenBGI knows (docs/dev/versions.md),\n"
	"// written by " OPENBGI_TITLE " (\"openbgi\" below).\n"
	"//\n"
	"// \"profiles\" are the engine builds: each names its generation (the opcode set,\n"
	"// one of the built-in profiles' names) and the parameters of the virtual\n"
	"// machine - the tagged-pointer layout (tagShift, the tags of the code, data and\n"
	"// heap areas, the first tag and the number of the dynamic blocks, the local\n"
	"// heap limit) and the boot thread's code and data areas.  A profile that\n"
	"// leaves a parameter out takes the generation's built-in value.\n"
	"//\n"
	"// \"games\" are the games: \"product\" is the product name the game's boot\n"
	"// program compares \"80 E8\" with (and the name the engine hands to its scripts:\n"
	"// the save data and the uninstaller mutex are named after it), \"profile\" the\n"
	"// profile to run it with, and \"hash\" the hash of its programs (ipl._bp and\n"
	"// sysprg.arc; --detect prints it).  A game is found by its product name when\n"
	"// one profile carries it, else by its hash; a game neither identifies is given\n"
	"// the engine its opcodes fit best.  The launcher adds \"directory\" (where it\n"
	"// found the game), \"name\" (what it shows) and \"options\" (extra command-line\n"
	"// options) to the games it started, and rewrites this file from these fields:\n"
	"// comments below this header are lost.  Strings are UTF-8.\n"
	"//\n"
	"// \"launcher\", at the top, is what the launcher keeps for itself: \"folders\"\n"
	"// (the folders it looks for games in, and single games: a folder, a .zip, an\n"
	"// .iso) and how it was left.  An entry of \"games\" with a \"directory\" and no\n"
	"// \"product\" holds what it keeps for the game there: \"name\", \"profile\" (the\n"
	"// compatibility chosen; none: detected), \"options\", \"patches\" (archives or\n"
	"// folders laid over the game, the later over the earlier), \"removed\", when it\n"
	"// was last played and for how long (\"played\", \"seconds\"), the file of its\n"
	"// picture from then (\"picture\", in the folder \"pictures\" of the user's data) -\n"
	"// until it was played, the picture of a few seconds of it that the launcher ran\n"
	"// out of sight (\"preview\": \"none\" when that showed nothing; taken out, it is\n"
	"// tried again) - and the icon of the game's own program (\"icon\", in the folder\n"
	"// \"icons\"; \"none\" when it has none; taken out, it is looked for again).\n"
	"// \"preview\": \"none\" in \"launcher\" itself: no such pictures for any game.\n"
	"//\n"
	"// \"input\" - here at the top for every game, or in a game's entry for that game -\n"
	"// says what a pad's controls do and more (docs/dev/input.md):\n"
	"//     \"input\": { \"pad\": { \"west\": \"key:A\", \"l2\": \"mouse:middle\" },\n"
	"//                \"ownPadKeys\": false, \"tapDelay\": 50,\n"
	"//                \"hotspots\": [ { \"name\": \"door\", \"rect\": [100, 200, 80, 60], \"program\": \"map\" } ] }\n"
	"//\n"
	"// \"movies\": \"player\", at the top, has the system's own movie player play the\n"
	"// movies where there is one (Windows), as the original does, instead of the\n"
	"// decoder built into OpenBGI; --movies=player or --movies=decoder says it for a run.\n"
	"//\n"
	"// The file is looked for in the game's directory, then next to the executable,\n"
	"// then in the user's data folder (where a default one is written when there is\n"
	"// none); --games=PATH or BGI_GAMES=PATH name another one, --engine=NAME skips\n"
	"// the detection.  A file in one of those places that cannot be written is not\n"
	"// used, unless it says \"staticConfig\": true at its top: then it is used as it\n"
	"// is and never written.\n";

char* Engine_FormatGamesJson(OpenBGI_t* o, size_t* len)
{
	Out_t out;
	int i;
	memset(&out, 0, sizeof out);
	ProfilesInit(o);
	OutPut(&out, kGamesJsonHeader);
	OutPut(&out, "{\n\t\"openbgi\": \"" OPENBGI_VERSION "\",\n");
	if(o->engine.launcherCfg && o->engine.launcherCfg->count)
	{
		int first = 1;
		OutPut(&out, "\t\"launcher\": {");
		OutCfg(&out, o->engine.launcherCfg, ",\n\t\t", &first);
		OutPut(&out, "\n\t},\n");
	}
	if(o->engine.moviesPlayer)
		OutPut(&out, "\t\"movies\": \"player\",\n");
	if(o->engine.inputAll)
	{
		char* text = InputSettings_Format(o->engine.inputAll);
		if(text)
		{
			OutPut(&out, "\t\"input\": ");
			OutPut(&out, text);
			OutPut(&out, ",\n");
		}
		free(text);
	}
	OutPut(&out, "\t\"profiles\": [\n");
	for(i = 0; i < o->engine.profileCount; i++)
	{
		const EngineProfile_t* p = &o->engine.profiles[i];
		int first = 1;
		OutPut(&out, "\t\t{");
		OutField(&out, "name", p->name, &first);
		OutField(&out, "generation", kGenNames[p->gen], &first);
		OutNumber(&out, "compat", (uint32_t)p->compat, &first);
		OutNumber(&out, "tagShift", (uint32_t)p->tagShift, &first);
		OutNumber(&out, "tagCode", p->tagCode, &first);
		OutNumber(&out, "tagData", p->tagData, &first);
		OutNumber(&out, "tagHeap", p->tagHeap, &first);
		OutNumber(&out, "dynFirst", p->dynFirst, &first);
		OutNumber(&out, "dynCount", p->dynCount, &first);
		OutNumber(&out, "heapLimit", p->heapLimit, &first);
		OutNumber(&out, "bootCode", p->bootCode, &first);
		OutNumber(&out, "bootData", p->bootData, &first);
		OutPut(&out, i + 1 < o->engine.profileCount ? " },\n" : " }\n");
	}
	OutPut(&out, "\t],\n\t\"games\": [\n");
	for(i = 0; i < o->engine.gameCount; i++)
	{
		const GameEntry_t* g = &o->engine.games[i];
		int first = 1;
		OutPut(&out, "\t\t{");
		if(g->product)
			OutField(&out, "product", g->product, &first);
		if(g->profile)
			OutField(&out, "profile", g->profile, &first);
		if(g->hash)
			OutField(&out, "hash", g->hash, &first);
		if(g->name)
			OutField(&out, "name", g->name, &first);
		if(g->directory)
			OutField(&out, "directory", g->directory, &first);
		if(g->options)
			OutField(&out, "options", g->options, &first);
		OutCfg(&out, &g->more, ", ", &first);
		if(g->input)
		{
			char* text = InputSettings_Format(g->input);
			if(text)
			{
				OutPut(&out, first ? " \"input\": " : ", \"input\": ");
				OutPut(&out, text);
				first = 0;
			}
			free(text);
		}
		OutPut(&out, i + 1 < o->engine.gameCount ? " },\n" : " }\n");
	}
	OutPut(&out, "\t]\n}\n");
	if(len)
		*len = out.len;
	return out.s;
}

int Engine_SaveGamesJson(OpenBGI_t* o, const char* path)
{
	size_t len;
	char* text = Engine_FormatGamesJson(o, &len);
	int ok = Host_WriteNativeFile(o, path, text, (uint32_t)len);
	free(text);
	return ok;
}

// ---- opcode overrides ------------------------------------------------------------

// the family of a spec's "main" / "7F" / "80" .. "EF" prefix (n characters); -1 when unknown
static int FamilyOf(const char* s, size_t n)
{
	static const char* const names[OPFAM_COUNT] = {"main", "7F", "80", "81", "90", "91", "92", "A0", "B0", "C0", "D0", "E0", "EF"};
	int i;
	for(i = 0; i < OPFAM_COUNT; i++)
		if(strlen(names[i]) == n && strncmp(names[i], s, n) == 0)
			return i;
	return -1;
}

// record an override from "FAMILY:OP" (the opcode in hex); 1 when the spec was understood
int Engine_OverrideOpcode(OpenBGI_t* o, const char* spec, int enable)
{
	const char* colon = strchr(spec, ':');
	int fam;
	long op;
	char* end;
	if(!colon)
		return 0;
	fam = FamilyOf(spec, (size_t)(colon - spec));
	op = strtol(colon + 1, &end, 16);
	if(fam < 0 || *end || op < 0 || op > 255)
		return 0;
	o->engine.override[fam][op] = (int8_t)(enable ? 2 : 1);
	return 1;
}

// the override of an opcode: -1 none, 0 disabled, 1 enabled
int Engine_OpcodeOverride(OpenBGI_t* o, OpFamily_t fam, int op)
{
	return o->engine.override[fam][op] ? o->engine.override[fam][op] - 1 : -1;
}
