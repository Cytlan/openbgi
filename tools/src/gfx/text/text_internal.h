/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * text_internal.h - the common includes of the text engine's files
 *                   (the files of src/gfx/text); the public interface is
 *                   inc/bgi/gfx/text.h
 *
 * Every file of the engine includes this one: the engine's own header, the
 * graphics and bitmap managers it draws through, the Shift-JIS string
 * helpers, the engine generation (many rules are gated on the build) and
 * the MSG_TXT_* character-class tables.
 */
#ifndef BGI_GFX_TEXT_INTERNAL_H
#define BGI_GFX_TEXT_INTERNAL_H

#include <string.h>
#include <stdio.h>

#include "bgi/gfx/text.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/strutil.h"
#include "bgi/version.h"
#include "bgi/msg.h"

#endif
