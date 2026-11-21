/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * inputcfg.c - the input settings of openbgi.json (bgi/inputcfg.h): the
 *              "input" objects read into InputSettings_t and written
 *              back; not part of the original engine
 *
 * The games table (version.c) reads the top-level object and each
 * entry's with InputSettings_Parse and writes them with
 * InputSettings_Format when the file is rewritten; the input mapper
 * (src/sys/inputmap.c, InputMap_Configure) applies them when a game
 * starts.
 */
#include "bgi/inputcfg.h"
#include "bgi/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the names --------------------------------------------------------------------------------

static const char* const kControls[13] = {"south", "east", "west", "north", "l1", "r1", "select", "start", "l3", "r3", "l2", "r2", "guide"};
static const char* const kDirs[4] = {"up", "down", "left", "right"};
// the actions, OPENBGI_ACTION_CONFIRM (1) first
static const char* const kActions[10] = {"confirm", "cancel", "skip", "backlog", "forward", "up", "down", "left", "right", "keyboard"};
static const char* const kMouse[3] = {"left", "right", "middle"}; // OPENBGI_BUTTON_LEFT ..

static const struct
{
	const char* name;
	int vk;
} kKeys[] = {
	{"RETURN", 0x0d},
	{"ENTER", 0x0d},
	{"SPACE", 0x20},
	{"ESCAPE", 0x1b},
	{"BACK", 0x08},
	{"TAB", 0x09},
	{"SHIFT", 0x10},
	{"CONTROL", 0x11},
	{"CTRL", 0x11},
	{"ALT", 0x12},
	{"UP", 0x26},
	{"DOWN", 0x28},
	{"LEFT", 0x25},
	{"RIGHT", 0x27},
	{"PRIOR", 0x21},
	{"PAGEUP", 0x21},
	{"NEXT", 0x22},
	{"PAGEDOWN", 0x22},
	{"HOME", 0x24},
	{"END", 0x23},
	{"INSERT", 0x2d},
	{"DELETE", 0x2e},
};

// a and b are the same but for the case of ASCII letters
static int SameWord(const char* a, const char* b)
{
	for(; *a && *b; a++, b++)
	{
		int x = *a >= 'a' && *a <= 'z' ? *a - 'a' + 'A' : *a, y = *b >= 'a' && *b <= 'z' ? *b - 'a' + 'A' : *b;
		if(x != y)
			return 0;
	}
	return !*a && !*b;
}

// the number after `prefix` in `s`, 0 .. max; -1 when s is not prefix and a number
static int Numbered(const char* s, const char* prefix, int max)
{
	size_t n = strlen(prefix);
	char* end;
	long v;
	if(strncmp(s, prefix, n) != 0 || s[n] < '0' || s[n] > '9')
		return -1;
	v = strtol(s + n, &end, 10);
	return !*end && v >= 0 && v <= max ? (int)v : -1;
}

int InputControl_ByName(const char* name)
{
	int i;
	for(i = 0; i < 13; i++)
		if(strcmp(name, kControls[i]) == 0)
			return i;
	for(i = 0; i < 4; i++)
		if(strcmp(name, kDirs[i]) == 0)
			return 32 + i;
	return Numbered(name, "button", 31);
}

const char* InputControl_Name(int control, char buf[16])
{
	if(control >= 0 && control < 13)
		return kControls[control];
	if(control >= 32 && control < 36)
		return kDirs[control - 32];
	snprintf(buf, 16, "button%d", control);
	return buf;
}

int InputKey_ByName(const char* name)
{
	size_t i;
	char* end;
	long v;
	if(name[0] && !name[1])
	{
		if((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9'))
			return name[0];
		if(name[0] >= 'a' && name[0] <= 'z')
			return name[0] - 'a' + 'A';
	}
	if((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9')
	{
		v = strtol(name + 1, &end, 10);
		if(!*end && v >= 1 && v <= 12)
			return 0x70 + (int)v - 1;
	}
	for(i = 0; i < BGI_COUNTOF(kKeys); i++)
		if(SameWord(name, kKeys[i].name))
			return kKeys[i].vk;
	if(name[0] == '0' && (name[1] == 'x' || name[1] == 'X') && name[2])
	{
		v = strtol(name + 2, &end, 16);
		if(!*end && v > 0 && v < 256)
			return (int)v;
	}
	return 0;
}

int InputBind_Parse(const char* text, uint16_t* bind)
{
	int i, vk;
	if(strcmp(text, "none") == 0)
	{
		*bind = 0;
		return 1;
	}
	for(i = 0; i < 10; i++)
		if(strcmp(text, kActions[i]) == 0)
		{
			*bind = (uint16_t)(1 + i);
			return 1;
		}
	if(strncmp(text, "key:", 4) == 0 && (vk = InputKey_ByName(text + 4)) != 0)
	{
		*bind = (uint16_t)(INPUT_BIND_KEY | vk);
		return 1;
	}
	if(strncmp(text, "mouse:", 6) == 0)
		for(i = 0; i < 3; i++)
			if(strcmp(text + 6, kMouse[i]) == 0)
			{
				*bind = (uint16_t)(INPUT_BIND_MOUSE | i);
				return 1;
			}
	if(strcmp(text, "wheel:up") == 0 || strcmp(text, "wheel:down") == 0)
	{
		*bind = (uint16_t)(INPUT_BIND_WHEEL | (text[6] == 'd'));
		return 1;
	}
	return 0;
}

void InputBind_Format(uint16_t bind, char* out, size_t n)
{
	int arg = bind & 0xff;
	size_t i;
	switch(bind & INPUT_BIND_KIND)
	{
		case 0:
			snprintf(out, n, "%s", bind >= 1 && bind <= 10 ? kActions[bind - 1] : "none");
			break;
		case INPUT_BIND_KEY:
			if((arg >= 'A' && arg <= 'Z') || (arg >= '0' && arg <= '9'))
				snprintf(out, n, "key:%c", arg);
			else if(arg >= 0x70 && arg <= 0x7b)
				snprintf(out, n, "key:F%d", arg - 0x70 + 1);
			else
			{
				for(i = 0; i < BGI_COUNTOF(kKeys) && kKeys[i].vk != arg; i++)
				{
				}
				if(i < BGI_COUNTOF(kKeys))
					snprintf(out, n, "key:%s", kKeys[i].name);
				else
					snprintf(out, n, "key:0x%02X", arg);
			}
			break;
		case INPUT_BIND_MOUSE: snprintf(out, n, "mouse:%s", kMouse[arg < 3 ? arg : 0]); break;
		case INPUT_BIND_WHEEL: snprintf(out, n, "wheel:%s", arg ? "down" : "up"); break;
		default: snprintf(out, n, "none"); break;
	}
}

// ---- reading ----------------------------------------------------------------------------------

// a number member of an array: element `i` as an int, `def` when there is none
static int Element(const JsonNode_t* arr, int i, int def)
{
	const JsonNode_t* e = arr ? arr->child : NULL;
	for(; e && i > 0; i--)
		e = e->next;
	return e && e->type == JSON_NUMBER ? (int)e->num : def;
}

static const char* const kPointer[3] = {"pad", "always", "never"};
static const char* const kPointerSize[3] = {"small", "normal", "large"};

// which of `count` words a string node is; -1 for none of them
static int Word(const JsonNode_t* n, const char* const* words, int count)
{
	int i;
	for(i = 0; n->type == JSON_STRING && i < count; i++)
		if(strcmp(n->str, words[i]) == 0)
			return i;
	return -1;
}

InputSettings_t* InputSettings_Parse(const JsonNode_t* obj, const char* path)
{
	InputSettings_t* s;
	const JsonNode_t *pad, *e, *spots, *n;
	int said = 0;
	if(!obj || obj->type != JSON_OBJECT)
		return NULL;
	s = (InputSettings_t*)BGI_Calloc(sizeof *s);
	if(!s)
		return NULL;
	s->ownPadKeys = s->tapDelay = s->pointer = s->pointerSize = s->stickSpeed = s->onScreenKeyboard = s->settingsKey = -1;
	pad = Json_Get(obj, "pad");
	for(e = pad && pad->type == JSON_OBJECT ? pad->child : NULL; e; e = e->next)
	{
		int ctl = e->key ? InputControl_ByName(e->key) : -1;
		uint16_t bind;
		if(ctl < 0)
			fprintf(stderr, "openbgi: %s: input: no control \"%s\"\n", path, e->key ? e->key : "");
		else if(e->type != JSON_STRING || !InputBind_Parse(e->str, &bind))
			fprintf(stderr, "openbgi: %s: input: \"%s\" cannot do \"%s\"\n", path, e->key, e->type == JSON_STRING ? e->str : "?");
		else
		{
			s->bound[ctl] = 1;
			s->bind[ctl] = bind;
			said = 1;
		}
	}
	n = Json_Get(obj, "ownPadKeys");
	if(n && n->type == JSON_BOOL)
	{
		s->ownPadKeys = n->num != 0;
		said = 1;
	}
	n = Json_Get(obj, "tapDelay");
	if(n && n->type == JSON_NUMBER && n->num >= 0 && n->num <= 2000)
	{
		s->tapDelay = (int)n->num;
		said = 1;
	}
	else if(n)
		fprintf(stderr, "openbgi: %s: input: \"tapDelay\" is 0 to 2000 milliseconds\n", path);
	n = Json_Get(obj, "pointer");
	if(n && (s->pointer = Word(n, kPointer, 3)) >= 0)
		said = 1;
	else if(n)
		fprintf(stderr, "openbgi: %s: input: \"pointer\" is \"pad\", \"always\" or \"never\"\n", path);
	n = Json_Get(obj, "pointerSize");
	if(n && (s->pointerSize = Word(n, kPointerSize, 3)) >= 0)
		said = 1;
	else if(n)
		fprintf(stderr, "openbgi: %s: input: \"pointerSize\" is \"small\", \"normal\" or \"large\"\n", path);
	n = Json_Get(obj, "stickSpeed");
	if(n && n->type == JSON_NUMBER && n->num >= INPUT_STICK_MIN && n->num <= INPUT_STICK_MAX)
	{
		s->stickSpeed = (int)n->num;
		said = 1;
	}
	else if(n)
		fprintf(stderr, "openbgi: %s: input: \"stickSpeed\" is %d to %d percent\n", path, INPUT_STICK_MIN, INPUT_STICK_MAX);
	n = Json_Get(obj, "onScreenKeyboard");
	if(n && n->type == JSON_BOOL)
	{
		s->onScreenKeyboard = n->num != 0;
		said = 1;
	}
	n = Json_Get(obj, "settingsKey");
	if(n && n->type == JSON_STRING && strcmp(n->str, "none") == 0)
	{
		s->settingsKey = 0;
		said = 1;
	}
	else if(n && n->type == JSON_STRING && InputKey_ByName(strncmp(n->str, "key:", 4) == 0 ? n->str + 4 : n->str) != 0)
	{
		s->settingsKey = InputKey_ByName(strncmp(n->str, "key:", 4) == 0 ? n->str + 4 : n->str);
		said = 1;
	}
	else if(n)
		fprintf(stderr, "openbgi: %s: input: \"settingsKey\" is a key (\"F9\") or \"none\"\n", path);
	spots = Json_Get(obj, "hotspots");
	if(spots && spots->type == JSON_ARRAY && Json_Count(spots) > 0)
	{
		int count = Json_Count(spots);
		if(count > INPUT_HOTSPOTS_MAX)
			count = INPUT_HOTSPOTS_MAX;
		s->hotspots = (InputHotspot_t*)BGI_Calloc(sizeof *s->hotspots * (size_t)count);
		for(e = spots->child; e && s->hotspots && s->hotspotCount < count; e = e->next)
		{
			InputHotspot_t* h = &s->hotspots[s->hotspotCount];
			const JsonNode_t* rect = Json_Get(e, "rect");
			if(!rect || rect->type != JSON_ARRAY || Json_Count(rect) != 4 || Element(rect, 2, 0) <= 0 || Element(rect, 3, 0) <= 0)
			{
				fprintf(stderr, "openbgi: %s: input: a hotspot needs \"rect\": [x, y, width, height]\n", path);
				continue;
			}
			snprintf(h->name, sizeof h->name, "%s", Json_String(e, "name", ""));
			snprintf(h->program, sizeof h->program, "%s", Json_String(e, "program", ""));
			h->x = Element(rect, 0, 0);
			h->y = Element(rect, 1, 0);
			h->w = Element(rect, 2, 0);
			h->h = Element(rect, 3, 0);
			s->hotspotCount++;
			said = 1;
		}
	}
	if(!said)
	{
		InputSettings_Free(s);
		return NULL;
	}
	return s;
}

void InputSettings_Free(InputSettings_t* s)
{
	if(!s)
		return;
	BGI_Free(s->hotspots);
	BGI_Free(s);
}

// ---- writing ----------------------------------------------------------------------------------

typedef struct Text
{
	char* s;
	size_t len, cap;
} Text_t;

static void Put(Text_t* t, const char* text)
{
	size_t n = strlen(text);
	if(t->len + n + 1 > t->cap)
	{
		char* grown;
		t->cap = (t->cap + n + 256) * 2;
		grown = (char*)realloc(t->s, t->cap);
		if(!grown)
			return;
		t->s = grown;
	}
	if(!t->s)
		return;
	memcpy(t->s + t->len, text, n + 1);
	t->len += n;
}

// a JSON string literal (UTF-8 passes through)
static void PutString(Text_t* t, const char* s)
{
	char buf[8];
	Put(t, "\"");
	for(; *s; s++)
	{
		unsigned char c = (unsigned char)*s;
		if(c == '"' || c == '\\')
			snprintf(buf, sizeof buf, "\\%c", c);
		else if(c < 0x20)
			snprintf(buf, sizeof buf, "\\u%04x", c);
		else
			snprintf(buf, sizeof buf, "%c", c);
		Put(t, buf);
	}
	Put(t, "\"");
}

char* InputSettings_Format(const InputSettings_t* s)
{
	Text_t t;
	char buf[96], name[16];
	const char* sep = "";
	int i, any = 0;
	memset(&t, 0, sizeof t);
	Put(&t, "{");
	for(i = 0; i < INPUT_CONTROLS; i++)
		any |= s->bound[i];
	if(any)
	{
		const char* in = "";
		Put(&t, "\"pad\": {");
		for(i = 0; i < INPUT_CONTROLS; i++)
			if(s->bound[i])
			{
				Put(&t, in);
				PutString(&t, InputControl_Name(i, name));
				Put(&t, ": ");
				InputBind_Format(s->bind[i], buf, sizeof buf);
				PutString(&t, buf);
				in = ", ";
			}
		Put(&t, "}");
		sep = ", ";
	}
	if(s->ownPadKeys >= 0)
	{
		snprintf(buf, sizeof buf, "%s\"ownPadKeys\": %s", sep, s->ownPadKeys ? "true" : "false");
		Put(&t, buf);
		sep = ", ";
	}
	if(s->tapDelay >= 0)
	{
		snprintf(buf, sizeof buf, "%s\"tapDelay\": %d", sep, s->tapDelay);
		Put(&t, buf);
		sep = ", ";
	}
	if(s->pointer >= 0 && s->pointer < 3)
	{
		snprintf(buf, sizeof buf, "%s\"pointer\": \"%s\"", sep, kPointer[s->pointer]);
		Put(&t, buf);
		sep = ", ";
	}
	if(s->pointerSize >= 0 && s->pointerSize < 3)
	{
		snprintf(buf, sizeof buf, "%s\"pointerSize\": \"%s\"", sep, kPointerSize[s->pointerSize]);
		Put(&t, buf);
		sep = ", ";
	}
	if(s->stickSpeed >= 0)
	{
		snprintf(buf, sizeof buf, "%s\"stickSpeed\": %d", sep, s->stickSpeed);
		Put(&t, buf);
		sep = ", ";
	}
	if(s->onScreenKeyboard >= 0)
	{
		snprintf(buf, sizeof buf, "%s\"onScreenKeyboard\": %s", sep, s->onScreenKeyboard ? "true" : "false");
		Put(&t, buf);
		sep = ", ";
	}
	if(s->settingsKey >= 0)
	{
		char key[24] = "key:none";
		if(s->settingsKey > 0)
			InputBind_Format((uint16_t)(INPUT_BIND_KEY | (s->settingsKey & 0xff)), key, sizeof key);
		snprintf(buf, sizeof buf, "%s\"settingsKey\": \"%s\"", sep, key + 4);
		Put(&t, buf);
		sep = ", ";
	}
	if(s->hotspotCount > 0)
	{
		Put(&t, sep);
		Put(&t, "\"hotspots\": [");
		for(i = 0; i < s->hotspotCount; i++)
		{
			const InputHotspot_t* h = &s->hotspots[i];
			Put(&t, i ? ", {" : "{");
			if(h->name[0])
			{
				Put(&t, "\"name\": ");
				PutString(&t, h->name);
				Put(&t, ", ");
			}
			snprintf(buf, sizeof buf, "\"rect\": [%d, %d, %d, %d]", h->x, h->y, h->w, h->h);
			Put(&t, buf);
			if(h->program[0])
			{
				Put(&t, ", \"program\": ");
				PutString(&t, h->program);
			}
			Put(&t, "}");
		}
		Put(&t, "]");
	}
	Put(&t, "}");
	return t.s;
}
