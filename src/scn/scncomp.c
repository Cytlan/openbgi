/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * scncomp.c - the scenario compiler: source text (docs/scenario.md) to a
 *             container (interface in scn.h)
 *
 * A recursive-descent parser builds a tree of the file; the generator
 * then writes the tokens the way the original compiler of the file's
 * style would: every statement that the original marks with its source
 * line gets a marker with the line the statement stands on in the text
 * (`#line` directives adjust the count), functions of the library style
 * are laid out in the original order (main first, then sorted by their
 * upper-cased names), strings go into the table in order of first use,
 * and the scenario style gets its label dispatcher appended.  The
 * decompiler (scndec.c) relies on these rules to reproduce a file exactly.
 */
#include "scn_internal.h"
#include "bgi/asm.h"
#include "bgi/os.h"

#include <ctype.h>
#include <stdarg.h>

// ---- the lexer -----------------------------------------------------------------------------------

enum TokKind
{
	T_EOF,
	T_IDENT,
	T_NUM,
	T_STR,
	T_PUNCT,    // an operator or punctuation, in `text`
	T_DIRECTIVE // "#name"
};

typedef struct Token
{
	int kind;
	int line;
	char text[SCN_NAME_MAX]; // identifiers, punctuation, directive names
	int64_t num;
	uint8_t* bytes; // strings (Shift-JIS, no NUL), freed by the parser when consumed
	uint32_t len;
} Token_t;

typedef struct Lex
{
	const char* p;
	const char* end;
	int line;
	int errors;
	int warnings;
	FILE* log;
	const char* fileName;
	Token_t tok; // the current token
	int peeked;
	Token_t peek;
} Lex_t;

static void Errorf(Lex_t* l, int line, const char* fmt, ...)
{
	va_list ap;
	l->errors++;
	if(l->errors > 50)
		return;
	fprintf(l->log, "%s:%d: error: ", l->fileName, line);
	va_start(ap, fmt);
	vfprintf(l->log, fmt, ap);
	va_end(ap);
	fputc('\n', l->log);
}

static void Warnf(Lex_t* l, int line, const char* fmt, ...)
{
	va_list ap;
	l->warnings++;
	fprintf(l->log, "%s:%d: warning: ", l->fileName, line);
	va_start(ap, fmt);
	vfprintf(l->log, fmt, ap);
	va_end(ap);
	fputc('\n', l->log);
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
		while(l->p < l->end && (*l->p == ' ' || *l->p == '\t' || *l->p == '\r'))
			l->p++;
		if(l->p < l->end && *l->p == '\n')
		{
			l->p++;
			l->line++;
			continue;
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
		return;
	}
}

static const char* const kPuncts[] = {">>>", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "<-", "(", ")", "{", "}", "[", "]", ",",
	";", ":", "=", "<", ">", "+", "-", "*", "/", "%", "&", "|", "^", "!", "~", "@", NULL};

static void ReadToken(Lex_t* l, Token_t* t)
{
	SkipSpace(l);
	memset(t, 0, sizeof *t);
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
		// count the newlines inside (a literal may span lines through \n only, but be safe)
		{
			const char* q;
			uint32_t k, srcQ = 0, outQ = 0;
			for(q = l->p; q < next; q++)
			{
				if(*q == '\n')
					l->line++;
				if(*q == '?')
					srcQ++;
			}
			/* a character outside Shift-JIS (CP932) comes out of the conversion as
			 * '?': the text engine cannot show it, so say so (the file is still written) */
			for(k = 0; k < t->len; k++)
				if(t->bytes[k] == '?')
					outQ++;
			if(outQ > srcQ)
				Warnf(l, l->line, "%u character%s not representable in Shift-JIS replaced by '?' in \"%.*s\"", outQ - srcQ,
					outQ - srcQ == 1 ? "" : "s", (int)(next - l->p - 2 > 40 ? 40 : next - l->p - 2), l->p + 1);
		}
		l->p = next;
		t->kind = T_STR;
		return;
	}
	if(isdigit((unsigned char)*l->p))
	{
		char* e;
		t->kind = T_NUM;
		if(l->p[0] == '0' && (l->p[1] == 'x' || l->p[1] == 'X'))
			t->num = (int64_t)strtoull(l->p, &e, 16);
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
				strcpy(t->text, kPuncts[i]);
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
	if(l->peeked)
	{
		l->tok = l->peek;
		l->peeked = 0;
		return;
	}
	ReadToken(l, &l->tok);
}

static const Token_t* Peek(Lex_t* l)
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

static int IsIdent(const Lex_t* l, const char* p)
{
	return l->tok.kind == T_IDENT && strcmp(l->tok.text, p) == 0;
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

static int Expect(Lex_t* l, const char* p)
{
	if(Accept(l, p))
		return 1;
	Errorf(l, l->tok.line, "'%s' expected", p);
	return 0;
}

// ---- the tree ----------------------------------------------------------------------------------------

enum NodeKind
{
	N_NUM,
	N_STR,
	N_NAME,    // a local or parameter, by name (its value)
	N_ADDROF,  // &name
	N_DEREF,   // *(int*)e or *(char*)e: a = e, size in `size`
	N_NEG,     // -e
	N_LNOT,    // !e
	N_NOT,     // ~e
	N_BINARY,  // a op b, the token in `op`
	N_CALL,    // name(args): a user function or a command
	N_RAWCALL, // @0xADDR(args): a function of the main file by address (the raw variant's scenes)
	N_LSTR,    // lstr(n) of the scenario style
	N_INDEX,   // name[i]
	// statements
	N_EMPTY, // `;`: a line marker and nothing else
	N_EXPR,
	N_ASSIGN,  // a = b (size from a)
	N_ASSIGN2, // a <- b: store2
	N_IF,      // a cond, body then, c else (may be NULL; an N_IF alone for `else if`)
	N_WHILE,
	N_SWITCH, // a expr, body; `num` the temp offset override (0 none)
	N_CASE,   // num, or `size` = 1 for default
	N_BREAK,
	N_CONTINUE,
	N_RETURN, // a may be NULL
	N_GOTO,   // text = label
	N_LABEL,
	N_MSG,     // args
	N_JUMP,    // jump a[, b]: size 0 jump / 1 call scenario / 2 local call (a label name in text)
	N_LSTRSET, // lstr(num) = a: a string, an lstr(m), a number, or a `+` chain of those
	N_END,
	N_ASM,      // items = asm lines (N_ASMTOKEN nodes)
	N_ASMTOKEN, // num = token; args hold operands (N_NUM, N_STR, N_NAME for labels); text for label definitions
	N_FILE,     // `#file "name"` inside a body: the line markers name this file from here on (bytes, len)
	N_BLOCK
};

typedef struct Node Node_t;
struct Node
{
	int kind;
	int line;      // the source line the statement stands on
	int closeLine; // the line of a closing brace (if / while / switch / function)
	int elseLine;
	Node_t* a;
	Node_t* b;
	Node_t* c;
	Node_t** items;
	int count, cap;
	int64_t num;
	int size;
	int op;
	int direct; // a call written `name!(..)`: the library convention in a scenario file (docs/scenario.md)
	char text[SCN_NAME_MAX];
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
		n->cap = n->cap ? n->cap * 2 : 8;
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
	if(n->kind != N_FILE) // a file node points at the unit's name
		free(n->bytes);
	free(n);
}

// a local variable declaration
typedef struct Var
{
	char name[SCN_NAME_MAX];
	uint32_t off;  // its offset below the frame pointer
	uint32_t size; // bytes
	int elem;      // the element size for indexing (1 char, 4 int), 0 for arrays of rows (text has the row size)
	int isChar;    // a scalar char
	int isArray;   // declared with [] (its name is its address)
	int isParam;
} Var_t;

typedef struct Func
{
	char name[SCN_NAME_MAX];
	Var_t vars[512];
	int varCount;
	int paramCount;
	uint32_t frameSize;
	Node_t* body;
	int line;      // the header's line
	int closeLine; // the closing brace's line
	int index;     // the source order
	uint8_t* file; // the file the line markers name when the function begins (NULL: the source); not owned
	uint32_t fileLen;
} Func_t;

typedef struct Unit
{
	int style;
	int lineMarkers; // `#style scenario-line`: the older scenario variant (scn_internal.h)
	int raw;         // `#style library-raw`: the headerless files of the 1.66 / 1.69 games (scn_internal.h)
	int srcinfo;     // the scenario compiler writes srcinfo markers
	uint8_t* file;   // `#file` at the top level: the marker file of the functions that follow (NULL: the source)
	uint32_t fileLen;
	uint8_t* files[256]; // every `#file` name seen, owned here (the functions and the nodes point at them)
	int fileCount;
	uint8_t* strings[256]; // `#string`: literals taken into the table right after the includes
	uint32_t stringLen[256];
	int stringCount;
	char imports[SCN_IMPORTS][SCN_NAME_MAX];
	int importCount;
	uint8_t* source; // the source name (Shift-JIS)
	uint32_t sourceLen;
	uint8_t* includes[32];
	uint32_t includeLen[32];
	int includeCount;
	Func_t** funcs;
	int funcCount, funcCap;
	Node_t* body;                    // the scenario style's statements
	char labels[1024][SCN_NAME_MAX]; // the scenario style's labels in order of definition
	int labelCount;
} Unit_t;

// ---- the parser ------------------------------------------------------------------------------------

typedef struct Parser
{
	Lex_t lex;
	Unit_t* unit;
	Func_t* func; // the function being parsed (NULL at the top level)
	const ScnCmdTable_t* cmds;
} Parser_t;

static Node_t* ParseExpression(Parser_t* p);
static Node_t* ParseStatement(Parser_t* p);
static void ParseDirective(Parser_t* p);
static Node_t* ParseBlock(Parser_t* p, int* closeLine);

/* the scenario style's label table, in order of first mention - a goto
 * or a definition; definitions only in the older variant (what the
 * dispatcher lists) */
static void MentionLabel(Parser_t* p, const char* name)
{
	Unit_t* u = p->unit;
	int i;
	if(p->func)
		return;
	for(i = 0; i < u->labelCount; i++)
		if(strcmp(u->labels[i], name) == 0)
			return;
	if(u->labelCount < (int)(sizeof u->labels / sizeof u->labels[0]))
		strcpy(u->labels[u->labelCount++], name);
}

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

static Node_t* StrNode(Parser_t* p)
{
	Node_t* n = NewNode(N_STR, p->lex.tok.line);
	n->bytes = p->lex.tok.bytes;
	n->len = p->lex.tok.len;
	p->lex.tok.bytes = NULL;
	Next(&p->lex);
	return n;
}

static Node_t* ParsePrimary(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* n;
	if(l->tok.kind == T_NUM)
	{
		n = NewNode(N_NUM, l->tok.line);
		n->num = l->tok.num;
		Next(l);
		return n;
	}
	if(l->tok.kind == T_STR)
		return StrNode(p);
	if(IsPunct(l, "@"))
	{ // @0xADDR(args): a function of the game's main file, called by address (the raw variant)
		int line = l->tok.line;
		Next(l);
		n = NewNode(N_RAWCALL, line);
		if(l->tok.kind != T_NUM)
			Errorf(l, line, "@ADDRESS(..) expects the address of a function of the main file");
		else
		{
			n->num = l->tok.num;
			Next(l);
		}
		Expect(l, "(");
		if(!IsPunct(l, ")"))
			for(;;)
			{
				AddItem(n, ParseExpression(p));
				if(!Accept(l, ","))
					break;
			}
		Expect(l, ")");
		return n;
	}
	if(Accept(l, "("))
	{
		n = ParseExpression(p);
		Expect(l, ")");
		return n;
	}
	if(l->tok.kind == T_IDENT)
	{
		char name[SCN_NAME_MAX];
		int line = l->tok.line;
		strcpy(name, l->tok.text);
		Next(l);
		if(Accept(l, "(") || (IsPunct(l, "!") && Peek(l)->kind == T_PUNCT && strcmp(Peek(l)->text, "(") == 0))
		{
			int direct = 0;
			if(IsPunct(l, "!"))
			{ // name!( .. ): the direct form
				Next(l);
				Next(l);
				direct = 1;
			}
			n = NewNode(strcmp(name, "lstr") == 0 && p->unit->style == SCN_STYLE_SCENARIO && !direct ? N_LSTR : N_CALL, line);
			strcpy(n->text, name);
			n->direct = direct;
			if(!IsPunct(l, ")"))
				for(;;)
				{
					AddItem(n, ParseExpression(p));
					if(!Accept(l, ","))
						break;
				}
			Expect(l, ")");
			return n;
		}
		if(Accept(l, "["))
		{
			n = NewNode(N_INDEX, line);
			strcpy(n->text, name);
			n->a = ParseExpression(p);
			Expect(l, "]");
			return n;
		}
		n = NewNode(N_NAME, line);
		strcpy(n->text, name);
		return n;
	}
	Errorf(l, l->tok.line, "an expression was expected");
	Next(l);
	return NewNode(N_NUM, l->tok.line);
}

static Node_t* ParseUnary(Parser_t* p)
{
	Lex_t* l = &p->lex;
	int line = l->tok.line;
	Node_t* n;
	if(Accept(l, "-"))
	{
		n = NewNode(N_NEG, line);
		n->a = ParseUnary(p);
		return n;
	}
	if(Accept(l, "!"))
	{
		n = NewNode(N_LNOT, line);
		n->a = ParseUnary(p);
		return n;
	}
	if(Accept(l, "~"))
	{
		n = NewNode(N_NOT, line);
		n->a = ParseUnary(p);
		return n;
	}
	if(Accept(l, "&"))
	{
		n = NewNode(N_ADDROF, line);
		if(l->tok.kind != T_IDENT)
			Errorf(l, line, "a name was expected after &");
		strcpy(n->text, l->tok.text);
		Next(l);
		return n;
	}
	if(Accept(l, "*"))
	{
		// *(int*)e / *(char*)e
		n = NewNode(N_DEREF, line);
		Expect(l, "(");
		if(IsIdent(l, "int"))
			n->size = 2;
		else if(IsIdent(l, "char"))
			n->size = 0;
		else
			Errorf(l, line, "int or char expected in a cast");
		Next(l);
		Expect(l, "*");
		Expect(l, ")");
		n->a = ParseUnary(p);
		return n;
	}
	return ParsePrimary(p);
}

static const ScnOperator_t* OperatorAt(const Lex_t* l)
{
	int i;
	if(l->tok.kind != T_PUNCT)
		return NULL;
	for(i = 0; i < kScnBinaryOpCount; i++)
		if(strcmp(kScnBinaryOps[i].text, l->tok.text) == 0)
			return &kScnBinaryOps[i];
	return NULL;
}

static Node_t* ParseBinary(Parser_t* p, int minPrec)
{
	Lex_t* l = &p->lex;
	Node_t* left = ParseUnary(p);
	for(;;)
	{
		const ScnOperator_t* op = OperatorAt(l);
		Node_t* n;
		if(!op || op->prec < minPrec)
			return left;
		Next(l);
		n = NewNode(N_BINARY, left->line);
		n->op = (int)op->token;
		n->a = left;
		n->b = ParseBinary(p, op->prec + 1);
		left = n;
	}
}

static Node_t* ParseExpression(Parser_t* p)
{
	return ParseBinary(p, 0);
}

// a condition: an expression, or an assignment (`if (x = v)` of the original sources, which pushes nothing)
static Node_t* ParseCondition(Parser_t* p)
{
	Node_t* n = ParseBinary(p, 0);
	if(Accept(&p->lex, "="))
	{
		Node_t* s = NewNode(N_ASSIGN, n->line);
		s->a = n;
		s->b = ParseExpression(p);
		return s;
	}
	return n;
}

// a declaration list at the start of a function body: int a, b; char buf[256]; char rows[4][16]; ...
static int ParseDeclarations(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Func_t* f = p->func;
	while(IsIdent(l, "int") || IsIdent(l, "char"))
	{
		int isChar = IsIdent(l, "char");
		Next(l);
		for(;;)
		{
			Var_t* v;
			if(l->tok.kind != T_IDENT)
			{
				Errorf(l, l->tok.line, "a variable name was expected");
				return 0;
			}
			if(f->varCount >= (int)(sizeof f->vars / sizeof f->vars[0]))
			{
				Errorf(l, l->tok.line, "too many variables");
				return 0;
			}
			v = &f->vars[f->varCount++];
			memset(v, 0, sizeof *v);
			strcpy(v->name, l->tok.text);
			v->size = isChar ? 1 : 4;
			v->elem = isChar ? 1 : 4;
			v->isChar = isChar;
			Next(l);
			if(Accept(l, "["))
			{
				uint32_t count;
				if(l->tok.kind != T_NUM)
					Errorf(l, l->tok.line, "an array size was expected");
				count = (uint32_t)l->tok.num;
				Next(l);
				Expect(l, "]");
				v->size = count * (isChar ? 1 : 4);
				v->isChar = 0;
				v->isArray = 1;
				if(Accept(l, "["))
				{
					uint32_t row;
					if(l->tok.kind != T_NUM)
						Errorf(l, l->tok.line, "a row size was expected");
					row = (uint32_t)l->tok.num;
					Next(l);
					Expect(l, "]");
					v->size = count * row * (isChar ? 1 : 4);
					v->elem = (int)(row * (isChar ? 1 : 4));
				}
			}
			if(!Accept(l, ","))
				break;
		}
		Expect(l, ";");
	}
	return 1;
}

static Node_t* ParseAsmBlock(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Node_t* n = NewNode(N_ASM, l->tok.line);
	Expect(l, "{");
	while(!IsPunct(l, "}") && l->tok.kind != T_EOF)
	{
		Node_t* t = NewNode(N_ASMTOKEN, l->tok.line);
		uint32_t tok;
		int k, i;
		if(l->tok.kind != T_IDENT)
		{
			Errorf(l, l->tok.line, "a token name was expected");
			Next(l);
			FreeNode(t);
			continue;
		}
		// a label definition
		if(Peek(l)->kind == T_PUNCT && strcmp(Peek(l)->text, ":") == 0)
		{
			strcpy(t->text, l->tok.text);
			t->num = -1;
			Next(l);
			Next(l);
			AddItem(n, t);
			continue;
		}
		tok = ScnTok_Lookup(l->tok.text, strlen(l->tok.text));
		if(tok == UINT32_MAX)
		{
			const ScnCmd_t* c = p->cmds ? Scn_FindCommandByName(p->cmds, l->tok.text, strlen(l->tok.text)) : NULL;
			if(c)
				tok = c->token;
			else if(strncmp(l->tok.text, "cmd_", 4) == 0)
				tok = (uint32_t)strtoul(l->tok.text + 4, NULL, 0);
			else if(strcmp(l->tok.text, "lineinfo") == 0)
				tok = SCN_LINEINFO;
			else
			{
				Errorf(l, l->tok.line, "unknown token '%s'", l->tok.text);
				tok = 0;
			}
		}
		t->num = tok;
		Next(l);
		k = Scn_OperandCount(tok);
		for(i = 0; i < k; i++)
		{
			Node_t* o;
			if(i)
				Expect(l, ",");
			if(l->tok.kind == T_NUM)
			{
				o = NewNode(N_NUM, l->tok.line);
				o->num = l->tok.num;
				Next(l);
			}
			else if(l->tok.kind == T_STR)
				o = StrNode(p);
			else if(l->tok.kind == T_IDENT)
			{
				o = NewNode(N_NAME, l->tok.line);
				strcpy(o->text, l->tok.text);
				Next(l);
			}
			else
			{
				Errorf(l, l->tok.line, "an operand was expected");
				o = NewNode(N_NUM, l->tok.line);
			}
			AddItem(t, o);
		}
		AddItem(n, t);
	}
	Expect(l, "}");
	return n;
}

static Node_t* ParseStatement(Parser_t* p)
{
	Lex_t* l = &p->lex;
	int line = l->tok.line;
	Node_t* n;
	if(l->tok.kind == T_DIRECTIVE)
	{ // `#line N` sets the count; `#file "name"` switches the marker file from here on
		uint8_t* before = p->unit->file;
		ParseDirective(p);
		if(p->unit->file != before)
		{
			n = NewNode(N_FILE, line);
			n->bytes = p->unit->file;
			n->len = p->unit->fileLen;
			return n;
		}
		return NULL;
	}
	if(IsPunct(l, "{"))
	{
		int closeLine;
		n = ParseBlock(p, &closeLine);
		n->closeLine = closeLine;
		return n;
	}
	if(Accept(l, ";"))
		return NewNode(N_EMPTY, line);
	if(l->tok.kind == T_IDENT)
	{
		// a label
		if(Peek(l)->kind == T_PUNCT && strcmp(Peek(l)->text, ":") == 0 && strcmp(l->tok.text, "default") != 0)
		{
			n = NewNode(N_LABEL, line);
			strcpy(n->text, l->tok.text);
			Next(l);
			Next(l);
			MentionLabel(p, n->text);
			return n;
		}
		if(IsIdent(l, "if"))
		{
			Next(l);
			n = NewNode(N_IF, line);
			Expect(l, "(");
			n->a = ParseCondition(p);
			Expect(l, ")");
			n->b = ParseBlock(p, &n->closeLine);
			if(IsIdent(l, "else"))
			{
				n->elseLine = l->tok.line;
				Next(l);
				if(IsIdent(l, "if"))
				{
					// `else if`: only where no marker is written for the braces (the scenario style)
					if(p->unit->style == SCN_STYLE_LIBRARY)
						Errorf(l, l->tok.line, "write `else { if .. }` in the library style (every brace has a line marker)");
					n->c = ParseStatement(p);
				}
				else
				{
					int closeLine;
					n->c = ParseBlock(p, &closeLine);
				}
			}
			return n;
		}
		if(IsIdent(l, "while"))
		{
			Next(l);
			n = NewNode(N_WHILE, line);
			Expect(l, "(");
			n->a = ParseCondition(p);
			Expect(l, ")");
			n->b = ParseBlock(p, &n->closeLine);
			return n;
		}
		if(IsIdent(l, "switch"))
		{
			Next(l);
			n = NewNode(N_SWITCH, line);
			if(Accept(l, "@"))
			{
				n->num = l->tok.num;
				Next(l);
			}
			Expect(l, "(");
			n->a = ParseExpression(p);
			Expect(l, ")");
			n->b = ParseBlock(p, &n->closeLine);
			return n;
		}
		if(IsIdent(l, "case"))
		{
			Next(l);
			n = NewNode(N_CASE, line);
			if(l->tok.kind != T_NUM)
				Errorf(l, line, "a case value was expected");
			n->num = l->tok.num;
			Next(l);
			Expect(l, ":");
			return n;
		}
		if(IsIdent(l, "default"))
		{
			Next(l);
			n = NewNode(N_CASE, line);
			n->size = 1;
			Expect(l, ":");
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
			if(!p->unit->lineMarkers) // the older variant lists the labels in definition order only
				MentionLabel(p, n->text);
			return n;
		}
		if(IsIdent(l, "end"))
		{
			Next(l);
			Expect(l, ";");
			return NewNode(N_END, line);
		}
		if(IsIdent(l, "__asm"))
		{
			Next(l);
			return ParseAsmBlock(p);
		}
		if(p->unit->style == SCN_STYLE_SCENARIO)
		{
			if(IsIdent(l, "msg") && !p->unit->lineMarkers) // the older variant has no builtin message form
			{
				Next(l);
				n = NewNode(N_MSG, line);
				Expect(l, "(");
				if(!IsPunct(l, ")"))
					for(;;)
					{
						AddItem(n, ParseExpression(p));
						if(!Accept(l, ","))
							break;
					}
				Expect(l, ")");
				Expect(l, ";");
				return n;
			}
			if(IsIdent(l, "jump") || IsIdent(l, "call"))
			{
				int isCall = IsIdent(l, "call");
				Next(l);
				n = NewNode(N_JUMP, line);
				if(l->tok.kind == T_IDENT)
				{
					// a local label
					n->size = 2;
					strcpy(n->text, l->tok.text);
					Next(l);
					Expect(l, ";");
					return n; // a call by name mentions no label to the dispatcher (a goto does)
				}
				n->size = isCall ? 1 : 0;
				n->a = ParseExpression(p);
				if(Accept(l, ","))
					n->b = ParseExpression(p);
				Expect(l, ";");
				return n;
			}
			if(IsIdent(l, "lstr") && Peek(l)->kind == T_PUNCT && strcmp(Peek(l)->text, "(") == 0)
			{
				// lstr(n) = "text"; or an expression statement
				const char* save = l->p;
				int saveLine = l->line, savePeeked = l->peeked;
				Token_t saveTok = l->tok, savePeek = l->peek;
				Next(l);
				Next(l);
				if(l->tok.kind == T_NUM)
				{
					int64_t index = l->tok.num;
					Next(l);
					if(Accept(l, ")") && Accept(l, "="))
					{
						n = NewNode(N_LSTRSET, line);
						n->num = index;
						n->a = ParseExpression(p);
						Expect(l, ";");
						return n;
					}
				}
				// not an assignment: rewind
				l->p = save;
				l->line = saveLine;
				l->peeked = savePeeked;
				l->tok = saveTok;
				l->peek = savePeek;
			}
		}
	}
	// an expression statement or an assignment
	n = ParseExpression(p);
	if(Accept(l, "="))
	{
		Node_t* s = NewNode(N_ASSIGN, line);
		s->a = n;
		s->b = ParseExpression(p);
		Expect(l, ";");
		return s;
	}
	if(Accept(l, "<-"))
	{
		Node_t* s = NewNode(N_ASSIGN2, line);
		s->a = n;
		s->b = ParseExpression(p);
		Expect(l, ";");
		return s;
	}
	Expect(l, ";");
	{
		Node_t* s = NewNode(N_EXPR, line);
		s->a = n;
		return s;
	}
}

// `{ statements }`; *closeLine receives the line of the closing brace
static Node_t* ParseBlock(Parser_t* p, int* closeLine)
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
	*closeLine = l->tok.line;
	n->closeLine = l->tok.line;
	Expect(l, "}");
	return n;
}

static int ParseFunction(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Func_t* f = (Func_t*)calloc(1, sizeof *f);
	Unit_t* u = p->unit;
	f->line = l->tok.line;
	strcpy(f->name, l->tok.text);
	f->index = u->funcCount;
	f->file = u->file;
	f->fileLen = u->fileLen;
	Next(l);
	Expect(l, "(");
	if(!IsPunct(l, ")"))
		for(;;)
		{
			Var_t* v;
			int elem = 4, byteParam = 0;
			// an optional pointer type: `char* p` / `int* p` (what p[i] steps by); `char p` is a byte parameter
			if(IsIdent(l, "char") || IsIdent(l, "int"))
			{
				elem = IsIdent(l, "char") ? 1 : 4;
				Next(l);
				if(elem == 1 && !IsPunct(l, "*"))
					byteParam = 1;
				else
					Expect(l, "*");
			}
			if(l->tok.kind != T_IDENT)
			{
				Errorf(l, l->tok.line, "a parameter name was expected");
				break;
			}
			v = &f->vars[f->varCount++];
			memset(v, 0, sizeof *v);
			strcpy(v->name, l->tok.text);
			v->size = byteParam ? 1 : 4;
			v->elem = elem;
			v->isParam = 1;
			v->isChar = byteParam;
			f->paramCount++;
			Next(l);
			if(!Accept(l, ","))
				break;
		}
	Expect(l, ")");
	Expect(l, "{");
	p->func = f;
	ParseDeclarations(p);
	f->body = NewNode(N_BLOCK, l->tok.line);
	while(!IsPunct(l, "}") && l->tok.kind != T_EOF)
	{
		Node_t* s = ParseStatement(p);
		if(s)
			AddItem(f->body, s);
	}
	f->closeLine = l->tok.line;
	Expect(l, "}");
	p->func = NULL;
	// the frame: locals in declaration order from offset 4 up, then the parameters, then the switch slot
	{
		uint32_t off = 0;
		int i;
		for(i = f->paramCount; i < f->varCount; i++)
		{
			off += f->vars[i].size;
			f->vars[i].off = off;
		}
		for(i = f->paramCount - 1; i >= 0; i--)
		{
			off += f->vars[i].size; // a byte parameter takes one byte
			f->vars[i].off = off;
		}
		f->frameSize = off + 4;
	}
	if(u->funcCount == u->funcCap)
	{
		u->funcCap = u->funcCap ? u->funcCap * 2 : 64;
		u->funcs = (Func_t**)realloc(u->funcs, (size_t)u->funcCap * sizeof *u->funcs);
	}
	u->funcs[u->funcCount++] = f;
	return 1;
}

static void ParseDirective(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Unit_t* u = p->unit;
	char name[SCN_NAME_MAX];
	int line = l->tok.line;
	strcpy(name, l->tok.text);
	Next(l);
	if(strcmp(name, "encoding") == 0)
		return; // the lexer has switched the encoding of the literals
	if(strcmp(name, "style") == 0)
	{
		if(IsIdent(l, "library"))
		{
			u->style = SCN_STYLE_LIBRARY;
			if(Peek(l)->kind == T_PUNCT && strcmp(Peek(l)->text, "-") == 0)
			{ // library-raw: the headerless files
				Next(l);
				Next(l);
				if(IsIdent(l, "raw"))
					u->raw = 1;
				else
					Errorf(l, line, "#style library, library-raw, scenario or scenario-line");
			}
		}
		else if(IsIdent(l, "scenario"))
		{
			u->style = SCN_STYLE_SCENARIO;
			if(Peek(l)->kind == T_PUNCT && strcmp(Peek(l)->text, "-") == 0)
			{ // scenario-line: the older variant
				Next(l);
				Next(l);
				if(IsIdent(l, "line"))
					u->lineMarkers = 1;
				else
					Errorf(l, line, "#style library, library-raw, scenario or scenario-line");
			}
		}
		else
			Errorf(l, line, "#style library, library-raw, scenario or scenario-line");
		Next(l);
	}
	else if(strcmp(name, "file") == 0)
	{ // the file the line markers name from here on (the includes of a raw library hold functions)
		if(l->tok.kind != T_STR)
		{
			Errorf(l, line, "a quoted name was expected");
			return;
		}
		if(u->fileCount < (int)(sizeof u->files / sizeof u->files[0]))
		{
			u->files[u->fileCount] = l->tok.bytes;
			u->file = l->tok.bytes;
			u->fileLen = l->tok.len;
			u->fileCount++;
		}
		else
			free(l->tok.bytes);
		l->tok.bytes = NULL;
		Next(l);
	}
	else if(strcmp(name, "srcinfo") == 0)
		u->srcinfo = 1;
	else if(strcmp(name, "import") == 0)
	{
		for(;;)
		{
			if(l->tok.kind != T_STR)
			{
				Errorf(l, line, "a quoted name was expected");
				break;
			}
			if(u->importCount < SCN_IMPORTS && l->tok.len < SCN_NAME_MAX)
			{
				memcpy(u->imports[u->importCount], l->tok.bytes, l->tok.len);
				u->imports[u->importCount][l->tok.len] = 0;
				u->importCount++;
			}
			free(l->tok.bytes);
			l->tok.bytes = NULL;
			Next(l);
			if(!Accept(l, ","))
				break;
		}
	}
	else if(strcmp(name, "source") == 0 || strcmp(name, "include") == 0 || strcmp(name, "string") == 0)
	{
		if(l->tok.kind != T_STR)
		{
			Errorf(l, line, "a quoted name was expected");
			return;
		}
		if(strcmp(name, "source") == 0)
		{
			free(u->source);
			u->source = l->tok.bytes;
			u->sourceLen = l->tok.len;
		}
		else if(strcmp(name, "string") == 0)
		{
			if(u->stringCount < (int)(sizeof u->strings / sizeof u->strings[0]))
			{
				u->strings[u->stringCount] = l->tok.bytes;
				u->stringLen[u->stringCount] = l->tok.len;
				u->stringCount++;
			}
			else
				free(l->tok.bytes);
		}
		else if(u->includeCount < (int)(sizeof u->includes / sizeof u->includes[0]))
		{
			u->includes[u->includeCount] = l->tok.bytes;
			u->includeLen[u->includeCount] = l->tok.len;
			u->includeCount++;
		}
		else
			free(l->tok.bytes);
		l->tok.bytes = NULL;
		Next(l);
	}
	else if(strcmp(name, "line") == 0)
	{
		// the next physical line gets this number
		if(l->tok.kind != T_NUM)
		{
			Errorf(l, line, "a line number was expected");
			return;
		}
		{
			int n = (int)l->tok.num;
			// skip to the end of the directive's line, then set the counter
			while(l->p < l->end && *l->p != '\n')
				l->p++;
			if(l->p < l->end)
				l->p++;
			l->line = n;
			l->peeked = 0;
			Next(l);
		}
	}
	else
		Errorf(l, line, "unknown directive #%s", name);
}

static void ParseUnit(Parser_t* p)
{
	Lex_t* l = &p->lex;
	Unit_t* u = p->unit;
	Next(l);
	while(l->tok.kind != T_EOF)
	{
		if(l->tok.kind == T_DIRECTIVE)
		{
			ParseDirective(p);
			continue;
		}
		if(u->style == SCN_STYLE_LIBRARY)
		{
			if(l->tok.kind == T_IDENT && Peek(l)->kind == T_PUNCT && strcmp(Peek(l)->text, "(") == 0)
			{
				ParseFunction(p);
				continue;
			}
			Errorf(l, l->tok.line, "a function definition was expected");
			Next(l);
			continue;
		}
		{
			Node_t* s = ParseStatement(p);
			if(s)
				AddItem(u->body, s);
		}
	}
}

// ---- the generator ---------------------------------------------------------------------------------

typedef struct Fixup
{
	uint32_t at; // dword index of the operand
	int label;   // a label id, or -1 for a function (name in `name`)
	char name[SCN_NAME_MAX];
} Fixup_t;

typedef struct Code
{
	uint32_t* w;
	uint32_t n, cap;
	Fixup_t* fix;
	int fixCount, fixCap;
	uint32_t* labelPos; // label id -> dword index (UINT32_MAX unresolved)
	int labelCount, labelCap;
} Code_t;

typedef struct Gen
{
	Unit_t* u;
	Lex_t* lex; // for the messages
	const ScnCmdTable_t* cmds;
	// the string table
	uint8_t* strings;
	uint32_t stringsLen, stringsCap;
	uint32_t* strOff; // the offsets of the strings in the table (relative to its start) for the lookup
	int strCount, strCap;
	Code_t* code; // the code being generated
	Func_t* func;
	const uint8_t* markerFile; // the file the line markers name (NULL: the source)
	uint32_t markerFileLen;
	// control context
	struct
	{
		int breakLabel, continueLabel;
	} ctx[64];
	int ctxDepth;
	int funcEndLabel;
	uint32_t switchTemp;
	int srcinfoIndex;
	// the scenario style's named labels: name -> label id
	struct
	{
		char name[SCN_NAME_MAX];
		int label;
	} named[1024];
	int namedCount;
	int errors;
} Gen_t;

static void GenError(Gen_t* g, int line, const char* fmt, ...)
{
	va_list ap;
	g->errors++;
	fprintf(g->lex->log, "%s:%d: error: ", g->lex->fileName, line);
	va_start(ap, fmt);
	vfprintf(g->lex->log, fmt, ap);
	va_end(ap);
	fputc('\n', g->lex->log);
}

static void Emit(Gen_t* g, uint32_t w)
{
	Code_t* c = g->code;
	if(c->n == c->cap)
	{
		c->cap = c->cap ? c->cap * 2 : 1024;
		c->w = (uint32_t*)realloc(c->w, c->cap * sizeof *c->w);
	}
	c->w[c->n++] = w;
}

static int NewLabel(Gen_t* g)
{
	Code_t* c = g->code;
	if(c->labelCount == c->labelCap)
	{
		c->labelCap = c->labelCap ? c->labelCap * 2 : 64;
		c->labelPos = (uint32_t*)realloc(c->labelPos, (size_t)c->labelCap * sizeof *c->labelPos);
	}
	c->labelPos[c->labelCount] = UINT32_MAX;
	return c->labelCount++;
}

static void PlaceLabel(Gen_t* g, int label)
{
	g->code->labelPos[label] = g->code->n;
}

// `addr LABEL` with a fixup
static void EmitAddr(Gen_t* g, int label, const char* funcName)
{
	Code_t* c = g->code;
	Emit(g, SCN_ADDR);
	if(c->fixCount == c->fixCap)
	{
		c->fixCap = c->fixCap ? c->fixCap * 2 : 64;
		c->fix = (Fixup_t*)realloc(c->fix, (size_t)c->fixCap * sizeof *c->fix);
	}
	c->fix[c->fixCount].at = c->n;
	c->fix[c->fixCount].label = label;
	c->fix[c->fixCount].name[0] = 0;
	if(funcName)
		snprintf(c->fix[c->fixCount].name, sizeof c->fix[0].name, "%s", funcName);
	c->fixCount++;
	Emit(g, 0);
}

// the offset of a string in the table (added at the end on first use); the absolute offset is fixed up at the end
static uint32_t InternString(Gen_t* g, const uint8_t* s, uint32_t len)
{
	int i;
	uint32_t off;
	for(i = 0; i < g->strCount; i++)
	{
		uint32_t o = g->strOff[i];
		if(strlen((const char*)g->strings + o) == len && memcmp(g->strings + o, s, len) == 0)
			return o;
	}
	if(g->stringsLen + len + 1 > g->stringsCap)
	{
		g->stringsCap = (g->stringsLen + len + 1) * 2 + 1024;
		g->strings = (uint8_t*)realloc(g->strings, g->stringsCap);
	}
	off = g->stringsLen;
	memcpy(g->strings + off, s, len);
	g->strings[off + len] = 0;
	g->stringsLen += len + 1;
	if(g->strCount == g->strCap)
	{
		g->strCap = g->strCap ? g->strCap * 2 : 256;
		g->strOff = (uint32_t*)realloc(g->strOff, (size_t)g->strCap * sizeof *g->strOff);
	}
	g->strOff[g->strCount++] = off;
	return off;
}

// `str S`: the operand is the table-relative offset, marked for the final fixup by a flag in the high bit
#define STR_MARK 0x80000000u

static void EmitStr(Gen_t* g, const uint8_t* s, uint32_t len)
{
	Emit(g, SCN_STR);
	Emit(g, InternString(g, s, len) | STR_MARK);
}

// a label name: identifiers are UTF-8 in the source, Shift-JIS in the file
static void EmitLabelName(Gen_t* g, const char* name)
{
	char sjis[SCN_NAME_MAX * 2];
	size_t n = OS_Utf8ToSjis(name, sjis, sizeof sjis);
	EmitStr(g, (const uint8_t*)sjis, (uint32_t)n);
}

static void EmitPush(Gen_t* g, uint32_t v)
{
	Emit(g, SCN_PUSH);
	Emit(g, v);
}

static void EmitLineMarker(Gen_t* g, int line)
{
	if(!g->u->source) // no #source: a file compiled without line markers
		return;
	if(g->u->style == SCN_STYLE_LIBRARY || g->u->lineMarkers)
	{
		Emit(g, SCN_LINE);
		if(g->markerFile)
			Emit(g, InternString(g, g->markerFile, g->markerFileLen) | STR_MARK);
		else
			Emit(g, InternString(g, g->u->source, g->u->sourceLen) | STR_MARK);
		Emit(g, (uint32_t)line);
	}
	else
	{
		EmitStr(g, g->u->source, g->u->sourceLen);
		EmitPush(g, (uint32_t)line);
		Emit(g, SCN_ARGS);
		Emit(g, 2);
		Emit(g, SCN_LINEINFO);
	}
}

static void GenExpr(Gen_t* g, Node_t* n);
static int GenLvalue(Gen_t* g, Node_t* n);

static Var_t* VarOf(Gen_t* g, Node_t* n, const char* name)
{
	static Var_t tmp; // `_tmp`: the switch slot at the bottom of the frame
	Var_t* v = FindVar(g->func, name);
	if(!v && g->func && strcmp(name, "_tmp") == 0)
	{
		memset(&tmp, 0, sizeof tmp);
		strcpy(tmp.name, name);
		tmp.off = g->func->frameSize;
		tmp.size = 4;
		tmp.elem = 4;
		return &tmp;
	}
	if(!v)
		GenError(g, n->line, "unknown variable '%s'", name);
	return v;
}

static int IsFunctionName(Gen_t* g, const char* name)
{
	int i;
	for(i = 0; i < g->u->funcCount; i++)
		if(strcmp(g->u->funcs[i]->name, name) == 0)
			return 1;
	return 0;
}

// the command a call name stands for, or NULL for a user function
static int CommandOf(Gen_t* g, const char* name, uint32_t* tok)
{
	const ScnCmd_t* c;
	if(strncmp(name, "cmd_", 4) == 0 && isdigit((unsigned char)name[4]))
	{
		*tok = (uint32_t)strtoul(name + 4, NULL, 0);
		return 1;
	}
	c = g->cmds ? Scn_FindCommandByName(g->cmds, name, strlen(name)) : NULL;
	if(c)
	{
		*tok = c->token;
		return 1;
	}
	return 0;
}

static void GenCall(Gen_t* g, Node_t* n)
{
	uint32_t tok;
	int i;
	if(!IsFunctionName(g, n->text) && CommandOf(g, n->text, &tok))
	{
		if(g->u->raw && !n->direct)
		{ // the raw variant: left to right, `args n` only when there is something to reverse
			for(i = 0; i < n->count; i++)
				GenExpr(g, n->items[i]);
			if(n->count >= 2)
			{
				Emit(g, SCN_ARGS);
				Emit(g, (uint32_t)n->count);
			}
		}
		else if(g->u->style == SCN_STYLE_LIBRARY || n->direct)
		{ // the arguments right to left, so that the first is on top; no args token
			for(i = n->count - 1; i >= 0; i--)
				GenExpr(g, n->items[i]);
		}
		else
		{ // left to right, and `args n` reverses them
			for(i = 0; i < n->count; i++)
				GenExpr(g, n->items[i]);
			Emit(g, SCN_ARGS);
			Emit(g, (uint32_t)n->count);
		}
		Emit(g, tok);
		return;
	}
	if(n->direct)
	{
		GenError(g, n->line, "%s!( ..): the direct form is for commands, not functions", n->text);
		return;
	}
	for(i = 0; i < n->count; i++)
		GenExpr(g, n->items[i]);
	if(IsFunctionName(g, n->text))
	{
		EmitAddr(g, -1, n->text);
		Emit(g, SCN_CALL);
	}
	else
	{
		EmitStr(g, (const uint8_t*)n->text, (uint32_t)strlen(n->text));
		Emit(g, SCN_CALLFN);
	}
}

static void GenExpr(Gen_t* g, Node_t* n)
{
	Var_t* v;
	switch(n->kind)
	{
		case N_NUM: EmitPush(g, (uint32_t)n->num); break;
		case N_STR: EmitStr(g, n->bytes, n->len); break;
		case N_RAWCALL:
		{ // @0xADDR(args): the arguments left to right, the address, the call-by-address command
			int i;
			for(i = 0; i < n->count; i++)
				GenExpr(g, n->items[i]);
			EmitPush(g, (uint32_t)n->num);
			Emit(g, SCN_RAWCALL);
			break;
		}
		case N_NAME:
			v = VarOf(g, n, n->text);
			if(!v)
				break;
			Emit(g, SCN_LOCAL);
			Emit(g, v->off);
			if((v->isParam && !v->isChar) || (!v->isArray && !v->isChar))
			{
				// a parameter (a value, pointer or not) or a scalar int
				Emit(g, SCN_LOAD);
				Emit(g, 2);
			}
			else if(v->isChar && v->size == 1)
			{
				Emit(g, SCN_LOAD);
				Emit(g, 0);
			}
			// an array: its address
			break;
		case N_ADDROF:
			v = VarOf(g, n, n->text);
			if(!v)
				break;
			Emit(g, SCN_LOCAL);
			Emit(g, v->off);
			break;
		case N_INDEX:
			v = VarOf(g, n, n->text);
			if(!v)
				break;
			Emit(g, SCN_LOCAL);
			Emit(g, v->off);
			if(v->isParam)
			{
				// a pointer parameter: index its value
				Emit(g, SCN_LOAD);
				Emit(g, 2);
			}
			GenExpr(g, n->a);
			EmitPush(g, (uint32_t)v->elem);
			Emit(g, SCN_MUL);
			Emit(g, SCN_ADD);
			if(v->elem == 4 || v->elem == 1)
			{
				Emit(g, SCN_LOAD);
				Emit(g, v->elem == 4 ? 2 : 0);
			}
			break;
		case N_DEREF:
			GenExpr(g, n->a);
			Emit(g, SCN_LOAD);
			Emit(g, (uint32_t)n->size);
			break;
		case N_NEG:
			if(g->u->style == SCN_STYLE_LIBRARY)
			{
				GenExpr(g, n->a);
				EmitPush(g, 0xffffffffu);
				Emit(g, SCN_MUL);
			}
			else
			{
				EmitPush(g, 0);
				GenExpr(g, n->a);
				Emit(g, SCN_SUB);
			}
			break;
		case N_LNOT:
			GenExpr(g, n->a);
			Emit(g, SCN_LNOT);
			break;
		case N_NOT:
			GenExpr(g, n->a);
			Emit(g, SCN_NOT);
			break;
		case N_BINARY:
			GenExpr(g, n->a);
			GenExpr(g, n->b);
			Emit(g, (uint32_t)n->op);
			break;
		case N_CALL: GenCall(g, n); break;
		case N_ASSIGN:
		{
			int size = GenLvalue(g, n->a);
			GenExpr(g, n->b);
			Emit(g, SCN_STORE);
			Emit(g, (uint32_t)size);
			break;
		}
		case N_LSTR:
			if(n->count != 1)
				GenError(g, n->line, "lstr takes one argument");
			else
				GenExpr(g, n->items[0]);
			Emit(g, 0xe8);
			break;
		default: GenError(g, n->line, "not an expression"); break;
	}
}

// the address of an lvalue and its store size
static int GenLvalue(Gen_t* g, Node_t* n)
{
	Var_t* v;
	switch(n->kind)
	{
		case N_NAME:
			v = VarOf(g, n, n->text);
			if(!v)
				return 2;
			Emit(g, SCN_LOCAL);
			Emit(g, v->off);
			return v->isChar ? 0 : 2;
		case N_INDEX:
			v = VarOf(g, n, n->text);
			if(!v)
				return 2;
			Emit(g, SCN_LOCAL);
			Emit(g, v->off);
			if(v->isParam)
			{
				Emit(g, SCN_LOAD);
				Emit(g, 2);
			}
			GenExpr(g, n->a);
			EmitPush(g, (uint32_t)v->elem);
			Emit(g, SCN_MUL);
			Emit(g, SCN_ADD);
			return v->elem == 1 ? 0 : 2;
		case N_DEREF: GenExpr(g, n->a); return n->size;
		default: GenError(g, n->line, "not an lvalue"); return 2;
	}
}

static void GenStatements(Gen_t* g, Node_t* block);

/* the items of `lstr(N) = a + b ..` (scndec.c: ParseLstrSet) into the
 * buffer at local 0x100: a `+` chain left to right, the first item
 * copied, the next ones appended after the buffer's length; a string or
 * an lstr(m) is copied with strcpy_r, a number only pushed, as the
 * original compiler does */
static void GenLstrItems(Gen_t* g, Node_t* n, int line, int first)
{
	if(n->kind == N_BINARY && n->op == SCN_ADD)
	{
		GenLstrItems(g, n->a, line, first);
		Emit(g, SCN_LOCAL);
		Emit(g, 0x100);
		Emit(g, SCN_LOCAL);
		Emit(g, 0x100);
		Emit(g, 0x94); // strlen
		Emit(g, SCN_ADD);
		GenLstrItems(g, n->b, line, 0);
		return;
	}
	(void)first;
	if(n->kind == N_STR)
	{
		EmitStr(g, n->bytes, n->len);
		Emit(g, 0x48);
	}
	else if(n->kind == N_LSTR && n->count == 1)
	{
		GenExpr(g, n->items[0]);
		Emit(g, 0xe8);
		Emit(g, 0x48);
	}
	else if(n->kind == N_NUM)
		EmitPush(g, (uint32_t)n->num);
	else
		GenError(g, line, "a string assignment takes strings, lstr(n) and numbers joined by +");
}

static void GenStatement(Gen_t* g, Node_t* n)
{
	int lib = g->u->style == SCN_STYLE_LIBRARY;
	switch(n->kind)
	{
		case N_BLOCK:
			GenStatements(g, n);
			if(lib)
				EmitLineMarker(g, n->closeLine);
			break;
		case N_EMPTY:
			EmitLineMarker(g, n->line);
			break;
		case N_FILE:
			g->markerFile = n->bytes;
			g->markerFileLen = n->len;
			break;
		case N_EXPR:
			EmitLineMarker(g, n->line);
			GenExpr(g, n->a);
			break;
		case N_ASSIGN:
		{
			int size;
			EmitLineMarker(g, n->line);
			size = GenLvalue(g, n->a);
			GenExpr(g, n->b);
			Emit(g, SCN_STORE);
			Emit(g, (uint32_t)size);
			break;
		}
		case N_ASSIGN2:
		{
			int size;
			EmitLineMarker(g, n->line);
			GenExpr(g, n->b);
			size = GenLvalue(g, n->a);
			Emit(g, SCN_STORE2);
			Emit(g, (uint32_t)size);
			break;
		}
		case N_IF:
		{
			int elseLabel = NewLabel(g), endLabel = NewLabel(g);
			if(lib || g->u->lineMarkers) // the older scenario variant marks the `if` as well
				EmitLineMarker(g, n->line);
			GenExpr(g, n->a);
			EmitAddr(g, elseLabel, NULL);
			Emit(g, SCN_JCC);
			Emit(g, 0);
			GenStatements(g, n->b);
			if(lib)
				EmitLineMarker(g, n->b->closeLine);
			EmitAddr(g, endLabel, NULL);
			Emit(g, SCN_JMP);
			PlaceLabel(g, elseLabel);
			if(n->c)
			{
				if(n->c->kind == N_IF)
					GenStatement(g, n->c); // the chain
				else
				{
					GenStatements(g, n->c);
					if(lib)
						EmitLineMarker(g, n->c->closeLine);
				}
			}
			PlaceLabel(g, endLabel);
			break;
		}
		case N_WHILE:
		{
			int condLabel = NewLabel(g), endLabel = NewLabel(g);
			if(lib)
				EmitLineMarker(g, n->line);
			PlaceLabel(g, condLabel);
			GenExpr(g, n->a);
			EmitAddr(g, endLabel, NULL);
			Emit(g, SCN_JCC);
			Emit(g, 0);
			g->ctx[g->ctxDepth].breakLabel = endLabel;
			g->ctx[g->ctxDepth].continueLabel = condLabel;
			g->ctxDepth++;
			GenStatements(g, n->b);
			g->ctxDepth--;
			if(lib)
				EmitLineMarker(g, n->b->closeLine);
			EmitAddr(g, condLabel, NULL);
			Emit(g, SCN_JMP);
			PlaceLabel(g, endLabel);
			break;
		}
		case N_SWITCH:
		{
			/* `expr; local T; store2 2`, then for every case: [line] (a jump
			 * over the test for all but the first) `push K; local T; load 2;
			 * eq; addr NEXT; jcc 0`; the default has no test; the closing
			 * brace's marker ends it */
			uint32_t temp = n->num ? (uint32_t)n->num : (g->func ? g->func->frameSize : 0);
			int endLabel = NewLabel(g), nextTest = -1, first = 1, i;
			if(lib)
				EmitLineMarker(g, n->line);
			GenExpr(g, n->a);
			Emit(g, SCN_LOCAL);
			Emit(g, temp);
			Emit(g, SCN_STORE2);
			Emit(g, 2);
			g->ctx[g->ctxDepth].breakLabel = endLabel;
			g->ctx[g->ctxDepth].continueLabel = -1;
			g->ctxDepth++;
			for(i = 0; i < n->b->count; i++)
			{
				Node_t* s = n->b->items[i];
				if(s->kind == N_CASE)
				{
					int body, isLast = 1, j;
					for(j = i + 1; j < n->b->count; j++)
						if(n->b->items[j]->kind == N_CASE)
							isLast = 0;
					if(lib)
						EmitLineMarker(g, s->line);
					// a default in last position needs no jump over a test: the failing test lands on it
					if(!first && !(s->size && isLast))
					{
						body = NewLabel(g);
						EmitAddr(g, body, NULL);
						Emit(g, SCN_JMP);
					}
					else
						body = -1;
					if(nextTest >= 0)
						PlaceLabel(g, nextTest);
					if(!s->size)
					{
						nextTest = NewLabel(g);
						EmitPush(g, (uint32_t)s->num);
						Emit(g, SCN_LOCAL);
						Emit(g, temp);
						Emit(g, SCN_LOAD);
						Emit(g, 2);
						Emit(g, SCN_EQ);
						EmitAddr(g, nextTest, NULL);
						Emit(g, SCN_JCC);
						Emit(g, 0);
					}
					else
						nextTest = -1;
					if(body >= 0)
						PlaceLabel(g, body);
					first = 0;
					continue;
				}
				GenStatement(g, s);
			}
			g->ctxDepth--;
			if(lib)
				EmitLineMarker(g, n->b->closeLine);
			if(nextTest >= 0)
				PlaceLabel(g, nextTest);
			PlaceLabel(g, endLabel);
			break;
		}
		case N_CASE: GenError(g, n->line, "a case outside a switch"); break;
		case N_BREAK:
		case N_CONTINUE:
		{
			int i, label = -1;
			for(i = g->ctxDepth - 1; i >= 0; i--)
			{
				int c = n->kind == N_BREAK ? g->ctx[i].breakLabel : g->ctx[i].continueLabel;
				if(c >= 0)
				{
					label = c;
					break;
				}
			}
			if(label < 0)
			{
				GenError(g, n->line, "%s outside a loop", n->kind == N_BREAK ? "break" : "continue");
				break;
			}
			if(lib)
				EmitLineMarker(g, n->line);
			EmitAddr(g, label, NULL);
			Emit(g, SCN_JMP);
			break;
		}
		case N_RETURN:
			if(g->funcEndLabel < 0)
			{
				GenError(g, n->line, "return outside a function");
				break;
			}
			if(lib)
				EmitLineMarker(g, n->line);
			if(n->a)
			{
				GenExpr(g, n->a);
				Emit(g, SCN_SETRET);
			}
			EmitAddr(g, g->funcEndLabel, NULL);
			Emit(g, SCN_JMP);
			break;
		case N_GOTO:
		{
			int i, label = -1;
			for(i = 0; i < g->namedCount; i++)
				if(strcmp(g->named[i].name, n->text) == 0)
					label = g->named[i].label;
			if(label < 0)
			{
				if(g->namedCount >= (int)(sizeof g->named / sizeof g->named[0]))
				{
					GenError(g, n->line, "too many labels");
					break;
				}
				label = NewLabel(g);
				strcpy(g->named[g->namedCount].name, n->text);
				g->named[g->namedCount++].label = label;
			}
			EmitLineMarker(g, n->line);
			if(g->u->lineMarkers)
			{ // the older variant: set the label and jump to the dispatcher at offset 0
				EmitLabelName(g, n->text);
				Emit(g, 0xf6);
				Emit(g, SCN_ADDR);
				Emit(g, 0);
				Emit(g, SCN_JMP);
				break;
			}
			if(!lib)
				EmitLabelName(g, n->text); // the scenario style pushes the name
			EmitAddr(g, label, NULL);
			Emit(g, SCN_JMP);
			break;
		}
		case N_LABEL:
		{
			int i, label = -1;
			for(i = 0; i < g->namedCount; i++)
				if(strcmp(g->named[i].name, n->text) == 0)
					label = g->named[i].label;
			if(label < 0)
			{
				if(g->namedCount >= (int)(sizeof g->named / sizeof g->named[0]))
				{
					GenError(g, n->line, "too many labels");
					break;
				}
				label = NewLabel(g);
				strcpy(g->named[g->namedCount].name, n->text);
				g->named[g->namedCount++].label = label;
			}
			PlaceLabel(g, label);
			break;
		}
		case N_MSG:
		{
			int i;
			EmitLineMarker(g, n->line);
			if(g->u->srcinfo)
			{
				Node_t* text = n->count ? n->items[0] : NULL;
				if(!text || text->kind != N_STR)
				{
					GenError(g, n->line, "the first argument of msg must be the text");
					break;
				}
				Emit(g, SCN_SRCINFO);
				Emit(g, SCN_SRCINFO_MAGIC);
				Emit(g, (uint32_t)g->srcinfoIndex++);
				Emit(g, InternString(g, text->bytes, text->len) | STR_MARK);
			}
			for(i = n->count - 1; i >= 0; i--) // the direct form: the text ends on top
				GenExpr(g, n->items[i]);
			Emit(g, SCN_MSG_TOKEN);
			break;
		}
		case N_JUMP:
			EmitLineMarker(g, n->line);
			if(n->size == 2)
			{
				EmitLabelName(g, n->text);
				Emit(g, 0xf6);
				Emit(g, SCN_ADDR);
				Emit(g, 0);
				Emit(g, SCN_CALL);
				break;
			}
			GenExpr(g, n->a);
			if(n->b)
				GenExpr(g, n->b);
			else
				EmitPush(g, 0);
			Emit(g, 0xf6);
			Emit(g, n->size == 1 ? 0xf0 : 0xf3);
			break;
		case N_LSTRSET:
			EmitLineMarker(g, n->line);
			Emit(g, SCN_PUSHFP);
			EmitPush(g, 0x100);
			Emit(g, SCN_ADD);
			Emit(g, SCN_SETFP);
			Emit(g, SCN_LOCAL);
			Emit(g, 0x100);
			GenLstrItems(g, n->a, n->line, 1);
			EmitPush(g, (uint32_t)n->num);
			Emit(g, 0xe8);
			Emit(g, SCN_LOCAL);
			Emit(g, 0x100);
			Emit(g, 0x48);
			Emit(g, SCN_PUSHFP);
			EmitPush(g, 0x100);
			Emit(g, SCN_SUB);
			Emit(g, SCN_SETFP);
			break;
		case N_END:
			EmitLineMarker(g, n->line);
			Emit(g, SCN_RET);
			break;
		case N_ASM:
		{
			int i, j;
			for(i = 0; i < n->count; i++)
			{
				Node_t* t = n->items[i];
				uint32_t tok = (uint32_t)t->num;
				if(t->num < 0)
				{
					// a label definition
					int k, label = -1;
					for(k = 0; k < g->namedCount; k++)
						if(strcmp(g->named[k].name, t->text) == 0)
							label = g->named[k].label;
					if(label < 0 && g->namedCount < (int)(sizeof g->named / sizeof g->named[0]))
					{
						label = NewLabel(g);
						strcpy(g->named[g->namedCount].name, t->text);
						g->named[g->namedCount++].label = label;
					}
					if(label >= 0)
						PlaceLabel(g, label);
					continue;
				}
				Emit(g, tok);
				for(j = 0; j < t->count; j++)
				{
					Node_t* o = t->items[j];
					if(o->kind == N_NUM)
						Emit(g, (uint32_t)o->num);
					else if(o->kind == N_STR)
						Emit(g, InternString(g, o->bytes, o->len) | STR_MARK);
					else
					{
						// a label or function name
						if(IsFunctionName(g, o->text))
						{
							g->code->n--; // EmitAddr writes the token itself
							EmitAddr(g, -1, o->text);
						}
						else
						{
							int k, label = -1;
							for(k = 0; k < g->namedCount; k++)
								if(strcmp(g->named[k].name, o->text) == 0)
									label = g->named[k].label;
							if(label < 0 && g->namedCount < (int)(sizeof g->named / sizeof g->named[0]))
							{
								label = NewLabel(g);
								strcpy(g->named[g->namedCount].name, o->text);
								g->named[g->namedCount++].label = label;
							}
							g->code->n--;
							EmitAddr(g, label, NULL);
						}
					}
				}
			}
			break;
		}
		default: GenError(g, n->line, "unexpected statement"); break;
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
	g->funcEndLabel = NewLabel(g);
	g->ctxDepth = 0;
	g->namedCount = 0;
	g->markerFile = f->file;
	g->markerFileLen = f->fileLen;
	// a function that is one token block is that block, prologue and all
	if(f->body->count == 1 && f->body->items[0]->kind == N_ASM)
	{
		GenStatement(g, f->body->items[0]);
		g->func = NULL;
		g->funcEndLabel = -1;
		return;
	}
	// prologue
	Emit(g, SCN_PUSHFP);
	EmitPush(g, f->frameSize);
	Emit(g, SCN_ADD);
	Emit(g, SCN_SETFP);
	for(i = f->paramCount - 1; i >= 0; i--)
	{
		Emit(g, SCN_LOCAL);
		Emit(g, f->vars[i].off);
		Emit(g, SCN_STORE2);
		Emit(g, f->vars[i].isChar ? 0 : 2);
	}
	EmitAddr(g, g->funcEndLabel, NULL);
	Emit(g, SCN_TRY);
	GenStatements(g, f->body);
	EmitLineMarker(g, f->closeLine);
	PlaceLabel(g, g->funcEndLabel);
	Emit(g, SCN_ENDTRY);
	Emit(g, SCN_PUSHFP);
	EmitPush(g, f->frameSize);
	Emit(g, SCN_SUB);
	Emit(g, SCN_SETFP);
	Emit(g, SCN_RET);
	g->func = NULL;
	g->funcEndLabel = -1;
}

// the scenario style: the body, its `ret`, and the dispatcher
static void GenScenario(Gen_t* g)
{
	Unit_t* u = g->u;
	int dispatcher = NewLabel(g), bodyLabel = NewLabel(g), i;
	g->funcEndLabel = -1;
	g->ctxDepth = 0;
	g->namedCount = 0;
	// a file that is one token block is that block, entry and dispatcher included
	if(u->body->count == 1 && u->body->items[0]->kind == N_ASM)
	{
		GenStatement(g, u->body->items[0]);
		return;
	}
	EmitAddr(g, dispatcher, NULL);
	Emit(g, SCN_JMP);
	PlaceLabel(g, bodyLabel);
	GenStatements(g, u->body);
	Emit(g, SCN_RET);
	PlaceLabel(g, dispatcher);
	EmitPush(g, 0);
	Emit(g, 0xf7);
	EmitAddr(g, bodyLabel, NULL);
	Emit(g, SCN_JCC);
	Emit(g, 1);
	for(i = 0; i < u->labelCount; i++)
	{
		int k, label = -1;
		for(k = 0; k < g->namedCount; k++)
			if(strcmp(g->named[k].name, u->labels[i]) == 0)
				label = g->named[k].label;
		if(label < 0)
			continue;
		EmitLabelName(g, u->labels[i]);
		Emit(g, 0xf7);
		EmitAddr(g, label, NULL);
		Emit(g, SCN_JCC);
		Emit(g, 1);
	}
	EmitStr(g, (const uint8_t*)SCN_LABEL_NOT_FOUND, (uint32_t)strlen(SCN_LABEL_NOT_FOUND));
	Emit(g, 0xf9);
	Emit(g, 0xf4);
}

static int CompareFuncNames(const void* a, const void* b)
{
	const Func_t* x = *(Func_t* const*)a;
	const Func_t* y = *(Func_t* const*)b;
	const char* p = x->name;
	const char* q = y->name;
	// main first, then by the upper-cased name
	if(strcmp(p, "main") == 0 && strcmp(q, "main") != 0)
		return -1;
	if(strcmp(q, "main") == 0 && strcmp(p, "main") != 0)
		return 1;
	for(; *p && *q; p++, q++)
	{
		int c = toupper((unsigned char)*p), e = toupper((unsigned char)*q);
		if(c != e)
			return c < e ? -1 : 1;
	}
	if(*p || *q)
		return *p ? 1 : -1;
	return x->index < y->index ? -1 : x->index > y->index;
}

static void FreeUnit(Unit_t* u)
{
	int i;
	for(i = 0; i < u->funcCount; i++)
	{
		FreeNode(u->funcs[i]->body);
		free(u->funcs[i]);
	}
	free(u->funcs);
	FreeNode(u->body);
	free(u->source);
	for(i = 0; i < u->includeCount; i++)
		free(u->includes[i]);
	for(i = 0; i < u->fileCount; i++)
		free(u->files[i]);
	for(i = 0; i < u->stringCount; i++)
		free(u->strings[i]);
	memset(u, 0, sizeof *u);
}

void Scn_Compile(const char* text, size_t len, const char* fileName, const ScnCmdTable_t* cmds, const PrepIncludes_t* inc, FILE* log,
	ScnCompileResult_t* out)
{
	Parser_t p;
	Unit_t unit;
	Gen_t g;
	Code_t* codes = NULL;
	int i, j;
	memset(out, 0, sizeof *out);
	Asm_ResetStringEncoding(); // the default, until an #encoding line says otherwise
	if(Scn16_IsSource(text, len))
	{ // a 16-bit scene of the 1.64 games
		out->errors = Scn16_Compile(text, len, fileName, inc, log, &out->file.image, &out->file.imageSize);
		return;
	}
	memset(&p, 0, sizeof p);
	memset(&unit, 0, sizeof unit);
	memset(&g, 0, sizeof g);
	unit.body = NewNode(N_BLOCK, 1);
	p.lex.p = text;
	p.lex.end = text + len;
	p.lex.line = 1;
	p.lex.log = log ? log : stderr;
	p.lex.fileName = fileName;
	p.unit = &unit;
	p.cmds = cmds;
	ParseUnit(&p);
	out->errors = p.lex.errors;
	out->warnings = p.lex.warnings;
	if(out->errors)
	{
		FreeUnit(&unit);
		return;
	}
	g.u = &unit;
	g.lex = &p.lex;
	g.cmds = cmds;
	// the string table starts with the source name and the includes (the library style)
	if(unit.style == SCN_STYLE_LIBRARY && unit.source)
	{
		InternString(&g, unit.source, unit.sourceLen);
		for(i = 0; i < unit.includeCount; i++)
			InternString(&g, unit.includes[i], unit.includeLen[i]);
	}
	for(i = 0; i < unit.stringCount; i++) // `#string`: the literals taken in before the code
		InternString(&g, unit.strings[i], unit.stringLen[i]);
	// generate
	if(unit.style == SCN_STYLE_LIBRARY)
	{
		Func_t** order;
		uint32_t pos = 0;
		codes = (Code_t*)calloc((size_t)unit.funcCount + 1, sizeof *codes);
		for(i = 0; i < unit.funcCount; i++)
		{
			g.code = &codes[i];
			GenFunction(&g, unit.funcs[i]);
		}
		// layout: main first, then by name (the raw variant too; it exports nothing, so its names are free)
		order = (Func_t**)malloc(((size_t)unit.funcCount + 1) * sizeof *order);
		if(unit.funcCount)
			memcpy(order, unit.funcs, (size_t)unit.funcCount * sizeof *order);
		qsort(order, (size_t)unit.funcCount, sizeof *order, CompareFuncNames);
		out->file.exports = (ScnExport_t*)calloc((size_t)unit.funcCount + 1, sizeof *out->file.exports);
		for(i = 0; i < unit.funcCount; i++)
		{
			strcpy(out->file.exports[i].name, order[i]->name);
			out->file.exports[i].off = pos * 4;
			pos += codes[order[i]->index].n;
		}
		out->file.exportCount = unit.funcCount;
		out->file.codeLen = pos;
		out->file.code = (uint32_t*)calloc(pos + 1, sizeof *out->file.code);
		pos = 0;
		for(i = 0; i < unit.funcCount; i++)
		{
			Code_t* c = &codes[order[i]->index];
			uint32_t base = pos;
			for(j = 0; j < c->fixCount; j++)
			{
				Fixup_t* fx = &c->fix[j];
				if(fx->label >= 0)
				{
					if(c->labelPos[fx->label] == UINT32_MAX)
					{
						GenError(&g, order[i]->line, "an unplaced label in %s", order[i]->name);
						continue;
					}
					c->w[fx->at] = (base + c->labelPos[fx->label]) * 4;
				}
				else
				{
					int k, found = 0;
					for(k = 0; k < unit.funcCount; k++)
						if(strcmp(out->file.exports[k].name, fx->name) == 0)
						{
							c->w[fx->at] = out->file.exports[k].off;
							found = 1;
						}
					if(!found)
						GenError(&g, order[i]->line, "unknown function %s", fx->name);
				}
			}
			memcpy(out->file.code + pos, c->w, c->n * sizeof *c->w);
			pos += c->n;
		}
		free(order);
		if(unit.raw)
		{ // the names served the fixups only
			out->file.exportCount = 0;
			out->file.raw = 1;
		}
	}
	else
	{
		Code_t code;
		memset(&code, 0, sizeof code);
		g.code = &code;
		GenScenario(&g);
		for(j = 0; j < code.fixCount; j++)
		{
			Fixup_t* fx = &code.fix[j];
			if(fx->label < 0 || code.labelPos[fx->label] == UINT32_MAX)
			{
				GenError(&g, 0, "undefined label");
				continue;
			}
			code.w[fx->at] = code.labelPos[fx->label] * 4;
		}
		out->file.codeLen = code.n;
		out->file.code = (uint32_t*)calloc(code.n + 1, sizeof *out->file.code);
		memcpy(out->file.code, code.w, code.n * sizeof *code.w);
		free(code.w);
		free(code.fix);
		free(code.labelPos);
	}
	// the string operands become absolute
	for(i = 0; i < (int)out->file.codeLen;)
	{
		uint32_t t = out->file.code[i];
		int k = Scn_OperandCount(t);
		if((t == SCN_STR || t == SCN_LINE) && (out->file.code[i + 1] & STR_MARK))
			out->file.code[i + 1] = (out->file.code[i + 1] & ~STR_MARK) + out->file.codeLen * 4;
		if(t == SCN_SRCINFO && (out->file.code[i + 3] & STR_MARK))
			out->file.code[i + 3] = (out->file.code[i + 3] & ~STR_MARK) + out->file.codeLen * 4;
		i += 1 + k;
	}
	out->file.strings = (uint8_t*)calloc(g.stringsLen + 1, 1);
	if(g.stringsLen) // a file without strings has no table to copy from
		memcpy(out->file.strings, g.strings, g.stringsLen);
	out->file.stringsLen = g.stringsLen;
	out->file.importCount = unit.importCount;
	memcpy(out->file.imports, unit.imports, sizeof unit.imports);
	out->errors = g.errors;
	if(codes)
	{
		for(i = 0; i < unit.funcCount; i++)
		{
			free(codes[i].w);
			free(codes[i].fix);
			free(codes[i].labelPos);
		}
		free(codes);
	}
	free(g.strings);
	free(g.strOff);
	FreeUnit(&unit);
	if(out->errors)
		Scn_Free(&out->file);
}
