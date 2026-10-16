/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * locks.h - named counting locks for the script threads
 *
 * A lock admits up to `max` holders at once.  The scripts create ("80 B0")
 * and delete ("80 B1") locks by name.  A thread waits for one with
 * "80 B4": it is queued by priority (SemMgr_Enqueue) and its wait object
 * (WaitLock, waitobj.h) polls SemMgr_TryAcquire until it heads the queue and
 * a place is free.  "80 B5" releases a place, "80 B6" asks whether a
 * request of a given priority would be admitted now.  The engine keeps one
 * manager, gSemMgr, cleared with every fresh state.
 */
#ifndef BGI_SYSOBJ_LOCKS_H_
#define BGI_SYSOBJ_LOCKS_H_

#include "bgi/common.h"

typedef struct SemMgr SemMgr_t;
extern SemMgr_t* gSemMgr;

SemMgr_t* SemMgr_New(void);
// free the manager with its locks
void SemMgr_Destroy(SemMgr_t* m);
// drop every lock (the manager stays usable)
void SemMgr_Clear(SemMgr_t* m);
// a lock admitting `max` holders ("80 B0"); 1 when created, 0 when the name exists
int SemMgr_Create(SemMgr_t* m, const char* name, uint32_t max);
// delete a lock ("80 B1"): 0 ok, 0x80000001 unknown lock, 0x80000002 the lock is held or awaited
uint32_t SemMgr_Delete(SemMgr_t* m, const char* name);
// queue thread `threadId` for the lock behind every waiter of equal or higher priority; 1 ok, 0 unknown lock
uint32_t SemMgr_Enqueue(SemMgr_t* m, const char* name, uint32_t threadId, uint32_t prio);
/* let the thread in when it heads the queue and a place is free; 0 ok,
 * 0x80000001 unknown lock, 0x80000004 not now */
uint32_t SemMgr_TryAcquire(SemMgr_t* m, const char* name, uint32_t threadId);
// give the thread's place back ("80 B5"); 0 ok, 0x80000001 unknown lock, 0x80000003 the thread holds no place
uint32_t SemMgr_Release(SemMgr_t* m, const char* name, uint32_t threadId);
// would a request of priority `prio` be admitted now ("80 B6"): 0 yes, 0x80000001 unknown lock, 0x80000004 no
uint32_t SemMgr_Query(SemMgr_t* m, const char* name, uint32_t prio);

#endif // BGI_SYSOBJ_LOCKS_H_
