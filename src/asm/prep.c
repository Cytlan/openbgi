/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * prep.c - the preprocessor of the source languages: `#include`,
 *          `#define` and `#undef` on the text of a source (interface and
 *          the directives in prep.h)
 *
 * One pass over the lines of the source and, in place of each `#include`,
 * of the file it names.  A line is copied to the output with the defined
 * names in it replaced; the scanner tells code from string literals,
 * character literals and comments - a block comment and a string may run
 * over several lines - and replaces only in code.  What a name stands for
 * is scanned again where the name is used, with the name itself out of
 * reach meanwhile, so a definition may use other names but cannot loop.
 * Every output line records the file and line it came from.
 */
#include "bgi/prep.h"
#include "bgi/os.h"
#include "bgi/os_common.h"

#include <ctype.h>
#include <stdarg.h>

#define PREP_DEPTH_MAX  16  // #include inside an included file, this deep
#define PREP_FILES_MAX  256 // files of one source
#define PREP_PATH_MAX   1024
#define PREP_NESTED_MAX 32 // names inside the text of names

// a defined name
typedef struct Macro
{
	char* name; // NULL: the slot is free
	char* body; // NULL: undefined again (the slot stays taken)
	size_t bodyLen;
	int file, line; // where it was defined
	int active;     // being replaced right now: not replaced inside its own text
} Macro_t;

// a file of the source
typedef struct PrepFile
{
	char* name; // as the messages name it
	char* path; // the native path it was read from, which its own #include lines are relative to
	char* key;  // the full path without "." and ".." steps: a file is read once however often it is included
	char* text; // the text of an included file (the source itself belongs to the caller)
} PrepFile_t;

// what the scanner is inside of at the end of a line
typedef struct Scan
{
	int comment; // a block comment
	int string;  // a string literal
} Scan_t;

typedef struct Prep
{
	const PrepIncludes_t* inc;
	FILE* log;
	int errors, warnings;
	char* buf; // the expanded text
	size_t len, cap;
	PrepLine_t* lines;
	int lineCount, lineCap;
	PrepFile_t files[PREP_FILES_MAX];
	int fileCount;
	int depth;       // of the includes
	Macro_t* macros; // open addressing by the name's hash
	uint32_t macroCap, macroCount;
} Prep_t;

static void Errorf(Prep_t* p, int file, int line, const char* fmt, ...)
{
	va_list ap;
	p->errors++;
	if(p->errors > 50)
		return;
	fprintf(p->log, "%s:%d: error: ", p->files[file].name, line);
	va_start(ap, fmt);
	vfprintf(p->log, fmt, ap);
	va_end(ap);
	fputc('\n', p->log);
}

static int IsIdentStart(int c)
{
	return isalpha(c) || c == '_' || c >= 0x80;
}

static int IsIdentChar(int c)
{
	return isalnum(c) || c == '_' || c >= 0x80;
}

// ---- the output -------------------------------------------------------------------------------------

static void Put(Prep_t* p, const char* s, size_t n)
{
	if(p->len + n + 1 > p->cap)
	{
		p->cap = (p->len + n + 1) * 2 + 4096;
		p->buf = (char*)realloc(p->buf, p->cap);
	}
	memcpy(p->buf + p->len, s, n);
	p->len += n;
}

// the line being written ends: it came from `line` of `file`
static void EndLine(Prep_t* p, int file, int line)
{
	if(p->lineCount == p->lineCap)
	{
		p->lineCap = p->lineCap ? p->lineCap * 2 : 1024;
		p->lines = (PrepLine_t*)realloc(p->lines, (size_t)p->lineCap * sizeof *p->lines);
	}
	p->lines[p->lineCount].file = file;
	p->lines[p->lineCount].line = line;
	p->lineCount++;
	Put(p, "\n", 1);
}

// ---- the defined names --------------------------------------------------------------------------------

static uint32_t HashName(const char* s, size_t n)
{
	uint32_t h = 2166136261u;
	size_t i;
	for(i = 0; i < n; i++)
		h = (h ^ (uint8_t)s[i]) * 16777619u;
	return h;
}

// the slot of the name: the one that holds it, or the free one it would take
static Macro_t* MacroSlot(Prep_t* p, const char* s, size_t n)
{
	uint32_t i = HashName(s, n) & (p->macroCap - 1);
	while(p->macros[i].name && !(strlen(p->macros[i].name) == n && memcmp(p->macros[i].name, s, n) == 0))
		i = (i + 1) & (p->macroCap - 1);
	return &p->macros[i];
}

static Macro_t* FindMacro(Prep_t* p, const char* s, size_t n)
{
	Macro_t* m;
	if(!p->macroCount)
		return NULL;
	m = MacroSlot(p, s, n);
	return m->name && m->body ? m : NULL;
}

static void GrowMacros(Prep_t* p)
{
	Macro_t* old = p->macros;
	uint32_t oldCap = p->macroCap, i;
	p->macroCap = oldCap ? oldCap * 2 : 256;
	p->macros = (Macro_t*)calloc(p->macroCap, sizeof *p->macros);
	for(i = 0; i < oldCap; i++)
		if(old[i].name)
			*MacroSlot(p, old[i].name, strlen(old[i].name)) = old[i];
	free(old);
}

/* What a definition stands for: the rest of its line without comments,
 * the blanks at both ends dropped (malloc'ed, `*outLen` bytes and a NUL);
 * `*openString` is set when a string literal in it lacks its end. */
static char* DefinitionText(const char* s, const char* e, size_t* outLen, int* openString)
{
	char* b = (char*)malloc((size_t)(e - s) + 1);
	size_t n = 0, start = 0;
	int string = 0;
	while(s < e)
	{
		if(string)
		{
			if(*s == '\\' && s + 1 < e)
				b[n++] = *s++;
			else if(*s == '"')
				string = 0;
			b[n++] = *s++;
		}
		else if(*s == '/' && s + 1 < e && s[1] == '/')
			break;
		else if(*s == '/' && s + 1 < e && s[1] == '*')
		{ // a comment inside the line counts as a blank
			s += 2;
			while(s < e && !(*s == '*' && s + 1 < e && s[1] == '/'))
				s++;
			s = s + 2 < e ? s + 2 : e;
			b[n++] = ' ';
		}
		else if(*s == '\'' && s + 2 < e && s[2] == '\'')
		{
			memcpy(b + n, s, 3);
			n += 3;
			s += 3;
		}
		else
		{
			string = *s == '"';
			b[n++] = *s++;
		}
	}
	while(n && (b[n - 1] == ' ' || b[n - 1] == '\t' || b[n - 1] == '\r'))
		n--;
	while(start < n && (b[start] == ' ' || b[start] == '\t'))
		start++;
	memmove(b, b + start, n - start);
	*openString = string;
	*outLen = n - start;
	b[*outLen] = 0;
	return b;
}

// `#define` (define 1) or `#undef` with what follows the directive's word on its line
static void Define(Prep_t* p, const char* s, const char* e, int define, int file, int line)
{
	const char* name;
	size_t n;
	Macro_t* m;
	while(s < e && (*s == ' ' || *s == '\t'))
		s++;
	name = s;
	if(s < e && IsIdentStart((unsigned char)*s))
		while(s < e && IsIdentChar((unsigned char)*s))
			s++;
	n = (size_t)(s - name);
	if(!n)
	{
		Errorf(p, file, line, "a name was expected after #%s", define ? "define" : "undef");
		return;
	}
	if(define && s < e && *s == '(')
	{
		Errorf(p, file, line, "%.*s: a name with parameters cannot be defined, only a constant", (int)n, name);
		return;
	}
	if(p->macroCount * 2 >= p->macroCap)
		GrowMacros(p);
	m = MacroSlot(p, name, n);
	if(!define)
	{
		if(m->name)
		{
			free(m->body);
			m->body = NULL;
		}
		return;
	}
	{
		size_t len;
		int openString;
		char* body = DefinitionText(s, e, &len, &openString);
		if(openString)
			Errorf(p, file, line, "%.*s: the string in its text does not end on the line", (int)n, name);
		if(m->name && m->body)
		{ // defined before: the same text again is fine (two files may both state a constant)
			if(m->bodyLen != len || memcmp(m->body, body, len) != 0)
				Errorf(p, file, line, "%s is defined already (%s:%d); #undef it first to give it another meaning", m->name,
					p->files[m->file].name, m->line);
			free(body);
			return;
		}
		if(!m->name)
		{
			m->name = (char*)malloc(n + 1);
			memcpy(m->name, name, n);
			m->name[n] = 0;
			p->macroCount++;
		}
		m->body = body;
		m->bodyLen = len;
		m->file = file;
		m->line = line;
		m->active = 0;
	}
}

// ---- the lines ----------------------------------------------------------------------------------------

static int IsOperatorChar(int c)
{
	return c && strchr("+-*/<>=!&|^%", c) != NULL;
}

/* Copy [s, e) to the output with the defined names replaced.  `st` is
 * where the scanner stands - inside a comment or a string that began on
 * an earlier line - and is updated; `nested` counts the names whose text
 * is being read. */
static void Expand(Prep_t* p, const char* s, const char* e, Scan_t* st, int nested, int file, int line)
{
	const char* start = s;
	while(s < e)
	{
		const char* q = s;
		if(st->comment)
		{
			while(q < e && !(*q == '*' && q + 1 < e && q[1] == '/'))
				q++;
			if(q < e)
			{
				q += 2;
				st->comment = 0;
			}
		}
		else if(st->string)
		{
			while(q < e && *q != '"')
				q += *q == '\\' && q + 1 < e ? 2 : 1;
			if(q < e)
			{
				q++;
				st->string = 0;
			}
		}
		else if(*s == '/' && s + 1 < e && s[1] == '/')
			q = e;
		else if(*s == '/' && s + 1 < e && s[1] == '*')
		{
			st->comment = 1;
			q += 2;
		}
		else if(*s == '"')
		{
			st->string = 1;
			q++;
		}
		else if(*s == '\'' && s + 2 < e && s[2] == '\'')
			q += 3;
		else if(isdigit((unsigned char)*s))
			while(q < e && (isalnum((unsigned char)*q) || *q == '_'))
				q++; // a number with its letters (0x1F) is no name
		else if(IsIdentStart((unsigned char)*s))
		{
			Macro_t* m;
			while(q < e && IsIdentChar((unsigned char)*q))
				q++;
			// family.name of an instruction: the part after the dot is no name of its own
			m = s > start && s[-1] == '.' ? NULL : FindMacro(p, s, (size_t)(q - s));
			if(m && !m->active)
			{
				if(nested >= PREP_NESTED_MAX)
					Errorf(p, file, line, "%s: the definitions are nested too deep", m->name);
				else if(m->bodyLen)
				{
					Scan_t inner = {0, 0};
					// keep an operator at either end of the text apart from one beside the name
					if(p->len && IsOperatorChar((unsigned char)p->buf[p->len - 1]) && IsOperatorChar((unsigned char)m->body[0]))
						Put(p, " ", 1);
					m->active = 1;
					Expand(p, m->body, m->body + m->bodyLen, &inner, nested + 1, file, line);
					m->active = 0;
					if(q < e && IsOperatorChar((unsigned char)*q) && IsOperatorChar((unsigned char)m->body[m->bodyLen - 1]))
						Put(p, " ", 1);
				}
				s = q;
				continue;
			}
		}
		else
			q++;
		Put(p, s, (size_t)(q - s));
		s = q;
	}
}

// ---- the files ----------------------------------------------------------------------------------------

static char* DupStr(const char* s)
{
	size_t n = strlen(s) + 1;
	char* d = (char*)malloc(n);
	memcpy(d, s, n);
	return d;
}

static int IsSep(int c)
{
	return c == '/' || c == OS_NATIVE_SEP;
}

/* `dir` and a name as one native path; a directory that ends in a
 * separator (or is empty) takes none */
static void JoinPath(char* out, size_t n, const char* dir, const char* name)
{
	if(!dir[0] || OsCommon_FilePart(dir)[0] == 0)
		snprintf(out, n, "%s%s", dir, name);
	else
		snprintf(out, n, "%s%c%s", dir, OS_NATIVE_SEP, name);
}

/* The full path of a file without its "." and "dir/.." components: what
 * tells whether two #include lines name the same file. */
static void PathKey(const char* path, char* out, size_t n)
{
	char full[PREP_PATH_MAX];
	size_t i = 0, o = 0, start;
	OS_NativeAbsolute(path, full, sizeof full);
	if(n == 0)
		return;
	// what precedes the first separator (nothing, or a drive) is kept as it is
	while(full[i] && !IsSep((unsigned char)full[i]) && o + 1 < n)
		out[o++] = full[i++];
	start = o;
	while(full[i])
	{
		size_t len = 0;
		while(IsSep((unsigned char)full[i]))
			i++;
		while(full[i + len] && !IsSep((unsigned char)full[i + len]))
			len++;
		if(len == 0 || (len == 1 && full[i] == '.'))
			; // nothing
		else if(len == 2 && full[i] == '.' && full[i + 1] == '.')
		{ // drop the component before it
			while(o > start && !IsSep((unsigned char)out[o - 1]))
				o--;
			if(o > start)
				o--;
		}
		else if(o + len + 2 < n)
		{
			out[o++] = OS_NATIVE_SEP;
			memcpy(out + o, full + i, len);
			o += len;
		}
		i += len;
	}
	out[o] = 0;
}

// whether two keys name one file: Windows does not tell the case of the letters apart
static int SameKey(const char* a, const char* b)
{
#if defined(_WIN32)
	for(; *a && *b; a++, b++)
		if(tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
	return *a == *b;
#else
	return strcmp(a, b) == 0;
#endif
}

// the length of the byte order mark some editors put in front of UTF-8 text
static size_t BomLength(const char* text, size_t len)
{
	return len >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0 ? 3 : 0;
}

// a file joins the table: its index, -1 when the table is full
static int AddFile(Prep_t* p, const char* name, const char* path, char* text)
{
	PrepFile_t* f;
	char key[PREP_PATH_MAX];
	if(p->fileCount == PREP_FILES_MAX)
		return -1;
	f = &p->files[p->fileCount];
	f->name = DupStr(name);
	f->path = DupStr(path);
	PathKey(path, key, sizeof key);
	f->key = DupStr(key);
	f->text = text;
	return p->fileCount++;
}

static void ReadLines(Prep_t* p, const char* text, size_t len, int file);

/* #include "name" on line `line` of `file`: the lines of that file, here.
 * It is looked for beside the file that includes it, then in the
 * directories the caller listed; one that was read before - or is being
 * read - is not read again. */
static void Include(Prep_t* p, const char* s, const char* e, int file, int line)
{
	char name[260], rel[PREP_PATH_MAX], native[PREP_PATH_MAX], path[PREP_PATH_MAX], key[PREP_PATH_MAX];
	const PrepIncludes_t* inc = p->inc;
	const char* from = p->files[file].path;
	const char* q;
	int k, i, index, dirs = inc ? inc->dirCount : 0;
	size_t n;
	// "name": the bytes between the quotes as they are (no escapes)
	while(s < e && (*s == ' ' || *s == '\t'))
		s++;
	q = s < e && *s == '"' ? (const char*)memchr(s + 1, '"', (size_t)(e - s - 1)) : NULL;
	if(!q || q == s + 1)
	{
		Errorf(p, file, line, "#include \"file\" was expected");
		return;
	}
	snprintf(name, sizeof name, "%.*s", (int)(q - s - 1), s + 1);
	// a name is written with '/' whatever the host uses, and in UTF-8
	snprintf(rel, sizeof rel, "%s", name);
	for(n = 0; rel[n]; n++)
		if(rel[n] == '/')
			rel[n] = OS_NATIVE_SEP;
	OS_Utf8ToNative(rel, native, sizeof native);
	for(k = -1; k < dirs; k++)
	{
		uint8_t* text;
		uint32_t size = 0;
		if(k < 0)
			snprintf(path, sizeof path, "%.*s%s", (int)(OsCommon_FilePart(from) - from), from, native);
		else
			JoinPath(path, sizeof path, inc->dirs[k], native);
		PathKey(path, key, sizeof key);
		for(i = 0; i < p->fileCount; i++)
			if(SameKey(p->files[i].key, key))
				return;
		text = inc && inc->read ? inc->read(inc->ctx, path, &size) : OS_ReadNativeFile(path, &size);
		if(!text)
			continue;
		index = p->depth < PREP_DEPTH_MAX ? AddFile(p, path, path, (char*)text) : -1;
		if(index < 0)
		{
			free(text);
			Errorf(p, file, line, p->depth < PREP_DEPTH_MAX ? "too many included files" : "the includes are nested too deep");
			return;
		}
		p->depth++;
		ReadLines(p, (const char*)text, size, index);
		p->depth--;
		return;
	}
	Errorf(p, file, line, "cannot read the include file \"%s\"", name);
}

// the lines of a file: the directives of the preprocessor carried out, everything else expanded
static void ReadLines(Prep_t* p, const char* text, size_t len, int file)
{
	const char* s = text + BomLength(text, len);
	const char* end = text + len;
	Scan_t st = {0, 0};
	int line = 1;
	while(s < end)
	{
		const char* e = (const char*)memchr(s, '\n', (size_t)(end - s));
		const char* d = s;
		if(!e)
			e = end;
		while(d < e && (*d == ' ' || *d == '\t'))
			d++;
		if(!st.comment && !st.string && d < e && *d == '#')
		{
			const char* w = ++d;
			size_t n;
			while(d < e && isalpha((unsigned char)*d))
				d++;
			n = (size_t)(d - w);
			if(n == 7 && memcmp(w, "include", 7) == 0)
				Include(p, d, e, file, line);
			else if(n == 6 && memcmp(w, "define", 6) == 0)
				Define(p, d, e, 1, file, line);
			else if(n == 5 && memcmp(w, "undef", 5) == 0)
				Define(p, d, e, 0, file, line);
			else
				Put(p, s, (size_t)(e - s)); // a directive of the compiler: as it is
		}
		else
			Expand(p, s, e, &st, 0, file, line);
		EndLine(p, file, line);
		s = e < end ? e + 1 : end;
		line++;
	}
}

// whether the text has a line the preprocessor acts on (a cheap look: most sources have none)
static int HasDirective(const char* text, size_t len)
{
	const char* s = text;
	const char* end = text + len;
	while(s < end && (s = (const char*)memchr(s, '#', (size_t)(end - s))) != NULL)
	{
		size_t left = (size_t)(end - ++s);
		if((left >= 7 && memcmp(s, "include", 7) == 0) || (left >= 6 && memcmp(s, "define", 6) == 0) ||
			(left >= 5 && memcmp(s, "undef", 5) == 0))
			return 1;
	}
	return 0;
}

void Prep_Expand(const char* text, size_t len, const char* fileName, const PrepIncludes_t* inc, FILE* log, PrepText_t* out)
{
	Prep_t* p;
	char path[PREP_PATH_MAX];
	int i;
	uint32_t k;
	size_t bom = BomLength(text, len);
	memset(out, 0, sizeof *out);
	out->name = fileName;
	out->text = text + bom;
	out->len = len - bom;
	if(!HasDirective(text, len))
		return;
	p = (Prep_t*)calloc(1, sizeof *p); // the table of files makes it too large for the stack
	p->inc = inc;
	p->log = log ? log : stderr;
	// the text itself is file 0; its includes are looked for beside it
	if(inc && inc->baseDir)
		JoinPath(path, sizeof path, inc->baseDir, OsCommon_FilePart(fileName));
	else
		snprintf(path, sizeof path, "%s", fileName);
	AddFile(p, fileName, path, NULL);
	ReadLines(p, text, len, 0);
	Put(p, "", 1); // the NUL
	out->expanded = p->buf;
	out->text = p->buf;
	out->len = p->len - 1;
	out->lines = p->lines;
	out->lineCount = p->lineCount;
	out->errors = p->errors;
	out->warnings = p->warnings;
	out->files = (char**)malloc((size_t)p->fileCount * sizeof *out->files);
	out->fileCount = p->fileCount;
	for(i = 0; i < p->fileCount; i++)
	{
		out->files[i] = p->files[i].name;
		free(p->files[i].path);
		free(p->files[i].key);
		free(p->files[i].text);
	}
	for(k = 0; k < p->macroCap; k++)
	{
		free(p->macros[k].name);
		free(p->macros[k].body);
	}
	free(p->macros);
	free(p);
}

const char* Prep_Where(const PrepText_t* t, int line, int* srcLine)
{
	if(!t->lines || line < 1 || line > t->lineCount)
	{
		*srcLine = line;
		return t->name;
	}
	*srcLine = t->lines[line - 1].line;
	return t->files[t->lines[line - 1].file];
}

void Prep_Free(PrepText_t* t)
{
	int i;
	for(i = 0; i < t->fileCount; i++)
		free(t->files[i]);
	free(t->files);
	free(t->lines);
	free(t->expanded);
	memset(t, 0, sizeof *t);
}
