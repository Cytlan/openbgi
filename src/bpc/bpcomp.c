/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bpcomp.c - the program compiler: the C-like source (docs/bpc.md) to a
 *            program file (interface in bpc.h, the code shapes in
 *            bpc_internal.h)
 *
 * A recursive-descent parser builds a tree of the program; the generator
 * then writes an assembler listing (docs/asm.md) the way the original
 * compiler laid the code out - the frame prologue and the parameter
 * pops, every statement's value left on the stack, the arguments of a
 * function right to left and of an instruction left to right, conditions
 * as chains of conditional jumps, one exit per function, the string
 * literals after the code in order of appearance - and Asm_Assemble
 * turns the listing into the file.  The decompiler (bpdec.c) relies on
 * these rules to reproduce a program exactly.
 *
 * Two things the decompiler never writes are for the people who edit a
 * source.  The text goes through the preprocessor first (prep.h), which
 * puts every `#include "file"` in its place and replaces the names of
 * `#define`; what the lexer reads is the expanded text, and a message
 * names the file and line a line of it came from.  And a variable can be
 * declared at a fixed address of one of the engine's memory areas -
 * `global int gAbort @ 1124;` in the global memory, `heap`, `data`, `code`
 * and `dynamic[n]` in the areas the engine's tagged pointers name.  Such
 * a declaration reserves nothing and emits nothing: the name stands for
 * the address, pushed the way the declaration wrote it.
 *
 * A third thing is the records: `struct Name { fields };` describes the
 * layout of a piece of memory, and a variable, a parameter or a field may
 * be of that type or a pointer to it.  `rec.field`, `ptr->field` and
 * `table[i].field` then compile to what the original programs do with
 * such data - the address, the field's offset and `add` - which the
 * decompiler can only write as `*(int*)(rec + 12)`.
 */
#include "bpc_internal.h"

#include <ctype.h>
#include <stdarg.h>

// ---- the lexer ------------------------------------------------------------------------------------

enum TokKind
{
	T_EOF,
	T_IDENT,
	T_NUM,
	T_STR,
	T_PUNCT,
	T_DIRECTIVE
};

typedef struct Token
{
	int kind;
	int line; // in the expanded text
	char text[128];
	char arg[64]; // T_DIRECTIVE: the rest of the line for #engine
	int64_t num;
	int wide;       // T_NUM: written with eight hex digits, which the compiler pushed as 32 bits whatever the value
	uint8_t* bytes; // T_STR: the bytes (Shift-JIS), freed by whoever consumes them
	uint32_t len;
} Token_t;

typedef struct Lex
{
	const char* p;
	const char* end;
	int line;
	int errors, warnings;
	FILE* log;
	Token_t tok;
	int peeked;
	Token_t peek;
	const PrepText_t* prep; // the expanded text being read: where its lines came from
} Lex_t;

// a message about a line of the expanded text, under the file and line that line came from
static void Message(Lex_t* l, int line, const char* kind, const char* fmt, va_list ap)
{
	int srcLine;
	const char* file = Prep_Where(l->prep, line, &srcLine);
	fprintf(l->log, "%s:%d: %s: ", file, srcLine, kind);
	vfprintf(l->log, fmt, ap);
	fputc('\n', l->log);
}

// an error at a line (a token's or a node's `line`); only the first 50 are shown
static void Errorf(Lex_t* l, int pos, const char* fmt, ...)
{
	va_list ap;
	l->errors++;
	if(l->errors > 50)
		return;
	va_start(ap, fmt);
	Message(l, pos, "error", fmt, ap);
	va_end(ap);
}

static void Warnf(Lex_t* l, int pos, const char* fmt, ...)
{
	va_list ap;
	l->warnings++;
	if(l->warnings > 50)
		return;
	va_start(ap, fmt);
	Message(l, pos, "warning", fmt, ap);
	va_end(ap);
}

static int IsIdentStart(int c)
{
	return isalpha(c) || c == '_' || c >= 0x80;
}

static int IsIdentChar(int c)
{
	return isalnum(c) || c == '_' || c >= 0x80;
}

static void SkipSpace(Lex_t* l)
{
	for(;;)
	{
		while(l->p < l->end && isspace((unsigned char)*l->p))
		{
			if(*l->p == '\n')
				l->line++;
			l->p++;
		}
		if(l->p + 1 < l->end && l->p[0] == '/' && l->p[1] == '/')
		{
			while(l->p < l->end && *l->p != '\n')
				l->p++;
			continue;
		}
		if(l->p + 1 < l->end && l->p[0] == '/' && l->p[1] == '*')
		{
			l->p += 2;
			while(l->p + 1 < l->end && !(l->p[0] == '*' && l->p[1] == '/'))
			{
				if(*l->p == '\n')
					l->line++;
				l->p++;
			}
			l->p += 2;
			continue;
		}
		break;
	}
}

static const char* const kPuncts[] = {">>>", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "<-", "->", "(", ")", "{", "}", "[", "]", ",",
	";", ":", "=", "<", ">", "+", "-", "*", "/", "%", "&", "|", "^", "!", "~", "?", "@", ".", NULL};

static void ReadToken(Lex_t* l, Token_t* t)
{
	memset(t, 0, sizeof *t);
	SkipSpace(l);
	t->line = l->line;
	if(l->p >= l->end)
	{
		t->kind = T_EOF;
		return;
	}
	if(*l->p == '#')
	{
		const char* s = ++l->p;
		while(l->p < l->end && IsIdentChar((unsigned char)*l->p))
			l->p++;
		t->kind = T_DIRECTIVE;
		snprintf(t->text, sizeof t->text, "%.*s", (int)(l->p - s), s);
		if(strcmp(t->text, "encoding") == 0)
		{ // "#encoding utf-8": takes effect here, before the next literal is read
			int ok;
			l->p = Asm_EncodingDirective(l->p, l->end, &ok);
			if(!ok)
				Errorf(l, l->line, "#encoding utf-8 or #encoding sjis");
		}
		if(strcmp(t->text, "engine") == 0)
		{ // the engine name: the rest of the line (it has dots and slashes)
			while(l->p < l->end && (*l->p == ' ' || *l->p == '\t'))
				l->p++;
			s = l->p;
			while(l->p < l->end && !isspace((unsigned char)*l->p))
				l->p++;
			snprintf(t->arg, sizeof t->arg, "%.*s", (int)(l->p - s), s);
		}
		return;
	}
	if(*l->p == '"')
	{
		const char* next;
		const char* err = Asm_ReadStringLiteral(l->p, l->end, &t->bytes, &t->len, &next);
		if(err)
		{
			Errorf(l, l->line, "%s", err);
			t->kind = T_EOF;
			l->p = l->end;
			return;
		}
		{
			const char* q;
			for(q = l->p; q < next; q++)
				if(*q == '\n')
					l->line++;
		}
		l->p = next;
		t->kind = T_STR;
		return;
	}
	if(*l->p == '\'' && l->p + 2 < l->end && l->p[2] == '\'')
	{
		t->kind = T_NUM;
		t->num = (uint8_t)l->p[1];
		l->p += 3;
		return;
	}
	if(isdigit((unsigned char)*l->p))
	{
		char* e;
		t->kind = T_NUM;
		t->wide = 0;
		if(l->p[0] == '0' && (l->p[1] == 'x' || l->p[1] == 'X'))
		{
			t->num = (int64_t)strtoull(l->p, &e, 16);
			t->wide = e - l->p == 10; // 0x00000000: eight digits
		}
		else
			t->num = (int64_t)strtoull(l->p, &e, 10);
		l->p = e;
		return;
	}
	if(IsIdentStart((unsigned char)*l->p))
	{
		const char* s = l->p;
		while(l->p < l->end && IsIdentChar((unsigned char)*l->p))
			l->p++;
		// family.name / family.0xNN: one token when the prefix names an instruction family (`op` the base set)
		if(l->p < l->end && *l->p == '.' && (Asm_FamilyByte(s, (size_t)(l->p - s)) >= 0 || (l->p - s == 2 && memcmp(s, "op", 2) == 0)))
		{
			l->p++;
			while(l->p < l->end && IsIdentChar((unsigned char)*l->p))
				l->p++;
		}
		t->kind = T_IDENT;
		snprintf(t->text, sizeof t->text, "%.*s", (int)(l->p - s), s);
		return;
	}
	{
		int i;
		for(i = 0; kPuncts[i]; i++)
		{
			size_t n = strlen(kPuncts[i]);
			if((size_t)(l->end - l->p) >= n && memcmp(l->p, kPuncts[i], n) == 0)
			{
				t->kind = T_PUNCT;
				memcpy(t->text, kPuncts[i], n + 1);
				l->p += n;
				return;
			}
		}
	}
	Errorf(l, l->line, "unexpected character '%c'", *l->p);
	l->p++;
	ReadToken(l, t);
}

static void Next(Lex_t* l)
{
	free(l->tok.bytes); // a string nobody took (after an error)
	l->tok.bytes = NULL;
	if(l->peeked)
	{
		l->tok = l->peek;
		l->peeked = 0;
	}
	else
		ReadToken(l, &l->tok);
}

static Token_t* Peek(Lex_t* l)
{
	if(!l->peeked)
	{
		ReadToken(l, &l->peek);
		l->peeked = 1;
	}
	return &l->peek;
}

static int IsPunct(const Lex_t* l, const char* p)
{
	return l->tok.kind == T_PUNCT && strcmp(l->tok.text, p) == 0;
}

static int IsIdent(const Lex_t* l, const char* name)
{
	return l->tok.kind == T_IDENT && strcmp(l->tok.text, name) == 0;
}

static int Accept(Lex_t* l, const char* p)
{
	if(IsPunct(l, p))
	{
		Next(l);
		return 1;
	}
	return 0;
}

static void Expect(Lex_t* l, const char* p)
{
	if(!Accept(l, p))
	{
		Errorf(l, l->tok.line, "'%s' expected, found '%s'", p, l->tok.kind == T_EOF ? "end of file" : l->tok.text);
		// resynchronize a little
		if(l->tok.kind != T_EOF)
			Next(l);
	}
}

// ---- the tree ---------------------------------------------------------------------------------------

enum NodeKind
{
	N_NUM,
	N_STR,      // bytes, len
	N_NAME,     // text
	N_CALL,     // a: the callee (N_NAME of a function, or any expression); items: the arguments
	N_OPCALL,   // op (fam << 8 | opcode); items: the arguments
	N_UNARY,    // op: '-', '~', '!'; a
	N_ADDR,     // &a: the address of an lvalue (a variable, an element, a field)
	N_INDEX,    // a[b]: an element of an array, or of what a pointer points to
	N_MEMBER,   // a.text, or a->text with op 1: a field of a record
	N_DEREF,    // *(T*)(a): size (0 / 1 / 2)
	N_BINARY,   // op: the opcode; a, b
	N_SELECT,   // a ? b : c
	N_ASSIGN,   // a = b (store_keep, or store when b is a call)
	N_ASSIGN2,  // a <- b (store)
	N_PAREN,    // (a): a value in a condition
	N_INITLIST, // {items}
	// statements
	N_EXPR,
	N_DECL,    // text, size (element), op (elements), num (1: an array), a: the initializer or NULL
	N_BLOCK,   // items
	N_IF,      // a: cond, b: then, c: else (may be NULL); num: 1 when the then part had no braces
	N_WHILE,   // a: cond (NULL: for (;;)), b: body
	N_DOWHILE, // a: cond, b: body
	N_FOR,     // a: init, b: cond, c: incr (statements / expressions, NULL when empty), items[0]: body
	N_BREAK,
	N_CONTINUE,
	N_RETURN,  // a: the value or NULL
	N_GOTO,    // text
	N_LABEL,   // text
	N_ASM,     // text: the lines
	N_INLINE,  // a: the address, bytes: the literal
	N_STOREMUL // a: the address, size, items: the values
};

typedef struct Node Node_t;
struct Node
{
	int kind;
	int line;
	int op;
	int size;
	int count, cap;
	int64_t num;
	Node_t* a;
	Node_t* b;
	Node_t* c;
	Node_t** items;
	char text[128];
	uint8_t* bytes;
	uint32_t len;
};

static Node_t* NewNode(int kind, int line)
{
	Node_t* n = (Node_t*)calloc(1, sizeof *n);
	n->kind = kind;
	n->line = line;
	return n;
}

static void AddItem(Node_t* n, Node_t* item)
{
	if(n->count == n->cap)
	{
		n->cap = n->cap ? n->cap * 2 : 4;
		n->items = (Node_t**)realloc(n->items, (size_t)n->cap * sizeof *n->items);
	}
	n->items[n->count++] = item;
}

static void FreeNode(Node_t* n)
{
	int i;
	if(!n)
		return;
	FreeNode(n->a);
	FreeNode(n->b);
	FreeNode(n->c);
	for(i = 0; i < n->count; i++)
		FreeNode(n->items[i]);
	free(n->items);
	free(n->bytes);
	free(n);
}

/* What a variable, a field or an element is: a scalar of 1, 2 or 4 bytes,
 * a record, a pointer to either (a word holding an address), or the
 * address of a function (`func`, a word too). */
typedef struct Record Record_t;
typedef struct TypeRef
{
	Record_t* rec; // the record type, or NULL for a scalar
	int elem;      // a scalar's size: 1, 2, 4
	int ptr;       // a pointer to that: the number of stars (char** p: 2)
	int func;      // the address of a function
} TypeRef_t;

typedef struct Field // one field of a record
{
	char name[64];
	TypeRef_t type;
	int isArray;
	uint32_t count; // the elements of an array
	uint32_t inner; // a two-dimensional array `f[count][inner]`: the elements of one row (0: one dimension)
	uint32_t off;   // its offset in the record
} Field_t;

struct Record // `struct Name { fields };`: the layout of a piece of memory
{
	char name[64];
	Field_t* fields;
	int fieldCount, fieldCap;
	uint32_t size; // bytes: the end of its last field
};

// the bytes one value of a type takes
static uint32_t TypeSize(const TypeRef_t* t)
{
	return (t->ptr || t->func) ? 4u : t->rec ? t->rec->size
											 : (uint32_t)t->elem;
}

// a word that holds an address: a pointer or a func
static int IsPointerType(const TypeRef_t* t)
{
	return t->ptr || t->func;
}

static const Field_t* FindField(const Record_t* r, const char* name)
{
	int i;
	for(i = 0; i < r->fieldCount; i++)
		if(strcmp(r->fields[i].name, name) == 0)
			return &r->fields[i];
	return NULL;
}

typedef struct Var
{
	char name[128];
	TypeRef_t type; // of the variable, or of one element of an array
	uint32_t size;  // bytes
	int isArray;
	uint32_t count; // the elements of an array
	uint32_t inner; // a two-dimensional array: the elements of one row (0: one dimension)
	uint32_t off;   // the frame offset from the frame's lowest address
} Var_t;

typedef struct Func
{
	char name[128];
	int params;
	Var_t* vars; // the parameters first, then the locals in declaration order
	int varCount, varCap;
	uint32_t frame;
	Node_t* body; // N_BLOCK
	int isAsm;    // the body is a single __asm block
	Node_t* asmNode;
} Func_t;

// the memory areas a variable can be declared in: what the tags of the engine's pointers name
enum Area
{
	AREA_GLOBAL,  // the global memory, tag 0: an address there is the plain number
	AREA_CODE,    // the running thread's code area (the program file as it was loaded)
	AREA_DATA,    // ... its data area (the frames of the locals)
	AREA_HEAP,    // ... its local heap ("70")
	AREA_DYNAMIC, // a dynamic global block of "80 20", by its number
	AREA_COUNT
};
static const char* const kAreaNames[AREA_COUNT] = {"global", "code", "data", "heap", "dynamic"};

/* a variable declared at an address of a memory area:
 * `global int name[count] @ address;` */
typedef struct AreaVar AreaVar_t;
struct AreaVar
{
	char name[128];
	int area;       // AREA_*
	uint32_t block; // AREA_DYNAMIC: the block number
	TypeRef_t type; // of the variable, or of one element of an array
	int isArray;
	uint32_t count; // the elements of an array
	uint32_t inner; // a two-dimensional array: the elements of one row (0: one dimension)
	Node_t* addr;   // the address as it was written: it is compiled as written wherever the name is used
	uint32_t value; // what it comes to: the offset in the area
	int line;
	int broken; // the address cannot be made for this engine (reported at the first use)
};

typedef struct Unit
{
	EngineGen_t gen;
	int genSet;
	Func_t** funcs;
	int funcCount, funcCap;
	AreaVar_t* areaVars; // in declaration order
	int areaCount, areaCap;
	Record_t** records; // the record types, in declaration order
	int recordCount, recordCap;
	char* program; // __program { listing }: the whole file as a listing
} Unit_t;

typedef struct Parser
{
	Lex_t lex;
	Unit_t* unit;
	Func_t* func;
} Parser_t;

// ---- declarations ------------------------------------------------------------------------------------

static Var_t* FindVar(Func_t* f, const char* name)
{
	int i;
	if(!f)
		return NULL;
	for(i = 0; i < f->varCount; i++)
		if(strcmp(f->vars[i].name, name) == 0)
			return &f->vars[i];
	return NULL;
}

static Var_t* AddVar(Func_t* f, const char* name, const TypeRef_t* type, uint32_t size, int isArray)
{
	Var_t* v;
	if(f->varCount == f->varCap)
	{
		f->varCap = f->varCap ? f->varCap * 2 : 16;
		f->vars = (Var_t*)realloc(f->vars, (size_t)f->varCap * sizeof *f->vars);
	}
	v = &f->vars[f->varCount++];
	memset(v, 0, sizeof *v);
	snprintf(v->name, sizeof v->name, "%s", name);
	v->type = *type;
	v->size = size;
	v->isArray = isArray;
	v->off = f->frame;
	f->frame += size;
	return v;
}

static Func_t* FindFunc(Unit_t* u, const char* name)
{
	int i;
	for(i = 0; i < u->funcCount; i++)
		if(strcmp(u->funcs[i]->name, name) == 0)
			return u->funcs[i];
	return NULL;
}

static AreaVar_t* FindArea(Unit_t* u, const char* name)
{
	int i;
	for(i = 0; i < u->areaCount; i++)
		if(strcmp(u->areaVars[i].name, name) == 0)
			return &u->areaVars[i];
	return NULL;
}

static Record_t* FindRecord(Unit_t* u, const char* name)
{
	int i;
	for(i = 0; i < u->recordCount; i++)
		if(strcmp(u->records[i]->name, name) == 0)
			return u->records[i];
	return NULL;
}

static int TypeKeyword(const Lex_t* l, int* elem)
{
	if(IsIdent(l, "int"))
		*elem = 4;
	else if(IsIdent(l, "char"))
		*elem = 1;
	else if(IsIdent(l, "short"))
		*elem = 2;
	else
		return 0;
	return 1;
}

/* whether the lexer is at a type: `int`, `short` or `char`, or - when a
 * name or a `*` follows, as in a declaration - `func` or the name of a
 * record (those two are ordinary words anywhere else) */
static int AtType(Parser_t* p)
{
	Lex_t* l = &p->lex;
	const Token_t* next;
	int elem;
	if(l->tok.kind != T_IDENT)
		return 0;
	if(TypeKeyword(l, &elem))
		return 1;
	if(!IsIdent(l, "func") && !FindRecord(p->unit, l->tok.text))
		return 0;
	next = Peek(l);
	return next->kind == T_IDENT || (next->kind == T_PUNCT && strcmp(next->text, "*") == 0);
}

static int EvalConst(Parser_t* p, const Node_t* n, const struct AreaVar* var, int leftmost, uint32_t* out);
static Node_t* ParseBinary(Parser_t* p, int minPrec);

/* the dimensions after a name, the lexer behind the `[`: `count]` or
 * `count][inner]` - a two-dimensional array is `count` rows of `inner`
 * elements, and indexing it once gives a row.  The counts are constant
 * expressions. */
static void ParseDims(Parser_t* p, const char* name, uint32_t* count, uint32_t* inner)
{
	Lex_t* l = &p->lex;
	int line = l->tok.line, k;
	*inner = 0;
	for(k = 0; k < 2; k++)
	{
		uint32_t* dst = k ? inner : count;
		Node_t* e = ParseBinary(p, 0);
		if(EvalConst(p, e, NULL, 1, dst) && *dst == 0)
			Errorf(l, line, "%s: an array has at least one element", name);
		FreeNode(e);
		Expect(l, "]");
		if(k || !Accept(l, "["))
			break;
	}
}

// read the type the lexer is at (AtType): the type word and an optional `*` for a pointer to it
static void ParseType(Parser_t* p, TypeRef_t* t)
{
	Lex_t* l = &p->lex;
	int line = l->tok.line;
	memset(t, 0, sizeof *t);
	t->elem = 4;
	if(IsIdent(l, "func"))
		t->func = 1;
	else if(!TypeKeyword(l, &t->elem))
		t->rec = FindRecord(p->unit, l->tok.text);
	Next(l);
	(void)line;
	while(Accept(l, "*")) // T* points to a T, T** to a T* (a table of addresses), func* to a table of functions
		t->ptr++;
}

// ---- expressions -------------------------------------------------------------------------------------

static Node_t* ParseExpression(Parser_t* p);
static Node_t* ParseAssignment(Parser_t* p);

static Node_t* StrNode(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* n = NewNode(N_STR, l->tok.line);
	n->bytes = l->tok.bytes;
	n->len = l->tok.len;
	l->tok.bytes = NULL;
	Next(l);
	return n;
}

static Node_t* ParseArgs(Parser_t* p, Node_t* call)
{
	Lex_t* l = &p->lex;
	Expect(l, "(");
	if(!IsPunct(l, ")"))
		for(;;)
		{
			AddItem(call, ParseAssignment(p));
			if(!Accept(l, ","))
				break;
		}
	Expect(l, ")");
	return call;
}

/* an instruction by its listing name, or `op.0xNN` for a base opcode
 * without a name (the listing writes those bare, which is no identifier) */
static int LookupInstruction(const char* name, EngineGen_t gen, int* fam, int* op)
{
	if(strncmp(name, "op.0x", 5) == 0)
	{
		char* end;
		long v = strtol(name + 5, &end, 16);
		if(*end || v < 0 || v >= 0x80)
			return 0;
		*fam = 0;
		*op = (int)v;
		return 1;
	}
	return Asm_LookupName(name, strlen(name), gen, fam, op);
}

static Node_t* ParsePrimary(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* n;
	int line = l->tok.line;
	if(l->tok.kind == T_NUM)
	{
		n = NewNode(N_NUM, line);
		n->num = l->tok.num;
		n->size = l->tok.wide ? 4 : 0; // a wide literal
		Next(l);
		return n;
	}
	if(l->tok.kind == T_STR)
		return StrNode(p);
	if(Accept(l, "("))
	{
		int elem;
		// a cast: (int*) / (char*) / (short*) followed by an operand - only in the *(T*)(e) form, handled in unary
		if(TypeKeyword(l, &elem) && Peek(l)->kind == T_PUNCT && strcmp(Peek(l)->text, "*") == 0)
		{
			Errorf(l, line, "a cast is written as *(type*)(address)");
			Next(l);
			Next(l);
			Expect(l, ")");
			return ParsePrimary(p);
		}
		n = NewNode(N_PAREN, line);
		n->a = ParseExpression(p);
		Expect(l, ")");
		return n;
	}
	if(l->tok.kind == T_IDENT)
	{
		char name[128];
		int fam, op;
		strcpy(name, l->tok.text);
		Next(l);
		if(strcmp(name, "sizeof") == 0 && IsPunct(l, "(") && !FindVar(p->func, name) && !FindFunc(p->unit, name))
		{ // sizeof(type) / sizeof(variable): the number of bytes, a plain number in the code
			TypeRef_t t;
			Var_t* v;
			AreaVar_t* a;
			int elem;
			n = NewNode(N_NUM, line);
			Next(l);
			if(l->tok.kind == T_IDENT && (TypeKeyword(l, &elem) || IsIdent(l, "func") || FindRecord(p->unit, l->tok.text)))
			{
				ParseType(p, &t);
				n->num = TypeSize(&t);
			}
			else if(l->tok.kind == T_IDENT && (v = FindVar(p->func, l->tok.text)) != NULL)
			{
				n->num = v->size;
				Next(l);
			}
			else if(l->tok.kind == T_IDENT && (a = FindArea(p->unit, l->tok.text)) != NULL)
			{
				n->num = (int64_t)TypeSize(&a->type) * a->count * (a->inner ? a->inner : 1);
				Next(l);
			}
			else
			{
				Errorf(l, line, "sizeof takes a type or the name of a variable");
				if(l->tok.kind != T_EOF && !IsPunct(l, ")"))
					Next(l);
			}
			Expect(l, ")");
			return n;
		}
		if(IsPunct(l, "("))
		{
			if(!FindVar(p->func, name) && !FindArea(p->unit, name) && !FindFunc(p->unit, name) && LookupInstruction(name, p->unit->gen, &fam, &op) && !(fam == 0 && (op >= 0x80 || BpcOp_ByOpcode(op))))
			{ // an instruction (the operators' own opcodes, `add`, `sub`, .. are not callable: they are expressions)
				n = NewNode(N_OPCALL, line);
				n->op = (fam << 8) | op;
				strcpy(n->text, name);
				return ParseArgs(p, n);
			}
			n = NewNode(N_CALL, line);
			n->a = NewNode(N_NAME, line);
			strcpy(n->a->text, name);
			return ParseArgs(p, n);
		}
		n = NewNode(N_NAME, line);
		strcpy(n->text, name);
		return n;
	}
	Errorf(l, line, "an expression was expected, found '%s'", l->tok.kind == T_EOF ? "end of file" : l->tok.text);
	if(l->tok.kind != T_EOF)
		Next(l);
	return NewNode(N_NUM, line);
}

static Node_t* ParsePostfix(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* n = ParsePrimary(p);
	for(;;)
	{
		int line = l->tok.line;
		if(IsPunct(l, "("))
		{ // a call through a value: (expr)(args), table[i](args)
			Node_t* call = NewNode(N_CALL, line);
			call->a = n;
			n = ParseArgs(p, call);
		}
		else if(Accept(l, "["))
		{ // a[index]: an element
			Node_t* e = NewNode(N_INDEX, line);
			e->a = n;
			e->b = ParseExpression(p);
			Expect(l, "]");
			n = e;
		}
		else if(IsPunct(l, ".") || IsPunct(l, "->"))
		{ // a.field, p->field
			Node_t* m = NewNode(N_MEMBER, line);
			m->op = IsPunct(l, "->");
			m->a = n;
			Next(l);
			if(l->tok.kind != T_IDENT)
				Errorf(l, line, "the name of a field was expected");
			else
			{
				snprintf(m->text, sizeof m->text, "%s", l->tok.text);
				Next(l);
			}
			n = m;
		}
		else
			return n;
	}
}

static Node_t* ParseUnary(Parser_t* p)
{
	Lex_t* l = &p->lex;
	int line = l->tok.line;
	Node_t* n;
	if(IsPunct(l, "-") || IsPunct(l, "~") || IsPunct(l, "!"))
	{
		char op = l->tok.text[0];
		Next(l);
		n = NewNode(N_UNARY, line);
		n->op = op;
		n->a = ParseUnary(p);
		return n;
	}
	if(IsPunct(l, "&"))
	{
		Next(l);
		if(l->tok.kind != T_IDENT)
		{
			Errorf(l, line, "& takes a variable name");
			return ParseUnary(p);
		}
		n = NewNode(N_ADDR, line); // &name, &name[index], &rec.field ..
		n->a = ParsePostfix(p);
		return n;
	}
	if(IsPunct(l, "*"))
	{
		int elem = 4, cast = 0;
		Next(l);
		// *(type*)(address) or *(type*)number
		if(Accept(l, "("))
		{
			if(!TypeKeyword(l, &elem))
			{
				Errorf(l, line, "*(type*) expected");
				return ParseUnary(p);
			}
			Next(l);
			Expect(l, "*");
			Expect(l, ")");
			cast = 1;
		}
		n = NewNode(N_DEREF, line);
		n->op = cast; // 0: a bare *p, which takes the size from p when p is declared a pointer
		n->size = elem == 1 ? 0 : elem == 2 ? 1
											: 2;
		n->a = ParseUnary(p);
		return n;
	}
	return ParsePostfix(p);
}

// the binary operators by precedence, lowest first; '?:' and assignments sit below them
static Node_t* ParseBinary(Parser_t* p, int minPrec)
{
	Lex_t* l = &p->lex;
	Node_t* left = ParseUnary(p);
	for(;;)
	{
		const BpcOperator_t* op = l->tok.kind == T_PUNCT ? BpcOp_ByText(l->tok.text, strlen(l->tok.text)) : NULL;
		Node_t* n;
		if(!op || op->prec < minPrec)
			return left;
		Next(l);
		n = NewNode(N_BINARY, left->line);
		n->op = op->op;
		n->a = left;
		n->b = ParseBinary(p, op->prec + 1);
		left = n;
	}
}

static Node_t* ParseSelect(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* c = ParseBinary(p, 0);
	if(Accept(l, "?"))
	{
		Node_t* n = NewNode(N_SELECT, c->line);
		n->a = c;
		n->b = ParseAssignment(p);
		Expect(l, ":");
		n->c = ParseSelect(p);
		return n;
	}
	return c;
}

static Node_t* ParseAssignment(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* left = ParseSelect(p);
	if(IsPunct(l, "=") || IsPunct(l, "<-"))
	{
		int store = IsPunct(l, "<-");
		Node_t* n;
		Next(l);
		n = NewNode(store ? N_ASSIGN2 : N_ASSIGN, left->line);
		n->a = left;
		if(IsPunct(l, "{"))
		{ // an initializer list: a store_multi statement
			Node_t* list = NewNode(N_INITLIST, l->tok.line);
			Next(l);
			if(!IsPunct(l, "}"))
				for(;;)
				{
					AddItem(list, ParseAssignment(p));
					if(!Accept(l, ","))
						break;
				}
			Expect(l, "}");
			n->b = list;
			return n;
		}
		n->b = ParseAssignment(p);
		return n;
	}
	return left;
}

static Node_t* ParseExpression(Parser_t* p)
{
	return ParseAssignment(p);
}

// ---- statements -------------------------------------------------------------------------------------

static Node_t* ParseStatement(Parser_t* p);
static int AreaDeclStart(Lex_t* l);
static void SkipStatement(Lex_t* l);

static Node_t* ParseBlock(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* n = NewNode(N_BLOCK, l->tok.line);
	Expect(l, "{");
	while(!IsPunct(l, "}") && l->tok.kind != T_EOF)
	{
		Node_t* s = ParseStatement(p);
		if(s)
			AddItem(n, s);
	}
	Expect(l, "}");
	return n;
}

// `{ lines }` after __asm: the text between the braces, verbatim (braces inside strings are allowed)
static Node_t* ParseAsmBlock(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* n = NewNode(N_ASM, l->tok.line);
	const char* s;
	const char* q;
	int depth = 1, inStr = 0;
	if(!IsPunct(l, "{"))
	{
		Errorf(l, l->tok.line, "'{' expected after __asm");
		return n;
	}
	// the lexer sits after the brace token; take the raw text from there
	s = l->peeked ? l->peek.text : NULL; // (no peek expected here)
	(void)s;
	if(l->peeked)
	{
		Errorf(l, l->tok.line, "internal: lookahead before __asm");
		return n;
	}
	s = l->p;
	for(q = s; q < l->end; q++)
	{
		if(inStr)
		{
			if(*q == '\\' && q + 1 < l->end)
				q++;
			else if(*q == '"')
				inStr = 0;
			continue;
		}
		if(*q == '"')
			inStr = 1;
		else if(*q == '{')
			depth++;
		else if(*q == '}')
		{
			if(--depth == 0)
				break;
		}
		else if(*q == '\n')
			l->line++;
	}
	n->len = (uint32_t)(q - s);
	n->bytes = (uint8_t*)malloc(n->len + 1);
	memcpy(n->bytes, s, n->len);
	n->bytes[n->len] = 0;
	l->p = q < l->end ? q + 1 : q;
	Next(l);
	return n;
}

// a local declaration, the lexer at its type: `type name;`, `type name[count];`, `type name = value;`
static Node_t* ParseDeclaration(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* n = NewNode(N_DECL, l->tok.line);
	TypeRef_t type;
	uint32_t elem, inner = 0;
	ParseType(p, &type);
	elem = TypeSize(&type);
	if(l->tok.kind != T_IDENT)
	{
		Errorf(l, n->line, "a variable name was expected");
		return n;
	}
	strcpy(n->text, l->tok.text);
	n->size = (int)elem;
	n->op = 1;
	Next(l);
	if(Accept(l, "["))
	{
		uint32_t count = 1;
		ParseDims(p, n->text, &count, &inner);
		n->op = (int)count;
		n->num = 1; // an array
	}
	if(Accept(l, "="))
	{
		if(n->num == 1 || (type.rec && !type.ptr))
			Errorf(l, n->line, "%s: only a single number or pointer can be given a value where it is declared", n->text);
		n->a = ParseAssignment(p);
	}
	Expect(l, ";");
	if(p->func)
	{
		if(FindVar(p->func, n->text))
			Errorf(l, n->line, "%s is declared twice", n->text);
		else
		{
			Var_t* v = AddVar(p->func, n->text, &type, elem * (uint32_t)n->op * (inner ? inner : 1), n->num == 1);
			v->count = (uint32_t)n->op;
			v->inner = inner;
		}
	}
	return n;
}

static Node_t* ParseStatement(Parser_t* p)
{
	Lex_t* l = &p->lex;
	int line = l->tok.line;
	Node_t* n;
	if(IsPunct(l, "{"))
		return ParseBlock(p);
	if(Accept(l, ";"))
		return NULL;
	if(AtType(p) && !FindVar(p->func, l->tok.text))
		return ParseDeclaration(p);
	if(AreaDeclStart(l) >= 0 && !FindVar(p->func, l->tok.text) && !FindArea(p->unit, l->tok.text))
	{ // (a variable that bears one of the words is just that: `dynamic[i] = 0;`)
		Errorf(l, line, "a %s variable is declared outside the functions", l->tok.text);
		SkipStatement(l);
		return NULL;
	}
	if(l->tok.kind == T_IDENT)
	{
		if(Peek(l)->kind == T_PUNCT && strcmp(Peek(l)->text, ":") == 0 && !IsIdent(l, "default"))
		{
			n = NewNode(N_LABEL, line);
			strcpy(n->text, l->tok.text);
			Next(l);
			Next(l);
			return n;
		}
		if(IsIdent(l, "if"))
		{
			Next(l);
			n = NewNode(N_IF, line);
			Expect(l, "(");
			n->a = ParseExpression(p);
			Expect(l, ")");
			n->num = !IsPunct(l, "{");
			n->b = ParseStatement(p);
			if(!n->b)
				n->b = NewNode(N_BLOCK, line);
			if(IsIdent(l, "else"))
			{
				Next(l);
				n->c = ParseStatement(p);
				if(!n->c)
					n->c = NewNode(N_BLOCK, line);
			}
			return n;
		}
		if(IsIdent(l, "while"))
		{
			Next(l);
			n = NewNode(N_WHILE, line);
			Expect(l, "(");
			n->a = ParseExpression(p);
			Expect(l, ")");
			n->b = ParseStatement(p);
			if(!n->b)
				n->b = NewNode(N_BLOCK, line);
			return n;
		}
		if(IsIdent(l, "do"))
		{
			Next(l);
			n = NewNode(N_DOWHILE, line);
			n->b = ParseStatement(p);
			if(!n->b)
				n->b = NewNode(N_BLOCK, line);
			if(!IsIdent(l, "while"))
				Errorf(l, l->tok.line, "'while' expected after the do body");
			else
				Next(l);
			Expect(l, "(");
			n->a = ParseExpression(p);
			Expect(l, ")");
			Expect(l, ";");
			return n;
		}
		if(IsIdent(l, "for"))
		{
			Next(l);
			n = NewNode(N_FOR, line);
			Expect(l, "(");
			if(!IsPunct(l, ";"))
				n->a = ParseExpression(p);
			Expect(l, ";");
			if(!IsPunct(l, ";"))
				n->b = ParseExpression(p);
			Expect(l, ";");
			if(!IsPunct(l, ")"))
				n->c = ParseExpression(p);
			Expect(l, ")");
			{
				Node_t* body = ParseStatement(p);
				AddItem(n, body ? body : NewNode(N_BLOCK, line));
			}
			return n;
		}
		if(IsIdent(l, "break"))
		{
			Next(l);
			Expect(l, ";");
			return NewNode(N_BREAK, line);
		}
		if(IsIdent(l, "continue"))
		{
			Next(l);
			Expect(l, ";");
			return NewNode(N_CONTINUE, line);
		}
		if(IsIdent(l, "return"))
		{
			Next(l);
			n = NewNode(N_RETURN, line);
			if(!IsPunct(l, ";"))
				n->a = ParseExpression(p);
			Expect(l, ";");
			return n;
		}
		if(IsIdent(l, "goto"))
		{
			Next(l);
			n = NewNode(N_GOTO, line);
			strcpy(n->text, l->tok.text);
			Next(l);
			Expect(l, ";");
			return n;
		}
		if(IsIdent(l, "__asm"))
		{
			Next(l);
			return ParseAsmBlock(p);
		}
		if(IsIdent(l, "__inline"))
		{
			Next(l);
			n = NewNode(N_INLINE, line);
			Expect(l, "(");
			n->a = ParseAssignment(p);
			Expect(l, ",");
			if(l->tok.kind != T_STR)
				Errorf(l, line, "__inline takes a string literal");
			else
			{
				n->bytes = l->tok.bytes;
				n->len = l->tok.len;
				l->tok.bytes = NULL;
				Next(l);
			}
			Expect(l, ")");
			Expect(l, ";");
			return n;
		}
	}
	n = NewNode(N_EXPR, line);
	n->a = ParseExpression(p);
	Expect(l, ";");
	return n;
}

// ---- constant expressions and the variables of the memory areas -----------------------------------

/* The value of a constant expression: numbers with the arithmetic and bit
 * operators.  For the address of `var` (NULL for a plain number such as an
 * array size) the arrays declared before it in the same area may be named
 * - and `&name` of a variable there - and stand for their own addresses.
 * `leftmost` says that nothing stands to the left of the node: outside
 * the global memory the first number of an address is the one that is
 * pushed with the area's tag, so only a number or a name may stand there
 * and a name nowhere else.  Returns 0, with a message, for anything else. */
static int EvalConst(Parser_t* p, const Node_t* n, const struct AreaVar* var, int leftmost, uint32_t* out)
{
	Lex_t* l = &p->lex;
	uint32_t a = 0, b = 0;
	int tagged = var && var->area != AREA_GLOBAL;
	switch(n->kind)
	{
		case N_NUM:
			*out = (uint32_t)n->num;
			return 1;
		case N_PAREN: return EvalConst(p, n->a, var, leftmost, out);
		case N_UNARY:
			if(n->op == '!')
				break;
			if(tagged && leftmost)
			{
				Errorf(l, n->line, "the address of a %s variable starts with a number or with the name of a variable of that area", kAreaNames[var->area]);
				return 0;
			}
			if(!EvalConst(p, n->a, var, 0, &a))
				return 0;
			*out = n->op == '-' ? 0u - a : ~a;
			return 1;
		case N_BINARY:
			if(!EvalConst(p, n->a, var, leftmost, &a) || !EvalConst(p, n->b, var, 0, &b))
				return 0;
			switch(n->op)
			{
				case 0x20: *out = a + b; return 1;
				case 0x21: *out = a - b; return 1;
				case 0x22: *out = a * b; return 1;
				case 0x23:
				case 0x24:
					if(b == 0 || ((int32_t)a == INT32_MIN && (int32_t)b == -1))
					{
						Errorf(l, n->line, "division by zero");
						return 0;
					}
					*out = n->op == 0x23 ? (uint32_t)((int32_t)a / (int32_t)b) : (uint32_t)((int32_t)a % (int32_t)b);
					return 1;
				case 0x25: *out = a & b; return 1;
				case 0x26: *out = a | b; return 1;
				case 0x27: *out = a ^ b; return 1;
				case 0x29: *out = a << (b & 31); return 1;
				case 0x2a: *out = a >> (b & 31); return 1;
				case 0x2b: *out = (uint32_t)((int32_t)a < 0 ? ~(~a >> (b & 31)) : a >> (b & 31)); return 1;
				default: break;
			}
			break;
		case N_NAME:
		case N_ADDR:
		{
			const AreaVar_t* v;
			const Node_t* nm = n->kind == N_ADDR ? n->a : n; // &name: the name
			if(!var || !nm || nm->kind != N_NAME)
				break;
			v = FindArea(p->unit, nm->text);
			if(!v)
			{
				Errorf(l, n->line, "%s is not a variable of a memory area declared before this one", nm->text);
				return 0;
			}
			if(n->kind == N_NAME && !v->isArray && !(v->type.rec && !v->type.ptr))
			{
				Errorf(l, n->line, "%s is a variable, not an array: its address is &%s", nm->text, nm->text);
				return 0;
			}
			if(v->area != var->area || v->block != var->block)
			{
				Errorf(l, n->line, "%s lies in another memory area", nm->text);
				return 0;
			}
			if(tagged && !leftmost)
			{
				Errorf(l, n->line, "in the address of a %s variable a name can only come first", kAreaNames[var->area]);
				return 0;
			}
			*out = v->value;
			return 1;
		}
		default: break;
	}
	Errorf(l, n->line, var ? "an address is made of numbers, operators and the names of variables of the same area" : "a constant was expected");
	return 0;
}

// past the next ';' (after an error in a declaration)
static void SkipStatement(Lex_t* l)
{
	while(l->tok.kind != T_EOF && !IsPunct(l, ";"))
	{
		free(l->tok.bytes);
		l->tok.bytes = NULL;
		Next(l);
	}
	Accept(l, ";");
}

static int IsReserved(const char* name)
{
	static const char* const words[] = {"int", "char", "short", "if", "else", "while", "do", "for", "break", "continue", "return", "goto",
		"struct", "sizeof", "__asm", "__inline", "__program", "__pop", "__sign", NULL};
	int i;
	for(i = 0; words[i]; i++)
		if(strcmp(words[i], name) == 0)
			return 1;
	return 0;
}

/* whether the lexer is at the start of a declaration in a memory area -
 * `global`, `code`, `data` or `heap` followed by a word (the type),
 * `dynamic` followed by its `[block]` - and which area (-1: it is not;
 * those words are ordinary names anywhere else, and a function may bear
 * one) */
static int AreaDeclStart(Lex_t* l)
{
	int area;
	const Token_t* next;
	if(l->tok.kind != T_IDENT)
		return -1;
	for(area = 0; area < AREA_COUNT; area++)
		if(strcmp(l->tok.text, kAreaNames[area]) == 0)
			break;
	if(area == AREA_COUNT)
		return -1;
	next = Peek(l);
	if(area == AREA_DYNAMIC)
		return next->kind == T_PUNCT && strcmp(next->text, "[") == 0 ? area : -1;
	return next->kind == T_IDENT ? area : -1; // a type, or a word that should have been one
}

/* AREA type name [ '[' count ']' ] '@' address ';' - a variable at a fixed
 * address of a memory area.  Nothing is reserved and nothing is compiled
 * for it; the declaration only gives the address a name, a type (a
 * scalar, a record, a pointer or a func) and, for an array, a length. */
static void ParseAreaDecl(Parser_t* p, int area)
{
	Lex_t* l = &p->lex;
	Unit_t* u = p->unit;
	AreaVar_t v;
	Node_t* e;
	int line = l->tok.line;
	memset(&v, 0, sizeof v);
	v.area = area;
	v.line = line;
	v.count = 1;
	Next(l); // the area
	if(area == AREA_DYNAMIC)
	{ // dynamic[block]
		Expect(l, "[");
		e = ParseBinary(p, 0);
		if(!EvalConst(p, e, NULL, 1, &v.block))
			v.broken = 1;
		FreeNode(e);
		Expect(l, "]");
	}
	if(!AtType(p))
	{
		Errorf(l, line, "a type (int, short, char, func or a record) was expected after %s", kAreaNames[area]);
		SkipStatement(l);
		return;
	}
	ParseType(p, &v.type);
	if(l->tok.kind != T_IDENT || IsReserved(l->tok.text))
	{
		Errorf(l, line, "a variable name was expected");
		SkipStatement(l);
		return;
	}
	snprintf(v.name, sizeof v.name, "%s", l->tok.text);
	Next(l);
	if(Accept(l, "["))
	{
		ParseDims(p, v.name, &v.count, &v.inner);
		v.isArray = 1;
	}
	if(IsPunct(l, "="))
	{
		Errorf(l, line, "%s: a %s variable cannot be given a value where it is declared (the engine does not fill the script memory from a program); assign it in a function", v.name, kAreaNames[area]);
		SkipStatement(l);
		return;
	}
	if(!Accept(l, "@"))
	{
		Errorf(l, line, "%s: '@ address' was expected: a %s variable is declared at the address it has", v.name, kAreaNames[area]);
		SkipStatement(l);
		return;
	}
	v.addr = ParseBinary(p, 0);
	if(IsPunct(l, "="))
	{
		Errorf(l, line, "%s: a %s variable cannot be given a value where it is declared (the engine does not fill the script memory from a program); assign it in a function", v.name, kAreaNames[area]);
		SkipStatement(l);
		FreeNode(v.addr);
		return;
	}
	Expect(l, ";");
	if(!EvalConst(p, v.addr, &v, 1, &v.value))
	{
		FreeNode(v.addr);
		return;
	}
	if(FindArea(u, v.name))
		Errorf(l, line, "%s is declared twice", v.name);
	else if(FindFunc(u, v.name))
		Errorf(l, line, "%s is the name of a function", v.name);
	else
	{
		if(u->areaCount == u->areaCap)
		{
			u->areaCap = u->areaCap ? u->areaCap * 2 : 64;
			u->areaVars = (AreaVar_t*)realloc(u->areaVars, (size_t)u->areaCap * sizeof *u->areaVars);
		}
		u->areaVars[u->areaCount++] = v;
		return;
	}
	FreeNode(v.addr);
}

/* `struct Name { type field; type field[count]; type field @ offset; .. };`
 * - a record: the layout of a piece of memory.  The fields lie one after
 * the other without gaps, in the order written; `@ offset` puts a field
 * at a given offset instead (and the following ones behind it), which is
 * how a layout with unknown or unused parts is written.  A field is a
 * scalar, a record declared before, a pointer (to this record too) or a
 * func, or an array of those.  Like a variable at a fixed address, a
 * record compiles to nothing by itself. */
static void ParseRecord(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Unit_t* u = p->unit;
	Record_t* r = (Record_t*)calloc(1, sizeof *r);
	uint32_t next = 0;
	int line = l->tok.line;
	Next(l); // struct
	snprintf(r->name, sizeof r->name, "%s", l->tok.text);
	Next(l);
	if(IsReserved(r->name) || FindRecord(u, r->name) || FindFunc(u, r->name) || FindArea(u, r->name))
		Errorf(l, line, "%s: the name of the record is taken", r->name);
	if(u->recordCount == u->recordCap)
	{
		u->recordCap = u->recordCap ? u->recordCap * 2 : 16;
		u->records = (Record_t**)realloc(u->records, (size_t)u->recordCap * sizeof *u->records);
	}
	u->records[u->recordCount++] = r; // known from here on, so that a field can point to its own record
	Expect(l, "{");
	while(!IsPunct(l, "}") && l->tok.kind != T_EOF)
	{
		Field_t f;
		Node_t* e;
		int fline = l->tok.line;
		memset(&f, 0, sizeof f);
		f.count = 1;
		if(!AtType(p))
		{
			Errorf(l, fline, "%s: a field starts with its type (int, short, char, func or a record)", r->name);
			SkipStatement(l);
			continue;
		}
		ParseType(p, &f.type);
		if(f.type.rec == r && !f.type.ptr)
			Errorf(l, fline, "%s: a record cannot contain itself (a pointer to it can: %s*)", r->name, r->name);
		if(l->tok.kind != T_IDENT || IsReserved(l->tok.text))
		{
			Errorf(l, fline, "%s: the name of a field was expected", r->name);
			SkipStatement(l);
			continue;
		}
		snprintf(f.name, sizeof f.name, "%s", l->tok.text);
		Next(l);
		if(Accept(l, "["))
		{
			ParseDims(p, f.name, &f.count, &f.inner);
			f.isArray = 1;
		}
		f.off = next;
		if(Accept(l, "@"))
		{
			e = ParseBinary(p, 0);
			EvalConst(p, e, NULL, 1, &f.off);
			FreeNode(e);
		}
		Expect(l, ";");
		if(FindField(r, f.name))
		{
			Errorf(l, fline, "%s.%s is declared twice", r->name, f.name);
			continue;
		}
		if(r->fieldCount == r->fieldCap)
		{
			r->fieldCap = r->fieldCap ? r->fieldCap * 2 : 16;
			r->fields = (Field_t*)realloc(r->fields, (size_t)r->fieldCap * sizeof *r->fields);
		}
		r->fields[r->fieldCount++] = f;
		next = f.off + TypeSize(&f.type) * f.count * (f.inner ? f.inner : 1);
		if(next > r->size)
			r->size = next;
	}
	Expect(l, "}");
	Accept(l, ";");
	if(r->size == 0)
		Errorf(l, line, "%s: a record has at least one field", r->name);
}

// ---- the unit ----------------------------------------------------------------------------------------

static void ParseDirective(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Unit_t* u = p->unit;
	char name[128], arg[64];
	int line = l->tok.line;
	strcpy(name, l->tok.text);
	strcpy(arg, l->tok.arg);
	Next(l);
	if(strcmp(name, "engine") == 0)
	{
		EngineGen_t g = GEN_COUNT;
		if(strcmp(arg, "any") != 0)
		{
			int i;
			for(i = 0; i < GEN_COUNT; i++)
				if(strcmp(kGenNames[i], arg) == 0)
					g = (EngineGen_t)i;
			if(g == GEN_COUNT)
				Errorf(l, line, "unknown engine %s", arg);
		}
		u->gen = g;
		u->genSet = 1;
		return;
	}
	if(strcmp(name, "encoding") == 0)
		return; // the lexer has switched the encoding of the literals
	Errorf(l, line, "unknown directive #%s", name);
}

static void ParseUnit(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Unit_t* u = p->unit;
	Next(l);
	while(l->tok.kind != T_EOF)
	{
		int line = l->tok.line;
		if(l->tok.kind == T_DIRECTIVE)
		{
			ParseDirective(p);
			continue;
		}
		if(IsIdent(l, "__program"))
		{
			Node_t* a;
			Next(l);
			a = ParseAsmBlock(p);
			u->program = (char*)a->bytes;
			a->bytes = NULL;
			FreeNode(a);
			continue;
		}
		{
			int area = AreaDeclStart(l);
			if(area >= 0)
			{
				ParseAreaDecl(p, area);
				continue;
			}
		}
		if(IsIdent(l, "struct") && Peek(l)->kind == T_IDENT)
		{
			ParseRecord(p);
			continue;
		}
		if(l->tok.kind == T_IDENT)
		{
			Func_t* f = (Func_t*)calloc(1, sizeof *f);
			snprintf(f->name, sizeof f->name, "%s", l->tok.text);
			Next(l);
			if(FindFunc(u, f->name))
				Errorf(l, line, "function %s is defined twice", f->name);
			if(FindArea(u, f->name))
				Errorf(l, line, "%s is the name of a %s variable", f->name, kAreaNames[FindArea(u, f->name)->area]);
			if(FindRecord(u, f->name))
				Errorf(l, line, "%s is the name of a record", f->name);
			if(u->funcCount == u->funcCap)
			{
				u->funcCap = u->funcCap ? u->funcCap * 2 : 16;
				u->funcs = (Func_t**)realloc(u->funcs, (size_t)u->funcCap * sizeof *u->funcs);
			}
			u->funcs[u->funcCount++] = f;
			Expect(l, "(");
			if(!IsPunct(l, ")"))
				for(;;)
				{
					// a parameter is a word; it may be given a type that says what the word is (`Panel* panel`, `func handler`)
					TypeRef_t type;
					memset(&type, 0, sizeof type);
					type.elem = 4;
					if(AtType(p))
					{
						ParseType(p, &type);
						if(TypeSize(&type) != 4 || (type.rec && !type.ptr))
							Errorf(l, l->tok.line, "a parameter is one word: an int, a pointer or a func");
					}
					if(l->tok.kind != T_IDENT)
					{
						Errorf(l, l->tok.line, "a parameter name was expected");
						break;
					}
					AddVar(f, l->tok.text, &type, 4, 0);
					f->params++;
					Next(l);
					if(!Accept(l, ","))
						break;
				}
			Expect(l, ")");
			{ // the parameters lie in the frame in reverse order: the last one at its lowest address
				int k;
				for(k = 0; k < f->params; k++)
					f->vars[k].off = (uint32_t)(f->params - 1 - k) * 4;
			}
			p->func = f;
			f->body = ParseBlock(p);
			p->func = NULL;
			if(f->body->count == 1 && f->body->items[0]->kind == N_ASM)
			{
				f->isAsm = 1;
				f->asmNode = f->body->items[0];
			}
			continue;
		}
		Errorf(l, line, "a function definition was expected, found '%s'", l->tok.text);
		Next(l);
	}
}

static void FreeUnit(Unit_t* u)
{
	int i;
	for(i = 0; i < u->funcCount; i++)
	{
		FreeNode(u->funcs[i]->body);
		free(u->funcs[i]->vars);
		free(u->funcs[i]);
	}
	free(u->funcs);
	for(i = 0; i < u->areaCount; i++)
		FreeNode(u->areaVars[i].addr);
	free(u->areaVars);
	for(i = 0; i < u->recordCount; i++)
	{
		free(u->records[i]->fields);
		free(u->records[i]);
	}
	free(u->records);
	free(u->program);
}

// ---- the generator ------------------------------------------------------------------------------------

typedef struct Gen
{
	Unit_t* u;
	Lex_t* lex;
	Func_t* func;
	FILE* out; // the listing
	int labels;
	int errors;
	// the string table: appended in order of appearance
	int strings;
	struct
	{
		uint8_t* bytes;
		uint32_t len;
	}* strTab;
	int strCap;
	// the loop contexts
	int breakLabel[64], continueLabel[64];
	int depth;
	int exitLabel;
	// named labels of the current function: name -> label id
	struct
	{
		char name[128];
		int label;
	} named[512];
	int namedCount;
} Gen_t;

static void GenError(Gen_t* g, int line, const char* fmt, ...)
{
	va_list ap;
	g->errors++;
	if(g->errors > 50)
		return;
	va_start(ap, fmt);
	Message(g->lex, line, "error", fmt, ap);
	va_end(ap);
}

static void Emit(Gen_t* g, const char* fmt, ...)
{
	va_list ap;
	fputc('\t', g->out);
	va_start(ap, fmt);
	vfprintf(g->out, fmt, ap);
	va_end(ap);
	fputc('\n', g->out);
}

static int NewLabel(Gen_t* g)
{
	return g->labels++;
}

static void PlaceLabel(Gen_t* g, int label)
{
	fprintf(g->out, "L%d:\n", label);
}

// an unconditional jump to a label
static void EmitJump(Gen_t* g, int label)
{
	Emit(g, "push_code_off L%d", label);
	Emit(g, "jmp");
}

static int InternString(Gen_t* g, const uint8_t* bytes, uint32_t len)
{
	if(g->strings == g->strCap)
	{
		g->strCap = g->strCap ? g->strCap * 2 : 64;
		g->strTab = realloc(g->strTab, (size_t)g->strCap * sizeof *g->strTab);
	}
	g->strTab[g->strings].bytes = (uint8_t*)malloc(len + 1);
	memcpy(g->strTab[g->strings].bytes, bytes, len);
	g->strTab[g->strings].bytes[len] = 0;
	g->strTab[g->strings].len = len;
	return g->strings++;
}

static void EmitPush(Gen_t* g, uint32_t v)
{
	if(v <= 127)
		Emit(g, "push_i8 %u", v);
	else if(v <= 32767)
		Emit(g, "push_i16 %u", v);
	else
		Emit(g, "push_i32 0x%x", v);
}

static int NamedLabel(Gen_t* g, const char* name, int line)
{
	int i;
	for(i = 0; i < g->namedCount; i++)
		if(strcmp(g->named[i].name, name) == 0)
			return g->named[i].label;
	if(g->namedCount >= (int)(sizeof g->named / sizeof g->named[0]))
	{
		GenError(g, line, "too many labels");
		return 0;
	}
	snprintf(g->named[g->namedCount].name, sizeof g->named[0].name, "%s", name);
	g->named[g->namedCount].label = NewLabel(g);
	return g->named[g->namedCount++].label;
}

static void GenExpr(Gen_t* g, Node_t* n);
static void GenCond(Gen_t* g, Node_t* n, int trueLabel, int falseLabel, int trueFalls);

static const char* OpMnemonic(Gen_t* g, int op)
{
	const char* name = Asm_OpName(0, op, g->u->gen);
	static char buf[16];
	if(name)
		return name;
	snprintf(buf, sizeof buf, "0x%02x", op);
	return buf;
}

// a variable reference: its frame address (push_local_addr F - off)
static int VarAddress(Gen_t* g, const char* name, int line, Var_t** out)
{
	Var_t* v = FindVar(g->func, name);
	if(!v)
	{
		GenError(g, line, "unknown variable %s", name);
		return 0;
	}
	Emit(g, "push_local_addr 0x%x", (unsigned)(g->func->frame - v->off));
	*out = v;
	return 1;
}

/* The tag of a variable's memory area as the value its address is or-ed
 * with (0 in the global memory); the layout is the engine's, so the areas
 * other than the global memory need one named.  0 with `v->broken` set,
 * and a message the first time, when the address cannot be made. */
static uint32_t AreaBase(Gen_t* g, AreaVar_t* v, int line)
{
	const EngineProfile_t* e;
	uint32_t tag;
	if(v->area == AREA_GLOBAL || v->broken)
		return 0;
	v->broken = 1;
	if(g->u->gen == GEN_COUNT)
	{
		GenError(g, line, "%s: a %s variable needs the engine named (#engine NAME): the pointers into that area differ between the builds", v->name, kAreaNames[v->area]);
		return 0;
	}
	e = Engine_ProfileOfGen(g->u->gen);
	switch(v->area)
	{
		case AREA_CODE: tag = e->tagCode; break;
		case AREA_DATA: tag = e->tagData; break;
		case AREA_HEAP: tag = e->tagHeap; break;
		default:
			if(e->dynCount == 0)
			{
				GenError(g, line, "%s: the dynamic blocks of engine %s have no fixed numbers (they come from pools by size); a pointer into one has to come from the program", v->name, e->name);
				return 0;
			}
			if(v->block >= e->dynCount)
			{
				GenError(g, line, "%s: engine %s has the dynamic blocks 0 to %u", v->name, e->name, (unsigned)e->dynCount - 1);
				return 0;
			}
			tag = e->dynFirst + v->block;
			break;
	}
	if(v->value >> e->tagShift)
	{
		GenError(g, line, "%s: the offset 0x%x does not fit the %d bits a pointer of engine %s has for it", v->name, (unsigned)v->value, e->tagShift, e->name);
		return 0;
	}
	v->broken = 0;
	return tag << e->tagShift;
}

static void GenAreaAddress(Gen_t* g, AreaVar_t* v, int line);

/* An address as its declaration wrote it - nothing is folded, as nowhere
 * else: `@ 0x1bb84 + 164` is two pushes and an add.  `base` is the area's
 * tag for the first number (0: none); a name is the address of that
 * variable, which brings its own. */
static void GenAddrExpr(Gen_t* g, const Node_t* n, uint32_t base)
{
	switch(n->kind)
	{
		case N_NUM:
			if(base || n->size == 4)
				Emit(g, "push_i32 0x%x", (unsigned)(base | (uint32_t)n->num));
			else
				EmitPush(g, (uint32_t)n->num);
			break;
		case N_PAREN: GenAddrExpr(g, n->a, base); break;
		case N_NAME:
		case N_ADDR:
		{
			AreaVar_t* v = FindArea(g->u, n->kind == N_ADDR ? n->a->text : n->text);
			if(v)
				GenAreaAddress(g, v, n->line);
			break;
		}
		case N_UNARY:
			GenAddrExpr(g, n->a, 0);
			Emit(g, "not");
			if(n->op == '-')
			{
				Emit(g, "push_i8 1");
				Emit(g, "add");
			}
			break;
		case N_BINARY:
			GenAddrExpr(g, n->a, base);
			GenAddrExpr(g, n->b, 0);
			Emit(g, "%s", OpMnemonic(g, n->op));
			break;
		default: break;
	}
}

// the address of a variable of a memory area
static void GenAreaAddress(Gen_t* g, AreaVar_t* v, int line)
{
	uint32_t base = AreaBase(g, v, line);
	if(!v->broken)
		GenAddrExpr(g, v->addr, base);
}

static int SizeCode(int elem)
{
	return elem == 1 ? 0 : elem == 2 ? 1
									 : 2;
}

/* What an lvalue denotes, once its address is on the stack: the type of
 * the thing there and, for a whole array, that it is one (its "value" is
 * then the address itself, like a record's). */
typedef struct Obj
{
	TypeRef_t type;
	int isArray;
	uint32_t count; // the elements of an array
	uint32_t inner; // a two-dimensional array: the elements of one row (0: one dimension)
} Obj_t;

// a record that lies at the address (not a pointer to one): it has no value of its own, its name stands for the address
static int IsRecordObj(const Obj_t* o)
{
	return o->type.rec && !IsPointerType(&o->type);
}

// the name an lvalue starts with, for the messages
static const char* ObjName(const Node_t* n)
{
	while(n && n->kind != N_NAME && n->a)
		n = n->a;
	return n && n->kind == N_NAME ? n->text : "the expression";
}

/* What the lvalue n denotes, without generating anything: GenObject's
 * rules for the type alone.  0 when n is no lvalue the rules know (the
 * caller then does what it did before records existed). */
static int ObjType(Gen_t* g, const Node_t* n, Obj_t* out)
{
	Obj_t base;
	memset(out, 0, sizeof *out);
	out->type.elem = 4;
	switch(n->kind)
	{
		case N_NAME:
		{
			Var_t* v = FindVar(g->func, n->text);
			AreaVar_t* a = v ? NULL : FindArea(g->u, n->text);
			if(v)
			{
				out->type = v->type;
				out->isArray = v->isArray;
				out->count = v->count;
				out->inner = v->inner;
				return 1;
			}
			if(a)
			{
				out->type = a->type;
				out->isArray = a->isArray;
				out->count = a->count;
				out->inner = a->inner;
				return 1;
			}
			return 0;
		}
		case N_PAREN: return ObjType(g, n->a, out);
		case N_INDEX:
			if(!ObjType(g, n->a, &base))
				return 0;
			out->type = base.type;
			if(base.isArray)
			{
				if(base.inner)
				{
					out->isArray = 1;
					out->count = base.inner;
				}
				return 1;
			}
			if(!base.type.ptr)
				return 0;
			out->type.ptr--;
			return 1;
		case N_MEMBER:
		{
			const Field_t* f;
			if(!ObjType(g, n->a, &base) || base.isArray || !base.type.rec || (n->op ? base.type.ptr != 1 : base.type.ptr != 0))
				return 0;
			f = FindField(base.type.rec, n->text);
			if(!f)
				return 0;
			out->type = f->type;
			out->isArray = f->isArray;
			out->count = f->count;
			out->inner = f->inner;
			return 1;
		}
		default: return 0;
	}
}

/* the size code of a dereference: that of its cast.  Without one, `*p`
 * reads what p was declared to hold - for a pointer (char* p) what it
 * points to, for an array (char name[32], also as a field) its first
 * element - and a dword in every other case, pointers to records and
 * rows of a two-dimensional array among them. */
static int DerefSize(Gen_t* g, const Node_t* n)
{
	Obj_t o;
	if(n->op || !ObjType(g, n->a, &o) || o.type.rec || o.type.func)
		return n->size;
	if(o.isArray ? !o.type.ptr && !o.inner : o.type.ptr == 1)
		return SizeCode(o.type.elem);
	return n->size;
}

/* The address of an lvalue, built the way the original programs build it,
 * with nothing folded:
 *   name      a local's frame address, or the address of a variable of a
 *             memory area as its declaration wrote it
 *   a[i]      the address of a (the value of a, when a is a pointer); i;
 *             then `push S; mul` for elements of S bytes when S > 1 -
 *             but `push 2; shl` when the elements are themselves
 *             pointers or funcs, which is how the original compiler
 *             indexed its tables of addresses; add.  An element of a
 *             two-dimensional array is a row, S the size of a row.
 *   a.f       the address of a; the offset of f; add (also for offset 0)
 *   p->f      the value of p; the offset of f; add
 *   *(T*)(e)  e
 * *out says what lies there.  0, with a message, when n is no lvalue. */
static int GenObject(Gen_t* g, Node_t* n, Obj_t* out)
{
	memset(out, 0, sizeof *out);
	out->type.elem = 4;
	switch(n->kind)
	{
		case N_NAME:
		{
			Var_t* v = FindVar(g->func, n->text);
			AreaVar_t* a;
			if(v)
			{
				Emit(g, "push_local_addr 0x%x", (unsigned)(g->func->frame - v->off));
				out->type = v->type;
				out->isArray = v->isArray;
				out->count = v->count;
				out->inner = v->inner;
				return 1;
			}
			a = FindArea(g->u, n->text);
			if(a)
			{
				GenAreaAddress(g, a, n->line);
				out->type = a->type;
				out->isArray = a->isArray;
				out->count = a->count;
				out->inner = a->inner;
				return 1;
			}
			GenError(g, n->line, "unknown variable %s", n->text);
			return 0;
		}
		case N_PAREN: return GenObject(g, n->a, out);
		case N_DEREF:
			GenExpr(g, n->a);
			out->type.elem = 1 << DerefSize(g, n);
			return 1;
		case N_INDEX:
		{
			Obj_t base;
			uint32_t size;
			if(!GenObject(g, n->a, &base))
				return 0;
			if(base.isArray)
			{
				out->type = base.type;
				if(n->b->kind == N_NUM && (uint64_t)n->b->num >= base.count)
					Warnf(g->lex, n->line, "%s has %u element%s: [%u] lies outside", ObjName(n->a), (unsigned)base.count, base.count == 1 ? "" : "s", (unsigned)n->b->num);
			}
			else if(base.type.ptr)
			{ // p[i]: an element of what the pointer points to
				Emit(g, "load 2");
				out->type = base.type;
				out->type.ptr--;
			}
			else
			{
				GenError(g, n->line, "%s is not an array", ObjName(n->a));
				return 0;
			}
			GenExpr(g, n->b);
			size = TypeSize(&out->type);
			if(base.isArray && base.inner)
			{ // a row of a two-dimensional array: an array itself
				out->isArray = 1;
				out->count = base.inner;
				size *= base.inner;
				EmitPush(g, size);
				Emit(g, "mul");
			}
			else if(IsPointerType(&out->type))
			{
				EmitPush(g, 2);
				Emit(g, "%s", OpMnemonic(g, 0x29));
			}
			else if(size > 1)
			{
				EmitPush(g, size);
				Emit(g, "mul");
			}
			Emit(g, "add");
			return 1;
		}
		case N_MEMBER:
		{
			Obj_t base;
			const Field_t* f;
			if(!GenObject(g, n->a, &base))
				return 0;
			if(n->op)
			{ // p->field
				if(base.isArray || base.type.ptr != 1 || !base.type.rec)
				{
					GenError(g, n->line, "%s is not a pointer to a record: -> cannot be used on it", ObjName(n->a));
					return 0;
				}
				Emit(g, "load 2");
			}
			else if(base.isArray || !IsRecordObj(&base))
			{
				GenError(g, n->line, base.type.rec && base.type.ptr && !base.isArray ? "%s is a pointer to a record: its fields are %s->field" : "%s is not a record", ObjName(n->a), ObjName(n->a));
				return 0;
			}
			f = FindField(base.type.rec, n->text);
			if(!f)
			{
				GenError(g, n->line, "a %s has no field %s", base.type.rec->name, n->text);
				return 0;
			}
			EmitPush(g, f->off);
			Emit(g, "add");
			out->type = f->type;
			out->isArray = f->isArray;
			out->count = f->count;
			out->inner = f->inner;
			return 1;
		}
		default:
			GenError(g, n->line, "not an lvalue");
			return 0;
	}
}

static int IsCallNode(const Node_t* n)
{
	return n->kind == N_CALL || n->kind == N_OPCALL || n->kind == N_SELECT;
}

// the address of something that can be assigned and the size code of the access
static int GenLvalue(Gen_t* g, Node_t* n)
{
	Obj_t o;
	if(!GenObject(g, n, &o))
		return 2;
	if(o.isArray)
		GenError(g, n->line, "%s is an array", ObjName(n));
	else if(IsRecordObj(&o))
		GenError(g, n->line, "%s is a record: assign its fields, or copy it with memcpy", ObjName(n));
	return SizeCode((int)TypeSize(&o.type));
}

/* the value of an lvalue: its address and a load - except for an array
 * and for a record, whose "value" is the address (so that `strcpy(buf,
 * rec.name)` and `memcpy(dst, rec, sizeof(Rec))` read as they do in C) */
static void GenObjectValue(Gen_t* g, Node_t* n)
{
	Obj_t o;
	if(GenObject(g, n, &o) && !o.isArray && !IsRecordObj(&o))
		Emit(g, "load %d", SizeCode((int)TypeSize(&o.type)));
}

static void GenCall(Gen_t* g, Node_t* n)
{
	int i;
	if(n->kind == N_OPCALL)
	{
		int fam = n->op >> 8, op = n->op & 0xff;
		const char* name = Asm_OpName(fam, op, g->u->gen);
		{ // a name the assembler would take for another number (a reused one under "any"): the number instead
			int f2, o2;
			char full[64];
			if(name)
				snprintf(full, sizeof full, fam ? "%s.%s" : "%s%s", fam ? Asm_FamilyName(fam) : "", name);
			if(name && (!Asm_LookupName(full, strlen(full), g->u->gen, &f2, &o2) || f2 != fam || o2 != op))
				name = NULL;
		}
		if(fam == 0 && op == OP_SPRINTF)
		{ // sprintf(dst, fmt, args..): the arguments right to left, then dst, then fmt
			if(n->count < 2)
			{
				GenError(g, n->line, "sprintf takes a destination and a format");
				return;
			}
			for(i = n->count - 1; i >= 2; i--)
				GenExpr(g, n->items[i]);
			GenExpr(g, n->items[0]);
			GenExpr(g, n->items[1]);
			Emit(g, "sprintf");
			return;
		}
		for(i = 0; i < n->count; i++)
			GenExpr(g, n->items[i]);
		if(fam)
			Emit(g, "%s.%s", Asm_FamilyName(fam), name ? name : n->text + strlen(Asm_FamilyName(fam)) + 1);
		else if(name)
			Emit(g, "%s", name);
		else
			Emit(g, "0x%02x", op); // op.0xNN: the listing's bare number
		return;
	}
	// a function: the arguments right to left, then the callee
	for(i = n->count - 1; i >= 0; i--)
		GenExpr(g, n->items[i]);
	if(n->a->kind == N_NAME && FindFunc(g->u, n->a->text) && !FindVar(g->func, n->a->text) && !FindArea(g->u, n->a->text))
		Emit(g, "push_code_off %s", n->a->text);
	else
		GenExpr(g, n->a);
	Emit(g, "call");
}

static void GenExpr(Gen_t* g, Node_t* n)
{
	switch(n->kind)
	{
		case N_NUM:
			if(n->size == 4)
				Emit(g, "push_i32 0x%x", (unsigned)n->num); // written as eight hex digits: pushed wide
			else
				EmitPush(g, (uint32_t)n->num);
			break;
		case N_STR: Emit(g, "push_code_addr str_%d", InternString(g, n->bytes, n->len)); break;
		case N_NAME:
			if(strcmp(n->text, "__pop") == 0)
				break; // the value already on the stack: nothing to emit
			if(FindVar(g->func, n->text) || FindArea(g->u, n->text))
				GenObjectValue(g, n); // a variable's value; an array's name is its address
			else if(FindFunc(g->u, n->text))
				Emit(g, "push_code_off %s", n->text);
			else
				GenError(g, n->line, "unknown name %s", n->text);
			break;
		case N_ADDR:
		{
			Obj_t o;
			GenObject(g, n->a, &o);
			break;
		}
		case N_INDEX:
		case N_MEMBER: GenObjectValue(g, n); break;
		case N_DEREF:
			GenExpr(g, n->a);
			Emit(g, "load %d", DerefSize(g, n));
			break;
		case N_PAREN: GenExpr(g, n->a); break;
		case N_CALL:
			if(n->a->kind == N_NAME && strcmp(n->a->text, "__sign") == 0)
			{
				GenError(g, n->line, "__sign(v) compares only in a condition: __sign(v) < 0 and the like");
				break;
			}
			GenCall(g, n);
			break;
		case N_OPCALL: GenCall(g, n); break;
		case N_SELECT:
			GenExpr(g, n->a);
			GenExpr(g, n->b);
			GenExpr(g, n->c);
			Emit(g, "select");
			break;
		case N_UNARY:
			GenExpr(g, n->a);
			if(n->op == '-')
			{
				Emit(g, "not");
				Emit(g, "push_i8 1");
				Emit(g, "add");
			}
			else if(n->op == '~')
				Emit(g, "not");
			else
				Emit(g, "lnot");
			break;
		case N_BINARY:
			GenExpr(g, n->a);
			GenExpr(g, n->b);
			Emit(g, "%s", OpMnemonic(g, n->op));
			break;
		case N_ASSIGN:
		{
			int size;
			if(n->b->kind == N_INITLIST)
			{ // a list of values stored from the address on: `*(int*)a = {1, 2};`, or `a = {1, 2};` with an array's name
				int i;
				Obj_t o;
				size = 2;
				if(GenObject(g, n->a, &o))
				{
					if(IsRecordObj(&o))
						GenError(g, n->line, "%s is a record: a list of values needs the type of its elements, *(int*)%s = {..}", ObjName(n->a), ObjName(n->a));
					size = SizeCode((int)TypeSize(&o.type));
				}
				for(i = 0; i < n->b->count; i++)
					GenExpr(g, n->b->items[i]);
				Emit(g, "store_multi %d, %d", size, n->b->count);
				break;
			}
			if(IsCallNode(n->b))
			{ // a call's value: the value first, then the address, and a plain store
				GenExpr(g, n->b);
				size = GenLvalue(g, n->a);
				Emit(g, "store %d", size);
				break;
			}
			size = GenLvalue(g, n->a);
			GenExpr(g, n->b);
			Emit(g, "store_keep %d", size);
			break;
		}
		case N_ASSIGN2:
		{
			int size;
			GenExpr(g, n->b);
			size = GenLvalue(g, n->a);
			Emit(g, "store %d", size);
			break;
		}
		default: GenError(g, n->line, "not an expression"); break;
	}
}

/* The jumps of a condition: `n` with the true target `trueLabel` and the
 * false target `falseLabel`, one of which is the fall-through
 * (`trueFalls`: the true one).  `&&`, `||` and `!` are chains of jumps;
 * anything else - a parenthesized expression among it - is a value and
 * one jcc. */
// whether n is `__sign(v) OP 0` with OP one of < <= > >=
static int IsSignTest(const Node_t* n)
{
	return n->kind == N_BINARY && n->op >= 0x32 && n->op <= 0x35 && n->a->kind == N_CALL && n->a->a->kind == N_NAME && strcmp(n->a->a->text, "__sign") == 0 && n->a->count == 1 && n->b->kind == N_NUM && n->b->num == 0;
}

static void GenCond(Gen_t* g, Node_t* n, int trueLabel, int falseLabel, int trueFalls)
{
	if(n->kind == N_BINARY && n->op == 0x38)
	{ // a && b: a false -> false target; a true -> b
		int next = NewLabel(g);
		GenCond(g, n->a, next, falseLabel, 1);
		PlaceLabel(g, next);
		GenCond(g, n->b, trueLabel, falseLabel, trueFalls);
		return;
	}
	if(n->kind == N_BINARY && n->op == 0x39)
	{ // a || b: a true -> true target; a false -> b
		int next = NewLabel(g);
		GenCond(g, n->a, trueLabel, next, 0);
		PlaceLabel(g, next);
		GenCond(g, n->b, trueLabel, falseLabel, trueFalls);
		return;
	}
	if(n->kind == N_UNARY && n->op == '!')
	{
		GenCond(g, n->a, falseLabel, trueLabel, !trueFalls);
		return;
	}
	if(n->kind == N_PAREN && IsSignTest(n->a))
	{ // the parentheses of !(__sign(v) < 0) make no value
		GenCond(g, n->a, trueLabel, falseLabel, trueFalls);
		return;
	}
	if(IsSignTest(n))
	{                                                                         // __sign(v) < 0 and the like: the jump tests the sign of the value itself (jcc 2..5)
		static const int ccTrue[4] = {4, 3, 5, 2}, ccFalse[4] = {2, 5, 3, 4}; // <= >= < > : the code, its complement
		GenExpr(g, n->a->items[0]);
		if(trueFalls)
		{
			Emit(g, "push_code_off L%d", falseLabel);
			Emit(g, "jcc %d", ccFalse[n->op - 0x32]);
		}
		else
		{
			Emit(g, "push_code_off L%d", trueLabel);
			Emit(g, "jcc %d", ccTrue[n->op - 0x32]);
		}
		return;
	}
	GenExpr(g, n);
	if(trueFalls)
	{
		Emit(g, "push_code_off L%d", falseLabel);
		Emit(g, "jcc 1");
	}
	else
	{
		Emit(g, "push_code_off L%d", trueLabel);
		Emit(g, "jcc 0");
	}
}

static void GenStatements(Gen_t* g, Node_t* block);

static int IsJumpStatement(const Node_t* s)
{
	if(!s)
		return 0;
	if(s->kind == N_BLOCK)
		return 0;
	return s->kind == N_BREAK || s->kind == N_CONTINUE || s->kind == N_GOTO || (s->kind == N_RETURN && !s->a);
}

static void GenJump(Gen_t* g, Node_t* s)
{
	switch(s->kind)
	{
		case N_BREAK:
			if(!g->depth)
				GenError(g, s->line, "break outside a loop");
			else
				EmitJump(g, g->breakLabel[g->depth - 1]);
			break;
		case N_CONTINUE:
			if(!g->depth)
				GenError(g, s->line, "continue outside a loop");
			else
				EmitJump(g, g->continueLabel[g->depth - 1]);
			break;
		case N_GOTO: EmitJump(g, NamedLabel(g, s->text, s->line)); break;
		case N_RETURN: EmitJump(g, g->exitLabel); break;
		default: break;
	}
}

static void EmitAsmLines(Gen_t* g, const char* text, int line)
{
	const char* p = text;
	while(*p)
	{
		const char* e = strchr(p, '\n');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		const char* s = p;
		size_t k = 0;
		while(k < n && isspace((unsigned char)s[k]))
			k++;
		if(k < n)
		{
			// `push_code_addr "literal"`: the literal goes into the string table here
			if(n - k > 15 && strncmp(s + k, "push_code_addr ", 15) == 0 && s[k + 15] == '"')
			{
				uint8_t* bytes;
				uint32_t len;
				const char* next;
				const char* err = Asm_ReadStringLiteral(s + k + 15, s + n, &bytes, &len, &next);
				if(err)
					GenError(g, line, "in __asm: %s", err);
				else
				{
					Emit(g, "push_code_addr str_%d", InternString(g, bytes, len));
					free(bytes);
				}
			}
			else
				fprintf(g->out, "%.*s\n", (int)(n - k), s + k);
		}
		if(!e)
			break;
		p = e + 1;
	}
}

static void GenStatement(Gen_t* g, Node_t* s)
{
	switch(s->kind)
	{
		case N_BLOCK: GenStatements(g, s); break;
		case N_EXPR: GenExpr(g, s->a); break;
		case N_DECL:
			if(s->a)
			{ // int x = v: the value, the address, store
				Var_t* v;
				GenExpr(g, s->a);
				if(VarAddress(g, s->text, s->line, &v))
					Emit(g, "store %d", SizeCode((int)TypeSize(&v->type)));
			}
			break;
		case N_IF:
		{
			int elseLabel = NewLabel(g), endLabel = NewLabel(g), thenLabel = NewLabel(g);
			if(!s->c && s->num && IsJumpStatement(s->b))
			{ // if (c) break; - the direct form: the condition jumps where the statement would
				int target;
				switch(s->b->kind)
				{
					case N_BREAK: target = g->depth ? g->breakLabel[g->depth - 1] : -1; break;
					case N_CONTINUE: target = g->depth ? g->continueLabel[g->depth - 1] : -1; break;
					case N_GOTO: target = NamedLabel(g, s->b->text, s->b->line); break;
					default: target = g->exitLabel; break;
				}
				if(target < 0)
				{
					GenError(g, s->line, "break or continue outside a loop");
					break;
				}
				GenCond(g, s->a, target, elseLabel, 0);
				PlaceLabel(g, elseLabel);
				break;
			}
			GenCond(g, s->a, thenLabel, elseLabel, 1);
			PlaceLabel(g, thenLabel);
			GenStatement(g, s->b);
			if(s->c)
			{
				EmitJump(g, endLabel);
				PlaceLabel(g, elseLabel);
				GenStatement(g, s->c);
			}
			else
				PlaceLabel(g, elseLabel);
			PlaceLabel(g, endLabel);
			break;
		}
		case N_WHILE:
		{
			int head = NewLabel(g), end = NewLabel(g), body = NewLabel(g);
			PlaceLabel(g, head);
			if(s->a)
				GenCond(g, s->a, body, end, 1);
			PlaceLabel(g, body);
			if(g->depth >= 64)
			{
				GenError(g, s->line, "loops nested too deep");
				break;
			}
			g->breakLabel[g->depth] = end;
			g->continueLabel[g->depth] = head;
			g->depth++;
			GenStatement(g, s->b);
			g->depth--;
			EmitJump(g, head);
			PlaceLabel(g, end);
			break;
		}
		case N_FOR:
		{
			int head = NewLabel(g), end = NewLabel(g), body = NewLabel(g), incr = NewLabel(g);
			if(s->a)
				GenExpr(g, s->a);
			PlaceLabel(g, head);
			if(s->b)
				GenCond(g, s->b, body, end, 1);
			PlaceLabel(g, body);
			if(g->depth >= 64)
			{
				GenError(g, s->line, "loops nested too deep");
				break;
			}
			g->breakLabel[g->depth] = end;
			g->continueLabel[g->depth] = s->c ? incr : head;
			g->depth++;
			GenStatement(g, s->items[0]);
			g->depth--;
			PlaceLabel(g, incr);
			if(s->c)
				GenExpr(g, s->c);
			EmitJump(g, head);
			PlaceLabel(g, end);
			break;
		}
		case N_DOWHILE:
		{
			int head = NewLabel(g), end = NewLabel(g), cond = NewLabel(g);
			PlaceLabel(g, head);
			if(g->depth >= 64)
			{
				GenError(g, s->line, "loops nested too deep");
				break;
			}
			g->breakLabel[g->depth] = end;
			g->continueLabel[g->depth] = cond;
			g->depth++;
			GenStatement(g, s->b);
			g->depth--;
			PlaceLabel(g, cond);
			GenCond(g, s->a, head, end, 0);
			PlaceLabel(g, end);
			break;
		}
		case N_BREAK:
		case N_CONTINUE:
		case N_GOTO: GenJump(g, s); break;
		case N_RETURN:
			if(s->a)
				GenExpr(g, s->a);
			EmitJump(g, g->exitLabel);
			break;
		case N_LABEL: PlaceLabel(g, NamedLabel(g, s->text, s->line)); break;
		case N_ASM: EmitAsmLines(g, (const char*)s->bytes, s->line); break;
		case N_INLINE:
		{
			uint32_t i;
			char* list;
			size_t o = 0;
			GenExpr(g, s->a);
			list = (char*)malloc(s->len * 6 + 16);
			for(i = 0; i < s->len; i++)
				o += (size_t)sprintf(list + o, "%s0x%02x", i ? ", " : "", s->bytes[i]);
			Emit(g, "store_inline %s", list);
			free(list);
			break;
		}
		default: GenError(g, s->line, "not a statement"); break;
	}
}

static void GenStatements(Gen_t* g, Node_t* block)
{
	int i;
	for(i = 0; i < block->count; i++)
		GenStatement(g, block->items[i]);
}

static void GenFunction(Gen_t* g, Func_t* f)
{
	int i;
	g->func = f;
	g->namedCount = 0;
	g->depth = 0;
	fprintf(g->out, "%s:\n", f->name);
	if(f->isAsm)
	{
		EmitAsmLines(g, (const char*)f->asmNode->bytes, f->asmNode->line);
		return;
	}
	g->exitLabel = NewLabel(g);
	Emit(g, "push_fp");
	EmitPush(g, f->frame);
	Emit(g, "add");
	Emit(g, "set_fp");
	for(i = 0; i < f->params; i++)
	{
		Emit(g, "push_local_addr 0x%x", (unsigned)(f->frame - f->vars[i].off));
		Emit(g, "store 2");
	}
	GenStatements(g, f->body);
	PlaceLabel(g, g->exitLabel);
	Emit(g, "push_fp");
	EmitPush(g, f->frame);
	Emit(g, "sub");
	Emit(g, "set_fp");
	Emit(g, "ret");
}

void Bpc_Compile(const char* text, size_t len, const char* fileName, EngineGen_t gen, const PrepIncludes_t* inc, FILE* log, BpcResult_t* out)
{
	Parser_t p;
	Unit_t unit;
	Gen_t g;
	PrepText_t pre;
	FILE* mem;
	char* listing;
	long n;
	int i;
	AsmResult_t ar;
	Asm_ResetStringEncoding(); // the default, until an #encoding line says otherwise
	memset(out, 0, sizeof *out);
	memset(&p, 0, sizeof p);
	memset(&unit, 0, sizeof unit);
	memset(&g, 0, sizeof g);
	// the includes and the defined names first: the lexer reads the expanded text
	Prep_Expand(text, len, fileName, inc, log, &pre);
	if(pre.errors)
	{
		out->errors = pre.errors;
		Prep_Free(&pre);
		return;
	}
	unit.gen = gen;
	p.lex.p = pre.text;
	p.lex.end = pre.text + pre.len;
	p.lex.line = 1;
	p.lex.log = log ? log : stderr;
	p.lex.prep = &pre;
	p.unit = &unit;
	ParseUnit(&p);
	if(p.lex.peeked)
		free(p.lex.peek.bytes);
	if(!p.lex.errors && !unit.funcCount && !unit.program)
		Warnf(&p.lex, 1, "the source defines no function: the program is empty");
	out->errors = p.lex.errors;
	out->warnings = p.lex.warnings;
	if(out->errors)
	{
		FreeUnit(&unit);
		Prep_Free(&pre);
		return;
	}
	mem = tmpfile();
	if(!mem)
	{
		out->errors = 1;
		FreeUnit(&unit);
		Prep_Free(&pre);
		return;
	}
	g.u = &unit;
	g.lex = &p.lex;
	g.out = mem;
	fprintf(mem, ".engine %s\n", unit.gen == GEN_COUNT ? "any" : kGenNames[unit.gen]);
	if(unit.program)
		fputs(unit.program, mem);
	else
	{
		for(i = 0; i < unit.funcCount; i++)
			GenFunction(&g, unit.funcs[i]);
		fprintf(mem, "__codeend:\n");  // where the code ends, for the decompiler's comparison
		fprintf(mem, "\t.align 16\n"); // the code is padded even when no string follows
		for(i = 0; i < g.strings; i++)
		{
			fprintf(mem, "str_%d:\n\t.str ", i);
			Asm_WriteStringLiteral(mem, g.strTab[i].bytes, g.strTab[i].len);
			fputc('\n', mem);
		}
		if(g.strings)
			fprintf(mem, "\t.align 16\n");
	}
	out->errors = g.errors;
	for(i = 0; i < g.strings; i++)
		free(g.strTab[i].bytes);
	free(g.strTab);
	if(out->errors)
	{
		fclose(mem);
		FreeUnit(&unit);
		Prep_Free(&pre);
		return;
	}
	n = ftell(mem);
	fseek(mem, 0, SEEK_SET);
	listing = (char*)malloc((size_t)n + 1);
	if(fread(listing, 1, (size_t)n, mem) != (size_t)n)
		n = 0;
	listing[n] = 0;
	fclose(mem);
	Asm_Assemble(listing, (size_t)n, unit.gen, p.lex.log, &ar);
	free(listing);
	out->errors = ar.errors;
	out->warnings = p.lex.warnings + ar.warnings;
	out->file = ar.file;
	out->size = ar.size;
	out->labels = ar.labels;
	out->labelCount = ar.labelCount;
	FreeUnit(&unit);
	Prep_Free(&pre);
}
