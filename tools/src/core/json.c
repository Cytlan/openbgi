/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * json.c - a small recursive-descent JSON reader; interface in json.h
 *
 * Accepts standard JSON (RFC 8259) plus '//' and slash-star comments so that
 * games.json can be annotated.  Not part of the original engine.  The parser
 * works on a (pointer, end) pair over the caller's text and never reads past
 * `end`; on an error it sets `failed`, frees what it built and leaves the
 * pointer near the offending character, which Json_Parse reports as the
 * error offset.
 */
#include "bgi/json.h"

typedef struct Parser
{
	const char* p;   // next character
	const char* end; // one past the last character
	int failed;      // set on the first syntax error
} Parser_t;

// skip blanks, line breaks and both comment forms
static void SkipSpace(Parser_t* ps)
{
	for(;;)
	{
		while(ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
			ps->p++;
		if(ps->p + 1 < ps->end && ps->p[0] == '/' && ps->p[1] == '/')
		{
			while(ps->p < ps->end && *ps->p != '\n')
				ps->p++;
			continue;
		}
		if(ps->p + 1 < ps->end && ps->p[0] == '/' && ps->p[1] == '*')
		{
			ps->p += 2;
			while(ps->p + 1 < ps->end && !(ps->p[0] == '*' && ps->p[1] == '/'))
				ps->p++;
			ps->p = ps->p + 2 <= ps->end ? ps->p + 2 : ps->end; // unterminated: ends the text
			continue;
		}
		return;
	}
}

static JsonNode_t* NewNode(JsonType_t type)
{
	JsonNode_t* n = (JsonNode_t*)BGI_Calloc(sizeof *n);
	n->type = type;
	return n;
}

// four hex digits at s into *out; 0 when one of them is not a hex digit
static int Hex4(const char* s, uint32_t* out)
{
	uint32_t v = 0;
	int i;
	for(i = 0; i < 4; i++)
	{
		char c = s[i];
		v <<= 4;
		if(c >= '0' && c <= '9')
			v |= (uint32_t)(c - '0');
		else if(c >= 'a' && c <= 'f')
			v |= (uint32_t)(c - 'a' + 10);
		else if(c >= 'A' && c <= 'F')
			v |= (uint32_t)(c - 'A' + 10);
		else
			return 0;
	}
	*out = v;
	return 1;
}

// encode the code point cp as UTF-8 (1 to 4 bytes); returns the bytes written
static size_t PutUtf8(char* out, uint32_t cp)
{
	if(cp < 0x80)
	{
		out[0] = (char)cp;
		return 1;
	}
	if(cp < 0x800)
	{
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if(cp < 0x10000)
	{
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	out[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

/* the string literal at ps->p (which is the opening quote), with its escapes
 * decoded (a \uXXXX high surrogate followed by a low one becomes one code
 * point); returns a new NUL-terminated string (BGI_Free it) and leaves ps->p
 * after the closing quote.  NULL on an unknown escape or an unterminated
 * literal. */
static char* ParseString(Parser_t* ps)
{
	const char* s = ps->p + 1;
	char* out;
	size_t o = 0;
	// the decoded text is never longer than the literal
	out = (char*)BGI_Alloc((size_t)(ps->end - s) + 1);
	while(s < ps->end && *s != '"')
	{
		if(*s == '\\')
		{
			s++;
			if(s >= ps->end)
				break;
			switch(*s)
			{
				case '"': out[o++] = '"'; break;
				case '\\': out[o++] = '\\'; break;
				case '/': out[o++] = '/'; break;
				case 'b': out[o++] = '\b'; break;
				case 'f': out[o++] = '\f'; break;
				case 'n': out[o++] = '\n'; break;
				case 'r': out[o++] = '\r'; break;
				case 't': out[o++] = '\t'; break;
				case 'u':
				{
					uint32_t cp, lo;
					if(s + 4 >= ps->end || !Hex4(s + 1, &cp))
						goto fail;
					s += 4;
					if(cp >= 0xd800 && cp < 0xdc00 && s + 6 < ps->end && s[1] == '\\' && s[2] == 'u' && Hex4(s + 3, &lo) &&
						lo >= 0xdc00 && lo < 0xe000)
					{
						cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
						s += 6;
					}
					o += PutUtf8(out + o, cp);
					break;
				}
				default: goto fail;
			}
			s++;
			continue;
		}
		out[o++] = *s++;
	}
	if(s >= ps->end)
		goto fail;
	out[o] = 0;
	ps->p = s + 1;
	return out;
fail:
	BGI_Free(out);
	ps->failed = 1;
	return NULL;
}

static JsonNode_t* ParseValue(Parser_t* ps);

/* an object or an array at ps->p (which is the opening bracket): the members
 * or elements are parsed in order and linked through `child` / `next`, a
 * member's key going to its node.  NULL (with everything built so far freed)
 * on a syntax error. */
static JsonNode_t* ParseContainer(Parser_t* ps, JsonType_t type)
{
	JsonNode_t* node = NewNode(type);
	JsonNode_t** tail = &node->child;
	char close = type == JSON_OBJECT ? '}' : ']';
	ps->p++;
	SkipSpace(ps);
	if(ps->p < ps->end && *ps->p == close)
	{
		ps->p++;
		return node;
	}
	for(;;)
	{
		JsonNode_t* v;
		char* key = NULL;
		SkipSpace(ps);
		if(type == JSON_OBJECT)
		{
			if(ps->p >= ps->end || *ps->p != '"')
				goto fail;
			key = ParseString(ps);
			if(!key)
				goto fail;
			SkipSpace(ps);
			if(ps->p >= ps->end || *ps->p != ':')
			{
				BGI_Free(key);
				goto fail;
			}
			ps->p++;
		}
		v = ParseValue(ps);
		if(!v)
		{
			BGI_Free(key);
			goto fail;
		}
		v->key = key;
		*tail = v;
		tail = &v->next;
		SkipSpace(ps);
		if(ps->p < ps->end && *ps->p == ',')
		{
			ps->p++;
			continue;
		}
		if(ps->p < ps->end && *ps->p == close)
		{
			ps->p++;
			return node;
		}
		goto fail;
	}
fail:
	ps->failed = 1;
	Json_Free(node);
	return NULL;
}

// any value at ps->p: a container, a string, true / false / null or a number; NULL on error
static JsonNode_t* ParseValue(Parser_t* ps)
{
	JsonNode_t* n;
	SkipSpace(ps);
	if(ps->p >= ps->end)
	{
		ps->failed = 1;
		return NULL;
	}
	switch(*ps->p)
	{
		case '{': return ParseContainer(ps, JSON_OBJECT);
		case '[': return ParseContainer(ps, JSON_ARRAY);
		case '"':
		{
			char* s = ParseString(ps);
			if(!s)
				return NULL;
			n = NewNode(JSON_STRING);
			n->str = s;
			return n;
		}
		case 't':
		case 'f':
		case 'n':
		{
			static const struct
			{
				const char* word;
				JsonType_t type;
				double num;
			} words[] = {{"true", JSON_BOOL, 1}, {"false", JSON_BOOL, 0}, {"null", JSON_NULL, 0}};
			size_t i;
			for(i = 0; i < BGI_COUNTOF(words); i++)
			{
				size_t len = strlen(words[i].word);
				if((size_t)(ps->end - ps->p) >= len && memcmp(ps->p, words[i].word, len) == 0)
				{
					ps->p += len;
					n = NewNode(words[i].type);
					n->num = words[i].num;
					return n;
				}
			}
			ps->failed = 1;
			return NULL;
		}
		default:
		{
			// a number: after the check of the first character strtod does the
			// parsing, so a little more than JSON is accepted (leading zeros, hex
			// floats, "-inf")
			char* endp;
			double v;
			if(!(*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9')))
			{
				ps->failed = 1;
				return NULL;
			}
			v = strtod(ps->p, &endp);
			if(endp == ps->p)
			{
				ps->failed = 1;
				return NULL;
			}
			ps->p = endp;
			n = NewNode(JSON_NUMBER);
			n->num = v;
			return n;
		}
	}
}

/* parse `len` bytes of text into a tree (see json.h); a UTF-8 byte order
 * mark is skipped and nothing but blanks and comments may follow the value.
 * NULL on a syntax error with *err (when given) the offset of the error, 0
 * on success. */
JsonNode_t* Json_Parse(const char* text, size_t len, size_t* err)
{
	Parser_t ps;
	JsonNode_t* root;
	// a UTF-8 byte order mark is allowed
	if(len >= 3 && (uint8_t)text[0] == 0xef && (uint8_t)text[1] == 0xbb && (uint8_t)text[2] == 0xbf)
	{
		text += 3;
		len -= 3;
	}
	ps.p = text;
	ps.end = text + len;
	ps.failed = 0;
	root = ParseValue(&ps);
	if(root)
	{
		SkipSpace(&ps);
		if(ps.p != ps.end)
		{
			Json_Free(root);
			root = NULL;
			ps.failed = 1;
		}
	}
	if(err)
		*err = ps.failed ? (size_t)(ps.p - text) : 0;
	return root;
}

// free a node, its children and every sibling after it
void Json_Free(JsonNode_t* n)
{
	while(n)
	{
		JsonNode_t* next = n->next;
		Json_Free(n->child);
		BGI_Free(n->key);
		BGI_Free(n->str);
		BGI_Free(n);
		n = next;
	}
}

// the member `key` of an object (the first one of that name); NULL otherwise
const JsonNode_t* Json_Get(const JsonNode_t* obj, const char* key)
{
	const JsonNode_t* m;
	if(!obj || obj->type != JSON_OBJECT)
		return NULL;
	for(m = obj->child; m; m = m->next)
		if(m->key && strcmp(m->key, key) == 0)
			return m;
	return NULL;
}

// the string member `key`, or def when missing or not a string
const char* Json_String(const JsonNode_t* obj, const char* key, const char* def)
{
	const JsonNode_t* m = Json_Get(obj, key);
	return m && m->type == JSON_STRING ? m->str : def;
}

// the number (or bool, as 0 / 1) member `key`, or def when missing or another type
double Json_Number(const JsonNode_t* obj, const char* key, double def)
{
	const JsonNode_t* m = Json_Get(obj, key);
	return m && (m->type == JSON_NUMBER || m->type == JSON_BOOL) ? m->num : def;
}

// the number of children: elements of an array, members of an object; 0 for NULL
int Json_Count(const JsonNode_t* arr)
{
	int n = 0;
	const JsonNode_t* c;
	if(!arr)
		return 0;
	for(c = arr->child; c; c = c->next)
		n++;
	return n;
}
