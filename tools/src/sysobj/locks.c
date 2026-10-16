/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * locks.c - named counting locks (inc/bgi/sysobj/locks.h)
 *
 * The manager is a singly linked list of locks behind a nameless sentinel
 * (so that creation appends and deletion unlinks without a special case
 * for the head).  Each lock keeps the threads holding it and the threads
 * waiting for it, the latter ordered by descending priority with equal
 * priorities first come first served.
 */
#include "bgi/sysobj/locks.h"

typedef struct SemHolder // a thread holding a place in the lock
{
	uint32_t threadId;
	struct SemHolder* next;
} SemHolder_t;

typedef struct SemWaiter // a thread queued for the lock
{
	uint32_t threadId;
	uint32_t prio; // the priority it queued with (higher goes first)
	struct SemWaiter* next;
} SemWaiter_t;

typedef struct Sem // one lock
{
	char name[0x100];
	uint32_t max;         // holders allowed at once
	uint32_t holderCount; // entries in `holders`
	uint32_t waiterCount; // entries in `waiters`
	SemHolder_t* holders;
	SemWaiter_t* waiters; // by descending priority
	struct Sem* next;
} Sem_t;

struct SemMgr
{
	Sem_t* head; // the nameless sentinel; the locks follow it
};

SemMgr_t* gSemMgr;

// a lock with no holders or waiters; NULL names the sentinel
static Sem_t* Sem_New(const char* name, uint32_t max)
{
	Sem_t* s = (Sem_t*)BGI_Calloc(sizeof(Sem_t));
	if(name)
		strcpy(s->name, name);
	s->max = max;
	return s;
}

// free a lock with its holder and waiter lists (the caller has unlinked it)
static void Sem_Delete(Sem_t* s)
{
	while(s->holders)
	{
		SemHolder_t* n = s->holders->next;
		BGI_Free(s->holders);
		s->holders = n;
	}
	while(s->waiters)
	{
		SemWaiter_t* n = s->waiters->next;
		BGI_Free(s->waiters);
		s->waiters = n;
	}
	BGI_Free(s);
}

// queue a thread behind every waiter of equal or higher priority
static void Sem_Enqueue(Sem_t* s, uint32_t threadId, uint32_t prio)
{
	SemWaiter_t **link = &s->waiters, *w;
	while(*link && prio <= (*link)->prio)
		link = &(*link)->next;
	w = (SemWaiter_t*)BGI_Alloc(sizeof(SemWaiter_t));
	w->threadId = threadId;
	w->prio = prio;
	w->next = *link;
	*link = w;
	s->waiterCount++;
}

/* only the head waiter may acquire, and only while a place is free; 1 when
 * the thread was moved from the waiters to the holders (the counts are
 * compared as signed, so a `max` of 0x80000000 or above admits nobody) */
static int Sem_TryAcquire(Sem_t* s, uint32_t threadId)
{
	SemWaiter_t* w = s->waiters;
	SemHolder_t* h;
	if(!w || w->threadId != threadId || (int32_t)s->holderCount >= (int32_t)s->max)
		return 0;
	s->waiters = w->next;
	BGI_Free(w);
	s->waiterCount--;
	h = (SemHolder_t*)BGI_Alloc(sizeof(SemHolder_t));
	h->threadId = threadId;
	h->next = s->holders;
	s->holders = h;
	s->holderCount++;
	return 1;
}

// 1 when the thread held a place and gave it back
static int Sem_RemoveHolder(Sem_t* s, uint32_t threadId)
{
	SemHolder_t** link = &s->holders;
	while(*link)
	{
		if((*link)->threadId == threadId)
		{
			SemHolder_t* h = *link;
			*link = h->next;
			BGI_Free(h);
			s->holderCount--;
			return 1;
		}
		link = &(*link)->next;
	}
	return 0;
}

// would a waiter of this priority be admitted now: the holders plus the waiters ahead of it must fit
static int Sem_CanAcquire(Sem_t* s, uint32_t prio)
{
	SemWaiter_t* w;
	uint32_t ahead = 0;
	for(w = s->waiters; w && prio <= w->prio; w = w->next)
		ahead++;
	return (int32_t)s->max >= (int32_t)(s->holderCount + ahead);
}

// An empty manager: the sentinel alone.
SemMgr_t* SemMgr_New(void)
{
	SemMgr_t* m = (SemMgr_t*)BGI_Alloc(sizeof(SemMgr_t));
	m->head = Sem_New(NULL, 0);
	return m;
}

// Drop every lock; the sentinel stays.
void SemMgr_Clear(SemMgr_t* m)
{
	Sem_t* s = m->head->next;
	while(s)
	{
		Sem_t* next = s->next;
		Sem_Delete(s);
		s = next;
	}
	m->head->next = NULL;
}

void SemMgr_Destroy(SemMgr_t* m)
{
	SemMgr_Clear(m);
	Sem_Delete(m->head);
	BGI_Free(m);
}

// the lock called `name`, NULL when there is none
static Sem_t* SemMgr_Find(SemMgr_t* m, const char* name)
{
	Sem_t* s;
	for(s = m->head->next; s; s = s->next)
		if(strcmp(s->name, name) == 0)
			return s;
	return NULL;
}

// Append a new lock admitting `max` holders: 1 done, 0 when the name is taken.
int SemMgr_Create(SemMgr_t* m, const char* name, uint32_t max)
{
	Sem_t *last = m->head, *s;
	for(s = last->next; s; last = s, s = s->next)
		if(strcmp(s->name, name) == 0)
			return 0;
	last->next = Sem_New(name, max);
	return 1;
}

// Delete a lock nobody holds or waits for: 0 ok, 0x80000001 unknown lock, 0x80000002 it is held or awaited.
uint32_t SemMgr_Delete(SemMgr_t* m, const char* name)
{
	Sem_t *prev = m->head, *s;
	for(s = prev->next; s; prev = s, s = s->next)
	{
		if(strcmp(s->name, name) != 0)
			continue;
		if(s->holderCount != 0 || s->waiterCount != 0)
			return 0x80000002u;
		prev->next = s->next;
		Sem_Delete(s);
		return 0;
	}
	return 0x80000001u;
}

// Queue a thread for the lock (the first half of "80 B4"): 1 ok, 0 unknown lock.
uint32_t SemMgr_Enqueue(SemMgr_t* m, const char* name, uint32_t threadId, uint32_t prio)
{
	Sem_t* s = SemMgr_Find(m, name);
	if(s)
		Sem_Enqueue(s, threadId, prio);
	return s != NULL;
}

// Let a queued thread in when it can be (polled by the "80 B4" wait): 0 ok, 0x80000001 unknown lock, 0x80000004 not now.
uint32_t SemMgr_TryAcquire(SemMgr_t* m, const char* name, uint32_t threadId)
{
	Sem_t* s = SemMgr_Find(m, name);
	if(!s)
		return 0x80000001u;
	return Sem_TryAcquire(s, threadId) ? 0 : 0x80000004u;
}

// Give the thread's place back: 0 ok, 0x80000001 unknown lock, 0x80000003 it holds no place.
uint32_t SemMgr_Release(SemMgr_t* m, const char* name, uint32_t threadId)
{
	Sem_t* s = SemMgr_Find(m, name);
	if(!s)
		return 0x80000001u;
	return Sem_RemoveHolder(s, threadId) ? 0 : 0x80000003u;
}

// Would a request of priority `prio` be admitted now: 0 yes, 0x80000001 unknown lock, 0x80000004 no.
uint32_t SemMgr_Query(SemMgr_t* m, const char* name, uint32_t prio)
{
	Sem_t* s = SemMgr_Find(m, name);
	if(!s)
		return 0x80000001u;
	return Sem_CanAcquire(s, prio) ? 0 : 0x80000004u;
}
