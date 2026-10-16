/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * as.c - the assembler: listing text to a program file (Asm_Assemble);
 *        interface in asm.h
 *
 * Syntax (docs/asm.md): one statement per line, `label:` prefixes, `;`
 * comments; mnemonics as the disassembler writes them (`push_i8 -1`,
 * `push_code_off loc_0040`, `sys.yield`, `sys.0x5f`); the directives
 * `.engine NAME`, `.str "text"` (Shift-JIS, NUL-terminated; UTF-8 text
 * and \xNN bytes), `.db` / `.dw` / `.dd` lists (strings allowed in `.db`,
 * without a NUL), `.align N`.  Operands are expressions of numbers,
 * labels, `+`, `-` and `code(label)` (a tagged pointer into the code area
 * as the engine generation lays pointers out).
 *
 * Convenience forms that expand to more than one instruction: `push V`
 * (`push_i8` / `push_i16` / `push_i32` by the value), `jmp L`, `call L`,
 * `jcc C, L` (a `push_code_off L` before the real instruction; in 1.667+
 * a `jcc` whose condition has bit 3 is the inline form instead).
 *
 * The text is parsed line by line (ParseLine) into a list of statements
 * (Stmt_t) whose operands are expression trees (Expr_t) over a symbol
 * table of labels; a label is a statement of its own so that the passes
 * can place it.  Then the statements are encoded (EncodeStmt) in passes:
 * the sizes and offsets are recomputed until nothing changes - a `push`
 * of a label may need a wider encoding once the labels have moved, and
 * sizes only ever grow, so the passes end - and the last pass emits the
 * bytes and reports what is out of range.
 */
#include "bgi/asm.h"
#include "bgi/os.h"

#include <ctype.h>

#define MAX_OPERANDS 64 // per statement (a `.db` list or a `store_inline`)

// ---- expressions ----------------------------------------------------------------------------

// the node kinds of an operand expression
typedef enum ExprKind
{
	EX_NUM,  // a number
	EX_SYM,  // a label
	EX_CODE, // code(expr): a tagged pointer into the code area
	EX_ADD,  // a + b
	EX_SUB,  // a - b
	EX_NEG   // -a
} ExprKind_t;

// one node of an operand expression
typedef struct Expr
{
	ExprKind_t kind;
	int64_t num;        // EX_NUM: the value
	char* sym;          // EX_SYM: the label's name (owned, except in a label statement: the symbol's)
	struct Expr *a, *b; // the operands of EX_ADD / EX_SUB; `a` alone for EX_NEG / EX_CODE
} Expr_t;

// one operand of a statement: an expression or a string literal
typedef struct Operand
{
	Expr_t* expr;   // an expression operand (NULL for a string)
	uint8_t* bytes; // a string operand: its bytes in Shift-JIS, without a NUL
	uint32_t len;   // their number
	int isString;   // 1 for a string operand
} Operand_t;

// what a statement is
typedef enum StmtKind
{
	ST_NONE, // a label: opd[0] names its symbol
	ST_INSN, // fam / op with operands
	ST_PUSH, // push V
	ST_JUMP, // jmp / call / jcc with a label: push_code_off + the instruction
	ST_STR,  // .str: NUL-terminated strings
	ST_DB,   // .db: bytes and strings
	ST_DW,   // .dw: 16-bit values
	ST_DD,   // .dd: 32-bit values
	ST_ALIGN // .align N[, fill]
} StmtKind_t;

// one statement of the text, with the state of its encoding
typedef struct Stmt
{
	StmtKind_t kind;
	int line;                    // the source line, for messages
	int fam, op;                 // ST_INSN / ST_JUMP: the instruction (fam 0: base)
	Operand_t opd[MAX_OPERANDS]; // the operands in source order
	int count;                   // the number of operands
	uint32_t off;                // the statement's offset in the area, assigned by the sizing passes
	uint32_t size;               // the current encoding's size in bytes
	int wide;                    // ST_PUSH: the immediate's size in bytes grown so far (1, 2, 4)
	int sleb;                    // not used: the varint operands are re-measured in every pass
} Stmt_t;

// a label, referenced or defined
typedef struct Symbol
{
	char* name;          // the label (owned)
	uint32_t off;        // its offset in the area, from the last sizing pass
	int defined;         // 1 once "name:" was seen; a symbol only referenced is reported by the final pass
	int line;            // the line of the definition, for the "defined again" message
	struct Symbol* next; // the list link
} Symbol_t;

// the state of one assembly
typedef struct As
{
	EngineGen_t gen;                // the generation assembled for (GEN_COUNT: any); `.engine` changes it
	const EngineProfile_t* profile; // its pointer layout, for code(label)
	FILE* log;                      // where messages go
	Stmt_t* stmts;                  // the statements in source order
	int count, cap;                 // their number and the array's capacity
	Symbol_t* symbols;              // every label referenced or defined
	int errors, warnings;           // the counts reported in AsmResult_t
	int line;                       // the line being parsed (1-based), for messages
	uint8_t* out;                   // the area being emitted (NULL during the sizing passes)
	uint32_t outSize, outCap;       // the bytes emitted (or, in a sizing pass, counted) and the buffer's capacity
	int emitting;                   // 1 in the final pass: Emit stores the bytes, else it only counts them
	int changed;                    // a sizing pass moved a label or widened an operand: another pass is needed
} As_t;

// report an error for a line and count it
static void Error(As_t* a, int line, const char* fmt, ...)
{
	va_list ap;
	fprintf(a->log, "%d: error: ", line);
	va_start(ap, fmt);
	vfprintf(a->log, fmt, ap);
	va_end(ap);
	fputc('\n', a->log);
	a->errors++;
}

// report a warning for a line and count it
static void Warn(As_t* a, int line, const char* fmt, ...)
{
	va_list ap;
	fprintf(a->log, "%d: warning: ", line);
	va_start(ap, fmt);
	vfprintf(a->log, fmt, ap);
	va_end(ap);
	fputc('\n', a->log);
	a->warnings++;
}

// the symbol of a name (`len` bytes), NULL when it has not been seen
static Symbol_t* SymFind(As_t* a, const char* name, size_t len)
{
	Symbol_t* s;
	for(s = a->symbols; s; s = s->next)
		if(strlen(s->name) == len && memcmp(s->name, name, len) == 0)
			return s;
	return NULL;
}

// the symbol of a name, created (undefined) when it is new
static Symbol_t* SymGet(As_t* a, const char* name, size_t len)
{
	Symbol_t* s = SymFind(a, name, len);
	if(s)
		return s;
	s = (Symbol_t*)calloc(1, sizeof *s);
	s->name = (char*)malloc(len + 1);
	memcpy(s->name, name, len);
	s->name[len] = 0;
	s->next = a->symbols;
	a->symbols = s;
	return s;
}

// ---- the lexer ------------------------------------------------------------------------------

// a cursor over one line of the text
typedef struct Lex
{
	const char* p;   // the cursor
	const char* end; // the end of the line
	int line;        // its number, for messages
} Lex_t;

static void SkipSpace(Lex_t* l)
{
	while(l->p < l->end && (*l->p == ' ' || *l->p == '\t'))
		l->p++;
}

// identifiers are letters, digits, '_' and '.' (directives and family mnemonics contain dots)
static int IsIdentStart(char c)
{
	return isalpha((unsigned char)c) || c == '_' || c == '.';
}

static int IsIdent(char c)
{
	return isalnum((unsigned char)c) || c == '_' || c == '.';
}

// an identifier at the cursor: its length (0 when none) and start, the cursor advanced past it
static size_t Ident(Lex_t* l, const char** start)
{
	const char* s = l->p;
	if(l->p >= l->end || !IsIdentStart(*l->p))
		return 0;
	while(l->p < l->end && IsIdent(*l->p))
		l->p++;
	*start = s;
	return (size_t)(l->p - s);
}

static Expr_t* NewExpr(ExprKind_t k)
{
	Expr_t* e = (Expr_t*)calloc(1, sizeof *e);
	e->kind = k;
	return e;
}

static void FreeExpr(Expr_t* e)
{
	if(!e)
		return;
	FreeExpr(e->a);
	FreeExpr(e->b);
	free(e->sym);
	free(e);
}

static Expr_t* ParseExpr(As_t* a, Lex_t* l);

/* a term of an expression at the cursor: a number (decimal, 0x hex, or a
 * character constant 'c'), a label, code(expr), a parenthesised expression
 * or a negated term.  NULL after an error (reported).  A label is entered
 * in the symbol table when first seen, undefined until its "name:" is
 * met. */
static Expr_t* ParseTerm(As_t* a, Lex_t* l)
{
	Expr_t* e;
	const char* s;
	size_t n;
	SkipSpace(l);
	if(l->p >= l->end)
	{
		Error(a, l->line, "operand expected");
		return NULL;
	}
	if(*l->p == '-')
	{
		l->p++;
		e = NewExpr(EX_NEG);
		e->a = ParseTerm(a, l);
		return e->a ? e : (FreeExpr(e), NULL);
	}
	if(*l->p == '(')
	{
		l->p++;
		e = ParseExpr(a, l);
		SkipSpace(l);
		if(!e || l->p >= l->end || *l->p != ')')
		{
			Error(a, l->line, "')' expected");
			FreeExpr(e);
			return NULL;
		}
		l->p++;
		return e;
	}
	if(*l->p == '\'')
	{ // a character constant: one byte
		if(l->p + 2 < l->end && l->p[2] == '\'')
		{
			e = NewExpr(EX_NUM);
			e->num = (uint8_t)l->p[1];
			l->p += 3;
			return e;
		}
		Error(a, l->line, "bad character constant");
		return NULL;
	}
	if(isdigit((unsigned char)*l->p))
	{
		char* end;
		unsigned long long v = strtoull(l->p, &end, 0);
		if(end == l->p)
		{
			Error(a, l->line, "bad number");
			return NULL;
		}
		l->p = end;
		e = NewExpr(EX_NUM);
		e->num = (int64_t)v;
		return e;
	}
	n = Ident(l, &s);
	if(!n)
	{
		Error(a, l->line, "unexpected character '%c'", *l->p);
		return NULL;
	}
	if(n == 4 && memcmp(s, "code", 4) == 0 && l->p < l->end && *l->p == '(')
	{
		l->p++;
		e = NewExpr(EX_CODE);
		e->a = ParseExpr(a, l);
		SkipSpace(l);
		if(!e->a || l->p >= l->end || *l->p != ')')
		{
			Error(a, l->line, "')' expected after code(");
			FreeExpr(e);
			return NULL;
		}
		l->p++;
		return e;
	}
	e = NewExpr(EX_SYM);
	e->sym = (char*)malloc(n + 1);
	memcpy(e->sym, s, n);
	e->sym[n] = 0;
	SymGet(a, s, n); // referenced: defined later or reported at the end
	return e;
}

// an expression at the cursor: terms joined by + and -, left to right; NULL after an error
static Expr_t* ParseExpr(As_t* a, Lex_t* l)
{
	Expr_t* e = ParseTerm(a, l);
	while(e)
	{
		Expr_t* r;
		SkipSpace(l);
		if(l->p >= l->end || (*l->p != '+' && *l->p != '-'))
			break;
		r = NewExpr(*l->p == '+' ? EX_ADD : EX_SUB);
		l->p++;
		r->a = e;
		r->b = ParseTerm(a, l);
		if(!r->b)
		{
			FreeExpr(r);
			return NULL;
		}
		e = r;
	}
	return e;
}

/* evaluate an expression: 1 with the value in `*out` when every label in it
 * is defined; else 0 with `*out` 0, and in the final pass an "undefined
 * label" error for `line` (a sizing pass stays quiet, so that the error is
 * reported once).  A label's value is its offset from the last sizing
 * pass; code(x) puts the profile's code tag above the offset bits of x. */
static int Eval(As_t* a, const Expr_t* e, int line, int final, int64_t* out)
{
	int64_t x, y;
	switch(e->kind)
	{
		case EX_NUM: *out = e->num; return 1;
		case EX_SYM:
		{
			Symbol_t* s = SymFind(a, e->sym, strlen(e->sym));
			if(!s || !s->defined)
			{
				if(final)
					Error(a, line, "undefined label %s", e->sym);
				*out = 0;
				return 0;
			}
			*out = s->off;
			return 1;
		}
		case EX_CODE:
			if(!Eval(a, e->a, line, final, &x))
				return 0;
			*out = (int64_t)(((uint32_t)a->profile->tagCode << a->profile->tagShift) | ((uint32_t)x & ((1u << a->profile->tagShift) - 1u)));
			return 1;
		case EX_NEG:
			if(!Eval(a, e->a, line, final, &x))
				return 0;
			*out = -x;
			return 1;
		case EX_ADD:
		case EX_SUB:
			if(!Eval(a, e->a, line, final, &x) || !Eval(a, e->b, line, final, &y))
				return 0;
			*out = e->kind == EX_ADD ? x + y : x - y;
			return 1;
	}
	return 0;
}

/* a string literal at the cursor into bytes (malloc'ed, no NUL added), as
 * Asm_ReadStringLiteral reads it; 1 on success, 0 when no literal starts
 * here or it is unterminated or has a bad escape (reported) */
static int ParseString(As_t* a, Lex_t* l, uint8_t** outBytes, uint32_t* outLen)
{
	const char* next;
	const char* err;
	if(l->p >= l->end || *l->p != '"')
		return 0;
	err = Asm_ReadStringLiteral(l->p, l->end, outBytes, outLen, &next);
	if(err)
	{
		Error(a, l->line, "%s", err);
		return 0;
	}
	l->p = next;
	return 1;
}

// ---- parsing a line ------------------------------------------------------------------------

// append an empty statement for the current line (a->count-- drops it again after an error)
static Stmt_t* NewStmt(As_t* a)
{
	Stmt_t* s;
	if(a->count == a->cap)
	{
		a->cap = a->cap ? a->cap * 2 : 1024;
		a->stmts = (Stmt_t*)realloc(a->stmts, (size_t)a->cap * sizeof *a->stmts);
	}
	s = &a->stmts[a->count++];
	memset(s, 0, sizeof *s);
	s->line = a->line;
	return s;
}

/* the operand list after a mnemonic or directive into `s`: expressions
 * and string literals separated by commas, up to the end of the line or a
 * comment.  1 on success (an empty list is fine), 0 after an error. */
static int ParseOperands(As_t* a, Lex_t* l, Stmt_t* s)
{
	for(;;)
	{
		Operand_t* o;
		SkipSpace(l);
		if(l->p >= l->end || *l->p == ';')
			return 1;
		if(s->count == MAX_OPERANDS)
		{
			Error(a, l->line, "too many operands");
			return 0;
		}
		o = &s->opd[s->count];
		if(*l->p == '"')
		{
			if(!ParseString(a, l, &o->bytes, &o->len))
				return 0;
			o->isString = 1;
		}
		else
		{
			o->expr = ParseExpr(a, l);
			if(!o->expr)
				return 0;
		}
		s->count++;
		SkipSpace(l);
		if(l->p < l->end && *l->p == ',')
		{
			l->p++;
			continue;
		}
		if(l->p < l->end && *l->p != ';')
		{
			Error(a, l->line, "unexpected text after the operands: %.10s", l->p);
			return 0;
		}
		return 1;
	}
}

/* Parse one line (`len` bytes, without its newline) into statements: a
 * statement for each label prefix, then one for the instruction or
 * directive, if any.  `.engine` takes effect at once (it changes the
 * generation the following mnemonics are looked up in) and leaves no
 * statement.  `jmp L`, `call L` and the stack form of `jcc C, L` become
 * ST_JUMP, `push V` ST_PUSH.  Errors are reported and the statement
 * dropped; parsing goes on with the next line. */
static void ParseLine(As_t* a, const char* text, size_t len)
{
	Lex_t l;
	const char* s;
	size_t n;
	Stmt_t* st;
	l.p = text;
	l.end = text + len;
	l.line = a->line;
	{ // a listing's offset / byte columns end with '|': the statement starts after it
		const char* bar = memchr(text, '|', len);
		const char* quote = memchr(text, '"', len);
		const char* semi = memchr(text, ';', len);
		if(bar && (!quote || bar < quote) && (!semi || bar < semi))
			l.p = bar + 1;
	}
	for(;;)
	{ // labels
		const char* save;
		SkipSpace(&l);
		save = l.p;
		n = Ident(&l, &s);
		if(n && l.p < l.end && *l.p == ':')
		{
			Symbol_t* sym = SymGet(a, s, n);
			if(sym->defined)
				Error(a, a->line, "label %.*s defined again (first at line %d)", (int)n, s, sym->line);
			sym->defined = 1;
			sym->line = a->line;
			sym->off = 0; // assigned by the passes
			{             // a label is a statement of its own so that the passes can place it
				st = NewStmt(a);
				st->kind = ST_NONE;
				st->opd[0].expr = NewExpr(EX_SYM);
				st->opd[0].expr->sym = sym->name; // shared with the symbol, not freed with the statement
				st->count = 1;
			}
			l.p++;
			continue;
		}
		l.p = save;
		break;
	}
	SkipSpace(&l);
	if(l.p >= l.end || *l.p == ';')
		return;
	if(l.p + 2 < l.end && l.p[0] == '0' && (l.p[1] == 'x' || l.p[1] == 'X'))
	{ // a bare number is a base opcode written numerically (the disassembler does that for a renumbered one)
		char* end;
		long v = strtol(l.p, &end, 16);
		if(v < 0 || v >= 0x80 || (end < l.end && IsIdent(*end)))
		{
			Error(a, a->line, "bad numeric opcode");
			return;
		}
		l.p = end;
		st = NewStmt(a);
		st->kind = ST_INSN;
		st->fam = 0;
		st->op = (int)v;
		if(!ParseOperands(a, &l, st))
			a->count--;
		return;
	}
	n = Ident(&l, &s);
	if(!n)
	{
		Error(a, a->line, "mnemonic expected");
		return;
	}
	st = NewStmt(a);
	if(s[0] == '.')
	{ // a directive
		if(n == 7 && memcmp(s, ".engine", 7) == 0)
		{ // .engine NAME: a generation name of kGenNames, or "any"
			const char* name;
			size_t m;
			int g;
			SkipSpace(&l);
			name = l.p;
			while(l.p < l.end && !isspace((unsigned char)*l.p) && *l.p != ';')
				l.p++;
			m = (size_t)(l.p - name);
			if(m == 3 && memcmp(name, "any", 3) == 0)
				a->gen = GEN_COUNT; // (the profile stays the one set at the start)
			else
			{
				for(g = 0; g < GEN_COUNT; g++)
					if(strlen(kGenNames[g]) == m && memcmp(kGenNames[g], name, m) == 0)
						break;
				if(g == GEN_COUNT)
					Error(a, a->line, "unknown engine %.*s", (int)m, name);
				else
				{
					a->gen = (EngineGen_t)g;
					a->profile = Engine_ProfileOfGen(a->gen);
				}
			}
			a->count--; // no statement
			return;
		}
		if(n == 4 && memcmp(s, ".str", 4) == 0)
			st->kind = ST_STR;
		else if(n == 3 && memcmp(s, ".db", 3) == 0)
			st->kind = ST_DB;
		else if(n == 3 && memcmp(s, ".dw", 3) == 0)
			st->kind = ST_DW;
		else if(n == 3 && memcmp(s, ".dd", 3) == 0)
			st->kind = ST_DD;
		else if(n == 6 && memcmp(s, ".align", 6) == 0)
			st->kind = ST_ALIGN;
		else
		{
			Error(a, a->line, "unknown directive %.*s", (int)n, s);
			a->count--;
			return;
		}
		if(!ParseOperands(a, &l, st))
			a->count--;
		return;
	}
	if(n == 4 && memcmp(s, "push", 4) == 0)
	{ // push V: starts as push_i8 and widens in the passes as the value needs
		st->kind = ST_PUSH;
		st->wide = 1;
		if(!ParseOperands(a, &l, st) || st->count != 1 || st->opd[0].isString)
		{
			Error(a, a->line, "push takes one value");
			a->count--;
		}
		return;
	}
	if(!Asm_LookupName(s, n, a->gen, &st->fam, &st->op))
	{
		Error(a, a->line, "unknown mnemonic %.*s", (int)n, s);
		a->count--;
		return;
	}
	st->kind = ST_INSN;
	if(!ParseOperands(a, &l, st))
	{
		a->count--;
		return;
	}
	// jmp L / call L / jcc C, L: the stack form needs a push_code_off first
	// ("14" and "16" take no inline operand; "15" only its condition)
	if(st->fam == 0 && ((st->op == 0x14 || st->op == 0x16) && st->count == 1))
		st->kind = ST_JUMP;
	else if(st->fam == 0 && st->op == 0x15 && st->count == 2)
	{
		// 1.667+: a condition with bit 3 makes the target an inline rel16 instead
		int64_t cc = 0;
		int inlineForm = (a->gen == GEN_COUNT || a->gen >= GEN_1_667) && Eval(a, st->opd[0].expr, a->line, 0, &cc) && (cc & 8);
		if(!inlineForm)
			st->kind = ST_JUMP;
	}
}

// ---- encoding ---------------------------------------------------------------------------------

// append `n` bytes to the area; a sizing pass only advances outSize
static void Emit(As_t* a, const void* p, uint32_t n)
{
	if(!a->emitting)
	{
		a->outSize += n;
		return;
	}
	if(a->outSize + n > a->outCap)
	{
		a->outCap = (a->outSize + n) * 2 + 0x1000;
		a->out = (uint8_t*)realloc(a->out, a->outCap);
	}
	memcpy(a->out + a->outSize, p, n);
	a->outSize += n;
}

// the low 8 / 16 / 32 bits of a value, little-endian
static void Emit8(As_t* a, int64_t v)
{
	uint8_t b = (uint8_t)v;
	Emit(a, &b, 1);
}

static void Emit16(As_t* a, int64_t v)
{
	uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
	Emit(a, b, 2);
}

static void Emit32(As_t* a, int64_t v)
{
	uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
	Emit(a, b, 4);
}

/* the number of bytes the signed LEB128 of a value takes: 7 bits per byte
 * until the rest is all sign bits and the last byte's bit 6 shows the sign */
static uint32_t SlebLen(int64_t v)
{
	uint32_t n = 0;
	for(;;)
	{
		uint8_t b = (uint8_t)(v & 0x7f);
		v >>= 7;
		n++;
		if((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40)))
			return n;
	}
}

// append a value as a signed LEB128 of the length SlebLen gives (bit 7 of every byte but the last set)
static void EmitSleb(As_t* a, int64_t v)
{
	for(;;)
	{
		uint8_t b = (uint8_t)(v & 0x7f);
		v >>= 7;
		if((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40)))
		{
			Emit(a, &b, 1);
			return;
		}
		b |= 0x80;
		Emit(a, &b, 1);
	}
}

/* the operand formats of a base opcode as a string of letters, one per
 * operand, the same table as decode.c reads them with (kept in step by the
 * round-trip test): b s8, h s16, w u32, n u16 local offset, c / j s16
 * data / code target from the opcode's offset, z size code, i inline
 * bytes, q count, k condition, L local reference (two source operands:
 * offset and size code), s signed LEB128, S signed LEB128 code target from
 * the end of the instruction, r s16 code target; '?' the optional target
 * of "15" in 1.667+ */
static const char* Formats(int op, EngineGen_t gen)
{
	switch(op)
	{
		case 0x00: return "b";
		case 0x01: return "h";
		case 0x02: return "w";
		case 0x04: return "n";
		case 0x05: return "c";
		case 0x06: return "j";
		case 0x08:
		case 0x09:
		case 0x0a: return "z";
		case 0x0b: return "i";
		case 0x0c: return "zq";
		case 0x15: return (gen == GEN_COUNT || gen >= GEN_1_667) ? "k?" : "k";
		case 0x0f:
		case 0x19:
		case 0x1d:
		case 0x1e: return "L";
		case 0x12: return "S";
		case 0x18: return "w";
		case 0x1a:
		case 0x1b: return "Ls";
		case 0x1c: return "Lb";
		case 0x1f:
		case 0x2c:
		case 0x2d:
		case 0x2e:
		case 0x2f: return "s";
		case 0x3b: return "kr";
		case 0x3c: return "b";
		default: return "";
	}
}

static int Fits(int64_t v, int64_t lo, int64_t hi)
{
	return v >= lo && v <= hi;
}

/* `push V` as push_i8 / push_i16 / push_i32 ("00" / "01" / "02"): the
 * narrowest encoding the value fits, but never narrower than a previous
 * pass chose (the size only grows, so the passes settle) */
static void EncodePush(As_t* a, Stmt_t* s, int64_t v, int final)
{
	int need = Fits(v, -128, 127) ? 1 : Fits(v, -32768, 32767) ? 2
															   : 4;
	if(!final && need > s->wide)
	{
		s->wide = need;
		a->changed = 1;
	}
	if(final && need > s->wide)
		s->wide = need; // (cannot happen after the passes settled)
	Emit8(a, s->wide == 1 ? 0x00 : s->wide == 2 ? 0x01
												: 0x02);
	if(s->wide == 1)
		Emit8(a, v);
	else if(s->wide == 2)
		Emit16(a, v);
	else
		Emit32(a, v);
}

/* Encode one real instruction (`fam` / `op` with `count` operands from
 * `opd`) at the offset `s->off`; `s` also gives the line for messages
 * (ST_JUMP passes a copy of its statement for each half).  The source
 * operands are consumed in the order of Formats; a local reference takes
 * two.  In the final pass values out of range, missing or surplus
 * operands are errors; a sizing pass only measures the current lengths. */
static void EncodeInsn(As_t* a, Stmt_t* s, int fam, int op, Operand_t* opd, int count, int final)
{
	const char* fmt;
	int i, oi = 0;
	uint32_t opIP = s->off, p;
	if(fam)
	{
		Emit8(a, fam);
		Emit8(a, op);
		if(count)
			Error(a, s->line, "a family instruction takes no operands");
		return;
	}
	Emit8(a, op);
	p = opIP + 1; // the offset of the next operand, for the end-relative 'S'
	fmt = Formats(op, a->gen);
	for(i = 0; fmt[i]; i++)
	{
		char c = fmt[i];
		int64_t v = 0, v2 = 0;
		Operand_t* o;
		if(c == '?')
		{ // "15" of 1.667+: the inline target only with cc & 8
			int64_t cc = 0;
			Eval(a, opd[0].expr, s->line, 0, &cc);
			if(!(cc & 8))
				break;
			c = 'r';
		}
		if(c == 'i')
		{ // store_inline: every remaining operand, a string as its bytes, an expression as one byte
			uint8_t buf[0x100];
			uint32_t n = 0;
			int k;
			for(k = oi; k < count; k++)
			{
				if(opd[k].isString)
				{
					if(n + opd[k].len > 0xff)
						break;
					memcpy(buf + n, opd[k].bytes, opd[k].len);
					n += opd[k].len;
				}
				else
				{
					if(!Eval(a, opd[k].expr, s->line, final, &v))
						v = 0;
					if(n == 0xff)
						break;
					buf[n++] = (uint8_t)v;
				}
			}
			if(k < count)
				Error(a, s->line, "store_inline takes at most 255 bytes");
			Emit8(a, n);
			Emit(a, buf, n);
			oi = count;
			break;
		}
		if(oi >= count)
		{
			if(final)
				Error(a, s->line, "operand %d missing", oi + 1);
			break;
		}
		o = &opd[oi++];
		if(o->isString)
		{
			Error(a, s->line, "a string is not a valid operand here");
			continue;
		}
		if(!Eval(a, o->expr, s->line, final, &v))
			v = 0;
		switch(c)
		{
			case 'b':
				if(final && !Fits(v, -128, 255))
					Error(a, s->line, "value %lld does not fit a byte", (long long)v);
				Emit8(a, v);
				p += 1;
				break;
			case 'h':
				if(final && !Fits(v, -32768, 65535))
					Error(a, s->line, "value %lld does not fit 16 bits", (long long)v);
				Emit16(a, v);
				p += 2;
				break;
			case 'w':
				if(final && !Fits(v, -2147483648LL, 4294967295LL))
					Error(a, s->line, "value %lld does not fit 32 bits", (long long)v);
				Emit32(a, v);
				p += 4;
				break;
			case 'n':
				if(final && !Fits(v, 0, 65535))
					Error(a, s->line, "local offset %lld does not fit 16 bits", (long long)v);
				Emit16(a, v);
				p += 2;
				break;
			case 'c':
			case 'j':
			case 'r':
			{ // relative to the opcode's own offset
				int64_t rel = v - (int64_t)opIP;
				if(final && !Fits(rel, -32768, 32767))
					Error(a, s->line, "target 0x%llx is out of the 16-bit range from 0x%x", (long long)v, (unsigned)opIP);
				Emit16(a, rel);
				p += 2;
				break;
			}
			case 'z':
				if(final && !Fits(v, 0, 2))
					Warn(a, s->line, "size code %lld is not 0, 1 or 2", (long long)v); // a warning: emitted as written
				Emit8(a, v);
				p += 1;
				break;
			case 'q':
			case 'k':
				if(final && !Fits(v, 0, 255))
					Error(a, s->line, "value %lld does not fit a byte", (long long)v);
				Emit8(a, v);
				p += 1;
				break;
			case 'L':
				// a local reference: two source operands, the offset (bits 0..13) and the size code (14..15)
				if(oi >= count || opd[oi].isString)
				{
					if(final)
						Error(a, s->line, "a local reference needs an offset and a size code");
					v2 = 0;
				}
				else if(!Eval(a, opd[oi++].expr, s->line, final, &v2))
					v2 = 0;
				if(final && (!Fits(v, 0, 0x3fff) || !Fits(v2, 0, 3)))
					Error(a, s->line, "local reference out of range");
				Emit16(a, (v & 0x3fff) | (v2 << 14));
				p += 2;
				break;
			case 's':
				EmitSleb(a, v);
				p += SlebLen(v);
				break;
			case 'S':
			{ // relative to the end of the instruction, whose length depends on the operand itself
				uint32_t len = 1;
				int64_t rel;
				int tries;
				for(tries = 0; tries < 6; tries++)
				{
					rel = v - (int64_t)(p + len);
					if(SlebLen(rel) == len)
						break;
					len = SlebLen(rel);
				}
				EmitSleb(a, rel);
				p += len;
				break;
			}
			default: break;
		}
	}
	if(oi < count && final)
		Error(a, s->line, "too many operands");
}

/* Encode one statement at the current output position (recorded in
 * `s->off`; `s->size` receives the encoding's size).  A label statement
 * updates its symbol's offset and flags the pass as changed when it moved;
 * ST_JUMP emits a `push_code_off` to its last operand and then the
 * instruction with the others; the data directives check their ranges in
 * the final pass; `.align N[, fill]` pads to a multiple of N. */
static void EncodeStmt(As_t* a, Stmt_t* s, int final)
{
	int64_t v;
	int i;
	s->off = a->outSize;
	switch(s->kind)
	{
		case ST_NONE:
		{ // a label: its offset
			Symbol_t* sym = SymFind(a, s->opd[0].expr->sym, strlen(s->opd[0].expr->sym));
			if(sym && sym->off != a->outSize)
			{
				sym->off = a->outSize;
				a->changed = 1;
			}
			break;
		}
		case ST_INSN: EncodeInsn(a, s, s->fam, s->op, s->opd, s->count, final); break;
		case ST_PUSH:
			if(!Eval(a, s->opd[0].expr, s->line, final, &v))
				v = 0;
			EncodePush(a, s, v, final);
			break;
		case ST_JUMP:
		{ // push_code_off L ("06"), then the instruction with its remaining operands
			Operand_t* target = &s->opd[s->count - 1];
			Stmt_t tmp = *s;
			tmp.off = a->outSize; // each half is relative to its own opcode
			EncodeInsn(a, &tmp, 0, 0x06, target, 1, final);
			tmp.off = a->outSize;
			EncodeInsn(a, &tmp, 0, s->op, s->opd, s->count - 1, final);
			break;
		}
		case ST_STR:
			for(i = 0; i < s->count; i++)
			{
				if(!s->opd[i].isString)
				{
					Error(a, s->line, ".str takes strings");
					continue;
				}
				Emit(a, s->opd[i].bytes, s->opd[i].len);
				Emit8(a, 0);
			}
			break;
		case ST_DB:
		case ST_DW:
		case ST_DD:
			for(i = 0; i < s->count; i++)
			{
				if(s->opd[i].isString)
				{
					if(s->kind != ST_DB)
						Error(a, s->line, "a string needs .db");
					else
						Emit(a, s->opd[i].bytes, s->opd[i].len);
					continue;
				}
				if(!Eval(a, s->opd[i].expr, s->line, final, &v))
					v = 0;
				if(s->kind == ST_DB)
				{
					if(final && !Fits(v, -128, 255))
						Error(a, s->line, "value %lld does not fit a byte", (long long)v);
					Emit8(a, v);
				}
				else if(s->kind == ST_DW)
				{
					if(final && !Fits(v, -32768, 65535))
						Error(a, s->line, "value %lld does not fit 16 bits", (long long)v);
					Emit16(a, v);
				}
				else
					Emit32(a, v);
			}
			break;
		case ST_ALIGN:
		{ // .align N[, fill]: N defaults to 1 (nothing), the fill byte to 0
			int64_t n = 1, fill = 0;
			if(s->count >= 1 && !s->opd[0].isString)
				Eval(a, s->opd[0].expr, s->line, final, &n);
			if(s->count >= 2 && !s->opd[1].isString)
				Eval(a, s->opd[1].expr, s->line, final, &fill);
			if(n <= 0)
				n = 1;
			while(a->outSize % (uint32_t)n)
				Emit8(a, fill);
			break;
		}
	}
	s->size = a->outSize - s->off;
}

// ---- the driver --------------------------------------------------------------------------------

/* Assemble the text: parse every line, warn about the opcodes the
 * generation does not define, size and place the statements until nothing
 * moves, emit the bytes, and wrap them in the 16-byte header.  `*out` is
 * cleared first; its `file` stays NULL when there were errors. */
void Asm_Assemble(const char* text, size_t len, EngineGen_t gen, FILE* log, AsmResult_t* out)
{
	As_t a;
	const char* p = text;
	const char* end = text + len;
	int pass, i;
	Symbol_t* s;
	memset(&a, 0, sizeof a);
	memset(out, 0, sizeof *out);
	a.gen = gen;
	// "any" has no pointer layout of its own: the reference build's serves for code(label)
	a.profile = Engine_ProfileOfGen(gen == GEN_COUNT ? GEN_1_69_444 : gen);
	a.log = log ? log : stderr;
	a.line = 1;

	// parse (lines end with LF or CR LF)
	while(p < end)
	{
		const char* nl = memchr(p, '\n', (size_t)(end - p));
		size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
		if(n && p[n - 1] == '\r')
			n--;
		ParseLine(&a, p, n);
		p = nl ? nl + 1 : end;
		a.line++;
	}
	// (an undefined label is reported where it is used, by the final evaluation)
	// the opcodes outside the generation's set: a warning each, the instruction is still emitted
	for(i = 0; i < a.count; i++)
	{
		Stmt_t* st = &a.stmts[i];
		if((st->kind == ST_INSN || st->kind == ST_JUMP) && a.gen != GEN_COUNT)
		{
			int fam = st->fam;
			OpFamily_t f = fam == 0 ? OPFAM_MAIN : fam == 0x7f ? OPFAM_7F
				: fam == 0x80                                  ? OPFAM_80
				: fam == 0x81                                  ? OPFAM_81
				: fam == 0x90                                  ? OPFAM_90
				: fam == 0x91                                  ? OPFAM_91
				: fam == 0x92                                  ? OPFAM_92
				: fam == 0xa0                                  ? OPFAM_A0
				: fam == 0xb0                                  ? OPFAM_B0
				: fam == 0xc0                                  ? OPFAM_C0
				: fam == 0xd0                                  ? OPFAM_D0
				: fam == 0xe0                                  ? OPFAM_E0
				: fam == 0xef                                  ? OPFAM_EF
															   : OPFAM_COUNT;
			if(f != OPFAM_COUNT && !OpSet_Has(f, st->op, a.gen))
				Warn(&a, st->line, "%s%s0x%02x is not defined in engine %s", fam ? Asm_FamilyName(fam) : "", fam ? "." : "op ", st->op, kGenNames[a.gen]);
		}
	}
	if(a.errors)
		goto done;

	// size and place until nothing moves (sizes only grow, so the loop ends; 64 is a safety bound)
	for(pass = 0; pass < 64; pass++)
	{
		a.changed = 0;
		a.outSize = 0;
		for(i = 0; i < a.count; i++)
			EncodeStmt(&a, &a.stmts[i], 0);
		if(!a.changed)
			break;
	}
	// emit, with the range checks
	a.emitting = 1;
	a.outSize = 0;
	for(i = 0; i < a.count; i++)
		EncodeStmt(&a, &a.stmts[i], 1);
	if(a.errors)
		goto done;
	{ // the header: the code offset 16, the area's size, two zero words
		uint32_t size = a.outSize;
		uint8_t* file = (uint8_t*)BGI_Alloc(16 + size);
		memset(file, 0, 16);
		file[0] = 0x10;
		file[4] = (uint8_t)size;
		file[5] = (uint8_t)(size >> 8);
		file[6] = (uint8_t)(size >> 16);
		file[7] = (uint8_t)(size >> 24);
		if(size) // an empty listing has no buffer
			memcpy(file + 16, a.out, size);
		out->file = file;
		out->size = 16 + size;
	}
	{ // the labels, for the tools that compare what a listing produced
		int n = 0;
		for(s = a.symbols; s; s = s->next)
			if(s->defined)
				n++;
		out->labels = (AsmLabel_t*)BGI_Alloc((size_t)(n + 1) * sizeof *out->labels);
		for(s = a.symbols; s; s = s->next)
			if(s->defined)
			{
				snprintf(out->labels[out->labelCount].name, sizeof out->labels[0].name, "%s", s->name);
				out->labels[out->labelCount].off = s->off;
				out->labelCount++;
			}
	}
done:
	out->errors = a.errors;
	out->warnings = a.warnings;
	// free the statements, their operands and the symbols
	for(i = 0; i < a.count; i++)
	{
		int k;
		Stmt_t* st = &a.stmts[i];
		for(k = 0; k < st->count; k++)
		{
			if(st->kind == ST_NONE)
				free(st->opd[k].expr); // the symbol's name belongs to the symbol
			else
				FreeExpr(st->opd[k].expr);
			free(st->opd[k].bytes);
		}
	}
	free(a.stmts);
	while(a.symbols)
	{
		s = a.symbols;
		a.symbols = s->next;
		free(s->name);
		free(s);
	}
	free(a.out);
}
