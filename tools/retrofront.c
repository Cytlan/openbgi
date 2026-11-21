/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * retrofront.c - a small libretro frontend without a window, for checking
 *                the libretro core (`make check-libretro`,
 *                tools/checkretro.py; docs/dev/libretro.md)
 *
 *     retrofront [options] CORE CONTENT
 *
 *     --run=MS       how long the game runs: MS * 60 / 1000 calls of
 *                    retro_run (10000)
 *     --every=MS     a picture every MS (0: none), at the times a script
 *                    would name them (--offset)
 *     --shots=PREFIX the pictures as PREFIX<ms>.ppm, 9 digits; the sound
 *                    summed up a second at a time as PREFIXsound.txt
 *     --offset=MS    a script's times and the pictures' names are MS
 *                    later than the core's clock (0): the test host's
 *                    clock is a second ahead of the core's (it measures
 *                    the processor for a second first), so its scripts
 *                    and pictures fit with --offset=1000
 *     --input=FILE   the player's input, from a script
 *     --loads=N      load the game N times in turn (1), each for the whole
 *                    run: unload and load again, as a frontend does
 *     --reopen       load the core's library anew for each load, too
 *     --system=DIR   the frontend's system directory (none)
 *     --save=DIR     its save directory (none)
 *     --option=KEY=VALUE  a core option's value (else its first; a value
 *                    that is none of the option's choices is not handed on)
 *     --options=N    the version of the core options interface the
 *                    frontend says it has: 2 (with categories), or 0 for
 *                    the plain variables only
 *     --display=WxH  the display's size, told to a core that asks (else
 *                    the frontend does not know it)
 *     --list         print what the core told at the end of the first
 *                    load: each option with its choices, each control
 *                    with what it does
 *
 * The script is a subset of the test host's (src/host/headless/main.c),
 * one action per line, `MS ACTION [ARGUMENTS]` (FROM-TO/EVERY for a
 * repeated one), `#` begins a comment:
 *
 *     key K, down K, up K   a key of the keyboard: RETURN, SPACE, ESCAPE,
 *                    BACK, TAB, SHIFT, CONTROL, UP, DOWN, LEFT, RIGHT, F1
 *                    to F12, a letter or a digit (key: let go 100 ms later)
 *     type TEXT      type the rest of the line (UTF-8), a character a key
 *     click X Y, rclick X Y  move the mouse to X Y (the pixels of the
 *                    picture the core hands over, through the pointer
 *                    device) and click (100 ms)
 *     move X Y       move it there
 *     rel DX DY      move it by DX DY (the mouse's own moves)
 *     wheel N        turn the wheel N notches (+ away from the player)
 *     tap X Y        touch the screen at X Y for 80 ms (needs
 *                    --option=openbgi_pointer="touch screen")
 *     pad C, paddown C, padup C  a control of the RetroPad: south, east,
 *                    west, north, l1, r1, l2, r2, select, start, up, down,
 *                    left, right (pad: let go 100 ms later)
 *     reset          the frontend's reset: the game from its start
 *     option KEY=VALUE  the player changes a core option
 *     shot           a picture at MS besides those of --every
 *     end            stop the run
 *
 * The result is a line: the calls made, the pictures the core handed
 * over, their size, the calls that brought no new one, the sound's
 * frames, and what the core said at the end (it asked to be shut down, or
 * was stopped); a line before it for every size the core announced on
 * the way.  The exit code is 0 when every load succeeded, every call
 * brought a picture's worth of sound and the core told the same number
 * of options each time.
 *
 * ISO C but for loading the library: dlopen on POSIX, LoadLibrary on
 * Windows.
 */
#include "host/libretro/libretro.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef HMODULE Lib_t;
typedef FARPROC Sym_t;
#define LibOpen(p)   LoadLibraryA(p)
#define LibSym(l, n) GetProcAddress(l, n)
#define LibClose(l)  FreeLibrary(l)
#else
#include <dlfcn.h>
typedef void* Lib_t;
typedef void* Sym_t;
#define LibOpen(p)   dlopen(p, RTLD_NOW | RTLD_LOCAL)
#define LibSym(l, n) dlsym(l, n)
#define LibClose(l)  dlclose(l)
#endif

#define FPS 60

// ---- the core's functions ---------------------------------------------------------------------

static struct
{
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	void (*init)(void);
	void (*deinit)(void);
	unsigned (*api_version)(void);
	void (*get_system_info)(struct retro_system_info*);
	void (*get_system_av_info)(struct retro_system_av_info*);
	bool (*load_game)(const struct retro_game_info*);
	void (*unload_game)(void);
	void (*run)(void);
	void (*reset)(void);
	size_t (*serialize_size)(void);
} gCore;

// ---- what the script and the options say --------------------------------------------------------

typedef struct Action
{
	long ms;
	char what[16];
	char arg[160];
} Action_t;

static Action_t* gActions;
static int gActionCount, gActionCap, gActionNext;

static const char* gSystem;
static const char* gSave;
static char gOptions[64][256]; // "KEY=VALUE", as the player has them
static int gOptionCount;
static int gOptionsChanged; // .. and one changed since the core asked
static unsigned gOptionsVersion = 2;
static int gDisplayW, gDisplayH;
static int gList;

// what the core told of its options: each key with its choices, "KEY\0first\0second\0\0"
#define TOLD_MAX 128
static char* gTold[TOLD_MAX];
static char gToldName[TOLD_MAX][128];
static int gToldCount, gToldTimes, gToldCategories, gToldBad;
static char gControls[32][96]; // .. and of its controls: "device index id: what"
static int gControlCount;
static long gFrame; // the call of retro_run under way
static const char* gShots;
static long gOffset;

// ---- the frontend's state -------------------------------------------------------------------------

static uint32_t* gPicture; // the last picture, 32 bits a pixel, gW x gH
static int gW, gH;
static unsigned gMaxW, gMaxH;  // the largest the core said its picture would be
static long gPictures, gDupes; // pictures handed over, calls without a new one
static long gSoundFrames;
static uint32_t gSoundSum = 2166136261u; // FNV-1a of the running second
static long gSoundInSecond;
static FILE* gSoundFile;
static int gShutdown; // the core asked to be shut down
static struct retro_keyboard_callback gKeys;
static unsigned gPadDown;        // bits by RETRO_DEVICE_ID_JOYPAD_*
static int gPointerX, gPointerY; // the pointer device's reading
static int gRelX, gRelY;         // the mouse's moves not yet read
static int gMouseLeft, gMouseRight, gWheel, gTouchDown;
static struct
{
	long frame;
	char what[16];
	int arg;
} gLater[32]; // what a key, a click or a pad's press lets go of later
static int gLaterCount;

static void Die(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "retrofront: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(2);
}

static void RETRO_CALLCONV LogLine(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	(void)level;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

// ---- what the core told of its options ------------------------------------------------------------

static int gToldBefore;  // how many it told the time before (-1: this is the first)
static size_t gToldRoom; // of the entry being written
static char* ToldChoice(char* t, const char* text);

static void ToldBegin(void)
{
	int i;
	gToldBefore = gToldTimes ? gToldCount : -1;
	for(i = 0; i < gToldCount; i++)
	{
		free(gTold[i]);
		gTold[i] = NULL;
	}
	gToldCount = 0;
	gToldTimes++;
}

// a new option; where its choices go on
static char* ToldAdd(const char* key, const char* name)
{
	char* t;
	if(gToldCount >= TOLD_MAX)
		Die("more than %d options", TOLD_MAX);
	gToldRoom = 0x8000;
	t = gTold[gToldCount] = (char*)calloc(1, gToldRoom);
	if(!t)
		Die("out of memory");
	snprintf(gToldName[gToldCount], sizeof gToldName[0], "%s", name ? name : "");
	gToldCount++;
	return ToldChoice(t, key);
}

static char* ToldChoice(char* t, const char* text)
{
	size_t len = strlen(text) + 1;
	if(!t || len + 1 > gToldRoom)
		return NULL;
	memcpy(t, text, len);
	gToldRoom -= len;
	t[len] = 0;
	return t + len;
}

static void ToldEnd(void)
{
	if(gToldBefore >= 0 && gToldBefore != gToldCount)
	{ // (the interface asks for the same options each time, with other choices at most)
		fprintf(stderr, "retrofront: the core told %d options, then %d\n", gToldBefore, gToldCount);
		gToldBad = 1;
	}
}

// the choices of an option the core told: the text after its key; NULL for none
static const char* Choices(const char* key)
{
	int i;
	for(i = 0; i < gToldCount; i++)
		if(strcmp(gTold[i], key) == 0)
			return gTold[i] + strlen(key) + 1;
	return NULL;
}

static const char* FirstChoice(const char* key)
{
	const char* c = Choices(key);
	return c && *c ? c : NULL;
}

// whether a value is one of the option's choices (any value for an option the core did not tell)
static int IsChoice(const char* key, const char* value)
{
	const char* c = Choices(key);
	if(!c)
		return 1;
	for(; *c; c += strlen(c) + 1)
		if(strcmp(c, value) == 0)
			return 1;
	return 0;
}

static void ListTold(void)
{
	int i;
	printf("retrofront: %d options in %d categories, told %d times\n", gToldCount, gToldCategories, gToldTimes);
	for(i = 0; i < gToldCount; i++)
	{
		const char* c = gTold[i] + strlen(gTold[i]) + 1;
		printf("option %s (%s):", gTold[i], gToldName[i]);
		for(; *c; c += strlen(c) + 1)
			printf(" [%s]", c);
		printf("\n");
	}
	for(i = 0; i < gControlCount; i++)
		printf("control %s\n", gControls[i]);
}

// the player's value of an option
static void SetOption(const char* keyValue)
{
	const char* eq = strchr(keyValue, '=');
	int i;
	if(!eq)
		Die("%s: KEY=VALUE is needed", keyValue);
	for(i = 0; i < gOptionCount; i++)
		if(strncmp(gOptions[i], keyValue, (size_t)(eq - keyValue) + 1) == 0)
			break;
	if(i == gOptionCount && gOptionCount++ >= (int)(sizeof gOptions / sizeof gOptions[0]))
		Die("too many options");
	snprintf(gOptions[i], sizeof gOptions[0], "%s", keyValue);
	gOptionsChanged = 1;
}

static bool RETRO_CALLCONV Environment(unsigned cmd, void* data)
{
	switch(cmd)
	{
		case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
			((struct retro_log_callback*)data)->log = LogLine;
			return true;
		case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
			*(const char**)data = gSystem;
			return gSystem != NULL;
		case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
			*(const char**)data = gSave;
			return gSave != NULL;
		case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
			return *(const enum retro_pixel_format*)data == RETRO_PIXEL_FORMAT_XRGB8888;
		case RETRO_ENVIRONMENT_GET_VARIABLE:
		{
			struct retro_variable* v = (struct retro_variable*)data;
			size_t len = strlen(v->key);
			int i;
			v->value = NULL;
			for(i = 0; i < gOptionCount; i++)
				if(strncmp(gOptions[i], v->key, len) == 0 && gOptions[i][len] == '=')
					v->value = gOptions[i] + len + 1;
			if(v->value && !IsChoice(v->key, v->value))
			{ // as a frontend does with a value its file has from another time
				fprintf(stderr, "retrofront: %s is no choice of %s\n", v->value, v->key);
				v->value = NULL;
			}
			if(!v->value)
				v->value = FirstChoice(v->key);
			return v->value != NULL;
		}
		case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
			*(bool*)data = gOptionsChanged != 0;
			gOptionsChanged = 0;
			return true;
		case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
			*(unsigned*)data = gOptionsVersion;
			return true;
		case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
		{
			const struct retro_core_options_v2* o = (const struct retro_core_options_v2*)data;
			const struct retro_core_option_v2_definition* d;
			const struct retro_core_option_v2_category* c;
			int n = 0;
			if(gOptionsVersion < 2)
				return false;
			ToldBegin();
			for(c = o->categories; c && c->key; c++)
				n++;
			gToldCategories = n;
			for(d = o->definitions; d && d->key; d++)
			{
				int k;
				char* t = ToldAdd(d->key, d->desc_categorized ? d->desc_categorized : d->desc);
				for(k = 0; d->values[k].value; k++)
					t = ToldChoice(t, d->values[k].value);
				for(c = o->categories; d->category_key && c && c->key && strcmp(c->key, d->category_key) != 0; c++)
				{
				}
				if(!d->default_value || !IsChoice(d->key, d->default_value) || (d->category_key && !(c && c->key)))
					gToldBad = 1; // a default that is no choice, a category that is none
			}
			ToldEnd();
			return true;
		}
		case RETRO_ENVIRONMENT_SET_VARIABLES:
		{
			const struct retro_variable* v = (const struct retro_variable*)data;
			ToldBegin();
			gToldCategories = 0;
			for(; v && v->key; v++)
			{ // "Name; first|second|.."
				const char* semi = strstr(v->value ? v->value : "", "; ");
				char name[128], *t;
				snprintf(name, sizeof name, "%.*s", semi ? (int)(semi - v->value) : 0, v->value ? v->value : "");
				t = ToldAdd(v->key, name);
				while(semi && t)
				{
					const char* from = semi + (*semi == ';' ? 2 : 1);
					char one[256];
					semi = strchr(from, '|');
					snprintf(one, sizeof one, "%.*s", semi ? (int)(semi - from) : (int)strlen(from), from);
					t = ToldChoice(t, one);
				}
			}
			ToldEnd();
			return true;
		}
		case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
		{
			const struct retro_input_descriptor* d = (const struct retro_input_descriptor*)data;
			for(gControlCount = 0; d && d->description && gControlCount < 32; d++)
				snprintf(gControls[gControlCount++], sizeof gControls[0], "%u %u %u: %s", d->device, d->index, d->id, d->description);
			return true;
		}
		case RETRO_ENVIRONMENT_GET_DISPLAY_METADATA:
		{
			struct retro_display_metadata* m = (struct retro_display_metadata*)data;
			if(!gDisplayW || m->version != 1 || m->size < sizeof *m)
				return false;
			m->geo.native_width = (unsigned)gDisplayW;
			m->geo.native_height = (unsigned)gDisplayH;
			return true;
		}
		case RETRO_ENVIRONMENT_SET_GEOMETRY:
		case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		{
			const struct retro_game_geometry* g = cmd == RETRO_ENVIRONMENT_SET_GEOMETRY ? (const struct retro_game_geometry*)data : &((const struct retro_system_av_info*)data)->geometry;
			if(cmd == RETRO_ENVIRONMENT_SET_GEOMETRY && (g->base_width > gMaxW || g->base_height > gMaxH))
				Die("the core announced %u x %u, larger than the %u x %u it said it would be at most", g->base_width, g->base_height, gMaxW, gMaxH);
			if(cmd == RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO)
			{
				gMaxW = g->max_width;
				gMaxH = g->max_height;
			}
			printf("retrofront: the core announced %u x %u at call %ld\n", g->base_width, g->base_height, gFrame);
			return true;
		}
		case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
			gKeys = *(const struct retro_keyboard_callback*)data;
			return true;
		case RETRO_ENVIRONMENT_SET_MESSAGE:
			fprintf(stderr, "retrofront: the core says: %s\n", ((const struct retro_message*)data)->msg);
			return true;
		case RETRO_ENVIRONMENT_GET_CAN_DUPE:
			*(bool*)data = true;
			return true;
		case RETRO_ENVIRONMENT_SHUTDOWN:
			gShutdown = 1;
			return true;
		default: return false;
	}
}

static void RETRO_CALLCONV Video(const void* data, unsigned width, unsigned height, size_t pitch)
{
	unsigned y;
	if(!data)
	{
		gDupes++;
		return;
	}
	if((int)width != gW || (int)height != gH || !gPicture)
	{
		free(gPicture);
		gPicture = (uint32_t*)malloc((size_t)width * height * 4);
		gW = (int)width;
		gH = (int)height;
	}
	if(!gPicture)
		Die("out of memory");
	for(y = 0; y < height; y++)
		memcpy(gPicture + (size_t)y * width, (const uint8_t*)data + (size_t)y * pitch, (size_t)width * 4);
	gPictures++;
}

static size_t RETRO_CALLCONV AudioBatch(const int16_t* data, size_t frames)
{
	size_t i;
	for(i = 0; i < frames * 2; i++)
	{
		gSoundSum = (gSoundSum ^ (uint8_t)data[i]) * 16777619u;
		gSoundSum = (gSoundSum ^ (uint8_t)(data[i] >> 8)) * 16777619u;
		if((i & 1) && ++gSoundInSecond == 44100)
		{
			if(gSoundFile)
				fprintf(gSoundFile, "%ld %08lx\n", gSoundFrames / 44100, (unsigned long)gSoundSum);
			gSoundSum = 2166136261u;
			gSoundInSecond = 0;
		}
		if(i & 1)
			gSoundFrames++;
	}
	return frames;
}

static void RETRO_CALLCONV AudioSample(int16_t left, int16_t right)
{
	int16_t pair[2];
	pair[0] = left;
	pair[1] = right;
	AudioBatch(pair, 1);
}

static void RETRO_CALLCONV InputPoll(void)
{
}

static int16_t RETRO_CALLCONV InputState(unsigned port, unsigned device, unsigned index, unsigned id)
{
	int16_t v = 0;
	if(port != 0)
		return 0;
	if(device == RETRO_DEVICE_JOYPAD)
		return (int16_t)((gPadDown >> id) & 1);
	if(device == RETRO_DEVICE_POINTER)
	{
		if(id == RETRO_DEVICE_ID_POINTER_X)
			return (int16_t)gPointerX;
		if(id == RETRO_DEVICE_ID_POINTER_Y)
			return (int16_t)gPointerY;
		if(id == RETRO_DEVICE_ID_POINTER_PRESSED)
			return (int16_t)(index == 0 && (gTouchDown || gMouseLeft));
		if(id == RETRO_DEVICE_ID_POINTER_COUNT)
			return (int16_t)(gTouchDown ? 1 : 0);
		return 0;
	}
	if(device != RETRO_DEVICE_MOUSE)
		return 0;
	switch(id)
	{
		case RETRO_DEVICE_ID_MOUSE_X:
			v = (int16_t)gRelX;
			gRelX = 0;
			break;
		case RETRO_DEVICE_ID_MOUSE_Y:
			v = (int16_t)gRelY;
			gRelY = 0;
			break;
		case RETRO_DEVICE_ID_MOUSE_LEFT: v = (int16_t)gMouseLeft; break;
		case RETRO_DEVICE_ID_MOUSE_RIGHT: v = (int16_t)gMouseRight; break;
		case RETRO_DEVICE_ID_MOUSE_WHEELUP:
			v = gWheel > 0;
			gWheel -= v;
			break;
		case RETRO_DEVICE_ID_MOUSE_WHEELDOWN:
			v = gWheel < 0;
			gWheel += v;
			break;
		default: break;
	}
	return v;
}

// ---- the script -----------------------------------------------------------------------------------

static void AddAction(long ms, const char* what, const char* arg)
{
	Action_t* a;
	if(gActionCount == gActionCap)
	{
		gActionCap = gActionCap ? gActionCap * 2 : 64;
		gActions = (Action_t*)realloc(gActions, (size_t)gActionCap * sizeof *gActions);
		if(!gActions)
			Die("out of memory");
	}
	a = &gActions[gActionCount++];
	a->ms = ms;
	snprintf(a->what, sizeof a->what, "%s", what);
	snprintf(a->arg, sizeof a->arg, "%s", arg);
}

static int CompareActions(const void* a, const void* b)
{
	const Action_t* x = (const Action_t*)a;
	const Action_t* y = (const Action_t*)b;
	if(x->ms != y->ms)
		return x->ms < y->ms ? -1 : 1;
	return x < y ? -1 : x > y; // (as written, for equal times)
}

static void ReadScript(const char* path)
{
	FILE* f = fopen(path, "r");
	char line[512];
	if(!f)
		Die("%s cannot be read", path);
	while(fgets(line, sizeof line, f))
	{
		long from, to, step = 0;
		char what[16], arg[160];
		char* hash = strchr(line, '#');
		char* p = line;
		int n = 0;
		if(hash)
			*hash = 0;
		line[strcspn(line, "\r\n")] = 0;
		from = strtol(p, &p, 10);
		to = from;
		if(*p == '-')
		{
			to = strtol(p + 1, &p, 10);
			if(*p == '/')
				step = strtol(p + 1, &p, 10);
		}
		arg[0] = 0;
		if(sscanf(p, " %15s %n", what, &n) < 1)
			continue;
		snprintf(arg, sizeof arg, "%s", p + n);
		for(; from <= to; from += step > 0 ? step : to - from + 1)
			AddAction(from, what, arg);
	}
	fclose(f);
	qsort(gActions, (size_t)gActionCount, sizeof *gActions, CompareActions);
}

// a key's name as a RETROK_*; 0 for none
static unsigned KeyCode(const char* name)
{
	static const struct
	{
		const char* name;
		unsigned key;
	} kKeys[] = {
		{"RETURN", RETROK_RETURN},
		{"SPACE", RETROK_SPACE},
		{"ESCAPE", RETROK_ESCAPE},
		{"BACK", RETROK_BACKSPACE},
		{"TAB", RETROK_TAB},
		{"SHIFT", RETROK_LSHIFT},
		{"CONTROL", RETROK_LCTRL},
		{"UP", RETROK_UP},
		{"DOWN", RETROK_DOWN},
		{"LEFT", RETROK_LEFT},
		{"RIGHT", RETROK_RIGHT},
	};
	size_t i;
	for(i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++)
		if(strcmp(name, kKeys[i].name) == 0)
			return kKeys[i].key;
	if(name[0] == 'F' && name[1] >= '1' && name[1] <= '9')
		return RETROK_F1 + (unsigned)atoi(name + 1) - 1;
	if(name[0] && !name[1])
	{
		if(name[0] >= 'A' && name[0] <= 'Z')
			return RETROK_a + (unsigned)(name[0] - 'A');
		if((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= '0' && name[0] <= '9'))
			return (unsigned)name[0];
	}
	return 0;
}

static unsigned PadBit(const char* name)
{
	static const struct
	{
		const char* name;
		unsigned id;
	} kPad[] = {
		{"south", RETRO_DEVICE_ID_JOYPAD_B},
		{"east", RETRO_DEVICE_ID_JOYPAD_A},
		{"west", RETRO_DEVICE_ID_JOYPAD_Y},
		{"north", RETRO_DEVICE_ID_JOYPAD_X},
		{"l1", RETRO_DEVICE_ID_JOYPAD_L},
		{"r1", RETRO_DEVICE_ID_JOYPAD_R},
		{"l2", RETRO_DEVICE_ID_JOYPAD_L2},
		{"r2", RETRO_DEVICE_ID_JOYPAD_R2},
		{"select", RETRO_DEVICE_ID_JOYPAD_SELECT},
		{"start", RETRO_DEVICE_ID_JOYPAD_START},
		{"up", RETRO_DEVICE_ID_JOYPAD_UP},
		{"down", RETRO_DEVICE_ID_JOYPAD_DOWN},
		{"left", RETRO_DEVICE_ID_JOYPAD_LEFT},
		{"right", RETRO_DEVICE_ID_JOYPAD_RIGHT},
	};
	size_t i;
	for(i = 0; i < sizeof kPad / sizeof kPad[0]; i++)
		if(strcmp(name, kPad[i].name) == 0)
			return 1u << kPad[i].id;
	return 0;
}

static void Later(long frame, const char* what, int arg)
{
	if(gLaterCount < (int)(sizeof gLater / sizeof gLater[0]))
	{
		gLater[gLaterCount].frame = frame;
		snprintf(gLater[gLaterCount].what, sizeof gLater[0].what, "%s", what);
		gLater[gLaterCount].arg = arg;
		gLaterCount++;
	}
}

static void KeyEvent(unsigned key, int down, uint32_t ch)
{
	if(gKeys.callback && key)
		gKeys.callback(down != 0, key, ch, 0);
}

// a place in the picture as the pointer device reads it
static void PointAt(int x, int y)
{
	if(gW > 0 && gH > 0)
	{
		gPointerX = (int)(((long)x * 2 + 1) * 0x7fff / gW) - 0x7fff;
		gPointerY = (int)(((long)y * 2 + 1) * 0x7fff / gH) - 0x7fff;
	}
}

static void WritePicture(long ms)
{
	char path[1024];
	FILE* f;
	int i;
	if(!gShots || !gPicture)
		return;
	snprintf(path, sizeof path, "%s%09ld.ppm", gShots, ms);
	f = fopen(path, "wb");
	if(!f)
		Die("%s cannot be written", path);
	fprintf(f, "P6\n%d %d\n255\n", gW, gH);
	for(i = 0; i < gW * gH; i++)
	{
		uint8_t rgb[3];
		rgb[0] = (uint8_t)(gPicture[i] >> 16);
		rgb[1] = (uint8_t)(gPicture[i] >> 8);
		rgb[2] = (uint8_t)gPicture[i];
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

// what is due at the call `frame`; 0 when the script ends the run
static int Act(long frame)
{
	long ms = frame * 1000 / FPS + gOffset;
	int i, go = 1;
	for(i = 0; i < gLaterCount;)
	{
		if(gLater[i].frame > frame)
		{
			i++;
			continue;
		}
		if(strcmp(gLater[i].what, "key") == 0)
			KeyEvent((unsigned)gLater[i].arg, 0, 0);
		else if(strcmp(gLater[i].what, "left") == 0)
			gMouseLeft = 0;
		else if(strcmp(gLater[i].what, "right") == 0)
			gMouseRight = 0;
		else if(strcmp(gLater[i].what, "touch") == 0)
			gTouchDown = 0;
		else if(strcmp(gLater[i].what, "pad") == 0)
			gPadDown &= ~(unsigned)gLater[i].arg;
		gLater[i] = gLater[--gLaterCount];
	}
	for(; gActionNext < gActionCount && gActions[gActionNext].ms <= ms; gActionNext++)
	{
		const Action_t* a = &gActions[gActionNext];
		int x = 0, y = 0;
		sscanf(a->arg, "%d %d", &x, &y);
		if(strcmp(a->what, "key") == 0 || strcmp(a->what, "down") == 0 || strcmp(a->what, "up") == 0)
		{
			unsigned k = KeyCode(a->arg);
			if(a->what[0] != 'u')
				KeyEvent(k, 1, 0);
			if(a->what[0] == 'u')
				KeyEvent(k, 0, 0);
			else if(a->what[0] == 'k')
				Later(frame + 6, "key", (int)k);
		}
		else if(strcmp(a->what, "type") == 0)
		{
			const uint8_t* u = (const uint8_t*)a->arg;
			while(*u)
			{
				uint32_t c = *u++;
				int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2
					: c >= 0xc0                      ? 1
													 : 0;
				c &= more == 3 ? 0x07 : more == 2 ? 0x0f
					: more == 1                   ? 0x1f
												  : 0x7f;
				for(; more > 0 && *u; more--)
					c = (c << 6) | (*u++ & 0x3f);
				if(gKeys.callback)
				{
					gKeys.callback(true, RETROK_UNKNOWN, c, 0);
					gKeys.callback(false, RETROK_UNKNOWN, 0, 0);
				}
			}
		}
		else if(strcmp(a->what, "move") == 0)
			PointAt(x, y);
		else if(strcmp(a->what, "rel") == 0)
			gRelX += x, gRelY += y;
		else if(strcmp(a->what, "click") == 0 || strcmp(a->what, "rclick") == 0)
		{
			PointAt(x, y);
			if(a->what[0] == 'c')
				gMouseLeft = 1;
			else
				gMouseRight = 1;
			Later(frame + 6, a->what[0] == 'c' ? "left" : "right", 0);
		}
		else if(strcmp(a->what, "wheel") == 0)
			gWheel += x;
		else if(strcmp(a->what, "tap") == 0)
		{
			PointAt(x, y);
			gTouchDown = 1;
			Later(frame + 5, "touch", 0);
		}
		else if(strcmp(a->what, "pad") == 0 || strcmp(a->what, "paddown") == 0 || strcmp(a->what, "padup") == 0)
		{
			unsigned bit = PadBit(a->arg);
			if(strcmp(a->what, "padup") == 0)
				gPadDown &= ~bit;
			else
				gPadDown |= bit;
			if(strcmp(a->what, "pad") == 0)
				Later(frame + 6, "pad", (int)bit);
		}
		else if(strcmp(a->what, "reset") == 0)
			gCore.reset();
		else if(strcmp(a->what, "option") == 0)
			SetOption(a->arg);
		else if(strcmp(a->what, "shot") == 0)
			WritePicture(a->ms);
		else if(strcmp(a->what, "end") == 0)
			go = 0;
	}
	return go;
}

// ---- the run --------------------------------------------------------------------------------------

// a function of the core into `slot`, the storage of a pointer to it (what the loader gives is copied as it is)
static void Bind(Lib_t lib, const char* name, void* slot)
{
	Sym_t p = LibSym(lib, name);
	if(!p)
		Die("the core has no %s", name);
	memcpy(slot, &p, sizeof p);
}

#define BIND(lib, field) Bind(lib, "retro_" #field, &gCore.field)

static Lib_t OpenCore(const char* path)
{
	Lib_t lib = LibOpen(path);
	if(!lib)
		Die("%s cannot be loaded", path);
	BIND(lib, set_environment);
	BIND(lib, set_video_refresh);
	BIND(lib, set_audio_sample);
	BIND(lib, set_audio_sample_batch);
	BIND(lib, set_input_poll);
	BIND(lib, set_input_state);
	BIND(lib, init);
	BIND(lib, deinit);
	BIND(lib, api_version);
	BIND(lib, get_system_info);
	BIND(lib, get_system_av_info);
	BIND(lib, load_game);
	BIND(lib, unload_game);
	BIND(lib, run);
	BIND(lib, reset);
	BIND(lib, serialize_size);
	if(gCore.api_version() != RETRO_API_VERSION)
		Die("the core speaks version %u of the interface, not %u", gCore.api_version(), (unsigned)RETRO_API_VERSION);
	gCore.set_environment(Environment);
	gCore.set_video_refresh(Video);
	gCore.set_audio_sample(AudioSample);
	gCore.set_audio_sample_batch(AudioBatch);
	gCore.set_input_poll(InputPoll);
	gCore.set_input_state(InputState);
	gCore.init();
	return lib;
}

int main(int argc, char** argv)
{
	long runMs = 10000, everyMs = 0, loads = 1, load;
	int reopen = 0, i = 1, bad = 0;
	const char* corePath;
	const char* content;
	const char* script = NULL;
	Lib_t lib = 0;
	for(; i < argc && strncmp(argv[i], "--", 2) == 0; i++)
	{
		const char* a = argv[i];
		if(strncmp(a, "--run=", 6) == 0)
			runMs = atol(a + 6);
		else if(strncmp(a, "--every=", 8) == 0)
			everyMs = atol(a + 8);
		else if(strncmp(a, "--shots=", 8) == 0)
			gShots = a + 8;
		else if(strncmp(a, "--offset=", 9) == 0)
			gOffset = atol(a + 9);
		else if(strncmp(a, "--input=", 8) == 0)
			script = a + 8;
		else if(strncmp(a, "--loads=", 8) == 0)
			loads = atol(a + 8);
		else if(strcmp(a, "--reopen") == 0)
			reopen = 1;
		else if(strncmp(a, "--system=", 9) == 0)
			gSystem = a + 9;
		else if(strncmp(a, "--save=", 7) == 0)
			gSave = a + 7;
		else if(strncmp(a, "--option=", 9) == 0)
			SetOption(a + 9);
		else if(strncmp(a, "--options=", 10) == 0)
			gOptionsVersion = (unsigned)atol(a + 10);
		else if(strncmp(a, "--display=", 10) == 0 && sscanf(a + 10, "%dx%d", &gDisplayW, &gDisplayH) == 2)
			;
		else if(strcmp(a, "--list") == 0)
			gList = 1;
		else
			Die("%s: no such option", a);
	}
	if(argc - i != 2)
	{
		fprintf(stderr, "usage: retrofront [--run=MS] [--every=MS] [--shots=PREFIX] [--offset=MS] [--input=FILE] [--loads=N] [--reopen]\n"
						"                  [--system=DIR] [--save=DIR] [--option=KEY=VALUE] [--options=N] [--display=WxH] [--list]\n"
						"                  CORE CONTENT\n");
		return 2;
	}
	corePath = argv[i];
	content = argv[i + 1];
	if(script)
		ReadScript(script);
	for(load = 0; load < loads; load++)
	{
		struct retro_system_info info;
		struct retro_system_av_info av;
		struct retro_game_info game;
		long frame, frames = runMs * FPS / 1000, nextShot = everyMs, soundBefore;
		if(!lib)
			lib = OpenCore(corePath);
		memset(&info, 0, sizeof info);
		gCore.get_system_info(&info);
		memset(&game, 0, sizeof game);
		game.path = content;
		gShutdown = 0;
		gPictures = gDupes = gSoundFrames = gSoundInSecond = 0;
		gSoundSum = 2166136261u;
		gActionNext = gLaterCount = 0;
		gPadDown = 0;
		gMouseLeft = gMouseRight = gWheel = gTouchDown = gRelX = gRelY = 0;
		gPointerX = gPointerY = 0;
		if(gShots && load == 0)
		{
			char path[1024];
			snprintf(path, sizeof path, "%ssound.txt", gShots);
			gSoundFile = fopen(path, "w");
		}
		if(!gCore.load_game(&game))
		{
			printf("retrofront: load %ld: %s %s did not load %s\n", load + 1, info.library_name, info.library_version, content);
			bad = 1;
			break;
		}
		memset(&av, 0, sizeof av);
		gCore.get_system_av_info(&av);
		gMaxW = av.geometry.max_width;
		gMaxH = av.geometry.max_height;
		gW = gH = 0;
		gOptionsChanged = 0; // (what the options were is what the load read)
		for(frame = 1; frame <= frames && !gShutdown; frame++)
		{
			long ms = frame * 1000 / FPS + gOffset;
			gFrame = frame;
			if(!Act(frame))
				break;
			soundBefore = gSoundFrames;
			gCore.run();
			if(gSoundFrames - soundBefore != 44100 / FPS)
				bad = 1; // a call brings a picture's worth of sound, no more and no less
			for(; everyMs > 0 && nextShot <= ms; nextShot += everyMs)
				if(load == 0)
					WritePicture(nextShot);
		}
		if(gSoundFile)
		{
			if(gSoundInSecond)
				fprintf(gSoundFile, "%ld %08lx %ld\n", gSoundFrames / 44100, (unsigned long)gSoundSum, gSoundInSecond);
			fclose(gSoundFile);
			gSoundFile = NULL;
		}
		if(gList && load == 0)
			ListTold();
		printf("retrofront: load %ld: %s %s, %ld calls, %ld pictures of %d x %d (announced %u x %u at %.0f Hz; %ld calls without a new one), "
			   "%ld frames of sound at %.0f Hz, %s\n",
			load + 1, info.library_name, info.library_version, frame - 1, gPictures, gW, gH, av.geometry.base_width, av.geometry.base_height,
			av.timing.fps, gDupes, gSoundFrames, av.timing.sample_rate, gShutdown ? "the core asked to be shut down" : "stopped");
		if(gToldBad)
			bad = 1;
		gCore.unload_game();
		if(reopen || load + 1 == loads)
		{
			gCore.deinit();
			LibClose(lib);
			lib = 0;
		}
	}
	while(gToldCount > 0)
		free(gTold[--gToldCount]);
	free(gPicture);
	free(gActions);
	return bad;
}
