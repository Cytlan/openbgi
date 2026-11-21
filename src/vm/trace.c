/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * trace.c - development aids of the script VM, switched on through the
 *           environment (src/include/bgi/vm.h); none of this is part of the
 *           original engine
 *
 *   BGI_TRACE=1            print every executed instruction to stderr:
 *                          time, thread, program, program-relative IP,
 *                          the opcode bytes, the frame pointer and the
 *                          three values on top of the stack
 *   BGI_TRACE=a._bp,b._bp  the same for those programs only
 *   BGI_TRACE_FROM=ms      start printing that many milliseconds in
 *   BGI_TRACE_OPS=90:50,.. print those instructions only ("main:16" for a
 *                          base opcode), with eight stack values and the
 *                          result the instruction leaves on the stack
 *   BGI_TRACE_LAST=N       print nothing; keep the last N instructions and
 *                          dump them when an error message box comes up
 *   BGI_OPSTAT=file        count the executed opcodes per family and write
 *                          "family opcode count" lines to the file at exit
 *
 * The scheduler (sched.c) calls Vm_TraceInit once, then Vm_TraceBefore and
 * Vm_TraceAfter around every instruction while o->trace.vmTraceOn is set,
 * and Vm_TraceShutdown when the machine stops; MsgBox (error.c) calls
 * Vm_TraceDump before it shows any message box, so the ring comes out on
 * every error.  See docs/dev/vm.md, "Development aids".
 */
#include "bgi/vm.h"
#include "bgi/engine.h"
#include "bgi/host.h"
#include "bgi/core.h"

// ---- the ring of the last instructions (BGI_TRACE_LAST) ----------------------------------

/* The ring holds the raw values; the module names are looked up only when
 * it is printed, so recording costs little. */
typedef struct TraceEntry
{
	uint32_t ms, opIP, fp, s0, s1, s2; // time since the first instruction, instruction offset, frame pointer, the three stack-top values
	int tid;                           // thread id
	uint8_t code[4];                   // the instruction's first four bytes
} TraceEntry_t;

// ---- opcode statistics (BGI_OPSTAT) ---------------------------------------------------------

// o->trace.vmStat[0] counts the first byte of every instruction, o->trace.vmStat[f] the
// second byte of the instructions whose first byte is kStatFamily[f]
static const uint8_t kStatFamily[8] = {0x00, 0x80, 0x90, 0x91, 0x92, 0xa0, 0xb0, 0xc0};

// ---- helpers ----------------------------------------------------------------------------------

/* The name of the module that holds code offset opIP of thread t (into buf,
 * 0x40 bytes) and, in *rel, the offset relative to that module's start.
 * Without a thread or a module buf gets "-" and *rel the offset as given.
 * Returns buf. */
static const char* TraceModule(Thread_t* t, uint32_t opIP, uint32_t* rel, char* buf)
{
	ModuleNode_t* mods;
	uint32_t count = t ? Thread_ExportModules(t, &mods) : 0;
	*rel = opIP;
	strcpy(buf, "-");
	if(count > 0)
	{
		ModuleNode_t* m = mods;
		while(m->base > opIP)
			m++;
		snprintf(buf, 0x40, "%s", m->name);
		*rel = opIP - m->base;
		BGI_Free(mods);
	}
	return buf;
}

// Is `name` one of the comma-separated programs of BGI_TRACE?
static int TraceMatches(OpenBGI_t* o, const char* name)
{
	const char* p = o->trace.vmTraceProg;
	size_t n = strlen(name);
	while(*p)
	{
		const char* e = strchr(p, ',');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		if(len == n && memcmp(p, name, n) == 0)
			return 1;
		if(!e)
			break;
		p = e + 1;
	}
	return 0;
}

/* The family index (OpFamily_t) of the instruction at `code` and, in *op,
 * its opcode within that family: the second byte after a family lead byte,
 * the first byte itself for a main-table instruction. */
static int TraceFamilyOf(const uint8_t* code, int* op)
{
	static const uint8_t kFam[8] = {OPFAM_7F, OPFAM_80, OPFAM_81, OPFAM_90, OPFAM_91, OPFAM_92, OPFAM_A0, OPFAM_B0};
	static const uint8_t kLead[8] = {0x7f, 0x80, 0x81, 0x90, 0x91, 0x92, 0xa0, 0xb0};
	int i;
	*op = code[1];
	if(code[0] == 0xc0)
		return OPFAM_C0;
	if(code[0] == 0xd0)
		return OPFAM_D0;
	if(code[0] == 0xe0)
		return OPFAM_E0;
	if(code[0] == 0xef)
		return OPFAM_EF;
	for(i = 0; i < 8; i++)
		if(code[0] == kLead[i])
			return kFam[i];
	*op = code[0];
	return OPFAM_MAIN;
}

/* Parse BGI_TRACE_OPS ("90:50,90:51,main:16": family name, colon, opcode
 * in hex) into o->trace.vmTraceOps; an entry with an unknown family name
 * is ignored.  One valid entry switches the selective trace on. */
static void TraceParseOps(OpenBGI_t* o, const char* p)
{
	static const char* const kNames[OPFAM_COUNT] = {"main", "7f", "80", "81", "90", "91", "92", "a0", "b0", "c0", "d0", "e0", "ef"};
	while(*p)
	{
		const char* colon = strchr(p, ':');
		const char* end = strchr(p, ',');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		int f;
		if(colon && (size_t)(colon - p) < len)
			for(f = 0; f < OPFAM_COUNT; f++)
				if(strlen(kNames[f]) == (size_t)(colon - p) && strncmp(kNames[f], p, (size_t)(colon - p)) == 0)
				{
					o->trace.vmTraceOps[f][strtoul(colon + 1, NULL, 16) & 0xff] = 1;
					o->trace.vmTraceOpsOn = 1;
					o->trace.vmTrace = 1;
				}
		p += len + (end ? 1 : 0);
	}
}

// ---- the interface used by the scheduler ---------------------------------------------------

/* Read the environment variables of the file header once, before the first
 * instruction, and set o->trace.vmTraceOn when any aid is wanted.  The
 * aids exclude each other in this order: a selection (BGI_TRACE_OPS) wins
 * over the ring (BGI_TRACE_LAST), which wins over the full trace
 * (BGI_TRACE); BGI_OPSTAT counts independently of them. */
void Vm_TraceInit(OpenBGI_t* o)
{
	const char* v = getenv("BGI_TRACE");
	const char* last = getenv("BGI_TRACE_LAST");
	const char* ops = getenv("BGI_TRACE_OPS");
	if(v && !*v)
		v = NULL; // an empty value is the same as none
	if(last && !*last)
		last = NULL;
	o->trace.vmTrace = v != NULL;
	o->trace.vmTraceProg = (v && strcmp(v, "1") != 0) ? v : NULL;
	if(getenv("BGI_TRACE_FROM"))
		o->trace.vmTraceFrom = (uint32_t)atoi(getenv("BGI_TRACE_FROM"));
	o->trace.vmStatFile = getenv("BGI_OPSTAT");
	if(o->trace.vmStatFile && !*o->trace.vmStatFile)
		o->trace.vmStatFile = NULL;
	if(last && atoi(last) > 0)
	{
		o->trace.traceRingSize = atoi(last);
		o->trace.traceRing = (TraceEntry_t*)BGI_Calloc((size_t)o->trace.traceRingSize * sizeof(TraceEntry_t));
		o->trace.vmTrace = 1;
	}
	if(ops && *ops)
		TraceParseOps(o, ops);
	o->trace.vmTraceOn = o->trace.vmTrace || o->trace.vmStatFile != NULL;
}

/* Print or record the instruction `op` thread `t` is about to execute:
 * the opening half of a BGI_TRACE_OPS line (Vm_TraceAfter completes it),
 * a ring entry, or the full BGI_TRACE line.  Nothing before BGI_TRACE_FROM
 * milliseconds have passed since the first traced instruction. */
static void TraceInsn(OpenBGI_t* o, Thread_t* t, uint8_t op)
{
	uint32_t opIP = Thread_CurOpIP(t);
	const uint8_t* code = t->code + opIP;
	// the three values on top of the evaluation stack (sp wraps)
	uint32_t n = t->stackEntries;
	uint32_t s0 = t->stack[(t->sp + n - 1) % n], s1 = t->stack[(t->sp + n - 2) % n], s2 = t->stack[(t->sp + n - 3) % n];
	uint32_t now = Clock_HostMs(o), t0;
	char name[0x40];
	uint32_t rel;
	if(!o->trace.traceT0)
		o->trace.traceT0 = now;
	t0 = o->trace.traceT0;
	if(now - t0 < o->trace.vmTraceFrom)
		return;
	if(o->trace.vmTraceOpsOn)
	{
		int fam, opc, i;
		fam = TraceFamilyOf(code, &opc);
		if(!o->trace.vmTraceOps[fam][opc])
			return;
		TraceModule(t, opIP, &rel, name);
		fprintf(stderr, "%7u T%d %-12s %06x: %02x %02x  fp=%x  [", (unsigned)(now - t0), (int)Thread_GetId(t), name,
			(unsigned)rel, op, code[1], (unsigned)Thread_GetFP(t));
		for(i = 1; i <= 8 && (uint32_t)i <= n; i++)
			fprintf(stderr, "%s%x", i > 1 ? " " : "", (unsigned)t->stack[(t->sp + n - (uint32_t)i) % n]);
		fprintf(stderr, "]");
		o->trace.vmTraceOpsPending = 1; // the result follows once the instruction ran
		return;
	}
	if(o->trace.traceRing)
	{
		TraceEntry_t* e = &o->trace.traceRing[o->trace.traceRingPos];
		e->ms = now - t0;
		e->tid = (int)Thread_GetId(t);
		e->opIP = opIP;
		e->fp = Thread_GetFP(t);
		e->s0 = s0;
		e->s1 = s1;
		e->s2 = s2;
		memcpy(e->code, code, 4);
		if(++o->trace.traceRingPos == o->trace.traceRingSize)
		{
			o->trace.traceRingPos = 0;
			o->trace.traceRingFull = 1;
		}
		return;
	}
	TraceModule(t, opIP, &rel, name);
	if(!o->trace.vmTraceProg || TraceMatches(o, name))
		fprintf(stderr, "%7u T%d %-12s %06x (%06x): %02x %02x %02x %02x  fp=%x  [%x %x %x]\n", (unsigned)(now - t0),
			(int)Thread_GetId(t), name, (unsigned)rel, (unsigned)opIP, op, code[1], code[2], code[3],
			(unsigned)Thread_GetFP(t), (unsigned)s0, (unsigned)s1, (unsigned)s2);
}

// Count the instruction for BGI_OPSTAT: its first byte, and its second one for the families of kStatFamily.
static void StatInsn(OpenBGI_t* o, Thread_t* t, uint8_t op)
{
	int f;
	o->trace.vmStat[0][op]++;
	for(f = 1; f < 8; f++)
		if(op == kStatFamily[f])
		{
			o->trace.vmStat[f][t->code[Thread_CurOpIP(t) + 1]]++;
			break;
		}
}

// Called before thread `t` executes the instruction `op` (its first byte).
void Vm_TraceBefore(OpenBGI_t* o, Thread_t* t, uint8_t op)
{
	if(o->trace.vmTrace)
		TraceInsn(o, t, op);
	if(o->trace.vmStatFile)
		StatInsn(o, t, op);
}

// Called after the instruction ran: completes the line of BGI_TRACE_OPS with the stack top.
void Vm_TraceAfter(OpenBGI_t* o, Thread_t* t)
{
	if(o->trace.vmTraceOpsPending)
	{
		uint32_t n = t->stackEntries;
		o->trace.vmTraceOpsPending = 0;
		fprintf(stderr, " -> %x\n", (unsigned)t->stack[(t->sp + n - 1) % n]);
	}
}

// Print the ring of BGI_TRACE_LAST to stderr (a no-op without it); called on every error.
void Vm_TraceDump(OpenBGI_t* o)
{
	int i, n = o->trace.traceRingFull ? o->trace.traceRingSize : o->trace.traceRingPos;
	if(!o->trace.traceRing || n == 0)
		return;
	fprintf(stderr, "---- the last %d instructions ----\n", n);
	for(i = 0; i < n; i++)
	{
		int k = o->trace.traceRingFull ? (o->trace.traceRingPos + i) % o->trace.traceRingSize : i;
		const TraceEntry_t* e = &o->trace.traceRing[k];
		Thread_t* t = o->vm.rootThread ? Thread_FindById(o->vm.rootThread, (uint32_t)e->tid) : NULL;
		char name[0x40];
		uint32_t rel;
		TraceModule(t, e->opIP, &rel, name);
		fprintf(stderr, "%7u T%d %-12s %06x (%06x): %02x %02x %02x %02x  fp=%x  [%x %x %x]\n", (unsigned)e->ms, e->tid,
			name, (unsigned)rel, (unsigned)e->opIP, e->code[0], e->code[1], e->code[2], e->code[3], (unsigned)e->fp,
			(unsigned)e->s0, (unsigned)e->s1, (unsigned)e->s2);
	}
	fprintf(stderr, "----\n");
}

// Write the BGI_OPSTAT file, a native path (a no-op without it); called when the machine stops.
void Vm_TraceShutdown(OpenBGI_t* o)
{
	char* text;
	size_t len = 0, cap = 8 * 256 * 18 + 1; // a line per instruction at most: "XX XX <count>\n"
	int f, op;
	if(!o->trace.vmStatFile)
		return;
	text = (char*)malloc(cap);
	if(!text)
		return;
	for(f = 0; f < 8; f++)
		for(op = 0; op < 256; op++)
			if(o->trace.vmStat[f][op])
				len += (size_t)snprintf(text + len, cap - len, "%02X %02X %u\n", kStatFamily[f], op, (unsigned)o->trace.vmStat[f][op]);
	Host_WriteNativeFile(o, o->trace.vmStatFile, text, (uint32_t)len);
	free(text);
}
