// This file is part of OpenBGI (https://openbgi.net).
// SPDX-License-Identifier: GPL-2.0-only
//
// settings.inc - the settings over a running game: the second way launcher.bpc runs.
//
// The engine of a game brings this program up over the game's picture when the player asks
// for the settings (the settings button of the on-screen controls; docs/dev/launcher.md).
// It runs in an engine of its own, which says so: the setting "mode" of the path "" reads
// "settings" there.  The picture is as large as what the host draws on ("surface": its
// width, its height, its pixels an inch) and see-through (obgi.see_through): the game is
// seen around the panel and goes on while the player changes how it is shown.
//
// The panel is a title with Close beside it, and under them a list of rows:
//
//   Filter                how the host scales the picture: the game's own choice, bicubic,
//                         bilinear, nearest, and the host's own filters ("filters", "filter")
//   High quality          the larger networks of a filter of the host's own ("quality")
//   Whole steps only      the picture only at whole multiples of its size ("whole")
//   On-screen pad         the pad of the on-screen controls ("pad")
//   Touch beside the pad  whether a touch beside the pad reaches the game ("through")
//   Performance display   the engine's figures of its own run, over everything ("perf")
//   Frame limiter         no more frames a second than the screen shows; off to measure ("limit"; where the host can)
//   Close the game        as the window's close button: the game asks, the launcher follows
//                         ("game")
//
// Each is a setting of the path "" that reaches the game's engine (docs/modding/obgi.md); the
// three of the picture are left out where the host does not scale it, the last where the
// engine under the panel runs the launcher.  Setting "panel" to "close" puts the panel away.
//
// The list scrolls when the screen is too low for it.  A finger on it is the left button
// held (obgi.touch_drag): lifted where it went down it is a tap on the row; moved further
// than a finger slips, the list goes with it, and on for a little after it lifts.
//
// Input: a touch screen, the keys (arrows, Enter, Escape), a mouse and its wheel, and a pad,
// whose controls type those keys (pad_keys); the Back button is the right mouse button and
// closes.

#define BMP_SET         200         // the panel: its box, the title, Close
#define BMP_SET_LIST    201         // the rows, all of them, one under the other
#define BMP_SET_VIEW    202         // the part of them the panel shows
#define BMP_SET_BACK    203         // the filter's two arrows, drawn once
#define BMP_SET_NEXT    204
#define PRIO_SET        200
#define SET_DP          160         // pixels an inch at which the layout has a scale of 100
#define SET_PANEL_W     480         // the panel's width at that scale,
#define SET_HEAD_H      60          // .. its title's height,
#define SET_ROW_H       54          // .. and a row's
#define SET_AROUND      34          // .. and what the panel's height has besides: its foot, the room around it
#define SET_SHRINK      85          // percent of its scale the panel may shrink to before the list scrolls
#define SET_MAX_ROWS    8
#define SET_MAX_FILTERS 12
#define COL_SET_PANEL   0xe80e131c
#define COL_SET_EDGE    0x58ffffff
#define COL_SET_FOCUS   0x38ffffff
#define COL_SET_PRESS   0x24ffffff
#define COL_SET_LINE    0x18ffffff
#define COL_SET_ON      0x4f8dff
#define COL_SET_OFF     0xff596273
#define COL_SET_WARN    0xff8a7a

// the rows
#define ROW_FILTER      0
#define ROW_QUALITY     1
#define ROW_WHOLE       2
#define ROW_PAD         3
#define ROW_THROUGH     4
#define ROW_CLOSE_GAME  5
#define ROW_PERF        6
#define ROW_LIMIT       7

// what a finger went down on, or the keys' focus is on: a row's number, or
#define SET_NOTHING     -2
#define SET_CLOSE       -1          // Close, beside the title

global int gSetSpr @ G_VARS + 0x400;        // the panel's sprite,
global int gSetListSpr @ G_VARS + 0x404;    // .. and the list's
global int gSetX @ G_VARS + 0x408;          // where the panel lies in the picture, and its size
global int gSetY @ G_VARS + 0x40c;
global int gSetPW @ G_VARS + 0x410;
global int gSetPH @ G_VARS + 0x414;
global int gSetHeadH @ G_VARS + 0x418;      // the title's height: the list begins under it
global int gSetListX @ G_VARS + 0x41c;      // the list, in the panel: its left edge,
global int gSetListW @ G_VARS + 0x420;      // .. its width,
global int gSetViewH @ G_VARS + 0x424;      // .. and how much of its height is seen
global int gSetRowH @ G_VARS + 0x428;
global int gSetRows @ G_VARS + 0x42c;       // the number of rows
global int gSetScroll @ G_VARS + 0x430;     // how far the list is moved up, in pixels
global int gSetMaxScroll @ G_VARS + 0x434;
global int gSetBtnX @ G_VARS + 0x438;       // Close, in the panel
global int gSetBtnY @ G_VARS + 0x43c;
global int gSetBtnW @ G_VARS + 0x440;
global int gSetBtnH @ G_VARS + 0x444;
global int gSetFocus @ G_VARS + 0x448;      // where the keys' focus is
global int gSetPress @ G_VARS + 0x44c;      // what the finger that is down went down on
global int gSetPressY @ G_VARS + 0x450;     // .. where,
global int gSetPressScroll @ G_VARS + 0x454; // .. and how far the list was moved then
global int gSetDrag @ G_VARS + 0x458;       // the finger moved the list: no tap comes of it
global int gSetLastY @ G_VARS + 0x45c;      // the finger a turn ago,
global int gSetVel @ G_VARS + 0x460;        // .. and how fast it moves the list, pixels a turn
global int gSetFling @ G_VARS + 0x464;      // the list still moving after the finger lifted, pixels a turn
global int gSetDpi @ G_VARS + 0x468;        // the surface's pixels an inch
global int gSetClosed @ G_VARS + 0x46c;     // the panel asked to be put away
global int gSetPad @ G_VARS + 0x470;        // what the rows show: the pad,
global int gSetThrough @ G_VARS + 0x474;    // .. touches beside it,
global int gSetWhole @ G_VARS + 0x478;      // .. whole steps,
global int gSetHigh @ G_VARS + 0x47c;       // .. the quality,
global int gSetFilter @ G_VARS + 0x480;     // .. the filter (an index of gSetFilters),
global int gSetFilterN @ G_VARS + 0x484;    // .. of so many (0: the host does not scale the picture)
global int gSetIsGame @ G_VARS + 0x488;     // a game runs under the panel (not the launcher)
global int gSetListAt @ G_VARS + 0x48c;     // the list as it was drawn into the view last (gSetScroll then; -1: not)
global int gSetPanelKey @ G_VARS + 0x490;   // what the panel was drawn for (settings_panel; -1: it is to be drawn),
global int gSetListKey @ G_VARS + 0x494;    // .. and the rows (settings_list)
global int gSetPerf @ G_VARS + 0x498;       // what the performance display's row shows
global int gSetLimit @ G_VARS + 0x49c;      // what the frame limit's row shows
global int gSetLimits @ G_VARS + 0x4a0;     // the host can hold its frames to the screen: there is such a row
global int gSetRow[SET_MAX_ROWS] @ G_VARS + 0x500;              // the rows, top to bottom: ROW_...
global char gSetFilters[SET_MAX_FILTERS][32] @ G_VARS + 0x600;  // the filters' names as the engine has them

settings_run() {
    settings_init();
    while (1) {
        gNow = sys.get_ticks();
        settings_follow();
        settings_input();
        gfx0.present(0);
        nap(14);
    }
}

// A value of the path "" as a number, `none` when there is none.
settings_num(char* key, i, none) {
    if (OBGI_CFG_GET("", key, i, gCfgTmp, 16) <= 0) return none;
    return to_int(gCfgTmp);
}

// Whether a value of the path "" reads `word`.
settings_is(char* key, char* word) {
    return OBGI_CFG_GET("", key, 0, gCfgTmp, 16) > 0 && streq(gCfgTmp, word);
}

settings_init() {
    obgi.text_direction(2);
    gfx0.set_fps(60);
    gfx0.wait_present_mode(0, 0);
    sys.set_idle_sleep(4);
    gfx0.set_antialias(1);
    strings_en();
    gLang = 0;
    if (OBGI_CFG_GET("", "language", 0, gTmp1, 8) > 0 && streq(gTmp1, "ja")) gLang = 1;
    if (gLang == 1) strings_ja();
    gFont = ext0.font_register(FONT_FACE);
    pad_keys();
    sys.input_layer_push(PRIO_LAYER);
    obgi.see_through(1);                    // before the picture is made: it has an alpha channel
    obgi.touch_drag(1);                     // a finger is the button, held: the list goes with it
    gSetSpr = new_sprite();
    gSetListSpr = new_sprite();
    gSetFocus = 0;
    gSetPress = SET_NOTHING;
    gSetListAt = -1;
    gSetPanelKey = -1;
    gSetListKey = -1;
    settings_read();
    gW = 0;
    gH = 0;
    settings_follow();
    sys.show_window(1);
}

// What the rows show, from the game's engine; the rows there are.
settings_read() { int i; int n;
    gSetPad = settings_is("pad", "on");
    gSetThrough = settings_is("through", "on");
    gSetPerf = settings_is("perf", "on");
    gSetLimits = OBGI_CFG_GET("", "limit", 0, gCfgTmp, 16) > 0;
    gSetLimit = gSetLimits && streq(gCfgTmp, "on");
    gSetWhole = settings_is("whole", "on");
    gSetHigh = settings_is("quality", "high");
    gSetIsGame = settings_is("game", "game");
    gSetFilterN = 0;
    while (gSetFilterN < SET_MAX_FILTERS && OBGI_CFG_GET("", "filters", gSetFilterN, gSetFilters[gSetFilterN], 32) > 0) {
        gSetFilterN = gSetFilterN + 1;
    }
    gSetFilter = 0;
    if (OBGI_CFG_GET("", "filter", 0, gTmp1, 32) > 0) {
        for (i = 0; i < gSetFilterN; i = i + 1) {
            if (streq(gSetFilters[i], gTmp1)) gSetFilter = i;
        }
    }
    n = 0;
    if (gSetFilterN > 0) {
        gSetRow[0] = ROW_FILTER;
        gSetRow[1] = ROW_QUALITY;
        gSetRow[2] = ROW_WHOLE;
        n = 3;
    }
    gSetRow[n] = ROW_PAD;
    gSetRow[n + 1] = ROW_THROUGH;
    gSetRow[n + 2] = ROW_PERF;
    n = n + 3;
    if (gSetLimits) {
        gSetRow[n] = ROW_LIMIT;
        n = n + 1;
    }
    if (gSetIsGame) {
        gSetRow[n] = ROW_CLOSE_GAME;
        n = n + 1;
    }
    gSetRows = n;
}

// The picture is as large as the surface, and the panel is laid out for it - at the start,
// and again when the surface is another (the device was turned).  The pad's switch follows
// the pad, which its own button changes as well.
settings_follow() { int w; int h; int dpi; int pad; int content; int room; int fit;
    w = settings_num("surface", 0, BASE_W);
    h = settings_num("surface", 1, BASE_H);
    dpi = settings_num("surface", 2, SET_DP);
    pad = settings_is("pad", "on");
    if (w == gW && h == gH && dpi == gSetDpi) {
        if (pad != gSetPad) {
            gSetPad = pad;
            settings_list();
        }
        return;
    }
    gW = w;
    gH = h;
    gSetDpi = dpi;
    gSetPad = pad;
    sys81.set_size_entry(0, w, h);
    sys.set_display_mode(0, 1, 0);
    // a finger's size whatever the screen's density; smaller where the panel is too wide, a
    // little smaller where that shows all the rows, and never so large that two do not fit
    gScale = iclamp(dpi * 100 / SET_DP, 100, 400);
    gScale = imin(gScale, (w - 16) * 100 / SET_PANEL_W);
    fit = h * 100 / (SET_HEAD_H + gSetRows * SET_ROW_H + SET_AROUND);
    if (fit < gScale) gScale = imax(fit, gScale * SET_SHRINK / 100);
    gScale = imin(gScale, h * 100 / (SET_HEAD_H + 2 * SET_ROW_H + SET_AROUND));
    gScale = imax(gScale, 40);
    gSetPW = px(SET_PANEL_W);
    gSetHeadH = px(SET_HEAD_H);
    gSetRowH = px(SET_ROW_H);
    gSetListX = px(10);
    gSetListW = gSetPW - 2 * gSetListX;
    gSetBtnW = px(112);
    gSetBtnH = px(40);
    gSetBtnX = gSetPW - px(18) - gSetBtnW;
    gSetBtnY = (gSetHeadH - gSetBtnH) / 2 + px(2);
    // as much of the list as the screen has room for; the rest is scrolled to
    content = gSetRows * gSetRowH;
    room = h - 2 * px(12) - gSetHeadH - px(10);
    gSetViewH = imin(content, imax(room, gSetRowH));
    gSetMaxScroll = content - gSetViewH;
    gSetScroll = iclamp(gSetScroll, 0, gSetMaxScroll);
    gSetPH = gSetHeadH + gSetViewH + px(10);
    gSetX = (w - gSetPW) / 2;
    gSetY = (h - gSetPH) / 2;
    gSetFling = 0;
    gSetPress = SET_NOTHING;
    settings_arrow(BMP_SET_BACK, -1);
    settings_arrow(BMP_SET_NEXT, 1);
    gSetPanelKey = -1;
    gSetListKey = -1;
    settings_panel();
    settings_list();
}

// The panel's box, its title and Close - when Close looks another way than it was drawn.
settings_panel() { int sz; int tw; int bg; int key;
    key = 0;
    if (gSetPress == SET_CLOSE) key = 1;
    if (gKeyMode && gSetFocus == SET_CLOSE) key = 2;
    if (key == gSetPanelKey) return;
    gSetPanelKey = key;
    canvas(BMP_SET, gSetPW, gSetPH);
    box(BMP_SET, 0, 0, gSetPW, gSetPH, px(18), COL_SET_PANEL);
    ring(BMP_SET, gSetPW, gSetPH, px(18), imax(px(1), 1), COL_SET_EDGE);
    text(BMP_SET, px(24), (gSetHeadH - px(25)) / 2, TXT_SET_TITLE, px(25), 1, COL_TEXT);
    bg = COL_BTN;
    if (gSetPress == SET_CLOSE) bg = COL_BTN_HOVER;
    if (gKeyMode && gSetFocus == SET_CLOSE) bg = argb(0xff, rgb_mix(COL_SET_ON, 0x101828, 60));
    box(BMP_SET, gSetBtnX, gSetBtnY, gSetBtnW, gSetBtnH, gSetBtnH / 2, bg);
    sz = px(17);
    tw = text_w(TXT_SET_CLOSE, sz, 0);
    text(BMP_SET, gSetBtnX + (gSetBtnW - tw) / 2, gSetBtnY + (gSetBtnH - sz) / 2 - px(1), TXT_SET_CLOSE, sz, 0, COL_TEXT);
    box(BMP_SET, gSetListX, gSetHeadH - imax(px(1), 1), gSetListW, imax(px(1), 1), 0, COL_SET_LINE);
    show(gSetSpr, gSetX, gSetY, BMP_SET, PRIO_SET);
}

// A row's name.
settings_label(row) {
    if (row == ROW_FILTER) return TXT_SET_FILTER;
    if (row == ROW_QUALITY) return TXT_SET_QUALITY;
    if (row == ROW_WHOLE) return TXT_SET_WHOLE;
    if (row == ROW_PAD) return TXT_SET_PAD;
    if (row == ROW_THROUGH) return TXT_SET_THROUGH;
    if (row == ROW_PERF) return TXT_SET_PERF;
    if (row == ROW_LIMIT) return TXT_SET_LIMIT;
    return TXT_SET_CLOSE_GAME;
}

// A filter as the player reads it: the four every host has by their texts, the host's own
// by the names it gives them.
settings_filter_text(i) {
    if (i == 0) return TXT_FILTER_GAME;
    if (i == 1) return TXT_FILTER_BICUBIC;
    if (i == 2) return TXT_FILTER_BILINEAR;
    if (i == 3) return TXT_FILTER_NEAREST;
    return gSetFilters[i];
}

// Whether a row can be changed now: the quality only of a filter of the host's own, the
// touches beside the pad only while there is a pad.
settings_usable(row) {
    if (row == ROW_QUALITY) return gSetFilter >= 4;
    if (row == ROW_THROUGH) return gSetPad;
    return 1;
}

// Whether a row's switch is on.
settings_on(row) {
    if (row == ROW_QUALITY) return gSetHigh;
    if (row == ROW_WHOLE) return gSetWhole;
    if (row == ROW_PAD) return gSetPad;
    if (row == ROW_PERF) return gSetPerf;
    if (row == ROW_LIMIT) return gSetLimit;
    return gSetThrough;
}

// A bitmap of its own with an arrow head that points left (dir -1) or right: px(16) wide,
// px(20) high.  (A line is drawn through a pattern as large as its bitmap, so the arrows
// are made once, small, and put on the rows from there.)
settings_arrow(bmp, dir) { int s; int x; int y;
    s = px(6);
    x = px(8);
    y = px(10);
    canvas(bmp, px(16), px(20));
    gPts[0] = (x - dir * s / 2) * 256;
    gPts[1] = (y - s) * 256;
    gPts[2] = (x + dir * s / 2) * 256;
    gPts[3] = y * 256;
    gPts[4] = (x - dir * s / 2) * 256;
    gPts[5] = (y + s) * 256;
    line(bmp, 3, imax(px(2), 2) * 256, argb(0xff, COL_TEXT_DIM), 0);
}

// How wide the filter's value is, with its two arrows: a tap on its left third is "the one
// before", anywhere else on the row "the next".
settings_value_w() { return px(210); }

// All the rows into their bitmap, and the part of it that is seen into the view - when
// a row looks another way than it was drawn: its value, the focus, the finger on it.
settings_list() { int i; int row; int y; int m; int sz; int col; int sw; int sh; int sx; int sy; int k; int vw; int tw; int usable;
    k = gSetFilter * 32 + gSetPad + gSetThrough * 2 + gSetWhole * 4 + gSetHigh * 8 + gSetPerf * 16 + gSetLimit * 1024;
    if (gKeyMode) k = k + (gSetFocus + 2) * 4096;
    if (gSetPress >= 0 && !gSetDrag) k = k + (gSetPress + 1) * 65536;
    if (k == gSetListKey) return;
    gSetListKey = k;
    m = px(14);
    canvas(BMP_SET_LIST, gSetListW, imax(gSetRows * gSetRowH, 2));
    for (i = 0; i < gSetRows; i = i + 1) {
        row = gSetRow[i];
        y = i * gSetRowH;
        usable = settings_usable(row);
        if (gKeyMode && gSetFocus == i) {
            box(BMP_SET_LIST, 0, y + px(3), gSetListW, gSetRowH - px(6), px(12), COL_SET_FOCUS);
        } else if (gSetPress == i && !gSetDrag) {
            box(BMP_SET_LIST, 0, y + px(3), gSetListW, gSetRowH - px(6), px(12), COL_SET_PRESS);
        }
        col = COL_TEXT;
        if (!usable) col = COL_TEXT_OFF;
        if (row == ROW_CLOSE_GAME) col = COL_SET_WARN;
        sz = px(19);
        text(BMP_SET_LIST, m, y + (gSetRowH - sz) / 2 - px(1), settings_label(row), sz, 0, col);
        if (row == ROW_FILTER) {            // the value between two arrows
            vw = settings_value_w();
            sx = gSetListW - m - vw;
            gfx0.bmp_blit(BMP_SET_LIST, sx, y + gSetRowH / 2 - px(10), BMP_SET_BACK, 0, 0);
            gfx0.bmp_blit(BMP_SET_LIST, sx + vw - px(16), y + gSetRowH / 2 - px(10), BMP_SET_NEXT, 0, 0);
            sz = px(18);
            fit_end(gFit, settings_filter_text(gSetFilter), sz, 0, vw - px(44));
            tw = text_w(gFit, sz, 0);
            text(BMP_SET_LIST, sx + (vw - tw) / 2, y + (gSetRowH - sz) / 2 - px(1), gFit, sz, 0, COL_TEXT);
        } else if (row != ROW_CLOSE_GAME) { // a switch
            sw = px(58);
            sh = px(31);
            sx = gSetListW - m - sw;
            sy = y + (gSetRowH - sh) / 2;
            k = sh - px(8);
            if (settings_on(row) && usable) {
                box(BMP_SET_LIST, sx, sy, sw, sh, sh / 2, argb(0xff, COL_SET_ON));
                box(BMP_SET_LIST, sx + sw - k - px(4), sy + px(4), k, k, k / 2, 0xffffffff);
            } else if (settings_on(row)) {
                box(BMP_SET_LIST, sx, sy, sw, sh, sh / 2, argb(0x70, COL_SET_ON));
                box(BMP_SET_LIST, sx + sw - k - px(4), sy + px(4), k, k, k / 2, 0x90ffffff);
            } else {
                box(BMP_SET_LIST, sx, sy, sw, sh, sh / 2, usable ? COL_SET_OFF : 0x70596273);
                box(BMP_SET_LIST, sx + px(4), sy + px(4), k, k, k / 2, usable ? 0xffffffff : 0x90ffffff);
            }
        }
    }
    gSetListAt = -1;
    settings_view();
}

// The part of the rows that is seen, where the list is moved to; a mark at the right says
// where that is in all of it when not all is seen.
settings_view() { int th; int ty;
    if (gSetListAt == gSetScroll) return;
    gSetListAt = gSetScroll;
    canvas(BMP_SET_VIEW, gSetListW, gSetViewH);
    gfx0.bmp_copy_rect(BMP_SET_VIEW, 0, 0, BMP_SET_LIST, 0, gSetScroll, gSetListW, gSetViewH);
    if (gSetMaxScroll > 0) {
        th = imax(gSetViewH * gSetViewH / (gSetViewH + gSetMaxScroll), px(16));
        ty = (gSetViewH - th) * gSetScroll / gSetMaxScroll;
        box(BMP_SET_VIEW, gSetListW - px(4), ty, imax(px(3), 2), th, px(2), 0x70ffffff);
    }
    show(gSetListSpr, gSetX + gSetListX, gSetY + gSetHeadH, BMP_SET_VIEW, PRIO_SET + 1);
}

// Move the list to `to` pixels; 0 when it is at an end and went no further.
settings_scroll(to) { int was;
    was = gSetScroll;
    gSetScroll = iclamp(to, 0, gSetMaxScroll);
    settings_view();
    return gSetScroll != was;
}

// Bring row i into what is seen.
settings_reveal(i) {
    if (i < 0) return;
    if (i * gSetRowH < gSetScroll) settings_scroll(i * gSetRowH);
    if ((i + 1) * gSetRowH > gSetScroll + gSetViewH) settings_scroll((i + 1) * gSetRowH - gSetViewH);
}

// What is at (x, y) of the picture: a row's number, SET_CLOSE or SET_NOTHING.
settings_hit(x, y) { int i;
    x = x - gSetX;
    y = y - gSetY;
    if (in_rect(x, y, gSetBtnX - px(10), 0, gSetBtnW + px(28), gSetHeadH)) return SET_CLOSE;
    if (!in_rect(x, y, gSetListX, gSetHeadH, gSetListW, gSetViewH)) return SET_NOTHING;
    i = (y - gSetHeadH + gSetScroll) / gSetRowH;
    if (i < 0 || i >= gSetRows) return SET_NOTHING;
    return i;
}

// The filter `n` after the one that is on (-1: before), round the list.
settings_filter(n) {
    if (gSetFilterN < 1) return;
    gSetFilter = (gSetFilter + n + gSetFilterN) % gSetFilterN;
    OBGI_CFG_SET("", "filter", 0, gSetFilters[gSetFilter]);
}

// A switch to `on` (1, 0) or over (-1).
settings_switch(row, on) {
    if (on < 0) on = !settings_on(row);
    if (row == ROW_QUALITY) {
        gSetHigh = on;
        if (on) { OBGI_CFG_SET("", "quality", 0, "high"); } else { OBGI_CFG_SET("", "quality", 0, "normal"); }
    } else if (row == ROW_WHOLE) {
        gSetWhole = on;
        if (on) { OBGI_CFG_SET("", "whole", 0, "on"); } else { OBGI_CFG_SET("", "whole", 0, "off"); }
    } else if (row == ROW_PAD) {
        gSetPad = on;
        if (on) { OBGI_CFG_SET("", "pad", 0, "on"); } else { OBGI_CFG_SET("", "pad", 0, "off"); }
    } else if (row == ROW_THROUGH) {
        gSetThrough = on;
        if (on) { OBGI_CFG_SET("", "through", 0, "on"); } else { OBGI_CFG_SET("", "through", 0, "off"); }
    } else if (row == ROW_PERF) {
        gSetPerf = on;
        if (on) { OBGI_CFG_SET("", "perf", 0, "on"); } else { OBGI_CFG_SET("", "perf", 0, "off"); }
    } else if (row == ROW_LIMIT) {
        gSetLimit = on;
        if (on) { OBGI_CFG_SET("", "limit", 0, "on"); } else { OBGI_CFG_SET("", "limit", 0, "off"); }
    }
}

// Row i (or Close) was tapped at x of the picture, or chosen with the keys (x -1); `dir`
// is a key's direction (-1 left, 1 right, 0 Enter or a tap).
settings_activate(i, x, dir) { int row;
    if (i == SET_CLOSE) {
        if (dir == 0) settings_close();
        return;
    }
    if (i < 0 || i >= gSetRows) return;
    row = gSetRow[i];
    if (!settings_usable(row)) return;
    if (row == ROW_FILTER) {
        if (dir == 0 && x >= 0 && x - gSetX - gSetListX < gSetListW - px(14) - settings_value_w() * 2 / 3
            && x - gSetX - gSetListX >= gSetListW - px(14) - settings_value_w() - px(20)) {
            dir = -1;                       // the arrow that points back
        }
        if (dir == 0) dir = 1;
        settings_filter(dir);
    } else if (row == ROW_CLOSE_GAME) {
        if (dir == 0 && !gSetClosed) {
            gSetClosed = 1;
            OBGI_CFG_SET("", "game", 0, "close");
        }
        return;
    } else if (dir == 0) {
        settings_switch(row, -1);
    } else {
        settings_switch(row, dir > 0);
    }
    settings_list();
}

// Put the settings away: the game's engine does it when this turn is over.
settings_close() {
    if (gSetClosed) return;
    gSetClosed = 1;
    OBGI_CFG_SET("", "panel", 0, "close");
}

// The keys (or the pad) were used: the focus is drawn from now on.  1 when it was drawn
// already, so that the key does its work; the first one only shows where the focus is.
settings_keys() {
    gSetFling = 0;
    if (gKeyMode) return 1;
    gKeyMode = 1;
    settings_reveal(gSetFocus);
    settings_panel();
    settings_list();
    return 0;
}

settings_focus(to) {
    gSetFocus = iclamp(to, SET_CLOSE, gSetRows - 1);
    settings_reveal(gSetFocus);
    settings_panel();
    settings_list();
}

// The presses of the key `more` less those of the key `less`.  (Each call's value is stored
// before the next call: a function that ran `x = v;` statements has left values on the stack,
// and in `f() - g()` those of g would lie between the two - docs/modding/programs.md.)
settings_trig(more, less) { int a; int b;
    a = trig(more);
    b = trig(less);
    return a - b;
}

settings_input() { int x; int y; int n; int d;
    sys.get_mouse_pos();
    y <- __pop;
    x <- __pop;
    n = sys.key_down(VK_LBUTTON);
    if (n && !gLDown) {                     // the finger (or the button) went down
        gSetFling = 0;
        gSetVel = 0;
        gSetDrag = 0;
        gSetPress = settings_hit(x, y);
        gSetPressY = y;
        gSetPressScroll = gSetScroll;
        gSetLastY = y;
        gKeyMode = 0;
        settings_panel();
        settings_list();
    }
    if (n && gSetPress >= 0) {              // .. is down on the list: past a finger's slip it moves the list
        d = y - gSetPressY;
        if (!gSetDrag && gSetMaxScroll > 0 && (d > px(8) || d < -px(8))) {
            gSetDrag = 1;
            settings_list();
        }
        if (gSetDrag) {
            settings_scroll(gSetPressScroll - d);
            gSetVel = (gSetVel + gSetLastY - y) / 2;
        }
        gSetLastY = y;
    }
    if (!n && gLDown) {                     // .. lifted: a tap where it went down, or the list goes on a little
        d = gSetPress;
        gSetPress = SET_NOTHING;
        if (gSetDrag) {
            gSetFling = gSetVel;
        } else if (d != SET_NOTHING && settings_hit(x, y) == d) {
            settings_activate(d, x, 0);
        }
        gSetDrag = 0;
        if (!gSetClosed) {
            settings_panel();
            settings_list();
        }
    }
    gLDown = n;
    if (!n && gSetFling) {
        if (!settings_scroll(gSetScroll + gSetFling)) gSetFling = 0;
        gSetFling = gSetFling * 15 / 16;
    }
    n = sys.key_down(VK_RBUTTON);
    if (n && !gRDown) settings_close();
    gRDown = n;
    if (trig(VK_ESCAPE)) settings_close();
    n = settings_trig(VK_WHEEL_DOWN, VK_WHEEL_UP);
    if (n) {
        gSetFling = 0;
        settings_scroll(gSetScroll + n * gSetRowH);
    }
    n = settings_trig(VK_DOWN, VK_UP);
    d = trig(VK_TAB);
    n = n + d;
    if (n) {
        if (settings_keys()) settings_focus(gSetFocus + n);
    }
    n = trig(VK_RETURN);
    if (trig(VK_SPACE)) n = 1;
    if (n) {
        if (settings_keys()) settings_activate(gSetFocus, -1, 0);
    }
    n = settings_trig(VK_RIGHT, VK_LEFT);
    if (n) {
        if (settings_keys()) settings_activate(gSetFocus, -1, n);
    }
}
