/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * version.c - the engine profiles (docs/versions.md), games.json, the
 *             game detection from ipl._bp and the opcode overrides
 *             (inc/bgi/version.h).  Not part of the original engine,
 *             which is built for one game at a time.
 *
 * The profiles and the known games are built in (the tables below) and
 * written out as the default games.json; a games.json that is found
 * replaces both lists, so a profile can be adjusted or added and a game
 * recorded without touching the binary.  The launcher (src/sys/
 * launcher.c) adds the entries of the games it started - a directory and
 * a name - and the file is written back from these lists, so an edit by
 * hand should keep to the fields described in the file's own header
 * (comments outside it are not kept).
 */
#include "bgi/version.h"
#include "bgi/json.h"
#include "bgi/os.h"
#include "bgi/engine.h" // PRODUCT_NAME_DEFAULT

// ---- the profiles --------------------------------------------------------------

/* The tagged-pointer layouts (docs/versions.md, "Tagged pointers"), as the
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
 * from build 472, 8 MB of code and 4 MB of data from 1.653 */
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
	{"1.494", GEN_1_494, 169, LAYOUT_C, BOOT_B},
	{"1.529", GEN_1_529, 169, LAYOUT_C, BOOT_B},
	{"1.535", GEN_1_535, 169, LAYOUT_D, BOOT_B},
	{"1.547", GEN_1_547, 169, LAYOUT_D, BOOT_B},
	{"1.553", GEN_1_553, 169, LAYOUT_D, BOOT_B},
	{"1.573", GEN_1_573, 171, LAYOUT_D, BOOT_B},
	{"1.588", GEN_1_588, 172, LAYOUT_D, BOOT_B},
	{"1.599", GEN_1_599, 172, LAYOUT_D, BOOT_B},
	{"1.616", GEN_1_616, 172, LAYOUT_D, BOOT_B},
	{"1.640", GEN_1_640, 172, LAYOUT_D, BOOT_B},
	{"1.653", GEN_1_653, 172, LAYOUT_D, BOOT_C},
	{"1.654", GEN_1_654, 172, LAYOUT_D, BOOT_C},
	{"1.659", GEN_1_659, 172, LAYOUT_D, BOOT_C},
	{"1.662", GEN_1_662, 172, LAYOUT_D, BOOT_C},
	{"1.667", GEN_1_667, 172, LAYOUT_D, BOOT_C},
	{"1.669", GEN_1_669, 172, LAYOUT_D, BOOT_C},
};

/* the profiles in use: the built-in ones, or those of games.json (the
 * names live in gProfileNames, the table's `name` pointers point there) */
static EngineProfile_t gProfiles[ENGINE_PROFILES_MAX];
static char gProfileNames[ENGINE_PROFILES_MAX][ENGINE_PROFILE_NAME_MAX];
static int gProfileCount;
const EngineProfile_t* gEngine = &kDefaultProfiles[GEN_1_69_444]; // the reference build until something selects another

// the built-in profiles become the table in use
static void ProfilesReset(void)
{
	int i;
	for(i = 0; i < GEN_COUNT; i++)
	{
		gProfiles[i] = kDefaultProfiles[i];
		snprintf(gProfileNames[i], sizeof gProfileNames[i], "%s", kDefaultProfiles[i].name);
		gProfiles[i].name = gProfileNames[i];
	}
	gProfileCount = GEN_COUNT;
	gEngine = &gProfiles[GEN_1_69_444];
}

static void ProfilesInit(void)
{
	if(gProfileCount == 0)
		ProfilesReset();
}

// select the profile named `name` ("1.69/444"); 1 when found, 0 (nothing changed) otherwise
int Engine_SelectProfile(const char* name)
{
	int i;
	ProfilesInit();
	for(i = 0; i < gProfileCount; i++)
		if(strcmp(gProfiles[i].name, name) == 0)
		{
			gEngine = &gProfiles[i];
			return 1;
		}
	return 0;
}

const EngineProfile_t* Engine_ProfileOfGen(EngineGen_t gen)
{
	return &kDefaultProfiles[gen];
}

int Engine_ProfileCount(void)
{
	ProfilesInit();
	return gProfileCount;
}

const EngineProfile_t* Engine_ProfileAt(int i)
{
	ProfilesInit();
	return i >= 0 && i < gProfileCount ? &gProfiles[i] : NULL;
}

// one line per profile: name, generation, compatibility level and offset width (--list-engines)
void Engine_ListProfiles(FILE* out)
{
	int i;
	ProfilesInit();
	for(i = 0; i < gProfileCount; i++)
	{
		const EngineProfile_t* p = &gProfiles[i];
		fprintf(out, "  %-10s compat %d.%02d  %d-bit offsets", p->name, p->compat / 100, p->compat % 100, p->tagShift);
		if(strcmp(p->name, kGenNames[p->gen]) != 0)
			fprintf(out, "  (opcodes of %s)", kGenNames[p->gen]);
		fputc('\n', out);
	}
}

// ---- games.json ----------------------------------------------------------------

typedef struct GameEntry
{
	char* match;     // a string of ipl._bp (UTF-8), NULL for an entry the launcher made of a directory alone
	char* profile;   // the profile name
	char* product;   // the product name ("80 E8"); defaults to `match`
	char* name;      // the name the launcher shows, NULL for `product`
	char* directory; // where the launcher found it (a native path as UTF-8), NULL when not recorded
	char* options;   // the launcher's extra command-line options, NULL for none
} GameEntry_t;
static GameEntry_t* gGames; // the entries of games.json in file order
static int gGameCount;
static char gProduct[0x100] = PRODUCT_NAME_DEFAULT; // the product name of the running game

// the games this build knows: what the default games.json lists
static const struct
{
	const char* match;
	const char* profile;
	const char* product;
} kDefaultGames[] = {
	{"NurseryRhymeTrial", "1.58", "NurseryRhyme"},
	{"NurseryRhyme", "1.64", NULL},
	{"ItsukaTodokuAnoSorani", "1.66", NULL},
	{"\xe3\x82\xbf\xe3\x83\xa6\xe3\x82\xbf\xe3\x83\x9e\xe3\x80\x90\xe4\xbd\x93\xe9\xa8\x93\xe7\x89\x88\xe3\x80\x91", "1.69/444", "Tayutama"}, // タユタマ【体験版】: the trial's own title string
	{"Tayutama", "1.69/444", NULL},
	{"\xe3\x82\xbf\xe3\x83\xa6\xe3\x82\xbf\xe3\x83\x9e", "1.69/451", "Tayutama"}, // タユタマ: the retail game's title string (its user-data folder)
	{"TayutamaHD", "1.69/472", NULL},
	{"PrismRhythm", "1.494", NULL},
	{"DiamicDays", "1.529", NULL},
	{"Gackoh_HD", "1.547", NULL},
	{"Gackoh", "1.535", NULL},
	{"HanairoHeptagram", "1.553", NULL},
	{"MagicalCharming!", "1.573", NULL},
	{"SekaiToSekaiNoMannakaDe", "1.588", NULL},
	{"UnmeisenjouNoPhi", "1.599", NULL},
	{"KodomoNoAsobi", "1.616", NULL},
	{"Tayutama2AS", "1.640", NULL},
	{"WakabaironoQuartet", "1.653", NULL},
	{"NekoTsukuSakura", "1.654", NULL},
	{"MadoiShirokinoKamikakushi", "1.659", NULL},
	{"Yumahorome", "1.662", NULL},
	{"ArcanaAlchemia", "1.667", NULL},
	{"HarukaAonoHanayomeni", "1.669", NULL},
};

const char* Engine_ProductName(void)
{
	return gProduct;
}

void Engine_SetProductName(const char* name)
{
	strncpy(gProduct, name, sizeof gProduct - 1);
	gProduct[sizeof gProduct - 1] = 0;
}

static void GamesClear(void)
{
	int i;
	for(i = 0; i < gGameCount; i++)
	{
		BGI_Free(gGames[i].match);
		BGI_Free(gGames[i].profile);
		BGI_Free(gGames[i].product);
		BGI_Free(gGames[i].name);
		BGI_Free(gGames[i].directory);
		BGI_Free(gGames[i].options);
	}
	BGI_Free(gGames);
	gGames = NULL;
	gGameCount = 0;
}

// a copy of a string, NULL for NULL or ""
static char* Dup(const char* s)
{
	return s && *s ? BGI_Strdup(s) : NULL;
}

// append an empty entry; its index
static int GamesAppend(void)
{
	GameEntry_t* grown = (GameEntry_t*)BGI_Calloc(sizeof *gGames * (size_t)(gGameCount + 1));
	if(gGameCount)
		memcpy(grown, gGames, sizeof *gGames * (size_t)gGameCount);
	BGI_Free(gGames);
	gGames = grown;
	return gGameCount++;
}

void Engine_ResetGames(void)
{
	size_t i;
	ProfilesReset();
	GamesClear();
	for(i = 0; i < BGI_COUNTOF(kDefaultGames); i++)
	{
		int k = GamesAppend();
		gGames[k].match = BGI_Strdup(kDefaultGames[i].match);
		gGames[k].profile = BGI_Strdup(kDefaultGames[i].profile);
		gGames[k].product = Dup(kDefaultGames[i].product);
	}
}

int Engine_GameCount(void)
{
	return gGameCount;
}

int Engine_GameAt(int i, EngineGame_t* out)
{
	const GameEntry_t* g;
	if(i < 0 || i >= gGameCount)
		return 0;
	g = &gGames[i];
	out->match = g->match;
	out->profile = g->profile;
	out->product = g->product ? g->product : g->match;
	out->name = g->name ? g->name : out->product;
	out->directory = g->directory;
	out->options = g->options;
	return 1;
}

int Engine_RecordGame(int index, const char* name, const char* directory, const char* profile, const char* options)
{
	GameEntry_t* g;
	if(index < 0 || index >= gGameCount)
		index = GamesAppend();
	g = &gGames[index];
	if(name && *name)
	{ // the name is kept only where it says more than the product name does
		BGI_Free(g->name);
		g->name = NULL;
		if(!g->match || strcmp(name, g->product ? g->product : g->match) != 0)
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
static int LoadProfiles(const JsonNode_t* list, const char* path)
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
			fprintf(stderr, "bgi: %s: profile \"%s\": unknown generation \"%s\" (one of the built-in profiles' names)\n", path,
				name, gen ? gen : "");
			continue;
		}
		base = &kDefaultProfiles[g];
		p = &gProfiles[n];
		snprintf(gProfileNames[n], sizeof gProfileNames[n], "%s", name);
		p->name = gProfileNames[n];
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
 * own, so that a game this build knows is detected whatever games.json
 * is in use: a profile by its name, a game by its match string.  Only
 * the memory tables grow; the file is rewritten when the launcher
 * records a game. */
static void MergeDefaults(void)
{
	size_t i;
	int k;
	for(i = 0; i < GEN_COUNT && gProfileCount < ENGINE_PROFILES_MAX; i++)
	{
		for(k = 0; k < gProfileCount; k++)
			if(strcmp(gProfiles[k].name, kDefaultProfiles[i].name) == 0)
				break;
		if(k < gProfileCount)
			continue;
		k = gProfileCount++;
		gProfiles[k] = kDefaultProfiles[i];
		snprintf(gProfileNames[k], sizeof gProfileNames[k], "%s", kDefaultProfiles[i].name);
		gProfiles[k].name = gProfileNames[k];
	}
	for(i = 0; i < BGI_COUNTOF(kDefaultGames); i++)
	{
		for(k = 0; k < gGameCount; k++)
			if(gGames[k].match && strcmp(gGames[k].match, kDefaultGames[i].match) == 0)
				break;
		if(k < gGameCount)
			continue;
		k = GamesAppend();
		gGames[k].match = BGI_Strdup(kDefaultGames[i].match);
		gGames[k].profile = BGI_Strdup(kDefaultGames[i].profile);
		gGames[k].product = Dup(kDefaultGames[i].product);
	}
}

/* Read games.json from `path`: {"profiles": [...], "games": [...]} (the
 * profiles list is optional; a bare list is a games list).  The profiles
 * replace the built-in table when the list has at least one valid entry;
 * the games replace the entry list (an entry needs "match" with "profile",
 * or "directory"); the built-in profiles and games the file lacks are
 * then appended (MergeDefaults).  Returns the number of game entries, 0
 * when the file cannot be read or is not a list (nothing is changed then;
 * a syntax error is reported on stderr). */
int Engine_LoadGamesJson(const char* path)
{
	uint32_t size;
	uint8_t* text;
	JsonNode_t* root;
	const JsonNode_t* e;
	const JsonNode_t* profiles;
	size_t err;
	const char* selected;

	text = OS_ReadNativeFile(path, &size);
	if(!text)
		return 0;
	root = Json_Parse((const char*)text, size, &err);
	free(text);
	if(!root)
	{
		fprintf(stderr, "bgi: %s: JSON syntax error at offset %u\n", path, (unsigned)err);
		return 0;
	}
	// either a bare list of entries or {"profiles": [...], "games": [...]}
	profiles = NULL;
	if(root->type == JSON_OBJECT && Json_Get(root, "games") && Json_Get(root, "games")->type == JSON_ARRAY)
	{
		e = Json_Get(root, "games")->child;
		profiles = Json_Get(root, "profiles");
	}
	else if(root->type == JSON_ARRAY)
		e = root->child;
	else
	{
		fprintf(stderr, "bgi: %s: expected a list of {\"match\", \"profile\"} entries\n", path);
		Json_Free(root);
		return 0;
	}
	ProfilesInit();
	selected = gEngine->name;
	if(profiles && profiles->type == JSON_ARRAY)
	{
		char keep[ENGINE_PROFILE_NAME_MAX];
		int n;
		snprintf(keep, sizeof keep, "%s", selected);
		n = LoadProfiles(profiles, path);
		if(n > 0)
		{
			gProfileCount = n;
			if(!Engine_SelectProfile(keep) && !Engine_SelectProfile(kDefaultProfiles[GEN_1_69_444].name))
				gEngine = &gProfiles[0];
		}
		else
			fprintf(stderr, "bgi: %s: no usable profile in \"profiles\": the built-in ones are kept\n", path);
	}
	GamesClear();
	for(; e; e = e->next)
	{
		const char* match = Json_String(e, "match", NULL);
		const char* profile = Json_String(e, "profile", NULL);
		const char* directory = Json_String(e, "directory", NULL);
		int k;
		if(!(match && profile) && !(directory && *directory))
			continue;
		k = GamesAppend();
		gGames[k].match = Dup(match);
		gGames[k].profile = Dup(profile);
		gGames[k].product = Dup(Json_String(e, "product", NULL));
		gGames[k].name = Dup(Json_String(e, "name", NULL));
		gGames[k].directory = Dup(directory);
		gGames[k].options = Dup(Json_String(e, "options", NULL));
	}
	Json_Free(root);
	MergeDefaults();
	return gGameCount;
}

// ---- writing games.json ----------------------------------------------------------

typedef struct Out
{
	char* s;
	size_t len, cap;
} Out_t;

static void OutPut(Out_t* o, const char* text)
{
	size_t n = strlen(text);
	if(o->len + n + 1 > o->cap)
	{
		o->cap = (o->cap + n + 1024) * 2;
		o->s = (char*)realloc(o->s, o->cap);
	}
	memcpy(o->s + o->len, text, n + 1);
	o->len += n;
}

// a JSON string literal: the quotes and the escapes of '"', '\\' and the control characters; UTF-8 passes through
static void OutString(Out_t* o, const char* s)
{
	char buf[8];
	OutPut(o, "\"");
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
		OutPut(o, buf);
	}
	OutPut(o, "\"");
}

static void OutField(Out_t* o, const char* key, const char* value, int* first)
{
	OutPut(o, *first ? " " : ", ");
	*first = 0;
	OutString(o, key);
	OutPut(o, ": ");
	OutString(o, value);
}

static void OutNumber(Out_t* o, const char* key, uint32_t value, int* first)
{
	char num[32];
	OutPut(o, *first ? " " : ", ");
	*first = 0;
	OutString(o, key);
	sprintf(num, ": %u", (unsigned)value);
	OutPut(o, num);
}

static const char kGamesJsonHeader[] =
	"// games.json - the engine profiles and the games this engine knows (docs/versions.md).\n"
	"//\n"
	"// \"profiles\" are the engine builds: each names its generation (the opcode set,\n"
	"// one of the built-in profiles' names) and the parameters of the virtual\n"
	"// machine - the tagged-pointer layout (tagShift, the tags of the code, data and\n"
	"// heap areas, the first tag and the number of the dynamic blocks, the local\n"
	"// heap limit) and the boot thread's code and data areas.  A profile that\n"
	"// leaves a parameter out takes the generation's built-in value.\n"
	"//\n"
	"// \"games\" are the games: \"match\" is a string that appears in the game's boot\n"
	"// program (ipl._bp: the product name the scripts hand to the engine, or the\n"
	"// window title) and \"profile\" the profile to run it with.  The first matching\n"
	"// entry in this order wins, so a trial's entry goes before the full game's when\n"
	"// their boot programs share names.  \"product\" is the name the engine build\n"
	"// carries as a literal and hands to the scripts (\"80 E8\"; the save data and\n"
	"// the uninstaller mutex are named after it) and defaults to \"match\".  The\n"
	"// launcher adds \"directory\" (where it found the game), \"name\" (what it shows)\n"
	"// and \"options\" (extra command-line options) to the games it started, and\n"
	"// rewrites this file from these fields: comments below this header are lost.\n"
	"// Strings are UTF-8; the engine converts the Shift-JIS text of ipl._bp before\n"
	"// comparing.\n"
	"//\n"
	"// The file is looked for in the game's directory, then next to the executable\n"
	"// (where a default one is written when there is none); --games=PATH or\n"
	"// BGI_GAMES=PATH name another one, --engine=NAME skips the detection.\n";

char* Engine_FormatGamesJson(size_t* len)
{
	Out_t o;
	int i;
	memset(&o, 0, sizeof o);
	ProfilesInit();
	OutPut(&o, kGamesJsonHeader);
	OutPut(&o, "{\n\t\"profiles\": [\n");
	for(i = 0; i < gProfileCount; i++)
	{
		const EngineProfile_t* p = &gProfiles[i];
		int first = 1;
		OutPut(&o, "\t\t{");
		OutField(&o, "name", p->name, &first);
		OutField(&o, "generation", kGenNames[p->gen], &first);
		OutNumber(&o, "compat", (uint32_t)p->compat, &first);
		OutNumber(&o, "tagShift", (uint32_t)p->tagShift, &first);
		OutNumber(&o, "tagCode", p->tagCode, &first);
		OutNumber(&o, "tagData", p->tagData, &first);
		OutNumber(&o, "tagHeap", p->tagHeap, &first);
		OutNumber(&o, "dynFirst", p->dynFirst, &first);
		OutNumber(&o, "dynCount", p->dynCount, &first);
		OutNumber(&o, "heapLimit", p->heapLimit, &first);
		OutNumber(&o, "bootCode", p->bootCode, &first);
		OutNumber(&o, "bootData", p->bootData, &first);
		OutPut(&o, i + 1 < gProfileCount ? " },\n" : " }\n");
	}
	OutPut(&o, "\t],\n\t\"games\": [\n");
	for(i = 0; i < gGameCount; i++)
	{
		const GameEntry_t* g = &gGames[i];
		int first = 1;
		OutPut(&o, "\t\t{");
		if(g->match)
			OutField(&o, "match", g->match, &first);
		if(g->profile)
			OutField(&o, "profile", g->profile, &first);
		if(g->product)
			OutField(&o, "product", g->product, &first);
		if(g->name)
			OutField(&o, "name", g->name, &first);
		if(g->directory)
			OutField(&o, "directory", g->directory, &first);
		if(g->options)
			OutField(&o, "options", g->options, &first);
		OutPut(&o, i + 1 < gGameCount ? " },\n" : " }\n");
	}
	OutPut(&o, "\t]\n}\n");
	if(len)
		*len = o.len;
	return o.s;
}

int Engine_SaveGamesJson(const char* path)
{
	size_t len;
	char* text = Engine_FormatGamesJson(&len);
	int ok = OS_WriteNativeFile(path, text, (uint32_t)len);
	free(text);
	return ok;
}

/* The identifying strings of an ipl._bp are its NUL-terminated literals
 * (the product name the scripts pass to the engine, the window title): walk
 * the decoded program and compare every run of 3 .. 255 printable bytes
 * that ends in a NUL - converted from Shift-JIS to UTF-8 - with the
 * entries.  The first entry in file order that matches wins, so a trial's
 * entry goes before the full game's when the trial's ipl also carries the
 * full name. */
int Engine_FindGame(const uint8_t* ipl, size_t size)
{
	size_t i = 0;
	char utf[0x400];
	int best = -1;
	while(i < size)
	{
		size_t j = i;
		while(j < size && ipl[j] != 0 && (ipl[j] >= 0x20 || ipl[j] == '\t'))
			j++;
		if(j < size && ipl[j] == 0 && j - i >= 3 && j - i < 0x100)
		{
			char sjis[0x100];
			int k;
			memcpy(sjis, ipl + i, j - i);
			sjis[j - i] = 0;
			OS_SjisToUtf8(sjis, utf, sizeof utf);
			for(k = 0; k < gGameCount; k++)
				if(gGames[k].match && strcmp(gGames[k].match, utf) == 0 && (best < 0 || k < best))
					best = k;
		}
		i = j + 1;
	}
	return best;
}

/* On a match the profile is selected and the product name set; returns
 * the profile name, or NULL when nothing matched or the entry names an
 * unknown profile (reported on stderr). */
const char* Engine_DetectGame(const uint8_t* ipl, size_t size)
{
	int best = Engine_FindGame(ipl, size);
	if(best < 0)
		return NULL;
	if(!gGames[best].profile || !Engine_SelectProfile(gGames[best].profile))
	{
		fprintf(stderr, "bgi: games.json: unknown profile \"%s\" for \"%s\"\n", gGames[best].profile ? gGames[best].profile : "",
			gGames[best].match);
		return NULL;
	}
	Engine_SetProductName(gGames[best].product ? gGames[best].product : gGames[best].match);
	return gGames[best].profile;
}

// ---- opcode overrides ------------------------------------------------------------

static int8_t gOverride[OPFAM_COUNT][256]; // 0 none, 1 disabled, 2 enabled

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
int Engine_OverrideOpcode(const char* spec, int enable)
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
	gOverride[fam][op] = (int8_t)(enable ? 2 : 1);
	return 1;
}

// the override of an opcode: -1 none, 0 disabled, 1 enabled
int Engine_OpcodeOverride(OpFamily_t fam, int op)
{
	return gOverride[fam][op] ? gOverride[fam][op] - 1 : -1;
}
