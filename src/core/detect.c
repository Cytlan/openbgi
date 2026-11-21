/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * detect.c - which game a directory holds and which engine profile runs
 *            it: the product name, the hash and the opcodes of its
 *            programs; interface in detect.h
 *
 * The programs are read with the disassembler's analysis (asm.h): it
 * follows the flows from the entry and every branch target, so data is
 * never taken for code, and it reads them under one generation at a time
 * - the opcodes that generation has, the encodings it uses.  The games
 * table and the profiles are those of version.h.
 */
#include "bgi/detect.h"
#include "bgi/asm.h"
#include "bgi/arcread.h"
#include "bgi/codec.h"
#include "bgi/host.h"
#include "bgi/fs.h"
#include "bgi/sjis.h"
#include "bgi/core.h"

// ---- the evidence --------------------------------------------------------------------------------

void DetectSet_Init(DetectSet_t* s)
{
	memset(s, 0, sizeof *s);
}

void DetectSet_Free(DetectSet_t* s)
{
	int i;
	for(i = 0; i < s->count; i++)
		free(s->progs[i].data);
	free(s->progs);
	memset(s, 0, sizeof *s);
}

// a compiled program: the header size 0x10, the size of what follows, eight zero bytes
static int IsProgram(const uint8_t* d, uint32_t n)
{
	uint32_t size;
	if(n < 16 || d[0] != 0x10 || d[1] || d[2] || d[3] || memcmp(d + 8, "\0\0\0\0\0\0\0\0", 8) != 0)
		return 0;
	size = d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t)d[7] << 24);
	return size <= n - 16;
}

int DetectSet_Add(OpenBGI_t* o, DetectSet_t* s, const char* name, const uint8_t* data, uint32_t size, int isIpl)
{
	uint8_t* copy;
	DetectProgram_t* p;
	if(size >= sizeof(DscHeader_t) && BGI_IsDsc(data))
	{ // a loose file may be compressed like an archive entry
		uint32_t n = BGI_DscOutSize(data);
		if(n > 0x4000000)
			return 0;
		copy = (uint8_t*)malloc(n + 1);
		if(!copy)
			return 0;
		size = BGI_DscDecode(o, data, copy);
	}
	else
	{
		copy = (uint8_t*)malloc(size + 1);
		if(!copy)
			return 0;
		memcpy(copy, data, size);
	}
	if(!IsProgram(copy, size))
	{
		free(copy);
		return 0;
	}
	if(s->count == s->cap)
	{
		int cap = s->cap ? s->cap * 2 : 64;
		DetectProgram_t* grown = (DetectProgram_t*)realloc(s->progs, (size_t)cap * sizeof *grown);
		if(!grown)
		{
			free(copy);
			return 0;
		}
		s->progs = grown;
		s->cap = cap;
	}
	if(isIpl && !s->haveIpl)
	{ // the boot program goes first
		memmove(s->progs + 1, s->progs, (size_t)s->count * sizeof *s->progs);
		p = &s->progs[0];
		s->haveIpl = 1;
	}
	else
		p = &s->progs[s->count];
	s->count++;
	memset(p, 0, sizeof *p);
	snprintf(p->name, sizeof p->name, "%s", name);
	p->data = copy;
	p->size = size;
	return 1;
}

// every program of an open archive into the set (the archive closed); -1 when it is none
static int AddArc(OpenBGI_t* o, DetectSet_t* s, ArcRead_t* a)
{
	uint32_t i;
	int taken = 0;
	if(!a)
		return -1;
	for(i = 0; i < a->count; i++)
	{
		uint32_t size;
		int dsc;
		uint8_t* data = ArcRead_Load(o, a, &a->entries[i], &size, &dsc);
		if(!data)
			continue;
		taken += DetectSet_Add(o, s, a->entries[i].name, data, size, 0);
		BGI_Free(data);
	}
	ArcRead_Close(a);
	return taken;
}

int DetectSet_AddArchive(OpenBGI_t* o, DetectSet_t* s, ArcRead_t* a)
{
	return AddArc(o, s, a);
}

// an archive of the game read through the engine's file layer
static int FsArcRead(void* ctx, uint32_t off, void* buf, uint32_t n)
{
	FsFile_t* f = (FsFile_t*)ctx;
	return Fs_FileSeek(f, off) && Fs_FileRead(f, buf, n) == n;
}

static void FsArcClose(void* ctx)
{
	Fs_FileClose((FsFile_t*)ctx);
}

// the archive at an engine path, NULL when there is none (or it is no archive)
static ArcRead_t* OpenGameArchive(OpenBGI_t* o, const char* path)
{
	ArcSource_t src;
	FsFile_t* f = Fs_FileOpenRead(o, path);
	if(!f)
		return NULL;
	src.read = FsArcRead;
	src.close = FsArcClose;
	src.ctx = f;
	return ArcRead_OpenReader(&src, path);
}

int DetectSet_AddGameArchive(OpenBGI_t* o, DetectSet_t* s, const char* path)
{
	ArcRead_t* a = Fs_FileAttrs(o, path) == OPENBGI_INVALID_ATTRS ? NULL : OpenGameArchive(o, path);
	return a ? AddArc(o, s, a) : -1;
}

// the spelling of a file name in a directory, matched without regard to ASCII case
typedef struct FindName
{
	const char* want;
	char found[0x200];
} FindName_t;

static void FindNameEntry(void* ctx, const char* name, int isDir)
{
	FindName_t* fn = (FindName_t*)ctx;
	const char* a = name;
	const char* b = fn->want;
	if(isDir || fn->found[0])
		return;
	for(; *a && *b; a++, b++)
	{
		int x = (uint8_t)*a, y = (uint8_t)*b;
		if(x >= 'A' && x <= 'Z')
			x += 32;
		if(y >= 'A' && y <= 'Z')
			y += 32;
		if(x != y)
			return;
	}
	if(!*a && !*b)
		snprintf(fn->found, sizeof fn->found, "%s", name);
}

// `name` in the native directory `dir` into `path` (the directory's spelling of it); 0 when there is no such file
static int DirFile(OpenBGI_t* o, const char* dir, const char* name, char* path, size_t n)
{
	FindName_t fn;
	memset(&fn, 0, sizeof fn);
	fn.want = name;
	Host_NativeList(o, dir, FindNameEntry, &fn);
	if(!fn.found[0])
		return 0;
	if(*dir && !*Host_NativeFilePart(o, dir)) // it ends in a separator
		snprintf(path, n, "%s%s", dir, fn.found);
	else
		snprintf(path, n, "%s%c%s", dir, Host_NativeSep(o), fn.found);
	return 1;
}

// the boot program of the archive at an engine path into the set
static void AddBootFromArchive(OpenBGI_t* o, DetectSet_t* s, const char* path)
{
	ArcRead_t* a = Fs_FileAttrs(o, path) == OPENBGI_INVALID_ATTRS ? NULL : OpenGameArchive(o, path);
	const ArcReadEntry_t* e = a ? ArcRead_Find(a, "ipl._bp") : NULL;
	if(e)
	{
		uint32_t size;
		int dsc;
		uint8_t* data = ArcRead_Load(o, a, e, &size, &dsc);
		if(data)
		{
			DetectSet_Add(o, s, "ipl._bp", data, size, 1);
			BGI_Free(data);
		}
	}
	ArcRead_Close(a);
}

int DetectSet_LoadGame(OpenBGI_t* o, DetectSet_t* s, const char* dir)
{
	char path[0x400];
	uint32_t size;
	uint8_t* data;
	snprintf(path, sizeof path, "%sipl._bp", dir);
	data = Fs_LoadFile(o, path, &size);
	if(data)
	{
		DetectSet_Add(o, s, "ipl._bp", data, size, 1);
		free(data);
	}
	if(!s->haveIpl)
	{
		snprintf(path, sizeof path, "%ssystem.arc", dir);
		AddBootFromArchive(o, s, path);
	}
	snprintf(path, sizeof path, "%ssysprg.arc", dir);
	DetectSet_AddGameArchive(o, s, path);
	return s->count;
}

int DetectSet_LoadDirectory(OpenBGI_t* o, DetectSet_t* s, const char* nativeDir)
{
	char path[0x400];
	if(DirFile(o, nativeDir, "ipl._bp", path, sizeof path))
	{
		uint32_t size;
		uint8_t* data = Host_ReadNativeFile(o, path, &size);
		if(data)
		{
			DetectSet_Add(o, s, "ipl._bp", data, size, 1);
			free(data);
		}
	}
	if(!s->haveIpl && DirFile(o, nativeDir, "system.arc", path, sizeof path))
	{
		ArcRead_t* a = ArcRead_OpenNative(o, path);
		const ArcReadEntry_t* e = a ? ArcRead_Find(a, "ipl._bp") : NULL;
		if(e)
		{
			uint32_t size;
			int dsc;
			uint8_t* data = ArcRead_Load(o, a, e, &size, &dsc);
			if(data)
			{
				DetectSet_Add(o, s, "ipl._bp", data, size, 1);
				BGI_Free(data);
			}
		}
		if(a)
			ArcRead_Close(a);
	}
	if(DirFile(o, nativeDir, "sysprg.arc", path, sizeof path))
	{
		ArcRead_t* a = ArcRead_OpenNative(o, path);
		if(a)
			DetectSet_AddArchive(o, s, a);
	}
	return s->count;
}

// ---- the product name --------------------------------------------------------------------------------

/* The boot programs check the product name the same way in every game
 * that checks it:
 *
 *      push_local_addr N
 *      sys.get_product_name         ; "80 E8": the name into the buffer
 *      push_local_addr N
 *      push_code_addr str           ; the literal
 *      streq                        ; "69"
 *      push_code_off quit / jcc 1
 *
 * so the literal is the string the first `streq` after "80 E8" compares,
 * within a few instructions and before any branch. */
#define PRODUCT_WINDOW 8 // instructions after "80 E8" to look for the comparison in

static int ProductIn(const DisMap_t* m, EngineGen_t gen, char* out, size_t n)
{
	uint32_t off;
	for(off = 0; off < m->size; off++)
	{
		AsmInsn_t in;
		uint32_t p, literal = 0, len = 0;
		int k;
		if(m->kind[off] != DIS_K_CODE || !Asm_Decode(m->area, m->size, off, gen, &in) || in.fam != 0x80 || in.op != 0xe8)
			continue;
		for(p = off + in.len, k = 0; p < m->size && k < PRODUCT_WINDOW; k++)
		{
			AsmInsn_t next;
			if(m->kind[p] != DIS_K_CODE || !Asm_Decode(m->area, m->size, p, gen, &next))
				break;
			if(next.fam == 0 && next.op == 0x05 && next.targetKind == 2 && next.target < m->size)
			{ // push_code_addr: a literal when a string lies there
				uint32_t l = Dis_StringLength(m, next.target);
				if(l > 0 && l < n)
				{
					literal = next.target;
					len = l;
				}
			}
			else if(next.fam == 0 && next.op == 0x69)
			{ // streq: the comparison
				if(len)
				{
					memcpy(out, m->area + literal, len);
					out[len] = 0;
					return 1;
				}
				break;
			}
			else if(next.fam == 0 && (next.op == 0x14 || next.op == 0x15 || next.op == 0x16 || next.op == 0x17))
				break; // a jump, call or return: the check is not this one
			p += next.len;
		}
	}
	return 0;
}

int Detect_ProductName(const uint8_t* file, uint32_t size, char* out, size_t n)
{
	// "any" reads every build but the 1.667+ encodings; the newest generation reads those
	static const EngineGen_t kGens[] = {GEN_COUNT, GEN_LAST};
	size_t i;
	for(i = 0; i < BGI_COUNTOF(kGens); i++)
	{
		DisMap_t m;
		int found;
		if(!Dis_Analyze(file, size, kGens[i], NULL, &m))
			return 0;
		found = ProductIn(&m, kGens[i], out, n);
		Dis_FreeMap(&m);
		if(found)
			return 1;
	}
	return 0;
}

// ---- the hash --------------------------------------------------------------------------------

static uint64_t Fnv1a(uint64_t h, const void* data, size_t n)
{
	const uint8_t* p = (const uint8_t*)data;
	size_t i;
	for(i = 0; i < n; i++)
	{
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

// a program's place in the hash: its name lower-cased (ASCII) and its index in the set
typedef struct HashOrder
{
	char name[0x61];
	int index;
} HashOrder_t;

static int ByName(const void* a, const void* b)
{
	return strcmp(((const HashOrder_t*)a)->name, ((const HashOrder_t*)b)->name);
}

/* FNV-1a (64 bits) over the boot program and then the other programs in
 * the order of their lower-cased names, each as its name, a NUL, its size
 * (4 bytes, little-endian) and its decoded bytes: the order of an
 * archive's index and the DSC layer do not change it. */
void Detect_Hash(const DetectSet_t* s, char* out)
{
	uint64_t h = 0xcbf29ce484222325ull;
	HashOrder_t* order;
	int i, j, first = s->haveIpl ? 1 : 0;
	out[0] = 0;
	if(!s->count)
		return;
	order = (HashOrder_t*)malloc((size_t)s->count * sizeof *order);
	if(!order)
		return;
	for(i = 0; i < s->count; i++)
	{
		const char* name = s->progs[i].name;
		for(j = 0; name[j] && j < 0x60; j++)
			order[i].name[j] = (char)(name[j] >= 'A' && name[j] <= 'Z' ? name[j] + 32 : name[j]);
		order[i].name[j] = 0;
		order[i].index = i;
	}
	qsort(order + first, (size_t)(s->count - first), sizeof *order, ByName);
	for(i = 0; i < s->count; i++)
	{
		const DetectProgram_t* p = &s->progs[order[i].index];
		uint8_t size[4];
		size[0] = (uint8_t)p->size;
		size[1] = (uint8_t)(p->size >> 8);
		size[2] = (uint8_t)(p->size >> 16);
		size[3] = (uint8_t)(p->size >> 24);
		h = Fnv1a(h, order[i].name, strlen(order[i].name) + 1);
		h = Fnv1a(h, size, 4);
		h = Fnv1a(h, p->data, p->size);
	}
	free(order);
	snprintf(out, 17, "%016llx", (unsigned long long)h);
}

// ---- the opcodes --------------------------------------------------------------------------------

// the opcode-set family of an instruction's family byte (0 for a base instruction); OPFAM_COUNT for the user instructions
static OpFamily_t FamilyIndex(int fam)
{
	static const struct
	{
		int byte;
		OpFamily_t fam;
	} kMap[] = {{0x7f, OPFAM_7F}, {0x80, OPFAM_80}, {0x81, OPFAM_81}, {0x90, OPFAM_90}, {0x91, OPFAM_91}, {0x92, OPFAM_92},
		{0xa0, OPFAM_A0}, {0xb0, OPFAM_B0}, {0xc0, OPFAM_C0}, {0xd0, OPFAM_D0}, {0xe0, OPFAM_E0}, {0xef, OPFAM_EF}};
	size_t i;
	if(fam == 0)
		return OPFAM_MAIN;
	for(i = 0; i < BGI_COUNTOF(kMap); i++)
		if(kMap[i].byte == fam)
			return kMap[i].fam;
	return OPFAM_COUNT;
}

/* Arguments that only some builds accept: an instruction given one of
 * these constants needs the build named (the handlers reject the value
 * before it).  `pos` is the argument's place counted from the last one
 * pushed (0: the top of the stack). */
typedef struct ArgRule
{
	int fam, op;       // the instruction
	int pos;           // which argument
	uint32_t lo, hi;   // the values (inclusive range)
	EngineGen_t since; // the first build that accepts them
} ArgRule_t;

static const ArgRule_t kArgRules[] = {
	// "90 38" set parameter: h, no, a, b - the numbers the base object handles
	{0x90, 0x38, 2, 0x8100, 0x8100, GEN_1_69_472},      // the sort bias
	{0x90, 0x38, 2, 0x8001, 0x8001, GEN_1_492},         // no perspective scaling
	{0x90, 0x38, 2, 0x7fff0000, 0x7fff0000, GEN_1_492}, // redraw every frame
	{0x90, 0x38, 2, 0x7fffffff, 0x7fffffff, GEN_1_492}, // the script values
	{0x90, 0x38, 2, 0xc0, 0xc1, GEN_1_573},             // pass "90 31" / "91 31" on to the children
	{0x90, 0x38, 2, 0xc4, 0xc4, GEN_1_616},             // follow the "91 06" offset
	{0x90, 0x38, 2, 0x8101, 0x8101, GEN_1_636},         // the sort mode
	{0x90, 0x38, 2, 0xc5, 0xc5, GEN_1_640},             // a switch (stored; its use was not found)
	{0x90, 0x38, 2, 0xffff, 0xffff, GEN_1_640},         // the flag bits
	// "91 38" read a parameter: out, h, no
	{0x91, 0x38, 0, 3, 3, GEN_1_492},                   // the priority
	{0x91, 0x38, 0, 0x7fffffff, 0x7fffffff, GEN_1_492}, // a script value
	{0x91, 0x38, 0, 0xffffffff, 0xffffffff, GEN_1_535}, // the filter's stored value
	{0x91, 0x38, 0, 0xfffffffe, 0xfffffffe, GEN_1_616}, // the class order
	// "91 9A" set a text function: func, param - each build knows the functions it had
	{0x91, 0x9a, 1, 0, 0, GEN_1_573},
	{0x91, 0x9a, 1, 0x80000000, 0x80000001, GEN_1_616},
	{0x91, 0x9a, 1, 0x80000002, 0x80000002, GEN_1_636},
	{0x91, 0x9a, 1, 0x80000003, 0x80000003, GEN_1_640},
	{0x91, 0x9a, 1, 0x80000004, 0x80000008, GEN_1_653},
	{0x91, 0x9a, 1, 0x80000009, 0x80000009, GEN_1_659},
	{0x91, 0x9a, 1, 0x8000000a, 0x8000000a, GEN_1_669},
	// "C0 1F" the control mode of the particle animations
	{0xc0, 0x1f, 0, 2, 2, GEN_1_636},
};

// the instructions the programs use, read under one generation's encodings
typedef struct OpUse
{
	uint8_t used[OPFAM_COUNT][256];       // 1 for every opcode seen in the code
	uint8_t args[BGI_COUNTOF(kArgRules)]; // 1 for every argument rule some instruction meets
	int inlineJump;                       // a conditional jump carries its target inline ("15" with bit 3 of the condition: 1.667 on)
	int count;                            // how many distinct opcodes
	int problems;                         // flows that did not decode
} OpUse_t;

#define ARG_LOOKBACK 32 // instructions remembered to find an argument's push in

/* The constant an instruction finds at stack position `pos` (0: the top)
 * when straight-line code pushed it: walk back over the instructions
 * before it (`hist`, `n` offsets, the last one just before it), skipping
 * what the values above it took, until the push of that value.  0 when
 * the value is computed, comes from a branch (a label lies between) or
 * from code the window does not reach. */
static int ArgConstant(const DisMap_t* m, const uint32_t* hist, int n, int pos, uint32_t* value)
{
	int k = pos, i;
	for(i = n - 1; i >= 0; i--)
	{
		AsmInsn_t in;
		int pops, pushes;
		if(!Asm_Decode(m->area, m->size, hist[i], GEN_COUNT, &in))
			return 0;
		if(in.fam == 0 && (in.op == 0x14 || in.op == 0x15 || in.op == 0x16 || in.op == 0x17))
			return 0; // control flow: the stack below is not this block's
		if(!Asm_StackEffect(in.fam, in.op, GEN_COUNT, &pops, &pushes) || pops < 0)
			return 0;
		if(k < pushes)
		{
			if(pushes == 1 && pops == 0 && in.fam == 0 && in.op <= 0x02)
			{ // push_i8 / push_i16 / push_i32, sign-extended as the machine does
				*value = (uint32_t)(int32_t)in.v[0];
				return 1;
			}
			return 0;
		}
		k += pops - pushes;
		if(m->label[hist[i]] & (DIS_L_JUMP | DIS_L_CALL))
			return 0; // control joins here: what lies below may come from elsewhere
	}
	return 0;
}

// note the argument rules an instruction meets
static void CheckArgs(const DisMap_t* m, const AsmInsn_t* in, const uint32_t* hist, int n, OpUse_t* u)
{
	size_t r;
	for(r = 0; r < BGI_COUNTOF(kArgRules); r++)
	{
		const ArgRule_t* a = &kArgRules[r];
		uint32_t v;
		if(a->fam != in->fam || a->op != in->op || u->args[r])
			continue;
		if(ArgConstant(m, hist, n, a->pos, &v) && v >= a->lo && v <= a->hi)
			u->args[r] = 1;
	}
}

/* Collect the instructions of every program as the disassembler's flow
 * analysis finds them under `gen`: base instructions by their opcode,
 * family instructions by family and second byte (the user instructions
 * "FF xx" and OpenBGI's own "EF xx" are not engine evidence). */
static void CollectUse(const DetectSet_t* s, EngineGen_t gen, OpUse_t* u)
{
	uint32_t hist[ARG_LOOKBACK];
	int i, n;
	memset(u, 0, sizeof *u);
	for(i = 0; i < s->count; i++)
	{
		DisMap_t m;
		uint32_t off;
		if(!Dis_Analyze(s->progs[i].data, s->progs[i].size, gen, NULL, &m))
		{
			u->problems++;
			continue;
		}
		u->problems += m.problems;
		n = 0;
		for(off = 0; off < m.size; off++)
		{
			AsmInsn_t in;
			OpFamily_t f;
			if(m.kind[off] != DIS_K_CODE || !Asm_Decode(m.area, m.size, off, gen, &in))
				continue;
			if(in.fam)
				CheckArgs(&m, &in, hist, n, u);
			else if(in.op == 0x15 && in.count > 1)
				u->inlineJump = 1;
			if(n == ARG_LOOKBACK)
				memmove(hist, hist + 1, (ARG_LOOKBACK - 1) * sizeof *hist);
			else
				n++;
			hist[n - 1] = off;
			f = FamilyIndex(in.fam);
			if(f == OPFAM_COUNT || f == OPFAM_EF)
				continue;
			if(!u->used[f][in.op])
			{
				u->used[f][in.op] = 1;
				u->count++;
			}
		}
		Dis_FreeMap(&m);
	}
}

/* The instructions of the programs.  The 1.667+ builds read some bytes
 * differently ("7F" is a family, "15" may carry its target inline): the
 * programs are read both ways, every build's opcodes accepted, and the
 * reading that leaves fewer flows undecoded is the programs' own. */
static void ProgramUse(const DetectSet_t* s, OpUse_t* u)
{
	OpUse_t* late = (OpUse_t*)malloc(sizeof *late);
	CollectUse(s, GEN_COUNT, u);
	if(!late)
		return;
	CollectUse(s, GEN_LAST, late);
	if(late->problems < u->problems)
		memcpy(u, late, sizeof *u);
	free(late);
}

// how a generation's opcode set compares with what the programs use
typedef struct GenFit
{
	int missing; // opcodes the programs use that the generation lacks
	int extra;   // opcodes the generation has that the programs do not use
} GenFit_t;

static void FitGeneration(const OpUse_t* u, EngineGen_t gen, GenFit_t* fit)
{
	int f, op;
	memset(fit, 0, sizeof *fit);
	for(f = 0; f < OPFAM_COUNT; f++)
	{
		if(f == OPFAM_EF)
			continue;
		for(op = 0; op < 256; op++)
		{
			int has = OpSet_Has((OpFamily_t)f, op, gen);
			if(f == OPFAM_MAIN && (op >= 0x80 || op == 0x7f))
				has = 0; // the family dispatchers are no instructions of their own
			if(u->used[f][op] && !has)
				fit->missing++;
			else if(!u->used[f][op] && has)
				fit->extra++;
		}
	}
	for(f = 0; f < (int)BGI_COUNTOF(kArgRules); f++)
		if(u->args[f] && gen < kArgRules[f].since)
			fit->missing++; // an argument the build rejects
	if(u->inlineJump && gen < GEN_1_667)
		fit->missing++; // an encoding the build reads differently
}

/* 1 when fit `a` is the closer match: fewer opcodes missing, then fewer
 * opcodes the programs never use (a newer build only adds to an older
 * one's set, so the closest build is the oldest that has them all) */
static int Closer(const GenFit_t* a, const GenFit_t* b)
{
	if(a->missing != b->missing)
		return a->missing < b->missing;
	return a->extra < b->extra;
}

EngineGen_t Detect_Opcodes(const DetectSet_t* s, const int* allowed, EngineGen_t* first, EngineGen_t* last, int* mismatches)
{
	OpUse_t* u = (OpUse_t*)malloc(sizeof *u);
	GenFit_t fits[GEN_COUNT], best;
	int g, any = 0;
	EngineGen_t pick = GEN_COUNT;
	*first = *last = GEN_COUNT;
	*mismatches = 0;
	if(!u)
		return GEN_COUNT;
	memset(&best, 0, sizeof best); // (the first fit takes its place)
	ProgramUse(s, u);
	for(g = 0; g < GEN_COUNT; g++)
	{
		if(allowed && !allowed[g])
			continue;
		FitGeneration(u, (EngineGen_t)g, &fits[g]);
		if(!any++ || Closer(&fits[g], &best))
			best = fits[g];
	}
	free(u);
	if(!any)
		return GEN_COUNT;
	for(g = 0; g < GEN_COUNT; g++)
	{
		if(allowed && !allowed[g])
			continue;
		if(fits[g].missing == best.missing && fits[g].extra == best.extra)
			pick = (EngineGen_t)g; // several equally close: the newest
		if(fits[g].missing == best.missing)
		{ // the range of builds that have everything the programs use (as far as they can)
			if(*first == GEN_COUNT)
				*first = (EngineGen_t)g;
			*last = (EngineGen_t)g;
		}
	}
	*mismatches = best.missing;
	return pick;
}

void Detect_Explain(const DetectSet_t* s, FILE* out)
{
	OpUse_t* u = (OpUse_t*)malloc(sizeof *u);
	int g;
	if(!u)
		return;
	ProgramUse(s, u);
	fprintf(out, "  %d programs, %d distinct opcodes, %d flows that do not decode\n", s->count, u->count, u->problems);
	for(g = 0; g < (int)BGI_COUNTOF(kArgRules); g++)
		if(u->args[g])
			fprintf(out, "  argument: %02X %02X given 0x%x needs %s\n", kArgRules[g].fam, kArgRules[g].op, (unsigned)kArgRules[g].lo,
				kGenNames[kArgRules[g].since]);
	for(g = 0; g < GEN_COUNT; g++)
	{
		GenFit_t fit;
		FitGeneration(u, (EngineGen_t)g, &fit);
		fprintf(out, "  %-9s %4d missing  %4d unused\n", kGenNames[g], fit.missing, fit.extra);
	}
	free(u);
}

// ---- the decision --------------------------------------------------------------------------------

// the profile of a generation in the table in use: the built-in name, else the first profile of that generation
static const char* ProfileOfGen(OpenBGI_t* o, EngineGen_t gen)
{
	const EngineProfile_t* builtin = Engine_ProfileOfGen(gen);
	int i, n = Engine_ProfileCount(o);
	for(i = 0; i < n; i++)
		if(strcmp(Engine_ProfileAt(o, i)->name, builtin->name) == 0)
			return builtin->name;
	for(i = 0; i < n; i++)
		if(Engine_ProfileAt(o, i)->gen == gen)
			return Engine_ProfileAt(o, i)->name;
	return NULL;
}

// the generation of a profile name in the table in use; GEN_COUNT when there is no such profile
static EngineGen_t GenOfProfile(OpenBGI_t* o, const char* name)
{
	int i, n = Engine_ProfileCount(o);
	for(i = 0; name && i < n; i++)
		if(strcmp(Engine_ProfileAt(o, i)->name, name) == 0)
			return Engine_ProfileAt(o, i)->gen;
	return GEN_COUNT;
}

static void Decide(OpenBGI_t* o, DetectResult_t* r, int how, int entry, const char* profile)
{
	r->how = how;
	r->entry = entry;
	snprintf(r->profile, sizeof r->profile, "%s", profile);
	r->gen = GenOfProfile(o, profile);
}

int Detect_Game(OpenBGI_t* o, const DetectSet_t* s, FILE* report, const char* prefix, DetectResult_t* r)
{
	int i, n = Engine_GameCount(o), named = 0, oneProfile = 1, candidates[GEN_COUNT];
	const char* named0 = NULL;
	char sjis[0x100];
	EngineGame_t g;
	memset(r, 0, sizeof *r);
	r->entry = -1;
	r->gen = r->fitFirst = r->fitLast = GEN_COUNT;
	if(!s->count)
		return DETECT_NONE;
	Detect_Hash(s, r->hash);

	// 1. the product name the programs check for, the boot program first
	for(i = 0; i < s->count && !r->product[0]; i++)
		if(Detect_ProductName(s->progs[i].data, s->progs[i].size, sjis, sizeof sjis))
			Sjis_ToUtf8(sjis, r->product, sizeof r->product, o->enc.textUtf8);
	memset(candidates, 0, sizeof candidates);
	if(r->product[0])
	{
		for(i = 0; i < n; i++)
		{
			if(!Engine_GameAt(o, i, &g) || !g.profile || strcmp(g.product, r->product) != 0 || GenOfProfile(o, g.profile) == GEN_COUNT)
				continue;
			if(named++ == 0)
				named0 = g.profile;
			else if(strcmp(named0, g.profile) != 0)
				oneProfile = 0;
			candidates[GenOfProfile(o, g.profile)] = 1;
			if(named == 1)
				r->entry = i;
		}
		if(named && oneProfile)
		{
			Decide(o, r, DETECT_PRODUCT, r->entry, named0);
			if(report)
				fprintf(report, "%sproduct name \"%s\": engine profile %s\n", prefix, r->product, r->profile);
			return r->how;
		}
		if(report)
			fprintf(report, "%sproduct name \"%s\": %s\n", prefix, r->product,
				named ? "several profiles carry it, so the hash or the opcodes decide" : "a game the table does not know");
		r->entry = -1;
	}
	else if(report)
		fprintf(report, "%sno product name check in the programs\n", prefix);

	// 2. the hash of the programs
	for(i = 0; i < n; i++)
	{
		if(!Engine_GameAt(o, i, &g) || !g.profile || !g.hash || GenOfProfile(o, g.profile) == GEN_COUNT)
			continue;
		if(strcmp(g.hash, r->hash) == 0 && (!named || candidates[GenOfProfile(o, g.profile)]))
		{
			Decide(o, r, DETECT_HASH, i, g.profile);
			if(report)
				fprintf(report, "%sprograms %s: %s, engine profile %s\n", prefix, r->hash, g.name, r->profile);
			return r->how;
		}
	}
	if(report)
		fprintf(report, "%sprograms %s: not in the table\n", prefix, r->hash);

	// 3. the opcodes
	{
		EngineGen_t gen = Detect_Opcodes(s, named ? candidates : NULL, &r->fitFirst, &r->fitLast, &r->mismatches);
		const char* profile = gen == GEN_COUNT ? NULL : ProfileOfGen(o, gen);
		if(!profile)
			return DETECT_NONE;
		Decide(o, r, DETECT_OPCODES, -1, profile);
		if(report)
		{
			fprintf(report, "%sopcodes: engine %s is the closest match", prefix, kGenNames[gen]);
			if(r->mismatches)
				fprintf(report, " (no build has all the opcodes and arguments the programs use; it lacks %d)\n", r->mismatches);
			else if(r->fitFirst != r->fitLast)
				fprintf(report, " (%s .. %s have everything the programs use)\n", kGenNames[r->fitFirst], kGenNames[r->fitLast]);
			else
				fputc('\n', report);
		}
	}
	return r->how;
}
