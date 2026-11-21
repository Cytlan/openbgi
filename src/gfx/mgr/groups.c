/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * groups.c - the graphics manager's group operations: the "90 Ex"
 *            instructions, and the general attach / detach of "91 3E" /
 *            "91 3F" (src/include/bgi/gfx/gfxmgr.h)
 *
 * A group is an invisible parent: the objects added to it follow its
 * position at their own offsets and inherit its level and visibility.  It
 * is not drawn, so it is not in the compositor.  Eight slots, addressed by
 * H_GROUP | index.  From 1.492 on any object may be the owner of another
 * (Gfx_ObjAttach), which uses the same child mechanism.
 */
#include "mgr_internal.h"

void Gfx_DeleteAllGroups(OpenBGI_t* o, Gfx_t* g)
{
	Mgr_DeleteAll(o, g->groups, GFX_GROUPS, &g->groupCount, &g->groupNextId);
}

/* "90 E0": a new group in the first free slot; not drawn, so not added
 * to the compositor.  Returns the handle, 0 when all 8 are in use. */
uint32_t Gfx_GroupCreate(OpenBGI_t* o, Gfx_t* g)
{
	Group_t* grp;
	uint32_t i;

	if(g->groupCount >= GFX_GROUPS)
		return 0;
	i = Mgr_FreeSlot(g->groups);
	grp = (Group_t*)BGI_Calloc(sizeof(Group_t));
	Group_Ctor(o, grp, g->groupNextId++);
	g->groups[i] = &grp->obj;
	g->groupCount++;
	return H_GROUP | i;
}

// "90 E1": groups are not in the compositor and leave no footprint, so nothing is repainted.  1 ok, 0 unknown handle
int Gfx_GroupDelete(OpenBGI_t* o, Gfx_t* g, uint32_t h)
{
	DispObj_t* obj = Gfx_FindGroup(g, h);
	uint32_t idx;

	if(!obj)
		return 0;
	idx = HANDLE_INDEX(h);
	if(g->groups[idx])
		g->groups[idx]->vt->destroy(o, g->groups[idx], 1);
	g->groups[idx] = NULL;
	g->groupCount--;
	return 1;
}

// "90 E4": show / hide (the children follow); 1 ok, 0 unknown handle
int Gfx_GroupShow(OpenBGI_t* o, Gfx_t* g, uint32_t h, int on)
{
	return Mgr_ShowBracket(o, Gfx_FindGroup(g, h), on);
}

// "90 E5": position and level of the group (its children follow); 1 ok, 0 unknown handle
int Gfx_GroupSet(OpenBGI_t* o, Gfx_t* g, uint32_t h, int x, int y, int level)
{
	DispObj_t* obj = Gfx_FindGroup(g, h);

	if(!obj)
		return 0;
	InvalidateIfVisible(o, obj);
	obj->vt->setPos(o, obj, x, y);
	obj->vt->setLevel(o, obj, level);
	InvalidateIfVisible(o, obj);
	return 1;
}

/* "90 E8": attach hObj to the group at offset (dx, dy).  0 ok, 1 the
 * object does not resolve, 3 the object is the group itself, 4 the attach
 * failed (the object already has an owner), 0xFF unknown group. */
int Gfx_GroupAdd(OpenBGI_t* o, Gfx_t* g, uint32_t hGroup, uint32_t hObj, int dx, int dy)
{
	DispObj_t* grp = Gfx_FindGroup(g, hGroup);
	DispObj_t* obj;

	if(!grp)
		return NO_OBJECT;
	obj = Gfx_FindObject(g, hObj);
	if(!obj)
		return 1;
	if(obj == grp)
		return 3;
	return DispObj_AttachChild(o, grp, obj, dx, dy) ? 0 : 4;
}

/* "91 3E" of 1.492 on: any object as the owner of another; the slave
 * follows the master's position at the offset (dx, dy).  0 ok, 0xff master
 * unknown, 6 slave unknown, 7 the slave is the master, 8 the slave already
 * has an owner. */
int Gfx_ObjAttach(OpenBGI_t* o, Gfx_t* g, uint32_t hMaster, uint32_t hSlave, int dx, int dy)
{
	DispObj_t* m = Gfx_FindObject(g, hMaster);
	DispObj_t* obj;
	if(!m)
		return 0xff;
	obj = Gfx_FindObject(g, hSlave);
	if(!obj)
		return 6;
	if(obj == m)
		return 7;
	return DispObj_AttachChild(o, m, obj, dx, dy) ? 0 : 8;
}

// "91 3F" of 1.492 on: undo Gfx_ObjAttach; 0 ok, 0xff master unknown, 6 slave unknown, 9 not attached to that master
int Gfx_ObjDetach(Gfx_t* g, uint32_t hMaster, uint32_t hSlave)
{
	DispObj_t* m = Gfx_FindObject(g, hMaster);
	DispObj_t* obj;
	if(!m)
		return 0xff;
	obj = Gfx_FindObject(g, hSlave);
	if(!obj)
		return 6;
	return DispObj_DetachChild(m, obj) ? 0 : 9;
}

// "90 E9": take hObj out of the group; 0 ok, 1 the object does not resolve, 2 it was not a child, 0xFF unknown group
int Gfx_GroupRemove(Gfx_t* g, uint32_t hGroup, uint32_t hObj)
{
	DispObj_t* grp = Gfx_FindGroup(g, hGroup);
	DispObj_t* obj;

	if(!grp)
		return NO_OBJECT;
	obj = Gfx_FindObject(g, hObj);
	if(!obj)
		return 1;
	return DispObj_DetachChild(grp, obj) ? 0 : 2;
}
