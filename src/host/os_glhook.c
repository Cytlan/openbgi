/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_glhook.c - shader files in the format of mpv's user shaders and the
 *               plan of their passes (os_glhook.h)
 */
#include "host/os_glhook.h"

#include <stdlib.h>
#include <string.h>

// ---- the file ---------------------------------------------------------------------------------

// a directive's value into dst[n], without the blanks around it; 0 when it does not fit
static int Value(char* dst, size_t n, const char* v, const char* end)
{
	while(v < end && (*v == ' ' || *v == '\t'))
		v++;
	while(end > v && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
		end--;
	if((size_t)(end - v) >= n)
		return 0;
	memcpy(dst, v, (size_t)(end - v));
	dst[end - v] = 0;
	return 1;
}

int OsGlHook_Parse(const char* text, size_t n, OsGlPass_t* out, int max)
{
	const char *p = text, *end = text + n;
	OsGlPass_t* cur = NULL;
	int count = 0;
	while(p < end)
	{
		const char* eol = (const char*)memchr(p, '\n', (size_t)(end - p));
		const char* next = eol ? eol + 1 : end;
		if(!eol)
			eol = end;
		if(eol - p >= 3 && memcmp(p, "//!", 3) == 0)
		{
			const char* key = p + 3;
			const char* v = key;
			char word[32];
			size_t len;
			while(v < eol && *v != ' ' && *v != '\t' && *v != '\r')
				v++;
			len = (size_t)(v - key);
			if(len >= sizeof word)
				return -1;
			memcpy(word, key, len);
			word[len] = 0;
			if(!cur || cur->body)
			{ // the first directive after a pass's code: the next pass
				if(cur)
					cur->bodyLen = (size_t)(p - cur->body);
				if(count >= max)
					return -1;
				cur = &out[count++];
				memset(cur, 0, sizeof *cur);
			}
			if(strcmp(word, "DESC") == 0)
				Value(cur->desc, sizeof cur->desc, v, eol); // (a long one is left out: it is only a name)
			else if(strcmp(word, "HOOK") == 0)
			{
				char name[OS_GLHOOK_NAME];
				if(!Value(name, sizeof name, v, eol))
					return -1;
				if(!cur->hook)
					cur->hook = strcmp(name, "MAIN") == 0 ? OS_GLHOOK_MAIN : strcmp(name, "PREKERNEL") == 0 ? OS_GLHOOK_PREKERNEL
																											: -1;
			}
			else if(strcmp(word, "BIND") == 0)
			{
				if(cur->binds >= OS_GLHOOK_BINDS || !Value(cur->bind[cur->binds], OS_GLHOOK_NAME, v, eol))
					return -1;
				cur->binds++;
			}
			else if(strcmp(word, "SAVE") == 0)
			{
				if(!Value(cur->save, sizeof cur->save, v, eol))
					return -1;
			}
			else if(strcmp(word, "WIDTH") == 0)
			{
				if(!Value(cur->width, sizeof cur->width, v, eol))
					return -1;
			}
			else if(strcmp(word, "HEIGHT") == 0)
			{
				if(!Value(cur->height, sizeof cur->height, v, eol))
					return -1;
			}
			else if(strcmp(word, "WHEN") == 0)
			{
				if(!Value(cur->when, sizeof cur->when, v, eol))
					return -1;
			}
			else if(strcmp(word, "COMPONENTS") == 0)
			{
				char num[8];
				if(!Value(num, sizeof num, v, eol))
					return -1;
				cur->components = atoi(num);
			}
			else
				cur->hook = -1; // (a directive this reader does not know: the pass is not run rather than run wrongly)
		}
		else if(cur && !cur->body)
			cur->body = p;
		p = next;
	}
	if(cur)
	{
		if(!cur->body)
			cur->body = end;
		cur->bodyLen = (size_t)(end - cur->body);
	}
	for(n = 0; n < (size_t)count; n++)
		if(out[n].hook < 0)
			out[n].hook = 0;
	return count;
}

// ---- the expressions ----------------------------------------------------------------------------

int OsGlHook_Eval(const char* expr, OsGlSizeFn_t size, void* ctx, double* out)
{
	double st[16];
	int sp = 0;
	const char* p = expr;
	for(;;)
	{
		char tok[OS_GLHOOK_NAME + 8];
		size_t len = 0;
		while(*p == ' ' || *p == '\t')
			p++;
		if(!*p)
			break;
		while(p[len] && p[len] != ' ' && p[len] != '\t')
			len++;
		if(len >= sizeof tok)
			return 0;
		memcpy(tok, p, len);
		tok[len] = 0;
		p += len;
		if(len == 1 && strchr("+-*/%><=", tok[0]))
		{
			double a, b;
			if(sp < 2)
				return 0;
			b = st[--sp];
			a = st[--sp];
			switch(tok[0])
			{
				case '+': a += b; break;
				case '-': a -= b; break;
				case '*': a *= b; break;
				case '/': a = b != 0 ? a / b : 0; break;
				case '%': a = b != 0 ? a - b * (double)(long)(a / b) : 0; break;
				case '>': a = a > b; break;
				case '<': a = a < b; break;
				default: a = a == b; break;
			}
			st[sp++] = a;
		}
		else if(len == 1 && tok[0] == '!')
		{
			if(sp < 1)
				return 0;
			st[sp - 1] = st[sp - 1] == 0;
		}
		else if(len > 2 && tok[len - 2] == '.' && (tok[len - 1] == 'w' || tok[len - 1] == 'h') && !(tok[0] >= '0' && tok[0] <= '9'))
		{
			int w = 0, h = 0, wide = tok[len - 1] == 'w';
			tok[len - 2] = 0;
			if(sp >= 16 || !size || !size(ctx, tok, &w, &h))
				return 0;
			st[sp++] = wide ? w : h;
		}
		else
		{
			char* stop;
			double v = strtod(tok, &stop);
			if(*stop || sp >= 16)
				return 0;
			st[sp++] = v;
		}
	}
	if(sp != 1)
		return 0;
	*out = st[0];
	return 1;
}

// ---- the plan -----------------------------------------------------------------------------------

#define TEXTURES (OS_GLPLAN_STEPS + 1) // what the steps make, and the picture
#define NAMES    48

typedef struct Plan
{
	int w[TEXTURES], h[TEXTURES]; // every texture a step makes, by its number; 0 is the picture as it came
	int count;
	struct
	{
		char name[OS_GLHOOK_NAME];
		int tex;
	} named[NAMES]; // what the passes saved under a name
	int names;
	int main;       // the picture as it is now
	int outW, outH; // where it is shown
} Plan_t;

// the texture a name stands for; -1 for none
static int Lookup(const Plan_t* s, const char* name)
{
	int i;
	if(strcmp(name, "MAIN") == 0 || strcmp(name, "HOOKED") == 0 || strcmp(name, "PREKERNEL") == 0)
		return s->main;
	if(strcmp(name, "NATIVE") == 0)
		return 0;
	for(i = 0; i < s->names; i++)
		if(strcmp(s->named[i].name, name) == 0)
			return s->named[i].tex;
	return -1;
}

static int SizeOf(void* ctx, const char* name, int* w, int* h)
{
	const Plan_t* s = (const Plan_t*)ctx;
	int t;
	if(strcmp(name, "OUTPUT") == 0)
	{
		*w = s->outW;
		*h = s->outH;
		return 1;
	}
	t = Lookup(s, name);
	if(t < 0)
		return 0;
	*w = s->w[t];
	*h = s->h[t];
	return 1;
}

// one pass into the plan when it runs; 0 when the plan is full
static int Step(Plan_t* s, OsGlPlan_t* plan, int file, int index, const OsGlPass_t* p)
{
	OsGlStep_t* st;
	double v = 0;
	int w = s->w[s->main], h = s->h[s->main], i, in[OS_GLHOOK_BINDS], tex;
	if(p->when[0] && (!OsGlHook_Eval(p->when, SizeOf, s, &v) || v == 0))
		return 1;
	for(i = 0; i < p->binds; i++)
		if((in[i] = Lookup(s, p->bind[i])) < 0)
			return 1; // (a texture that was never made: the pass is left out)
	if(p->width[0])
	{
		if(!OsGlHook_Eval(p->width, SizeOf, s, &v))
			return 1;
		w = (int)(v + 0.5);
	}
	if(p->height[0])
	{
		if(!OsGlHook_Eval(p->height, SizeOf, s, &v))
			return 1;
		h = (int)(v + 0.5);
	}
	if(w < 1 || h < 1 || w > 16384 || h > 16384)
		return 1;
	if(plan->steps >= OS_GLPLAN_STEPS || s->count >= TEXTURES)
		return 0;
	tex = s->count++;
	s->w[tex] = w;
	s->h[tex] = h;
	st = &plan->step[plan->steps++];
	st->file = file;
	st->pass = index;
	st->w = w;
	st->h = h;
	st->out = tex;
	for(i = 0; i < OS_GLHOOK_BINDS; i++)
		st->in[i] = i < p->binds ? in[i] : -1;
	if(!p->save[0] || strcmp(p->save, "MAIN") == 0 || strcmp(p->save, "HOOKED") == 0 || strcmp(p->save, "PREKERNEL") == 0)
		s->main = tex;
	else
	{
		for(i = 0; i < s->names && strcmp(s->named[i].name, p->save) != 0; i++)
		{
		}
		if(i == s->names)
		{
			if(s->names >= NAMES)
				return 0;
			memcpy(s->named[i].name, p->save, OS_GLHOOK_NAME);
			s->names++;
		}
		s->named[i].tex = tex;
	}
	return 1;
}

int OsGlHook_Plan(const OsGlPass_t* const* files, const int* counts, int nFiles, int picW, int picH, int outW, int outH,
	OsGlPlan_t* plan)
{
	static const int kPhases[2] = {OS_GLHOOK_MAIN, OS_GLHOOK_PREKERNEL};
	Plan_t s;
	int last[TEXTURES], slotOf[TEXTURES], until[OS_GLPLAN_SLOTS], phase, f, i, k;
	memset(plan, 0, sizeof *plan);
	memset(&s, 0, sizeof s);
	if(picW < 1 || picH < 1 || outW < 1 || outH < 1)
		return 0;
	s.w[0] = picW;
	s.h[0] = picH;
	s.count = 1;
	s.outW = outW;
	s.outH = outH;
	for(phase = 0; phase < 2; phase++)
		for(f = 0; f < nFiles; f++)
			for(i = 0; i < counts[f]; i++)
				if(files[f][i].hook == kPhases[phase] && !Step(&s, plan, f, i, &files[f][i]))
					return 0;
	// how long each texture is read: the picture after the last step for good
	for(i = 0; i < s.count; i++)
		last[i] = -1;
	for(i = 0; i < plan->steps; i++)
		for(k = 0; k < OS_GLHOOK_BINDS; k++)
			if(plan->step[i].in[k] >= 0)
				last[plan->step[i].in[k]] = i;
	last[s.main] = OS_GLPLAN_STEPS;
	// a slot for each: one of its size that nothing reads any more, else a new one
	plan->slot[0].w = picW;
	plan->slot[0].h = picH;
	plan->slots = 1;
	slotOf[0] = 0;
	until[0] = OS_GLPLAN_STEPS;
	for(i = 0; i < plan->steps; i++)
	{
		OsGlStep_t* st = &plan->step[i];
		int tex = st->out, slot = -1;
		for(k = 1; k < plan->slots && slot < 0; k++)
			if(until[k] < i && plan->slot[k].w == st->w && plan->slot[k].h == st->h)
				slot = k;
		if(slot < 0)
		{
			if(plan->slots >= OS_GLPLAN_SLOTS)
				return 0;
			slot = plan->slots++;
			plan->slot[slot].w = st->w;
			plan->slot[slot].h = st->h;
		}
		slotOf[tex] = slot;
		until[slot] = last[tex] > i ? last[tex] : i;
		for(k = 0; k < OS_GLHOOK_BINDS; k++)
			if(st->in[k] >= 0)
				st->in[k] = slotOf[st->in[k]];
		st->out = slot;
	}
	plan->result = slotOf[s.main];
	return 1;
}
