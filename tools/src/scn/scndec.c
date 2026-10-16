/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * scndec.c - the scenario decompiler: a token stream back to source text
 *            (interface in scn.h, the language in docs/scenario.md)
 *
 * The decompiler recognizes the shapes the two original compilers give
 * the constructs of their language - a function's prologue, the `line`
 * markers, how an `if`, a `while` and a `switch` are laid out, the
 * argument order of a command - and writes them back as source, placing
 * every statement on the source line its marker names so that the
 * compiler (scncomp.c) regenerates the same markers.  Expressions are
 * recovered by running the stack machine symbolically.  Whatever is not
 * recognized (or does not survive the round-trip check Scn_Decompile
 * runs) is written as an `__asm` block of tokens, so the text always
 * compiles back to the input.
 */
#include "scn_internal.h"
#include "bgi/asm.h"
#include "bgi/os.h"

#include <ctype.h>

// ---- the decoded tokens ----------------------------------------------------------------------

typedef struct Insn
{
	uint32_t off;    // byte offset in the stream
	uint32_t tok;    // the token
	uint32_t ops[3]; // its inline operands
	uint32_t len;    // dwords
} Insn_t;

// a value on the symbolic stack
enum ExprKind
{
	E_NUM,
	E_STR,
	E_LOCALADDR, // `local n`: aux = n
	E_ADDR,      // `addr off`: aux = the offset
	E_CALL,      // a call expression (may or may not have a value)
	E_LOADLOCAL, // the value of a scalar local: aux = n
	E_SCALED,    // index * K: aux = K, `index` the index text
	E_INDEXADDR, // base + index * K: aux = the local's offset, num = K, `index` the index text, `baseIsValue` for a pointer parameter
	E_OTHER
};

typedef struct Expr
{
	char* text;
	int prec;
	int kind;
	uint32_t aux;
	int64_t num;
	char* index;       // E_SCALED / E_INDEXADDR: the index expression
	int baseIsValue;   // E_INDEXADDR: the base is the value of a local (a pointer parameter), not its address
	uint32_t startOff; // the offset of the first token of the expression (where a label into the statement may point)
} Expr_t;

// one piece of output: a line of source placed at a source line number (0: wherever it fits)
typedef struct Entry
{
	int line;
	int depth;           // indentation
	int kind;            // ENT_TEXT, ENT_OPEN (ends with '{'), ENT_CLOSE ('}'), ENT_LABEL
	uint32_t off;        // the token offset the statement starts at (labels are inserted before it)
	int ctx;             // the control context at the statement (for jumps), -1 none
	uint32_t jumpTarget; // for ENT_JUMP: resolved to break / continue / return / goto when the function is done
	uint32_t fileOff;    // the string the function's line markers name (UINT32_MAX: the source), for `#file`
	char* text;
} Entry_t;

enum EntryKind
{
	ENT_TEXT,
	ENT_OPEN,
	ENT_CLOSE,
	ENT_LABEL,
	ENT_JUMP,
	ENT_RAW // a block of lines (an __asm fallback): the writer resynchronizes the line count after it
};

// a loop or switch, what `break` and `continue` mean inside it
typedef struct Ctx
{
	int parent;
	uint32_t breakTarget;    // the offset a `break` jumps to (UINT32_MAX until known)
	uint32_t continueTarget; // UINT32_MAX for a switch
	uint32_t switchTemp;     // the temporary of a switch (nested switches reuse the slot), 0 for a loop
	uint32_t start;          // the instruction index the construct's body starts at
} Ctx_t;

// what the body told about a local of the current function
struct Local
{
	uint32_t off;
	int scalar; // loaded or stored directly: 0 no, 2 as dword, 1 as byte
	int elem;   // the element size the indexing uses (0 none): 4 int, 1 char; for a parameter: it is a pointer to those
	int isParam;
};

typedef struct Dec
{
	const ScnFile_t* f;
	const ScnCmdTable_t* cmds;
	const ScnFuncInfo_t* importFuncs; // the functions of the imports (from the options)
	int importFuncCount;
	int style;
	int lineMarkers;    // the scenario style's older variant (scn_internal.h)
	int raw;            // the headerless variant (scn_internal.h): d->f is a copy with the functions found as exports
	uint32_t funcFile;  // the string the current function's line markers name (UINT32_MAX: none / the source)
	uint32_t curOff;    // the offset of the token ParseExpr is at (the start of what it pushes)
	uint32_t popMinOff; // the lowest start offset popped since that token began (its result starts there)
	Insn_t* insn;
	uint32_t n;
	uint32_t* atOff; // dword offset -> instruction index, UINT32_MAX in between
	// the symbolic stack
	Expr_t stack[256];
	int sp;
	int pendingArgs; // the n of an `args n` waiting for its command, -1 none
	// output
	Entry_t* ent;
	int entCount, entCap;
	int depth;
	Ctx_t ctx[256];
	int ctxCount, curCtx;
	// the current function
	struct Local locals[512];
	int localCount;
	uint32_t funcStart, funcEnd; // the function's first token and its endtry
	uint32_t frameSize;
	int paramCount;
	uint32_t paramOff[256];
	int paramByte[256]; // 1 for a byte parameter (`char p`)
	const char* funcName;
	int srcinfoIndex; // the running message index of srcinfo (scenario style)
	// labels of the scenario style (the dispatcher's), and goto targets
	struct
	{
		uint32_t off;
		char name[SCN_NAME_MAX]; // UTF-8, as written in the source
		char sjis[SCN_NAME_MAX]; // as the file has it
	} labels[1024];
	int labelCount;
	char sourceName[0x100]; // Shift-JIS, what the line markers refer to
	uint32_t sourceStrOff;  // its offset
	// the statement being parsed: where it starts and the line its marker names (for the flushed calls)
	uint32_t stmtOff;
	int stmtLine;
	// failure
	int verbose; // report every failed pattern on stderr
	int failed;
	char failMsg[256];
	uint32_t failOff;
	// the scenario-style body range
	uint32_t bodyStart, bodyEnd;
} Dec_t;

static void Fail(Dec_t* d, uint32_t off, const char* fmt, ...)
{
	va_list ap;
	if(d->failed)
		return;
	d->failed = 1;
	d->failOff = off;
	va_start(ap, fmt);
	vsnprintf(d->failMsg, sizeof d->failMsg, fmt, ap);
	va_end(ap);
	if(d->verbose)
	{
		int i;
		fprintf(stderr, "  %s: at 0x%x: %s\n", d->funcName ? d->funcName : "", off, d->failMsg);
		for(i = d->entCount > 4 ? d->entCount - 4 : 0; i < d->entCount; i++)
			fprintf(stderr, "      %s\n", d->ent[i].text ? d->ent[i].text : "(jump)");
	}
}

// ---- small helpers -------------------------------------------------------------------------

static char* Strdup(const char* s)
{
	size_t n = strlen(s) + 1;
	char* p = (char*)malloc(n);
	memcpy(p, s, n);
	return p;
}

static char* Format(const char* fmt, ...)
{
	va_list ap;
	char buf[0x4000];
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	return Strdup(buf);
}

// the string literal of a string-table offset as source text (quoted), or NULL
static char* StringLiteral(Dec_t* d, uint32_t off)
{
	const char* s = Scn_String(d->f, off);
	FILE* mem;
	char* out;
	long n;
	if(!s)
		return NULL;
	mem = tmpfile();
	if(!mem)
		return NULL;
	Asm_WriteStringLiteral(mem, (const uint8_t*)s, (uint32_t)strlen(s));
	n = ftell(mem);
	fseek(mem, 0, SEEK_SET);
	out = (char*)malloc((size_t)n + 1);
	if(fread(out, 1, (size_t)n, mem) != (size_t)n)
		n = 0;
	out[n] = 0;
	fclose(mem);
	return out;
}

static uint32_t IndexAt(const Dec_t* d, uint32_t off)
{
	if(off & 3 || off / 4 > d->f->codeLen)
		return UINT32_MAX;
	return d->atOff[off / 4];
}

static const Insn_t* At(const Dec_t* d, uint32_t i)
{
	static const Insn_t none = {0, 0xffffffffu, {0, 0, 0}, 1};
	return i < d->n ? &d->insn[i] : &none;
}

static int IsTok(const Dec_t* d, uint32_t i, uint32_t tok)
{
	return i < d->n && d->insn[i].tok == tok;
}

static const ScnCmd_t* Command(const Dec_t* d, uint32_t tok)
{
	return d->cmds ? Scn_FindCommand(d->cmds, tok) : NULL;
}

// the source name of a command: from the table, else "cmd_0x..."
static void CommandName(const Dec_t* d, uint32_t tok, char* out, size_t n)
{
	const ScnCmd_t* c = Command(d, tok);
	if(c)
		snprintf(out, n, "%s", c->name);
	else
		snprintf(out, n, "cmd_0x%x", tok);
}

// ---- the symbolic stack --------------------------------------------------------------------------

static void Push(Dec_t* d, char* text, int prec, int kind, uint32_t aux)
{
	Expr_t* e;
	if(d->sp >= (int)(sizeof d->stack / sizeof d->stack[0]))
	{
		Fail(d, 0, "expression stack overflow");
		free(text);
		return;
	}
	e = &d->stack[d->sp++];
	e->text = text;
	e->prec = prec;
	e->kind = kind;
	e->aux = aux;
	e->num = 0;
	e->index = NULL;
	e->baseIsValue = 0;
	e->startOff = d->popMinOff < d->curOff ? d->popMinOff : d->curOff; // the operands popped for it come first
}

static void FreeExpr(Expr_t* e)
{
	free(e->text);
	free(e->index);
	e->text = NULL;
	e->index = NULL;
}

static void ResetStack(Dec_t* d)
{
	while(d->sp > 0)
		free(d->stack[--d->sp].text);
	d->pendingArgs = -1;
}

static void PushNum(Dec_t* d, uint32_t v)
{
	Expr_t* e;
	char buf[32];
	int32_t sv = (int32_t)v;
	if(v >= 0x10000 && sv > 0)
		snprintf(buf, sizeof buf, "0x%x", v);
	else if(sv < 0)
		snprintf(buf, sizeof buf, "0x%x", v);
	else
		snprintf(buf, sizeof buf, "%u", v);
	Push(d, Strdup(buf), SCN_PREC_PRIMARY, E_NUM, 0);
	e = &d->stack[d->sp - 1];
	e->num = v;
}

static Expr_t Pop(Dec_t* d, uint32_t off)
{
	Expr_t e;
	if(d->sp == 0)
	{
		Fail(d, off, "expression stack underflow");
		memset(&e, 0, sizeof e);
		e.text = Strdup("?");
		e.prec = SCN_PREC_PRIMARY;
		e.kind = E_OTHER;
		e.startOff = off;
		return e;
	}
	e = d->stack[--d->sp];
	if(e.startOff < d->popMinOff)
		d->popMinOff = e.startOff;
	return e;
}


// the text of an operand, parenthesized when its precedence requires
static char* Operand(const Expr_t* e, int minPrec)
{
	if(e->prec < minPrec)
		return Format("(%s)", e->text);
	return Strdup(e->text);
}

// ---- locals of the current function ---------------------------------------------------------------

static struct Local* LocalAt(Dec_t* d, uint32_t off, int create)
{
	int i;
	for(i = 0; i < d->localCount; i++)
		if(d->locals[i].off == off)
			return &d->locals[i];
	if(!create || d->localCount >= (int)(sizeof d->locals / sizeof d->locals[0]))
		return NULL;
	memset(&d->locals[d->localCount], 0, sizeof d->locals[0]);
	d->locals[d->localCount].off = off;
	return &d->locals[d->localCount++];
}

static int ParamIndex(const Dec_t* d, uint32_t off)
{
	int i;
	for(i = 0; i < d->paramCount; i++)
		if(d->paramOff[i] == off)
			return i;
	return -1;
}

// the name of a local by its offset: params are p1.. (the first parameter pops last), others l<hex>
static void LocalName(const Dec_t* d, uint32_t off, char* out, size_t n)
{
	int p = ParamIndex(d, off);
	if(p >= 0)
		snprintf(out, n, "p%d", d->paramCount - p);
	else if(off == d->frameSize && d->style == SCN_STYLE_LIBRARY)
		snprintf(out, n, "_tmp"); // the slot below the parameters that the switch statement uses
	else
		snprintf(out, n, "l%x", off);
}

// ---- output entries ----------------------------------------------------------------------------

static Entry_t* AddEntry(Dec_t* d, int line, int kind, uint32_t off, char* text)
{
	Entry_t* e;
	if(d->entCount == d->entCap)
	{
		d->entCap = d->entCap ? d->entCap * 2 : 1024;
		d->ent = (Entry_t*)realloc(d->ent, (size_t)d->entCap * sizeof *d->ent);
	}
	e = &d->ent[d->entCount++];
	e->line = line;
	e->depth = d->depth;
	e->kind = kind;
	e->off = off;
	e->ctx = d->curCtx;
	e->jumpTarget = 0;
	e->fileOff = d->funcFile;
	e->text = text;
	return e;
}

static int PushCtx(Dec_t* d, uint32_t breakTarget, uint32_t continueTarget)
{
	Ctx_t* c;
	if(d->ctxCount >= (int)(sizeof d->ctx / sizeof d->ctx[0]))
	{
		Fail(d, 0, "too many nested loops");
		return d->curCtx;
	}
	c = &d->ctx[d->ctxCount];
	c->parent = d->curCtx;
	c->breakTarget = breakTarget;
	c->continueTarget = continueTarget;
	c->switchTemp = 0;
	d->curCtx = d->ctxCount++;
	return d->curCtx;
}

static int IsJump(const Dec_t* d, uint32_t i, uint32_t* target);

/* The end the enclosing switch's breaks jump to, as far as its body up to
 * instruction `here` shows: the farthest forward jump target below the
 * function's end.  UINT32_MAX when no switch encloses the position or no
 * break was seen yet. */
static uint32_t EnclosingSwitchEnd(const Dec_t* d, uint32_t temp, uint32_t here)
{
	int c;
	uint32_t i, t, best = UINT32_MAX;
	for(c = d->curCtx; c >= 0; c = d->ctx[c].parent)
		if(d->ctx[c].switchTemp == temp)
			break;
	if(c < 0)
		return UINT32_MAX;
	for(i = d->ctx[c].start; i + 1 < here; i++)
		if(IsJump(d, i, &t) && t > d->insn[here].off && t != d->funcEnd && (best == UINT32_MAX || t > best))
			best = t;
	return best;
}

static void PopCtx(Dec_t* d)
{
	if(d->curCtx >= 0)
		d->curCtx = d->ctx[d->curCtx].parent;
}

// ---- expressions -----------------------------------------------------------------------------------

static uint32_t LineMarker(const Dec_t* d, uint32_t i, int* line);

static int IsCommandToken(uint32_t t)
{
	return t >= 0x40 && t != SCN_LINE && t != SCN_SRCINFO;
}

// pop `count` arguments (the first popped is the first argument) into a comma list
static char* ArgList(Dec_t* d, int count, uint32_t off)
{
	char** parts = (char**)calloc((size_t)count + 1, sizeof *parts);
	size_t total = 1;
	int i;
	char* out;
	for(i = 0; i < count; i++)
	{
		Expr_t e = Pop(d, off);
		parts[i] = e.text;
		total += strlen(e.text) + 2;
	}
	out = (char*)malloc(total);
	out[0] = 0;
	for(i = 0; i < count; i++)
	{
		if(i)
			strcat(out, ", ");
		strcat(out, parts[i]);
		free(parts[i]);
	}
	free(parts);
	return out;
}

// the top `count` values in push order (the scenario style's `args n` form)
static char* ArgListInOrder(Dec_t* d, int count)
{
	char* out;
	size_t total = 1;
	int i;
	for(i = d->sp - count; i < d->sp; i++)
		total += strlen(d->stack[i].text) + 2;
	out = (char*)malloc(total);
	out[0] = 0;
	for(i = d->sp - count; i < d->sp; i++)
	{
		if(i > d->sp - count)
			strcat(out, ", ");
		strcat(out, d->stack[i].text);
		free(d->stack[i].text);
		if(d->stack[i].startOff < d->popMinOff)
			d->popMinOff = d->stack[i].startOff;
	}
	d->sp -= count;
	return out;
}

// the arguments of a user function: the whole stack (its arity is not known)
static char* AllArgsInOrder(Dec_t* d)
{
	// pushed left to right: the bottom of the stack is the first argument
	char* out;
	size_t total = 1;
	int i;
	for(i = 0; i < d->sp; i++)
		total += strlen(d->stack[i].text) + 2;
	out = (char*)malloc(total);
	out[0] = 0;
	for(i = 0; i < d->sp; i++)
	{
		if(i)
			strcat(out, ", ");
		strcat(out, d->stack[i].text);
		free(d->stack[i].text);
		if(d->stack[i].startOff < d->popMinOff)
			d->popMinOff = d->stack[i].startOff;
	}
	d->sp = 0;
	return out;
}

/* the number of parameters of a function: of this file (counted from its
 * prologue) or of an import (from the options); -1 when unknown */
static int FunctionArity(const Dec_t* d, const char* name)
{
	int i;
	for(i = 0; i < d->f->exportCount; i++)
		if(strcmp(d->f->exports[i].name, name) == 0)
		{
			uint32_t k = IndexAt(d, d->f->exports[i].off), n = 0;
			if(k == UINT32_MAX || !IsTok(d, k, SCN_PUSHFP) || !IsTok(d, k + 3, SCN_SETFP))
				return -1;
			k += 4;
			while(IsTok(d, k, SCN_LOCAL) && IsTok(d, k + 1, SCN_STORE2))
			{
				n++;
				k += 2;
			}
			return (int)n;
		}
	for(i = 0; i < d->importFuncCount; i++)
		if(strcmp(d->importFuncs[i].name, name) == 0)
			return d->importFuncs[i].params;
	return -1;
}

// the arguments of a function with `arity` parameters (-1: everything on the stack), in push order
static char* FunctionArgs(Dec_t* d, int arity, uint32_t off)
{
	if(arity < 0 || arity > d->sp)
		return AllArgsInOrder(d);
	(void)off;
	return ArgListInOrder(d, arity);
}

// the bytes the parameters take in the frame (4 each, 1 for a byte parameter)
static uint32_t ParamsSize(const Dec_t* d)
{
	uint32_t n = 0;
	int j;
	for(j = 0; j < d->paramCount; j++)
		n += d->paramByte[j] ? 1 : 4;
	return n;
}

// the name of the function at `off` (an export), or NULL
static const char* FunctionAt(const Dec_t* d, uint32_t off)
{
	int i;
	for(i = 0; i < d->f->exportCount; i++)
		if(d->f->exports[i].off == off)
			return d->f->exports[i].name;
	return NULL;
}

static const char* LabelAt(const Dec_t* d, uint32_t off)
{
	int i;
	for(i = 0; i < d->labelCount; i++)
		if(d->labels[i].off == off)
			return d->labels[i].name;
	return NULL;
}

/* `e` loaded with size `s`: a scalar local, or a deref.  The sugar for
 * indexing (`a[i]`) is applied when `e` is `base + i * k` with a local
 * array base and a matching element size. */
/* `base + i * K` loaded or stored with a matching size is `name[i]`: an
 * array of ints or chars (a local), or a pointer to them (a parameter).
 * The first indexed use decides the declaration; a use with another
 * element size keeps the explicit form. */
static char* IndexSugar(Dec_t* d, const Expr_t* e, int size)
{
	struct Local* l;
	char name[SCN_NAME_MAX];
	int elem = (int)e->num;
	if(e->kind != E_INDEXADDR || !((elem == 4 && size == 2) || (elem == 1 && size == 0)))
		return NULL;
	l = LocalAt(d, e->aux, 1);
	if(!l)
		return NULL;
	if(e->baseIsValue != (ParamIndex(d, e->aux) >= 0))
		return NULL; // a local's address indexed as a pointer, or a parameter's address: not the plain forms
	if(l->elem == 0)
		l->elem = elem;
	else if(l->elem != elem)
		return NULL;
	if(!e->baseIsValue && l->scalar)
		return NULL; // also used as a scalar: keep the explicit form
	LocalName(d, e->aux, name, sizeof name);
	return Format("%s[%s]", name, e->index);
}

static void Deref(Dec_t* d, Expr_t e, int size, uint32_t off)
{
	char name[SCN_NAME_MAX];
	char* sugar;
	(void)off;
	if(e.kind == E_LOCALADDR)
	{
		struct Local* l = LocalAt(d, e.aux, 1);
		if(l)
			l->scalar = size == 0 ? 1 : 2;
		LocalName(d, e.aux, name, sizeof name);
		FreeExpr(&e);
		Push(d, Strdup(name), SCN_PREC_PRIMARY, E_LOADLOCAL, e.aux);
		return;
	}
	sugar = IndexSugar(d, &e, size);
	if(sugar)
	{
		FreeExpr(&e);
		Push(d, sugar, SCN_PREC_PRIMARY, E_OTHER, 0);
		return;
	}
	{
		char* t = Operand(&e, SCN_PREC_PRIMARY + 1); // always parenthesized
		FreeExpr(&e);
		Push(d, Format("*(%s*)%s", size == 0 ? "char" : "int", t), SCN_PREC_UNARY, E_OTHER, 0);
		free(t);
	}
}

static void BinaryOp(Dec_t* d, const ScnOperator_t* op, uint32_t off)
{
	Expr_t b = Pop(d, off);
	Expr_t a = Pop(d, off);
	char* ta;
	char* tb;
	// the library compiler's unary minus: x * 0xffffffff
	if(op->token == SCN_MUL && b.kind == E_NUM && b.num == 0xffffffffu && d->style == SCN_STYLE_LIBRARY)
	{
		ta = Operand(&a, SCN_PREC_UNARY);
		Push(d, Format("-%s", ta), SCN_PREC_UNARY, E_OTHER, 0);
		free(ta);
		FreeExpr(&a);
		FreeExpr(&b);
		return;
	}
	// the scenario compiler's: 0 - x
	if(op->token == SCN_SUB && a.kind == E_NUM && a.num == 0 && d->style == SCN_STYLE_SCENARIO)
	{
		tb = Operand(&b, SCN_PREC_UNARY);
		Push(d, Format("-%s", tb), SCN_PREC_UNARY, E_OTHER, 0);
		free(tb);
		FreeExpr(&a);
		FreeExpr(&b);
		return;
	}
	ta = Operand(&a, op->prec);
	tb = Operand(&b, op->prec + 1);
	Push(d, Format("%s %s %s", ta, op->text, tb), op->prec, E_OTHER, 0);
	// the shapes of indexing: `i * K` and `base + i * K`
	if(op->token == SCN_MUL && b.kind == E_NUM && (b.num == 1 || b.num == 4))
	{
		Expr_t* r = &d->stack[d->sp - 1];
		r->kind = E_SCALED;
		r->aux = (uint32_t)b.num;
		r->index = Strdup(a.text);
	}
	else if(op->token == SCN_ADD && b.kind == E_SCALED && (a.kind == E_LOCALADDR || a.kind == E_LOADLOCAL))
	{
		Expr_t* r = &d->stack[d->sp - 1];
		r->kind = E_INDEXADDR;
		r->aux = a.aux;
		r->num = b.aux;
		r->index = b.index;
		r->baseIsValue = a.kind == E_LOADLOCAL;
		b.index = NULL;
	}
	free(ta);
	free(tb);
	FreeExpr(&a);
	FreeExpr(&b);
}

/* Run the stack machine from instruction `i` until a token that ends an
 * expression (a store, a jump, a line marker, ..).  Returns the index of
 * that token.  `stmtStart` is where the statement began (for the
 * scenario style's builtins). */
static int IsJump(const Dec_t* d, uint32_t i, uint32_t* target);

static uint32_t ParseExpr(Dec_t* d, uint32_t i, uint32_t end)
{
	uint32_t dummyTarget;
	for(; i < end && !d->failed; i++)
	{
		const Insn_t* in = &d->insn[i];
		uint32_t t = in->tok;
		const ScnOperator_t* op;
		int lineNo;
		if(d->style == SCN_STYLE_SCENARIO && LineMarker(d, i, &lineNo))
			return i;
		d->curOff = in->off;
		d->popMinOff = UINT32_MAX;
		switch(t)
		{
			case SCN_PUSH: PushNum(d, in->ops[0]); break;
			case SCN_STR:
			{
				char* lit = StringLiteral(d, in->ops[0]);
				if(!lit)
				{
					Fail(d, in->off, "string offset out of range");
					return i;
				}
				Push(d, lit, SCN_PREC_PRIMARY, E_STR, in->ops[0]);
				break;
			}
			case SCN_LOCAL:
			{
				char name[SCN_NAME_MAX];
				LocalAt(d, in->ops[0], 1);
				LocalName(d, in->ops[0], name, sizeof name);
				Push(d, Format("&%s", name), SCN_PREC_UNARY, E_LOCALADDR, in->ops[0]);
				break;
			}
			case SCN_ADDR:
			{
				const char* fn = FunctionAt(d, in->ops[0]);
				const char* lb = fn ? NULL : LabelAt(d, in->ops[0]);
				Push(d, Format("%s", fn ? fn : lb ? lb
												  : "?"),
					SCN_PREC_PRIMARY, E_ADDR, in->ops[0]);
				break;
			}
			case SCN_LOAD:
			{
				Expr_t e = Pop(d, in->off);
				Deref(d, e, (int)in->ops[0], in->off);
				break;
			}
			case SCN_NOT:
			case SCN_LNOT:
			{
				Expr_t e = Pop(d, in->off);
				char* te = Operand(&e, SCN_PREC_UNARY);
				Push(d, Format("%s%s", t == SCN_NOT ? "~" : "!", te), SCN_PREC_UNARY, E_OTHER, 0);
				free(te);
				free(e.text);
				break;
			}
			case SCN_ARGS: d->pendingArgs = (int)in->ops[0]; break;
			case SCN_CALLFN:
			{
				Expr_t name = Pop(d, in->off);
				char* args;
				const char* s;
				if(name.kind != E_STR || !(s = Scn_String(d->f, name.aux)))
				{
					Fail(d, in->off, "callfn without a name string");
					free(name.text);
					return i;
				}
				args = FunctionArgs(d, FunctionArity(d, s), in->off);
				Push(d, Format("%s(%s)", s, args), SCN_PREC_PRIMARY, E_CALL, 0);
				free(args);
				free(name.text);
				break;
			}
			case SCN_CALL:
			{
				Expr_t a = Pop(d, in->off);
				char* args;
				const char* fn;
				if(a.kind != E_ADDR || !(fn = FunctionAt(d, a.aux)))
				{
					// `call 0` after set_label is the scenario style's local call: handled by the statement parser
					d->sp++;
					return i;
				}
				args = FunctionArgs(d, FunctionArity(d, fn), in->off);
				Push(d, Format("%s(%s)", fn, args), SCN_PREC_PRIMARY, E_CALL, 0);
				free(args);
				free(a.text);
				break;
			}
			default:
				op = ScnOp_ByToken(t);
				if(op)
				{
					BinaryOp(d, op, in->off);
					break;
				}
				if(t == SCN_RAWCALL && d->raw && d->sp >= 1 && d->stack[d->sp - 1].kind == E_NUM && d->pendingArgs < 0)
				{ /* the raw variant's call of a function of the main file: the address on top, the arguments
				   * below it (as many as the function takes when its file is at hand, else all of them) */
					Expr_t a = Pop(d, in->off);
					char name[32];
					char* args;
					snprintf(name, sizeof name, "@0x%x", (unsigned)a.num);
					args = FunctionArgs(d, FunctionArity(d, name), in->off);
					Push(d, Format("%s(%s)", name, args), SCN_PREC_PRIMARY, E_CALL, 1);
					free(args);
					free(a.text);
					return i + 1; // a statement of its own
				}
				if(IsCommandToken(t) && t != SCN_LINEINFO)
				{
					const ScnCmd_t* c = Command(d, t);
					char name[SCN_NAME_MAX];
					char* args;
					int count, result;
					if(t == SCN_MSG_TOKEN && d->style == SCN_STYLE_SCENARIO && !d->lineMarkers && d->pendingArgs < 0)
						return i; // the message statement
					if(t == 0xe8 && d->style == SCN_STYLE_SCENARIO && d->pendingArgs < 0)
					{
						// lstr(n): the scenario compiler's string variable reference, no args token
						Expr_t n = Pop(d, in->off);
						Push(d, Format("lstr(%s)", n.text), SCN_PREC_PRIMARY, E_CALL, 0);
						free(n.text);
						break;
					}
					if(d->style == SCN_STYLE_SCENARIO && d->pendingArgs < 0 && (t == 0xf6 || t == 0xf0 || t == 0xf3))
						return i; // the jump builtins are statements of their own
					CommandName(d, t, name, sizeof name);
					if(d->style == SCN_STYLE_SCENARIO && d->pendingArgs < 0)
						strcat(name, "!"); // the direct form (no args token) in a scenario file
					if(d->pendingArgs >= 0)
						count = d->pendingArgs;
					else if(d->raw)
						count = d->sp >= 1 ? 1 : 0; // the raw variant: no `args` means at most one argument
					else if(c && c->args >= 0)
						count = c->args;
					else
						count = d->sp;
					if(count > d->sp)
					{
						Fail(d, in->off, "%s needs %d arguments, %d on the stack", name, count, d->sp);
						return i;
					}
					/* the library style leaves nothing on the stack at the end of a
					 * statement: a command there takes everything (the table's
					 * count is short when the handler pops in a subroutine) */
					if(d->style == SCN_STYLE_LIBRARY && d->pendingArgs < 0 && count < d->sp && !(c && c->result) && (i + 1 >= end || d->insn[i + 1].tok == SCN_LINE || IsJump(d, i + 1, &dummyTarget) || d->insn[i + 1].tok == SCN_ENDTRY))
						count = d->sp;
					// a builtin's name under the args form would compile to the builtin: write the number instead
					if(d->pendingArgs >= 0 && ((t == SCN_MSG_TOKEN && !d->lineMarkers && !d->raw) || t == 0xe8))
						snprintf(name, sizeof name, "cmd_0x%x", t);
					args = d->pendingArgs >= 0 ? ArgListInOrder(d, count) : ArgList(d, count, in->off);
					d->pendingArgs = -1;
					result = c ? c->result : 0;
					if(!c || c->args < 0)
					{
						// not in the table (or there without its pops and pushes): it has a result when the expression goes on after it
						const Insn_t* nx = At(d, i + 1);
						int l2;
						result = !(i + 1 >= end || nx->tok == SCN_LINE || IsJump(d, i + 1, &dummyTarget) || nx->tok == SCN_ENDTRY || nx->tok == SCN_RET || nx->tok == SCN_SRCINFO || LineMarker(d, i + 1, &l2) || (IsCommandToken(nx->tok) && !(d->raw && d->sourceStrOff != UINT32_MAX)));
						// (the raw variant with markers between its statements: a command right after another takes its value)
					}
					{
						char* call = Format("%s(%s)", name, args);
						free(args);
						if(result)
						{
							Push(d, call, SCN_PREC_PRIMARY, E_CALL, 0);
							break;
						}
						// a statement of its own: the expression ends here, the statement parser flushes it
						Push(d, call, SCN_PREC_PRIMARY, E_CALL, 1);
						return i + 1;
					}
				}
				return i; // a terminator
		}
	}
	return i;
}

// ---- statements ------------------------------------------------------------------------------------

static uint32_t ParseStmts(Dec_t* d, uint32_t i, uint32_t end, uint32_t switchTemp, int* stoppedAtCase);
static uint32_t ParseStmt(Dec_t* d, uint32_t i, uint32_t end, uint32_t switchTemp, int* stoppedAtCase);

// the line marker at i: library `line F, L`; scenario `str F; push L; args 2; lineinfo`.  Returns the token count, 0 when none.
static uint32_t LineMarker(const Dec_t* d, uint32_t i, int* line)
{
	const Insn_t* in = At(d, i);
	if(d->style == SCN_STYLE_LIBRARY || d->lineMarkers)
	{
		if(in->tok == SCN_LINE)
		{
			*line = (int)in->ops[1];
			return 1;
		}
		return 0;
	}
	if(in->tok == SCN_STR && IsTok(d, i + 1, SCN_PUSH) && IsTok(d, i + 2, SCN_ARGS) && d->insn[i + 2].ops[0] == 2 && IsTok(d, i + 3, SCN_LINEINFO) && in->ops[0] == d->sourceStrOff)
	{
		*line = (int)d->insn[i + 1].ops[0];
		return 4;
	}
	return 0;
}

static int IsBareLine(const Dec_t* d, uint32_t i, uint32_t end)
{
	int line;
	uint32_t k = LineMarker(d, i, &line);
	if(!k || d->style != SCN_STYLE_LIBRARY)
		return 0;
	return i + k >= end || LineMarker(d, i + k, &line) || IsTok(d, i + k, SCN_ENDTRY);
}

// `addr X; jmp` at i
static int IsJump(const Dec_t* d, uint32_t i, uint32_t* target)
{
	if(IsTok(d, i, SCN_ADDR) && IsTok(d, i + 1, SCN_JMP))
	{
		*target = d->insn[i].ops[0];
		return 1;
	}
	return 0;
}

// the case test `push K; local T; load 2; eq; addr N; jcc 0` at i
static int IsCaseTest(const Dec_t* d, uint32_t i, uint32_t temp, uint32_t* value, uint32_t* next)
{
	if(IsTok(d, i, SCN_PUSH) && IsTok(d, i + 1, SCN_LOCAL) && d->insn[i + 1].ops[0] == temp && IsTok(d, i + 2, SCN_LOAD) && d->insn[i + 2].ops[0] == 2 && IsTok(d, i + 3, SCN_EQ) && IsTok(d, i + 4, SCN_ADDR) && IsTok(d, i + 5, SCN_JCC) && d->insn[i + 5].ops[0] == 0)
	{
		*value = d->insn[i].ops[0];
		*next = d->insn[i + 4].ops[0];
		return 1;
	}
	return 0;
}

/* a case label at i: `[line] addr B; jmp` with B the end of a test of
 * `temp` that follows, or B right after the jmp (default).  Returns the
 * token count of the label and its test, 0 when it is not one. */
static uint32_t IsCaseLabel(const Dec_t* d, uint32_t i, uint32_t temp, int* line, uint32_t* value, int* isDefault)
{
	uint32_t k = LineMarker(d, i, line), target, next;
	if(d->style == SCN_STYLE_LIBRARY && !k)
		return 0;
	if(!IsJump(d, i + k, &target))
		return 0;
	if(IsCaseTest(d, i + k + 2, temp, value, &next) && At(d, i + k + 8)->off == target)
	{
		*isDefault = 0;
		return k + 8;
	}
	if(At(d, i + k + 2)->off == target)
	{
		*isDefault = 1;
		return k + 2;
	}
	return 0;
}

static void FlushStatements(Dec_t* d, int line, uint32_t off)
{
	// every value left on the stack is an expression statement, bottom first
	int i;
	for(i = 0; i < d->sp; i++)
		AddEntry(d, line, ENT_TEXT, off, Format("%s;", d->stack[i].text));
	for(i = 0; i < d->sp; i++)
		free(d->stack[i].text);
	d->sp = 0;
}

/* values under the `keep` the consumer needs: calls whose result nothing
 * used are statements of their own (the scenario style puts no marker
 * between a call and the condition that follows it) */
static void FlushUnder(Dec_t* d, int keep, int line, uint32_t off)
{
	int i, extra = d->sp - keep;
	(void)line;
	(void)off;
	if(extra <= 0)
		return;
	for(i = 0; i < extra; i++)
		if(d->stack[i].kind != E_CALL)
			return;
	for(i = 0; i < extra; i++)
	{ // the first statement starts at the marker, the next ones where their expressions begin (a label may point there)
		AddEntry(d, i ? 0 : d->stmtLine, ENT_TEXT, i ? d->stack[i].startOff : d->stmtOff, Format("%s;", d->stack[i].text));
		free(d->stack[i].text);
	}
	memmove(d->stack, d->stack + extra, (size_t)keep * sizeof d->stack[0]);
	d->sp = keep;
}

static void AddJump(Dec_t* d, int line, uint32_t off, uint32_t target)
{
	Entry_t* e = AddEntry(d, line, ENT_JUMP, off, NULL);
	e->jumpTarget = target;
}

// the lvalue text of a store address expression
static char* Lvalue(Dec_t* d, Expr_t addr, int size)
{
	char name[SCN_NAME_MAX];
	char* t;
	char* out;
	if(addr.kind == E_LOCALADDR)
	{
		struct Local* l = LocalAt(d, addr.aux, 1);
		if(l)
			l->scalar = size == 0 ? 1 : 2;
		LocalName(d, addr.aux, name, sizeof name);
		return Strdup(name);
	}
	out = IndexSugar(d, &addr, size);
	if(out)
		return out;
	t = Operand(&addr, SCN_PREC_PRIMARY + 1);
	out = Format("*(%s*)%s", size == 0 ? "char" : "int", t);
	free(t);
	return out;
}

/* The if / while at the jcc of instruction `i` (the condition is on the
 * stack, the target in `target`).  Returns the index after the whole
 * construct. */
static uint32_t ParseIf(Dec_t* d, uint32_t i, uint32_t end, int line, uint32_t condStart, uint32_t target, uint32_t switchTemp,
	char* condText)
{
	uint32_t xi = IndexAt(d, target), k, jt, bodyEnd, elseEnd;
	Expr_t cond;
	int closeLine = 0;
	uint32_t condOff = d->insn[condStart].off; // where the construct starts: the condition, after whatever was flushed before it
	if(condText)
	{
		// the condition was an assignment (the original source's `if (x = v)`): nothing is on the stack
		cond.text = condText;
		cond.prec = 0;
		cond.kind = E_OTHER;
		cond.aux = 0;
		cond.num = 0;
	}
	else
	{
		int flushed = d->sp > 1;
		FlushUnder(d, 1, line, d->insn[condStart].off);
		if(d->sp != 1)
		{
			Fail(d, d->insn[i].off, "a condition with %d values", d->sp);
			return end;
		}
		cond = Pop(d, 0);
		if(flushed && cond.startOff > condOff)
			condOff = cond.startOff;
	}
	if(xi == UINT32_MAX || xi > end || xi < i + 1)
	{
		Fail(d, d->insn[i].off, "bad branch target");
		free(cond.text);
		return end;
	}
	// a loop: the range ends with `[line] addr COND; jmp`
	if(xi >= i + 3 && IsJump(d, xi - 2, &jt) && jt == condOff)
	{
		bodyEnd = xi - 2;
		if(d->style == SCN_STYLE_LIBRARY)
		{
			if(bodyEnd > i + 1 && LineMarker(d, bodyEnd - 1, &closeLine))
				bodyEnd--;
			else
			{
				Fail(d, d->insn[i].off, "a loop without a closing line marker");
				free(cond.text);
				return end;
			}
		}
		AddEntry(d, line, ENT_OPEN, condOff, Format("while (%s) {", cond.text));
		free(cond.text);
		PushCtx(d, target, condOff);
		d->depth++;
		ParseStmts(d, i + 1, bodyEnd, 0, NULL);
		d->depth--;
		PopCtx(d);
		AddEntry(d, closeLine, ENT_CLOSE, d->insn[bodyEnd].off, Strdup("}"));
		return xi;
	}
	// an if: the then-range ends with `[line] addr E; jmp`
	if(xi < i + 3 || !IsJump(d, xi - 2, &jt) || jt < target)
	{
		Fail(d, d->insn[i].off, "an if without the jump over its else part");
		free(cond.text);
		return end;
	}
	bodyEnd = xi - 2;
	if(d->style == SCN_STYLE_LIBRARY)
	{
		if(bodyEnd > i + 1 && LineMarker(d, bodyEnd - 1, &closeLine))
			bodyEnd--;
		else
		{
			Fail(d, d->insn[i].off, "an if without a closing line marker");
			free(cond.text);
			return end;
		}
	}
	AddEntry(d, line, ENT_OPEN, condOff, Format("if (%s) {", cond.text));
	free(cond.text);
	d->depth++;
	ParseStmts(d, i + 1, bodyEnd, switchTemp, NULL);
	d->depth--;
	if(jt == target)
	{
		AddEntry(d, closeLine, ENT_CLOSE, d->insn[bodyEnd].off, Strdup("}"));
		return xi;
	}
	// the else part [xi, elseEnd)
	elseEnd = IndexAt(d, jt);
	if(elseEnd == UINT32_MAX || elseEnd > end)
	{
		Fail(d, d->insn[i].off, "bad else end (0x%x, range end %u, else end %u)", jt, end, elseEnd);
		return end;
	}
	{
		int elseClose = 0, hasClose = 0;
		uint32_t elseBodyEnd = elseEnd;
		int before;
		if(d->style == SCN_STYLE_LIBRARY && elseEnd > xi && IsBareLine(d, elseEnd - 1, elseEnd))
		{
			LineMarker(d, elseEnd - 1, &elseClose);
			hasClose = 1;
			elseBodyEnd = elseEnd - 1;
		}
		before = d->entCount;
		AddEntry(d, closeLine, ENT_CLOSE, d->insn[bodyEnd].off, Strdup("} else {"));
		d->depth++;
		k = ParseStmts(d, xi, elseBodyEnd, switchTemp, NULL);
		d->depth--;
		(void)k;
		if(!hasClose && d->style == SCN_STYLE_LIBRARY)
		{
			// the library compiler writes a marker for every closing brace: there is no `else if` form
			Fail(d, d->insn[xi].off, "an else part without a closing line marker");
			return end;
		}
		if(d->style == SCN_STYLE_SCENARIO)
		{
			// no markers: an else that is a single if is written as a chain too
			if(d->entCount > before + 1 && d->ent[before + 1].kind == ENT_OPEN && strncmp(d->ent[before + 1].text, "if (", 4) == 0)
			{
				int j, anyOuter = 0;
				for(j = before + 2; j < d->entCount; j++)
					if(d->ent[j].depth == d->ent[before + 1].depth && d->ent[j].kind != ENT_CLOSE)
						anyOuter = 1;
				if(!anyOuter)
				{
					char* merged = Format("} else %s", d->ent[before + 1].text);
					free(d->ent[before].text);
					free(d->ent[before + 1].text);
					d->ent[before].text = merged;
					d->ent[before].kind = ENT_OPEN;
					if(d->lineMarkers)
						d->ent[before].line = d->ent[before + 1].line; // the inner if's marker names the line

					memmove(&d->ent[before + 1], &d->ent[before + 2], (size_t)(d->entCount - before - 2) * sizeof *d->ent);
					d->entCount--;
					for(j = before + 1; j < d->entCount; j++)
						d->ent[j].depth--;
					return elseEnd;
				}
			}
			AddEntry(d, 0, ENT_CLOSE, d->insn[elseEnd - 1].off + 4, Strdup("}"));
		}
		else
			AddEntry(d, elseClose, ENT_CLOSE, d->insn[elseBodyEnd].off, Strdup("}"));
	}
	return elseEnd;
}

// whether some `addr X; jmp` of [a, b) targets `target`
static int JumpsTo(const Dec_t* d, uint32_t a, uint32_t b, uint32_t target)
{
	uint32_t i, t;
	for(i = a; i + 1 < b; i++)
		if(IsJump(d, i, &t) && t == target)
			return 1;
	return 0;
}

/* the switch whose temporary is `temp`, its first case test at `i`
 * (its `line` marker already consumed) */
static uint32_t ParseSwitch(Dec_t* d, uint32_t i, uint32_t end, int firstCaseLine, uint32_t temp, int leadingDefault)
{
	uint32_t value, next, k, switchStart = i;
	int ctx;
	int stopped = 0;
	if(leadingDefault)
	{
		// `default:` as the first case: its marker alone, no test; 2: statements before any label, i at the first
		next = UINT32_MAX;
		ctx = PushCtx(d, UINT32_MAX, UINT32_MAX);
		d->ctx[ctx].switchTemp = temp;
		d->ctx[ctx].start = i;
		d->depth++;
		if(leadingDefault == 1)
			AddEntry(d, firstCaseLine, ENT_TEXT, d->insn[i - 1].off, Strdup("default:"));
	}
	else
	{
		if(!IsCaseTest(d, i, temp, &value, &next))
		{
			Fail(d, d->insn[i].off, "switch without a first case");
			return end;
		}
		ctx = PushCtx(d, UINT32_MAX, UINT32_MAX);
		d->ctx[ctx].switchTemp = temp;
		d->ctx[ctx].start = i;
		d->depth++;
		AddEntry(d, firstCaseLine, ENT_TEXT, d->insn[i].off, Format("case %u:", value));
		i += 6;
	}
	for(;;)
	{
		int line = 0, isDefault = 0;
		d->depth++;
		i = ParseStmts(d, i, end, temp, &stopped);
		d->depth--;
		if(d->failed)
			break;
		if(stopped == 1)
		{
			k = IsCaseLabel(d, i, temp, &line, &value, &isDefault);
			AddEntry(d, line, ENT_TEXT, d->insn[i].off, isDefault ? Strdup("default:") : Format("case %u:", value));
			if(isDefault)
				next = UINT32_MAX;
			else
				IsCaseTest(d, i + k - 6, temp, &value, &next);
			i += k;
			continue;
		}
		if(stopped == 2)
		{
			int l;
			k = LineMarker(d, i, &l);
			/* a bare line where the last test's failure lands right after it
			 * is a `default:` in last position - unless a break of the switch
			 * jumps there too, which makes it the closing brace */
			if(next != UINT32_MAX && At(d, i + k)->off == next && !JumpsTo(d, switchStart, i, next))
			{
				AddEntry(d, l, ENT_TEXT, d->insn[i].off, Strdup("default:"));
				next = UINT32_MAX;
				i += k;
				continue;
			}
			d->depth--;
			AddEntry(d, l, ENT_CLOSE, d->insn[i].off, Strdup("}"));
			d->depth++;
			i += k;
		}
		else
		{
			d->depth--;
			AddEntry(d, 0, ENT_CLOSE, i < d->n ? d->insn[i].off : d->f->codeLen * 4, Strdup("}"));
			d->depth++;
		}
		break;
	}
	d->depth--;
	d->ctx[ctx].breakTarget = i < d->n ? d->insn[i].off : d->f->codeLen * 4;
	if(d->verbose > 1)
		fprintf(stderr, "    switch (temp 0x%x) from 0x%x ends at 0x%x (ctx %d, stopped %d)\n", temp, d->insn[switchStart].off,
			d->ctx[ctx].breakTarget, ctx, stopped);
	PopCtx(d);
	return i;
}

/* the scenario style's message statement: `[srcinfo] values.. str text;
 * msg` with no args token: all values are its arguments */
static uint32_t ParseMsg(Dec_t* d, uint32_t i, int line, uint32_t stmtOff)
{
	const ScnCmd_t* c = Command(d, SCN_MSG_TOKEN);
	char* args;
	if(c && c->args > 0 && c->args <= d->sp)
		FlushUnder(d, c->args, line, stmtOff);
	args = ArgList(d, d->sp, d->insn[i].off); // the direct form: the text on top is the first argument
	AddEntry(d, line, ENT_TEXT, stmtOff, Format("msg(%s);", args));
	free(args);
	return i + 1;
}

/* the jump builtins of the scenario style: `str S; (str L | push 0);
 * set_label; jump_scenario|call_scenario` and the local call `str L;
 * set_label; addr 0; call` */
static uint32_t ParseJumpBuiltin(Dec_t* d, uint32_t i, int line, uint32_t stmtOff)
{
	const Insn_t* in = &d->insn[i]; // the set_label token
	Expr_t label, script;
	if(IsTok(d, i + 1, 0xf3) || IsTok(d, i + 1, 0xf0))
		FlushUnder(d, 2, line, stmtOff);
	else
		FlushUnder(d, 1, line, stmtOff);
	if(d->sp == 2 && IsTok(d, i + 1, 0xf3))
	{
		label = Pop(d, 0);
		script = Pop(d, 0);
		if(label.kind == E_NUM && label.num == 0)
			AddEntry(d, line, ENT_TEXT, stmtOff, Format("jump %s;", script.text));
		else
			AddEntry(d, line, ENT_TEXT, stmtOff, Format("jump %s, %s;", script.text, label.text));
		free(label.text);
		free(script.text);
		return i + 2;
	}
	if(d->sp == 2 && IsTok(d, i + 1, 0xf0))
	{
		label = Pop(d, 0);
		script = Pop(d, 0);
		if(label.kind == E_NUM && label.num == 0)
			AddEntry(d, line, ENT_TEXT, stmtOff, Format("call %s;", script.text));
		else
			AddEntry(d, line, ENT_TEXT, stmtOff, Format("call %s, %s;", script.text, label.text));
		free(label.text);
		free(script.text);
		return i + 2;
	}
	if(d->sp == 1 && IsTok(d, i + 1, SCN_ADDR) && d->insn[i + 1].ops[0] == 0 && IsTok(d, i + 2, SCN_CALL))
	{
		const char* s;
		char utf[SCN_NAME_MAX * 3];
		label = Pop(d, 0);
		s = label.kind == E_STR ? Scn_String(d->f, label.aux) : NULL;
		if(!s || strlen(s) * 3 >= sizeof utf)
		{
			Fail(d, in->off, "a local call without a label name");
			free(label.text);
			return i;
		}
		OS_SjisToUtf8(s, utf, sizeof utf);
		AddEntry(d, line, ENT_TEXT, stmtOff, Format("call %s;", utf));
		free(label.text);
		return i + 3;
	}
	Fail(d, in->off, "unrecognized set_label use");
	return i;
}

/* The scenario style's string variable assignment `lstr(N) = e1 + e2 ..`:
 * a 256-byte buffer is opened on the frame (push_fp; push 0x100; add;
 * set_fp), every item is copied into it - the first with `local 0x100;
 * ITEM; strcpy_r`, the next ones appended with `local 0x100; local
 * 0x100; strlen; add; ITEM; strcpy_r` -, the buffer is copied into the
 * variable (push N; lstr; local 0x100; strcpy_r) and the frame is closed
 * (push_fp; push 0x100; sub; set_fp).  An item is a literal (str S),
 * another variable (push M; lstr) or a number (push V), which the
 * original compiler pushes without a copy. */
static int LstrItem(Dec_t* d, uint32_t* pi, char** text)
{
	uint32_t i = *pi;
	if(IsTok(d, i, SCN_STR))
	{
		*text = StringLiteral(d, d->insn[i].ops[0]);
		if(!*text)
			return 0;
		if(!IsTok(d, i + 1, 0x48))
		{
			free(*text);
			return 0;
		}
		*pi = i + 2;
		return 1;
	}
	if(IsTok(d, i, SCN_PUSH) && IsTok(d, i + 1, 0xe8) && IsTok(d, i + 2, 0x48))
	{
		*text = Format("lstr(%u)", d->insn[i].ops[0]);
		*pi = i + 3;
		return 1;
	}
	if(IsTok(d, i, SCN_PUSH))
	{
		*text = Format("%u", d->insn[i].ops[0]);
		*pi = i + 1;
		return 1;
	}
	return 0;
}

static uint32_t ParseLstrSet(Dec_t* d, uint32_t i, int line, uint32_t stmtOff)
{
	char* expr = NULL;
	char* item;
	uint32_t start = i;
	if(!(IsTok(d, i, SCN_PUSHFP) && IsTok(d, i + 1, SCN_PUSH) && d->insn[i + 1].ops[0] == 0x100 && IsTok(d, i + 2, SCN_ADD) && IsTok(d, i + 3, SCN_SETFP) && IsTok(d, i + 4, SCN_LOCAL) && d->insn[i + 4].ops[0] == 0x100))
	{
		Fail(d, d->insn[i].off, "unrecognized frame use");
		return i;
	}
	i += 5;
	if(!LstrItem(d, &i, &expr))
	{
		Fail(d, d->insn[start].off, "unrecognized string assignment");
		return start;
	}
	// the appended items
	while(IsTok(d, i, SCN_LOCAL) && d->insn[i].ops[0] == 0x100 && IsTok(d, i + 1, SCN_LOCAL) && d->insn[i + 1].ops[0] == 0x100 && IsTok(d, i + 2, 0x94) && IsTok(d, i + 3, SCN_ADD))
	{
		char* joined;
		i += 4;
		if(!LstrItem(d, &i, &item))
		{
			free(expr);
			Fail(d, d->insn[start].off, "unrecognized string assignment");
			return start;
		}
		joined = Format("%s + %s", expr, item);
		free(expr);
		free(item);
		expr = joined;
	}
	if(!(IsTok(d, i, SCN_PUSH) && IsTok(d, i + 1, 0xe8) && IsTok(d, i + 2, SCN_LOCAL) && d->insn[i + 2].ops[0] == 0x100 && IsTok(d, i + 3, 0x48) && IsTok(d, i + 4, SCN_PUSHFP) && IsTok(d, i + 5, SCN_PUSH) && d->insn[i + 5].ops[0] == 0x100 && IsTok(d, i + 6, SCN_SUB) && IsTok(d, i + 7, SCN_SETFP)))
	{
		free(expr);
		Fail(d, d->insn[start].off, "unrecognized string assignment");
		return start;
	}
	AddEntry(d, line, ENT_TEXT, stmtOff, Format("lstr(%u) = %s;", d->insn[i].ops[0], expr));
	free(expr);
	return i + 8;
}

static uint32_t ParseStmt(Dec_t* d, uint32_t i, uint32_t end, uint32_t switchTemp, int* stoppedAtCase)
{
	int line = 0;
	uint32_t k, stmtOff, condStart, target;
	const Insn_t* in;
	if(stoppedAtCase)
		*stoppedAtCase = 0;
	stmtOff = d->insn[i].off;
	k = LineMarker(d, i, &line);
	i += k;
	ResetStack(d);
	d->stmtOff = stmtOff;
	d->stmtLine = line;
	if(d->verbose > 1)
		fprintf(stderr, "    stmt at 0x%x line %d depth %d tok 0x%x\n", stmtOff, line, d->depth, i < d->n ? d->insn[i].tok : 0);
	if(i >= end)
	{
		if(k)
			AddEntry(d, line, ENT_TEXT, stmtOff, Strdup(""));
		return i;
	}
	in = &d->insn[i];
	// a marker followed by another: an empty statement (the scenario style; a bare line of the library style is a brace)
	if(k && d->style == SCN_STYLE_SCENARIO)
	{
		int l2;
		if(LineMarker(d, i, &l2))
		{
			AddEntry(d, line, ENT_TEXT, stmtOff, Strdup(";"));
			return i;
		}
	}
	// a jump statement
	if(IsJump(d, i, &target))
	{
		AddJump(d, line, stmtOff, target);
		return i + 2;
	}
	// the scenario style's goto: `str NAME; addr LABEL; jmp` (the name is left on the stack)
	if(d->style == SCN_STYLE_SCENARIO && !d->lineMarkers && in->tok == SCN_STR && IsJump(d, i + 1, &target))
	{
		const char* s = Scn_String(d->f, in->ops[0]);
		int li;
		for(li = 0; s && li < d->labelCount; li++)
			if(d->labels[li].off == target && strcmp(d->labels[li].sjis, s) == 0)
			{
				AddEntry(d, line, ENT_TEXT, stmtOff, Format("goto %s;", d->labels[li].name));
				return i + 3;
			}
	}
	// the older variant's goto: `str NAME; set_label; addr 0; jmp` (through the dispatcher)
	if(d->lineMarkers && in->tok == SCN_STR && IsTok(d, i + 1, 0xf6) && IsJump(d, i + 2, &target) && target == 0)
	{
		const char* s = Scn_String(d->f, in->ops[0]);
		int li;
		for(li = 0; s && li < d->labelCount; li++)
			if(strcmp(d->labels[li].sjis, s) == 0)
			{
				AddEntry(d, line, ENT_TEXT, stmtOff, Format("goto %s;", d->labels[li].name));
				return i + 4;
			}
	}
	// the scenario style's explicit end
	if(in->tok == SCN_RET && d->style == SCN_STYLE_SCENARIO)
	{
		AddEntry(d, line, ENT_TEXT, stmtOff, Strdup("end;"));
		return i + 1;
	}
	if(in->tok == SCN_PUSHFP && d->style == SCN_STYLE_SCENARIO)
		return ParseLstrSet(d, i, line, stmtOff);
	if(in->tok == SCN_SRCINFO)
	{
		if(in->ops[0] != SCN_SRCINFO_MAGIC || in->ops[1] != (uint32_t)d->srcinfoIndex)
		{
			Fail(d, in->off, "unexpected srcinfo operands");
			return end;
		}
		d->srcinfoIndex++;
		i++;
	}
	condStart = i;
	i = ParseExpr(d, i, end);
	if(d->failed)
		return end;
	in = At(d, i);
	switch(in->tok)
	{
		case SCN_STORE:
		{
			Expr_t value, addr;
			char* lv;
			FlushUnder(d, 2, line, stmtOff);
			if(d->sp != 2)
			{
				Fail(d, in->off, "store with %d values", d->sp);
				return end;
			}
			value = Pop(d, 0);
			addr = Pop(d, 0);
			lv = Lvalue(d, addr, (int)in->ops[0]);
			// an assignment as a condition: `addr; value; store; addr X; jcc 0`
			if(IsTok(d, i + 1, SCN_ADDR) && IsTok(d, i + 2, SCN_JCC) && d->insn[i + 2].ops[0] == 0)
			{
				char* cond = Format("%s = %s", lv, value.text);
				free(lv);
				free(addr.text);
				free(value.text);
				return ParseIf(d, i + 2, end, line, condStart, d->insn[i + 1].ops[0], switchTemp, cond);
			}
			AddEntry(d, line, ENT_TEXT, stmtOff, Format("%s = %s;", lv, value.text));
			free(lv);
			free(addr.text);
			free(value.text);
			return i + 1;
		}
		case SCN_STORE2:
		{
			Expr_t addr, value;
			FlushUnder(d, 2, line, stmtOff);
			if(d->sp != 2)
			{
				Fail(d, in->off, "store2 with %d values", d->sp);
				return end;
			}
			addr = Pop(d, 0);
			value = Pop(d, 0);
			// a switch: the temporary is stored and the first case test (or a leading `default:`, a bare line) follows
			if(addr.kind == E_LOCALADDR && in->ops[0] == 2)
			{
				uint32_t v, nx;
				int cl, leadingDefault;
				uint32_t kk = LineMarker(d, i + 1, &cl);
				int bare = kk && IsBareLine(d, i + 1, end) && d->style == SCN_STYLE_LIBRARY;
				int empty = 0;
				/* a bare line right after the header is either the closing brace
				 * of an empty switch or a `default:` in first position.  Nested
				 * switches share the slot, so what follows does not tell them
				 * apart; what does is a break right after the brace that jumps
				 * where the enclosing switch's breaks go: that break is the
				 * enclosing switch's, so this one was empty. */
				if(bare)
				{
					uint32_t outerEnd = EnclosingSwitchEnd(d, addr.aux, i + 1), jt;
					int l2;
					uint32_t k2 = LineMarker(d, i + 1 + kk, &l2);
					if(outerEnd != UINT32_MAX && IsJump(d, i + 1 + kk + k2, &jt) && jt == outerEnd)
						empty = 1;
				}
				if(empty)
				{
					struct Local* l = LocalAt(d, addr.aux, 1);
					if(l)
						l->scalar = 2;
					if(addr.aux == d->frameSize)
						AddEntry(d, line, ENT_OPEN, stmtOff, Format("switch (%s) {", value.text));
					else
						AddEntry(d, line, ENT_OPEN, stmtOff, Format("switch @0x%x (%s) {", addr.aux, value.text));
					AddEntry(d, cl, ENT_CLOSE, d->insn[i + 1].off, Strdup("}"));
					free(addr.text);
					free(value.text);
					return i + 1 + kk;
				}
				leadingDefault = bare;
				/* a marker and a statement right after the header (not a test, not
				 * a bare line): a switch whose body starts before its first label -
				 * the original compiler takes that; nothing else stores the slot */
				if(kk && !bare && d->style == SCN_STYLE_LIBRARY && addr.aux == d->frameSize && !IsCaseTest(d, i + 1 + kk, addr.aux, &v, &nx))
					leadingDefault = 2;
				if(((d->style == SCN_STYLE_SCENARIO || kk) && IsCaseTest(d, i + 1 + kk, addr.aux, &v, &nx)) || leadingDefault)
				{
					struct Local* l = LocalAt(d, addr.aux, 1);
					if(l)
						l->scalar = 2;
					if(addr.aux == d->frameSize)
						AddEntry(d, line, ENT_OPEN, stmtOff, Format("switch (%s) {", value.text));
					else
						AddEntry(d, line, ENT_OPEN, stmtOff, Format("switch @0x%x (%s) {", addr.aux, value.text));
					free(addr.text);
					free(value.text);
					return ParseSwitch(d, leadingDefault == 2 ? i + 1 : i + 1 + kk, end, cl, addr.aux, leadingDefault);
				}
			}
			{
				char* lv = Lvalue(d, addr, (int)in->ops[0]);
				AddEntry(d, line, ENT_TEXT, stmtOff, Format("%s <- %s;", lv, value.text));
				free(lv);
			}
			free(addr.text);
			free(value.text);
			return i + 1;
		}
		case SCN_JCC:
			if(in->ops[0] != 0 || !IsTok(d, i - 1, SCN_ADDR) || d->sp < 2)
			{
				Fail(d, in->off, "unrecognized conditional jump");
				return end;
			}
			{
				Expr_t a = Pop(d, 0);
				target = a.aux;
				free(a.text);
			}
			return ParseIf(d, i, end, line, condStart, target, switchTemp, NULL);
		case SCN_SETRET:
		{
			Expr_t v;
			if(d->sp != 1 || !IsJump(d, i + 1, &target) || target != d->funcEnd)
			{
				Fail(d, in->off, "unrecognized return");
				return end;
			}
			v = Pop(d, 0);
			AddEntry(d, line, ENT_TEXT, stmtOff, Format("return %s;", v.text));
			free(v.text);
			return i + 3;
		}
		case SCN_MSG_TOKEN:
			if(d->style == SCN_STYLE_SCENARIO && !d->lineMarkers)
				return ParseMsg(d, i, line, stmtOff);
			break;
		case 0xf6:
			if(d->style == SCN_STYLE_SCENARIO)
				return ParseJumpBuiltin(d, i, line, stmtOff);
			break;
		default: break;
	}
	// the statement ended: every value is an expression statement
	if(i < end)
	{
		int ok = in->tok == SCN_LINE || IsJump(d, i, &target) || in->tok == SCN_ENDTRY || in->tok == SCN_RET || in->tok == SCN_SRCINFO || in->tok == SCN_PUSHFP;
		if(d->style == SCN_STYLE_SCENARIO || d->sourceStrOff == UINT32_MAX)
			ok = 1; // no markers (for control flow, or at all): the next statement may start with any expression token
		if(!ok)
		{
			Fail(d, in->off, "unrecognized token 0x%x in a statement", in->tok);
			return end;
		}
	}
	FlushStatements(d, line, stmtOff);
	return i;
}

/* statements of [i, end).  In a switch body (`switchTemp` set) parsing
 * also stops at a case label (*stoppedAtCase = 1) or at the switch's
 * closing bare line (2).  Returns where it stopped. */
static uint32_t ParseStmts(Dec_t* d, uint32_t i, uint32_t end, uint32_t switchTemp, int* stoppedAtCase)
{
	if(stoppedAtCase)
		*stoppedAtCase = 0;
	while(i < end && !d->failed)
	{
		uint32_t before = i;
		if(switchTemp)
		{
			int line, isDefault;
			uint32_t v;
			if(IsCaseLabel(d, i, switchTemp, &line, &v, &isDefault))
			{
				if(stoppedAtCase)
					*stoppedAtCase = 1;
				return i;
			}
			if(IsBareLine(d, i, end))
			{
				if(stoppedAtCase)
					*stoppedAtCase = 2;
				return i;
			}
			if(d->style == SCN_STYLE_SCENARIO && stoppedAtCase)
			{
				// no markers: the switch ends where a jump out of it would land; the caller decides
			}
		}
		i = ParseStmt(d, i, end, switchTemp, NULL);
		if(i == before)
		{
			Fail(d, d->insn[i].off, "no progress");
			break;
		}
	}
	return i;
}

// ---- functions (library style) -----------------------------------------------------------------------

/* resolve the jump entries of [first, entCount) against their contexts:
 * break, continue, return or goto (a label entry is inserted at the
 * target for a goto) */
static void ResolveJumps(Dec_t* d, int first)
{
	int i;
	for(i = first; i < d->entCount; i++)
	{
		Entry_t* e = &d->ent[i];
		int c;
		if(e->kind != ENT_JUMP)
			continue;
		e->kind = ENT_TEXT;
		if(e->jumpTarget == d->funcEnd && d->style == SCN_STYLE_LIBRARY)
		{
			e->text = Strdup("return;");
			continue;
		}
		for(c = e->ctx; c >= 0; c = d->ctx[c].parent)
		{
			if(d->ctx[c].breakTarget == e->jumpTarget)
			{
				e->text = Strdup("break;");
				break;
			}
			if(d->ctx[c].continueTarget == e->jumpTarget)
			{
				e->text = Strdup("continue;");
				break;
			}
		}
		if(e->text)
			continue;
		// a goto: label the target entry
		{
			int j, found = 0;
			for(j = first; j < d->entCount; j++)
				if(d->ent[j].off == e->jumpTarget && d->ent[j].kind != ENT_LABEL)
				{
					Entry_t lab;
					memset(&lab, 0, sizeof lab);
					lab.kind = ENT_LABEL;
					lab.off = e->jumpTarget;
					lab.depth = d->ent[j].depth;
					lab.ctx = -1;
					lab.text = Format("L_%x:", e->jumpTarget);
					if(d->entCount == d->entCap)
					{
						d->entCap *= 2;
						d->ent = (Entry_t*)realloc(d->ent, (size_t)d->entCap * sizeof *d->ent);
						e = &d->ent[i];
					}
					memmove(&d->ent[j + 1], &d->ent[j], (size_t)(d->entCount - j) * sizeof *d->ent);
					d->ent[j] = lab;
					d->entCount++;
					if(j <= i)
						e = &d->ent[++i];
					found = 1;
					break;
				}
			if(!found)
			{
				if(d->verbose)
					for(c = e->ctx; c >= 0; c = d->ctx[c].parent)
						fprintf(stderr, "      ctx %d: break 0x%x continue 0x%x\n", c, d->ctx[c].breakTarget, d->ctx[c].continueTarget);
				Fail(d, e->off, "a jump to no statement (0x%x)", e->jumpTarget);
				e->text = Format("goto L_%x;", e->jumpTarget);
				return;
			}
			e->text = Format("goto L_%x;", e->jumpTarget);
		}
	}
}

static int CompareLocals(const void* a, const void* b)
{
	uint32_t x = ((const struct Local*)a)->off, y = ((const struct Local*)b)->off;
	return x < y ? -1 : x > y;
}

/* the declarations of the locals seen in the body, in offset order with
 * fillers for what the frame holds but the body never touches */
static char* Declarations(Dec_t* d)
{
	char buf[0x4000];
	size_t n = 0;
	uint32_t prev = 0, localsEnd;
	int i;
	buf[0] = 0;
	localsEnd = d->frameSize - 4 - ParamsSize(d); // the highest local offset
	qsort(d->locals, (size_t)d->localCount, sizeof d->locals[0], CompareLocals);
	for(i = 0; i < d->localCount; i++)
	{
		struct Local* l = &d->locals[i];
		uint32_t size;
		char name[SCN_NAME_MAX];
		if(l->isParam || l->off > localsEnd)
			continue;
		size = l->off - prev;
		LocalName(d, l->off, name, sizeof name);
		if(l->scalar == 2 && size == 4)
			n += (size_t)snprintf(buf + n, sizeof buf - n, "int %s; ", name);
		else if(l->scalar == 1 && size == 1)
			n += (size_t)snprintf(buf + n, sizeof buf - n, "char %s; ", name);
		else if(l->scalar)
		{
			// a scalar at an offset whose gap is larger: pad below it first
			if(size > (uint32_t)(l->scalar == 2 ? 4 : 1))
				n += (size_t)snprintf(buf + n, sizeof buf - n, "char _pad%x[%u]; ", prev, size - (l->scalar == 2 ? 4 : 1));
			n += (size_t)snprintf(buf + n, sizeof buf - n, "%s %s; ", l->scalar == 2 ? "int" : "char", name);
		}
		else if(l->elem == 4 && size % 4 == 0)
			n += (size_t)snprintf(buf + n, sizeof buf - n, "int %s[%u]; ", name, size / 4);
		else
			n += (size_t)snprintf(buf + n, sizeof buf - n, "char %s[%u]; ", name, size);
		prev = l->off;
		if(n >= sizeof buf - 64)
			break;
	}
	if(localsEnd > prev)
		n += (size_t)snprintf(buf + n, sizeof buf - n, "char _pad%x[%u]; ", prev, localsEnd - prev);
	if(n && buf[n - 1] == ' ')
		buf[--n] = 0;
	return Strdup(buf);
}

// parse one function [start, end) of the library style; returns 1 on success
static int ParseFunction(Dec_t* d, const char* name, uint32_t start, uint32_t end)
{
	uint32_t i = IndexAt(d, start), ie = IndexAt(d, end), bodyStart, bodyEnd, target, k;
	int firstEntry = d->entCount, closeLine = 0, j, headerLine;
	char* params;
	char* decl;
	Entry_t header;
	if(i == UINT32_MAX || ie == UINT32_MAX)
	{
		Fail(d, start, "function boundary inside a token");
		return 0;
	}
	d->localCount = 0;
	d->paramCount = 0;
	d->funcName = name;
	d->funcStart = start;
	d->curCtx = -1;
	d->ctxCount = 0;
	ResetStack(d);
	// the file the function's line markers name (the first marker's; the raw variant's includes hold functions)
	d->funcFile = UINT32_MAX;
	for(k = i; k < ie; k++)
		if(d->insn[k].tok == SCN_LINE)
		{
			if(d->insn[k].ops[0] != d->sourceStrOff)
				d->funcFile = d->insn[k].ops[0];
			break;
		}
	// prologue: push_fp; push F; add; set_fp
	if(!(IsTok(d, i, SCN_PUSHFP) && IsTok(d, i + 1, SCN_PUSH) && IsTok(d, i + 2, SCN_ADD) && IsTok(d, i + 3, SCN_SETFP)))
	{
		Fail(d, start, "no function prologue");
		return 0;
	}
	d->frameSize = d->insn[i + 1].ops[0];
	i += 4;
	// the parameter pops: local n; store2 2 (store2 0: a byte parameter)
	while(IsTok(d, i, SCN_LOCAL) && IsTok(d, i + 1, SCN_STORE2) && (d->insn[i + 1].ops[0] == 2 || d->insn[i + 1].ops[0] == 0))
	{
		if(d->paramCount >= (int)(sizeof d->paramOff / sizeof d->paramOff[0]))
		{
			Fail(d, start, "too many parameters");
			return 0;
		}
		d->paramByte[d->paramCount] = d->insn[i + 1].ops[0] == 0;
		d->paramOff[d->paramCount++] = d->insn[i].ops[0];
		i += 2;
	}
	// addr END; try
	if(!(IsTok(d, i, SCN_ADDR) && IsTok(d, i + 1, SCN_TRY)))
	{
		Fail(d, start, "no try after the prologue");
		return 0;
	}
	target = d->insn[i].ops[0];
	d->funcEnd = target;
	bodyStart = i + 2;
	// the end: [line] endtry; push_fp; push F; sub; set_fp; ret
	k = IndexAt(d, target);
	if(k == UINT32_MAX || !IsTok(d, k, SCN_ENDTRY) || !IsTok(d, k + 1, SCN_PUSHFP) || !IsTok(d, k + 2, SCN_PUSH) || d->insn[k + 2].ops[0] != d->frameSize || !IsTok(d, k + 3, SCN_SUB) || !IsTok(d, k + 4, SCN_SETFP) || !IsTok(d, k + 5, SCN_RET) || k + 6 != ie)
	{
		Fail(d, start, "no function epilogue");
		return 0;
	}
	bodyEnd = k;
	if(bodyEnd > bodyStart && LineMarker(d, bodyEnd - 1, &closeLine) && IsBareLine(d, bodyEnd - 1, bodyEnd))
		bodyEnd--;
	else if(d->sourceStrOff != UINT32_MAX)
	{
		Fail(d, start, "no closing line marker of the function");
		return 0;
	}
	// check the frame: locals + params + 4 (the parameters from the slot up, the last one lowest, a byte one 1 byte)
	{
		uint32_t at = d->frameSize - 4;
		for(j = d->paramCount - 1; j >= 0; j--)
		{
			if(d->paramOff[j] != at)
			{
				Fail(d, start, "parameter layout");
				return 0;
			}
			at -= d->paramByte[j] ? 1 : 4;
		}
	}
	// the header entry: filled in after the body (the declarations need the body)
	memset(&header, 0, sizeof header);
	header.kind = ENT_OPEN;
	header.off = start;
	header.ctx = -1;
	header.depth = 0;
	AddEntry(d, 0, ENT_OPEN, start, NULL);
	d->depth = 1;
	ParseStmts(d, bodyStart, bodyEnd, 0, NULL);
	d->depth = 0;
	if(d->failed)
		return 0;
	ResolveJumps(d, firstEntry);
	if(d->failed)
		return 0;
	AddEntry(d, closeLine, ENT_CLOSE, d->insn[bodyEnd].off, Strdup("}"));
	// the header text: a parameter indexed as a pointer is declared as one
	{
		char buf[0x1000];
		size_t n = 0;
		buf[0] = 0;
		for(j = 0; j < d->paramCount; j++)
		{
			struct Local* l = LocalAt(d, d->paramOff[d->paramCount - 1 - j], 0);
			const char* type = d->paramByte[d->paramCount - 1 - j] ? "char " : l && l->elem == 4 ? "int* "
				: l && l->elem == 1                                                              ? "char* "
																								 : "";
			n += (size_t)snprintf(buf + n, sizeof buf - n, "%s%sp%d", j ? ", " : "", type, j + 1);
		}
		params = Strdup(buf);
	}
	for(j = 0; j < d->paramCount; j++)
	{
		struct Local* l = LocalAt(d, d->paramOff[j], 1);
		if(l)
			l->isParam = 1;
	}
	// an array is referred to by its name alone: drop the & the parser put in front of every local address
	for(j = 0; j < d->localCount; j++)
	{
		struct Local* l = &d->locals[j];
		char name[SCN_NAME_MAX + 2];
		int e;
		if(l->scalar || l->isParam || l->off > d->frameSize - 4 - ParamsSize(d))
			continue;
		name[0] = '&';
		LocalName(d, l->off, name + 1, sizeof name - 1);
		for(e = firstEntry; e < d->entCount; e++)
		{
			char* s = d->ent[e].text;
			char* p;
			size_t nl = strlen(name);
			if(!s)
				continue;
			while((p = strstr(s, name)) != NULL)
			{
				char after = p[nl];
				if(!(isalnum((unsigned char)after) || after == '_'))
					memmove(p, p + 1, strlen(p)); // drop the '&'
				s = p + 1;
			}
		}
	}
	decl = Declarations(d);
	// the header goes one line before the first statement when that line is free
	headerLine = 0;
	for(j = firstEntry + 1; j < d->entCount; j++)
		if(d->ent[j].line)
		{
			headerLine = d->ent[j].line - 1;
			break;
		}
	if(headerLine <= 0)
		headerLine = closeLine > 0 ? closeLine : 1;
	d->ent[firstEntry].line = headerLine;
	d->ent[firstEntry].text = decl[0] ? Format("%s(%s) { %s", name, params, decl) : Format("%s(%s) {", name, params);
	free(params);
	free(decl);
	return 1;
}

// ---- the scenario style's body ----------------------------------------------------------------------

/* the label dispatcher at the end of a scenario file: `push 0; test_label;
 * addr BODY; jcc 1; (str L; test_label; addr X; jcc 1)*; str MSG; msgbox;
 * quit`.  Fills the label table; returns 1 when the shape matches. */
static int ParseDispatcher(Dec_t* d, uint32_t i)
{
	const char* s;
	if(!(IsTok(d, i, SCN_PUSH) && d->insn[i].ops[0] == 0 && IsTok(d, i + 1, 0xf7) && IsTok(d, i + 2, SCN_ADDR) && d->insn[i + 2].ops[0] == d->bodyStart && IsTok(d, i + 3, SCN_JCC) && d->insn[i + 3].ops[0] == 1))
		return 0;
	i += 4;
	while(IsTok(d, i, SCN_STR) && IsTok(d, i + 1, 0xf7) && IsTok(d, i + 2, SCN_ADDR) && IsTok(d, i + 3, SCN_JCC) && d->insn[i + 3].ops[0] == 1)
	{
		s = Scn_String(d->f, d->insn[i].ops[0]);
		if(!s || strlen(s) * 3 >= SCN_NAME_MAX || d->labelCount >= (int)(sizeof d->labels / sizeof d->labels[0]))
			return 0;
		d->labels[d->labelCount].off = d->insn[i + 2].ops[0];
		strcpy(d->labels[d->labelCount].sjis, s);
		OS_SjisToUtf8(s, d->labels[d->labelCount].name, SCN_NAME_MAX); // identifiers are UTF-8 in the source
		d->labelCount++;
		i += 4;
	}
	if(!(IsTok(d, i, SCN_STR) && IsTok(d, i + 1, 0xf9) && IsTok(d, i + 2, 0xf4) && i + 3 == d->n))
		return 0;
	s = Scn_String(d->f, d->insn[i].ops[0]);
	return s && strcmp(s, SCN_LABEL_NOT_FOUND) == 0;
}

static int ParseScenarioBody(Dec_t* d)
{
	uint32_t i, dispatcher, bodyEndIdx, k;
	int j, firstEntry = d->entCount;
	if(d->n < 2 || !IsJump(d, 0, &dispatcher))
	{
		Fail(d, 0, "no entry jump");
		return 0;
	}
	d->bodyStart = d->insn[2].off;
	d->bodyEnd = dispatcher;
	k = IndexAt(d, dispatcher);
	if(k == UINT32_MAX || !ParseDispatcher(d, k))
	{
		Fail(d, dispatcher, "unrecognized label dispatcher");
		return 0;
	}
	// the body ends with the generated `ret`
	bodyEndIdx = k;
	if(!(bodyEndIdx > 2 && IsTok(d, bodyEndIdx - 1, SCN_RET)))
	{
		Fail(d, dispatcher, "the body does not end with ret");
		return 0;
	}
	bodyEndIdx--;
	d->funcEnd = UINT32_MAX;
	d->curCtx = -1;
	d->ctxCount = 0;
	d->depth = 0;
	ResetStack(d);
	i = 2;
	ParseStmts(d, i, bodyEndIdx, 0, NULL);
	if(d->failed)
		return 0;
	ResolveJumps(d, firstEntry);
	if(d->failed)
		return 0;
	// the labels: inserted before the entries that start at their offsets
	for(j = 0; j < d->labelCount; j++)
	{
		int e, found = 0;
		for(e = firstEntry; e < d->entCount; e++)
			if(d->ent[e].off == d->labels[j].off && d->ent[e].kind != ENT_LABEL)
			{
				Entry_t lab;
				memset(&lab, 0, sizeof lab);
				lab.kind = ENT_LABEL;
				lab.off = d->labels[j].off;
				lab.depth = d->ent[e].depth;
				lab.ctx = -1;
				lab.text = Format("%s:", d->labels[j].name);
				if(d->entCount == d->entCap)
				{
					d->entCap *= 2;
					d->ent = (Entry_t*)realloc(d->ent, (size_t)d->entCap * sizeof *d->ent);
				}
				memmove(&d->ent[e + 1], &d->ent[e], (size_t)(d->entCount - e) * sizeof *d->ent);
				d->ent[e] = lab;
				d->entCount++;
				found = 1;
				break;
			}
		if(!found)
		{
			// a label at the very end of the body
			if(d->labels[j].off == d->insn[bodyEndIdx].off)
			{
				AddEntry(d, 0, ENT_LABEL, d->labels[j].off, Format("%s:", d->labels[j].name));
				continue;
			}
			Fail(d, d->labels[j].off, "label %s points into a statement", d->labels[j].name);
			return 0;
		}
	}
	return 1;
}

// ---- the token listing and __asm blocks -----------------------------------------------------------------

// one token as an __asm line, with labels for the targets of this range
static void WriteToken(Dec_t* d, const Insn_t* in, FILE* out)
{
	const char* nm = ScnTok_Name(in->tok);
	char name[SCN_NAME_MAX];
	switch(in->tok)
	{
		case SCN_STR:
		{
			char* lit = StringLiteral(d, in->ops[0]);
			fprintf(out, "str %s", lit ? lit : "?");
			free(lit);
			return;
		}
		case SCN_ADDR:
		{
			const char* fn = FunctionAt(d, in->ops[0]);
			if(fn)
				fprintf(out, "addr %s", fn);
			else
				fprintf(out, "addr L_%x", in->ops[0]);
			return;
		}
		case SCN_LINE:
		{
			char* lit = StringLiteral(d, in->ops[0]);
			fprintf(out, "line %s, %u", lit ? lit : "?", in->ops[1]);
			free(lit);
			return;
		}
		case SCN_SRCINFO:
		{
			char* lit = StringLiteral(d, in->ops[2]);
			fprintf(out, "srcinfo 0x%x, %u, %s", in->ops[0], in->ops[1], lit ? lit : "?");
			free(lit);
			return;
		}
		case SCN_PUSH:
			if(in->ops[0] >= 0x10000)
				fprintf(out, "push 0x%x", in->ops[0]);
			else
				fprintf(out, "push %u", in->ops[0]);
			return;
		case SCN_LOCAL: fprintf(out, "local 0x%x", in->ops[0]); return;
		case SCN_LOAD:
		case SCN_STORE:
		case SCN_STORE2:
		case SCN_JCC:
		case SCN_ARGS: fprintf(out, "%s %u", nm, in->ops[0]); return;
		default: break;
	}
	if(nm)
	{
		fputs(nm, out);
		return;
	}
	CommandName(d, in->tok, name, sizeof name);
	fputs(name, out);
}

// the tokens of [start, end) as an __asm block (targets inside get labels)
static void WriteAsmBlock(Dec_t* d, uint32_t start, uint32_t end, int indent, FILE* out)
{
	uint32_t i = IndexAt(d, start), ie = IndexAt(d, end), j;
	uint8_t* isTarget = (uint8_t*)calloc(d->n + 1, 1);
	for(j = i; j < ie; j++)
		if(d->insn[j].tok == SCN_ADDR)
		{
			uint32_t t = IndexAt(d, d->insn[j].ops[0]);
			if(t != UINT32_MAX && !FunctionAt(d, d->insn[j].ops[0]))
				isTarget[t] = 1;
		}
	fprintf(out, "%*s__asm {\n", indent, "");
	for(j = i; j < ie; j++)
	{
		if(isTarget[j])
			fprintf(out, "%*sL_%x:\n", indent, "", d->insn[j].off);
		fprintf(out, "%*s", indent + 4, "");
		WriteToken(d, &d->insn[j], out);
		fputc('\n', out);
	}
	fprintf(out, "%*s}\n", indent, "");
	free(isTarget);
}

static void WriteListing(Dec_t* d, FILE* out)
{
	uint32_t j;
	int i;
	fprintf(out, "; %u tokens, %u bytes of strings\n", d->n, d->f->stringsLen);
	for(j = 0; j < d->n; j++)
	{
		const char* fn = FunctionAt(d, d->insn[j].off);
		if(fn)
			fprintf(out, "%s:\n", fn);
		fprintf(out, "  %06x  ", d->insn[j].off);
		WriteToken(d, &d->insn[j], out);
		fputc('\n', out);
	}
	fprintf(out, "; strings:\n");
	{
		uint32_t off = d->f->codeLen * 4;
		while(off < d->f->codeLen * 4 + d->f->stringsLen)
		{
			const char* s = Scn_String(d->f, off);
			fprintf(out, "  %06x  ", off);
			Asm_WriteStringLiteral(out, (const uint8_t*)s, (uint32_t)strlen(s));
			fputc('\n', out);
			off += (uint32_t)strlen(s) + 1;
		}
	}
	for(i = 0; i < d->f->exportCount; i++)
		fprintf(out, "; export %s = 0x%x\n", d->f->exports[i].name, d->f->exports[i].off);
}

// ---- the whole file ---------------------------------------------------------------------------------

static int CompareExports(const void* a, const void* b)
{
	uint32_t x = ((const ScnExport_t*)a)->off, y = ((const ScnExport_t*)b)->off;
	return x < y ? -1 : x > y;
}

// drop the entries from `mark` on
static void FreeEntries(Dec_t* d, int mark)
{
	int i;
	for(i = mark; i < d->entCount; i++)
		free(d->ent[i].text);
	d->entCount = mark;
}

// the source name the markers use: the library compiler's first string, the scenario compiler's lineinfo operand
static void FindSourceName(Dec_t* d)
{
	uint32_t i;
	d->sourceName[0] = 0;
	d->sourceStrOff = UINT32_MAX;
	for(i = 0; i < d->n; i++)
	{
		const Insn_t* in = &d->insn[i];
		const char* s = NULL;
		if(in->tok == SCN_LINE)
			s = Scn_String(d->f, in->ops[0]);
		else if(in->tok == SCN_LINEINFO && i >= 3 && d->insn[i - 3].tok == SCN_STR)
			s = Scn_String(d->f, d->insn[i - 3].ops[0]);
		if(s)
		{
			snprintf(d->sourceName, sizeof d->sourceName, "%s", s);
			d->sourceStrOff = in->tok == SCN_LINE ? in->ops[0] : d->insn[i - 3].ops[0];
			return;
		}
	}
}

/* Write the entries as lines of source after a `#line 1`: an entry with a
 * line number goes there, one without follows the previous entry (on the
 * next line when it is free, else on the same line); a raw block is
 * followed by a `#line` that puts the next entry back on its line. */
static void WriteEntries(Dec_t* d, FILE* out)
{
	int i, cur = 0, j;
	uint32_t curFile = UINT32_MAX;
	// assign lines
	for(i = 0; i < d->entCount; i++)
	{
		Entry_t* e = &d->ent[i];
		if(e->fileOff != curFile)
		{ // another file's markers: its lines start over
			curFile = e->fileOff;
			cur = 0;
		}
		if(e->line > 0)
		{
			if(e->line < cur && !(e->kind == ENT_OPEN && e->depth == 0))
				e->line = cur; // out of order: keep it on the current line (a function may start further up: `#line`)
			cur = e->line;
			continue;
		}
		// the next fixed line decides whether a fresh line is free
		{
			int next = 0;
			for(j = i + 1; j < d->entCount; j++)
				if(d->ent[j].line > 0)
				{
					next = d->ent[j].line;
					break;
				}
			if(next == 0 || cur + 1 < next)
				cur++;
			if(cur < 1)
				cur = 1;
			e->line = cur;
		}
	}
	// write
	{
		int line = 1;
		int atLineStart = 1, resync = 0;
		curFile = UINT32_MAX;
		fputs("#line 1\n", out);
		for(i = 0; i < d->entCount; i++)
		{
			Entry_t* e = &d->ent[i];
			int depth = e->kind == ENT_LABEL ? (e->depth > 0 ? e->depth - 1 : 0) : e->depth;
			if(e->fileOff != curFile)
			{ // `#file "name"` (the source itself when back to it), and the line count restarts
				const char* name = e->fileOff == UINT32_MAX ? d->sourceName : Scn_String(d->f, e->fileOff);
				if(!atLineStart)
					fputc('\n', out);
				fputs("#file ", out);
				Asm_WriteStringLiteral(out, (const uint8_t*)(name ? name : ""), name ? strlen(name) : 0);
				fputc('\n', out);
				atLineStart = 1;
				curFile = e->fileOff;
				resync = 1;
			}
			if(e->kind == ENT_RAW)
			{
				if(!atLineStart)
					fputc('\n', out);
				fputs(e->text, out);
				if(e->text[0] && e->text[strlen(e->text) - 1] != '\n')
					fputc('\n', out);
				line = e->line + 1;
				atLineStart = 1;
				resync = 1;
				continue;
			}
			if(resync || e->line < line)
			{
				if(!atLineStart)
					fputc('\n', out);
				fprintf(out, "#line %d\n", e->line);
				line = e->line;
				atLineStart = 1;
				resync = 0;
			}
			while(line < e->line)
			{
				fputc('\n', out);
				line++;
				atLineStart = 1;
			}
			if(!e->text[0])
				continue;
			if(atLineStart)
				fprintf(out, "%*s", depth * 4, "");
			else
				fputc(' ', out);
			fputs(e->text, out);
			atLineStart = 0;
		}
		if(!atLineStart)
			fputc('\n', out);
	}
}

typedef struct FuncRange
{
	const char* name;
	uint32_t start, end;
	int asmFallback;
	int firstLine;
	int fileRank; // the file the function's markers name: the source 0, then the includes in order
} FuncRange_t;

/* The strings of the table from `off` on (into tabOff, up to `cap`) and
 * how many at its start the compiler took in before any code used them:
 * the prefix of the table that is not in the order of first use when the
 * functions are read in the given (source) order (`#string` lines put
 * them there again).  `*inversions` counts the pairs still out of that
 * order after the prefix: what no `#string` can mend. */
static uint32_t PreStrings(Dec_t* d, uint32_t off, const FuncRange_t* funcs, int funcCount, uint32_t* tabOff, uint32_t cap,
	uint32_t* inversions)
{
	uint32_t firstUse[4096];
	uint32_t count = 0, j, used, seq = 0;
	int fi;
	if(cap > 4096)
		cap = 4096;
	for(; count < cap;)
	{
		const char* s = Scn_String(d->f, off);
		if(!s)
			break;
		tabOff[count] = off;
		firstUse[count] = UINT32_MAX;
		count++;
		off += (uint32_t)strlen(s) + 1;
	}
	for(fi = 0; fi < funcCount; fi++)
		for(j = IndexAt(d, funcs[fi].start); j < d->n && d->insn[j].off < funcs[fi].end; j++)
		{
			uint32_t ref = d->insn[j].tok == SCN_STR || d->insn[j].tok == SCN_LINE ? d->insn[j].ops[0]
				: d->insn[j].tok == SCN_SRCINFO                                    ? d->insn[j].ops[2]
																				   : UINT32_MAX;
			uint32_t k;
			seq++;
			if(ref == UINT32_MAX)
				continue;
			for(k = 0; k < count; k++)
				if(tabOff[k] == ref && firstUse[k] == UINT32_MAX)
					firstUse[k] = seq;
		}
	/* the prefix that is not in first-use order: a string whose first use comes after that of
	 * a later string of the table was taken in early */
	used = 0;
	for(j = 0; j < count; j++)
	{
		uint32_t k, minLater = UINT32_MAX;
		for(k = j + 1; k < count; k++)
			if(firstUse[k] < minLater)
				minLater = firstUse[k];
		if(firstUse[j] <= minLater)
			break;
		used = j + 1;
	}
	if(inversions)
	{ // how far the rest of the table is from first-use order: the pairs out of order after that prefix
		uint32_t k;
		*inversions = 0;
		for(j = used; j < count; j++)
			for(k = j + 1; k < count; k++)
				if(firstUse[j] != UINT32_MAX && firstUse[k] != UINT32_MAX && firstUse[j] > firstUse[k])
					(*inversions)++;
	}
	return used;
}

// where the string table's includes end: the first string a literal refers to
static uint32_t FirstLiteralOffset(Dec_t* d)
{
	uint32_t off = d->sourceStrOff + (uint32_t)strlen(d->sourceName) + 1;
	for(;;)
	{
		const char* s = Scn_String(d->f, off);
		uint32_t j;
		int referenced = 0;
		if(!s)
			return off;
		for(j = 0; j < d->n && !referenced; j++)
			if((d->insn[j].tok == SCN_STR && d->insn[j].ops[0] == off) || (d->insn[j].tok == SCN_SRCINFO && d->insn[j].ops[2] == off))
				referenced = 1;
		if(referenced)
			return off;
		off += (uint32_t)strlen(s) + 1;
	}
}

static void WriteHeader(Dec_t* d, const ScnDecOptions_t* opt, const FuncRange_t* funcs, int funcCount, FILE* out)
{
	int i;
	fprintf(out, "// %s - decompiled by bgiscn (docs/scenario.md)\n", opt->name ? opt->name : "scenario");
	fprintf(out, "#style %s\n", d->style == SCN_STYLE_LIBRARY ? (d->raw ? "library-raw" : "library") : d->lineMarkers ? "scenario-line"
																													  : "scenario");
	if(gAsmUtf8)
		fprintf(out, "#encoding utf-8\n");
	if(d->f->importCount)
	{
		fputs("#import", out);
		for(i = 0; i < d->f->importCount; i++)
		{
			fputs(i ? ", " : " ", out);
			Asm_WriteStringLiteral(out, (const uint8_t*)d->f->imports[i], strlen(d->f->imports[i]));
		}
		fputc('\n', out);
	}
	if(d->sourceName[0])
	{ // as a literal: the older scenario compiler wrote full Windows paths with backslashes
		fputs("#source ", out);
		Asm_WriteStringLiteral(out, (const uint8_t*)d->sourceName, strlen(d->sourceName));
		fputc('\n', out);
	}
	for(i = 0; i < (int)d->n; i++)
		if(d->insn[i].tok == SCN_SRCINFO)
		{
			fputs("#srcinfo\n", out);
			break;
		}
	/* the library compiler lists the included headers after the source name in the string table: up
	 * to the first string a literal refers to (a line marker may name an include, which holds functions) */
	if(d->style == SCN_STYLE_LIBRARY && d->sourceStrOff != UINT32_MAX)
	{
		uint32_t off = d->sourceStrOff + (uint32_t)strlen(d->sourceName) + 1;
		for(;;)
		{
			const char* s = Scn_String(d->f, off);
			uint32_t j;
			int referenced = 0;
			if(!s)
				break;
			for(j = 0; j < d->n && !referenced; j++)
				if((d->insn[j].tok == SCN_STR && d->insn[j].ops[0] == off) || (d->insn[j].tok == SCN_SRCINFO && d->insn[j].ops[2] == off))
					referenced = 1;
			if(referenced)
				break;
			fputs("#include ", out);
			Asm_WriteStringLiteral(out, (const uint8_t*)s, strlen(s));
			fputc('\n', out);
			off += (uint32_t)strlen(s) + 1;
		}
		/* strings the compiler took in before any code used them (constants of the headers, by the
		 * look of it): those of the table that stand earlier than their first use, as `#string` */
		if(d->raw)
		{
			uint32_t tabOff[4096];
			uint32_t j, used = PreStrings(d, off, funcs, funcCount, tabOff, 4096, NULL);
			for(j = 0; j < used; j++)
			{
				fputs("#string ", out);
				Asm_WriteStringLiteral(out, (const uint8_t*)Scn_String(d->f, tabOff[j]), strlen(Scn_String(d->f, tabOff[j])));
				fputc('\n', out);
			}
		}
	}
}

static int CompareFuncByLine(const void* a, const void* b)
{
	const FuncRange_t* x = (const FuncRange_t*)a;
	const FuncRange_t* y = (const FuncRange_t*)b;
	if(x->fileRank != y->fileRank)
		return x->fileRank < y->fileRank ? -1 : 1;
	if(x->firstLine != y->firstLine)
		return x->firstLine < y->firstLine ? -1 : 1;
	return x->start < y->start ? -1 : x->start > y->start;
}

// the first source line a function's tokens mention (its order in the source)
static int FirstLineOf(Dec_t* d, uint32_t start, uint32_t end)
{
	uint32_t i;
	for(i = IndexAt(d, start); i < d->n && d->insn[i].off < end; i++)
		if(d->insn[i].tok == SCN_LINE)
			return (int)d->insn[i].ops[1];
	return 0;
}

/* the rank of the file a function's markers name: the string table lists the
 * source and then the includes, in the order the compiler read them, which
 * is the order it took the functions (and their strings) in */
static int FileRankOf(Dec_t* d, uint32_t start, uint32_t end)
{
	uint32_t i;
	for(i = IndexAt(d, start); i < d->n && d->insn[i].off < end; i++)
		if(d->insn[i].tok == SCN_LINE)
		{
			uint32_t off = d->sourceStrOff, file = d->insn[i].ops[0];
			int rank = 0;
			if(off == UINT32_MAX)
				return 0;
			while(off < file)
			{
				const char* s = Scn_String(d->f, off);
				if(!s)
					break;
				off += (uint32_t)strlen(s) + 1;
				rank++;
			}
			return off == file ? 2 * rank : 0x10000; // even ranks: the source's own place is decided later
		}
	return 0;
}

// the __asm fallback of a range as a raw entry
static void AddAsmEntry(Dec_t* d, const char* name, uint32_t start, uint32_t end)
{
	FILE* mem = tmpfile();
	long n;
	char* text;
	Entry_t* e;
	if(!mem)
		return;
	if(name)
		fprintf(mem, "%s() {\n", name);
	WriteAsmBlock(d, start, end, name ? 4 : 0, mem);
	if(name)
		fputs("}\n", mem);
	n = ftell(mem);
	fseek(mem, 0, SEEK_SET);
	text = (char*)malloc((size_t)n + 1);
	if(fread(text, 1, (size_t)n, mem) != (size_t)n)
		n = 0;
	text[n] = 0;
	fclose(mem);
	d->depth = 0;
	e = AddEntry(d, 0, ENT_RAW, start, text);
	e->ctx = -1;
}

static void WriteSource(Dec_t* d, const ScnDecOptions_t* opt, FuncRange_t* funcs, int funcCount, FILE* out)
{
	int i;
	WriteHeader(d, opt, funcs, funcCount, out);
	FreeEntries(d, 0);
	if(d->style == SCN_STYLE_LIBRARY)
	{
		int prevEnd = 0;
		for(i = 0; i < funcCount; i++)
		{
			FuncRange_t* fr = &funcs[i];
			int mark = d->entCount;
			d->failed = 0;
			if(!fr->asmFallback && ParseFunction(d, fr->name, fr->start, fr->end))
			{
				int first = d->ent[mark].line, last, j;
				if(mark > 0 && d->ent[mark].fileOff != d->ent[mark - 1].fileOff)
					prevEnd = 0; // another file: its lines start over
				/* the header sits on the line before the first statement, or shares the previous function's
				 * last line (when the first statement is on that line or the next); a function further up
				 * the file (the raw variant lays them out as called for) keeps its line */
				if(first <= prevEnd && first + 1 >= prevEnd)
					d->ent[mark].line = prevEnd;
				for(j = mark, last = 0; j < d->entCount; j++)
					if(d->ent[j].line > last)
						last = d->ent[j].line;
				prevEnd = last;
				continue;
			}
			FreeEntries(d, mark);
			fr->asmFallback = 1;
			AddAsmEntry(d, fr->name, fr->start, fr->end);
		}
		WriteEntries(d, out);
		FreeEntries(d, 0);
		return;
	}
	d->failed = 0;
	d->srcinfoIndex = 0;
	d->labelCount = 0;
	if(!funcs[0].asmFallback && ParseScenarioBody(d))
	{
		WriteEntries(d, out);
		FreeEntries(d, 0);
		return;
	}
	funcs[0].asmFallback = 1;
	FreeEntries(d, 0);
	AddAsmEntry(d, NULL, 0, d->f->codeLen * 4);
	WriteEntries(d, out);
	FreeEntries(d, 0);
}

// decode the stream into instructions
static int Decode(Dec_t* d)
{
	uint32_t i = 0, cap = 1024;
	d->insn = (Insn_t*)malloc(cap * sizeof *d->insn);
	d->atOff = (uint32_t*)malloc((d->f->codeLen + 1) * sizeof *d->atOff);
	memset(d->atOff, 0xff, (d->f->codeLen + 1) * sizeof *d->atOff);
	d->n = 0;
	while(i < d->f->codeLen)
	{
		uint32_t t = d->f->code[i];
		int k = Scn_OperandCount(t), j;
		Insn_t in;
		if(i + 1 + (uint32_t)k > d->f->codeLen)
			return 0;
		in.off = i * 4;
		in.tok = t;
		in.len = 1 + (uint32_t)k;
		for(j = 0; j < 3; j++)
			in.ops[j] = j < k ? d->f->code[i + 1 + (uint32_t)j] : 0;
		if(d->n == cap)
		{
			cap *= 2;
			d->insn = (Insn_t*)realloc(d->insn, cap * sizeof *d->insn);
		}
		d->atOff[i] = d->n;
		d->insn[d->n++] = in;
		i += in.len;
	}
	d->atOff[d->f->codeLen] = d->n;
	return 1;
}

/* `line` tokens mean the library compiler - unless the file is a
 * scenario of the older variant: no exports, the dispatcher jump first and
 * a label test at the end (scn_internal.h); d->lineMarkers notes that */
static int DetectStyle(Dec_t* d)
{
	uint32_t i;
	int hasLine = 0, hasTestLabel = 0;
	d->lineMarkers = 0;
	if(d->raw)
		return SCN_STYLE_LIBRARY; // the headerless files: functions, with or without markers
	for(i = 0; i < d->n; i++)
	{
		if(d->insn[i].tok == SCN_LINE)
			hasLine = 1;
		else if(d->insn[i].tok == 0xf7)
			hasTestLabel = 1;
	}
	if(hasLine)
	{
		if(d->f->exportCount == 0 && hasTestLabel && IsTok(d, 0, SCN_ADDR) && IsTok(d, 1, SCN_JMP))
		{
			d->lineMarkers = 1;
			return SCN_STYLE_SCENARIO;
		}
		return SCN_STYLE_LIBRARY;
	}
	for(i = 0; i < d->n; i++)
		if(d->insn[i].tok == SCN_LINEINFO || d->insn[i].tok == SCN_ARGS)
			return SCN_STYLE_SCENARIO;
	return d->f->exportCount ? SCN_STYLE_LIBRARY : SCN_STYLE_SCENARIO;
}

// write the decompilation into a memory buffer
static char* WriteToBuffer(Dec_t* d, const ScnDecOptions_t* opt, FuncRange_t* funcs, int funcCount, size_t* len)
{
	FILE* mem = tmpfile();
	char* text;
	long n;
	if(!mem)
		return NULL;
	WriteSource(d, opt, funcs, funcCount, mem);
	n = ftell(mem);
	fseek(mem, 0, SEEK_SET);
	text = (char*)malloc((size_t)n + 1);
	if(fread(text, 1, (size_t)n, mem) != (size_t)n)
		n = 0;
	text[n] = 0;
	fclose(mem);
	*len = (size_t)n;
	return text;
}

/* the functions of a raw file, which exports nothing: one starts at offset
 * 0 and after every `ret`, and is named `main` (the first) or `fn_<index>` */
static void FindRawFunctions(Dec_t* d, ScnFile_t* copy)
{
	uint32_t i, cap = 16;
	copy->exports = (ScnExport_t*)malloc(cap * sizeof *copy->exports);
	copy->exportCount = 0;
	for(i = 0; i < d->n; i++)
		if(i == 0 || d->insn[i - 1].tok == SCN_RET)
		{
			ScnExport_t* e;
			if((uint32_t)copy->exportCount == cap)
			{
				cap *= 2;
				copy->exports = (ScnExport_t*)realloc(copy->exports, cap * sizeof *copy->exports);
			}
			e = &copy->exports[copy->exportCount++];
			e->off = d->insn[i].off;
			if(i == 0)
				strcpy(e->name, "main");
			else // numbered in layout order: the compiler lays functions out by name, main first
				snprintf(e->name, sizeof e->name, "fn_%04d", copy->exportCount - 1);
		}
}

int Scn_Decompile(const ScnFile_t* f, const ScnDecOptions_t* opt, FILE* out)
{
	Dec_t* d = (Dec_t*)calloc(1, sizeof *d);
	FuncRange_t* funcs = NULL;
	int funcCount = 0, i, fallbacks = 0, pass;
	ScnExport_t* sorted;
	ScnFile_t rawCopy; // a raw file with the functions found as its exports (freed below; `f` stays as it is)
	if(f->image)
	{
		free(d);
		return Scn16_Decompile(f->image, f->imageSize, opt, out);
	}
	d->f = f;
	d->cmds = opt->cmds;
	d->importFuncs = opt->funcs;
	d->importFuncCount = opt->funcCount;
	d->verbose = opt->verbose;
	d->curCtx = -1;
	d->raw = f->raw;
	d->funcFile = UINT32_MAX;
	if(!Decode(d))
	{
		fprintf(out, "// %s: a token runs past the end of the stream\n", opt->name ? opt->name : "");
		free(d->insn);
		free(d->atOff);
		free(d);
		return 1;
	}
	if(f->raw)
	{
		rawCopy = *f;
		FindRawFunctions(d, &rawCopy);
		d->f = &rawCopy;
	}
	d->style = DetectStyle(d);
	FindSourceName(d);
	if(opt->rawTokens)
	{
		WriteListing(d, out);
		free(d->insn);
		free(d->atOff);
		free(d);
		return 0;
	}
	// the functions: the exports in offset order (the library style); one body (the scenario style)
	if(d->style == SCN_STYLE_LIBRARY)
	{
		const ScnFile_t* df = d->f;
		sorted = (ScnExport_t*)malloc(((size_t)df->exportCount + 1) * sizeof *sorted);
		memcpy(sorted, df->exports, (size_t)df->exportCount * sizeof *sorted);
		qsort(sorted, (size_t)df->exportCount, sizeof *sorted, CompareExports);
		funcs = (FuncRange_t*)calloc((size_t)df->exportCount + 1, sizeof *funcs);
		for(i = 0; i < df->exportCount; i++)
		{
			funcs[i].name = sorted[i].name;
			funcs[i].start = sorted[i].off;
			funcs[i].end = i + 1 < df->exportCount ? sorted[i + 1].off : df->codeLen * 4;
			funcs[i].firstLine = FirstLineOf(d, funcs[i].start, funcs[i].end);
			funcs[i].fileRank = d->raw ? FileRankOf(d, funcs[i].start, funcs[i].end) : 0;
		}
		funcCount = df->exportCount;
		if(funcCount == 0 || sorted[0].off != 0)
		{
			// code before the first export (or no exports): the whole file is a token block
			funcCount = 0;
		}
		// the source order: by file (the raw variant's includes hold functions) and line
		if(d->raw && d->sourceStrOff != UINT32_MAX && funcCount > 0)
		{
			/* the source's own functions stand somewhere among the includes (its #include lines may
			 * follow some of them): the place that leaves the fewest strings out of first-use order */
			int includes = 0, r, best = 0;
			uint32_t bestPre = UINT32_MAX, litOff = FirstLiteralOffset(d), o;
			for(o = d->sourceStrOff + (uint32_t)strlen(d->sourceName) + 1; o < litOff; includes++)
				o += (uint32_t)strlen(Scn_String(d->f, o)) + 1;
			for(r = 0; r <= includes; r++)
			{
				uint32_t tabOff[4096], pre;
				for(i = 0; i < funcCount; i++)
					if(funcs[i].fileRank == 0)
						funcs[i].fileRank = 2 * r + 1; // between include r and r + 1 (includes get even ranks)
				qsort(funcs, (size_t)funcCount, sizeof *funcs, CompareFuncByLine);
				PreStrings(d, litOff, funcs, funcCount, tabOff, 4096, &pre);
				if(d->verbose > 1)
					fprintf(stderr, "  source after include %d: %u strings out of order\n", r, (unsigned)pre);
				if(pre < bestPre)
				{
					bestPre = pre;
					best = r;
				}
				for(i = 0; i < funcCount; i++)
					if(funcs[i].fileRank == 2 * r + 1)
						funcs[i].fileRank = 0;
			}
			for(i = 0; i < funcCount; i++)
				if(funcs[i].fileRank == 0)
					funcs[i].fileRank = 2 * best + 1;
		}
		qsort(funcs, (size_t)funcCount, sizeof *funcs, CompareFuncByLine);
	}
	else
	{
		funcs = (FuncRange_t*)calloc(1, sizeof *funcs);
		funcs[0].name = "";
		funcs[0].start = 0;
		funcs[0].end = f->codeLen * 4;
		funcCount = 1;
		sorted = NULL;
	}
	if(d->style == SCN_STYLE_LIBRARY && funcCount == 0)
	{
		WriteHeader(d, opt, funcs, funcCount, out);
		WriteAsmBlock(d, 0, f->codeLen * 4, 0, out);
		fallbacks = 1;
	}
	else
	{
		/* write, compile back, compare; a function whose tokens differ is
		 * switched to its token block and the file written again */
		for(pass = 0; pass < funcCount + 1; pass++)
		{
			size_t len;
			char* text = WriteToBuffer(d, opt, funcs, funcCount, &len);
			ScnCompileResult_t res;
			uint8_t* back;
			uint32_t backSize, firstDiff = UINT32_MAX, j;
			int bad = -1;
			if(!text)
				break;
			if(opt->noVerify)
			{
				fputs(text, out);
				free(text);
				break;
			}
			Scn_Compile(text, len, opt->name ? opt->name : "scenario", opt->cmds, NULL, NULL, &res);
			if(res.errors == 0)
			{
				back = Scn_Save(&res.file, &backSize);
				// compare the token streams and the strings
				if(res.file.codeLen != f->codeLen)
					firstDiff = (res.file.codeLen < f->codeLen ? res.file.codeLen : f->codeLen) * 4;
				for(j = 0; j < f->codeLen && j < res.file.codeLen && firstDiff == UINT32_MAX; j++)
					if(res.file.code[j] != f->code[j])
						firstDiff = j * 4;
				if(firstDiff == UINT32_MAX && (res.file.stringsLen != f->stringsLen || memcmp(res.file.strings, f->strings, f->stringsLen) != 0 || res.file.exportCount != f->exportCount || res.file.importCount != f->importCount))
					firstDiff = f->codeLen * 4;
				{
					uint32_t origSize;
					uint8_t* orig = Scn_Save(f, &origSize);
					if(firstDiff == UINT32_MAX && (origSize != backSize || memcmp(orig, back, origSize) != 0))
						firstDiff = 0;
					BGI_Free(orig);
				}
				BGI_Free(back);
				Scn_Free(&res.file);
			}
			else
				firstDiff = 0;
			if(firstDiff == UINT32_MAX)
			{
				fputs(text, out);
				free(text);
				break;
			}
			// the function containing the first difference falls back
			for(i = 0; i < funcCount; i++)
				if(!funcs[i].asmFallback && firstDiff >= funcs[i].start && firstDiff < funcs[i].end)
					bad = i;
			if(bad < 0)
				for(i = 0; i < funcCount; i++)
					if(!funcs[i].asmFallback && (bad < 0 || funcs[i].start > funcs[bad].start) && funcs[i].start <= firstDiff)
						bad = i;
			if(bad < 0)
			{
				// nothing left to fall back to: write what there is
				fprintf(out, "// WARNING: the round trip differs at 0x%x and no function could be switched to tokens\n", firstDiff);
				fputs(text, out);
				free(text);
				break;
			}
			funcs[bad].asmFallback = 1;
			fallbacks++;
			free(text);
		}
	}
	for(i = 0, fallbacks = 0; i < funcCount; i++)
		fallbacks += funcs[i].asmFallback;
	if(d->style == SCN_STYLE_LIBRARY && funcCount == 0)
		fallbacks = 1;
	FreeEntries(d, 0);
	free(d->ent);
	free(funcs);
	free(sorted);
	if(f->raw)
		free(rawCopy.exports);
	free(d->insn);
	free(d->atOff);
	free(d);
	return fallbacks;
}
