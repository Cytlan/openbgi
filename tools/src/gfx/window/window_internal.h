/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window_internal.h - shared between the files of the message Window
 *                     display object (window.c, window_layers.c,
 *                     window_text.c); the public interface is
 *                     inc/bgi/gfx/window.h
 *
 * The three files split the object by layer: window.c owns the object
 * itself and the composite, window_layers.c the frame, the items and the
 * sub-sprites, window_text.c the text layer with its font, area and
 * cursor.  They share the blit result mapping below.
 */
#ifndef BGI_GFX_WINDOW_INTERNAL_H
#define BGI_GFX_WINDOW_INTERNAL_H

#include <string.h>

#include "bgi/gfx/window.h"
#include "bgi/gfx/sprite.h"
#include "bgi/gfx/text.h"
#include "bgi/gfx/font.h"
#include "bgi/file.h"
#include "bgi/version.h" // the size limits changed with the builds

/* Map a Bmp_Blit result onto the window's result codes: 0 ok, 1..4 ->
 * 4..7 (incompatible modes, unknown effect, bad level, nothing
 * overlapped); `fallback` for anything else. */
int Window_BlitResult(int r, int fallback);

#endif
