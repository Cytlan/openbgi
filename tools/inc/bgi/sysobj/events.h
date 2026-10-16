/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * events.h - the engine's event queue for the scripts
 *
 * Three-dword events, first in first out, that the engine posts and the
 * scripts drain with "80 A0" (one event per call).  The first dword is
 * the kind, the other two its arguments; the kinds the engine posts are
 *
 *     0           the script's own post, "80 A1" (a, b)
 *     1           the display style changed (1 fullscreen, 0 windowed)
 *     2           the window's close box in close mode 0 (the script decides)
 *     3           a key or button went down (the virtual key: mouse buttons
 *                 1 left, 2 right, 4 middle, 5 / 6 the X buttons, 0x0e /
 *                 0x0f the wheel up / down)
 *     0x10        a file was dropped on the window ("80 6D" reads it)
 *     0x80, 0x81  a double click of the left / right button (0x81 also for
 *                 every right button press)
 *     0x1000      a knob was grabbed, 0x1001 released (the knob handle)
 *     0x10000001  a menu wait ended (thread id, the selected item),
 *     0x10000002  its highlight moved (thread id, the item under the mouse)
 *     0x20000001  an icon wait's focus moved (thread id, focus flag << 16 |
 *                 icon index)
 */
#ifndef BGI_SYSOBJ_EVENTS_H_
#define BGI_SYSOBJ_EVENTS_H_

#include "bgi/common.h"

// drop every event (the engine's reset)
void MsgQueue_Clear(void);
// append the event (a, b, c)
void MsgQueue_Post(uint32_t a, uint32_t b, uint32_t c);
// the oldest event into out[3] ("80 A0"); 1 when there was one
int MsgQueue_Get(uint32_t out[3]);

#endif // BGI_SYSOBJ_EVENTS_H_
