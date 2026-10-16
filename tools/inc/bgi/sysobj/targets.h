/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * targets.h - sprites registered as click targets
 *
 * "90 FA" registers a sprite, "90 FB" removes it (as does deleting the
 * sprite, "90 51"), "90 FC" asks which target is under the mouse, "90 FD"
 * reads whether a target was pressed during the last pass, "90 F8" clears
 * the set.  A registered sprite also gets a mouse layer that follows it,
 * so that the input layer's hit test knows about it; Target_UpdateAll
 * samples the buttons over that layer once per pass of the scheduler.
 * Targets are addressed by their index, the running number of their
 * registration since the last clear.
 */
#ifndef BGI_SYSOBJ_TARGETS_H_
#define BGI_SYSOBJ_TARGETS_H_

#include "bgi/common.h"

// unregister every target and restart the indices at 0 ("90 F8", the engine's reset)
void Target_Clear(void);
// register the sprite with handle `id` ("90 FA"); 1 ok, 0 when the handle is not a sprite
int Target_Add(uint32_t id);
// unregister the sprite with handle `id` ("90 FB"); 1 ok, 0 when it was not registered
int Target_Remove(uint32_t id);
// the index of the first target under the mouse ("90 FC"), 0xffffffff for none
uint32_t Target_Hit(void);
// the last sampled press state (1 pressed) of the target with index `idx` into *out ("90 FD"); 1 ok, 0 for an unknown index
int Target_Get(uint32_t* out, uint32_t idx);
// sample the mouse buttons over every target (once per pass of the scheduler)
void Target_UpdateAll(void);

#endif // BGI_SYSOBJ_TARGETS_H_
