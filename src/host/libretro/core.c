/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * core.c - OpenBGI as a libretro core: the engine behind the interface of
 *          libretro.h, for RetroArch and the other frontends that load
 *          such cores (docs/dev/libretro.md); not part of the original engine
 *
 * The content is a game: a zip archive or a disc image (".zip", ".iso"),
 * or any file of an unpacked game - its program, its BGI.gdb, one of its
 * archives - which stands for the folder it is in.  Nothing is written
 * into any of them: a game's own files go to
 * <save directory>/OpenBGI/games/<name>/, the engine's settings
 * (openbgi.json) to <save directory>/OpenBGI/.  The player's own fonts
 * are taken from <system directory>/openbgi/fonts/, and the zip archives
 * of a folder "patches" beside the game are offered as patches to lay
 * over it.
 *
 * One call of retro_run is one picture of a display of 60 Hz on a clock
 * of the core's own: the clock is moved on by a sixtieth of a second and
 * the engine runs its frames until it has caught up, so a frontend that
 * runs faster or slower, or stops, takes the game along and two runs
 * with the same input give the same pictures.  The sound is 44100 Hz, 735
 * frames a call.
 *
 * The picture: a frontend does not say how large its window is, and
 * scales what a core hands it as it sees fit.  So the core hands over a
 * canvas of the display's size - as the frontend tells it, else 1920 x
 * 1080, or what the options say - with the game's picture in its middle
 * on black, scaled by the largest power of two that fits, bicubic
 * (host/os_present.h); the options choose other steps and filters, or the
 * game's own picture as it is.  Only what changed is scaled again, and a
 * call that changed nothing hands the frontend no new picture.
 *
 * The input: the RetroPad of port 1 as a pad (the buttons by where they
 * sit: B, the bottom one, confirms, and so does Start, which is where a
 * frontend puts the Enter key), the keyboard with what it types, a
 * mouse - its place through the frontend's pointer device where that
 * follows the mouse, else by its moves - and a touch screen through the
 * same pointer device.  The engine draws its arrow for the pad; for a
 * mouse the frontend's own pointer is counted on unless the options say
 * otherwise.  The message boxes, the scripts' dialogs and the text entry
 * are drawn in the picture.
 *
 * The options (retro_core_options_v2, with the plain variables for a
 * frontend that has only those): the picture's, the pointer's and the
 * pad's hold at once - the pad's and the pointer's go to the engine as
 * input settings (OpenBGI_InputSettings) - the others are part of the
 * engine's start and hold from the next one, the frontend's restart
 * included, which makes a new engine.
 *
 * The files are the OS layer's (host/os.h), as for the other hosts.  One
 * game at a time: the core keeps its state in this file, as cores do.
 */
#include "libretro.h"

#include "openbgi.h"
#include "bgi/fontfile.h"
#include "bgi/release.h"
#include "host/os.h"
#include "host/os_common.h"
#include "host/os_present.h"
#ifdef BGI_POSIX
#include "host/os_posix.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FPS           60
#define AUDIO_FRAMES  (OPENBGI_AUDIO_RATE / FPS) // the sound of one call
#define AUDIO_LEAD    50                         // ms of sound the engine keeps ahead of the reads
#define PATH_MAX_     2048
#define DEFAULT_W     800 // the picture until the game has one
#define DEFAULT_H     600
#define CANVAS_W      1920 // the canvas when neither the frontend nor the options say one, and
#define CANVAS_H      1080 // what the frontend is told to expect at most (a larger one is announced when it comes)
#define MAX_TOUCHES   4
#define KEY_HOLD      (FPS * 5) // calls after a key event for which a key still down keeps the pad as it was
#define MAX_FONTS     256
#define PAD_BUTTONS   12  // the RetroPad's buttons the options bind
#define PATCH_OPTIONS 4   // the options "Patch 1" ..
#define PATCH_CHOICES 100 // archives of the patches folder they offer
#define PATCH_NAME    120 // bytes of an archive's name

#ifdef BGI_POSIX
/* What the POSIX back end's start and end call in its window and font
 * files (OS_Init's signals, OS_Shutdown); the core has neither file and
 * calls neither function. */
void OsPosix_RequestClose(void)
{
}

void OsPosix_FontShutdown(void)
{
}
#endif

// ---- the frontend ---------------------------------------------------------------------------

static retro_environment_t gEnv;
static retro_video_refresh_t gVideo;
static retro_audio_sample_batch_t gAudioBatch;
static retro_input_poll_t gInputPoll;
static retro_input_state_t gInputState;
static retro_log_printf_t gLog;

static void Log(enum retro_log_level level, const char* fmt, ...)
{
	char text[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof text, fmt, ap);
	va_end(ap);
	if(gLog)
		gLog(level, "[OpenBGI] %s\n", text);
	else
		fprintf(stderr, "openbgi: %s\n", text);
}

// a line on the frontend's screen for a few seconds
static void Notice(const char* text)
{
	struct retro_message msg;
	msg.msg = text;
	msg.frames = FPS * 6;
	if(gEnv)
		gEnv(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
}

// ---- the state --------------------------------------------------------------------------------

static OpenBGI_t* gEngine;
static OpenBGI_Frame_t gFrame;                    // the last frame the engine ran
static int gRunning;                              // the game runs (0 before a game, after its end)
static int gShutdownSent;                         // the frontend was told that the game ended
static uint32_t gNow;                             // the clock, ms
static uint64_t gCalls;                           // calls of retro_run since the game started
static char gGamePath[PATH_MAX_];                 // the game as the engine is given it: an archive or a folder, native
static char gCmdLine[PATH_MAX_ * 4];              // .. and the command line of its start
static char gSystemDir[PATH_MAX_];                // <system>/openbgi/, native, with its separator ("" when the frontend has none)
static char gSaveDir[PATH_MAX_];                  // <save>/OpenBGI/, native, with its separator
static OsPresent_t* gPresent;                     // lays the game's picture on the canvas
static int gWidth = CANVAS_W, gHeight = CANVAS_H; // the canvas: the geometry the frontend knows
static int gMaxW = CANVAS_W, gMaxH = CANVAS_H;
static int gDisplayW, gDisplayH; // the display as the frontend tells it (0 x 0 when it does not)
static int gCanDupe;             // the frontend takes "the picture before again"

// the options as the frontend has them (ReadOptions)
enum
{
	POINTER_AUTO,
	POINTER_MOUSE,
	POINTER_TOUCH
};
static struct
{
	int steps, filter;    // OS_PRESENT_*
	int canvasW, canvasH; // the canvas asked for; 0 x 0 automatic, -1 x -1 the game's own picture
	int pointer;          // POINTER_*
	char input[1024];     // the input settings (OpenBGI_InputSettings)
	// what a start takes: the engine's options on the command line and the host's services
	char start[PATH_MAX_ * 3];
	int movies, playerFonts;
} gOpt;
static char gStarted[PATH_MAX_ * 3 + 16]; // what the running engine was started with
static char gToldFor[PATH_MAX_ * 3 + 16]; // .. and the change the player was told needs a restart

// ---- the clocks -------------------------------------------------------------------------------

static uint32_t TicksMs(void* user)
{
	(void)user;
	return gNow;
}

static int PerfNs(void* user, int64_t* out)
{
	(void)user;
	*out = (int64_t)gNow * 1000000;
	return 1;
}

static void LocalTime(void* user, OpenBGI_DateTime_t* out)
{
	time_t t = time(NULL);
	const struct tm* tm = localtime(&t);
	(void)user;
	memset(out, 0, sizeof *out);
	if(!tm)
		return;
	out->year = (uint16_t)(tm->tm_year + 1900);
	out->month = (uint16_t)(tm->tm_mon + 1);
	out->dayOfWeek = (uint16_t)tm->tm_wday;
	out->day = (uint16_t)tm->tm_mday;
	out->hour = (uint16_t)tm->tm_hour;
	out->minute = (uint16_t)tm->tm_min;
	out->second = (uint16_t)tm->tm_sec;
}

// ---- the files --------------------------------------------------------------------------------

static OpenBGI_Files_t gFiles; // the OS layer's, with the two folders below in place of its own

static void UserDataDir(void* user, char* buf, size_t n)
{
	(void)user;
	snprintf(buf, n, "%s", gSaveDir);
}

// where openbgi.json is looked for beside the program: the core's folder in the system directory
static void BinaryDir(void* user, char* buf, size_t n)
{
	(void)user;
	snprintf(buf, n, "%s", gSystemDir[0] ? gSystemDir : gSaveDir);
}

static int SameName(const char* a, const char* b);

/* `dir` (UTF-8, as the frontend gives it) with `sub` under it, native,
 * made when missing, with a closing separator.  A frontend that sorts
 * its folders by core hands over one that ends in the core's name
 * already: that one is taken as it is. */
static void MakeDir(const char* dir, const char* sub, char* out, size_t n)
{
	char given[PATH_MAX_], native[PATH_MAX_];
	size_t len;
	out[0] = 0;
	if(!dir || !dir[0])
		return;
	OS_Utf8ToNative(dir, given, sizeof given);
	OS_NativeAbsolute(given, native, sizeof native); // (the game's folder must be a whole path: its drive is asked for)
	len = strlen(native);
	while(len > 1 && (native[len - 1] == '/' || native[len - 1] == OS_NATIVE_SEP))
		native[--len] = 0;
	if(SameName(OsCommon_FilePart(native), sub))
		sub = "";
	if((size_t)snprintf(out, n, "%s%s%s", native, sub[0] ? (const char[]){OS_NATIVE_SEP, 0} : "", sub) >= n)
	{
		out[0] = 0;
		return;
	}
	OS_NativeMkdir(native);
	OS_NativeMkdir(out);
	len = strlen(out);
	if(len + 1 < n)
	{
		out[len] = OS_NATIVE_SEP;
		out[len + 1] = 0;
	}
}

// ---- the player's fonts -------------------------------------------------------------------------

/* The fonts of <system>/openbgi/fonts/: each family of each face of each
 * file, found by its name as the games ask for it.  A family that is
 * there counts as installed, so it is opened before a built-in font. */
typedef struct PlayerFont
{
	char family[FONTFILE_NAME_MAX]; // UTF-8
	char path[1024];                // native
	int index;                      // the face in a collection
} PlayerFont_t;

static PlayerFont_t* gFonts;
static int gFontCount;

static int SameName(const char* a, const char* b)
{
	for(; *a && *b; a++, b++)
	{
		char x = *a, y = *b;
		if(x >= 'A' && x <= 'Z')
			x = (char)(x + 32);
		if(y >= 'A' && y <= 'Z')
			y = (char)(y + 32);
		if(x != y)
			return 0;
	}
	return *a == *b;
}

static void FontEntry(void* ctx, const char* name, int isDir)
{
	const char* dir = (const char*)ctx;
	const char* dot = strrchr(name, '.');
	char path[1024];
	uint8_t* data;
	uint32_t size = 0;
	int faces, face;
	if(isDir || !dot || !(SameName(dot, ".ttf") || SameName(dot, ".ttc") || SameName(dot, ".otf") || SameName(dot, ".otc")))
		return;
	if((size_t)snprintf(path, sizeof path, "%s%s", dir, name) >= sizeof path)
		return;
	data = OS_ReadNativeFile(path, &size);
	if(!data)
		return;
	faces = FontFile_Faces(data, size);
	for(face = 0; face < faces; face++)
	{
		char names[8][FONTFILE_NAME_MAX];
		int n = FontFile_Families(data, size, face, names, 8), i;
		for(i = 0; i < n && i < 8 && gFontCount < MAX_FONTS; i++)
		{
			PlayerFont_t* f = &gFonts[gFontCount++];
			snprintf(f->family, sizeof f->family, "%s", names[i]);
			snprintf(f->path, sizeof f->path, "%s", path);
			f->index = face;
		}
	}
	free(data);
}

static void LoadFonts(void)
{
	char dir[PATH_MAX_];
	gFontCount = 0;
	if(!gOpt.playerFonts || !gSystemDir[0])
		return;
	if(!gFonts)
		gFonts = (PlayerFont_t*)calloc(MAX_FONTS, sizeof *gFonts);
	if(!gFonts || (size_t)snprintf(dir, sizeof dir, "%sfonts%c", gSystemDir, OS_NATIVE_SEP) >= sizeof dir)
		return;
	OS_NativeMkdir(dir);
	OS_NativeList(dir, FontEntry, dir);
	if(gFontCount)
		Log(RETRO_LOG_INFO, "%d font famil%s of the player's in %s", gFontCount, gFontCount == 1 ? "y" : "ies", dir);
}

static const PlayerFont_t* FindFont(const char* family)
{
	int i;
	for(i = 0; i < gFontCount; i++)
		if(SameName(gFonts[i].family, family))
			return &gFonts[i];
	return NULL;
}

static int FontFind(void* user, const char* family, int serif, int bold, int italic, OpenBGI_FontFile_t* out)
{
	const PlayerFont_t* f = FindFont(family);
	(void)user, (void)serif, (void)bold, (void)italic;
	if(!f)
		return 0;
	memset(out, 0, sizeof *out);
	snprintf(out->path, sizeof out->path, "%s", f->path);
	out->index = f->index;
	return OPENBGI_FONT_FOUND;
}

static int FontFamily(void* user, const char* family)
{
	(void)user;
	return FindFont(family) ? OPENBGI_FONT_FOUND : 0;
}

static void FontFamilies(void* user, const char* lang, void (*each)(void* ctx, const char* family), void* ctx)
{
	int i;
	(void)user, (void)lang;
	for(i = 0; i < gFontCount; i++)
		each(ctx, gFonts[i].family);
}

static const OpenBGI_FontFiles_t kFonts = {
	.size = sizeof(OpenBGI_FontFiles_t), .find = FontFind, .family = FontFamily, .families = FontFamilies};

// ---- the options --------------------------------------------------------------------------------

typedef struct retro_core_option_value Choice_t;

// the choices of each option, its default first
static const Choice_t kScale[] = {{"powers of two", "Powers of two"}, {"whole numbers", "Whole numbers"},
	{"fill", "Fill the screen"}, {"none", "None"}, {NULL, NULL}};
static const Choice_t kFilter[] = {{"bicubic", "Bicubic"}, {"bilinear", "Bilinear"}, {"nearest", "Nearest"}, {NULL, NULL}};
/* (On Android the game's own picture is the default: the frontend scales
 * it with the graphics processor there, which the phone's processor is
 * better spared.) */
static const Choice_t kCanvas[] = {
#if defined(__ANDROID__) || defined(ANDROID)
	{"game", "The game's own"}, {"automatic", "Automatic"},
#else
	{"automatic", "Automatic"}, {"game", "The game's own"},
#endif
	{"1280x720", NULL}, {"1366x768", NULL}, {"1600x900", NULL}, {"1920x1080", NULL}, {"2560x1440", NULL}, {"3840x2160", NULL},
	{"1280x800", NULL}, {"1920x1200", NULL}, {"2560x1600", NULL}, {"2560x1080", NULL}, {"3440x1440", NULL},
	{"1024x768", NULL}, {"1280x1024", NULL}, {"1600x1200", NULL}, {NULL, NULL}};
static const Choice_t kDevice[] = {{"automatic", "Automatic"}, {"mouse", "Mouse"}, {"touch screen", "Touch screen"}, {NULL, NULL}};
static const Choice_t kOffOn[] = {{"off", "Off"}, {"on", "On"}, {NULL, NULL}};
static const Choice_t kOnOff[] = {{"on", "On"}, {"off", "Off"}, {NULL, NULL}};
static const Choice_t kSize[] = {{"normal", "Normal"}, {"small", "Small"}, {"large", "Large"}, {NULL, NULL}};
static const Choice_t kSpeed[] = {{"100%", NULL}, {"50%", NULL}, {"75%", NULL}, {"125%", NULL}, {"150%", NULL}, {"200%", NULL}, {"300%", NULL}, {NULL, NULL}};
static const Choice_t kConfirm[] = {{"bottom", "B confirms"}, {"right", "A confirms"}, {NULL, NULL}};
static const Choice_t kBind[] = { // what a button does: the texts are the engine's own (bgi/inputcfg.h)
	{"default", NULL},            // (its label says what that is)
	{"confirm", "Confirm"}, {"cancel", "Cancel"}, {"skip", "Skip (held)"}, {"backlog", "Backlog"}, {"forward", "Forward"}, {"keyboard", "Screen keyboard"},
	{"mouse:left", "Left mouse button"}, {"mouse:right", "Right mouse button"}, {"mouse:middle", "Middle mouse button"},
	{"wheel:up", "Wheel up"}, {"wheel:down", "Wheel down"},
	{"key:RETURN", "Enter key"}, {"key:SPACE", "Space key"}, {"key:ESCAPE", "Escape key"}, {"key:CONTROL", "Ctrl key"}, {"key:SHIFT", "Shift key"},
	{"key:TAB", "Tab key"}, {"key:BACK", "Backspace key"}, {"key:PRIOR", "Page Up key"}, {"key:NEXT", "Page Down key"}, {"key:HOME", "Home key"}, {"key:END", "End key"},
	{"key:F1", "F1 key"}, {"key:F2", "F2 key"}, {"key:F3", "F3 key"}, {"key:F4", "F4 key"}, {"key:F5", "F5 key"}, {"key:F6", "F6 key"},
	{"key:F7", "F7 key"}, {"key:F8", "F8 key"}, {"key:F9", "F9 key"}, {"key:F10", "F10 key"}, {"key:F11", "F11 key"}, {"key:F12", "F12 key"},
	{"none", "Nothing"}, {NULL, NULL}};
#define BIND_CHOICES (sizeof kBind / sizeof kBind[0])
static const Choice_t kOwnKeys[] = {{"automatic", "Automatic"}, {"on", "On"}, {"off", "Off"}, {NULL, NULL}};
static const Choice_t kLang[] = {{"English", NULL}, {"Japanese", "Japanese"}, {NULL, NULL}};
static const Choice_t kTap[] = {{"automatic", "Automatic"}, {"0 ms", NULL}, {"50 ms", NULL}, {"100 ms", NULL}, {"150 ms", NULL}, {"200 ms", NULL},
	{"300 ms", NULL}, {"500 ms", NULL}, {NULL, NULL}};
static const Choice_t kOsk[] = {{"automatic", "Automatic"}, {"when asked for", "Only when asked for"}, {NULL, NULL}};
static const Choice_t kDiscCheck[] = {{"the game's", "The game's own"}, {"skipped", "Skipped"}, {NULL, NULL}};
static const Choice_t kDiscMarker[] = {{"hidden", "Hidden"}, {"shown", "Shown"}, {NULL, NULL}};
static const Choice_t kMovies[] = {{"on", "On"}, {"skipped", "Skipped"}, {NULL, NULL}};
static const Choice_t kStandIn[] = {{"automatic", "By the font's name"}, {"gothic", "Gothic"}, {"mincho", "Mincho"}, {NULL, NULL}};
static Choice_t gEngineChoices[RETRO_NUM_CORE_OPTION_VALUES_MAX]; // "automatic" and the engine's profiles (BuildOptions)
static Choice_t gPatchChoices[PATCH_CHOICES + 2];                 // "none" and the archives of the patches folder

// the RetroPad's buttons the options bind: the option, its name, the engine's control, the button, what it does unless bound
static const struct
{
	const char* key;
	const char* name;
	const char* control;
	unsigned id;
	const char* usual;
} kPad[PAD_BUTTONS] = {
	{"openbgi_pad_b", "B (the bottom button)", "south", RETRO_DEVICE_ID_JOYPAD_B, "confirm"},
	{"openbgi_pad_a", "A (the right button)", "east", RETRO_DEVICE_ID_JOYPAD_A, "cancel"},
	{"openbgi_pad_y", "Y (the left button)", "west", RETRO_DEVICE_ID_JOYPAD_Y, "none"},
	{"openbgi_pad_x", "X (the top button)", "north", RETRO_DEVICE_ID_JOYPAD_X, "keyboard"},
	{"openbgi_pad_l", "L", "l1", RETRO_DEVICE_ID_JOYPAD_L, "backlog"},
	{"openbgi_pad_r", "R", "r1", RETRO_DEVICE_ID_JOYPAD_R, "forward"},
	{"openbgi_pad_l2", "L2", "l2", RETRO_DEVICE_ID_JOYPAD_L2, "none"},
	{"openbgi_pad_r2", "R2", "r2", RETRO_DEVICE_ID_JOYPAD_R2, "skip"},
	{"openbgi_pad_l3", "L3 (the left stick pressed)", "l3", RETRO_DEVICE_ID_JOYPAD_L3, "none"},
	{"openbgi_pad_r3", "R3 (the right stick pressed)", "r3", RETRO_DEVICE_ID_JOYPAD_R3, "none"},
	{"openbgi_pad_select", "Select", "select", RETRO_DEVICE_ID_JOYPAD_SELECT, "none"},
	{"openbgi_pad_start", "Start", "start", RETRO_DEVICE_ID_JOYPAD_START, "confirm"}, // (not the engine's cancel: InputJson)
};

static struct retro_core_option_v2_category gCategories[] = {
	{"picture", "Picture", "How the game's picture is laid on the screen. These hold at once."},
	{"pointer", "Pointer", "The mouse, the touch screen and the pointer drawn for a pad. These hold at once."},
	{"pad", "Pad", "What the pad's buttons do. These hold at once."},
	{"text", "Language and touch", "The language of OpenBGI's own messages, taps and the on-screen keyboard."},
	{"compat", "Compatibility", "For a game that does not run as it is. These hold from the game's next start (Restart)."},
	{"fonts", "Fonts", "Which fonts stand in for those a game asks for. These hold from the game's next start (Restart)."},
	{"patches", "Patches", "Zip archives of the folder \"patches\" beside the game, laid over its files. These hold from the game's next start (Restart)."},
	{NULL, NULL, NULL},
};

#define RESTART " Holds from the game's next start (Restart)."
typedef struct Option
{
	const char* key;
	const char* category;
	const char* name;
	const char* info;
	const Choice_t* choices;
} Option_t;
static const Option_t kOptions[] = {
	{"openbgi_scale", "picture", "Scaling",
		"The game's picture is shown in the middle of the screen, on black, at the largest size of these steps that fits: powers of two (1/2x, 1x, 2x, 4x), "
		"whole numbers (1x, 2x, 3x), whatever fills the screen, or as it is.",
		kScale},
	{"openbgi_filter", "picture", "Scaling filter", "How the picture is made larger or smaller: bicubic is smooth and sharp, bilinear soft, nearest has hard edges.",
		kFilter},
	{"openbgi_canvas", "picture", "Screen size",
		"The size of the picture the frontend is handed. Automatic asks the frontend for the display's and takes 1920x1080 when it does not say. "
		"With the game's own, the frontend gets the game's picture as it is and scales it itself.",
		kCanvas},
	{"openbgi_pointer", "pointer", "Pointer device", "What the frontend's pointer is: a mouse, or a touch screen with its taps and gestures.", kDevice},
	{"openbgi_mouse_arrow", "pointer", "Draw the pointer for a mouse",
		"Off counts on the frontend's own pointer. On draws OpenBGI's arrow where the mouse is, for a frontend that shows none (in full screen).", kOffOn},
	{"openbgi_pointer_size", "pointer", "Size of the drawn pointer", NULL, kSize},
	{"openbgi_stick_speed", "pointer", "Stick speed", "How fast the left stick moves the pointer.", kSpeed},
	{"openbgi_pad_confirm", "pad", "Confirm and cancel", "Which of the two buttons confirms; the other one cancels. A button bound below does what it is bound to.", kConfirm},
	{"openbgi_pad_b", "pad", NULL, NULL, kBind},
	{"openbgi_pad_a", "pad", NULL, NULL, kBind},
	{"openbgi_pad_y", "pad", NULL, NULL, kBind},
	{"openbgi_pad_x", "pad", NULL, NULL, kBind},
	{"openbgi_pad_l", "pad", NULL, NULL, kBind},
	{"openbgi_pad_r", "pad", NULL, NULL, kBind},
	{"openbgi_pad_l2", "pad", NULL, NULL, kBind},
	{"openbgi_pad_r2", "pad", NULL, NULL, kBind},
	{"openbgi_pad_l3", "pad", NULL, NULL, kBind},
	{"openbgi_pad_r3", "pad", NULL, NULL, kBind},
	{"openbgi_pad_select", "pad", NULL, NULL, kBind},
	{"openbgi_pad_start", "pad", NULL, NULL, kBind},
	{"openbgi_pad_keys", "pad", "The game's own pad keys",
		"Some games give the pad's buttons keys of their own. Off ignores those, and every button does what it does here. Automatic is on, unless openbgi.json says otherwise.",
		kOwnKeys},
	{"openbgi_lang", "text", "Messages of OpenBGI", "The language of the engine's own messages and boxes (not the game's); Japanese are the original's." RESTART, kLang},
	{"openbgi_tap_delay", "text", "Tap delay",
		"The time between the pointer's move and the press of a tap or of a pad's click, for a game whose buttons need the pointer to rest on them first.", kTap},
	{"openbgi_osk", "text", "On-screen keyboard", "When OpenBGI's keyboard comes up by itself: automatic is for a pad, or a touch, on a text field. The keyboard action brings it up in any case.", kOsk},
	{"openbgi_engine", "compat", "Engine profile", "The version of the engine the game is run as. Automatic finds it from the game." RESTART, gEngineChoices},
	{"openbgi_disc_check", "compat", "Disc and registration check",
		"Skipped: the game counts as registered, for one that asks for its disc or its registration program." RESTART, kDiscCheck},
	{"openbgi_disc_marker", "compat", "Disc marker file",
		"The file a disc carries to say that it is one. Games quit when they find it beside them, so it is hidden." RESTART, kDiscMarker},
	{"openbgi_movies", "compat", "Movies", "Skipped: movies are not played." RESTART, kMovies},
	{"openbgi_player_fonts", "fonts", "The player's fonts before the built-in ones",
		"On: a font a game asks for is taken from the frontend's system folder, openbgi/fonts, when it is there." RESTART, kOnOff},
	{"openbgi_stand_in_font", "fonts", "Design of the built-in fonts",
		"Which built-in font stands in for one that is not there: by its name (Gothic or Mincho), or always the same." RESTART, kStandIn},
	{"openbgi_patch_1", "patches", "Patch 1", "A zip archive whose files take the place of the game's (a translation, a fix)." RESTART, gPatchChoices},
	{"openbgi_patch_2", "patches", "Patch 2", "Laid over patch 1." RESTART, gPatchChoices},
	{"openbgi_patch_3", "patches", "Patch 3", "Laid over patch 2." RESTART, gPatchChoices},
	{"openbgi_patch_4", "patches", "Patch 4", "Laid over patch 3." RESTART, gPatchChoices},
};
#define OPTIONS (sizeof kOptions / sizeof kOptions[0])

static struct retro_core_option_v2_definition gDefs[OPTIONS + 1];
static char gDesc[OPTIONS][96];                  // "Category > Name", for a frontend without categories
static char gPadLabel[PAD_BUTTONS][64];          // "Default (Confirm)"
static struct retro_variable gVars[OPTIONS + 1]; // the same as plain variables,
static char* gVarText[OPTIONS];                  // .. "Name; first|second|.."

// the patches of the game: the folder (native, with its separator; "" for none) and its archives' names (UTF-8), sorted
static char gPatchDir[PATH_MAX_];
static char gPatchName[PATCH_CHOICES][PATCH_NAME];
static int gPatchCount;

static void PatchEntry(void* ctx, const char* name, int isDir)
{
	const char* dot = strrchr(name, '.');
	char utf[PATH_MAX_];
	(void)ctx;
	if(isDir || !dot || !SameName(dot, ".zip") || gPatchCount >= PATCH_CHOICES)
		return;
	OS_NativeToUtf8(name, utf, sizeof utf);
	if(strlen(utf) >= PATCH_NAME || strpbrk(utf, "\"|;")) // (what would end a name in an option or on the command line)
		return;
	snprintf(gPatchName[gPatchCount++], PATCH_NAME, "%s", utf);
}

static int ComparePatch(const void* a, const void* b)
{
	return strcmp((const char*)a, (const char*)b);
}

/* The patches offered for the game at gGamePath: the zip archives of the
 * folder "patches" in a game's folder, or beside a game's archive - there
 * of "patches/<the archive's name without its extension>" when that is a
 * folder, so that the archives of one folder need not share theirs. */
static void FindPatches(int isArchive)
{
	char dir[PATH_MAX_];
	const char* part = isArchive ? OsCommon_FilePart(gGamePath) : gGamePath + strlen(gGamePath);
	const char* dot = strrchr(part, '.');
	int in = (int)(part - gGamePath), found = 0; // (a folder's path ends in its separator; "." is where the frontend runs)
	size_t len;
	gPatchCount = 0;
	gPatchDir[0] = 0;
	if(!isArchive && strcmp(gGamePath, ".") == 0)
		in = 0;
	if(isArchive && dot > part)
	{ // the archive's own
		len = (size_t)snprintf(dir, sizeof dir, "%.*spatches%c%.*s", in, gGamePath, OS_NATIVE_SEP, (int)(dot - part), part);
		found = len < sizeof dir && OS_NativeStat(dir, NULL) == OS_NATIVE_DIR;
	}
	if(!found)
	{
		len = (size_t)snprintf(dir, sizeof dir, "%.*spatches", in, gGamePath);
		found = len < sizeof dir && OS_NativeStat(dir, NULL) == OS_NATIVE_DIR;
	}
	if(!found || (size_t)snprintf(gPatchDir, sizeof gPatchDir, "%s%c", dir, OS_NATIVE_SEP) >= sizeof gPatchDir)
	{
		gPatchDir[0] = 0;
		return;
	}
	OS_NativeList(gPatchDir, PatchEntry, NULL);
	qsort(gPatchName, (size_t)gPatchCount, sizeof gPatchName[0], ComparePatch);
	if(gPatchCount)
		Log(RETRO_LOG_INFO, "%d patch%s to choose from in %s", gPatchCount, gPatchCount == 1 ? "" : "es", gPatchDir);
}

static const char* CategoryName(const char* key)
{
	size_t i;
	for(i = 0; gCategories[i].key; i++)
		if(strcmp(gCategories[i].key, key) == 0)
			return gCategories[i].desc;
	return "";
}

// what a binding is called ("confirm", "key:F5", ..); "" for one that is none of the choices
static const char* BindLabel(const char* value)
{
	size_t i;
	for(i = 1; kBind[i].value; i++)
		if(strcmp(kBind[i].value, value) == 0)
			return kBind[i].label;
	return "";
}

// the options as the frontend takes them, from the tables above and what the run has (the profiles, the patches)
static void BuildOptions(void)
{
	size_t i, k;
	int n;
	gEngineChoices[0].value = "automatic";
	gEngineChoices[0].label = "Automatic";
	for(n = 0; OpenBGI_ProfileName(n) && n + 2 < RETRO_NUM_CORE_OPTION_VALUES_MAX; n++)
	{
		gEngineChoices[n + 1].value = OpenBGI_ProfileName(n);
		gEngineChoices[n + 1].label = NULL;
	}
	gEngineChoices[n + 1].value = gEngineChoices[n + 1].label = NULL;
	gPatchChoices[0].value = "none";
	gPatchChoices[0].label = "None";
	for(n = 0; n < gPatchCount; n++)
	{
		gPatchChoices[n + 1].value = gPatchName[n];
		gPatchChoices[n + 1].label = NULL;
	}
	gPatchChoices[n + 1].value = gPatchChoices[n + 1].label = NULL;
	for(i = 0; i < OPTIONS; i++)
	{
		const Option_t* o = &kOptions[i];
		struct retro_core_option_v2_definition* d = &gDefs[i];
		const char* name = o->name;
		memset(d, 0, sizeof *d);
		for(k = 0; o->choices[k].value && k + 1 < RETRO_NUM_CORE_OPTION_VALUES_MAX; k++)
			d->values[k] = o->choices[k];
		for(k = 0; k < PAD_BUTTONS; k++)
			if(strcmp(kPad[k].key, o->key) == 0)
			{ // a button: its name, and what its default is
				name = kPad[k].name;
				snprintf(gPadLabel[k], sizeof gPadLabel[k], "Default (%s)", BindLabel(kPad[k].usual));
				d->values[0].label = gPadLabel[k];
			}
		snprintf(gDesc[i], sizeof gDesc[i], "%s > %s", CategoryName(o->category), name);
		d->key = o->key;
		d->desc = gDesc[i];
		d->desc_categorized = name;
		d->info = o->info;
		d->category_key = o->category;
		d->default_value = d->values[0].value;
	}
	memset(&gDefs[OPTIONS], 0, sizeof gDefs[OPTIONS]);
}

static void FreeOptions(void)
{
	size_t i;
	for(i = 0; i < OPTIONS; i++)
	{
		free(gVarText[i]);
		gVarText[i] = NULL;
	}
}

/* The options to the frontend: with their categories where it has those,
 * else as plain variables.  Called again when the game is known, with
 * the patches found for it - the same options, other choices. */
static void TellOptions(void)
{
	unsigned version = 0;
	size_t i, k;
	BuildOptions();
	if(gEnv(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2)
	{
		struct retro_core_options_v2 options;
		options.categories = gCategories;
		options.definitions = gDefs;
		gEnv(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
		return;
	}
	FreeOptions();
	for(i = 0; i < OPTIONS; i++)
	{
		size_t need = strlen(gDesc[i]) + 3;
		for(k = 0; gDefs[i].values[k].value; k++)
			need += strlen(gDefs[i].values[k].value) + 1;
		gVarText[i] = (char*)malloc(need);
		if(!gVarText[i])
			break;
		snprintf(gVarText[i], need, "%s; ", gDesc[i]);
		for(k = 0; gDefs[i].values[k].value; k++)
			snprintf(gVarText[i] + strlen(gVarText[i]), need - strlen(gVarText[i]), "%s%s", k ? "|" : "", gDefs[i].values[k].value);
		gVars[i].key = gDefs[i].key;
		gVars[i].value = gVarText[i];
	}
	gVars[i].key = gVars[i].value = NULL;
	gEnv(RETRO_ENVIRONMENT_SET_VARIABLES, gVars);
}

static const char* Variable(const char* key)
{
	struct retro_variable v;
	v.key = key;
	v.value = NULL;
	return gEnv && gEnv(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value ? v.value : "";
}

static int Is(const char* key, const char* value)
{
	return strcmp(Variable(key), value) == 0;
}

// more text at the end of out[n]
static void Append(char* out, size_t n, const char* fmt, ...)
{
	size_t len = strlen(out);
	va_list ap;
	va_start(ap, fmt);
	if(len < n)
		vsnprintf(out + len, n - len, fmt, ap);
	va_end(ap);
}

// what a button of the pad does as the options have it: a binding's text, "" when they do not say
static const char* PadBinding(int button)
{
	const char* v = Variable(kPad[button].key);
	if(v[0] && strcmp(v, "default") != 0 && BindLabel(v)[0])
		return v;
	if(button < 2 && Is("openbgi_pad_confirm", "right"))
		return button == 0 ? "cancel" : "confirm";
	return "";
}

/* The input settings the options make, as the engine takes them
 * (OpenBGI_InputSettings): only what the player chose, so that the rest
 * stays as openbgi.json has it.  Under that file's lies the one thing
 * that is another here than in the engine: Start confirms, since a
 * frontend's Enter key is the pad's Start (RetroArch), and Enter is to
 * confirm. */
static void InputJson(char* out, size_t n)
{
	const char* v;
	int i, any = 0;
	snprintf(out, n, "{\"defaults\": {\"pad\": {\"start\": \"confirm\"}}, \"pad\": {");
	for(i = 0; i < PAD_BUTTONS; i++)
	{
		v = PadBinding(i);
		if(v[0])
		{
			Append(out, n, "%s\"%s\": \"%s\"", any ? ", " : "", kPad[i].control, v);
			any = 1;
		}
	}
	Append(out, n, "}");
	v = Variable("openbgi_pad_keys");
	if(strcmp(v, "on") == 0 || strcmp(v, "off") == 0)
		Append(out, n, ", \"ownPadKeys\": %s", v[1] == 'n' ? "true" : "false");
	v = Variable("openbgi_tap_delay");
	if(v[0] >= '0' && v[0] <= '9')
		Append(out, n, ", \"tapDelay\": %d", atoi(v));
	if(Is("openbgi_mouse_arrow", "on"))
		Append(out, n, ", \"pointer\": \"always\"");
	v = Variable("openbgi_pointer_size");
	if(strcmp(v, "small") == 0 || strcmp(v, "large") == 0)
		Append(out, n, ", \"pointerSize\": \"%s\"", v);
	v = Variable("openbgi_stick_speed");
	if(v[0] >= '1' && v[0] <= '9' && atoi(v) != 100 && atoi(v) >= 25 && atoi(v) <= 400)
		Append(out, n, ", \"stickSpeed\": %d", atoi(v));
	if(Is("openbgi_osk", "when asked for"))
		Append(out, n, ", \"onScreenKeyboard\": false");
	Append(out, n, "}");
}

// the engine's options a start has on its command line, each with a space before it
static void StartOptions(char* out, size_t n)
{
	const char* v;
	int i, k;
	snprintf(out, n, " --lang=%s", Is("openbgi_lang", "Japanese") ? "ja" : "en");
	v = Variable("openbgi_engine");
	for(i = 0; v[0] && OpenBGI_ProfileName(i); i++)
		if(strcmp(v, OpenBGI_ProfileName(i)) == 0)
			Append(out, n, " --engine=%s", v);
	if(Is("openbgi_disc_check", "skipped"))
		Append(out, n, " --no-disc-check");
	if(Is("openbgi_disc_marker", "shown"))
		Append(out, n, " --disc-marker");
	v = Variable("openbgi_stand_in_font");
	if(strcmp(v, "gothic") == 0 || strcmp(v, "mincho") == 0)
		Append(out, n, " --stand-in-font=%s", v);
	for(i = 0; i < PATCH_OPTIONS; i++)
	{
		char key[32], native[PATH_MAX_];
		snprintf(key, sizeof key, "openbgi_patch_%d", i + 1);
		v = Variable(key);
		for(k = 0; k < gPatchCount; k++)
			if(strcmp(v, gPatchName[k]) == 0)
			{ // one of those found: by its path
				OS_Utf8ToNative(gPatchName[k], native, sizeof native);
				Append(out, n, " --patch=\"%s%s\"", gPatchDir, native);
			}
	}
}

// the options as the frontend has them now
static void ReadOptions(void)
{
	const char* v = Variable("openbgi_scale");
	int w = 0, h = 0;
	gOpt.steps = strcmp(v, "whole numbers") == 0 ? OS_PRESENT_WHOLE : strcmp(v, "fill") == 0 ? OS_PRESENT_FILL
		: strcmp(v, "none") == 0                                                             ? OS_PRESENT_NONE
																							 : OS_PRESENT_POW2;
	v = Variable("openbgi_filter");
	gOpt.filter = strcmp(v, "bilinear") == 0 ? OS_PRESENT_BILINEAR : strcmp(v, "nearest") == 0 ? OS_PRESENT_NEAREST
																							   : OS_PRESENT_BICUBIC;
	v = Variable("openbgi_canvas");
	gOpt.canvasW = gOpt.canvasH = 0;
	if(!v[0])
		v = kCanvas[0].value; // (a frontend that does not say: the default)
	if(strcmp(v, "game") == 0)
		gOpt.canvasW = gOpt.canvasH = -1;
	else if(sscanf(v, "%dx%d", &w, &h) == 2 && w >= 320 && w <= 8192 && h >= 240 && h <= 8192)
	{
		gOpt.canvasW = w;
		gOpt.canvasH = h;
	}
	v = Variable("openbgi_pointer");
	gOpt.pointer = strcmp(v, "mouse") == 0 ? POINTER_MOUSE : strcmp(v, "touch screen") == 0 ? POINTER_TOUCH
																							: POINTER_AUTO;
	InputJson(gOpt.input, sizeof gOpt.input);
	StartOptions(gOpt.start, sizeof gOpt.start);
	gOpt.movies = !Is("openbgi_movies", "skipped");
	gOpt.playerFonts = !Is("openbgi_player_fonts", "off");
}

// what a start takes of the options, as one text to compare
static void StartSignature(char* out, size_t n)
{
	snprintf(out, n, "%s|%d|%d", gOpt.start, gOpt.movies, gOpt.playerFonts);
}

// whether the pointer device is a touch screen
static int TouchMode(void)
{
#if defined(__ANDROID__) || defined(ANDROID)
	return gOpt.pointer != POINTER_MOUSE;
#else
	return gOpt.pointer == POINTER_TOUCH;
#endif
}

// what the pad's controls do, for the frontend's own lists: as the options have them
static void TellControls(void)
{
	static struct retro_input_descriptor desc[PAD_BUTTONS + 8];
	static char label[PAD_BUTTONS][40];
	static const struct retro_input_descriptor kRest[] = {
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
		{0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Pointer left and right"},
		{0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Pointer up and down"},
		{0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Wheel"},
		{0, 0, 0, 0, NULL},
	};
	int i, n = 0;
	for(i = 0; i < PAD_BUTTONS; i++)
	{
		const char* v = PadBinding(i);
		if(!v[0])
			v = kPad[i].usual;
		if(strcmp(v, "none") == 0)
			continue;
		snprintf(label[i], sizeof label[i], "%s", BindLabel(v));
		desc[n].port = 0;
		desc[n].device = RETRO_DEVICE_JOYPAD;
		desc[n].index = 0;
		desc[n].id = kPad[i].id;
		desc[n].description = label[i];
		n++;
	}
	memcpy(&desc[n], kRest, sizeof kRest);
	gEnv(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

// ---- the input ----------------------------------------------------------------------------------

static int gKeysDown;               // keys of the keyboard that are down
static uint64_t gKeyCall;           // the call of retro_run before the last key event
static int gPadSeen;                // the pad was used: its state is handed over from now on
static uint32_t gPadWas[4];         // what was handed over last: buttons, D-pad, the sticks
static int gPointerX, gPointerY;    // where the pointer is, in the picture
static int gRelX, gRelY;            // of the mouse's own moves: what is not yet a pixel of the picture
static int gAbsX, gAbsY, gAbsKnown; // the pointer device's last reading
static int gMouseButtons;           // the mouse's buttons that are down
static struct
{
	int down, x, y;
} gTouch[MAX_TOUCHES];

// a RETROK_* as the engine's key: a Windows virtual key code, 0 for none
static int VirtualKey(unsigned k)
{
	if(k >= RETROK_a && k <= RETROK_z)
		return 'A' + (int)(k - RETROK_a);
	if(k >= RETROK_0 && k <= RETROK_9)
		return '0' + (int)(k - RETROK_0);
	if(k >= RETROK_F1 && k <= RETROK_F15)
		return 0x70 + (int)(k - RETROK_F1);
	if(k >= RETROK_KP0 && k <= RETROK_KP9)
		return 0x60 + (int)(k - RETROK_KP0);
	switch(k)
	{
		case RETROK_BACKSPACE: return 0x08;
		case RETROK_TAB: return 0x09;
		case RETROK_CLEAR: return 0x0c;
		case RETROK_RETURN:
		case RETROK_KP_ENTER: return 0x0d;
		case RETROK_LSHIFT:
		case RETROK_RSHIFT: return 0x10;
		case RETROK_LCTRL:
		case RETROK_RCTRL: return 0x11;
		case RETROK_LALT:
		case RETROK_RALT: return 0x12;
		case RETROK_PAUSE: return 0x13;
		case RETROK_CAPSLOCK: return 0x14;
		case RETROK_ESCAPE: return 0x1b;
		case RETROK_SPACE: return 0x20;
		case RETROK_PAGEUP: return 0x21;
		case RETROK_PAGEDOWN: return 0x22;
		case RETROK_END: return 0x23;
		case RETROK_HOME: return 0x24;
		case RETROK_LEFT: return 0x25;
		case RETROK_UP: return 0x26;
		case RETROK_RIGHT: return 0x27;
		case RETROK_DOWN: return 0x28;
		case RETROK_PRINT: return 0x2c;
		case RETROK_INSERT: return 0x2d;
		case RETROK_DELETE: return 0x2e;
		case RETROK_LSUPER: return 0x5b;
		case RETROK_RSUPER: return 0x5c;
		case RETROK_MENU: return 0x5d;
		case RETROK_KP_MULTIPLY: return 0x6a;
		case RETROK_KP_PLUS: return 0x6b;
		case RETROK_KP_MINUS: return 0x6d;
		case RETROK_KP_PERIOD: return 0x6e;
		case RETROK_KP_DIVIDE: return 0x6f;
		case RETROK_NUMLOCK: return 0x90;
		case RETROK_SCROLLOCK: return 0x91;
		case RETROK_SEMICOLON: return 0xba;
		case RETROK_EQUALS: return 0xbb;
		case RETROK_COMMA: return 0xbc;
		case RETROK_MINUS: return 0xbd;
		case RETROK_PERIOD: return 0xbe;
		case RETROK_SLASH: return 0xbf;
		case RETROK_BACKQUOTE: return 0xc0;
		case RETROK_LEFTBRACKET: return 0xdb;
		case RETROK_BACKSLASH: return 0xdc;
		case RETROK_RIGHTBRACKET: return 0xdd;
		case RETROK_QUOTE: return 0xde;
		default: return 0;
	}
}

static uint8_t gKeyDown[256]; // by virtual key: down as the engine knows it

// the frontend's keyboard: a key went down or up, with what it types
static void RETRO_CALLCONV KeyEvent(bool down, unsigned keycode, uint32_t character, uint16_t modifiers)
{
	int vk = VirtualKey(keycode);
	if(!gEngine || !gRunning)
		return;
	if(vk)
	{
		int flags = (down && gKeyDown[vk] ? OPENBGI_KEY_REPEAT : 0) | (vk != 0x12 && (modifiers & RETROKMOD_ALT) ? OPENBGI_KEY_SYSTEM : 0);
		if(down && !gKeyDown[vk])
			gKeysDown++;
		else if(!down && gKeyDown[vk])
			gKeysDown--;
		gKeyDown[vk] = down ? 1 : 0;
		gKeyCall = gCalls;
		OpenBGI_Key(gEngine, vk, down ? 1 : 0, flags);
	}
	if(down && character >= 0x20 && character != 0x7f && !(modifiers & (RETROKMOD_CTRL | RETROKMOD_ALT)))
	{
		char utf[8];
		size_t n = 0;
		if(character < 0x80)
			utf[n++] = (char)character;
		else if(character < 0x800)
		{
			utf[n++] = (char)(0xc0 | (character >> 6));
			utf[n++] = (char)(0x80 | (character & 0x3f));
		}
		else if(character < 0x10000)
		{
			utf[n++] = (char)(0xe0 | (character >> 12));
			utf[n++] = (char)(0x80 | ((character >> 6) & 0x3f));
			utf[n++] = (char)(0x80 | (character & 0x3f));
		}
		else if(character < 0x110000)
		{
			utf[n++] = (char)(0xf0 | (character >> 18));
			utf[n++] = (char)(0x80 | ((character >> 12) & 0x3f));
			utf[n++] = (char)(0x80 | ((character >> 6) & 0x3f));
			utf[n++] = (char)(0x80 | (character & 0x3f));
		}
		utf[n] = 0;
		if(n)
			OpenBGI_Text(gEngine, utf);
	}
}

/* The RetroPad of port 1.  Frontends bind keys of the keyboard to it as
 * well, and the keyboard is the engine's already: while a key is down
 * (for five seconds after the last key event at most: a frontend that
 * loses the focus may never say that a key went up) the pad is left as
 * it was, so a key does not act twice.  The pad is
 * handed over from its first use on - before that the engine has none,
 * and so draws no pad's keyboard for a player who types. */
static void FeedPad(void)
{
	static const struct
	{
		unsigned id;
		uint32_t bit;
	} kButtons[] = {
		{RETRO_DEVICE_ID_JOYPAD_B, OPENBGI_PAD_SOUTH},
		{RETRO_DEVICE_ID_JOYPAD_A, OPENBGI_PAD_EAST},
		{RETRO_DEVICE_ID_JOYPAD_Y, OPENBGI_PAD_WEST},
		{RETRO_DEVICE_ID_JOYPAD_X, OPENBGI_PAD_NORTH},
		{RETRO_DEVICE_ID_JOYPAD_L, OPENBGI_PAD_L1},
		{RETRO_DEVICE_ID_JOYPAD_R, OPENBGI_PAD_R1},
		{RETRO_DEVICE_ID_JOYPAD_SELECT, OPENBGI_PAD_SELECT},
		{RETRO_DEVICE_ID_JOYPAD_START, OPENBGI_PAD_START},
		{RETRO_DEVICE_ID_JOYPAD_L3, OPENBGI_PAD_L3},
		{RETRO_DEVICE_ID_JOYPAD_R3, OPENBGI_PAD_R3},
		{RETRO_DEVICE_ID_JOYPAD_L2, OPENBGI_PAD_L2},
		{RETRO_DEVICE_ID_JOYPAD_R2, OPENBGI_PAD_R2},
	};
	OpenBGI_PadState_t s;
	uint32_t now[4];
	size_t i;
	if(gKeysDown > 0 && gCalls - gKeyCall < KEY_HOLD)
		return;
	memset(&s, 0, sizeof s);
	s.size = sizeof s;
	for(i = 0; i < sizeof kButtons / sizeof kButtons[0]; i++)
		if(gInputState(0, RETRO_DEVICE_JOYPAD, 0, kButtons[i].id))
			s.buttons |= kButtons[i].bit;
	if(gInputState(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
		s.dpad |= OPENBGI_DPAD_UP;
	if(gInputState(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
		s.dpad |= OPENBGI_DPAD_DOWN;
	if(gInputState(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
		s.dpad |= OPENBGI_DPAD_LEFT;
	if(gInputState(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
		s.dpad |= OPENBGI_DPAD_RIGHT;
	s.leftX = gInputState(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
	s.leftY = gInputState(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
	s.rightX = gInputState(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
	s.rightY = gInputState(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
	now[0] = s.buttons;
	now[1] = s.dpad;
	now[2] = ((uint32_t)(uint16_t)s.leftX << 16) | (uint16_t)s.leftY;
	now[3] = ((uint32_t)(uint16_t)s.rightX << 16) | (uint16_t)s.rightY;
	if(!gPadSeen)
	{ // at rest it is not there yet (a stick's slack is no use)
		if(!s.buttons && !s.dpad && abs(s.leftX) < 0x2000 && abs(s.leftY) < 0x2000 && abs(s.rightX) < 0x2000 && abs(s.rightY) < 0x2000)
			return;
		gPadSeen = 1;
	}
	else if(memcmp(now, gPadWas, sizeof now) == 0)
		return;
	memcpy(gPadWas, now, sizeof now);
	OpenBGI_Pad(gEngine, 0, &s);
}

/* A reading of the pointer device (-0x7fff .. 0x7fff over the canvas,
 * each way) as a place in the game's picture, kept inside it. */
static void FromPointer(int vx, int vy, int* x, int* y)
{
	int cx = (int)(((int64_t)vx + 0x7fff) * gWidth / 0xfffe), cy = (int)(((int64_t)vy + 0x7fff) * gHeight / 0xfffe);
	int px, py, pw, ph;
	OsPresent_Place(gPresent, &px, &py, &pw, &ph);
	if(pw > 0 && ph > 0)
	{
		OsPresent_ToPicture(gPresent, cx, cy, x, y);
		return;
	}
	*x = *y = 0; // (no picture yet)
}

static void MouseButton(int button, int down, int bit)
{
	if(((gMouseButtons & bit) != 0) == (down != 0))
		return;
	gMouseButtons ^= bit;
	OpenBGI_Button(gEngine, button, down, gPointerX, gPointerY, 0);
}

/* The mouse: where it is from the pointer device when that follows it
 * (its reading changes while no button of a touch screen could be down),
 * else from its own moves, which are the screen's pixels: as many of the
 * picture's as the picture is shown smaller or larger.  Its buttons and
 * its wheel. */
static void FeedMouse(void)
{
	int w = gFrame.width > 0 ? gFrame.width : DEFAULT_W, h = gFrame.height > 0 ? gFrame.height : DEFAULT_H;
	int ax = gInputState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
	int ay = gInputState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
	int dx = gInputState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
	int dy = gInputState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
	int x = gPointerX, y = gPointerY;
	if(gAbsKnown && (ax != gAbsX || ay != gAbsY) && ax > -0x8000 && ay > -0x8000)
	{
		FromPointer(ax, ay, &x, &y);
		gRelX = gRelY = 0;
	}
	else if(dx || dy)
	{
		int px, py, pw, ph;
		OsPresent_Place(gPresent, &px, &py, &pw, &ph);
		if(pw <= 0 || ph <= 0)
		{
			pw = w;
			ph = h;
		}
		gRelX += dx * w;
		gRelY += dy * h;
		x += gRelX / pw;
		y += gRelY / ph;
		gRelX %= pw;
		gRelY %= ph;
		x = x < 0 ? 0 : x >= w ? w - 1
							   : x;
		y = y < 0 ? 0 : y >= h ? h - 1
							   : y;
	}
	gAbsX = ax;
	gAbsY = ay;
	gAbsKnown = 1;
	if(x != gPointerX || y != gPointerY)
	{
		gPointerX = x;
		gPointerY = y;
		OpenBGI_Pointer(gEngine, x, y);
	}
	MouseButton(OPENBGI_BUTTON_LEFT, gInputState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT), 1);
	MouseButton(OPENBGI_BUTTON_RIGHT, gInputState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT), 2);
	MouseButton(OPENBGI_BUTTON_MIDDLE, gInputState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE), 4);
	if(gInputState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP))
		OpenBGI_Wheel(gEngine, 120);
	if(gInputState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN))
		OpenBGI_Wheel(gEngine, -120);
}

// the touch screen: each finger of the pointer device as a contact
static void FeedTouch(void)
{
	int count = gInputState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_COUNT), i;
	if(count <= 0 && gInputState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED))
		count = 1; // (a frontend that does not count them)
	for(i = 0; i < MAX_TOUCHES; i++)
	{
		int down = i < count && gInputState(0, RETRO_DEVICE_POINTER, (unsigned)i, RETRO_DEVICE_ID_POINTER_PRESSED);
		int x = gTouch[i].x, y = gTouch[i].y;
		if(down)
			FromPointer(gInputState(0, RETRO_DEVICE_POINTER, (unsigned)i, RETRO_DEVICE_ID_POINTER_X),
				gInputState(0, RETRO_DEVICE_POINTER, (unsigned)i, RETRO_DEVICE_ID_POINTER_Y), &x, &y);
		if(down && !gTouch[i].down)
			OpenBGI_Touch(gEngine, i, OPENBGI_TOUCH_DOWN, x, y);
		else if(down && (x != gTouch[i].x || y != gTouch[i].y))
			OpenBGI_Touch(gEngine, i, OPENBGI_TOUCH_MOVE, x, y);
		else if(!down && gTouch[i].down)
			OpenBGI_Touch(gEngine, i, OPENBGI_TOUCH_UP, x, y);
		gTouch[i].down = down;
		gTouch[i].x = x;
		gTouch[i].y = y;
	}
}

static void ResetInput(void)
{
	gKeysDown = 0;
	gPadSeen = 0;
	gAbsKnown = 0;
	gMouseButtons = 0;
	gPointerX = gPointerY = 0;
	gRelX = gRelY = 0;
	memset(gPadWas, 0, sizeof gPadWas);
	memset(gKeyDown, 0, sizeof gKeyDown);
	memset(gTouch, 0, sizeof gTouch);
}

// ---- the requests -------------------------------------------------------------------------------

/* What the engine asks the host: the boxes it could not draw itself (one
 * made before there is a picture, or after the game) are shown as a line
 * of the frontend's and take their default answer; a file or a folder to
 * pick and the game chooser are not to be had here. */
static void AnswerRequests(void)
{
	OpenBGI_Request_t q;
	memset(&q, 0, sizeof q);
	q.size = sizeof q;
	while(gEngine && OpenBGI_NextRequest(gEngine, &q))
	{
		int answer = 0;
		if(q.kind == OPENBGI_ASK_MESSAGE)
		{
			uint32_t buttons = q.u.message.flags & 0xf;
			int second = (q.u.message.flags & OPENBGI_MB_DEFBUTTON2) != 0;
			char utf[1024];
			OsCommon_SjisToUtf8(q.u.message.text ? q.u.message.text : "", utf, sizeof utf);
			Log(RETRO_LOG_WARN, "%s", utf);
			Notice(utf);
			if(buttons == OPENBGI_MB_YESNO)
				answer = second ? OPENBGI_IDNO : OPENBGI_IDYES;
			else if(buttons == OPENBGI_MB_RETRYCANCEL || (buttons == OPENBGI_MB_OKCANCEL && second))
				answer = OPENBGI_IDCANCEL;
			else
				answer = OPENBGI_IDOK;
		}
		else if(q.kind == OPENBGI_ASK_GAME_CHOOSER)
			answer = -1;
		OpenBGI_Answer(gEngine, q.id, answer);
	}
}

// ---- the game -----------------------------------------------------------------------------------

// the engine's frames until the clock is at `target`; 0 once the game ended
static int RunTo(uint32_t target)
{
	while(gRunning && (int32_t)(target - gNow) > 0)
	{
		uint32_t d;
		int i;
		AnswerRequests();
		if(OpenBGI_RunFrame(gEngine, &gFrame) != OPENBGI_RUNNING)
		{
			gRunning = 0;
			break;
		}
		// what the frame changed of the picture: scaled anew with the next picture handed over
		if(gFrame.changes < 0)
			OsPresent_Changed(gPresent, 0, -1);
		for(i = 0; i < gFrame.changes; i++)
			OsPresent_Changed(gPresent, gFrame.change[i].y, gFrame.change[i].h);
		OpenBGI_PumpEvents(gEngine);
		d = gFrame.idleMs > 1 ? (uint32_t)gFrame.idleMs : 1; // what the frame asks to wait, a millisecond at least
		if(gFrame.presented && target - gNow > d)
			d = target - gNow; // a picture went out: on to the display's next refresh
		gNow += d;
	}
	return gRunning;
}

// tear the run down; the engine stays for a new start
static void StopGame(void)
{
	if(!gEngine)
		return;
	AnswerRequests(); // what an ended run still asks
	OpenBGI_Stop(gEngine);
	AnswerRequests(); // and what the shutdown does
	gRunning = 0;
	memset(&gFrame, 0, sizeof gFrame);
	gFrame.size = sizeof gFrame;
}

/* A new engine for the game at gGamePath, with what the options say of a
 * start: the fonts, the movies, the engine's own options on the command
 * line.  0 when there is no memory for one. */
static int MakeEngine(void)
{
	static OpenBGI_Host_t host;
	LoadFonts();
	gFiles = *OsCommon_Files();
	gFiles.userDataDir = UserDataDir;
	gFiles.binaryDir = BinaryDir;
	memset(&host, 0, sizeof host);
	host.size = sizeof host;
	host.api = OPENBGI_API_VERSION;
	host.ticksMs = TicksMs;
	host.perfNs = PerfNs;
	host.localTime = LocalTime;
	host.files = &gFiles;
	host.fontFiles = gFontCount ? &kFonts : NULL;
	host.movieDecoder = gOpt.movies ? OsCommon_MovieDecoder() : NULL;
#if defined(__ANDROID__) || defined(ANDROID)
	host.caps = 0; // no keyboard to count on: a touch on a text field brings the engine's
#else
	host.caps = OPENBGI_CAP_KEYBOARD;
#endif
	gEngine = OpenBGI_New(&host);
	if(!gEngine)
	{
		Log(RETRO_LOG_ERROR, "no engine (out of memory)");
		return 0;
	}
	OpenBGI_InputSettings(gEngine, gOpt.input);
	// the command line: the archive itself, or the folder the file is in, never written into
	snprintf(gCmdLine, sizeof gCmdLine, "--overlay%s \"%s\"", gOpt.start, gGamePath);
	StartSignature(gStarted, sizeof gStarted);
	snprintf(gToldFor, sizeof gToldFor, "%s", gStarted);
	return 1;
}

static int StartGame(void)
{
	gNow = 0;
	gCalls = 0;
	gShutdownSent = 0;
	ResetInput();
	memset(&gFrame, 0, sizeof gFrame);
	gFrame.size = sizeof gFrame;
	OsPresent_Changed(gPresent, 0, -1);
	gRunning = OpenBGI_Start(gEngine, gCmdLine) == OPENBGI_RUNNING;
	if(!gRunning)
	{
		StopGame();
		return 0;
	}
	OpenBGI_SetAudioLead(gEngine, AUDIO_LEAD);
	return 1;
}

// ---- the picture --------------------------------------------------------------------------------

// the display's size, when the frontend tells it
static void AskDisplay(void)
{
	struct retro_display_metadata m;
	memset(&m, 0, sizeof m);
	m.version = 1;
	m.size = sizeof m;
	gDisplayW = gDisplayH = 0;
	if(gEnv(RETRO_ENVIRONMENT_GET_DISPLAY_METADATA, &m) && m.geo.native_width >= 320 && m.geo.native_width <= 8192 && m.geo.native_height >= 240 &&
		m.geo.native_height <= 8192)
	{
		gDisplayW = (int)m.geo.native_width;
		gDisplayH = (int)m.geo.native_height;
	}
}

/* The canvas as the options, the frontend and the game have it now - what
 * the options say, else the display's size, else CANVAS_W x CANVAS_H; with
 * "the game's own" the picture's size, nothing scaled - and the frontend
 * told (`tell`) when it is another than the one it knows. */
static void SetCanvas(int tell)
{
	struct retro_system_av_info av;
	int w = CANVAS_W, h = CANVAS_H, steps = gOpt.steps, filter = gOpt.filter;
	if(gOpt.canvasW > 0)
	{
		w = gOpt.canvasW;
		h = gOpt.canvasH;
	}
	else if(gOpt.canvasW < 0)
	{
		w = gFrame.width > 0 ? gFrame.width : gWidth;
		h = gFrame.height > 0 ? gFrame.height : gHeight;
		steps = OS_PRESENT_NONE;
		filter = OS_PRESENT_NEAREST;
	}
	else if(gDisplayW > 0)
	{
		w = gDisplayW;
		h = gDisplayH;
	}
	OsPresent_Setup(gPresent, w, h, steps, filter);
	if(w == gWidth && h == gHeight)
		return;
	gWidth = w;
	gHeight = h;
	if(!tell)
	{ // (the frontend asks next)
		gMaxW = w > gMaxW ? w : gMaxW;
		gMaxH = h > gMaxH ? h : gMaxH;
		return;
	}
	retro_get_system_av_info(&av);
	if(w > gMaxW || h > gMaxH)
	{ // larger than announced: the frontend makes its buffers anew
		gMaxW = w > gMaxW ? w : gMaxW;
		gMaxH = h > gMaxH ? h : gMaxH;
		av.geometry.max_width = (unsigned)gMaxW;
		av.geometry.max_height = (unsigned)gMaxH;
		gEnv(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
	}
	else
		gEnv(RETRO_ENVIRONMENT_SET_GEOMETRY, &av.geometry);
}

// the canvas with the frame's picture on it to the frontend; nothing new when nothing changed
static void ShowPicture(void)
{
	const OpenBGI_Frame_t* f = &gFrame;
	int changed = 1;
	const uint32_t* canvas = OsPresent_Draw(gPresent, f->width > 0 && f->height > 0 ? f->pixels : NULL, f->width, f->height, f->pitch, f->bpp, &changed);
	if(canvas && (changed || !gCanDupe))
		gVideo(canvas, (unsigned)gWidth, (unsigned)gHeight, (size_t)gWidth * 4);
	else
		gVideo(NULL, (unsigned)gWidth, (unsigned)gHeight, 0);
}

/* The options changed, or are read for the first time with an engine
 * there: what holds at once is applied, and the player told when the
 * rest needs the game started again. */
static void ApplyOptions(void)
{
	char now[sizeof gStarted];
	ReadOptions();
	if(!gEngine)
		return;
	OpenBGI_InputSettings(gEngine, gOpt.input);
	TellControls();
	AskDisplay();
	StartSignature(now, sizeof now);
	if(gRunning && strcmp(now, gStarted) != 0 && strcmp(now, gToldFor) != 0)
		Notice("OpenBGI: this option holds from the game's next start (Restart)");
	snprintf(gToldFor, sizeof gToldFor, "%s", now);
}

// ---- the interface ------------------------------------------------------------------------------

RETRO_API unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
	struct retro_log_callback log;
	gEnv = cb;
	if(cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
		gLog = log.log;
	TellOptions(); // (the patches' choices come with the game)
	TellControls();
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)
{
	gVideo = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)
{
	(void)cb;
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
	gAudioBatch = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb)
{
	gInputPoll = cb;
}

RETRO_API void retro_set_input_state(retro_input_state_t cb)
{
	gInputState = cb;
}

RETRO_API void retro_init(void)
{
}

RETRO_API void retro_deinit(void)
{
	OsPresent_Delete(gPresent);
	gPresent = NULL;
	free(gFonts);
	gFonts = NULL;
	gFontCount = 0;
	FreeOptions();
}

RETRO_API void retro_get_system_info(struct retro_system_info* info)
{
	memset(info, 0, sizeof *info);
	info->library_name = "OpenBGI";
	info->library_version = OPENBGI_VERSION;
	info->valid_extensions = "zip|iso|exe|gdb|arc|_bp";
	info->need_fullpath = true; // the game is played from its file, not from a copy in memory
	info->block_extract = true; // a zip archive is the game: the frontend is not to unpack it
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info)
{
	memset(info, 0, sizeof *info);
	info->geometry.base_width = (unsigned)gWidth;
	info->geometry.base_height = (unsigned)gHeight;
	info->geometry.max_width = (unsigned)gMaxW;
	info->geometry.max_height = (unsigned)gMaxH;
	info->geometry.aspect_ratio = (float)gWidth / (float)gHeight;
	info->timing.fps = FPS;
	info->timing.sample_rate = OPENBGI_AUDIO_RATE;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
	(void)port, (void)device;
}

/* The same game from its start, in a new engine: what the options say of
 * a start holds from here. */
RETRO_API void retro_reset(void)
{
	if(!gEngine)
		return;
	StopGame();
	OpenBGI_Delete(gEngine);
	gEngine = NULL;
	ReadOptions();
	if(MakeEngine() && StartGame())
		return;
	Log(RETRO_LOG_ERROR, "the game could not be started again");
	gRunning = 0; // (retro_run tells the frontend)
}

// the last component of a path's extension, lower case, is one of the two archives'
static int IsArchiveName(const char* path)
{
	const char* dot = strrchr(path, '.');
	return dot && (SameName(dot, ".zip") || SameName(dot, ".iso"));
}

RETRO_API bool retro_load_game(const struct retro_game_info* game)
{
	struct retro_keyboard_callback keys;
	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
	const char* dir = NULL;
	bool dupe = false;
	int isArchive;
	if(!game || !game->path || !game->path[0])
	{
		Log(RETRO_LOG_ERROR, "no game: a zip archive, a disc image or a file of a game's folder is needed");
		return false;
	}
	if(!gEnv(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format))
	{
		Log(RETRO_LOG_ERROR, "the frontend takes no pictures of 32 bits a pixel");
		return false;
	}
	gCanDupe = gEnv(RETRO_ENVIRONMENT_GET_CAN_DUPE, &dupe) && dupe;
	gSaveDir[0] = gSystemDir[0] = 0;
	if(gEnv(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir && dir[0])
		MakeDir(dir, "OpenBGI", gSaveDir, sizeof gSaveDir);
	dir = NULL;
	if(gEnv(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir && dir[0])
		MakeDir(dir, "openbgi", gSystemDir, sizeof gSystemDir);
	if(!gSaveDir[0])
	{ // a frontend without a save directory: beside the game
		char folder[PATH_MAX_];
		const char* part;
		snprintf(folder, sizeof folder, "%s", game->path);
		part = OsCommon_FilePart(folder);
		folder[part - folder] = 0;
		MakeDir(folder[0] ? folder : ".", "OpenBGI", gSaveDir, sizeof gSaveDir);
	}

	// the game: the archive itself, or the folder the file is in
	OS_Utf8ToNative(game->path, gGamePath, sizeof gGamePath);
	isArchive = IsArchiveName(gGamePath);
	if(!isArchive)
	{
		const char* part = OsCommon_FilePart(gGamePath);
		if(part > gGamePath)
			gGamePath[part - gGamePath] = 0;
		else
			snprintf(gGamePath, sizeof gGamePath, ".");
	}
	FindPatches(isArchive);
	TellOptions(); // again, with the patches to choose from
	ReadOptions();
	TellControls();
	AskDisplay();
	if(gDisplayW)
		Log(RETRO_LOG_INFO, "the display is %d x %d", gDisplayW, gDisplayH);
	else
		Log(RETRO_LOG_INFO, "the frontend does not tell the display's size: the canvas is %d x %d unless the option \"Screen size\" names another", CANVAS_W,
			CANVAS_H);
	if(!gPresent)
		gPresent = OsPresent_New();
	if(!gPresent || !MakeEngine())
		return false;
	gWidth = CANVAS_W;
	gHeight = CANVAS_H;
	gMaxW = CANVAS_W;
	gMaxH = CANVAS_H;
	if(!StartGame())
	{
		Log(RETRO_LOG_ERROR, "%s is no game that can be played", game->path);
		OpenBGI_Delete(gEngine);
		gEngine = NULL;
		return false;
	}
	// the first frames, until the game's window has its size: the frontend asks for the canvas next
	while(gRunning && gNow < 2000 && !(gFrame.pixels && gFrame.width > 0))
		RunTo(gNow + 1);
	SetCanvas(0);
	gCalls = ((uint64_t)gNow * FPS + 999) / 1000; // the calls that this time would have been
	keys.callback = KeyEvent;
	gEnv(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &keys);
	Log(RETRO_LOG_INFO, "playing %s; what the game writes goes to %sgames", game->path, gSaveDir);
	return true;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t count)
{
	(void)type, (void)info, (void)count;
	return false;
}

RETRO_API void retro_unload_game(void)
{
	if(!gEngine)
		return;
	StopGame();
	OpenBGI_Delete(gEngine);
	gEngine = NULL;
	gFontCount = 0;
	gPatchCount = 0;
}

RETRO_API void retro_run(void)
{
	static int16_t sound[AUDIO_FRAMES * 2];
	bool changed = false;
	if(!gEngine)
	{ // a restart that found no engine: nothing to run
		if(!gShutdownSent)
			gEnv(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
		gShutdownSent = 1;
		return;
	}
	if(gEnv(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &changed) && changed)
		ApplyOptions();
	else if(gOpt.canvasW == 0 && gCalls % FPS == 0)
		AskDisplay(); // (it may be another by now)
	gInputPoll();
	if(gRunning)
	{
		FeedPad();
		if(TouchMode())
			FeedTouch();
		else
			FeedMouse();
		gCalls++;
		RunTo((uint32_t)(gCalls * 1000 / FPS));
	}
	SetCanvas(1);
	ShowPicture();
	memset(sound, 0, sizeof sound);
	OpenBGI_AudioRead(gEngine, sound, AUDIO_FRAMES);
	gAudioBatch(sound, AUDIO_FRAMES);
	if(!gRunning && !gShutdownSent)
	{ // the game ended (its own "end", an error that stopped it): the frontend closes the core
		StopGame();
		gShutdownSent = 1;
		gEnv(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
	}
}

// no saved states yet (the engine's state is prepared for them: all of it is in one struct)
RETRO_API size_t retro_serialize_size(void)
{
	return 0;
}

RETRO_API bool retro_serialize(void* data, size_t size)
{
	(void)data, (void)size;
	return false;
}

RETRO_API bool retro_unserialize(const void* data, size_t size)
{
	(void)data, (void)size;
	return false;
}

RETRO_API void retro_cheat_reset(void)
{
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
	(void)index, (void)enabled, (void)code;
}

RETRO_API unsigned retro_get_region(void)
{
	return RETRO_REGION_NTSC;
}

RETRO_API void* retro_get_memory_data(unsigned id)
{
	(void)id;
	return NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
	(void)id;
	return 0;
}
