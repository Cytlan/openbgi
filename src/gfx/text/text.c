/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * text.c - the text engine's settings: the pacing (cursor blink, character
 *          and scroll intervals, fades, auto-advance), the shadow / outline
 *          style, the layer mode, the rich-layout parameters and the
 *          numbered "text functions" (src/include/bgi/gfx/text.h)
 *
 * Everything here is a setting the "90 9x" / "91 9x" instructions set and
 * the layout (layout_*.c) and the text wait classes (src/wait/wait_text.c)
 * read; a setting takes effect with the next output.  The glyph sheet and
 * cursor pictures are in glyphs.c, the ruby dictionary in ruby.c, the
 * string helpers in strings.c.
 */
#include "text_internal.h"
#include "bgi/core.h"

// ---- the engine's manager pointers -------------------------------------------------------------

void Text_SetGfxPtr(OpenBGI_t* o, Gfx_t* g)
{
	o->text.textGfx = g;
}

void Text_SetBmpMgrPtr(OpenBGI_t* o, BmpMgr_t* m)
{
	o->text.textBmpMgr = m;
}

// ---- pacing and style setters ------------------------------------------------------------------

void Text_SetCursorInterval(OpenBGI_t* o, int ms) // "90 99"
{
	o->text.cursorInterval = ms;
}

void Text_SetCursorPos(OpenBGI_t* o, int mode, int x, int y) // "90 9A": mode 1 = draw the cursor picture at (x, y)
{
	o->text.cursorPosMode = mode;
	o->text.cursorX = x;
	o->text.cursorY = y;
}

void Text_SetStartDelay(OpenBGI_t* o, int on, int ms) // "90 9B"
{
	o->text.startDelayOn = on;
	o->text.startDelayMs = ms;
}

void Text_SetCharInterval(OpenBGI_t* o, int ms) // "90 94"
{
	o->text.charInterval = ms;
}

// "90 95": a line scroll takes `steps` steps of `ms` each; 1 stored, 0 when steps <= 0
int Text_SetScroll(OpenBGI_t* o, int steps, int ms)
{
	if(steps > 0)
	{
		o->text.scrollSteps = steps;
		o->text.scrollInterval = ms;
	}
	return steps > 0;
}

// "90 96": a page-break fade takes `steps` steps of `ms` each; 1 stored, 0 when steps <= 0
int Text_SetFade(OpenBGI_t* o, int steps, int ms)
{
	if(steps > 0)
	{
		o->text.fadeSteps = steps;
		o->text.fadeInterval = ms;
	}
	return steps > 0;
}

void Text_SetAutoAdvance(OpenBGI_t* o, int on, int ms) // "90 97"
{
	o->text.autoAdvanceOn = on;
	o->text.autoAdvanceMs = ms;
}

void Text_SetStyleOn(OpenBGI_t* o, int on) // "90 9C": the shadow of the plain output
{
	o->text.textStyle[0] = on;
}

/* "90 9D": the shadow offsets in percent of the font size (0..100 each) and
 * its density 0..0x100 (0x100 opaque); the shadow colour is reset to 0,
 * the dimmed glyph.  1 ok, 0 out of range (nothing stored). */
int Text_SetStyleParams(OpenBGI_t* o, int dxPct, int dyPct, int level)
{
	if(dxPct < 0 || dxPct > 100 || dyPct < 0 || dyPct > 100 || (uint32_t)level > 0x100)
		return 0;
	o->text.textStyle[4] = level;
	o->text.textStyle[1] = dxPct;
	o->text.textStyle[2] = dyPct;
	o->text.textStyle[3] = 0;
	return 1;
}

void Text_GetStyle(OpenBGI_t* o, int32_t out[5])
{
	memcpy(out, o->text.textStyle, sizeof o->text.textStyle);
}

/* build a style block {on, dx%, dy%, shadowColour, level} as "92 90" /
 * "92 91" / "92 9C" pass it explicitly; on == 0 clears it.  A shadowColour
 * of 0 means "dimmed copy of the glyph".  1.492 on accepts `on` == 2 as
 * well, the outline style, and refuses other values; the earlier builds
 * store any non-zero `on` as 1.  1 ok, 0 bad range (nothing stored). */
int Text_MakeStyle(OpenBGI_t* o, int32_t out[5], int on, int dxPct, int dyPct, uint32_t shadowColour, int level)
{
	if(!on)
	{
		memset(out, 0, 5 * sizeof(int32_t));
		return 1;
	}
	if(o->engine.profile->gen >= GEN_1_492 && (uint32_t)on > 2)
		return 0;
	if(dxPct < 0 || dxPct > 100 || dyPct < 0 || dyPct > 100 || (uint32_t)level > 0x100)
		return 0;
	out[0] = o->engine.profile->gen >= GEN_1_492 ? on : 1;
	out[1] = dxPct;
	out[2] = dyPct;
	out[3] = (int32_t)shadowColour;
	out[4] = level;
	return 1;
}

void Text_SetInputFinishes(OpenBGI_t* o, int f) // "90 9F"
{
	o->text.textInputFinishes = f;
}

/* "90 91": which input layer the text output listens on: `mode` 0 = the
 * standard layer 2, 1 = layer `layerNo` (< 0x1000), 2 = the window's
 * priority.  0 ok, 0x80000004 unknown mode, 0x80000005 bad layer number. */
int Text_SetLayerMode(OpenBGI_t* o, int mode, int layerNo)
{
	switch(mode)
	{
		case 0:
		case 2:
			o->text.textLayerMode = mode;
			return 0;
		case 1:
			if((uint32_t)layerNo >= (o->engine.profile->gen >= GEN_1_529 ? 0x10000u : 0x1000u)) // a priority: 0x10000 of them from 1.529 on
				return (int)0x80000005;
			o->text.textLayerNo = layerNo;
			o->text.textLayerMode = 1;
			return 0;
		default:
			return (int)0x80000004;
	}
}

/* "91 98": the parameters of the rich output - the reveal delay added per
 * character and the steps of its fade-in (at least 1), the extra pixels
 * after every fixed-pitch character, the ruby size in percent of the font
 * (25..100), the indent every line starts with when ruby is on, and
 * whether a leading bracket hangs into the margin.  0 ok, 0x80000001 bad
 * size percentage, 0x80000002 negative indent (nothing stored). */
int Text_SetRichParams(OpenBGI_t* o, int charDelay, int fadeSteps, int pitch, int sizePct, int indent, int hangBrackets)
{
	if(sizePct < 0x19 || sizePct > 0x64)
		return (int)0x80000001;
	if(indent < 0)
		return (int)0x80000002;
	o->text.rubyCharDelay = charDelay;
	o->text.rubyFadeSteps = (uint32_t)fadeSteps > 0 ? fadeSteps : 1;
	o->text.textIndent = indent;
	o->text.charPitch = pitch;
	o->text.rubySizePct = sizePct;
	o->text.hangBrackets = hangBrackets;
	return 0;
}

int32_t Text_GetIndent(OpenBGI_t* o)
{
	return o->text.textIndent;
}

// ---- the text functions of 1.573 on ("91 9A") --------------------------------------------------

/* "91 9A": store the parameter of text function `func` (0, or 0x80000000 +
 * n) in o->text.textFunc.  Each build accepts the numbers it had: none
 * before 1.573, function 0 only before 1.616, up to 0x80000001 in 1.616,
 * up to 0x80000002 in 1.636, up to 0x80000003 before 1.653, up to
 * 0x80000008 before 1.659, up to 0x80000009 before 1.669, up to 0x8000000A
 * from then on.  0 ok, 0x80000007 unknown number, 0x80000008 bad
 * parameter.  Function 0x80000001 refuses a negative parameter; 0x80000005
 * takes only 0 and 1 and silently ignores the rest. */
uint32_t Text_SetFunction(OpenBGI_t* o, uint32_t func, int32_t param)
{
	int count, idx;
	EngineGen_t gen = o->engine.profile->gen;
	if(gen < GEN_1_573)
		count = 0;
	else if(gen < GEN_1_616)
		count = 1;
	else if(gen < GEN_1_636)
		count = 3;
	else if(gen < GEN_1_640)
		count = 4;
	else if(gen < GEN_1_653)
		count = 5;
	else if(gen < GEN_1_659)
		count = 10;
	else if(gen < GEN_1_669)
		count = 11;
	else
		count = 12;
	if(func == 0)
		idx = 0;
	else if(func >= 0x80000000u && func - 0x80000000u < (uint32_t)(TEXT_FUNC_MAX - 1))
		idx = 1 + (int)(func - 0x80000000u);
	else
		return 0x80000007;
	if(idx >= count)
		return 0x80000007;
	if(idx == 2 && param < 0)
		return 0x80000008;
	if(idx == 6)
	{
		if((uint32_t)param < 2)
			o->text.textFunc[idx] = param;
		return 0;
	}
	o->text.textFunc[idx] = param;
	return 0;
}

/* "91 99" of 1.529 on: a divisor the later text engines apply to glyph
 * measures through its 16.16 reciprocal (x * recip >> 16); 0 turns it off.
 * 1 ok, 0 for a negative value.  The reciprocal is only stored here: no
 * measure of this engine applies it. */
int Text_SetScaleDivisor(OpenBGI_t* o, int32_t v)
{
	if(v < 0)
		return 0;
	o->text.textScaleRecip = v ? (int32_t)((0x10000u + (uint32_t)v - 1) / (uint32_t)v) : 0;
	return 1;
}
