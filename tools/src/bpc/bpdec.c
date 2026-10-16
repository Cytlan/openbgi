/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * bpdec.c - the program decompiler: bytecode back to the C-like source
 *           (interface in bpc.h, the source in docs/bpc.md, the code
 *           shapes in bpc_internal.h)
 *
 * The disassembler's analysis (Dis_Analyze) tells code from data and
 * marks the branch targets; every block of code that starts with a frame
 * prologue is a function.  Each function is decompiled on its own: the
 * frame size and the parameters come from the prologue, the locals from
 * the frame offsets the body touches, and the statements from a symbolic
 * run of the stack machine over the instructions, in which the values
 * left on the stack are the expression statements (the original compiler
 * never pops them) and the conditional jumps are reassembled into the
 * conditions of if, while and do-while by searching for the `&&` / `||`
 * tree that the compiler's jump scheme would have produced.  Jumps that
 * no construct explains become break, continue, return or goto.
 *
 * After writing the source the decompiler compiles it (bpcomp.c) and
 * compares; a function whose bytes differ is written as an `__asm` block
 * of listing lines instead, and the whole program when the data trailer
 * differs, so that the text always compiles back to the input.
 */
#include "bpc_internal.h"

#include <ctype.h>
#include <stdarg.h>

// ---- text helpers ---------------------------------------------------------------------------------

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
	char buf[0x2000];
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	return Strdup(buf);
}

// ---- the function being decompiled ---------------------------------------------------------------

enum ExprKind
{
	E_NUM,       // a constant: num
	E_LOCALADDR, // push_local_addr n: aux = n (the address of a local)
	E_FUNC,      // push_code_off of a function: aux = its offset
	E_STR,       // a string literal: aux = its offset
	E_CALL,      // the value of a call or an instruction (an assignment's rhs in the `store` form)
	E_ASSIGN,    // the value of an assignment (store_keep)
	E_LOGIC,     // land / lor / lnot in value form: written in parentheses when it leads a condition
	E_NOT,       // ~x (a unary minus when 1 is added to it)
	E_OTHER
};

typedef struct Expr
{
	char* text;
	int prec;
	int kind;
	int64_t num;
	uint32_t aux;
	uint32_t startIdx; // the index of the first instruction of the expression
} Expr_t;

enum EntryKind
{
	ENT_TEXT,
	ENT_OPEN,  // ends with '{'
	ENT_CLOSE, // '}' (or "} else {", "} while (c);")
	ENT_LABEL,
	ENT_JUMP, // a jump to resolve: break / continue / return / goto
	ENT_RAW   // listing lines (an __asm block)
};

typedef struct Entry
{
	int kind;
	int depth;
	uint32_t off;        // the offset the statement starts at (labels are inserted before it)
	int ctx;             // the loop context (for break / continue), -1 outside loops
	uint32_t jumpTarget; // ENT_JUMP: the target offset
	char* text;
} Entry_t;

typedef struct Ctx
{
	int parent;
	uint32_t breakTarget;    // the offset a break goes to
	uint32_t continueTarget; // the offset a continue goes to
} Ctx_t;

// a local of the current function
typedef struct Local
{
	uint32_t n;    // push_local_addr n: the frame offset below the frame pointer
	uint32_t size; // bytes up to the next local
	int sizes[3];  // direct loads / stores of each size code
	int pushes;    // push_local_addr of it, in all
	int addrUses;  // the address taken (an argument, arithmetic): the pushes that were not direct accesses
	int elem;      // the declared element size (1, 2, 4), 0 before the declaration is decided
	int isArray;
} Local_t;

typedef struct Func
{
	uint32_t start, end; // the offsets of the function's code
	uint32_t frame;      // F
	int params;          // parameters (the pops after the prologue)
	int prologueOk;
} Func_t;

typedef struct Dec
{
	const DisMap_t* map;
	EngineGen_t gen;
	const BpcDecOptions_t* opt;
	// the functions
	Func_t* funcs;
	int funcCount;
	int* funcLine; // the line each function starts at in the text written last
	// the current function
	const Func_t* f;
	AsmInsn_t* insn;
	uint32_t n;
	uint32_t* idxAt;  // offset -> instruction index (UINT32_MAX in between)
	uint32_t exitIdx; // the index of the epilogue's push_fp
	uint32_t bodyIdx; // the first instruction after the parameter pops
	uint32_t* params; // the frame offsets of the parameters in order
	Local_t* locals;
	int localCount;
	// the symbolic stack
	Expr_t stack[512];
	int sp;
	uint32_t curIdx, popMinIdx;
	// output
	Entry_t* ent;
	int entCount, entCap;
	int depth;
	Ctx_t ctx[64];
	int ctxCount;
	int curCtx;
	uint32_t loopHead;                 // the head of the loop whose body is being parsed (UINT32_MAX: none): not a loop again
	uint32_t loopFrom[64], loopTo[64]; // the loops of this pass, as offset ranges
	int loopCount;
	uint32_t noLoop[16]; // heads that are not to be loops: a jump from outside landed inside one (then gotos)
	int noLoopCount;
	uint32_t badLoop; // what a failed ResolveJumps blames (UINT32_MAX: nothing)
	int failed;
	char failMsg[256];
	uint32_t failOff;
	int quiet; // a trial run (the condition search): nothing is recorded
	// the condition-tree search
	int* memoKey;
	int memoCount;
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
	e->startIdx = d->popMinIdx < d->curIdx ? d->popMinIdx : d->curIdx;
}

static void PushNum(Dec_t* d, uint32_t v)
{
	char buf[32];
	int32_t sv = (int32_t)v;
	if(v >= 0x10000 || sv < 0)
		snprintf(buf, sizeof buf, "0x%x", v);
	else
		snprintf(buf, sizeof buf, "%u", v);
	Push(d, Strdup(buf), BPC_PREC_PRIMARY, E_NUM, 0);
	d->stack[d->sp - 1].num = v;
}

/* pop a value; an empty stack yields `__pop`, the value the original
 * program finds there: what an earlier statement (or a called function)
 * left on the evaluation stack, which the compiler never clears */
static Expr_t Pop(Dec_t* d, uint32_t off)
{
	Expr_t e;
	(void)off;
	if(d->sp == 0)
	{
		memset(&e, 0, sizeof e);
		e.text = Strdup("__pop");
		e.prec = BPC_PREC_PRIMARY;
		e.kind = E_OTHER;
		e.startIdx = d->curIdx;
		return e;
	}
	e = d->stack[--d->sp];
	if(e.startIdx < d->popMinIdx)
		d->popMinIdx = e.startIdx;
	return e;
}

static void ResetStack(Dec_t* d)
{
	while(d->sp > 0)
		free(d->stack[--d->sp].text);
}

// the text of an operand, parenthesized when its precedence requires
static char* Operand(const Expr_t* e, int minPrec)
{
	if(e->prec < minPrec)
		return Format("(%s)", e->text);
	return Strdup(e->text);
}

// ---- locals ----------------------------------------------------------------------------------------

static Local_t* LocalAt(Dec_t* d, uint32_t n, int create)
{
	int i;
	for(i = 0; i < d->localCount; i++)
		if(d->locals[i].n == n)
			return &d->locals[i];
	if(!create)
		return NULL;
	d->locals = (Local_t*)realloc(d->locals, ((size_t)d->localCount + 1) * sizeof *d->locals);
	memset(&d->locals[d->localCount], 0, sizeof d->locals[0]);
	d->locals[d->localCount].n = n;
	return &d->locals[d->localCount++];
}

static int ParamIndex(const Dec_t* d, uint32_t n)
{
	int i;
	for(i = 0; i < d->f->params; i++)
		if(d->params[i] == n)
			return i;
	return -1;
}

// the name of the local at frame offset n: p<k> for a parameter, l<hex> otherwise
static void LocalName(const Dec_t* d, uint32_t n, char* out, size_t size)
{
	int p = ParamIndex(d, n);
	if(p >= 0)
		snprintf(out, size, "p%d", p + 1);
	else
		snprintf(out, size, "l%x", n);
}

static const char* SizeType(int sizeCode)
{
	return sizeCode == 0 ? "char" : sizeCode == 1 ? "short"
												  : "int";
}

static int CmpLocalDesc(const void* a, const void* b)
{
	uint32_t x = ((const Local_t*)a)->n, y = ((const Local_t*)b)->n;
	return x > y ? -1 : x < y;
}

/* decide every local's declaration from what the body did with it: the
 * size from the gap to the next local (the last one ends at the frame
 * pointer), the element size from the direct loads and stores (the most
 * frequent code, 4 when there are none), an array when the address is
 * used or the slot is larger than its element */
static void DecideLocals(Dec_t* d)
{
	int i;
	if(d->localCount) // a function without locals has no list
		qsort(d->locals, (size_t)d->localCount, sizeof *d->locals, CmpLocalDesc);
	for(i = 0; i < d->localCount; i++)
	{
		Local_t* l = &d->locals[i];
		uint32_t next = i + 1 < d->localCount ? d->locals[i + 1].n : 0;
		int best = 2, k;
		l->size = l->n - next;
		l->addrUses = l->pushes - (l->sizes[0] + l->sizes[1] + l->sizes[2]);
		for(k = 0; k < 3; k++)
			if(l->sizes[k] > l->sizes[best] || (l->sizes[best] == 0 && l->sizes[k] > 0))
				best = k;
		l->elem = best == 0 ? 1 : best == 1 ? 2
											: 4;
		if(l->sizes[0] + l->sizes[1] + l->sizes[2] == 0)
		{
			l->elem = (l->size % 4 == 0) ? 4 : 1;
			l->isArray = 1;
		}
		else
			l->isArray = (uint32_t)l->elem < l->size && l->addrUses > 0 && ParamIndex(d, l->n) < 0;
	}
}

// the lvalue of a sized access through an address expression (frees nothing)
static char* LvalueText(Dec_t* d, const Expr_t* ptr, int sizeCode)
{
	char name[32];
	if(ptr->kind == E_LOCALADDR)
	{
		Local_t* l = LocalAt(d, ptr->aux, 0);
		LocalName(d, ptr->aux, name, sizeof name);
		if(l && !l->isArray && l->elem == (sizeCode == 0 ? 1 : sizeCode == 1 ? 2
																			 : 4))
			return Strdup(name);
		if(l && l->isArray)
			return Format("*(%s*)%s", SizeType(sizeCode), name);
		return Format("*(%s*)&%s", SizeType(sizeCode), name);
	}
	if(ptr->kind == E_NUM)
		return Format("*(%s*)%s", SizeType(sizeCode), ptr->text);
	return Format("*(%s*)(%s)", SizeType(sizeCode), ptr->text);
}

// note a direct access of a local (the pre-pass decides the declarations from these counts)
static void NoteAccess(Dec_t* d, const Expr_t* ptr, int sizeCode)
{
	if(ptr->kind == E_LOCALADDR)
	{
		Local_t* l = LocalAt(d, ptr->aux, 1);
		if(l && sizeCode >= 0 && sizeCode <= 2)
			l->sizes[sizeCode]++;
	}
}

// ---- output entries --------------------------------------------------------------------------------

static Entry_t* AddEntry(Dec_t* d, int kind, uint32_t off, char* text)
{
	Entry_t* e;
	if(d->entCount == d->entCap)
	{
		d->entCap = d->entCap ? d->entCap * 2 : 256;
		d->ent = (Entry_t*)realloc(d->ent, (size_t)d->entCap * sizeof *d->ent);
	}
	e = &d->ent[d->entCount++];
	e->kind = kind;
	e->depth = d->depth;
	e->off = off;
	e->ctx = d->curCtx;
	e->jumpTarget = 0;
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
	d->curCtx = d->ctxCount++;
	return d->curCtx;
}

static void PopCtx(Dec_t* d)
{
	d->curCtx = d->ctx[d->curCtx].parent;
}

// ---- instructions -----------------------------------------------------------------------------------

static int IsOp(const Dec_t* d, uint32_t i, int op)
{
	return i < d->n && d->insn[i].fam == 0 && d->insn[i].op == op;
}

static uint32_t Off(const Dec_t* d, uint32_t i)
{
	return i < d->n ? d->insn[i].off : d->f->end;
}

static uint32_t IndexAt(const Dec_t* d, uint32_t off)
{
	if(off < d->f->start || off > d->f->end)
		return UINT32_MAX;
	if(off == d->f->end)
		return d->n;
	return d->idxAt[off - d->f->start];
}

// `push_code_off X; jmp` at i
static int IsJump(const Dec_t* d, uint32_t i, uint32_t* target)
{
	if(IsOp(d, i, OP_CODE_OFF) && IsOp(d, i + 1, OP_JMP))
	{
		*target = d->insn[i].target;
		return 1;
	}
	return 0;
}

// `push_code_off X; jcc c` at i
static int IsJcc(const Dec_t* d, uint32_t i, uint32_t* target, int* cc)
{
	if(IsOp(d, i, OP_CODE_OFF) && IsOp(d, i + 1, OP_JCC))
	{
		*target = d->insn[i].target;
		*cc = (int)d->insn[i + 1].v[0];
		return 1;
	}
	return 0;
}

// the last instruction of [a, b) that jumps back to `target` (UINT32_MAX none)
static uint32_t LastBackJump(const Dec_t* d, uint32_t a, uint32_t b, uint32_t target)
{
	uint32_t i, t, found = UINT32_MAX;
	int cc;
	for(i = a; i < b && i < d->n; i++)
		if((IsJump(d, i, &t) || IsJcc(d, i, &t, &cc)) && t == target)
			found = i;
	return found;
}

static int IsLabelled(const Dec_t* d, uint32_t off)
{
	return off < d->map->size && (d->map->label[off] & (DIS_L_JUMP | DIS_L_CALL)) != 0;
}

static const Func_t* FuncAt(const Dec_t* d, uint32_t off)
{
	int i;
	for(i = 0; i < d->funcCount; i++)
		if(d->funcs[i].start == off)
			return &d->funcs[i];
	return NULL;
}

static void FuncName(const Dec_t* d, uint32_t off, char* out, size_t n)
{
	if(off == 0)
		snprintf(out, n, "main");
	else
		snprintf(out, n, "sub_%04x", (unsigned)off);
}

/* the string at a data offset as a quoted literal: the bytes up to the
 * NUL (whatever they are - a literal of the source may hold control
 * characters); NULL when the offset is not in data or no NUL follows */
static char* StringLiteral(const Dec_t* d, uint32_t off)
{
	uint32_t len = 0;
	FILE* mem;
	char* text;
	long n;
	if(off >= d->map->size || d->map->kind[off] != DIS_K_DATA)
		return NULL;
	while(off + len < d->map->size && d->map->kind[off + len] == DIS_K_DATA && d->map->area[off + len] != 0)
		len++;
	if(off + len >= d->map->size || d->map->area[off + len] != 0)
		return NULL;
	mem = tmpfile();
	if(!mem)
		return NULL;
	Asm_WriteStringLiteral(mem, d->map->area + off, len);
	n = ftell(mem);
	fseek(mem, 0, SEEK_SET);
	text = (char*)malloc((size_t)n + 1);
	if(fread(text, 1, (size_t)n, mem) != (size_t)n)
		n = 0;
	text[n] = 0;
	fclose(mem);
	return text;
}

// the conversions of a format string at a data offset (-1 when no string lies there)
static int FormatConversions(const Dec_t* d, uint32_t off)
{
	uint32_t len, i;
	int n = 0;
	if(off >= d->map->size || d->map->kind[off] != DIS_K_DATA)
		return -1;
	len = Dis_StringLength(d->map, off);
	for(i = 0; i < len; i++)
	{
		if(d->map->area[off + i] == '%')
		{
			if(i + 1 < len && d->map->area[off + i + 1] == '%')
				i++;
			else
				n++;
		}
	}
	return n;
}

// ---- expressions ------------------------------------------------------------------------------------

// the arguments of an instruction: pushed left to right, so the bottom-most is the first
static char* ArgsInOrder(Dec_t* d, int count, uint32_t off)
{
	char* out;
	size_t total = 1;
	int i;
	int missing = count > d->sp ? count - d->sp : 0; // the first ones are what earlier code left: `__pop`
	count -= missing;
	total += (size_t)missing * 7;
	for(i = d->sp - count; i < d->sp; i++)
		total += strlen(d->stack[i].text) + 2;
	out = (char*)malloc(total);
	out[0] = 0;
	for(i = 0; i < missing; i++)
		strcat(out, i ? ", __pop" : "__pop");
	for(i = d->sp - count; i < d->sp; i++)
	{
		if(i > d->sp - count || missing)
			strcat(out, ", ");
		strcat(out, d->stack[i].text);
		if(d->stack[i].startIdx < d->popMinIdx)
			d->popMinIdx = d->stack[i].startIdx;
		free(d->stack[i].text);
	}
	d->sp -= count;
	return out;
}

// the arguments of a function: pushed right to left, so the top is the first
static char* ArgsReversed(Dec_t* d, int count, uint32_t off)
{
	char** parts;
	char* out;
	size_t total = 1;
	int i;
	// fewer values than arguments: the last ones are what earlier code left (Pop gives `__pop`)
	parts = (char**)calloc((size_t)count + 1, sizeof *parts);
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

/* how many values an indirect call takes: everything above the last
 * assignment value (an argument is an expression; a value left by an
 * earlier statement is an assignment) */
static int GuessArity(const Dec_t* d)
{
	int i, n = 0;
	for(i = d->sp - 1; i >= 0; i--)
	{
		if(d->stack[i].kind == E_ASSIGN)
			break;
		n++;
	}
	return n;
}

static void BinaryOp(Dec_t* d, const BpcOperator_t* op, uint32_t off)
{
	Expr_t b = Pop(d, off);
	Expr_t a = Pop(d, off);
	char* ta;
	char* tb;
	int kind = (op->op == 0x38 || op->op == 0x39) ? E_LOGIC : E_OTHER;
	// the unary minus: x; not; push 1; add
	if(op->op == OP_ADD && a.kind == E_NOT && b.kind == E_NUM && b.num == 1)
	{
		Push(d, Format("-%s", a.text + 1), BPC_PREC_UNARY, E_OTHER, 0);
		free(a.text);
		free(b.text);
		return;
	}
	ta = Operand(&a, op->prec);
	tb = Operand(&b, op->prec + 1); // left associative: the right operand needs parentheses at equal precedence
	Push(d, Format("%s %s %s", ta, op->text, tb), op->prec, kind, 0);
	free(ta);
	free(tb);
	free(a.text);
	free(b.text);
}

/* Run the instructions from i, pushing symbolic values, until a statement
 * boundary: a jump (returned), a label (returned), the end, or a
 * statement that leaves nothing (emitted, the values below it flushed
 * first).  Returns the index of the instruction it stopped at. */
static void FlushStatements(Dec_t* d, uint32_t off);

static uint32_t Run(Dec_t* d, uint32_t i, uint32_t end)
{
	for(; i < end && !d->failed; i++)
	{
		const AsmInsn_t* in = &d->insn[i];
		uint32_t off = in->off;
		const BpcOperator_t* op;
		int pops, pushes;
		if(i > d->curIdx && IsLabelled(d, off) && i != d->bodyIdx)
			return i; // a label: a statement boundary
		d->curIdx = i;
		d->popMinIdx = UINT32_MAX;
		if(in->fam == 0)
		{
			switch(in->op)
			{
				case OP_PUSH_I8:
				case OP_PUSH_I16: PushNum(d, (uint32_t)in->v[0]); continue;
				case OP_PUSH_I32:
					if((uint32_t)in->v[0] <= 32767)
					{ // a wide push of a small value: eight hex digits say so in the source
						char buf[16];
						snprintf(buf, sizeof buf, "0x%08x", (unsigned)in->v[0]);
						Push(d, Strdup(buf), BPC_PREC_PRIMARY, E_NUM, 0);
						d->stack[d->sp - 1].num = (uint32_t)in->v[0];
					}
					else
						PushNum(d, (uint32_t)in->v[0]);
					continue;
				case OP_LOCAL_ADDR:
				{
					char name[32];
					Local_t* l = LocalAt(d, (uint32_t)in->v[0], 1);
					LocalName(d, (uint32_t)in->v[0], name, sizeof name);
					if(l)
						l->pushes++;
					if(l && l->isArray)
						Push(d, Strdup(name), BPC_PREC_PRIMARY, E_LOCALADDR, (uint32_t)in->v[0]); // an array stands for its address
					else
						Push(d, Format("&%s", name), BPC_PREC_UNARY, E_LOCALADDR, (uint32_t)in->v[0]);
					continue;
				}
				case OP_CODE_ADDR:
				{
					char* lit = StringLiteral(d, in->target);
					if(!lit)
					{
						Fail(d, off, "a data reference to something that is not a string");
						return i;
					}
					Push(d, lit, BPC_PREC_PRIMARY, E_STR, in->target);
					continue;
				}
				case OP_CODE_OFF:
				{
					uint32_t target;
					int cc;
					char name[32];
					if(IsJump(d, i, &target) || IsJcc(d, i, &target, &cc))
						return i; // a jump: the statement parser's business
					if(!FuncAt(d, in->target))
					{
						Fail(d, off, "a code address that is not a function");
						return i;
					}
					FuncName(d, in->target, name, sizeof name);
					Push(d, Strdup(name), BPC_PREC_PRIMARY, E_FUNC, in->target);
					continue;
				}
				case OP_LOAD:
				{
					Expr_t p = Pop(d, off);
					char* lv;
					NoteAccess(d, &p, (int)in->v[0]);
					lv = LvalueText(d, &p, (int)in->v[0]);
					Push(d, lv, p.kind == E_LOCALADDR ? BPC_PREC_PRIMARY : BPC_PREC_UNARY, E_OTHER, 0);
					free(p.text);
					continue;
				}
				case OP_STORE_KEEP:
				{
					Expr_t v = Pop(d, off);
					Expr_t p = Pop(d, off);
					char* lv;
					char* tv;
					NoteAccess(d, &p, (int)in->v[0]);
					lv = LvalueText(d, &p, (int)in->v[0]);
					tv = Operand(&v, BPC_PREC_ASSIGN);
					Push(d, Format("%s = %s", lv, tv), BPC_PREC_ASSIGN, E_ASSIGN, 0);
					free(lv);
					free(tv);
					free(v.text);
					free(p.text);
					continue;
				}
				case OP_STORE:
				{
					Expr_t p = Pop(d, off);
					Expr_t v = Pop(d, off);
					char* lv;
					char* tv;
					NoteAccess(d, &p, (int)in->v[0]);
					FlushStatements(d, off);
					lv = LvalueText(d, &p, (int)in->v[0]);
					tv = Operand(&v, BPC_PREC_ASSIGN);
					// a call's value takes the plain assignment (that is the form the compiler gives it); anything else the explicit one
					AddEntry(d, ENT_TEXT, Off(d, v.startIdx < p.startIdx ? v.startIdx : p.startIdx),
						Format(v.kind == E_CALL ? "%s = %s;" : "%s <- %s;", lv, tv));
					free(lv);
					free(tv);
					free(v.text);
					free(p.text);
					continue;
				}
				case OP_STORE_INL:
				{
					Expr_t p = Pop(d, off);
					FILE* mem = tmpfile();
					char* lit;
					long n;
					if(!mem)
						return i;
					Asm_WriteStringLiteral(mem, in->bytes, (uint32_t)in->v[0]);
					n = ftell(mem);
					fseek(mem, 0, SEEK_SET);
					lit = (char*)malloc((size_t)n + 1);
					if(fread(lit, 1, (size_t)n, mem) != (size_t)n)
						n = 0;
					lit[n] = 0;
					fclose(mem);
					FlushStatements(d, off);
					AddEntry(d, ENT_TEXT, Off(d, p.startIdx), Format("__inline(%s, %s);", p.text, lit));
					free(lit);
					free(p.text);
					continue;
				}
				case OP_STORE_MUL:
				{
					int count = (int)in->v[1];
					char* list;
					Expr_t p;
					char* lv;
					if(count + 1 > d->sp)
					{
						Fail(d, off, "store_multi of %d values with %d on the stack", count, d->sp);
						return i;
					}
					list = ArgsInOrder(d, count, off);
					p = Pop(d, off);
					FlushStatements(d, off);
					lv = LvalueText(d, &p, (int)in->v[0]);
					AddEntry(d, ENT_TEXT, Off(d, p.startIdx), Format("%s = {%s};", lv, list));
					free(lv);
					free(list);
					free(p.text);
					continue;
				}
				case OP_JMP:
				case OP_JCC:
				case OP_RET:
				case OP_PUSH_FP:
				case OP_SET_FP: Fail(d, off, "unexpected control instruction"); return i;
				case OP_CALL:
				{
					Expr_t callee = Pop(d, off);
					int arity;
					char* args;
					char* tc;
					if(callee.kind == E_FUNC)
					{
						const Func_t* cf = FuncAt(d, callee.aux);
						arity = cf && cf->prologueOk ? cf->params : GuessArity(d);
					}
					else
						arity = GuessArity(d);
					args = ArgsReversed(d, arity, off);
					tc = Operand(&callee, BPC_PREC_PRIMARY);
					Push(d, Format("%s(%s)", tc, args), BPC_PREC_PRIMARY, E_CALL, 0);
					free(tc);
					free(args);
					free(callee.text);
					continue;
				}
				case OP_NOT:
				{
					Expr_t a = Pop(d, off);
					char* ta = Operand(&a, BPC_PREC_UNARY);
					Push(d, Format("~%s", ta), BPC_PREC_UNARY, E_NOT, 0);
					free(ta);
					free(a.text);
					continue;
				}
				case OP_LNOT:
				{
					Expr_t a = Pop(d, off);
					char* ta = Operand(&a, BPC_PREC_UNARY);
					Push(d, Format("!%s", ta), BPC_PREC_UNARY, E_LOGIC, 0);
					free(ta);
					free(a.text);
					continue;
				}
				case OP_SELECT:
				{
					Expr_t f = Pop(d, off);
					Expr_t t = Pop(d, off);
					Expr_t c = Pop(d, off);
					char* tc = Operand(&c, BPC_PREC_SELECT + 1);
					char* tt = Operand(&t, BPC_PREC_SELECT);
					char* tf = Operand(&f, BPC_PREC_SELECT + 1);
					Push(d, Format("%s ? %s : %s", tc, tt, tf), BPC_PREC_SELECT, E_CALL, 0);
					free(tc);
					free(tt);
					free(tf);
					free(c.text);
					free(t.text);
					free(f.text);
					continue;
				}
				default: break;
			}
			op = BpcOp_ByOpcode(in->op);
			if(op)
			{
				BinaryOp(d, op, off);
				continue;
			}
		}
		// an instruction with a name: a call of it
		{
			const char* name = Asm_OpName(in->fam, in->op, d->gen);
			{ // a name the compiler would resolve to another number (a reused one under "any") is no use: the number then
				int f2, o2;
				char full2[64];
				if(name && in->fam)
					snprintf(full2, sizeof full2, "%s.%s", Asm_FamilyName(in->fam), name);
				else if(name)
					snprintf(full2, sizeof full2, "%s", name);
				if(name && (!Asm_LookupName(full2, strlen(full2), d->gen, &f2, &o2) || f2 != in->fam || o2 != in->op))
					name = NULL;
			}
			char full[64];
			char* args;
			if(!Asm_StackEffect(in->fam, in->op, d->gen, &pops, &pushes))
			{ /* an instruction the tables do not know (a script-defined one, "FF xx", or one of a
			   * build this engine does not cover): it takes what the statement pushed and leaves a
			   * value when the next instruction stores or tests one */
				uint32_t t;
				int cc;
				pops = GuessArity(d);
				pushes = (i + 2 < end && IsOp(d, i + 1, OP_LOCAL_ADDR) && IsOp(d, i + 2, OP_STORE)) || (i + 1 < end && IsJcc(d, i + 1, &t, &cc));
			}
			if(in->fam == 0 && in->op == OP_SPRINTF)
			{ // sprintf: the conversion arguments right to left, then dst and fmt
				Expr_t fmt = Pop(d, off);
				Expr_t dst = Pop(d, off);
				// a literal format tells the conversion count; anything else takes what the statement pushed
				int k = fmt.kind == E_STR ? FormatConversions(d, fmt.aux) : GuessArity(d);
				char* rest;
				if(k < 0)
				{
					Fail(d, off, "sprintf with a format whose conversions are unclear");
					return i;
				}
				rest = ArgsReversed(d, k, off);
				FlushStatements(d, off);
				AddEntry(d, ENT_TEXT, Off(d, d->popMinIdx < dst.startIdx ? d->popMinIdx : dst.startIdx),
					Format("sprintf(%s, %s%s%s);", dst.text, fmt.text, k ? ", " : "", rest));
				free(rest);
				free(fmt.text);
				free(dst.text);
				continue;
			}
			if(pops < 0)
			{
				Fail(d, off, "an instruction with a variable argument count");
				return i;
			}
			if(!name)
				snprintf(full, sizeof full, "%s.0x%02x", in->fam ? Asm_FamilyName(in->fam) : "op", in->op);
			else if(in->fam)
				snprintf(full, sizeof full, "%s.%s", Asm_FamilyName(in->fam), name);
			else
				snprintf(full, sizeof full, "%s", name);
			args = ArgsInOrder(d, pops, off);
			{   // a conditional jump right after an instruction tests "its value", whatever it pushes (the
				// source wrote `if (instruction(...))`; a value an earlier statement left is what runs)
				uint32_t t;
				int cc;
				if(pushes == 0 && i + 1 < end && IsJcc(d, i + 1, &t, &cc))
					pushes = 1;
			}
			if(pushes == 1)
				Push(d, Format("%s(%s)", full, args), BPC_PREC_PRIMARY, E_CALL, 0);
			else
			{
				/* nothing, or several values: a statement of its own; the values
				 * stay on the stack for whatever takes them (`__pop`) */
				uint32_t startIdx = d->popMinIdx < d->curIdx ? d->popMinIdx : d->curIdx;
				int k;
				FlushStatements(d, off);
				AddEntry(d, ENT_TEXT, Off(d, startIdx), Format("%s(%s);", full, args));
				for(k = 0; k < pushes; k++)
					Push(d, Strdup("__pop"), BPC_PREC_PRIMARY, E_OTHER, 0);
			}
			free(args);
		}
	}
	return i;
}

// every value on the stack is a statement of its own, in order
static void FlushStatements(Dec_t* d, uint32_t off)
{
	int i;
	(void)off;
	for(i = 0; i < d->sp; i++)
	{
		if(strcmp(d->stack[i].text, "__pop") != 0) // a value nobody took: it stays where it is
			AddEntry(d, ENT_TEXT, Off(d, d->stack[i].startIdx), Format("%s;", d->stack[i].text));
		free(d->stack[i].text);
	}
	d->sp = 0;
}

// ---- conditions -----------------------------------------------------------------------------------

typedef struct Leaf
{
	char* text; // the value
	int logic;  // the value is a land / lor / lnot: parenthesized in a chain
	int cc;     // the jump condition: 1 jumps when zero, 0 when nonzero
	int sign;   // 2 .. 5: the jump tests the sign of the value (`__sign(v) > 0` ..), and cc is 0
	uint32_t target;
	uint32_t start;    // the instruction index the leaf's expression begins at
	uint32_t startOff; // its offset (what the targets of other leaves name)
	uint32_t next;     // the index after its jcc
} Leaf_t;

#define TARGET_NEXT UINT32_MAX // "the fall-through": T or F is the position after the chain

typedef struct Memo
{
	int lo, hi, trueFalls;
	uint32_t T, F;
	char* text; // NULL: no tree (the entry exists to remember that)
	int used;
} Memo_t;

typedef struct Cond
{
	Dec_t* d;
	Leaf_t* leaves;
	int count;
	Memo_t* memo; // the results of Build by (lo, hi, T, F): the search is exponential without it
	int memoCount, memoCap;
} Cond_t;

static Memo_t* MemoFind(Cond_t* c, int lo, int hi, uint32_t T, uint32_t F, int trueFalls)
{
	int i;
	for(i = 0; i < c->memoCount; i++)
		if(c->memo[i].lo == lo && c->memo[i].hi == hi && c->memo[i].T == T && c->memo[i].F == F && c->memo[i].trueFalls == trueFalls)
			return &c->memo[i];
	return NULL;
}

static void MemoAdd(Cond_t* c, int lo, int hi, uint32_t T, uint32_t F, int trueFalls, const char* text)
{
	Memo_t* m;
	if(c->memoCount == c->memoCap)
	{
		c->memoCap = c->memoCap ? c->memoCap * 2 : 256;
		c->memo = (Memo_t*)realloc(c->memo, (size_t)c->memoCap * sizeof *c->memo);
	}
	m = &c->memo[c->memoCount++];
	m->lo = lo;
	m->hi = hi;
	m->T = T;
	m->F = F;
	m->trueFalls = trueFalls;
	m->text = text ? Strdup(text) : NULL;
	m->used = 1;
}

// the text of a leaf, negated or not, as an operand of && / ||
static char* LeafText(const Leaf_t* l, int negate)
{
	if(l->sign)
	{ // the sign test, or its complement (jcc 2 > and 4 <=, 3 >= and 5 <)
		static const char* const ops[4] = {">", ">=", "<=", "<"};
		int cc = negate ? (l->sign <= 3 ? l->sign + 2 : l->sign - 2) : l->sign;
		return Format("__sign(%s) %s 0", l->text, ops[cc - 2]);
	}
	if(negate)
	{
		// !x for a name, !(e) for anything with structure
		size_t i;
		int simple = 1;
		for(i = 0; l->text[i]; i++)
			if(!isalnum((unsigned char)l->text[i]) && l->text[i] != '_')
				simple = 0;
		return Format(simple ? "!%s" : "!(%s)", l->text);
	}
	if(l->logic)
		return Format("(%s)", l->text);
	return Strdup(l->text);
}

// whether a condition text has an `||` at its top level (outside parentheses and strings)
static int TopIsOr(const char* t)
{
	int depth = 0, inStr = 0;
	for(; *t; t++)
	{
		if(inStr)
		{
			if(*t == '\\' && t[1])
				t++;
			else if(*t == '"')
				inStr = 0;
			continue;
		}
		if(*t == '"')
			inStr = 1;
		else if(*t == '(')
			depth++;
		else if(*t == ')')
			depth--;
		else if(depth == 0 && t[0] == '|' && t[1] == '|')
			return 1;
	}
	return 0;
}

/* The tree of leaves [lo, hi) that the compiler's scheme would have
 * compiled with true target T and false target F, one of which is the
 * position after leaf hi - 1 (the fall-through): T when trueFalls, else
 * F.  NULL when there is none.  The scheme: `a && b` is cond(a, start(b),
 * F) cond(b, T, F) with a's true falling through; `a || b` is cond(a, T,
 * start(b)) cond(b, T, F) with a's false falling through; a leaf jumps to
 * the target that is not the fall-through, when its value is zero (jcc 1)
 * or not (jcc 0); a negated leaf swaps the targets. */
static char* Build(Cond_t* c, int lo, int hi, uint32_t T, uint32_t F, int trueFalls, int depth)
{
	const Leaf_t* l = &c->leaves[lo];
	int k;
	Memo_t* m;
	char* result = NULL;
	if(depth > 64)
		return NULL;
	if(hi - lo == 1)
	{
		uint32_t t = l->target, fall = Off(c->d, l->next);
		if(trueFalls)
			return T == fall && t == F ? LeafText(l, l->cc == 0) : NULL; // jumps to F: when zero (plain) or when nonzero (negated)
		return F == fall && t == T ? LeafText(l, l->cc == 1) : NULL;     // jumps to T: when nonzero (plain) or when zero (negated)
	}
	m = MemoFind(c, lo, hi, T, F, trueFalls);
	if(m)
		return m->text ? Strdup(m->text) : NULL;
	for(k = lo + 1; k < hi && !result; k++)
	{
		uint32_t startK = c->leaves[k].startOff;
		char* left;
		char* right;
		// a && b
		left = Build(c, lo, k, startK, F, 1, depth + 1);
		if(left)
		{
			right = Build(c, k, hi, T, F, trueFalls, depth + 1);
			if(right)
			{
				/* an || as an operand of && would need parentheses, and a
				 * parenthesized expression is a value, not a chain: such a tree
				 * is not what the source could have said; another tree gives the
				 * same jumps */
				if(!TopIsOr(left) && !TopIsOr(right))
					result = Format("%s && %s", left, right);
				free(right);
			}
			free(left);
		}
		if(result)
			break;
		// a || b: an || inside an && operand is parenthesized by the leaf text rules below
		left = Build(c, lo, k, T, startK, 0, depth + 1);
		if(left)
		{
			right = Build(c, k, hi, T, F, trueFalls, depth + 1);
			if(right)
			{
				result = Format("%s || %s", left, right);
				free(right);
			}
			free(left);
		}
	}
	MemoAdd(c, lo, hi, T, F, trueFalls, result);
	return result;
}

/* Build with the precedence of the result: the text is rebuilt with
 * parentheses around every `||` that becomes an operand of `&&`.  Build
 * itself produces "a && b || c" shapes left to right, which C reads as
 * (a && b) || c - the same tree - so only an `||` inside an `&&` needs
 * them; Build never puts one there without the leaf's own parentheses
 * (an && operand that is an || group is written by the || case's left
 * side, which is then the left operand of the enclosing &&).  To keep
 * the text unambiguous that case is parenthesized here. */
static char* BuildCond(Cond_t* c, int lo, int hi, uint32_t T, uint32_t F)
{
	char* r;
	int i;
	uint32_t fall = Off(c->d, c->leaves[hi - 1].next);
	int trueFalls = T == TARGET_NEXT;
	if(T == TARGET_NEXT)
		T = fall;
	if(F == TARGET_NEXT)
		F = fall;
	c->memo = NULL;
	c->memoCount = c->memoCap = 0;
	r = Build(c, lo, hi, T, F, trueFalls, 0);
	for(i = 0; i < c->memoCount; i++)
		free(c->memo[i].text);
	free(c->memo);
	c->memo = NULL;
	return r;
}

static void FreeLeaves(Leaf_t* leaves, int count)
{
	int i;
	for(i = 0; i < count; i++)
		free(leaves[i].text);
	free(leaves);
}

/* Collect the leaves of a condition from instruction i: expression, jcc,
 * expression, jcc .. as long as each expression starts on an empty stack
 * and ends in a jcc, stopping at a label from outside the chain.  The
 * first leaf may take a condition already on the stack (`firstReady`).
 * Returns the count; *leaves is malloc'ed. */
static void FreeEntries(Dec_t* d, int from);

static uint32_t gCollectRetry; // CollectLeaves: where the first leaf's expression began when values lay below it

static int CollectLeaves(Dec_t* d, uint32_t i, uint32_t end, Leaf_t** leaves, int firstReady)
{
	int count = 0, cap = 0;
	uint32_t chainStart = i;
	*leaves = NULL;
	gCollectRetry = UINT32_MAX;
	for(;;)
	{
		uint32_t j, target;
		int cc;
		Expr_t v;
		Leaf_t* l;
		if(count && IsLabelled(d, Off(d, i)))
		{
			// a label inside the chain is fine when only the chain jumps to it
			uint32_t k, t;
			int c2, fromOutside = 0;
			for(k = 0; k < d->n && !fromOutside; k++)
				if((IsJump(d, k, &t) || IsJcc(d, k, &t, &c2)) && t == Off(d, i) && (k < chainStart || k >= i))
					fromOutside = 1;
			if(fromOutside)
				break;
		}
		if(firstReady && count == 0)
		{
			j = i;
			if(d->sp != 1)
				break;
		}
		else
		{
			int entBefore = d->entCount;
			if(i >= end)
				break;
			ResetStack(d);
			d->curIdx = i;
			d->quiet++;
			j = Run(d, i, end);
			d->quiet--;
			if(d->failed || d->sp != 1 || d->entCount != entBefore)
			{
				if(d->opt->verbose > 2)
					fprintf(stderr, "    not a leaf at 0x%x: %s (sp %d, %d entries)\n", (unsigned)Off(d, i), d->failed ? d->failMsg : "-", d->sp,
						d->entCount - entBefore);
				// values below the top: statements before the chain, which may begin where the top began
				if(!d->failed && count == 0 && d->sp > 1 && d->entCount == entBefore && d->stack[d->sp - 1].startIdx > i)
					gCollectRetry = d->stack[d->sp - 1].startIdx;
				d->failed = 0; // not a leaf: the statement parser reports whatever it is
				ResetStack(d);
				FreeEntries(d, entBefore);
				break;
			}
		}
		if(!IsJcc(d, j, &target, &cc))
		{
			ResetStack(d);
			break;
		}
		v = Pop(d, 0);
		if(count == cap)
		{
			cap = cap ? cap * 2 : 8;
			*leaves = (Leaf_t*)realloc(*leaves, (size_t)cap * sizeof **leaves);
		}
		l = &(*leaves)[count++];
		l->text = v.text;
		l->logic = v.kind == E_LOGIC;
		l->sign = 0;
		l->cc = cc;
		if(cc >= 2 && cc <= 5)
		{ // a sign test of the value: __sign(v) > 0 (2), >= 0 (3), <= 0 (4), < 0 (5), taken when true
			l->sign = cc;
			l->cc = 0;
		}
		l->target = target;
		l->start = firstReady && count == 1 ? v.startIdx : i;
		l->startOff = Off(d, l->start);
		l->next = j + 2;
		i = j + 2;
		firstReady = 0;
	}
	ResetStack(d);
	return count;
}

// ---- statements -------------------------------------------------------------------------------------

static uint32_t ParseStmts(Dec_t* d, uint32_t i, uint32_t end);
static void FreeEntries(Dec_t* d, int from);

/* a jump statement: return when the target is the exit (with the value
 * on top of the stack when it is a plain value), else a jump entry to
 * resolve into break / continue / goto */
static void JumpStatement(Dec_t* d, uint32_t i, uint32_t target)
{
	uint32_t off = Off(d, i);
	if(target == Off(d, d->exitIdx))
	{
		if(d->sp > 0 && d->stack[d->sp - 1].kind != E_ASSIGN && d->stack[d->sp - 1].kind != E_CALL)
		{
			Expr_t v = Pop(d, off);
			FlushStatements(d, off);
			AddEntry(d, ENT_TEXT, Off(d, v.startIdx), Format("return %s;", v.text));
			free(v.text);
		}
		else
		{
			FlushStatements(d, off);
			AddEntry(d, ENT_TEXT, off, Strdup("return;"));
		}
		return;
	}
	FlushStatements(d, off);
	AddEntry(d, ENT_JUMP, off, NULL)->jumpTarget = target;
}

// whether any jump outside instructions [from, to) targets `off`
static int JumpedToFromOutside(const Dec_t* d, uint32_t off, uint32_t from, uint32_t to)
{
	uint32_t k, t;
	int cc;
	for(k = 0; k < d->n; k++)
		if((k < from || k >= to) && (IsJump(d, k, &t) || IsJcc(d, k, &t, &cc)) && t == off)
			return 1;
	return 0;
}

/* The if at the chain of leaves starting at instruction i (the first
 * leaf's value may be on the stack already).  Tries the longest prefix of
 * leaves that forms a condition: the block form (true falls through into
 * the then part, false jumps past it) and the direct form (true jumps to
 * a break / continue / goto target, false falls through).  Returns the
 * index after the construct, or i when it is none. */
static uint32_t ParseIf(Dec_t* d, uint32_t i, uint32_t end, int firstReady)
{
	Leaf_t* leaves;
	int count = CollectLeaves(d, i, end, &leaves, firstReady), len;
	Cond_t c;
	uint32_t result = i;
	if(count == 0)
	{
		FreeLeaves(leaves, count);
		return i;
	}
	c.d = d;
	c.leaves = leaves;
	for(len = count; len > 0 && result == i; len--)
	{
		uint32_t X = leaves[len - 1].target, next = leaves[len - 1].next;
		uint32_t xi = IndexAt(d, X);
		char* cond;
		c.count = len;
		if(xi == UINT32_MAX)
			continue;
		// the direct form first when the target is where a jump statement would go
		if(X != Off(d, next) && xi != d->exitIdx && d->curCtx >= 0 && (X == d->ctx[d->curCtx].breakTarget || X == d->ctx[d->curCtx].continueTarget || xi < next))
		{
			cond = BuildCond(&c, 0, len, X, TARGET_NEXT);
			if(cond)
			{
				AddEntry(d, ENT_TEXT, Off(d, leaves[0].start), Format("if (%s) __jump__;", cond))->jumpTarget = X;
				d->ent[d->entCount - 1].kind = ENT_JUMP;
				free(cond);
				result = next;
				break;
			}
		}
		if(xi == d->exitIdx && X != Off(d, next))
		{ // return (without a value) from an if: the direct form
			cond = BuildCond(&c, 0, len, X, TARGET_NEXT);
			if(cond)
			{
				AddEntry(d, ENT_TEXT, Off(d, leaves[0].start), Format("if (%s) return;", cond));
				free(cond);
				result = next;
				break;
			}
		}
		if(X == Off(d, next))
		{ // an empty then part
			cond = BuildCond(&c, 0, len, TARGET_NEXT, X);
			if(cond)
			{
				AddEntry(d, ENT_TEXT, Off(d, leaves[0].start), Format("if (%s) {}", cond));
				free(cond);
				result = next;
				break;
			}
		}
		if(xi >= next && xi <= end)
		{ // the block form: then = [next, xi)
			cond = BuildCond(&c, 0, len, TARGET_NEXT, X);
			if(cond)
			{
				uint32_t thenEnd = xi, elseEnd = UINT32_MAX, jt;
				/* an else part when the then part ends with a jump past the else part (an unlabelled
				 * one: a jump something jumps to is a statement; and nothing but the condition jumps
				 * to the else part: a label there is a goto's) */
				if(thenEnd >= next + 2 && IsJump(d, thenEnd - 2, &jt) && !IsLabelled(d, Off(d, thenEnd - 2)) && jt > X && IndexAt(d, jt) != UINT32_MAX && IndexAt(d, jt) <= end && !(d->curCtx >= 0 && (jt == d->ctx[d->curCtx].breakTarget || jt == d->ctx[d->curCtx].continueTarget)) && IndexAt(d, jt) != d->exitIdx && !JumpedToFromOutside(d, X, leaves[0].start, next))
				{
					elseEnd = IndexAt(d, jt);
					thenEnd -= 2;
				}
				AddEntry(d, ENT_OPEN, Off(d, leaves[0].start), Format("if (%s) {", cond));
				free(cond);
				d->depth++;
				ParseStmts(d, next, thenEnd);
				d->depth--;
				if(elseEnd != UINT32_MAX)
				{
					AddEntry(d, ENT_CLOSE, Off(d, thenEnd), Strdup("} else {"));
					d->depth++;
					ParseStmts(d, xi, elseEnd);
					d->depth--;
					AddEntry(d, ENT_CLOSE, Off(d, elseEnd), Strdup("}"));
					result = elseEnd;
				}
				else
				{
					AddEntry(d, ENT_CLOSE, Off(d, xi), Strdup("}"));
					result = xi;
				}
				break;
			}
		}
		if(X != Off(d, next) && !(xi >= next && xi <= end))
		{ // a jump elsewhere: the direct form with a goto
			cond = BuildCond(&c, 0, len, X, TARGET_NEXT);
			if(cond)
			{
				AddEntry(d, ENT_TEXT, Off(d, leaves[0].start), Format("if (%s) __jump__;", cond))->jumpTarget = X;
				d->ent[d->entCount - 1].kind = ENT_JUMP;
				free(cond);
				result = next;
				break;
			}
		}
	}
	FreeLeaves(leaves, count);
	return result;
}

static void NoteLoop(Dec_t* d, uint32_t from, uint32_t to)
{
	if(d->loopCount < (int)(sizeof d->loopFrom / sizeof d->loopFrom[0]))
	{
		d->loopFrom[d->loopCount] = from;
		d->loopTo[d->loopCount] = to;
		d->loopCount++;
	}
}

static int IsNoLoop(const Dec_t* d, uint32_t off)
{
	int i;
	for(i = 0; i < d->noLoopCount; i++)
		if(d->noLoop[i] == off)
			return 1;
	return 0;
}

/* whether instruction k can end a statement: a jump, a store, a call or
 * an engine instruction (whatever value it leaves, the statement is over) */
static int StatementEnd(const Dec_t* d, uint32_t k)
{
	return IsOp(d, k, OP_JCC) || IsOp(d, k, OP_JMP) || IsOp(d, k, OP_STORE) || IsOp(d, k, OP_STORE_MUL) || IsOp(d, k, OP_STORE_INL) || IsOp(d, k, OP_CALL) || IsOp(d, k, OP_STORE_KEEP) || d->insn[k].fam != 0 || d->insn[k].op >= 0x60;
}

/* The loop whose head is instruction i and whose last backward jump is
 * instruction j (a `push_code_off i` followed by jmp or jcc). */
static uint32_t ParseLoop(Dec_t* d, uint32_t i, uint32_t j, uint32_t end)
{
	uint32_t loopEnd = j + 2, bodyEnd = j;
	uint32_t headOff = Off(d, i), outerHead;
	int ctx;
	if(IsOp(d, j + 1, OP_JMP))
	{
		// while: a condition at the head whose false target is the loop end
		Leaf_t* leaves;
		int count, len;
		Cond_t c;
		uint32_t bodyStart = i;
		char* cond = NULL;
		ResetStack(d);
		count = CollectLeaves(d, i, j, &leaves, 0);
		c.d = d;
		c.leaves = leaves;
		for(len = count; len > 0 && !cond; len--)
		{
			if(leaves[len - 1].target != Off(d, loopEnd))
				continue;
			c.count = len;
			cond = BuildCond(&c, 0, len, TARGET_NEXT, Off(d, loopEnd));
			if(cond)
				bodyStart = leaves[len - 1].next;
		}
		FreeLeaves(leaves, count);
		if(cond)
			AddEntry(d, ENT_OPEN, headOff, Format("while (%s) {", cond));
		else
			AddEntry(d, ENT_OPEN, headOff, Strdup("for (;;) {"));
		free(cond);
		ctx = PushCtx(d, Off(d, loopEnd), headOff);
		d->depth++;
		outerHead = d->loopHead;
		d->loopHead = headOff;
		ParseStmts(d, bodyStart, bodyEnd);
		d->loopHead = outerHead;
		d->depth--;
		PopCtx(d);
		AddEntry(d, ENT_CLOSE, Off(d, bodyEnd), Strdup("}"));
		NoteLoop(d, headOff, Off(d, loopEnd));
		return loopEnd;
	}
	else
	{
		// do-while: the condition is the chain of leaves ending at j, found backwards
		uint32_t k, condStart = j, bestStart = UINT32_MAX;
		char* best = NULL;
		Leaf_t* leaves;
		int count;
		Cond_t c;
		// candidate chain starts: every instruction from the head to j, longest chain first
		for(k = i; k < j && !best; k++)
		{
			if(k > i && !IsLabelled(d, Off(d, k)) && !StatementEnd(d, k - 1))
				continue; // a leaf starts after a statement boundary of some kind (cheap filter)
			ResetStack(d);
			count = CollectLeaves(d, k, j + 2, &leaves, 0);
			if(count == 0 && gCollectRetry != UINT32_MAX && gCollectRetry < j)
			{ // the chain begins after values an earlier statement left: from there
				FreeLeaves(leaves, count);
				k = gCollectRetry;
				ResetStack(d);
				count = CollectLeaves(d, k, j + 2, &leaves, 0);
			}
			if(d->opt->verbose > 1)
				fprintf(stderr, "    do-while condition from 0x%x: %d leaves%s\n", (unsigned)Off(d, k), count,
					count > 0 && leaves[count - 1].next == j + 2 ? ", reaching the jump" : "");
			if(count > 0 && leaves[count - 1].next == j + 2 && leaves[count - 1].target == headOff)
			{
				c.d = d;
				c.leaves = leaves;
				c.count = count;
				best = BuildCond(&c, 0, count, headOff, Off(d, loopEnd));
				if(best)
					bestStart = k;
			}
			FreeLeaves(leaves, count);
		}
		if(!best)
		{ // no expression before the jump: it tests a value an earlier statement left (dead code, mostly)
			uint32_t t;
			int cc;
			if(IsJcc(d, j, &t, &cc) && (cc == 0 || cc == 1))
			{
				best = Strdup(cc == 0 ? "__pop" : "!__pop");
				bestStart = j;
			}
		}
		if(!best)
		{
			Fail(d, Off(d, j), "a do-while whose condition could not be recovered");
			return j;
		}
		condStart = bestStart;
		AddEntry(d, ENT_OPEN, headOff, Strdup("do {"));
		ctx = PushCtx(d, Off(d, loopEnd), Off(d, condStart));
		d->depth++;
		outerHead = d->loopHead;
		d->loopHead = headOff;
		ParseStmts(d, i, condStart);
		d->loopHead = outerHead;
		d->depth--;
		PopCtx(d);
		AddEntry(d, ENT_CLOSE, Off(d, condStart), Format("} while (%s);", best));
		free(best);
		NoteLoop(d, headOff, Off(d, loopEnd));
		return loopEnd;
	}
	(void)ctx;
}

static uint32_t ParseStmts(Dec_t* d, uint32_t i, uint32_t end)
{
	while(i < end && !d->failed)
	{
		uint32_t off = Off(d, i), target, j;
		int cc;
		if(d->opt->verbose > 1)
			fprintf(stderr, "    stmt at 0x%x depth %d sp %d\n", off, d->depth, d->sp);
		// a loop head: a label that a later instruction of the range jumps back to (the body of the
		// loop being parsed starts at its head, which is not another loop)
		if(IsLabelled(d, off) && off != d->loopHead && !IsNoLoop(d, off) && (j = LastBackJump(d, i, end, off)) != UINT32_MAX && j + 2 <= end)
		{
			FlushStatements(d, off);
			i = ParseLoop(d, i, j, end);
			continue;
		}
		if(IsLabelled(d, off) && i != d->bodyIdx)
		{
			FlushStatements(d, off);
			AddEntry(d, ENT_LABEL, off, NULL);
		}
		ResetStack(d);
		d->curIdx = i;
		j = Run(d, i, end);
		if(d->failed)
			break;
		if(j >= end)
		{
			FlushStatements(d, off);
			return j;
		}
		if(IsJump(d, j, &target))
		{
			JumpStatement(d, j, target);
			i = j + 2;
			continue;
		}
		if(IsJcc(d, j, &target, &cc))
		{
			uint32_t r;
			if(d->sp < 1)
			{
				Fail(d, Off(d, j), "a conditional jump without a condition");
				break;
			}
			// the condition is on top; what is below are earlier statements
			{
				Expr_t cond = Pop(d, Off(d, j));
				FlushStatements(d, Off(d, j));
				Push(d, cond.text, cond.prec, cond.kind, cond.aux);
				d->stack[d->sp - 1].startIdx = cond.startIdx;
			}
			r = ParseIf(d, j, end, 1);
			if(r == j)
			{
				Fail(d, Off(d, j), "a conditional jump that is no construct");
				break;
			}
			i = r;
			continue;
		}
		if(IsLabelled(d, Off(d, j)))
		{ // stopped at a label: the values so far are statements
			FlushStatements(d, Off(d, j));
			i = j;
			continue;
		}
		if(j == i)
		{
			Fail(d, off, "no progress at 0x%x", off);
			break;
		}
		i = j;
	}
	return i;
}

// ---- jumps and labels ---------------------------------------------------------------------------

/* every ENT_JUMP becomes break, continue or goto (with a label entry
 * where it lands); labels nobody jumps to are dropped */
static void ResolveJumps(Dec_t* d, int firstEntry)
{
	int e, j;
	for(e = firstEntry; e < d->entCount; e++)
	{
		Entry_t* en = &d->ent[e];
		const char* what = NULL;
		int c;
		if(en->kind != ENT_JUMP)
			continue;
		c = en->ctx;
		if(c >= 0 && en->jumpTarget == d->ctx[c].breakTarget)
			what = "break";
		else if(c >= 0 && en->jumpTarget == d->ctx[c].continueTarget)
			what = "continue";
		if(!what)
		{
			// a goto: the label entry at the target (the first entry whose offset is the target)
			int found = 0;
			for(j = firstEntry; j < d->entCount; j++)
				if(d->ent[j].off == en->jumpTarget && d->ent[j].kind != ENT_CLOSE)
				{
					if(d->ent[j].kind != ENT_LABEL)
					{
						Entry_t lab;
						memset(&lab, 0, sizeof lab);
						lab.kind = ENT_LABEL;
						lab.off = en->jumpTarget;
						lab.depth = d->ent[j].depth;
						lab.ctx = -1;
						lab.text = Format("L_%04x:", (unsigned)en->jumpTarget);
						if(d->entCount == d->entCap)
						{
							d->entCap *= 2;
							d->ent = (Entry_t*)realloc(d->ent, (size_t)d->entCap * sizeof *d->ent);
							en = &d->ent[e];
						}
						memmove(&d->ent[j + 1], &d->ent[j], (size_t)(d->entCount - j) * sizeof *d->ent);
						d->ent[j] = lab;
						d->entCount++;
						if(j <= e)
							en = &d->ent[++e];
					}
					else if(!d->ent[j].text)
						d->ent[j].text = Format("L_%04x:", (unsigned)en->jumpTarget);
					found = 1;
					break;
				}
			if(!found)
			{
				/* a jump into a loop (from outside, or from a nested loop into the condition of a
				 * do-while): the innermost loop around the target is better written with labels and gotos */
				int k;
				uint32_t span = UINT32_MAX;
				for(k = 0; k < d->loopCount; k++)
					if(en->jumpTarget >= d->loopFrom[k] && en->jumpTarget < d->loopTo[k] && d->loopTo[k] - d->loopFrom[k] < span)
					{
						d->badLoop = d->loopFrom[k];
						span = d->loopTo[k] - d->loopFrom[k];
					}
				Fail(d, en->off, "a jump to 0x%x, which is inside a statement", (unsigned)en->jumpTarget);
				return;
			}
		}
		{
			char* jump = what ? Format("%s;", what) : Format("goto L_%04x;", (unsigned)en->jumpTarget);
			if(en->text)
			{ // "if (c) __jump__;"
				char* p = strstr(en->text, "__jump__;");
				char* out;
				*p = 0;
				out = Format("%s%s", en->text, jump);
				free(en->text);
				free(jump);
				en->text = out;
			}
			else
				en->text = jump;
			en->kind = ENT_TEXT;
		}
	}
	// labels nobody mentions
	for(e = firstEntry; e < d->entCount; e++)
		if(d->ent[e].kind == ENT_LABEL && !d->ent[e].text)
		{
			memmove(&d->ent[e], &d->ent[e + 1], (size_t)(d->entCount - e - 1) * sizeof *d->ent);
			d->entCount--;
			e--;
		}
}

// ---- functions --------------------------------------------------------------------------------------

static void FreeEntries(Dec_t* d, int from)
{
	int i;
	for(i = from; i < d->entCount; i++)
		free(d->ent[i].text);
	d->entCount = from;
}

static int ImmediateValue(const Dec_t* d, uint32_t i, uint32_t* v)
{
	if(IsOp(d, i, OP_PUSH_I8) || IsOp(d, i, OP_PUSH_I16) || IsOp(d, i, OP_PUSH_I32))
	{
		*v = (uint32_t)d->insn[i].v[0];
		return 1;
	}
	return 0;
}

// decode the instructions of a function into d->insn (the map says where the code is)
static int LoadFunction(Dec_t* d, const Func_t* f)
{
	uint32_t off = f->start, cap = 64;
	d->f = f;
	d->n = 0;
	d->insn = (AsmInsn_t*)malloc(cap * sizeof *d->insn);
	d->idxAt = (uint32_t*)malloc(((size_t)(f->end - f->start) + 1) * sizeof *d->idxAt);
	memset(d->idxAt, 0xff, ((size_t)(f->end - f->start) + 1) * sizeof *d->idxAt);
	while(off < f->end)
	{
		if(d->map->kind[off] != DIS_K_CODE)
			return 0;
		if(d->n == cap)
		{
			cap *= 2;
			d->insn = (AsmInsn_t*)realloc(d->insn, cap * sizeof *d->insn);
		}
		if(!Asm_Decode(d->map->area, d->map->size, off, d->gen, &d->insn[d->n]))
			return 0;
		d->idxAt[off - f->start] = d->n;
		off += d->insn[d->n].len;
		d->n++;
	}
	return 1;
}

static void UnloadFunction(Dec_t* d)
{
	free(d->insn);
	free(d->idxAt);
	free(d->params);
	free(d->locals);
	d->insn = NULL;
	d->idxAt = NULL;
	d->params = NULL;
	d->locals = NULL;
	d->localCount = 0;
}

// the prologue and the epilogue: the frame size and the parameters; 0 when the function has no such shape
static int ParseFrame(Dec_t* d)
{
	uint32_t F, F2, k;
	if(d->n < 9 || !IsOp(d, 0, OP_PUSH_FP) || !ImmediateValue(d, 1, &F) || !IsOp(d, 2, OP_ADD) || !IsOp(d, 3, OP_SET_FP))
		return 0;
	if(!IsOp(d, d->n - 5, OP_PUSH_FP) || !ImmediateValue(d, d->n - 4, &F2) || !IsOp(d, d->n - 3, OP_SUB) || !IsOp(d, d->n - 2, OP_SET_FP) || !IsOp(d, d->n - 1, OP_RET) || F2 != F)
		return 0;
	d->exitIdx = d->n - 5;
	k = 4;
	((Func_t*)d->f)->params = 0;
	d->params = (uint32_t*)malloc((d->n + 1) * sizeof *d->params);
	while(k + 1 < d->exitIdx && IsOp(d, k, OP_LOCAL_ADDR) && IsOp(d, k + 1, OP_STORE) && d->insn[k + 1].v[0] == 2)
	{
		d->params[((Func_t*)d->f)->params++] = (uint32_t)d->insn[k].v[0];
		k += 2;
	}
	{ /* the parameters lie at the bottom of the frame, the first one highest: the pops go up by 4
	   * and end at the frame's bottom (n == F); pops that do not are statements (`l <- __pop`) */
		int i, e = -1, n = d->f->params;
		for(i = 0; i < n; i++)
			if(d->params[i] == F)
			{
				e = i;
				break;
			}
		for(i = 0; i < e; i++)
			if(d->params[i] != F - 4 * (uint32_t)(e - i))
				e = -1;
		((Func_t*)d->f)->params = e + 1;
		k = 4 + 2 * (uint32_t)(e + 1);
	}
	d->bodyIdx = k;
	((Func_t*)d->f)->frame = F;
	{ // the parameters are locals too, used or not
		int i;
		for(i = 0; i < d->f->params; i++)
			LocalAt(d, d->params[i], 1);
	}
	return 1;
}

// the declarations of the locals (after the parameters), as one line
static char* Declarations(Dec_t* d)
{
	char* out = Strdup("");
	int i;
	uint32_t bottom = d->localCount ? d->f->frame - d->locals[0].n : d->f->frame;
	if(bottom > 0 && bottom <= d->f->frame)
	{ // nothing refers to the lowest part of the frame: unused variables, declared as padding
		char decl[48];
		snprintf(decl, sizeof decl, " char __pad[%u];", (unsigned)bottom);
		free(out);
		out = Strdup(decl);
	}
	for(i = 0; i < d->localCount; i++)
	{
		const Local_t* l = &d->locals[i];
		char name[32], decl[96];
		char* more;
		int p = ParamIndex(d, l->n);
		LocalName(d, l->n, name, sizeof name);
		if(p >= 0)
		{
			if(l->size > 4)
			{ // a gap after a parameter: padding
				snprintf(decl, sizeof decl, " char %s_[%u];", name, (unsigned)(l->size - 4));
				more = Format("%s%s", out, decl);
				free(out);
				out = more;
			}
			continue;
		}
		if(l->isArray)
		{
			uint32_t elems = l->size / (uint32_t)l->elem;
			if(elems * (uint32_t)l->elem != l->size)
			{
				snprintf(decl, sizeof decl, " char %s[%u];", name, (unsigned)l->size);
			}
			else
				snprintf(decl, sizeof decl, " %s %s[%u];", SizeType(l->elem == 1 ? 0 : l->elem == 2 ? 1
																									: 2),
					name, (unsigned)elems);
		}
		else
		{
			snprintf(decl, sizeof decl, " %s %s;", SizeType(l->elem == 1 ? 0 : l->elem == 2 ? 1
																							: 2),
				name);
			if(l->size > (uint32_t)l->elem)
			{
				char pad[48];
				snprintf(pad, sizeof pad, " char %s_[%u];", name, (unsigned)(l->size - (uint32_t)l->elem));
				strncat(decl, pad, sizeof decl - strlen(decl) - 1);
			}
		}
		more = Format("%s%s", out, decl);
		free(out);
		out = more;
	}
	return out;
}

// the listing of a function (or any range) as an __asm block, strings inline
static void WriteAsmBlock(Dec_t* d, uint32_t start, uint32_t end, FILE* out, int depth)
{
	FILE* mem = tmpfile();
	char line[0x1000];
	if(!mem)
		return;
	Dis_WriteRange(d->map, start, end, d->gen, NULL, mem);
	fseek(mem, 0, SEEK_SET);
	fprintf(out, "%*s__asm {\n", depth * 4, "");
	while(fgets(line, sizeof line, mem))
	{
		char* nl = strchr(line, '\n');
		char* comment;
		char own[40];
		if(nl)
			*nl = 0;
		// the function's own label is what the compiler writes for its name
		Dis_LabelName(d->map, start, own, sizeof own);
		if(strncmp(line, own, strlen(own)) == 0 && line[strlen(own)] == ':')
			continue;
		// a data reference becomes the literal itself (the compiler places it in the table in turn)
		if(strncmp(line, "\tpush_code_addr ", 16) == 0)
		{
			uint32_t target = (uint32_t)strtoul(line + 16 + 4, NULL, 16);
			char* lit = StringLiteral(d, target);
			if(lit)
			{
				fprintf(out, "%*s    push_code_addr %s\n", depth * 4, "", lit);
				free(lit);
				continue;
			}
		}
		comment = strstr(line, "              ; ");
		if(comment)
			*comment = 0;
		{ // a label that names a function start is written as the function's name
			char* p = strstr(line, "loc_");
			if(!p)
				p = strstr(line, "sub_");
			if(p && isxdigit((unsigned char)p[4]))
			{
				uint32_t target = (uint32_t)strtoul(p + 4, NULL, 16);
				if(FuncAt(d, target))
				{
					char name[32], rest[0x1000];
					FuncName(d, target, name, sizeof name);
					snprintf(rest, sizeof rest, "%s", p + 8);
					snprintf(p, sizeof line - (size_t)(p - line), "%s%s", name, rest);
				}
			}
		}
		if(line[0] == '\t')
			fprintf(out, "%*s    %s\n", depth * 4, "", line + 1);
		else
			fprintf(out, "%*s%s\n", depth * 4, "", line);
	}
	fprintf(out, "%*s}\n", depth * 4, "");
	fclose(mem);
}

static void WriteEntries(Dec_t* d, int from, FILE* out)
{
	int i;
	for(i = from; i < d->entCount; i++)
	{
		const Entry_t* e = &d->ent[i];
		if(!e->text)
			fprintf(out, "%*s// %s to 0x%x at 0x%x\n", (e->depth + 1) * 4, "", e->kind == ENT_JUMP ? "jump" : "entry", (unsigned)e->jumpTarget,
				(unsigned)e->off);
		else if(e->kind == ENT_LABEL)
			fprintf(out, "%s\n", e->text);
		else
			fprintf(out, "%*s%s\n", (e->depth + 1) * 4, "", e->text);
	}
}

/* decompile one function into entries and write it; 1 when written as
 * source, 0 when as an __asm block */
static int WriteFunction(Dec_t* d, Func_t* f, FILE* out, int forceAsm)
{
	int first = d->entCount, ok = 0, i;
	char name[32];
	FuncName(d, f->start, name, sizeof name);
	if(!forceAsm && LoadFunction(d, f) && ParseFrame(d))
	{
		int pass, attempt;
		d->noLoopCount = 0;
		for(attempt = 0; attempt < (int)(sizeof d->noLoop / sizeof d->noLoop[0]); attempt++)
		{
			d->badLoop = UINT32_MAX;
			for(pass = 0; pass < 2; pass++)
			{
				int k;
				d->failed = 0;
				d->depth = 0;
				d->ctxCount = 0;
				d->curCtx = -1;
				d->loopHead = UINT32_MAX;
				d->loopCount = 0;
				ResetStack(d);
				FreeEntries(d, first);
				for(k = 0; k < d->localCount; k++)
					d->locals[k].sizes[0] = d->locals[k].sizes[1] = d->locals[k].sizes[2] = d->locals[k].pushes = 0;
				ParseStmts(d, d->bodyIdx, d->exitIdx);
				if(pass == 0)
				{ // the first pass only counts the accesses of the locals; the declarations decide how they are written
					ResetStack(d);
					DecideLocals(d);
				}
			}
			if(!d->failed)
			{
				// the value before the exit is the return value
				if(d->sp > 0 && d->stack[d->sp - 1].kind != E_ASSIGN && d->stack[d->sp - 1].kind != E_CALL)
				{
					Expr_t v = Pop(d, 0);
					FlushStatements(d, 0);
					AddEntry(d, ENT_TEXT, Off(d, v.startIdx), Format("return %s;", v.text));
					free(v.text);
				}
				else
					FlushStatements(d, 0);
				ResolveJumps(d, first);
			}
			if(!d->failed || d->badLoop == UINT32_MAX)
				break;
			// again, with the blamed loop as labels and gotos
			if(d->opt->verbose)
				fprintf(stderr, "%s: at 0x%x: %s; again with the loop at 0x%x as gotos\n", name, (unsigned)d->failOff, d->failMsg,
					(unsigned)d->badLoop);
			d->noLoop[d->noLoopCount++] = d->badLoop;
		}
		ok = !d->failed;
		if(!ok && d->opt->verbose)
			fprintf(stderr, "%s: at 0x%x: %s\n", name, (unsigned)d->failOff, d->failMsg);
		if(!ok && d->opt->verbose > 1)
		{ // the trace: what was built before the failure
			fprintf(stderr, "---- %s as far as it went:\n", name);
			WriteEntries(d, first, stderr);
			fprintf(stderr, "----\n");
		}
	}
	else if(!forceAsm)
	{
		if(d->opt->verbose)
			fprintf(stderr, "%s: no frame prologue or epilogue\n", name);
	}
	if(ok)
	{
		char* params = Strdup("");
		char* decls;
		for(i = 0; i < f->params; i++)
		{
			char* more = Format("%s%sp%d", params, i ? ", " : "", i + 1);
			free(params);
			params = more;
		}
		decls = Declarations(d);
		fprintf(out, "%s(%s) {%s\n", name, params, decls);
		free(params);
		free(decls);
		WriteEntries(d, first, out);
		fprintf(out, "}\n\n");
	}
	else
	{
		fprintf(out, "%s() {\n", name);
		WriteAsmBlock(d, f->start, f->end, out, 1);
		fprintf(out, "}\n\n");
	}
	FreeEntries(d, first);
	ResetStack(d);
	UnloadFunction(d);
	return ok;
}

// ---- the program -----------------------------------------------------------------------------------

/* the functions: offset 0 and every labelled offset where a frame
 * prologue begins (push_fp; push; add; set_fp), each ending where the
 * next begins or the code ends */
static void FindFunctions(Dec_t* d)
{
	uint32_t off = 0;
	int cap = 16;
	d->funcs = (Func_t*)malloc((size_t)cap * sizeof *d->funcs);
	d->funcCount = 0;
	while(off < d->map->size)
	{
		if(d->map->kind[off] == DIS_K_CODE && (off == 0 || (d->map->label[off] & (DIS_L_JUMP | DIS_L_CALL))))
		{
			AsmInsn_t in;
			int isFunc = off == 0;
			if(!isFunc && Asm_Decode(d->map->area, d->map->size, off, d->gen, &in) && in.fam == 0 && in.op == OP_PUSH_FP)
			{
				uint32_t o2 = off + in.len;
				AsmInsn_t in2, in3, in4;
				if(Asm_Decode(d->map->area, d->map->size, o2, d->gen, &in2) && in2.fam == 0 && in2.op <= OP_PUSH_I32 && Asm_Decode(d->map->area, d->map->size, o2 + in2.len, d->gen, &in3) && in3.fam == 0 && in3.op == OP_ADD && Asm_Decode(d->map->area, d->map->size, o2 + in2.len + in3.len, d->gen, &in4) && in4.fam == 0 && in4.op == OP_SET_FP)
					isFunc = 1;
			}
			if(isFunc)
			{
				if(d->funcCount == cap)
				{
					cap *= 2;
					d->funcs = (Func_t*)realloc(d->funcs, (size_t)cap * sizeof *d->funcs);
				}
				memset(&d->funcs[d->funcCount], 0, sizeof d->funcs[0]);
				d->funcs[d->funcCount].start = off;
				if(d->funcCount)
					d->funcs[d->funcCount - 1].end = off;
				d->funcCount++;
			}
		}
		if(d->map->kind[off] == DIS_K_CODE)
		{
			uint32_t len = Asm_InsnLength(d->map->area, d->map->size, off, d->gen);
			off += len ? len : 1;
		}
		else
			off++;
	}
	// the last function ends where the data begins
	if(d->funcCount)
	{
		uint32_t e = d->funcs[d->funcCount - 1].start;
		while(e < d->map->size && d->map->kind[e] != DIS_K_DATA)
			e++;
		d->funcs[d->funcCount - 1].end = e;
	}
}

// the prologues of every function, for the arities of the calls
static void ParseAllFrames(Dec_t* d)
{
	int i;
	for(i = 0; i < d->funcCount; i++)
	{
		Func_t* f = &d->funcs[i];
		f->params = 0;
		if(LoadFunction(d, f) && ParseFrame(d))
			f->prologueOk = 1;
		UnloadFunction(d);
	}
}

// write the whole program: the heading, the functions (asm for those in `asmMask`)
// the lines written to `out` so far (a temporary file: counted from its content)
static int LinesSoFar(FILE* out)
{
	long pos = ftell(out), i;
	int lines = 1;
	char buf[0x1000];
	fseek(out, 0, SEEK_SET);
	for(i = 0; i < pos;)
	{
		size_t n = fread(buf, 1, (size_t)(pos - i) < sizeof buf ? (size_t)(pos - i) : sizeof buf, out);
		size_t k;
		if(!n)
			break;
		for(k = 0; k < n; k++)
			if(buf[k] == '\n')
				lines++;
		i += (long)n;
	}
	fseek(out, pos, SEEK_SET);
	return lines;
}

static int WriteProgram(Dec_t* d, const uint8_t* asmMask, FILE* out)
{
	int i, fallbacks = 0;
	fprintf(out, "// %s - decompiled by bpasm (docs/bpc.md)\n", d->opt->name ? d->opt->name : "program");
	fprintf(out, "#engine %s\n%s\n", d->gen == GEN_COUNT ? "any" : kGenNames[d->gen], gAsmUtf8 ? "#encoding utf-8\n" : "");
	for(i = 0; i < d->funcCount; i++)
	{
		if(d->funcLine)
			d->funcLine[i] = LinesSoFar(out);
		if(!WriteFunction(d, &d->funcs[i], out, asmMask[i]))
			fallbacks++;
	}
	return fallbacks;
}

// the function that holds a line of the text written last (-1: none)
static int FuncOfLine(const Dec_t* d, int line)
{
	int i, best = -1;
	for(i = 0; i < d->funcCount; i++)
		if(d->funcLine[i] <= line)
			best = i;
	return best;
}

static char* WriteToBuffer(Dec_t* d, const uint8_t* asmMask, size_t* len, int* fallbacks)
{
	FILE* mem = tmpfile();
	char* text;
	long n;
	if(!mem)
		return NULL;
	*fallbacks = WriteProgram(d, asmMask, mem);
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

int Bpc_Decompile(const uint8_t* file, uint32_t size, const BpcDecOptions_t* opt, FILE* out)
{
	Dec_t d;
	DisMap_t map;
	uint8_t* asmMask;
	int fallbacks = 0, whole = 0;
	memset(&d, 0, sizeof d);
	if(!Dis_Analyze(file, size, opt->gen, NULL, &map))
		return -1;
	d.map = &map;
	d.gen = opt->gen;
	d.opt = opt;
	d.curCtx = -1;
	FindFunctions(&d);
	ParseAllFrames(&d);
	asmMask = (uint8_t*)calloc((size_t)d.funcCount + 1, 1);
	d.funcLine = (int*)calloc((size_t)d.funcCount + 1, sizeof *d.funcLine);
	if(map.problems)
		whole = 1; // undefined opcodes or overlapping flows: the listing is the only exact form
	for(;;)
	{
		size_t len = 0;
		char* text;
		BpcResult_t res;
		int again = 0;
		if(whole)
			break;
		text = WriteToBuffer(&d, asmMask, &len, &fallbacks);
		if(opt->noVerify)
		{
			fputs(text, out);
			free(text);
			break;
		}
		{
			FILE* log = tmpfile();
			Bpc_Compile(text, len, opt->name ? opt->name : "program", opt->gen, NULL, log ? log : stderr, &res);
			if(res.errors && log)
			{ // the first message names a line: the function there is the one that failed
				char msg[0x400];
				int line = 0, fi;
				fseek(log, 0, SEEK_SET);
				if(fgets(msg, sizeof msg, log))
				{
					const char* colon = strchr(msg, ':');
					if(colon)
						line = atoi(colon + 1);
					if(opt->verbose)
						fputs(msg, stderr);
				}
				fi = FuncOfLine(&d, line);
				if(fi >= 0 && !asmMask[fi])
				{
					asmMask[fi] = 1;
					again = 1;
				}
				else
					whole = 1;
				fclose(log);
				free(text);
				if(!again)
					break;
				continue;
			}
			if(log)
				fclose(log);
		}
		if(res.errors == 0)
		{
			/* which function differs: the first whose compiled size is not its
			 * original size, else (every function in place) the first whose
			 * bytes differ - unless the string table itself differs, which
			 * changes the references inside the functions */
			int culprit = -1, k;
			uint32_t codeSize = res.size >= 16 ? res.size - 16 : 0;
			const uint8_t* area = res.file + 16;
			uint32_t strStart = codeSize, origStrStart = d.funcCount ? d.funcs[d.funcCount - 1].end : map.size;
			uint32_t* starts = (uint32_t*)malloc(((size_t)d.funcCount + 1) * sizeof *starts);
			for(k = 0; k < res.labelCount; k++)
				if(strcmp(res.labels[k].name, "__codeend") == 0)
					strStart = res.labels[k].off; // the end of the last function (the padding and the strings follow)
			for(k = 0; k < d.funcCount; k++)
			{
				char name[32];
				int j;
				FuncName(&d, d.funcs[k].start, name, sizeof name);
				starts[k] = UINT32_MAX;
				for(j = 0; j < res.labelCount; j++)
					if(strcmp(res.labels[j].name, name) == 0)
						starts[k] = res.labels[j].off;
			}
			for(k = 0; k < d.funcCount && culprit < 0; k++)
			{
				uint32_t origSize = d.funcs[k].end - d.funcs[k].start;
				uint32_t next = k + 1 < d.funcCount ? starts[k + 1] : strStart;
				if(starts[k] == UINT32_MAX || next == UINT32_MAX || next < starts[k] || next - starts[k] != origSize)
					culprit = k;
			}
			if(culprit < 0)
			{
				// every function in place: the trailer (padding and strings) first, then the functions' bytes
				uint32_t newCodeEnd = d.funcCount ? starts[d.funcCount - 1] + (d.funcs[d.funcCount - 1].end - d.funcs[d.funcCount - 1].start) : 0;
				uint32_t origTail = map.size - origStrStart, newTail = codeSize >= newCodeEnd ? codeSize - newCodeEnd : 0;
				if(origTail != newTail || memcmp(map.area + origStrStart, area + newCodeEnd, origTail) != 0)
					whole = 1;
				else
					for(k = 0; k < d.funcCount && culprit < 0; k++)
						if(memcmp(map.area + d.funcs[k].start, area + starts[k], d.funcs[k].end - d.funcs[k].start) != 0)
							culprit = k;
			}
			free(starts);
			if(culprit < 0 && !whole)
			{
				fputs(text, out);
				free(text);
				BGI_Free(res.file);
				BGI_Free(res.labels);
				break;
			}
			if(opt->verbose)
				fprintf(stderr, "%s: the compiled program differs in %s\n", opt->name ? opt->name : "program",
					whole ? "the string table" : "a function");
			if(culprit >= 0 && !asmMask[culprit])
			{
				if(opt->verbose)
				{
					char name[32];
					FuncName(&d, d.funcs[culprit].start, name, sizeof name);
					fprintf(stderr, "%s: %s does not compile back to its bytes\n", opt->name ? opt->name : "program", name);
				}
				asmMask[culprit] = 1;
				again = 1;
			}
			else
				whole = 1;
			BGI_Free(res.file);
			BGI_Free(res.labels);
		}
		else
		{
			if(opt->verbose)
				fprintf(stderr, "%s: the decompiled source does not compile\n", opt->name ? opt->name : "program");
			whole = 1;
		}
		free(text);
		if(!again)
			break;
	}
	if(whole)
	{
		fprintf(out, "// %s - decompiled by bpasm (docs/bpc.md)\n", opt->name ? opt->name : "program");
		fprintf(out, "#engine %s\n%s\n", d.gen == GEN_COUNT ? "any" : kGenNames[d.gen], gAsmUtf8 ? "#encoding utf-8\n" : "");
		fprintf(out, "__program {\n");
		{
			FILE* mem = tmpfile();
			char line[0x1000];
			if(mem)
			{
				Dis_WriteRange(&map, 0, map.size, d.gen, NULL, mem);
				fseek(mem, 0, SEEK_SET);
				while(fgets(line, sizeof line, mem))
					fputs(line, out);
				fclose(mem);
			}
		}
		fprintf(out, "}\n");
		fallbacks = d.funcCount ? d.funcCount : 1;
	}
	free(asmMask);
	free(d.funcs);
	free(d.funcLine);
	free(d.ent);
	Dis_FreeMap(&map);
	return fallbacks;
}
