/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * inframe.c - the message boxes and the scripts' dialogs drawn in the
 *             frame (src/include/bgi/inframe.h); not part of the original engine
 *
 * A dialog that is up is a box of the dialog toolkit (src/include/bgi/ui.h) with
 * its border and caption bar, an overlay over the window's picture
 * (Screen_Overlay, SCREEN_OVERLAY_DIALOG), centred on it; a refusing
 * button's complaint is a second box in the slot over it, which takes the
 * input until it is closed.
 * The texts are drawn with the engine's typefaces in MS Gothic (or what
 * stands in for it) at 16 pixels, as the host's dialogs draw them.  The
 * boxes are painted again whenever an event changed them, and placed
 * anew when the picture's size changes.
 *
 * The display settings (Inframe_Settings, the settings key) are such a box
 * too, on every host that fits the window to its screen, whoever shows
 * the other dialogs there: it answers no request, and what is chosen in
 * it holds at once (MainWin_ViewApply).
 */
#include "bgi/inframe.h"
#include "bgi/ui.h"
#include "bgi/request.h"
#include "bgi/display.h"
#include "bgi/sys.h"
#include "bgi/msg.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/gfx/typeface.h"
#include "bgi/core.h"

#include <stdio.h>
#include <string.h>

#define INFRAME_IDLE_MS  10 // the host's wait between two frames while a dialog is up

// "ＭＳ ゴシック" (MS Gothic) in UTF-8, for the scripts' UTF-8 mode
#define GOTHIC_UTF8      "\xef\xbc\xad\xef\xbc\xb3 \xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf"

#define SETTINGS_FILTERS 20 // the most filters the display settings offer

// a box that is up
typedef struct InframeBox
{
	UiDialog_t d;
	uint32_t* frame; // the box with its border and caption bar (Ui_FrameSize)
	int fw, fh;      // its size
	int x, y;        // its top left in the picture
	int closeDown;   // the close box is held down
} InframeBox_t;

typedef struct Inframe
{
	OpenBGI_Request_t q;    // the request the dialog answers
	InframeBox_t box[2];    // the request's dialog, and a complaint over it
	int depth;              // the boxes up: 1, or 2 with the complaint
	UiFont_t font;          // the boxes' font: the typeface below
	Typeface_t* face;       // smooth (Typeface_OpenSmooth); NULL when no font could be had: the texts are left out
	int utf8;               // the texts are UTF-8
	int drag, dragX, dragY; // the caption is dragged: the pointer's offset from the box's top left
	int wheel;              // wheel units not yet a notch
	int picW, picH;         // the picture size the boxes were placed for
	int settings;           // the box is the display settings: no request waits for it
	// .. and the names its filter picker steps through: the four and the host's own
	char filterText[SETTINGS_FILTERS][32];
	const char* filterList[SETTINGS_FILTERS];
	int filters;
} Inframe_t;

// ---- the font -------------------------------------------------------------------------

// The dialogs' typeface: MS Gothic (as the scripts name it) with a cell of `height`, smooth; NULL when there is none.
Typeface_t* Inframe_OpenFontSized(OpenBGI_t* o, int height)
{
	return Typeface_OpenSmooth(o, FontNames_Resolve(o, o->enc.textUtf8 ? GOTHIC_UTF8 : MSG_FONT_GOTHIC), height, height / 2);
}

Typeface_t* Inframe_OpenFont(OpenBGI_t* o)
{
	return Inframe_OpenFontSized(o, UI_FONT_H);
}

// the toolkit's draw and width: the typeface's smooth text
static int FontDraw(void* ctx, uint32_t* pix, int pitch, int w, int h, int x, int y, const char* text, uint32_t rgb)
{
	return Typeface_SmoothText(((Inframe_t*)ctx)->face, pix, pitch, w, h, x, y, text, rgb, 0);
}

static int FontWidth(void* ctx, const char* text, int bytes)
{
	return Typeface_SmoothWidth(((Inframe_t*)ctx)->face, text, bytes);
}

// ---- the boxes ------------------------------------------------------------------------

// the box that takes the input
static InframeBox_t* Top(Inframe_t* u)
{
	return &u->box[u->depth - 1];
}

// paint box i and put it over the picture where it is
static void Paint(OpenBGI_t* o, Inframe_t* u, int i)
{
	InframeBox_t* b = &u->box[i];
	b->d.redraw = 0;
	Ui_Draw(&b->d);
	Ui_FrameDraw(&b->d, b->frame, b->fw, b->closeDown);
	Screen_Overlay(o, SCREEN_OVERLAY_DIALOG + i, b->frame, b->fw, b->x, b->y, b->fw, b->fh, 0);
}

// box i centred on the picture (its top left inside it)
static void Place(OpenBGI_t* o, Inframe_t* u, int i)
{
	InframeBox_t* b = &u->box[i];
	b->x = (o->screen.pic.w - b->fw) / 2;
	b->y = (o->screen.pic.h - b->fh) / 2;
	if(b->x < 0)
		b->x = 0;
	if(b->y < 0)
		b->y = 0;
}

// the widest message box for the picture: the box with its border fits it, with some room
static int BoxMaxW(OpenBGI_t* o)
{
	return o->screen.pic.w - 2 * UI_BORDER - 16;
}

// box i, built, up: 1, 0 when there is no memory for it (it is left closed)
static int BoxOpen(OpenBGI_t* o, Inframe_t* u, int i)
{
	InframeBox_t* b = &u->box[i];
	Ui_FrameSize(&b->d, &b->fw, &b->fh);
	if(!Ui_Alloc(&b->d))
		return 0;
	b->frame = (uint32_t*)BGI_Calloc((size_t)b->fw * (size_t)b->fh * 4);
	Place(o, u, i);
	u->depth = i + 1;
	Paint(o, u, i);
	return 1;
}

static void BoxClose(OpenBGI_t* o, Inframe_t* u, int i)
{
	InframeBox_t* b = &u->box[i];
	Screen_Overlay(o, SCREEN_OVERLAY_DIALOG + i, NULL, 0, 0, 0, 0, 0, 0);
	Ui_Free(&b->d);
	BGI_Free(b->frame);
	b->frame = NULL;
}

// the answer of a dialog that cannot be shown: the message box's default button, else cancelled
static int DefaultAnswer(const OpenBGI_Request_t* q)
{
	uint32_t type = q->u.message.flags & 0xf, second = q->u.message.flags & OPENBGI_MB_DEFBUTTON2;
	if(q->kind != OPENBGI_ASK_MESSAGE)
		return 0;
	switch(type)
	{
		case OPENBGI_MB_OKCANCEL: return second ? OPENBGI_IDCANCEL : OPENBGI_IDOK;
		case OPENBGI_MB_YESNO: return second ? OPENBGI_IDNO : OPENBGI_IDYES;
		case OPENBGI_MB_RETRYCANCEL: return second ? OPENBGI_IDCANCEL : OPENBGI_IDRETRY;
		default: return OPENBGI_IDOK;
	}
}

// close everything, without an answer
static void Close(OpenBGI_t* o)
{
	Inframe_t* u = o->inframe.up;
	int i;
	if(!u)
		return;
	for(i = u->depth - 1; i >= 0; i--)
		BoxClose(o, u, i);
	Typeface_Close(u->face);
	BGI_Free(u);
	o->inframe.up = NULL;
}

/* the dialog ended with `result` (a button's id): the entries into the
 * request's buffers when it was confirmed, the boxes down, the answer */
static void Finish(OpenBGI_t* o, int result)
{
	Inframe_t* u = o->inframe.up;
	OpenBGI_Request_t q = u->q;
	const UiDialog_t* d = &u->box[0].d;
	if(u->settings)
	{ // nobody waits for an answer
		Close(o);
		return;
	}
	if(result == 1)
		switch(q.kind)
		{
			case OPENBGI_ASK_INPUT: snprintf(q.u.input.out, 0x100, "%s", d->fields[0].f.text); break;
			case OPENBGI_ASK_INPUT2:
				snprintf(q.u.input2.out[0], 0x100, "%s", d->fields[0].f.text);
				snprintf(q.u.input2.out[1], 0x100, "%s", d->fields[1].f.text);
				break;
			case OPENBGI_ASK_PROFILE:
				Ui_ProfileResult(d, q.u.profile.surname, q.u.profile.givenName, q.u.profile.nickname, q.u.profile.pronoun, q.u.profile.month,
					q.u.profile.day);
				break;
			default: break;
		}
	Close(o);
	OpenBGI_Answer(o, q.id, q.kind == OPENBGI_ASK_MESSAGE ? result : result == 1);
}

/* the request's dialog up; answered at once (with the default) when it
 * cannot be shown */
static void Open(OpenBGI_t* o, const OpenBGI_Request_t* q)
{
	Inframe_t* u = (Inframe_t*)BGI_Calloc(sizeof *u);
	UiDialog_t* d = &u->box[0].d;
	u->q = *q;
	u->utf8 = o->enc.textUtf8;
	u->face = Inframe_OpenFont(o);
	u->font.ctx = u;
	u->font.height = UI_FONT_H;
	if(u->face)
	{
		u->font.draw = FontDraw;
		u->font.width = FontWidth;
	}
	u->picW = o->screen.pic.w;
	u->picH = o->screen.pic.h;
	d->font = &u->font;
	switch(q->kind)
	{
		case OPENBGI_ASK_MESSAGE: Ui_BuildMessageBox(d, q->u.message.text, q->u.message.caption, q->u.message.flags, u->utf8, BoxMaxW(o)); break;
		case OPENBGI_ASK_INPUT: Ui_BuildInput1(d, q->u.input.title, q->u.input.initial, q->u.input.maxLen, q->u.input.digits, u->utf8); break;
		case OPENBGI_ASK_INPUT2:
			Ui_BuildInput2(d, q->u.input2.title, q->u.input2.label[0], q->u.input2.initial[0], q->u.input2.maxLen[0], q->u.input2.digits[0],
				q->u.input2.label[1], q->u.input2.initial[1], q->u.input2.maxLen[1], q->u.input2.digits[1], q->u.input2.compact, u->utf8);
			break;
		default:
			Ui_BuildProfile(d, q->u.profile.surname, q->u.profile.givenName, q->u.profile.nickname, q->u.profile.pronoun, (int)*q->u.profile.month,
				(int)*q->u.profile.day, u->utf8);
			break;
	}
	o->inframe.up = u;
	if(!BoxOpen(o, u, 0))
	{
		Close(o);
		OpenBGI_Answer(o, q->id, DefaultAnswer(q));
	}
}

// ---- the display settings -------------------------------------------------------------

// a picker of the settings stepped: what it says holds from now on
static void SettingsChanged(UiDialog_t* d, int field, void* ctx)
{
	OpenBGI_t* o = (OpenBGI_t*)ctx;
	if(field == UI_SETTINGS_QUALITY(d))
	{
		MainWin_ViewSetHigh(o, Ui_PickValue(d, field));
		return;
	}
	if(field == UI_SETTINGS_FULL(d))
	{ // as Alt+Enter does it; the picker shows what came of it
		if((Ui_PickValue(d, field) != 0) != (o->display.fullscreen != 0) && o->display.windowRestored)
			Display_ToggleStyle(o);
		Ui_PickSet(d, field, o->display.fullscreen != 0);
		d->redraw = 1;
		return;
	}
	MainWin_ViewApply(o, Ui_PickValue(d, UI_SETTINGS_SIZE), Ui_PickValue(d, UI_SETTINGS_FILTER), Ui_PickValue(d, UI_SETTINGS_WHOLE(d)));
}

// "Reset window size": the size the game asks for again, and the box stays; "Close" closes it
static int SettingsButton(UiDialog_t* d, int id, void* ctx)
{
	OpenBGI_t* o = (OpenBGI_t*)ctx;
	if(id != UI_SETTINGS_RESET)
		return 1;
	Ui_PickSet(d, UI_SETTINGS_SIZE, VIEW_SIZE_AUTO);
	MainWin_ViewApply(o, VIEW_SIZE_AUTO, Ui_PickValue(d, UI_SETTINGS_FILTER), Ui_PickValue(d, UI_SETTINGS_WHOLE(d)));
	MainWin_ViewReset(o);
	d->redraw = 1;
	return 0;
}

int Inframe_SettingsUp(OpenBGI_t* o)
{
	return o->inframe.up && o->inframe.up->settings;
}

void Inframe_Settings(OpenBGI_t* o)
{
	Inframe_t* u = o->inframe.up;
	UiDialog_t* d;
	int size, filter, whole;
	if(u)
	{ // up: down again (another dialog stays as it is)
		if(u->settings)
			Close(o);
		return;
	}
	if(o->inframe.stopped || !o->screen.pic.pixels || !MainWin_ViewOn(o))
		return;
	u = (Inframe_t*)BGI_Calloc(sizeof *u);
	if(!u)
		return;
	d = &u->box[0].d;
	u->settings = 1;
	u->utf8 = o->enc.textUtf8;
	u->face = Inframe_OpenFont(o);
	u->font.ctx = u;
	u->font.height = UI_FONT_H;
	if(u->face)
	{
		u->font.draw = FontDraw;
		u->font.width = FontWidth;
	}
	u->picW = o->screen.pic.w;
	u->picH = o->screen.pic.h;
	d->font = &u->font;
	MainWin_ViewSettings(o, &size, &filter, &whole);
	{ // the filters by their names: the four, then what the host has of its own (a quality only with those)
		static const char* const kFour[VIEW_FILTER_OWN] = {"The game's", "Bicubic", "Bilinear", "Nearest"};
		const char* name;
		int n;
		for(n = 0; n < VIEW_FILTER_OWN; n++)
			snprintf(u->filterText[n], sizeof u->filterText[n], "%s", kFour[n]);
		for(n = 0; VIEW_FILTER_OWN + n < SETTINGS_FILTERS && (name = MainWin_ViewFilterName(o, n)) != NULL; n++)
			snprintf(u->filterText[VIEW_FILTER_OWN + n], sizeof u->filterText[0], "%s", name);
		u->filters = VIEW_FILTER_OWN + n;
		for(n = 0; n < u->filters; n++)
			u->filterList[n] = u->filterText[n];
		if(filter >= u->filters)
			filter = VIEW_FILTER_GAME;
	}
	Ui_BuildDisplaySettings(d, size, u->filterList, u->filters, filter, u->filters > VIEW_FILTER_OWN ? MainWin_ViewHigh(o) : -1, whole,
		o->display.fullscreen != 0, u->utf8);
	d->onChange = SettingsChanged;
	d->onButton = SettingsButton;
	d->ctx = o;
	o->inframe.up = u;
	if(!BoxOpen(o, u, 0))
		Close(o);
}

/* after an event: a complaint goes up over the dialog, an ended
 * complaint comes down, an ended dialog answers its request, and what
 * changed is painted again */
static void Settle(OpenBGI_t* o)
{
	Inframe_t* u = o->inframe.up;
	UiDialog_t* d;
	int i;
	if(!u)
		return; // (the settings' own key closed them)
	d = &u->box[0].d;
	if(u->depth == 2 && u->box[1].d.done)
	{
		BoxClose(o, u, 1);
		u->depth = 1;
		u->drag = 0;
		d->redraw = 1;
	}
	if(u->depth == 1 && d->done)
	{
		Finish(o, d->result);
		return;
	}
	if(u->depth == 1 && d->complaint[0])
	{ // a refusing button's message: a box over the dialog
		UiDialog_t* c = &u->box[1].d;
		memset(&u->box[1], 0, sizeof u->box[1]);
		c->font = &u->font;
		Ui_BuildMessageBox(c, d->complaint, d->complaintCaption, d->complaintFlags, u->utf8, BoxMaxW(o));
		d->complaint[0] = 0;
		u->drag = 0;
		BoxOpen(o, u, 1); // (without memory for it the dialog goes on without the complaint)
	}
	for(i = 0; i < u->depth; i++)
		if(u->box[i].d.redraw)
			Paint(o, u, i);
}

// ---- the engine's side ----------------------------------------------------------------

int Inframe_Takes(OpenBGI_t* o, int kind)
{
	if(kind < OPENBGI_ASK_MESSAGE || kind > OPENBGI_ASK_PROFILE || o->inframe.stopped || !o->screen.pic.pixels)
		return 0;
	if(o->engine.optPreview)
		return 0; // (a run nobody sees: its boxes are for the host to answer)
	if(o->engine.optDialogs)
		return o->engine.optDialogs == 1;
	if(!(o->host.caps & OPENBGI_CAP_DIALOGS) && !(kind == OPENBGI_ASK_MESSAGE && (o->host.caps & OPENBGI_CAP_MESSAGES)))
		return 1;          // (a host with boxes alone, OPENBGI_CAP_MESSAGES, takes the messages)
	return Pad_HasTurn(o); // the host's windows are out of a pad's reach
}

int Inframe_Up(OpenBGI_t* o)
{
	return o->inframe.up != NULL;
}

int Inframe_Focus(OpenBGI_t* o)
{
	const UiDialog_t* d;
	if(!o->inframe.up)
		return 0;
	d = &Top(o->inframe.up)->d;
	if(d->focus < d->nFields)
		return d->fields[d->focus].numeric || d->fields[d->focus].choices ? INFRAME_FOCUS_PICKER : INFRAME_FOCUS_FIELD;
	return d->focus < d->nFields + d->nButtons ? INFRAME_FOCUS_BUTTON : INFRAME_FOCUS_LIST;
}

void Inframe_Frame(OpenBGI_t* o, OpenBGI_Frame_t* f)
{
	Inframe_t* u;
	InframeBox_t* b;
	int field, i;
	if(!o->inframe.up)
	{
		OpenBGI_Request_t q;
		if(!Request_NextInframe(o, &q))
			return;
		if(!o->screen.pic.pixels)
		{ // the picture went since it was asked
			OpenBGI_Answer(o, q.id, DefaultAnswer(&q));
			return;
		}
		Open(o, &q);
		if(!o->inframe.up)
			return;
	}
	u = o->inframe.up;
	if(!o->screen.pic.pixels)
	{ // the picture went from under the dialog: it counts as cancelled
		Finish(o, DefaultAnswer(&u->q));
		return;
	}
	if(u->picW != o->screen.pic.w || u->picH != o->screen.pic.h)
	{ // a new picture size: the boxes centred on it again
		u->picW = o->screen.pic.w;
		u->picH = o->screen.pic.h;
		u->drag = 0;
		for(i = 0; i < u->depth; i++)
		{
			Place(o, u, i);
			Paint(o, u, i);
		}
	}
	b = Top(u);
	field = Ui_FocusedField(&b->d);
	f->textInput = 0; // the dialog has the keyboard, not the text entry
	memset(&f->textRect, 0, sizeof f->textRect);
	if(field >= 0 && !b->d.fields[field].pick)
	{ // a field takes typed text: the host's input method there
		f->textInput = 1;
		f->textRect.x = b->x + UI_BORDER + b->d.fields[field].x;
		f->textRect.y = b->y + UI_BORDER + UI_CAPTION_H + b->d.fields[field].y;
		f->textRect.w = b->d.fields[field].w;
		f->textRect.h = b->d.fields[field].h;
	}
	if(f->idleMs < INFRAME_IDLE_MS)
		f->idleMs = INFRAME_IDLE_MS;
}

void Inframe_Key(OpenBGI_t* o, int vk, int down, int flags)
{
	UiDialog_t* d;
	int alt;
	if(!o->inframe.up || !down)
		return; // the releases go nowhere
	d = &Top(o->inframe.up)->d;
	alt = Input_KeyDown(o, 0x12) || ((flags & OPENBGI_KEY_SYSTEM) && vk != 0x79);
	if(alt && vk == 0x73)
		Ui_Close(d); // Alt+F4 closes the box, as the close box does
	else
		Ui_Key(d, vk, Input_KeyDown(o, 0x10), Input_KeyDown(o, 0x11), alt); // (a key that types: its text follows, OpenBGI_Text)
	Settle(o);
}

void Inframe_Text(OpenBGI_t* o, const char* utf8)
{
	if(!o->inframe.up)
		return;
	Ui_Type(&Top(o->inframe.up)->d, 0, utf8);
	Settle(o);
}

// a drag of the caption moves the box that takes the input, keeping part of its caption in the picture
void Inframe_Pointer(OpenBGI_t* o, int x, int y)
{
	Inframe_t* u = o->inframe.up;
	InframeBox_t* b;
	if(!u || !u->drag)
		return;
	b = Top(u);
	x -= u->dragX;
	y -= u->dragY;
	if(x > o->screen.pic.w - 32)
		x = o->screen.pic.w - 32;
	if(x < 32 - b->fw)
		x = 32 - b->fw;
	if(y > o->screen.pic.h - UI_CAPTION_H)
		y = o->screen.pic.h - UI_CAPTION_H;
	if(y < 0)
		y = 0;
	if(x == b->x && y == b->y)
		return;
	b->x = x;
	b->y = y;
	Screen_Overlay(o, SCREEN_OVERLAY_DIALOG + u->depth - 1, b->frame, b->fw, b->x, b->y, b->fw, b->fh, 0);
}

/* the left button: on the client area the dialog's, on the caption a
 * drag, on the close box (released over it) the cancel button */
void Inframe_Button(OpenBGI_t* o, int button, int down, int x, int y)
{
	Inframe_t* u = o->inframe.up;
	InframeBox_t* b;
	int cx, cy, hit;
	if(!u || button != OPENBGI_BUTTON_LEFT)
		return;
	b = Top(u);
	hit = Ui_FrameHit(&b->d, x - b->x, y - b->y, &cx, &cy);
	if(down)
	{
		if(hit == UI_HIT_CLIENT)
			Ui_Press(&b->d, cx, cy, Clock_HostMs(o));
		else if(hit == UI_HIT_CAPTION)
		{
			u->drag = 1;
			u->dragX = x - b->x;
			u->dragY = y - b->y;
		}
		else if(hit == UI_HIT_CLOSE)
		{
			b->closeDown = 1;
			b->d.redraw = 1;
		}
	}
	else if(u->drag)
		u->drag = 0;
	else if(b->closeDown)
	{
		b->closeDown = 0;
		b->d.redraw = 1;
		if(hit == UI_HIT_CLOSE)
			Ui_Close(&b->d);
	}
	else
		Ui_Release(&b->d, cx, cy);
	Settle(o);
}

// the wheel over a list of the box that takes the input, a notch at a time
void Inframe_Wheel(OpenBGI_t* o, int delta)
{
	Inframe_t* u = o->inframe.up;
	InframeBox_t* b;
	int cx, cy, notches;
	if(!u)
		return;
	b = Top(u);
	u->wheel += delta;
	notches = u->wheel / 120;
	u->wheel -= notches * 120;
	if(notches && Ui_FrameHit(&b->d, o->input.pointerX - b->x, o->input.pointerY - b->y, &cx, &cy) == UI_HIT_CLIENT)
		Ui_Wheel(&b->d, cx, cy, notches);
	Settle(o);
}

void Inframe_Free(OpenBGI_t* o)
{
	Close(o);
	o->inframe.stopped = 1;
}
