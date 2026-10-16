/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * userop.c - user-defined instructions "FF xx" (UserOp_Init, UserOp_Clear
 *            and the handler Opcode_User are declared in inc/bgi/vm.h)
 *
 * A script registers a program file under a slot ("FF F0"); afterwards
 * the byte pair FF <slot> calls that program like a subroutine.  The call
 * is made through a 16-byte "Mediation program" appended to the code area
 * together with a copy of the registered program:
 *
 *      06 10 00      push_code_off  +0x10   (= start of the user program)
 *      16            call
 *      FF F8         user-return: unload both modules, pop the frame
 *
 * so that the user program's own `ret` lands on FF F8, which cleans up and
 * continues after the FF xx instruction (opIP + 2).  The registrations are
 * global, not per thread; the copies live in the calling thread's code
 * area only for the duration of the call.
 */
#include "bgi/vm.h"
#include "bgi/file.h"
#include "bgi/sys.h"
#include "bgi/error.h"
#include "bgi/msg.h"

typedef struct UserOp // 8 bytes: one registered slot
{
	char* name;     // heap copy of the file name
	uint8_t* image; // copy of the loaded program
} UserOp_t;

#define USEROP_SLOTS 0xf0                // slots 00 .. EF; F0 .. FF are the management sub-opcodes
static UserOp_t* gUserOps[USEROP_SLOTS]; // the registrations, NULL for a free slot

// clear the slot table, once at start-up
void UserOp_Init(void)
{
	memset(gUserOps, 0, sizeof gUserOps);
}

// free the registration of `slot`; 1 when there was one, 0 for a free slot
static int UserOp_Unregister(uint32_t slot)
{
	UserOp_t* u = gUserOps[slot];
	if(!u)
		return 0;
	BGI_Free(u->name);
	BGI_Free(u->image);
	BGI_Free(u);
	gUserOps[slot] = NULL;
	return 1;
}

// free every registration (part of the machine's reset, Vm_ResetSubsystems)
void UserOp_Clear(void)
{
	uint32_t i;
	for(i = 0; i < USEROP_SLOTS; i++)
		UserOp_Unregister(i);
}

/* load program `name` of archive `arc` (at most 0x20000 bytes) and keep a
 * copy of it under `slot`, replacing an earlier registration; 1 on
 * success, 0 when the file loads as 0 bytes (the slot is then free) */
static int UserOp_Register(uint32_t slot, const char* arc, const char* name)
{
	uint8_t* buf = (uint8_t*)BGI_Alloc(0x20000);
	uint32_t n;
	int ok;
	UserOp_Unregister(slot);
	n = LoadFile(buf, arc, name);
	ok = n != 0;
	if(ok)
	{
		UserOp_t* u = (UserOp_t*)BGI_Alloc(sizeof(UserOp_t));
		gUserOps[slot] = u;
		u->name = BGI_Strdup(name);
		u->image = (uint8_t*)BGI_Alloc(n);
		memcpy(u->image, buf, n);
	}
	BGI_Free(buf);
	return ok;
}

// register program `file` of archive `arc` under `slot`; a slot of F0 or above, or a program that cannot be loaded, is a script error
static void OpFFF0Register(Thread_t* t) // file, arc, slot →
{
	char* file = (char*)PopPtr(t);
	char* arc = (char*)PopPtr(t);
	uint32_t slot = Thread_Pop(t);
	char msg[0x100];
	if(slot >= USEROP_SLOTS)
	{
		sprintf(msg, MSG_BAD_USEROP_SLOT, (int)slot);
		ScriptError(msg, t);
	}
	if(!UserOp_Register(slot, arc, file))
	{
		sprintf(msg, MSG_PROGRAM_NOT_FOUND, arc, file);
		ScriptError(msg, t);
	}
}

// free `slot` (a free slot is no error); a slot of F0 or above is a script error
static void OpFFF1Unregister(Thread_t* t) // slot →
{
	uint32_t slot = Thread_Pop(t);
	if(slot >= USEROP_SLOTS)
	{
		char msg[0x100];
		sprintf(msg, MSG_BAD_USEROP_SLOT, (int)slot);
		ScriptError(msg, t);
	}
	UserOp_Unregister(slot);
}

/* the return from a user-defined instruction: drop the two modules the
 * call appended (the program copy and the mediation program) and continue
 * at the address Opcode_User pushed on the frame stack, the one after the
 * FF xx instruction */
static void OpFFF8Return(Thread_t* t)
{
	Thread_UnloadLastModule(t);
	Thread_UnloadLastModule(t);
	Thread_SetIP(t, FramePop(t));
}

/*
 * "FF xx".  The second byte selects the management operations F0
 * (register), F1 (unregister) and F8 (return); any other value from F0 on
 * is an "undefined management instruction" script error.  A value below F0
 * calls the program registered under that slot (an empty slot is a script
 * error): the mediation program and a copy of the registered one are
 * appended to the code area (a code area without room for them is a
 * script error), the address after this instruction is pushed on the frame
 * stack (a full data area is a script error) and execution jumps to the
 * mediation program.  Always returns scheduler code 0.
 */
int Opcode_User(Thread_t* t)
{
	static const uint8_t mediation[0x28] = {
		0x10, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,             // ProgramHeader {0x10, 0x10} + pad
		0x06, 0x10, 0x00, 0x16, 0xff, 0xf8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // the code: push_code_off +0x10, call, FF F8
		0, 0, 0, 0, 0, 0, 0, 0};
	uint8_t sub = RdU8(t);
	char msg[0x100];
	uint32_t base;

	switch(sub)
	{
		case 0xf0: OpFFF0Register(t); return 0;
		case 0xf1: OpFFF1Unregister(t); return 0;
		case 0xf8: OpFFF8Return(t); return 0;
		default: break;
	}
	if(sub >= USEROP_SLOTS)
	{
		sprintf(msg, MSG_UNDEF_USEROP_MGMT, sub);
		ScriptError(msg, t);
	}
	if(!gUserOps[sub])
	{
		sprintf(msg, MSG_UNDEF_USEROP, sub);
		ScriptError(msg, t);
	}
	base = Thread_LoadModule(t, mediation, "Mediation program");
	if(base == 0x80000000u)
	{
		sprintf(msg, MSG_USEROP_CODE_FULL, sub);
		ScriptError(msg, t);
	}
	if(Thread_LoadModule(t, gUserOps[sub]->image, gUserOps[sub]->name) == 0x80000000u)
	{
		sprintf(msg, MSG_USEROP_CODE_FULL, sub);
		ScriptError(msg, t);
	}
	if(Thread_GetFP(t) + 4 >= t->dataSize)
		ScriptError(MSG_STACK_FULL, t);
	FramePush(t, Thread_CurOpIP(t) + 2);
	Thread_SetIP(t, base);
	return 0;
}
