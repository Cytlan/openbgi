/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * edit.c - the text entry control of "B0 20" .. "B0 2A"; interface in
 *          bgi/edit.h
 *
 * One edit control sits over the main window.  Up to 1.69/451 it is made
 * with the window and lives as long as it; from 1.69/472 on each setup
 * makes it anew and "B0 21" destroys it.  Its style and the work of Tab
 * and Enter changed over the builds (OPENBGI_ENTRY_* of openbgi.h,
 * Flags below).  A host with a text entry of its own
 * (OpenBGI_Host_t.textEntry: Windows' EDIT control, as the original's)
 * gets the calls; the original's subclass procedure - Tab and Enter, the
 * re-presenting of the picture under the transparent control at WM_PAINT
 * - is then the host's (which puts the picture back from the last frame).
 *
 * Without one, and for a setup made while a pad has the turn (the host's
 * control is out of a pad's reach: Pad_HasTurn, Edit_ToFrame), the engine
 * draws the entry itself, as the original's
 * control looks: the text in its colour with nothing behind it, aligned
 * as the style says and at the top of the rectangle; on one line scrolled
 * to the caret when it is wider (ES_AUTOHSCROLL), or wrapped on as many
 * lines as the rectangle holds (ES_MULTILINE: at spaces and between
 * double-byte characters, no more text taken than the lines hold); the
 * selection highlighted whether the entry has the focus or not
 * (ES_NOHIDESEL), a caret while it has.  It lies over the picture
 * (Screen_Overlay, SCREEN_OVERLAY_EDIT), and while it has the focus it
 * takes the keys and the typed text; a click on it gives it the focus, a
 * click elsewhere takes the focus away, as the main window's button
 * handlers do.  This file also holds the script-visible state: the text
 * applied by the next setup, the rectangle of the last setup, the colour,
 * "B0 28", "B0 29" and "B0 2A".
 */
#include "bgi/edit.h"
#include "bgi/display.h"
#include "bgi/input.h"
#include "bgi/msg.h"
#include "bgi/ui.h"
#include "bgi/version.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/gfx/typeface.h"
#include "bgi/core.h"

#include <string.h>

#define EDIT_LINES (EDIT_TEXT_MAX + 1) // a line holds a byte at least

// the entry the engine draws in the frame
typedef struct EditFrame
{
	int shown, focus;     // "B0 24"; it takes the keys and the text
	int x, y, w, h;       // its rectangle, the picture's pixels
	int flags;            // OPENBGI_ENTRY_* of the last setup
	UiField_t field;      // the text, the caret, the selection, the limit, "B0 29"
	Typeface_t* face;     // the font of the last setup (smooth), NULL before one or when none could be had
	int size;             // its cell height, a line's height
	uint32_t* layer;      // w x h, 0xAARRGGBB: the text, the selection and the caret over nothing
	int scroll;           // one line: pixels the text is moved to the left when it is wider than the entry
	int top;              // several lines: the first line shown
	int eol;              // several lines: the caret, where a line wraps, is shown at the end of the line before
	int line[EDIT_LINES]; // several lines: where each starts in the text (Layout) ..
	int lines;            // .. and how many there are
	int pressed;          // the buttons pressed on the entry (a bit each): their releases are its too
	int dirty;            // the layer is drawn again before the frame goes out
} EditFrame_t;

// the host's entry, NULL when the engine draws its own
static const OpenBGI_TextEntry_t* Host(OpenBGI_t* o)
{
	return o->edit.own ? NULL : o->host.textEntry;
}

// 1.69/472 on: the control is made by a setup and destroyed by "B0 21"
static int MadeBySetup(OpenBGI_t* o)
{
	return o->engine.profile->gen >= GEN_1_69_472;
}

// a control exists: always up to 1.69/451 (it is made with the window), else from a setup until "B0 21"
static int Made(OpenBGI_t* o)
{
	return !MadeBySetup(o) || o->edit.made;
}

/* The OPENBGI_ENTRY_* of a control of h pixels with a font of `size`
 * pixels, as the build makes it: centred on one line up to 1.69/451; from
 * 1.69/472 on left-aligned ("B0 2A" from 1.636 on), several lines when more
 * than one fits, the IME status window with it, Enter hiding it with "B0
 * 28"; from 1.529 on Tab hides it. */
static int Flags(OpenBGI_t* o, int h, int size)
{
	int gen = o->engine.profile->gen, flags;
	if(gen < GEN_1_69_472)
		return OPENBGI_ENTRY_CENTRE;
	flags = OPENBGI_ENTRY_IME_STATUS;
	if(o->edit.align == 1)
		flags |= OPENBGI_ENTRY_CENTRE;
	else if(o->edit.align == 2)
		flags |= OPENBGI_ENTRY_RIGHT;
	if(size > 0 && h / size > 1)
		flags |= OPENBGI_ENTRY_MULTILINE;
	if(gen >= GEN_1_529)
		flags |= OPENBGI_ENTRY_TAB_HIDES;
	if(o->edit.closeOnEnter)
		flags |= OPENBGI_ENTRY_ENTER_HIDES;
	return flags;
}

/* the engine's entry, made as the original's control is made: hidden, with
 * MSG_EDIT_DEFAULT_TEXT and the script's colour and "B0 29" */
static EditFrame_t* Frame(OpenBGI_t* o)
{
	EditFrame_t* e = o->edit.frame;
	if(!e)
	{
		e = (EditFrame_t*)BGI_Calloc(sizeof *e);
		e->field.utf8 = o->enc.textUtf8;
		Ui_FromSjis(MSG_EDIT_DEFAULT_TEXT, e->field.text, (int)sizeof e->field.text, e->field.utf8);
		e->field.caret = (int)strlen(e->field.text);
		e->field.noAscii = o->edit.rejectAscii;
		e->w = e->h = 1; // at (0, 0), 1 x 1 until a setup
		e->flags = OPENBGI_ENTRY_CENTRE;
		e->lines = 1;
		o->edit.frame = e;
	}
	return e;
}

static void Dirty(EditFrame_t* e)
{
	e->dirty = 1;
}

// the engine's entry goes: off the picture, its resources freed
static void FrameFree(OpenBGI_t* o)
{
	EditFrame_t* e = o->edit.frame;
	if(!e)
		return;
	Screen_Overlay(o, SCREEN_OVERLAY_EDIT, NULL, 0, 0, 0, 0, 0, 0);
	Typeface_Close(e->face);
	BGI_Free(e->layer);
	BGI_Free(e);
	o->edit.frame = NULL;
}

// ---- the script's instructions ------------------------------------------------------------

void Edit_Show(OpenBGI_t* o, int show)
{
	const OpenBGI_TextEntry_t* h = Host(o);
	EditFrame_t* e;
	if(!Made(o))
		return; // no control to show
	if(h)
	{
		if(h->show)
			h->show(o->host.user, show != 0);
		return;
	}
	e = Frame(o);
	e->shown = show != 0;
	if(!e->shown)
		e->focus = 0; // a hidden entry takes no keys
	Dirty(e);
}

/* the engine's entry at a new place with a new font and style, the text
 * and the limit set (the text as SetWindowText puts it, whatever the
 * limit), everything selected */
static void FrameSetup(OpenBGI_t* o, int x, int y, int w, int h, const char* face, int size, int width, int maxLen, int focus, int flags,
	const char* text)
{
	EditFrame_t* e;
	if(MadeBySetup(o))
		FrameFree(o); // a new control
	e = Frame(o);
	if(!e->layer || w != e->w || h != e->h)
	{
		BGI_Free(e->layer);
		e->layer = (uint32_t*)BGI_Calloc((size_t)w * (size_t)h * 4);
	}
	Typeface_Close(e->face);
	e->face = Typeface_OpenSmooth(o, face, size, width);
	e->size = size;
	e->x = x;
	e->y = y;
	e->w = w;
	e->h = h;
	e->flags = flags;
	e->shown = 1;
	e->field.utf8 = o->enc.textUtf8;
	e->field.noAscii = o->edit.rejectAscii;
	e->field.limit = 0;
	UiField_SetText(&e->field, text);
	e->field.limit = maxLen < (int)sizeof e->field.text ? maxLen : 0;
	UiField_SelectAll(&e->field);
	e->focus = focus != 0;
	e->scroll = 0;
	e->top = 0;
	e->eol = 0;
	Dirty(e);
}

// the colour as the engine's entry draws it, 0x00RRGGBB
static uint32_t Rgb(OpenBGI_t* o)
{
	uint32_t c = o->edit.editColour;
	return ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);
}

/* The entry from here on is the engine's (`own`) or the host's, where the
 * host has one: the other one goes off the screen, and the host's learns
 * what the scripts set while it was away. */
static void Whose(OpenBGI_t* o, int own)
{
	const OpenBGI_TextEntry_t* h = o->host.textEntry;
	if(!h || own == o->edit.own)
		return;
	if(own)
	{
		if(h->show)
			h->show(o->host.user, 0);
	}
	else
	{
		FrameFree(o);
		if(h->setColour)
			h->setColour(o->host.user, Rgb(o));
		if(h->rejectAscii)
			h->rejectAscii(o->host.user, o->edit.rejectAscii);
	}
	o->edit.own = own;
}

void Edit_ToFrame(OpenBGI_t* o)
{
	const OpenBGI_TextEntry_t* h = o->host.textEntry;
	const Rect_t* r = &o->edit.editRect;
	const char* face;
	char text[EDIT_TEXT_MAX];
	if(!h || o->edit.own || !o->edit.made || !o->screen.pic.pixels || !Edit_Shown(o))
		return;
	face = FontNameByNo(o, o->edit.fontNo);
	if(!face)
		return;
	text[0] = 0;
	if(h->getText)
		h->getText(o->host.user, text, EDIT_TEXT_MAX);
	Whose(o, 1);
	FrameSetup(o, r->l, r->t, r->r - r->l + 1, r->b - r->t + 1, face, o->edit.size, o->edit.size * o->edit.editWidthPct / 100 / 2, o->edit.maxLen, 1,
		o->edit.flags, text);
	Frame(o)->field.selAll = 0; // (what was typed stays as the next key comes)
}

/* "B0 20": place the control at (x, y) with a w x h client area, give it
 * the font `fontNo` at `size` pixels and a text limit of `maxLen` bytes,
 * put the pending text (Edit_SetText) in it selected, and show it;
 * `focus` non-zero also gives it the keyboard.  From 1.69/472 on this is
 * a new control, in the style of Flags.  0 ok, 1 w or h outside 8 .. the
 * picture size, 2 unknown font number, 3 size outside 8 .. 0x40, 4 maxLen
 * outside 1 .. 0x100.  Where the host has an entry, each setup decides
 * anew whose it is: the engine's while a pad has the turn. */
int Edit_Setup(OpenBGI_t* o, int x, int y, int w, int h, int fontNo, int size, int maxLen, int focus)
{
	const OpenBGI_TextEntry_t* t;
	const char* face;
	int width, flags;
	if(w < 8 || w > o->display.screenWidths[o->display.sizeIndex] || h < 8 || h > o->display.screenHeights[o->display.sizeIndex])
		return 1;
	face = FontNameByNo(o, fontNo);
	if(!face)
		return 2;
	if(size < 8 || size > 0x40)
		return 3;
	if(maxLen < 1 || maxLen > 0x100)
		return 4;
	width = size * o->edit.editWidthPct / 100 / 2;
	flags = Flags(o, h, size);
	Whose(o, o->host.textEntry && Pad_HasTurn(o));
	t = Host(o);
	o->edit.fontNo = fontNo;
	o->edit.size = size;
	o->edit.maxLen = maxLen;
	o->edit.flags = flags;
	o->edit.editRect.l = x;
	o->edit.editRect.t = y;
	o->edit.editRect.r = x + w - 1;
	o->edit.editRect.b = y + h - 1;
	o->edit.made = 1;
	if(!t)
	{
		FrameSetup(o, x, y, w, h, face, size, width, maxLen, focus, flags, o->edit.editText);
		return 0;
	}
	if(t->setStyle)
		t->setStyle(o->host.user, flags);
	Edit_Show(o, 1);
	if(t->move)
		t->move(o->host.user, x, y, w, h);
	if(t->setFont)
		t->setFont(o->host.user, face, size, width);
	if(t->limitText)
		t->limitText(o->host.user, maxLen);
	if(t->setText)
		t->setText(o->host.user, o->edit.editText);
	if(t->selectAll)
		t->selectAll(o->host.user);
	if(t->focus)
		t->focus(o->host.user, focus != 0);
	return 0;
}

/* "B0 21" (1.69/472 on): the control is destroyed (hidden, its text
 * gone: "B0 27" answers -1 and "B0 24" does nothing until the next
 * setup); 0 */
int Edit_Remove(OpenBGI_t* o)
{
	const OpenBGI_TextEntry_t* h = Host(o);
	if(!o->edit.made)
		return 0;
	if(h)
	{
		if(h->show)
			h->show(o->host.user, 0);
	}
	else
		FrameFree(o);
	o->edit.made = 0;
	return 0;
}

/* "B0 22" (1.529 on): the glyph width of the font the next Edit_Setup
 * creates, in percent of the normal width (half the size); 1 when within
 * 25 .. 200 and taken, else 0 */
int Edit_SetFontWidthPct(OpenBGI_t* o, int pct)
{
	if(pct < 25 || pct > 200)
		return 0;
	o->edit.editWidthPct = pct;
	return 1;
}

// "B0 26": the text the next Edit_Setup puts in the control (truncated to 0xff bytes)
void Edit_SetText(OpenBGI_t* o, const char* s)
{
	// the original copies without a bound into the 0x100 byte buffer
	strncpy(o->edit.editText, s, sizeof o->edit.editText - 1);
	o->edit.editText[sizeof o->edit.editText - 1] = 0;
}

// "B0 27": the control's current text into buf (0x100 bytes); returns its length, -1 (buf untouched) without a control
int Edit_GetText(OpenBGI_t* o, char* buf)
{
	const OpenBGI_TextEntry_t* h = Host(o);
	size_t n;
	if(!Made(o))
		return -1;
	if(h)
	{
		buf[0] = 0;
		return h->getText ? h->getText(o->host.user, buf, EDIT_TEXT_MAX) : 0;
	}
	n = strlen(Frame(o)->field.text);
	if(n > EDIT_TEXT_MAX - 1)
		n = EDIT_TEXT_MAX - 1;
	memcpy(buf, Frame(o)->field.text, n);
	buf[n] = 0;
	return (int)n;
}

// "B0 25": the text colour as 0x00RRGGBB; stored as a COLORREF (0x00BBGGRR)
void Edit_SetColour(OpenBGI_t* o, uint32_t rgb)
{
	const OpenBGI_TextEntry_t* h = Host(o);
	o->edit.editColour = ((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff);
	if(!h)
	{
		if(o->edit.frame)
			Dirty(o->edit.frame);
	}
	else if(h->setColour)
		h->setColour(o->host.user, rgb);
}

void Edit_RejectAscii(OpenBGI_t* o, int reject)
{
	const OpenBGI_TextEntry_t* h = Host(o);
	o->edit.rejectAscii = reject != 0;
	if(!h)
	{
		if(o->edit.frame)
			o->edit.frame->field.noAscii = o->edit.rejectAscii;
	}
	else if(h->rejectAscii)
		h->rejectAscii(o->host.user, reject);
}

/* "B0 28" (1.69/472 on): Enter hides the control as well; a host's entry
 * that exists learns it at once */
void Edit_SetCloseOnEnter(OpenBGI_t* o, int on)
{
	const OpenBGI_TextEntry_t* h = Host(o);
	EditFrame_t* e = o->edit.frame;
	o->edit.closeOnEnter = on != 0;
	if(!MadeBySetup(o))
		return;
	o->edit.flags = on ? o->edit.flags | OPENBGI_ENTRY_ENTER_HIDES : o->edit.flags & ~OPENBGI_ENTRY_ENTER_HIDES;
	if(h)
	{
		if(o->edit.made && h->setStyle)
			h->setStyle(o->host.user, o->edit.flags);
	}
	else if(e)
		e->flags = o->edit.flags;
}

// "B0 2A" (1.636 on): the next setup's alignment, 0 left, 1 centred, 2 right; 1 when taken, 0 for another value
int Edit_SetAlign(OpenBGI_t* o, uint32_t align)
{
	if(align > 2)
		return 0;
	o->edit.align = (int)align;
	return 1;
}

/* "B0 23" (1.69/472 on): the control is shown - the state its input
 * method status window was last given, which Tab and Enter may have
 * changed */
int Edit_Shown(OpenBGI_t* o)
{
	const OpenBGI_TextEntry_t* h = Host(o);
	if(!Made(o))
		return 0;
	if(h)
		return h->shown ? h->shown(o->host.user) != 0 : 0;
	return o->edit.frame && o->edit.frame->shown;
}

void Edit_Destroy(OpenBGI_t* o)
{
	const OpenBGI_TextEntry_t* h = o->host.textEntry;
	o->edit.made = 0;
	o->edit.own = 0;
	FrameFree(o); // (the engine's, where it stood in for the host's as well)
	if(h && h->destroy)
		h->destroy(o->host.user);
}

// ---- the engine's entry: its lines --------------------------------------------------------

// the width of `n` bytes of the text from byte `from`
static int Width(const EditFrame_t* e, int from, int n)
{
	return n > 0 ? Typeface_SmoothWidth(e->face, e->field.text + from, n) : 0;
}

// a character of the text: a byte, or two of a double-byte character (three or four in UTF-8)
static int CharLen(const EditFrame_t* e, int at)
{
	return Ui_CharLen(e->field.text + at, e->field.utf8);
}

// a line may break before the character at `at` (after a space, before or after a double-byte character)
static int CanBreak(const EditFrame_t* e, int from, int at)
{
	const char* t = e->field.text;
	int prev = from, p;
	if(at <= from)
		return 0;
	for(p = from; p < at; p += CharLen(e, p))
		prev = p;
	return t[prev] == ' ' || (uint8_t)t[prev] >= 0x80 || (uint8_t)t[at] >= 0x80;
}

/* The lines as the control wraps the text (several lines; one line holds
 * all of it): a line ends at "\r\n", or before the character that would
 * not fit, at the last place it may break (else at that character). */
static void Layout(EditFrame_t* e)
{
	const char* t = e->field.text;
	int from = 0, p = 0, n = 0;
	e->line[n++] = 0;
	if(!(e->flags & OPENBGI_ENTRY_MULTILINE))
	{
		e->lines = 1;
		return;
	}
	while(t[p] && n < EDIT_LINES)
	{
		int len = CharLen(e, p);
		if(t[p] == '\r' && t[p + 1] == '\n')
		{
			from = p + 2;
			e->line[n++] = from;
			p = from;
			continue;
		}
		if(p > from && Width(e, from, p + len - from) > e->w)
		{ // t[p] would not fit: break at the last place before it, or at it
			int q, at = p;
			for(q = from + CharLen(e, from); q < p; q += CharLen(e, q))
				if(CanBreak(e, from, q))
					at = q;
			if(CanBreak(e, from, p))
				at = p;
			from = at;
			e->line[n++] = from;
			p = from;
			continue;
		}
		p += len;
	}
	e->lines = n;
}

// the bytes of line i, without the line break that ends it
static int LineLen(const EditFrame_t* e, int i)
{
	int end = i + 1 < e->lines ? e->line[i + 1] : (int)strlen(e->field.text);
	if(end - e->line[i] >= 2 && e->field.text[end - 2] == '\r' && e->field.text[end - 1] == '\n')
		end -= 2;
	return end - e->line[i];
}

// the line the caret is on
static int CaretLine(const EditFrame_t* e)
{
	int i = e->lines - 1;
	while(i > 0 && e->line[i] > e->field.caret)
		i--;
	if(e->eol && i > 0 && e->line[i] == e->field.caret && LineLen(e, i - 1) == e->line[i] - e->line[i - 1])
		i--; // at the end of a wrapped line
	return i;
}

// where line i starts, from the entry's left edge, as the alignment puts it
static int LineX(const EditFrame_t* e, int i)
{
	int lw = Width(e, e->line[i], LineLen(e, i));
	if(e->flags & OPENBGI_ENTRY_CENTRE)
		return (e->w - lw) / 2;
	if(e->flags & OPENBGI_ENTRY_RIGHT)
		return e->w - lw;
	return 0;
}

// the lines that show
static int Visible(const EditFrame_t* e)
{
	int n = e->size > 0 ? e->h / e->size : 1;
	return n > 0 ? n : 1;
}

// ---- the engine's entry: its picture -------------------------------------------------------

// one line: where the text starts in the entry, aligned when it fits, else scrolled to keep the caret in view
static int TextX(EditFrame_t* e, int textW, int caretX)
{
	if(textW <= e->w)
	{
		e->scroll = 0;
		if(e->flags & OPENBGI_ENTRY_CENTRE)
			return (e->w - textW) / 2;
		return e->flags & OPENBGI_ENTRY_RIGHT ? e->w - textW : 0;
	}
	if(caretX - e->scroll > e->w - 1)
		e->scroll = caretX - (e->w - 1);
	if(caretX < e->scroll)
		e->scroll = caretX;
	if(textW - e->scroll < e->w - 1) // nothing left empty at the right
		e->scroll = textW - (e->w - 1);
	return -e->scroll;
}

// a line's text at (x, y) of the layer: on the selection's highlight when all is selected
static void DrawLine(EditFrame_t* e, OpenBGI_t* o, int from, int n, int x, int y)
{
	char s[EDIT_TEXT_MAX + 1];
	int sel = e->field.selAll && e->field.text[0];
	memcpy(s, e->field.text + from, (size_t)n);
	s[n] = 0;
	if(sel)
		Ui_FillRect(e->layer, e->w, e->w, e->h, x, y, Width(e, from, n), e->size, 0xff000000u | UI_SELECT_BG);
	Typeface_SmoothText(e->face, e->layer, e->w, e->w, e->h, x, y, s, sel ? UI_SELECT_TEXT : Rgb(o), 1);
}

// the layer: the text (on the selection's highlight when it is all selected), then the caret
static void Draw(OpenBGI_t* o, EditFrame_t* e)
{
	const UiField_t* f = &e->field;
	memset(e->layer, 0, (size_t)e->w * (size_t)e->h * 4);
	Layout(e);
	if(!(e->flags & OPENBGI_ENTRY_MULTILINE))
	{
		int textW = Width(e, 0, (int)strlen(f->text)), caretX = Width(e, 0, f->caret);
		int tx = TextX(e, textW, caretX);
		DrawLine(e, o, 0, (int)strlen(f->text), tx, 0);
		if(e->focus && !f->selAll)
			Ui_FillRect(e->layer, e->w, e->w, e->h, tx + caretX, 0, 1, e->size, 0xff000000u | Rgb(o));
	}
	else
	{
		int i, cl = CaretLine(e), vis = Visible(e);
		if(cl < e->top)
			e->top = cl;
		if(cl >= e->top + vis)
			e->top = cl - vis + 1;
		for(i = e->top; i < e->lines && (i - e->top) * e->size < e->h; i++)
			DrawLine(e, o, e->line[i], LineLen(e, i), LineX(e, i), (i - e->top) * e->size);
		if(e->focus && !f->selAll)
			Ui_FillRect(e->layer, e->w, e->w, e->h, LineX(e, cl) + Width(e, e->line[cl], f->caret - e->line[cl]),
				(cl - e->top) * e->size, 1, e->size, 0xff000000u | Rgb(o));
	}
	e->dirty = 0;
}

void Edit_Frame(OpenBGI_t* o, OpenBGI_Frame_t* f)
{
	EditFrame_t* e = o->edit.frame;
	if(Host(o) || !e)
		return;
	if(!e->shown || !e->layer)
	{
		if(o->screen.overlay[SCREEN_OVERLAY_EDIT].pix)
			Screen_Overlay(o, SCREEN_OVERLAY_EDIT, NULL, 0, 0, 0, 0, 0, 0);
		return;
	}
	if(e->dirty)
	{
		Draw(o, e);
		Screen_Overlay(o, SCREEN_OVERLAY_EDIT, e->layer, e->w, e->x, e->y, e->w, e->h, 1);
	}
	if(e->focus)
	{ // the host's input method there
		f->textInput = 1;
		f->textRect.x = e->x;
		f->textRect.y = e->y;
		f->textRect.w = e->w;
		f->textRect.h = e->h;
	}
}

// ---- the engine's entry: its input ----------------------------------------------------------

// the engine's entry, when it is up
static EditFrame_t* Up(OpenBGI_t* o)
{
	EditFrame_t* e = o->edit.frame;
	return !Host(o) && e && e->shown ? e : NULL;
}

static int Over(const EditFrame_t* e, int x, int y)
{
	return x >= e->x && x < e->x + e->w && y >= e->y && y < e->y + e->h;
}

// the character boundary of line i nearest to x (from the entry's left edge)
static int Nearest(EditFrame_t* e, int i, int lx, int x)
{
	int from = e->line[i], n = LineLen(e, i), at = 0, best = from, bestD = -1;
	for(;;)
	{
		int d = lx + Width(e, from, at) - x;
		if(d < 0)
			d = -d;
		if(bestD < 0 || d < bestD)
		{
			bestD = d;
			best = from + at;
		}
		if(at >= n)
			break;
		at += CharLen(e, from + at);
	}
	return best;
}

/* the keys of several lines the field does not know: Up and Down move to
 * the line above or below, Home and End to the start or end of the line
 * (with Ctrl they are the field's: the start or end of the text); 1 when
 * taken */
static int LineKey(EditFrame_t* e, int vk, int ctrl)
{
	int cl, x;
	if(!(e->flags & OPENBGI_ENTRY_MULTILINE) || ctrl || (vk != 0x26 && vk != 0x28 && vk != 0x24 && vk != 0x23))
		return 0;
	Layout(e);
	cl = CaretLine(e);
	e->field.selAll = 0;
	e->eol = 0;
	switch(vk)
	{
		case 0x24: // Home
			e->field.caret = e->line[cl];
			break;
		case 0x23: // End
			e->field.caret = e->line[cl] + LineLen(e, cl);
			e->eol = cl + 1 < e->lines;
			break;
		default: // Up, Down
			if((vk == 0x26 && cl == 0) || (vk == 0x28 && cl + 1 >= e->lines))
				break;
			x = LineX(e, cl) + Width(e, e->line[cl], e->field.caret - e->line[cl]);
			cl += vk == 0x26 ? -1 : 1;
			e->field.caret = Nearest(e, cl, LineX(e, cl), x);
			e->eol = cl + 1 < e->lines && e->field.caret == e->line[cl + 1];
			break;
	}
	return 1;
}

/* several lines take no more text than they hold (no ES_AUTOVSCROLL): a
 * change that needs more lines than there were, and more than show, is
 * undone; 1 when the change stays */
static int Fits(EditFrame_t* e, const UiField_t* before)
{
	UiField_t after = e->field;
	int lines;
	if(!(e->flags & OPENBGI_ENTRY_MULTILINE))
		return 1;
	e->field = *before;
	Layout(e);
	lines = e->lines;
	e->field = after;
	Layout(e);
	if(e->lines <= Visible(e) || e->lines <= lines)
		return 1;
	e->field = *before;
	Layout(e);
	return 0;
}

/* A key while the entry has the focus is its own (its release too), but
 * for Alt+F4, which closes the window as the control's default did: Tab
 * selects the text and gives the focus back to the main window, or hides
 * the entry (1.529 on); Enter selects the text and gives the focus back,
 * and hides the entry too with "B0 28"; the editing keys edit, a key that
 * types waits for its text. */
int Edit_Key(OpenBGI_t* o, int vk, int down, int flags)
{
	EditFrame_t* e = Up(o);
	if(!e || !e->focus)
		return 0;
	if((flags & OPENBGI_KEY_SYSTEM) && vk == 0x73)
		return 0; // Alt+F4
	if(!down)
		return 1;
	if(vk == 0x09 && (e->flags & OPENBGI_ENTRY_TAB_HIDES))
		Edit_Show(o, 0);
	else if(vk == 0x09 || vk == 0x0d)
	{
		UiField_SelectAll(&e->field);
		e->eol = 0;
		e->focus = 0;
		if(vk == 0x0d && (e->flags & OPENBGI_ENTRY_ENTER_HIDES))
			Edit_Show(o, 0);
	}
	else if(!LineKey(e, vk, Input_KeyDown(o, 0x11)))
	{
		UiField_t before = e->field;
		if(UiField_Key(&e->field, vk, Input_KeyDown(o, 0x11),
			   Input_KeyDown(o, 0x12) || ((flags & OPENBGI_KEY_SYSTEM) && vk != 0x79)) >= 0)
			e->eol = 0;
		Fits(e, &before);
	}
	Dirty(e);
	return 1;
}

/* Typed text while the entry has the focus is its own: on several lines
 * as much of it as the lines hold (whole characters from its start). */
int Edit_Text(OpenBGI_t* o, const char* utf8)
{
	EditFrame_t* e = Up(o);
	char s[EDIT_TEXT_MAX];
	UiField_t before;
	size_t n;
	if(!e || !e->focus)
		return 0;
	before = e->field;
	n = strlen(utf8) < sizeof s - 1 ? strlen(utf8) : sizeof s - 1;
	memcpy(s, utf8, n);
	s[n] = 0;
	while(UiField_Type(&e->field, s))
	{
		if(Fits(e, &before))
		{
			e->eol = 0;
			Dirty(e);
			break;
		}
		do // a character less
			n--;
		while(n > 0 && ((uint8_t)s[n] & 0xc0) == 0x80);
		s[n] = 0;
	}
	return 1;
}

/* A press on the entry gives it the focus with the caret at the
 * character boundary nearest to it, and that button's release is the
 * entry's too; a press elsewhere takes the focus away. */
int Edit_Button(OpenBGI_t* o, int button, int down, int x, int y)
{
	EditFrame_t* e = Up(o);
	if(!e || button < 0 || button > 4)
		return 0;
	if(!down)
	{
		if(!(e->pressed & (1 << button)))
			return 0;
		e->pressed &= ~(1 << button);
		return 1;
	}
	if(!Over(e, x, y))
	{
		if(e->focus)
		{
			e->focus = 0;
			Dirty(e);
		}
		return 0;
	}
	Layout(e);
	if(!(e->flags & OPENBGI_ENTRY_MULTILINE))
	{
		UiField_t* f = &e->field;
		int textW = Width(e, 0, (int)strlen(f->text)), tx = TextX(e, textW, Width(e, 0, f->caret));
		f->caret = Nearest(e, 0, tx, x - e->x);
	}
	else
	{
		int i = e->top + (y - e->y) / (e->size > 0 ? e->size : 1);
		if(i >= e->lines)
			i = e->lines - 1;
		e->field.caret = Nearest(e, i, LineX(e, i), x - e->x);
		e->eol = i + 1 < e->lines && e->field.caret == e->line[i + 1];
	}
	e->field.selAll = 0;
	e->focus = 1;
	e->pressed |= 1 << button;
	Dirty(e);
	return 1;
}

// the entry loses the focus to another window (a child window clicked)
void Edit_Unfocus(OpenBGI_t* o)
{
	EditFrame_t* e = Up(o);
	if(e && e->focus)
	{
		e->focus = 0;
		Dirty(e);
	}
}

int Edit_Over(OpenBGI_t* o, int x, int y)
{
	EditFrame_t* e = Up(o);
	return e && Over(e, x, y);
}

int Edit_Rect(OpenBGI_t* o, OpenBGI_Rect_t* out)
{
	EditFrame_t* e = Up(o);
	if(!e || !e->layer)
		return 0;
	out->x = e->x;
	out->y = e->y;
	out->w = e->w;
	out->h = e->h;
	return e->focus ? 2 : 1;
}
