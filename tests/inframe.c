/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/inframe.c - unit test of the dialogs, the text entry and the
 *                   child windows the engine draws in the frame
 *                   (src/sys/inframe.c, src/include/bgi/inframe.h, src/sys/edit.c,
 *                   src/include/bgi/edit.h, src/sys/child.c), of the picture's
 *                   overlays (src/sys/screen.c) and of the frame's repaint
 *                   (OpenBGI_Frame_t.expose); built as test_inframe by
 *                   `make test`
 *
 * An engine without a game, its picture made by the test, and no fonts
 * for the dialogs (their texts are left out; their layout does not depend
 * on them where the test clicks).  The text entry gets a renderer of the
 * test's, whose characters are blocks of ink at known places.  The test
 * is the host: it keeps a window of its own up to date with the changes
 * of each frame, as a host does, and looks for the dialog in it.  The
 * groups, in the order they run:
 *
 *   route   - which requests the frame takes: the message boxes and the
 *             scripts' dialogs when the host has no dialogs of its own or
 *             --dialogs=frame says so, never without a picture, never the
 *             others; the host gets the rest
 *   box     - the box over the picture, centred, the engine's picture
 *             itself untouched; the keys are the box's (the game gets no
 *             event, the key state still follows); Enter answers with the
 *             default button and the box goes from the window
 *   input   - the field takes typed text and the frame says so, with the
 *             field's place; Enter answers 1 with the entry, Escape, the
 *             Cancel button and the close box 0 with the buffer untouched
 *   profile - a half-width name brings a complaint over the dialog, which
 *             takes the keys until it is closed; then the entry goes
 *   pointer - a drag of the caption moves the box; a new picture size
 *             centres it again; a 16-bit picture gets the box in its
 *             pixels
 *   stop    - OpenBGI_Stop takes the box down; boxes are the host's after it
 *   entry   - the text entry over the picture: its text centred at the top
 *             in its colour, the selection highlighted whether it has the
 *             focus or not, the caret while it has; the keys and the text
 *             are its while it has the focus (the game gets no event, the
 *             key state follows), Tab and Enter select and give the focus
 *             back; the limit, "B0 29"; a click on it focuses it with the
 *             caret where it was clicked, its release is its too, a click
 *             elsewhere takes the focus away; a text wider than it is
 *             scrolled to the caret; hidden it takes nothing; a dialog over
 *             it takes the keys; the default text before a setup
 *   builds  - from 1.69/472 on: no control before a setup or after "B0
 *             21"; "B0 2A"'s alignment; Tab hiding the entry (1.529 on),
 *             Enter with "B0 28"; several lines when the rectangle holds
 *             them, wrapped at spaces and between double-byte characters,
 *             taking no more text than they show, Up / Down / Home and a
 *             click on a line; 1.69/472's Tab
 *   host    - with a host's entry every call goes to the host, the engine
 *             draws nothing and takes no key; a key the host's entry took
 *             only goes into the key state; the style a later build's
 *             setup and "B0 28" give it, "B0 21"
 *   children  - the child windows over the picture: made hidden, shown
 *             with a caption bar, the sizes and the slots; a press makes
 *             one active (the main window inactive) and is not the
 *             game's; Ctrl+C; the stacking; the caption's drag; a window
 *             moved into the picture; the host's focus meanwhile; hiding
 *             or closing the active one; the text entry's focus; a new
 *             picture size, a 16-bit picture
 *   childhost - with a host's child windows every call goes to them, the
 *             picture presented at every drawing; a window the host could
 *             not make; the scaled display's picture; --child-windows=frame
 *   expose  - the frame's repaint: black, the picture or nothing
 */
#include "bgi/core.h"
#include "bgi/inframe.h"
#include "bgi/request.h"
#include "bgi/display.h"
#include "bgi/input.h"
#include "bgi/sysobj/events.h"
#include "bgi/ui.h"
#include "bgi/edit.h"
#include "bgi/msg.h"
#include "host/os.h"
#include "host/os_common.h"
#include "bgi/sys.h"
#include "bgi/gfx.h"
#include "bgi/gfx/gfxmgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

// ---- the host -------------------------------------------------------------------

static uint32_t gNow = 1000; // the host's clock (ms)

static uint32_t HostTicksMs(void* user)
{
	BGI_UNUSED(user);
	return gNow;
}

static int HostPerfNs(void* user, int64_t* out)
{
	BGI_UNUSED(user);
	BGI_UNUSED(out);
	return 0;
}

static void HostLocalTime(void* user, OpenBGI_DateTime_t* out)
{
	BGI_UNUSED(user);
	memset(out, 0, sizeof *out);
	out->year = 2026;
	out->month = 10;
	out->day = 25;
}

static OpenBGI_Host_t gHost = {.size = sizeof(OpenBGI_Host_t), .api = OPENBGI_API_VERSION, .ticksMs = HostTicksMs, .perfNs = HostPerfNs, .localTime = HostLocalTime};

// the host's window: its copy of the picture, kept up to date with the changes of the frames
static struct
{
	uint8_t* px;
	int w, h, pitch, bpp;
} gWin;

static OpenBGI_Frame_t gFrame = {.size = sizeof gFrame}; // the last frame

static void Frame(OpenBGI_t* o)
{
	OpenBGI_Frame_t* f = &gFrame;
	int i, y;
	gNow += 10;
	OpenBGI_RunFrame(o, f);
	if(!f->pixels || !f->changes)
		return;
	if(f->changes < 0 || f->width != gWin.w || f->height != gWin.h || f->bpp != gWin.bpp)
	{
		free(gWin.px);
		gWin.px = (uint8_t*)malloc((size_t)f->pitch * (size_t)f->height);
		gWin.w = f->width;
		gWin.h = f->height;
		gWin.pitch = f->pitch;
		gWin.bpp = f->bpp;
		memcpy(gWin.px, f->pixels, (size_t)f->pitch * (size_t)f->height);
		return;
	}
	for(i = 0; i < f->changes; i++)
	{
		const OpenBGI_Rect_t* r = &f->change[i];
		for(y = r->y; y < r->y + r->h; y++)
			memcpy(gWin.px + (size_t)y * gWin.pitch + (size_t)r->x * (f->bpp / 8), f->pixels + (size_t)y * f->pitch + (size_t)r->x * (f->bpp / 8),
				(size_t)r->w * (f->bpp / 8));
	}
}

// pixel (x, y) of the window, 0x00RRGGBB (5-bit channels widened)
static uint32_t WinPx(int x, int y)
{
	const uint8_t* row = gWin.px + (size_t)y * gWin.pitch;
	if(gWin.bpp == 16)
	{
		unsigned v = (unsigned)(row[x * 2] | row[x * 2 + 1] << 8), r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
		return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
	}
	return ((const uint32_t*)row)[x] & 0xffffff;
}

// the rectangle of the window that is not black (the dialog over a black picture); 0 when it is all black
static int Found(OpenBGI_Rect_t* r)
{
	int x, y, l = gWin.w, t = gWin.h, rr = -1, b = -1;
	for(y = 0; y < gWin.h; y++)
		for(x = 0; x < gWin.w; x++)
			if(WinPx(x, y))
			{
				l = x < l ? x : l;
				t = y < t ? y : t;
				rr = x > rr ? x : rr;
				b = y > b ? y : b;
			}
	r->x = l;
	r->y = t;
	r->w = rr - l + 1;
	r->h = b - t + 1;
	return rr >= 0;
}

static OpenBGI_t* New(uint32_t caps, int w, int h, int mode)
{
	OpenBGI_t* o;
	gHost.caps = caps;
	gHost.files = OsCommon_Files();
	o = OpenBGI_New(&gHost);
	if(o)
		Screen_Setup(o, w, h, mode);
	return o;
}

// the engine's picture is all black (the dialogs lie over it)
static int PictureBlack(OpenBGI_t* o)
{
	size_t i, n = (size_t)o->screen.pic.pitch * (size_t)o->screen.pic.h;
	for(i = 0; i < n; i++)
		if(o->screen.mem[i])
			return 0;
	return 1;
}

static void Click(OpenBGI_t* o, int x, int y)
{
	OpenBGI_Pointer(o, x, y);
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 1, x, y, 0);
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 0, x, y, 0);
}

static void Key(OpenBGI_t* o, int vk)
{
	OpenBGI_Key(o, vk, 1, 0);
	OpenBGI_Key(o, vk, 0, 0);
}

// the script events queued for the game ("80 AC"), all taken; how many there were
static int Events(OpenBGI_t* o)
{
	uint32_t v[3];
	int n = 0;
	while(MsgQueue_Get(o, v))
		n++;
	return n;
}

// ---- a renderer of the test's: a character is a block of ink inside its cell -------------------

typedef struct FakeFont
{
	int h, w; // the cell, the half-width character's width
} FakeFont_t;

static void* FOpen(void* user, const char* face, int height, int width, int bold, int italic)
{
	FakeFont_t* f = (FakeFont_t*)malloc(sizeof *f);
	BGI_UNUSED(user);
	if(f)
	{
		f->h = height;
		f->w = width;
	}
	return f;
}

static void FClose(void* user, void* font)
{
	BGI_UNUSED(user);
	free(font);
}

static int FAdvance(void* user, void* font, const char* ch, int len)
{
	BGI_UNUSED(user);
	return ((FakeFont_t*)font)->w * (len > 1 ? 2 : 1);
}

// ink from a fifth into four fifths of the advance and from a quarter into three quarters of the cell
static void FRender(void* user, void* font, const char* ch, int len, uint8_t* buf, int pitch, int w, int h)
{
	const FakeFont_t* f = (const FakeFont_t*)font;
	int adv = FAdvance(user, font, ch, len), x, y;
	for(y = 0; y < h; y++)
		for(x = 0; x < w; x++)
			buf[y * pitch + x] = x >= adv / 5 && x < adv * 4 / 5 && y >= f->h / 4 && y < f->h * 3 / 4 ? 255 : 0;
}

static const OpenBGI_FontRenderer_t kRenderer = {sizeof(OpenBGI_FontRenderer_t), FOpen, NULL, FClose, FRender, NULL, FAdvance, NULL, NULL, NULL, NULL, NULL, NULL};

// ---- the groups -----------------------------------------------------------------------

static void TestRoute(void)
{
	OpenBGI_t* o = New(0, 400, 300, PM_RGB32);
	OpenBGI_Request_t q = {.size = sizeof q};
	char path[0x100];
	uint32_t id;
	printf("route\n");
	if(!o)
		return;
	id = Request_Box(o, "t", "c", OPENBGI_MB_OK, REQUEST_TREAT_NONE);
	CHECK(id && Request_Pending(o) == 1 && !OpenBGI_NextRequest(o, &q) && !Inframe_Up(o));
	Frame(o);
	CHECK(Inframe_Up(o) && !OpenBGI_NextRequest(o, &q));
	memset(&q, 0, sizeof q);
	q.size = sizeof q;
	q.kind = OPENBGI_ASK_FOLDER; // never the frame's
	q.u.folder.path = path;
	q.u.folder.pathSize = sizeof path;
	CHECK(Request_Post(o, &q, REQUEST_TREAT_NONE) && OpenBGI_NextRequest(o, &q) && q.kind == OPENBGI_ASK_FOLDER);
	OpenBGI_Answer(o, q.id, 0);
	OpenBGI_Delete(o); // with the box up

	o = New(OPENBGI_CAP_DIALOGS, 400, 300, PM_RGB32);
	if(!o)
		return;
	id = Request_Box(o, "t", "c", OPENBGI_MB_OK, REQUEST_TREAT_NONE);
	CHECK(OpenBGI_NextRequest(o, &q) && q.id == id); // the host's
	OpenBGI_Answer(o, id, OPENBGI_IDOK);
	o->engine.optDialogs = 1; // --dialogs=frame
	id = Request_Box(o, "t", "c", OPENBGI_MB_OK, REQUEST_TREAT_NONE);
	CHECK(!OpenBGI_NextRequest(o, &q));
	Frame(o);
	CHECK(Inframe_Up(o));
	Key(o, 0x0d);
	CHECK(!Inframe_Up(o) && Request_Pending(o) == 0);
	OpenBGI_Delete(o);

	o = New(0, 400, 300, PM_RGB32);
	if(!o)
		return;
	o->engine.optDialogs = 2; // --dialogs=host
	id = Request_Box(o, "t", "c", OPENBGI_MB_OK, REQUEST_TREAT_NONE);
	CHECK(OpenBGI_NextRequest(o, &q) && q.id == id);
	OpenBGI_Answer(o, id, OPENBGI_IDOK);
	o->engine.optDialogs = 0;
	Screen_Free(o); // no picture: the host's
	id = Request_Box(o, "t", "c", OPENBGI_MB_OK, REQUEST_TREAT_NONE);
	CHECK(OpenBGI_NextRequest(o, &q) && q.id == id);
	OpenBGI_Answer(o, id, OPENBGI_IDOK);
	OpenBGI_Delete(o);
}

static void TestBox(void)
{
	OpenBGI_t* o = New(0, 400, 300, PM_RGB32);
	OpenBGI_Rect_t r;
	uint32_t id, v[3];
	int answer = -1;
	printf("box\n");
	if(!o)
		return;
	Frame(o);
	CHECK(gWin.px && !Found(&r));
	id = Request_Box(o, "t", "c", OPENBGI_MB_OKCANCEL | OPENBGI_MB_DEFBUTTON2, REQUEST_TREAT_NONE);
	Frame(o);
	CHECK(Found(&r) && r.w == 220 + 2 * UI_BORDER && r.x == (400 - r.w) / 2 && r.y == (300 - r.h) / 2);
	CHECK(WinPx(r.x + 10, r.y + r.h / 2) == UI_FACE && PictureBlack(o));
	CHECK(!gFrame.textInput && gFrame.idleMs >= 10);
	while(MsgQueue_Get(o, v)) // (the focus the engine got when made)
	{
	}
	OpenBGI_Key(o, 'A', 1, 0); // the box's: no event for the game, the key state still follows
	CHECK(!MsgQueue_Get(o, v) && Input_KeyDown(o, 'A'));
	OpenBGI_Key(o, 'A', 0, 0);
	CHECK(!Input_KeyDown(o, 'A'));
	OpenBGI_Key(o, 0x0d, 1, 0); // the default button: Cancel
	CHECK(!Inframe_Up(o) && Request_Take(o, id, &answer) && answer == OPENBGI_IDCANCEL);
	OpenBGI_Key(o, 0x0d, 0, 0); // (the release is the game's again)
	OpenBGI_Key(o, 'B', 1, 0);  // and so are the keys
	CHECK(MsgQueue_Get(o, v));
	OpenBGI_Key(o, 'B', 0, 0);
	Frame(o);
	CHECK(!Found(&r) && gFrame.pixels == o->screen.mem);
	OpenBGI_Delete(o);
}

static void TestInput(void)
{
	OpenBGI_t* o = New(0, 400, 300, PM_RGB32);
	OpenBGI_Request_t q = {.size = sizeof q};
	OpenBGI_Rect_t r;
	char out[0x100];
	uint32_t id;
	int answer = -1;
	printf("input\n");
	if(!o)
		return;
	memset(&q, 0, sizeof q);
	q.size = sizeof q;
	q.kind = OPENBGI_ASK_INPUT;
	q.u.input.initial = "ab";
	q.u.input.maxLen = 10;
	q.u.input.out = out;
	strcpy(out, "untouched");
	id = Request_Post(o, &q, REQUEST_TREAT_NONE);
	Frame(o);
	CHECK(Found(&r) && r.w == 344 + 2 * UI_BORDER && r.h == 82 + UI_CAPTION_H + 2 * UI_BORDER);
	CHECK(gFrame.textInput && gFrame.textRect.x == r.x + UI_BORDER + UI_MARGIN && gFrame.textRect.y == r.y + UI_BORDER + UI_CAPTION_H + UI_MARGIN);
	CHECK(gFrame.textRect.w == 320 && gFrame.textRect.h == UI_FIELD_H);
	OpenBGI_Key(o, 'C', 1, 0); // the key, then its text, as a window gets them
	OpenBGI_Text(o, "c\xe3\x81\x82");
	OpenBGI_Key(o, 'C', 0, 0);
	Key(o, 0x0d);
	CHECK(Request_Take(o, id, &answer) && answer == 1 && strcmp(out, "abc\x82\xa0") == 0);
	Frame(o);
	CHECK(!gFrame.textInput);

	// Escape, the Cancel button, the close box: 0, the buffer untouched
	strcpy(out, "untouched");
	id = Request_Post(o, &q, REQUEST_TREAT_NONE);
	Frame(o);
	OpenBGI_Text(o, "x");
	Key(o, 0x1b);
	CHECK(Request_Take(o, id, &answer) && answer == 0 && strcmp(out, "untouched") == 0);
	id = Request_Post(o, &q, REQUEST_TREAT_NONE);
	Frame(o);
	Found(&r);
	Click(o, r.x + UI_BORDER + 344 - UI_MARGIN - UI_BTN_W + 10, r.y + UI_BORDER + UI_CAPTION_H + 46 + 10);
	CHECK(Request_Take(o, id, &answer) && answer == 0);
	id = Request_Post(o, &q, REQUEST_TREAT_NONE);
	Frame(o);
	Found(&r);
	Click(o, r.x + UI_BORDER + 344 - 10, r.y + UI_BORDER + UI_CAPTION_H / 2);
	CHECK(Request_Take(o, id, &answer) && answer == 0 && strcmp(out, "untouched") == 0);
	// the OK button
	id = Request_Post(o, &q, REQUEST_TREAT_NONE);
	Frame(o);
	Found(&r);
	Click(o, r.x + UI_BORDER + 344 - UI_MARGIN - 2 * UI_BTN_W - 8 + 10, r.y + UI_BORDER + UI_CAPTION_H + 46 + 10);
	CHECK(Request_Take(o, id, &answer) && answer == 1 && strcmp(out, "ab") == 0);
	OpenBGI_Delete(o);
}

static void TestProfile(void)
{
	OpenBGI_t* o = New(0, 640, 480, PM_RGB32);
	OpenBGI_Request_t q = {.size = sizeof q};
	char names[4][0x100];
	int32_t month = 3, day = 4;
	uint32_t id;
	int answer = -1;
	printf("profile\n");
	if(!o)
		return;
	memset(&q, 0, sizeof q);
	q.size = sizeof q;
	q.kind = OPENBGI_ASK_PROFILE;
	strcpy(names[0], "a");
	names[1][0] = names[2][0] = names[3][0] = 0;
	q.u.profile.surname = names[0];
	q.u.profile.givenName = names[1];
	q.u.profile.nickname = names[2];
	q.u.profile.pronoun = names[3];
	q.u.profile.month = &month;
	q.u.profile.day = &day;
	id = Request_Post(o, &q, REQUEST_TREAT_NONE);
	Frame(o);
	CHECK(Inframe_Up(o) && !gFrame.textInput); // the focus on OK
	Key(o, 0x0d);
	CHECK(Inframe_Up(o) && Request_Pending(o) == 1 && o->screen.overlay[SCREEN_OVERLAY_DIALOG + 1].pix); // the complaint over it
	Frame(o);
	Key(o, 0x09); // the complaint's: nothing to tab to but its button
	Key(o, 0x0d); // closes the complaint
	CHECK(Inframe_Up(o) && !o->screen.overlay[SCREEN_OVERLAY_DIALOG + 1].pix && Request_Pending(o) == 1);
	Key(o, 0x09); // the dialog's again: the surname, selected
	Frame(o);
	CHECK(gFrame.textInput);
	OpenBGI_Text(o, "\xe3\x81\x82");
	Key(o, 0x0d);
	CHECK(Request_Take(o, id, &answer) && answer == 1 && strcmp(names[0], "\x82\xa0") == 0 && month == 3 && day == 4);
	OpenBGI_Delete(o);
}

static void TestPointer(void)
{
	OpenBGI_t* o = New(0, 400, 300, PM_RGB32);
	OpenBGI_Rect_t r, m;
	uint32_t id;
	int answer = -1;
	printf("pointer\n");
	if(!o)
		return;
	id = Request_Box(o, "t", "c", OPENBGI_MB_OK, REQUEST_TREAT_NONE);
	Frame(o);
	Found(&r);
	OpenBGI_Pointer(o, r.x + 20, r.y + 8);
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 1, r.x + 20, r.y + 8, 0); // the caption
	OpenBGI_Pointer(o, r.x + 50, r.y + 28);
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 0, r.x + 50, r.y + 28, 0);
	Frame(o);
	CHECK(Found(&m) && m.x == r.x + 30 && m.y == r.y + 20 && m.w == r.w && m.h == r.h && Inframe_Up(o));
	Screen_Setup(o, 300, 200, PM_RGB32); // a new picture size: centred on it again
	Frame(o);
	CHECK(gWin.w == 300 && Found(&m) && m.x == (300 - r.w) / 2 && m.y == (200 - r.h) / 2);
	Click(o, m.x + m.w / 2, m.y + m.h - UI_BORDER - UI_MARGIN - UI_BTN_H / 2); // OK, centred at the bottom
	CHECK(Request_Take(o, id, &answer) && answer == OPENBGI_IDOK);
	Frame(o);
	CHECK(!Found(&m));

	// a 16-bit picture
	Screen_Setup(o, 400, 300, PM_RGB16);
	id = Request_Box(o, "t", "c", OPENBGI_MB_OK, REQUEST_TREAT_NONE);
	Frame(o);
	CHECK(gWin.bpp == 16 && Found(&r) && WinPx(r.x + 10, r.y + r.h / 2) == 0xf7f7f7); // UI_FACE in 5-5-5
	// Escape: the close box's button, OK
	Key(o, 0x1b);
	CHECK(Request_Take(o, id, &answer) && answer == OPENBGI_IDOK);
	OpenBGI_Delete(o);
}

static void TestStop(void)
{
	OpenBGI_t* o = New(0, 400, 300, PM_RGB32);
	OpenBGI_Request_t q = {.size = sizeof q};
	OpenBGI_Rect_t r;
	uint32_t id;
	printf("stop\n");
	if(!o)
		return;
	Request_Box(o, "t", "c", OPENBGI_MB_OK, REQUEST_TREAT_NONE);
	Frame(o);
	CHECK(Inframe_Up(o) && Found(&r));
	OpenBGI_Stop(o);
	CHECK(!Inframe_Up(o) && Request_Pending(o) == 0 && !o->screen.overlay[SCREEN_OVERLAY_DIALOG].pix);
	id = Request_Box(o, "after", "c", OPENBGI_MB_OK, REQUEST_TREAT_NONE);
	CHECK(OpenBGI_NextRequest(o, &q) && q.id == id); // the host's
	OpenBGI_Delete(o);
}

// an engine with the test's renderer and an entry's limits for a 400 x 300 picture
static OpenBGI_t* NewWithFont(const OpenBGI_TextEntry_t* entry)
{
	OpenBGI_t* o;
	gHost.fontRenderer = &kRenderer;
	gHost.textEntry = entry;
	o = New(0, 400, 300, PM_RGB32);
	gHost.fontRenderer = NULL;
	gHost.textEntry = NULL;
	if(o)
	{
		o->display.sizeIndex = 0;
		o->display.screenWidths[0] = 400;
		o->display.screenHeights[0] = 300;
	}
	return o;
}

/* A setup at (100, 50), 200 x 30, size 20 (characters 10 pixels wide, ink
 * in columns 2 .. 7 and rows 5 .. 14 of their cell), red; the window
 * after a frame. */
static void TestEntry(void)
{
	OpenBGI_t* o = NewWithFont(NULL);
	char buf[EDIT_TEXT_MAX];
	printf("entry\n");
	if(!o)
		return;
	CHECK(Edit_GetText(o, buf) == (int)strlen(MSG_EDIT_DEFAULT_TEXT) && strcmp(buf, MSG_EDIT_DEFAULT_TEXT) == 0); // the control's own text before a setup
	Frame(o);
	Events(o);
	Edit_SetText(o, "ab");
	Edit_SetColour(o, 0xff0000);
	CHECK(Edit_Setup(o, 100, 50, 200, 30, 0, 20, 10, 1) == 0);
	CHECK(Edit_Setup(o, 100, 50, 4, 30, 0, 20, 10, 1) == 1 && Edit_Setup(o, 100, 50, 200, 30, 0, 70, 10, 1) == 3);
	CHECK(Edit_Setup(o, 100, 50, 200, 30, 0, 20, 10, 1) == 0);
	Frame(o);
	// "ab" selected: 20 pixels centred at x 90 of the entry, on the highlight, at the top
	CHECK(WinPx(100 + 90, 50) == UI_SELECT_BG && WinPx(100 + 109, 50 + 19) == UI_SELECT_BG && WinPx(100 + 110, 50) == 0);
	CHECK(WinPx(100 + 92, 50 + 5) == UI_SELECT_TEXT && WinPx(100 + 97, 50 + 14) == UI_SELECT_TEXT && WinPx(100 + 90, 50 + 20) == 0);
	CHECK(PictureBlack(o) && gFrame.textInput && gFrame.textRect.x == 100 && gFrame.textRect.y == 50 && gFrame.textRect.w == 200 &&
		gFrame.textRect.h == 30);

	// the pointer over it; Alt+F4 is the window's (the control's default closes the window)
	CHECK(Edit_Over(o, 100, 50) && Edit_Over(o, 299, 79) && !Edit_Over(o, 300, 50) && !Edit_Over(o, 100, 80));
	CHECK(!Edit_Key(o, 0x73, 1, OPENBGI_KEY_SYSTEM) && !Edit_Key(o, 0x73, 0, OPENBGI_KEY_SYSTEM) && Edit_Key(o, 0x10, 0, 0));

	// the keys and the text are the entry's: typing replaces the selection, the caret after it
	OpenBGI_Key(o, 'C', 1, 0);
	OpenBGI_Text(o, "c");
	OpenBGI_Key(o, 'C', 0, 0);
	CHECK(Events(o) == 0 && !Input_KeyDown(o, 'C') && Edit_GetText(o, buf) == 1 && strcmp(buf, "c") == 0);
	Frame(o);
	CHECK(WinPx(100 + 97, 50 + 5) == 0xff0000 && WinPx(100 + 96, 50 + 5) == 0 && WinPx(100 + 105, 50) == 0xff0000 && WinPx(100 + 105, 50 + 19) == 0xff0000);
	CHECK(WinPx(100 + 105, 50 + 20) == 0 && WinPx(100 + 90, 50) == 0); // no highlight
	Key(o, 0x08);
	OpenBGI_Text(o, "\xe3\x81\x82\xe3\x81\x84"); // あい: Shift-JIS, 20 pixels each
	CHECK(Edit_GetText(o, buf) == 4 && strcmp(buf, "\x82\xa0\x82\xa2") == 0);
	OpenBGI_Text(o, "abcdefg"); // the limit: 10 bytes
	CHECK(Edit_GetText(o, buf) == 10 && strcmp(buf, "\x82\xa0\x82\xa2"
													"abcdef") == 0);
	Key(o, 0x24); // Home
	Key(o, 0x2e); // Delete: あ
	CHECK(Edit_GetText(o, buf) == 8);
	Edit_RejectAscii(o, 1);
	OpenBGI_Text(o, "z");
	CHECK(Edit_GetText(o, buf) == 8);
	Edit_RejectAscii(o, 0);

	// Enter selects and gives the focus back; the selection still shows, the next key is the game's
	OpenBGI_Key(o, 0x0d, 1, 0);
	CHECK(Events(o) == 0);
	OpenBGI_Key(o, 0x0d, 0, 0);
	Frame(o);
	CHECK(!gFrame.textInput && WinPx(100 + (200 - 80) / 2, 50) == UI_SELECT_BG);
	OpenBGI_Text(o, "x");
	CHECK(Edit_GetText(o, buf) == 8);
	Key(o, 'B');
	CHECK(Events(o) > 0);

	// a click on it: the focus and the caret at the nearest boundary; the game sees neither the press nor its release
	Click(o, 100 + 60 + 28, 50 + 10); // nearest the boundary after "a" (at 60 + 30; the one before it at 60 + 20)
	CHECK(Events(o) == 0);
	OpenBGI_Text(o, "Q");
	CHECK(Edit_GetText(o, buf) == 9 && strcmp(buf, "\x82\xa2"
												   "aQbcdef") == 0);
	Click(o, 10, 10); // elsewhere: the game's, the entry's focus gone
	CHECK(Events(o) > 0);
	OpenBGI_Text(o, "R");
	CHECK(Edit_GetText(o, buf) == 9);
	Frame(o);
	CHECK(!gFrame.textInput);

	// a text wider than the entry is scrolled to keep the caret in view
	Edit_SetText(o, "abcdefgh");
	CHECK(Edit_Setup(o, 100, 50, 40, 30, 0, 20, 10, 1) == 0);
	Frame(o);
	CHECK(WinPx(100 + 31, 50 + 5) == UI_SELECT_TEXT && WinPx(100 + 36, 50 + 5) == UI_SELECT_TEXT); // h at 70 - 41
	Key(o, 0x24);
	Frame(o);
	CHECK(WinPx(100 + 2, 50 + 5) == 0xff0000 && WinPx(100, 50) == 0xff0000); // a at 0, the caret before it
	Key(o, 0x23);
	Frame(o);
	CHECK(WinPx(100 + 39, 50) == 0xff0000 && WinPx(100 + 31, 50 + 5) == 0xff0000); // the caret at the right edge

	// a dialog over it takes the keys; hidden it takes nothing
	Request_Box(o, "t", "c", OPENBGI_MB_OK, REQUEST_TREAT_NONE);
	Frame(o);
	CHECK(Inframe_Up(o) && !gFrame.textInput);
	OpenBGI_Text(o, "S");
	Key(o, 0x0d);
	CHECK(!Inframe_Up(o) && Edit_GetText(o, buf) == 8);
	Edit_Show(o, 0);
	Frame(o);
	CHECK(!o->screen.overlay[SCREEN_OVERLAY_EDIT].pix && WinPx(100 + 39, 50) == 0 && !gFrame.textInput);
	Events(o);
	Key(o, 'D');
	CHECK(Events(o) > 0);
	Edit_Destroy(o);
	OpenBGI_Delete(o);
}

/* The builds from 1.69/472 on, with the engine's entry: made by a setup,
 * "B0 2A", Tab and "B0 28", "B0 21", several lines. */
static void TestBuilds(void)
{
	OpenBGI_t* o = NewWithFont(NULL);
	char buf[EDIT_TEXT_MAX];
	printf("builds\n");
	if(!o)
		return;
	o->engine.profile = Engine_ProfileOfGen(GEN_1_636);
	Frame(o);
	Events(o);

	// no control before a setup: no text, nothing shown
	strcpy(buf, "kept");
	Edit_Show(o, 1);
	Frame(o);
	CHECK(Edit_GetText(o, buf) == -1 && strcmp(buf, "kept") == 0 && !Edit_Shown(o) && !o->screen.overlay[SCREEN_OVERLAY_EDIT].pix);

	// right-aligned ("B0 2A" 2): "ab" at 180 .. 199 of the entry, selected
	CHECK(Edit_SetAlign(o, 3) == 0 && Edit_SetAlign(o, 2) == 1);
	Edit_SetText(o, "ab");
	Edit_SetColour(o, 0xff0000);
	CHECK(Edit_Setup(o, 100, 50, 200, 30, 0, 20, 10, 1) == 0 && Edit_Shown(o));
	Frame(o);
	CHECK(WinPx(100 + 180, 50) == UI_SELECT_BG && WinPx(100 + 199, 50 + 19) == UI_SELECT_BG && WinPx(100 + 179, 50) == 0);

	// Tab hides it (its release is the game's); shown again it has no focus until a click
	OpenBGI_Key(o, 0x09, 1, 0);
	CHECK(Events(o) == 0 && !Edit_Shown(o));
	OpenBGI_Key(o, 0x09, 0, 0);
	Frame(o);
	CHECK(!o->screen.overlay[SCREEN_OVERLAY_EDIT].pix && !gFrame.textInput);
	Edit_Show(o, 1);
	Frame(o);
	CHECK(Edit_Shown(o) && !gFrame.textInput && WinPx(100 + 180, 50) == UI_SELECT_BG);
	Click(o, 100 + 199, 50 + 10);
	Frame(o);
	CHECK(gFrame.textInput);

	// Enter selects and gives the focus back; with "B0 28" it hides the entry too
	Key(o, 0x0d);
	Frame(o);
	CHECK(Edit_Shown(o) && !gFrame.textInput);
	Click(o, 100 + 199, 50 + 10);
	Edit_SetCloseOnEnter(o, 1);
	OpenBGI_Key(o, 0x0d, 1, 0);
	CHECK(!Edit_Shown(o) && Edit_GetText(o, buf) == 2);
	OpenBGI_Key(o, 0x0d, 0, 0);
	Edit_SetCloseOnEnter(o, 0);

	// "B0 21": the control goes
	CHECK(Edit_Remove(o) == 0 && Edit_GetText(o, buf) == -1);
	Edit_Show(o, 1);
	Frame(o);
	CHECK(!Edit_Shown(o) && !o->screen.overlay[SCREEN_OVERLAY_EDIT].pix);

	// two lines of 60 pixels (40 / 20), left-aligned: "abc " and "def", wrapped at the space
	Edit_SetAlign(o, 0);
	Edit_SetText(o, "");
	CHECK(Edit_Setup(o, 100, 50, 60, 40, 0, 20, 0x100, 1) == 0);
	OpenBGI_Text(o, "abc def");
	Frame(o);
	CHECK(WinPx(100 + 2, 50 + 5) == 0xff0000 && WinPx(100 + 2, 50 + 20 + 5) == 0xff0000 && WinPx(100 + 22, 50 + 20 + 5) == 0xff0000);
	CHECK(WinPx(100 + 30, 50 + 20) == 0xff0000 && WinPx(100 + 30, 50 + 19) == 0); // the caret after "def"
	// no more text than two lines hold: "ghi" fits on the second line, "j" would need a third
	OpenBGI_Text(o, "ghij");
	CHECK(Edit_GetText(o, buf) == 10 && strcmp(buf, "abc defghi") == 0);
	OpenBGI_Text(o, "k");
	CHECK(Edit_GetText(o, buf) == 10);
	// Up: to the end of the first line; a character there would need a third line; Home, then BackSpace joins the lines
	Key(o, 0x26);
	OpenBGI_Text(o, "Z");
	CHECK(Edit_GetText(o, buf) == 10);
	Frame(o);
	CHECK(WinPx(100 + 40, 50) == 0xff0000 && WinPx(100 + 40, 50 + 19) == 0xff0000); // the caret at the end of "abc "
	Key(o, 0x28);                                                                   // Down: back to the second line
	Key(o, 0x24);                                                                   // Home: its start
	Key(o, 0x08);
	CHECK(Edit_GetText(o, buf) == 9 && strcmp(buf, "abcdefghi") == 0);
	Frame(o);
	CHECK(WinPx(100 + 52, 50 + 5) == 0xff0000 && WinPx(100 + 2, 50 + 20 + 5) == 0xff0000); // "abcdef", "ghi"
	// a click on the second line: the caret at the nearest boundary there
	Click(o, 100 + 14, 50 + 25);
	OpenBGI_Text(o, "Y");
	CHECK(Edit_GetText(o, buf) == 10 && strcmp(buf, "abcdefgYhi") == 0);
	// double-byte characters break anywhere: "abcdef" and "gYhi" + あ (20 pixels: 60) fit, い does not
	Key(o, 0x23);
	OpenBGI_Text(o, "\xe3\x81\x82\xe3\x81\x84");
	CHECK(Edit_GetText(o, buf) == 12 && strcmp(buf, "abcdefgYhi\x82\xa0") == 0);

	// 1.69/472: left-aligned, Tab selects and gives the focus back, the entry stays
	o->engine.profile = Engine_ProfileOfGen(GEN_1_69_472);
	Edit_SetText(o, "ab");
	CHECK(Edit_Setup(o, 100, 50, 200, 30, 0, 20, 10, 1) == 0);
	Key(o, 0x09);
	Frame(o);
	CHECK(Edit_Shown(o) && !gFrame.textInput && WinPx(100, 50) == UI_SELECT_BG && WinPx(100 + 20, 50) == 0);
	Edit_Destroy(o);
	OpenBGI_Delete(o);
}

// ---- a host's entry, which notes what it is asked ----------------------------------------

static char gCalls[256];

static void Note(const char* what)
{
	strncat(gCalls, what, sizeof gCalls - strlen(gCalls) - 1);
}

static void HShow(void* user, int show)
{
	Note(show ? "show " : "hide ");
}

static void HMove(void* user, int x, int y, int w, int h)
{
	char b[48];
	snprintf(b, sizeof b, "move %d,%d,%d,%d ", x, y, w, h);
	Note(b);
}

static void HSetFont(void* user, const char* face, int height, int width)
{
	char b[48];
	snprintf(b, sizeof b, "font %d,%d ", height, width);
	Note(b);
}

static void HLimit(void* user, int maxBytes)
{
	char b[24];
	snprintf(b, sizeof b, "limit %d ", maxBytes);
	Note(b);
}

static void HSetText(void* user, const char* text)
{
	Note("text ");
	Note(text);
	Note(" ");
}

static void HSelectAll(void* user)
{
	Note("all ");
}

static void HFocus(void* user, int toEntry)
{
	Note(toEntry ? "focus " : "unfocus ");
}

static int HGetText(void* user, char* buf, size_t n)
{
	snprintf(buf, n, "%s", "typed");
	return 5;
}

static void HSetColour(void* user, uint32_t rgb)
{
	Note(rgb == 0x00ff00 ? "green " : "colour ");
}

static void HSetStyle(void* user, int flags)
{
	char b[24];
	snprintf(b, sizeof b, "style %d ", flags);
	Note(b);
}

static int HShown(void* user)
{
	return 1;
}

static const OpenBGI_TextEntry_t kEntry = {sizeof(OpenBGI_TextEntry_t), HShow, HMove, HSetFont, HLimit, HSetText, HSelectAll, HFocus, HGetText, HSetColour, NULL, NULL,
	NULL, HSetStyle, HShown};

static void TestHost(void)
{
	OpenBGI_t* o = NewWithFont(&kEntry);
	char buf[EDIT_TEXT_MAX];
	printf("host\n");
	if(!o)
		return;
	gCalls[0] = 0;
	Edit_SetText(o, "ab");
	Edit_SetColour(o, 0x00ff00);
	o->edit.editWidthPct = 50;
	CHECK(Edit_Setup(o, 1, 2, 30, 40, 0, 20, 7, 1) == 0);
	CHECK(strcmp(gCalls, "green style 1 show move 1,2,30,40 font 20,5 limit 7 text ab all focus ") == 0); // centred, one line
	CHECK(Edit_GetText(o, buf) == 5 && strcmp(buf, "typed") == 0 && Edit_Shown(o) == 1);
	Frame(o);
	CHECK(!o->screen.overlay[SCREEN_OVERLAY_EDIT].pix && !gFrame.textInput);
	Events(o);
	Key(o, 'A'); // the game's: the host's entry has its own keys
	CHECK(Events(o) > 0);
	OpenBGI_Key(o, 'E', 1, OPENBGI_KEY_TAKEN); // a key the host's entry took: the key state only
	CHECK(Events(o) == 0 && Input_KeyDown(o, 'E'));
	OpenBGI_Key(o, 'E', 0, OPENBGI_KEY_TAKEN);
	CHECK(!Input_KeyDown(o, 'E') && Events(o) == 0);

	// 1.636: no control before a setup; the style of the setup's rectangle, "B0 28" at once, "B0 21"
	o->engine.profile = Engine_ProfileOfGen(GEN_1_636);
	Edit_Remove(o); // (the control of the setup above)
	gCalls[0] = 0;
	strcpy(buf, "kept");
	Edit_Show(o, 1);
	CHECK(Edit_GetText(o, buf) == -1 && strcmp(buf, "kept") == 0 && Edit_Shown(o) == 0 && gCalls[0] == 0);
	CHECK(Edit_SetAlign(o, 3) == 0 && Edit_SetAlign(o, 2) == 1);
	CHECK(Edit_Setup(o, 1, 2, 30, 40, 0, 20, 7, 0) == 0);
	CHECK(strncmp(gCalls, "style 46 show ", 14) == 0); // right, several lines, Tab hides, the IME status window
	gCalls[0] = 0;
	Edit_SetCloseOnEnter(o, 1);
	CHECK(strcmp(gCalls, "style 62 ") == 0); // and Enter hides
	gCalls[0] = 0;
	CHECK(Edit_Remove(o) == 0 && strcmp(gCalls, "hide ") == 0 && Edit_GetText(o, buf) == -1 && Edit_Shown(o) == 0);
	Edit_Show(o, 1);
	CHECK(strcmp(gCalls, "hide ") == 0);
	Edit_Destroy(o);
	OpenBGI_Delete(o);
}

// ---- the child windows -------------------------------------------------------------

#define CW       64 // the test's windows' client size
#define CH       48
#define FW       (CW + 2 * UI_BORDER) // and the size drawn with the border and the caption bar
#define FH       (CH + UI_CAPTION_H + 2 * UI_BORDER)
#define GREY     0x808080  // an inactive window's caption bar at its left end
#define BLUE     0x0a246a  // the active window's
#define CLIENT_X UI_BORDER // the client area's top left in a window drawn at (0, 0)
#define CLIENT_Y (UI_BORDER + UI_CAPTION_H)

// 0x00RRGGBB as a 16-bit window shows it (5-5-5, widened again)
static uint32_t Px16(uint32_t rgb)
{
	uint32_t r = (rgb >> 19) & 31, g = (rgb >> 11) & 31, b = (rgb >> 3) & 31;
	return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

/* The windows the engine draws: made hidden, shown with a caption bar
 * and the back buffer in their client area over the picture (which stays
 * untouched), the sizes and the slots checked; a press makes a window the
 * active one (its caption blue, the main window inactive) and is not the
 * game's, a press on the picture ends that; Ctrl+C is the active window's,
 * its other keys and keys without Ctrl the game's; the window shown or
 * clicked last is over the others; the caption drags a window; a window
 * that does not fit is drawn moved into the picture, where it was put
 * staying its position; the host's focus while a window is active; hiding
 * or closing the active window makes the main window active again; a
 * press on a window takes the focus from the text entry; a new picture
 * size and a 16-bit picture. */
static void TestChildren(void)
{
	OpenBGI_t* o;
	OpenBGI_Rect_t r;
	uint32_t h1, h2, h[8];
	int32_t pos[2];
	int i;
	printf("children\n");
	o = NewWithFont(NULL);
	if(!o)
		return;
	Child_InitTable(o);
	OpenBGI_Focus(o, 1);
	CHECK(Window_IsActive(o));

	// made hidden; the sizes
	CHECK(Child_Create(o, &h1, "a", 10, 20, 0x1f, CH) == 0x80000001 && Child_Create(o, &h1, "a", 10, 20, CW, 0x401) == 0x80000001);
	CHECK(Child_Create(o, &h1, "one", 10, 20, CW, CH) == 0 && h1 == 0xff000000u);
	Frame(o);
	CHECK(!o->screen.overlay[SCREEN_OVERLAY_CHILD].pix && !Found(&r));
	CHECK(Child_Show(o, h1, 1) == 1 && Child_Show(o, 0xff000008u, 1) == 0 && Child_Show(o, 0x01000000u, 1) == 0);
	Frame(o);
	CHECK(Found(&r) && r.x == 10 && r.y == 20 && r.w == FW && r.h == FH);
	CHECK(WinPx(10 + UI_BORDER, 20 + UI_BORDER) == GREY && WinPx(10 + CLIENT_X, 20 + CLIENT_Y) == 0); // inactive; the client black
	CHECK(PictureBlack(o));
	CHECK(Child_Fill(o, h1, 0xff0000) == 1);
	Frame(o);
	CHECK(WinPx(10 + CLIENT_X, 20 + CLIENT_Y) == 0xff0000 && WinPx(10 + CLIENT_X + CW - 1, 20 + CLIENT_Y + CH - 1) == 0xff0000);
	CHECK(WinPx(10 + CLIENT_X + CW, 20 + CLIENT_Y) != 0xff0000 && PictureBlack(o)); // (the border)
	CHECK(Child_GetPos(o, pos, h1) == 1 && pos[0] == 10 && pos[1] == 20);

	// a press: the window's, active; on the picture: the game's, none active
	Events(o);
	Click(o, 10 + 30, 20 + CLIENT_Y + 10);
	CHECK(Events(o) == 0 && !Window_IsActive(o));
	Frame(o);
	CHECK(WinPx(10 + UI_BORDER, 20 + UI_BORDER) == BLUE);
	Click(o, 300, 250);
	CHECK(Events(o) > 0 && Window_IsActive(o));
	Frame(o);
	CHECK(WinPx(10 + UI_BORDER, 20 + UI_BORDER) == GREY);

	// Ctrl+C
	Click(o, 10 + 30, 20 + CLIENT_Y + 10);
	CHECK(Child_SetCopyText(o, h1, "copied") == 1 && Child_SetCopyText(o, 0xff000009u, "x") == 0);
	OpenBGI_Key(o, 0x11, 1, 0);
	Events(o);
	OpenBGI_Key(o, 'C', 1, 0);
	CHECK(Events(o) == 0); // the window's
	OpenBGI_Key(o, 'C', 0, 0);
	OpenBGI_Key(o, 'X', 1, 0);
	CHECK(Events(o) > 0); // the game's
	OpenBGI_Key(o, 'X', 0, 0);
	OpenBGI_Key(o, 0x11, 0, 0);
	Events(o);
	OpenBGI_Key(o, 'C', 1, 0);
	CHECK(Events(o) > 0); // without Ctrl
	OpenBGI_Key(o, 'C', 0, 0);
	Click(o, 300, 250);
	OpenBGI_Key(o, 0x11, 1, 0);
	Events(o);
	OpenBGI_Key(o, 'C', 1, 0);
	CHECK(Events(o) > 0); // no window active
	OpenBGI_Key(o, 'C', 0, 0);
	OpenBGI_Key(o, 0x11, 0, 0);

	// the stacking: the window shown last over the other, a press brings one up
	CHECK(Child_Create(o, &h2, "two", 40, 40, CW, CH) == 0 && h2 == 0xff000001u);
	Child_Fill(o, h2, 0x00ff00);
	Child_Show(o, h2, 1);
	Frame(o);
	CHECK(WinPx(60, 40 + CLIENT_Y + 5) == 0x00ff00 && WinPx(14, 20 + CLIENT_Y + 2) == 0xff0000);
	Click(o, 14, 20 + CLIENT_Y + 2);
	Frame(o);
	CHECK(WinPx(60, 40 + CLIENT_Y + 5) == 0xff0000 && WinPx(40 + UI_BORDER + CW - 1, 40 + CLIENT_Y + 5) == 0x00ff00);
	CHECK(WinPx(10 + UI_BORDER, 20 + UI_BORDER) == BLUE && WinPx(10 + UI_BORDER + CW - 1, 20 + UI_BORDER) == 0xa6caf0);
	CHECK(WinPx(40 + UI_BORDER + CW - 1, 40 + UI_BORDER) == 0xc0c0c0); // (the right ends of the caption bars)

	// a drag of the caption; the moves and the presses are not the game's
	Events(o);
	OpenBGI_Pointer(o, 30, 20 + UI_BORDER + 5);
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 1, 30, 20 + UI_BORDER + 5, 0);
	OpenBGI_Pointer(o, 60, 50 + UI_BORDER + 5);
	OpenBGI_Button(o, OPENBGI_BUTTON_LEFT, 0, 60, 50 + UI_BORDER + 5, 0);
	CHECK(Events(o) == 0 && Child_GetPos(o, pos, h1) && pos[0] == 40 && pos[1] == 50);
	Frame(o);
	CHECK(WinPx(40 + UI_BORDER, 50 + UI_BORDER) == BLUE && WinPx(40 + CLIENT_X, 50 + CLIENT_Y) == 0xff0000);
	OpenBGI_Pointer(o, 300, 250); // released: no more dragged
	CHECK(Child_GetPos(o, pos, h1) && pos[0] == 40 && pos[1] == 50);

	// a window that does not fit: drawn inside the picture, its position as put
	CHECK(Child_Move(o, h1, 380, 290) == 1);
	Frame(o);
	CHECK(WinPx(400 - FW + UI_BORDER, 300 - FH + UI_BORDER) == BLUE && WinPx(400 - FW + CLIENT_X, 300 - FH + CLIENT_Y) == 0xff0000);
	CHECK(Child_GetPos(o, pos, h1) && pos[0] == 380 && pos[1] == 290);
	Child_Move(o, h1, -50, 10);
	Frame(o);
	CHECK(WinPx(UI_BORDER, 10 + UI_BORDER) == BLUE);

	// the host's focus while a window is active: the main window stays inactive until a press on the picture
	OpenBGI_Focus(o, 0);
	OpenBGI_Focus(o, 1);
	CHECK(!Window_IsActive(o));
	Click(o, 300, 250);
	CHECK(Window_IsActive(o));
	OpenBGI_Focus(o, 0); // a press while the host's window has no focus: inactive it stays
	Click(o, UI_BORDER + 5, 10 + CLIENT_Y + 5);
	Click(o, 300, 250);
	CHECK(!Window_IsActive(o));
	OpenBGI_Focus(o, 1);
	CHECK(Window_IsActive(o));

	// hiding or closing the active window: the main window active again
	Click(o, UI_BORDER + 5, 10 + CLIENT_Y + 5);
	CHECK(!Window_IsActive(o) && Child_Show(o, h1, 0) == 1 && Window_IsActive(o));
	Frame(o);
	CHECK(o->screen.overlay[SCREEN_OVERLAY_CHILD].pix && !o->screen.overlay[SCREEN_OVERLAY_CHILD + 1].pix); // "two" alone
	Child_Show(o, h1, 1);
	Click(o, UI_BORDER + 5, 10 + CLIENT_Y + 5);
	CHECK(!Window_IsActive(o) && Child_Destroy(o, h1) == 1 && Window_IsActive(o));
	CHECK(Child_Destroy(o, h1) == 1 && Child_Destroy(o, 0xff00000au) == 0); // (closed already; no slot)
	Frame(o);
	CHECK(!o->screen.overlay[SCREEN_OVERLAY_CHILD + 1].pix && WinPx(UI_BORDER, 10 + UI_BORDER) == 0);

	// the slots: the free one first, eight at most
	CHECK(Child_Create(o, &h[0], "re", 0, 0, CW, CH) == 0 && h[0] == 0xff000000u);
	for(i = 1; i < 7; i++)
		CHECK(Child_Create(o, &h[i], "more", 0, 0, CW, CH) == 0 && h[i] == (0xff000001u + (uint32_t)i));
	CHECK(Child_Create(o, &h[7], "full", 0, 0, CW, CH) == 0x80000002);
	for(i = 0; i < 7; i++)
		Child_Destroy(o, h[i]);

	// a press on a window takes the focus from the text entry
	CHECK(Edit_Setup(o, 200, 100, 150, 30, 0, 20, 10, 1) == 0);
	Events(o);
	Key(o, 'A');
	CHECK(Events(o) == 0); // the entry's
	Click(o, 60, 40 + CLIENT_Y + 5);
	Key(o, 'A');
	CHECK(Events(o) > 0); // the game's
	Edit_Destroy(o);

	// a new picture size: the windows placed in it again; a 16-bit picture
	Child_Move(o, h2, 350, 40);
	Frame(o);
	CHECK(WinPx(400 - FW + CLIENT_X, 40 + CLIENT_Y) == 0x00ff00);
	Screen_Setup(o, 300, 200, PM_RGB32);
	Frame(o);
	CHECK(gWin.w == 300 && WinPx(300 - FW + CLIENT_X, 40 + CLIENT_Y) == 0x00ff00);
	Screen_Setup(o, 400, 300, PM_RGB16);
	Frame(o);
	CHECK(gWin.bpp == 16 && WinPx(400 - FW + CLIENT_X, 40 + CLIENT_Y) == 0x00ff00 && WinPx(400 - FW + UI_BORDER, 40 + UI_BORDER) == Px16(BLUE));
	Child_DestroyAll(o);
	Frame(o);
	CHECK(!Found(&r));
	OpenBGI_Delete(o);
}

// a host's child windows, which note what they are asked
static int gChildWin[8];            // the host's windows: their addresses are the handles
static int gChildFail;              // `create` fails
static const uint8_t* gChildPixels; // what was presented last
static int gChildPitch;
static const char* gChildCopy; // the copy text

static int Slot(void* win)
{
	return (int)((int*)win - gChildWin);
}

static void* CCreate(void* user, int slot, const char* title, int x, int y, int w, int h)
{
	char s[96];
	snprintf(s, sizeof s, "create %d %s %d,%d %dx%d ", slot, title, x, y, w, h);
	Note(s);
	return gChildFail ? NULL : &gChildWin[slot];
}

static void CClose(void* user, void* win)
{
	char s[32];
	snprintf(s, sizeof s, "close %d ", Slot(win));
	Note(s);
}

static void CShow(void* user, void* win, int show)
{
	char s[32];
	snprintf(s, sizeof s, "show %d %d ", Slot(win), show);
	Note(s);
}

static void CSetTitle(void* user, void* win, const char* title)
{
	char s[96];
	snprintf(s, sizeof s, "title %d %s ", Slot(win), title);
	Note(s);
}

static void CMove(void* user, void* win, int x, int y)
{
	char s[48];
	snprintf(s, sizeof s, "move %d %d,%d ", Slot(win), x, y);
	Note(s);
}

static void CGetPos(void* user, void* win, int* x, int* y)
{
	*x = 111 + Slot(win);
	*y = 222;
}

static void CPresent(void* user, void* win, const uint8_t* pixels, int pitch, int w, int h, int bpp)
{
	char s[48];
	snprintf(s, sizeof s, "present %d %dx%d/%d ", Slot(win), w, h, bpp);
	Note(s);
	gChildPixels = pixels;
	gChildPitch = pitch;
}

static void CSetCopyText(void* user, void* win, const char* text)
{
	char s[96];
	snprintf(s, sizeof s, "copy %d %s ", Slot(win), text);
	Note(s);
	gChildCopy = text;
}

static const OpenBGI_ChildWindows_t kChildWindows = {sizeof(OpenBGI_ChildWindows_t), CCreate, CClose, CShow, CSetTitle, CMove, CGetPos, CPresent, CSetCopyText};

// pixel (x, y) of what the host's window got, 32-bit
static uint32_t ChildPx(int x, int y)
{
	return ((const uint32_t*)(gChildPixels + (size_t)y * (size_t)gChildPitch))[x] & 0xffffff;
}

/* With a host's child windows every call goes to them: the window made
 * with its picture (black), shown, moved, retitled, its copy text, its
 * position the host's, closed; a picture presented at every drawing; the
 * frame draws nothing and the presses are the game's; a window the host
 * could not make leaves its slot free; on a scaled display the host gets
 * the picture as the original's blit put it in the window; with
 * --child-windows=frame the engine draws them itself. */
static void TestChildHost(void)
{
	OpenBGI_t* o;
	uint32_t h1, h2;
	int32_t pos[2], pt[2];
	int dstH, y;
	printf("childhost\n");
	gHost.childWindows = &kChildWindows;
	o = NewWithFont(NULL);
	gHost.childWindows = NULL;
	if(!o)
		return;
	Gfx_Create(o, 0xffffffffu, 1);
	Gfx_SetupScreen(o, o->gfx.gfx, 400, 300, PM_RGB32, 0, 0, 0, 0);
	Child_InitTable(o);
	OpenBGI_Focus(o, 1);
	gCalls[0] = 0;
	CHECK(Child_Create(o, &h1, "one", 10, 20, CW, CH) == 0 && h1 == 0xff000000u);
	CHECK(strcmp(gCalls, "create 0 one 10,20 64x48 present 0 64x48/32 ") == 0);
	CHECK(gChildPixels == o->child.children[0].back.pixels && ChildPx(0, 0) == 0 && ChildPx(CW - 1, CH - 1) == 0);
	gCalls[0] = 0;
	Child_Show(o, h1, 1);
	Child_Fill(o, h1, 0xff0000);
	Child_Move(o, h1, 5, 6);
	Child_SetTitle(o, h1, "new");
	Child_SetCopyText(o, h1, "text");
	CHECK(strcmp(gCalls, "show 0 1 present 0 64x48/32 move 0 5,6 title 0 new copy 0 text ") == 0);
	CHECK(ChildPx(0, 0) == 0xff0000 && gChildCopy && strcmp(gChildCopy, "text") == 0);
	CHECK(Child_GetPos(o, pos, h1) && pos[0] == 111 && pos[1] == 222);
	Frame(o);
	CHECK(!o->screen.overlay[SCREEN_OVERLAY_CHILD].pix);
	Events(o);
	Click(o, 10, 30); // where the engine would draw it: the game's
	CHECK(Events(o) > 0 && Window_IsActive(o));
	gCalls[0] = 0;
	CHECK(Child_Destroy(o, h1) == 1 && strcmp(gCalls, "close 0 ") == 0);

	// a window the host could not make: the slot free again, the handle's calls harmless
	gChildFail = 1;
	gCalls[0] = 0;
	CHECK(Child_Create(o, &h1, "no", 0, 0, CW, CH) == 0 && h1 == 0xff000000u && strcmp(gCalls, "create 0 no 0,0 64x48 ") == 0);
	CHECK(Child_Fill(o, h1, 0x123456) == 1 && Child_Show(o, h1, 1) == 1 && Child_GetPos(o, pos, h1) && pos[0] == 0 && pos[1] == 0);
	gChildFail = 0;
	CHECK(Child_Create(o, &h2, "yes", 0, 0, CW, CH) == 0 && h2 == 0xff000000u);

	// a scaled display: the back buffer as the original's blit put it in the window
	Gfx_SetupScreen(o, o->gfx.gfx, 400, 300, PM_RGB32, 1, 400, 300, 3);
	Child_Fill(o, h2, 0xff0000);
	CHECK(gChildPixels && gChildPixels != o->child.children[0].back.pixels);
	Gfx_MapPoint(o->gfx.gfx, pt, 0, 0, 0);
	dstH = (int)(((uint32_t)Gfx_ScaleFactor(o->gfx.gfx) * (uint32_t)CH) >> 16);
	CHECK(pt[0] == 0 && pt[1] < 0 && pt[1] + dstH > 0 && pt[1] + dstH < CH);
	for(y = 0; y < CH; y++)
		if(ChildPx(CW / 2, y) != (y < pt[1] + dstH ? 0xff0000u : 0u))
			break;
	CHECK(y == CH);
	Gfx_SetupScreen(o, o->gfx.gfx, 400, 300, PM_RGB32, 0, 0, 0, 0);
	Child_Fill(o, h2, 0x00ff00);
	CHECK(gChildPixels == o->child.children[0].back.pixels && ChildPx(CW / 2, CH - 1) == 0x00ff00);

	// --child-windows=frame
	Child_DestroyAll(o);
	Child_InitTable(o);
	o->engine.optChildFrame = 1;
	gCalls[0] = 0;
	CHECK(Child_Create(o, &h1, "own", 10, 20, CW, CH) == 0 && Child_Show(o, h1, 1) && gCalls[0] == 0);
	Frame(o);
	CHECK(o->screen.overlay[SCREEN_OVERLAY_CHILD].pix != NULL);
	Child_DestroyAll(o);
	Gfx_Destroy(o);
	OpenBGI_Delete(o);
}

/* What the frame says of the window's repaint: black before the engine is
 * up and while a movie plays, nothing while presenting is off or the
 * display disabled, the picture otherwise. */
static void TestExpose(void)
{
	OpenBGI_t* o = New(0, 400, 300, PM_RGB32);
	printf("expose\n");
	if(!o)
		return;
	o->display.displayEnabled = 1;
	o->present.presentAllowed = 1;
	Frame(o);
	CHECK(gFrame.expose == OPENBGI_EXPOSE_BLACK);
	o->engine.engineUp = 1;
	Frame(o);
	CHECK(gFrame.expose == OPENBGI_EXPOSE_PICTURE);
	o->movie.moviePlaying = 1;
	Frame(o);
	CHECK(gFrame.expose == OPENBGI_EXPOSE_BLACK);
	o->movie.moviePlaying = 0;
	o->present.presentAllowed = 0;
	Frame(o);
	CHECK(gFrame.expose == OPENBGI_EXPOSE_NONE);
	o->present.presentAllowed = 1;
	o->display.displayEnabled = 0;
	Frame(o);
	CHECK(gFrame.expose == OPENBGI_EXPOSE_NONE);
	o->engine.engineUp = 0;
	OpenBGI_Delete(o);
}

int main(void)
{
	TestRoute();
	TestBox();
	TestInput();
	TestProfile();
	TestPointer();
	TestStop();
	TestEntry();
	TestBuilds();
	TestHost();
	TestChildren();
	TestChildHost();
	TestExpose();
	free(gWin.px);
	if(gFails)
	{
		printf("inframe: %d checks failed\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
