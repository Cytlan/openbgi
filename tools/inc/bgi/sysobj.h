/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sysobj.h - the small script-visible containers the engine keeps
 *
 * One header per container under inc/bgi/sysobj/; this one includes them
 * all for the code that uses several (the "80 8x .. 80 Dx" instruction
 * handlers, the engine's reset):
 *
 *   names.h      the global name table                  GName_*
 *   flags.h      named groups of bit flags              FlagMgr_*, GFlag_*
 *   database.h   the global database file BGI.gdb       GlobalDB_*, SysArea_*
 *   history.h    the message back-log                   History_*
 *   rings.h      bounded most-recent-first record lists Ring_*
 *   locks.h      named counting locks                   SemMgr_*
 *   dicts.h      string-keyed record tables             Dict_*
 *   strtables.h  numbered string lists                  StrTab_*
 *   strgroups.h  named groups of string pairs           StrGroups_*
 *   targets.h    sprites registered as click targets    Target_*
 *   events.h     the event queue for the scripts        MsgQueue_*
 *
 * (The panels addressed by id live in panel.h.)
 */
#ifndef BGI_SYSOBJ_H_
#define BGI_SYSOBJ_H_

#include "bgi/sysobj/names.h"
#include "bgi/sysobj/flags.h"
#include "bgi/sysobj/database.h"
#include "bgi/sysobj/history.h"
#include "bgi/sysobj/rings.h"
#include "bgi/sysobj/locks.h"
#include "bgi/sysobj/dicts.h"
#include "bgi/sysobj/strtables.h"
#include "bgi/sysobj/strgroups.h"
#include "bgi/sysobj/targets.h"
#include "bgi/sysobj/events.h"

#endif // BGI_SYSOBJ_H_
