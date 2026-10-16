/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * text.c - the text engine's settings: the pacing (cursor blink, character
 *          and scroll intervals, fades, auto-advance), the shadow / outline
 *          style, the layer mode, the rich-layout parameters and the
 *          numbered "text functions" (inc/bgi/gfx/text.h)
 *
 * Everything here is a global the "90 9x" / "91 9x" instructions set and
 * the layout (layout_*.c) and the text wait classes (src/wait/wait_text.c)
 * read; a setting takes effect with the next output.  The glyph sheet and
 * cursor pictures are in glyphs.c, the ruby dictionary in ruby.c, the
 * string helpers in strings.c.
 */
#include "text_internal.h"

// ---- the engine's manager pointers -------------------------------------------------------------

Gfx_t* gTextGfx;
BmpMgr_t* gTextBmpMgr;

void Text_SetGfxPtr(Gfx_t* g)
{
	gTextGfx = g;
}

void Text_SetBmpMgrPtr(BmpMgr_t* m)
{
	gTextBmpMgr = m;
}

// ---- globals -----------------------------------------------------------------------------------

Bmp_t gGlyphSheet;
int32_t gGlyphCount;
int32_t gGlyphCellW;
int32_t gTextItemCount;
Bmp_t* gTextItems;
int32_t gCursorInterval;
int32_t gCharInterval;
int32_t gScrollSteps;
int32_t gScrollInterval;
int32_t gFadeSteps;
int32_t gFadeInterval;
int32_t gTextStyle[5];
int32_t gCursorPosMode;
int32_t gCursorX;
int32_t gCursorY;
int32_t gStartDelayOn;
int32_t gStartDelayMs;
int32_t gAutoAdvanceOn;
int32_t gAutoAdvanceMs;
int32_t gTextInputFinishes;
int32_t gTextLayerMode;
int32_t gTextLayerNo;
RubyDict_t gRubyDict;
int32_t gRubyCharDelay = 25; // the rich output's defaults until "91 98" sets them
int32_t gRubyFadeSteps = 30;
int32_t gRubySizePct = 40;
int32_t gCharPitch;
int32_t gHangBrackets;
int32_t gTextIndent;

// ---- pacing and style setters ------------------------------------------------------------------

void Text_SetCursorInterval(int ms) // "90 99"
{
	gCursorInterval = ms;
}

void Text_SetCursorPos(int mode, int x, int y) // "90 9A": mode 1 = draw the cursor picture at (x, y)
{
	gCursorPosMode = mode;
	gCursorX = x;
	gCursorY = y;
}

void Text_SetStartDelay(int on, int ms) // "90 9B"
{
	gStartDelayOn = on;
	gStartDelayMs = ms;
}

void Text_SetCharInterval(int ms) // "90 94"
{
	gCharInterval = ms;
}

// "90 95": a line scroll takes `steps` steps of `ms` each; 1 stored, 0 when steps <= 0
int Text_SetScroll(int steps, int ms)
{
	if(steps > 0)
	{
		gScrollSteps = steps;
		gScrollInterval = ms;
	}
	return steps > 0;
}

// "90 96": a page-break fade takes `steps` steps of `ms` each; 1 stored, 0 when steps <= 0
int Text_SetFade(int steps, int ms)
{
	if(steps > 0)
	{
		gFadeSteps = steps;
		gFadeInterval = ms;
	}
	return steps > 0;
}

void Text_SetAutoAdvance(int on, int ms) // "90 97"
{
	gAutoAdvanceOn = on;
	gAutoAdvanceMs = ms;
}

void Text_SetStyleOn(int on) // "90 9C": the shadow of the plain output
{
	gTextStyle[0] = on;
}

/* "90 9D": the shadow offsets in percent of the font size (0..100 each) and
 * its density 0..0x100 (0x100 opaque); the shadow colour is reset to 0,
 * the dimmed glyph.  1 ok, 0 out of range (nothing stored). */
int Text_SetStyleParams(int dxPct, int dyPct, int level)
{
	if(dxPct < 0 || dxPct > 100 || dyPct < 0 || dyPct > 100 || (uint32_t)level > 0x100)
		return 0;
	gTextStyle[4] = level;
	gTextStyle[1] = dxPct;
	gTextStyle[2] = dyPct;
	gTextStyle[3] = 0;
	return 1;
}

void Text_GetStyle(int32_t out[5])
{
	memcpy(out, gTextStyle, sizeof gTextStyle);
}

/* build a style block {on, dx%, dy%, shadowColour, level} as "92 90" /
 * "92 91" / "92 9C" pass it explicitly; on == 0 clears it.  A shadowColour
 * of 0 means "dimmed copy of the glyph".  1.494 on accepts `on` == 2 as
 * well, the outline style, and refuses other values; the earlier builds
 * store any non-zero `on` as 1.  1 ok, 0 bad range (nothing stored). */
int Text_MakeStyle(int32_t out[5], int on, int dxPct, int dyPct, uint32_t shadowColour, int level)
{
	if(!on)
	{
		memset(out, 0, 5 * sizeof(int32_t));
		return 1;
	}
	if(gEngine->gen >= GEN_1_494 && (uint32_t)on > 2)
		return 0;
	if(dxPct < 0 || dxPct > 100 || dyPct < 0 || dyPct > 100 || (uint32_t)level > 0x100)
		return 0;
	out[0] = gEngine->gen >= GEN_1_494 ? on : 1;
	out[1] = dxPct;
	out[2] = dyPct;
	out[3] = (int32_t)shadowColour;
	out[4] = level;
	return 1;
}

void Text_SetInputFinishes(int f) // "90 9F"
{
	gTextInputFinishes = f;
}

/* "90 91": which input layer the text output listens on: `mode` 0 = the
 * standard layer 2, 1 = layer `layerNo` (< 0x1000), 2 = the window's
 * priority.  0 ok, 0x80000004 unknown mode, 0x80000005 bad layer number. */
int Text_SetLayerMode(int mode, int layerNo)
{
	switch(mode)
	{
		case 0:
		case 2:
			gTextLayerMode = mode;
			return 0;
		case 1:
			if((uint32_t)layerNo >= 0x1000u)
				return (int)0x80000005;
			gTextLayerNo = layerNo;
			gTextLayerMode = 1;
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
int Text_SetRichParams(int charDelay, int fadeSteps, int pitch, int sizePct, int indent, int hangBrackets)
{
	if(sizePct < 0x19 || sizePct > 0x64)
		return (int)0x80000001;
	if(indent < 0)
		return (int)0x80000002;
	gRubyCharDelay = charDelay;
	gRubyFadeSteps = (uint32_t)fadeSteps > 0 ? fadeSteps : 1;
	gTextIndent = indent;
	gCharPitch = pitch;
	gRubySizePct = sizePct;
	gHangBrackets = hangBrackets;
	return 0;
}

int32_t Text_GetIndent(void)
{
	return gTextIndent;
}

// ---- the text functions of 1.573 on ("91 9A") --------------------------------------------------

int32_t gTextFunc[TEXT_FUNC_MAX] = {1, 0, 0x10000, 0, 0, 0, 0, 0, 1, 0, 0, 0}; // the defaults as a 1.659 build has them
int32_t gTextLastPos[2];                                                       // "92 9B" 0x100: the pen after the last layout into a bitmap
uint32_t gTextLastColour;                                                      // "92 9B" 0x101: the colour in effect at its end
static int32_t gTextScaleRecip;                                                // "91 99": 16.16 reciprocal of the divisor

/* "91 9A": store the parameter of text function `func` (0, or 0x80000000 +
 * n) in gTextFunc.  Each build accepts the numbers it had: none before
 * 1.573, function 0 only up to 1.616, up to 0x80000001 before 1.640, up to
 * 0x80000003 before 1.653, up to 0x80000008 before 1.659, up to 0x80000009
 * before 1.669, up to 0x8000000A from then on.  0 ok, 0x80000007 unknown
 * number, 0x80000008 bad parameter.  Function 0x80000001 refuses a negative
 * parameter; 0x80000005 takes only 0 and 1 and silently ignores the rest. */
uint32_t Text_SetFunction(uint32_t func, int32_t param)
{
	int count, idx;
	EngineGen_t gen = gEngine->gen;
	if(gen < GEN_1_573)
		count = 0;
	else if(gen < GEN_1_616)
		count = 1;
	else if(gen < GEN_1_640)
		count = 3;
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
			gTextFunc[idx] = param;
		return 0;
	}
	gTextFunc[idx] = param;
	return 0;
}

/* "91 99" of 1.529 on: a divisor the later text engines apply to glyph
 * measures through its 16.16 reciprocal (x * recip >> 16); 0 turns it off.
 * 1 ok, 0 for a negative value.  The reciprocal is only stored here: no
 * measure of this engine applies it. */
int Text_SetScaleDivisor(int32_t v)
{
	if(v < 0)
		return 0;
	gTextScaleRecip = v ? (int32_t)((0x10000u + (uint32_t)v - 1) / (uint32_t)v) : 0;
	return 1;
}
