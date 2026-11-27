/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpu.c - processor identification; interface in bgi/sys.h
 *
 * At start-up the engine fills the system information block
 * (o->util.sysInfo, read out by "80 0A") from CPUID: the vendor, family /
 * model / stepping, the brand index, the sizes of the three cache levels
 * and the clock measured over one second of the time stamp counter.  The
 * cache sizes size the compositor's bands (engine.c), the feature bits
 * select the SSE blitters and the blit tuning (src/gfx/mgr/gfxmgr.c), and
 * CMOV + MMX are required to run at all (OpenBGI_Start).
 *
 * Intel caches are decoded from the CPUID leaf 2 descriptor bytes with the
 * table below (only the descriptors known in 2008); everything else reads
 * the extended leaves 0x80000005 / 0x80000006.  The host supplies the
 * CPUID instruction and the time stamp counter (OpenBGI_Machine_t,
 * Host_Cpuid), so a host without them reports no processor at all.
 */
#include "bgi/sys.h"
#include "bgi/host.h"
#include "bgi/core.h"

// CPUID `leaf` into out[4] (EAX, EBX, ECX, EDX); 0 without CPUID or when the leaf is unsupported
static int Cpu_Cpuid(OpenBGI_t* o, uint32_t out[4], uint32_t leaf)
{
	return Host_Cpuid(o, leaf, out);
}

// bit `bit` of CPUID.1:EDX, the standard feature flags; 0 without CPUID
int Cpu_HasFeature(OpenBGI_t* o, int bit)
{
	uint32_t r[4];
	if(!Cpu_Cpuid(o, r, 1))
		return 0;
	return (r[3] >> bit) & 1;
}

int Cpu_HasCmov(OpenBGI_t* o)
{
	return Cpu_HasFeature(o, 15);
}

int Cpu_HasMmx(OpenBGI_t* o)
{
	return Cpu_HasFeature(o, 23);
}

int Cpu_HasSse(OpenBGI_t* o)
{
	return Cpu_HasFeature(o, 25);
}

int Cpu_HasSse2(OpenBGI_t* o)
{
	return Cpu_HasFeature(o, 26);
}

/* The vendor index (0 Intel, 1 AMD, 2 Centaur, 3 Transmeta, 4 other),
 * family / model / stepping with the extended fields folded in, and the
 * brand (Intel: the brand index byte of leaf 1; others: the low word of
 * leaf 0x80000001 EBX).  0 without CPUID or when a leaf is missing. */
static int Cpu_Identify(OpenBGI_t* o, uint32_t* vendor, uint32_t* family, uint32_t* model, uint32_t* stepping, uint32_t* brand)
{
	static const char* const kVendors[4] = {"GenuineIntel", "AuthenticAMD", "CentaurHauls", "GenuineTMx86"};
	uint32_t r[4];
	char id[13];
	uint32_t v, sig;

	if(!Cpu_Cpuid(o, r, 0))
		return 0;
	memcpy(id, &r[1], 4); // the vendor string is EBX, EDX, ECX
	memcpy(id + 4, &r[3], 4);
	memcpy(id + 8, &r[2], 4);
	id[12] = 0;
	for(v = 0; v < 4 && strcmp(id, kVendors[v]) != 0; v++)
		;
	*vendor = v;

	if(!Cpu_Cpuid(o, r, 1))
		return 0;
	sig = r[0];
	*family = (sig >> 8) & 0xf;
	if(*family == 0 || *family == 0xf)
		*family |= (sig >> 16) & 0xff0; // the extended family, shifted into place
	*model = (sig >> 4) & 0xf;
	if(*model == 0xf)
		*model = ((sig & 0xf0000) | 0xf000) >> 12; // extended model << 4 | 0xf
	*stepping = sig & 0xf;
	if(v == 0)
	{
		*brand = r[1] & 0xff;
		return 1;
	}
	if(!Cpu_Cpuid(o, r, 0x80000001))
		return 0;
	*brand = r[1] & 0xffff;
	return 1;
}

/* The clock in MHz: the time stamp counter over one second, so start-up
 * takes a second (0 without CPUID).  The original sleeps in 1 ms steps
 * until GetTicks passes the end; here the start (Cpu_Detect) and the end
 * (Cpu_MeasureStep) are taken over the frames of the start-up.  The
 * original measures once a process; so does an engine: a start after its
 * first - a game the launcher starts, the launcher after that game -
 * takes what was measured then (`cpuKept`, from Engine_Renew) and no
 * second. */
static void Cpu_MeasureBegin(OpenBGI_t* o)
{
	uint32_t r[4];
	o->util.sysInfo.mhz = o->util.cpuKept;
	o->util.cpuMeasuring = !o->util.cpuKept && Cpu_Cpuid(o, r, 0);
	if(!o->util.cpuMeasuring)
		return;
	o->util.cpuUntil = GetTicks(o) + 1000;
	o->util.cpuTsc0 = Host_ReadTsc(o);
}

int Cpu_MeasureStep(OpenBGI_t* o)
{
	if(!o->util.cpuMeasuring)
		return 1;
	if(o->util.cpuUntil > GetTicks(o))
		return 0;
	o->util.sysInfo.mhz = (uint32_t)((Host_ReadTsc(o) - o->util.cpuTsc0) / 1000000u);
	o->util.cpuMeasuring = 0;
	return 1;
}

// a cache descriptor of CPUID leaf 2: which level it describes and the encoded geometry
enum CacheLevel
{
	CL_L1 = 0,
	CL_L2,
	CL_L3,
	CL_L2_OR_L3 // descriptor 0x49: L3 on family 0xf, else L2
};
typedef struct CacheDesc
{
	uint8_t id;     // the descriptor byte
	uint8_t level;  // enum CacheLevel
	uint32_t value; // line size << 24 | ways << 16 | KB
} CacheDesc_t;

static const CacheDesc_t kCacheDescs[] = { // the descriptors the original knows
	{0x0a, CL_L1, 0x20020008}, {0x0c, CL_L1, 0x20040010}, {0x10, CL_L1, 0x20040010}, {0x22, CL_L3, 0x40040200},
	{0x23, CL_L3, 0x40080400}, {0x25, CL_L3, 0x40080800}, {0x29, CL_L3, 0x40081000}, {0x2c, CL_L1, 0x40080020},
	{0x39, CL_L2, 0x40040080}, {0x3a, CL_L2, 0x400600c0}, {0x3b, CL_L2, 0x40020080}, {0x3c, CL_L2, 0x40040100},
	{0x3d, CL_L2, 0x40060180}, {0x3e, CL_L2, 0x40040200}, {0x41, CL_L2, 0x20040080}, {0x42, CL_L2, 0x20040100},
	{0x43, CL_L2, 0x20040200}, {0x44, CL_L2, 0x20040400}, {0x45, CL_L2, 0x20040800}, {0x46, CL_L3, 0x40041000},
	{0x47, CL_L3, 0x40082000}, {0x49, CL_L2_OR_L3, 0x40101000}, {0x4a, CL_L3, 0x400c1800}, {0x4b, CL_L3, 0x40102000},
	{0x4c, CL_L3, 0x400c3000}, {0x4d, CL_L3, 0x40104000}, {0x60, CL_L1, 0x40080010}, {0x66, CL_L1, 0x40040008},
	{0x67, CL_L1, 0x40040010}, {0x68, CL_L1, 0x40040020}, {0x78, CL_L2, 0x40040400}, {0x79, CL_L2, 0x40080080},
	{0x7a, CL_L2, 0x40080100}, {0x7b, CL_L2, 0x40080200}, {0x7c, CL_L2, 0x40080400}, {0x7d, CL_L2, 0x40080800},
	{0x7e, CL_L2, 0x80080100}, {0x7f, CL_L2, 0x40020200}, {0x81, CL_L2, 0x20080080}, {0x82, CL_L2, 0x20080100},
	{0x83, CL_L2, 0x20080200}, {0x84, CL_L2, 0x20080400}, {0x85, CL_L2, 0x20080800}, {0x86, CL_L2, 0x40040200},
	{0x87, CL_L2, 0x40080400}, {0x88, CL_L3, 0x40040800}, {0x89, CL_L3, 0x40041000}, {0x8a, CL_L3, 0x40042000},
	{0x8d, CL_L3, 0x800c0c00}};

/* The Intel caches from the leaf 2 descriptor bytes: every known byte
 * sets its level (a later one overwrites an earlier one), unknown bytes
 * are ignored.  Levels without a descriptor keep their value. */
static void Cpu_IntelCaches(OpenBGI_t* o, uint32_t family, uint32_t* l1, uint32_t* l2, uint32_t* l3)
{
	uint32_t r[4];
	uint32_t rounds, n, i, k;
	uint8_t* bytes;
	if(!Cpu_Cpuid(o, r, 2))
		return;
	rounds = r[0] & 0xff; // AL: how often leaf 2 must be read
	bytes = (uint8_t*)BGI_Alloc(rounds * 16 + 1);
	for(n = 0; n < rounds; n++)
	{
		Cpu_Cpuid(o, r, 2);
		r[0] &= ~0xffu; // the count byte is not a descriptor
		for(k = 0; k < 4; k++)
			if(r[k] & 0x80000000u) // a set bit 31 marks a reserved register
				r[k] = 0;
		memcpy(bytes + n * 16, r, 16);
	}
	for(i = 0; i < rounds * 16; i++)
	{
		uint8_t id = bytes[i];
		for(k = 0; k < sizeof kCacheDescs / sizeof kCacheDescs[0]; k++)
		{
			const CacheDesc_t* d = &kCacheDescs[k];
			if(d->id != id)
				continue;
			switch(d->level)
			{
				case CL_L1: *l1 = d->value; break;
				case CL_L2: *l2 = d->value; break;
				case CL_L3: *l3 = d->value; break;
				default: *(family == 0xf ? l3 : l2) = d->value; break;
			}
			break;
		}
	}
	BGI_Free(bytes);
}

/* The L1 data and L2 caches of the other vendors from the extended leaves,
 * re-packed into the line size << 24 | ways << 16 | KB form; a missing
 * leaf leaves the value alone. */
static void Cpu_ExtendedCaches(OpenBGI_t* o, uint32_t* l1, uint32_t* l2)
{
	uint32_t r[4], ecx, ways;
	if(!Cpu_Cpuid(o, r, 0x80000005))
		return;
	ecx = r[2]; // L1 data: size KB << 24 | ways << 16 | lines per tag << 8 | line size
	*l1 = (ecx & 0xff0000) | (ecx >> 24) | (ecx << 24);
	if(!Cpu_Cpuid(o, r, 0x80000006))
		return;
	ecx = r[2]; // L2: size KB << 16 | associativity code << 12 | lines per tag << 8 | line size
	ways = (ecx >> 12) & 0xf;
	ways = ways ? 1u << (ways >> 1) : 0; // the AMD code 1, 2, 4, 6, 8 -> 1, 2, 4, 8, 16 ways
	*l2 = (((ecx << 8) | ways) << 16) | (ecx >> 16);
}

/* Fill o->util.sysInfo: identification and caches, and start measuring
 * the clock, which Cpu_MeasureStep finishes a second later.  0 without
 * CPUID. */
int Cpu_Detect(OpenBGI_t* o)
{
	SysInfo_t* s = &o->util.sysInfo;
	if(!Cpu_Identify(o, &s->vendor, &s->family, &s->model, &s->stepping, &s->brand))
		return 0;
	s->l1 = s->l2 = s->l3 = 0;
	if(s->vendor == 0)
		Cpu_IntelCaches(o, s->family, &s->l1, &s->l2, &s->l3);
	else
		Cpu_ExtendedCaches(o, &s->l1, &s->l2);
	Cpu_MeasureBegin(o);
	return 1;
}

/* "81 0A" of 1.529 on: the processor brand string of CPUID leaves
 * 0x80000002 .. 4 into `out` (0x40 bytes), its runs of spaces collapsed
 * to one and the ends trimmed; 1 when the leaves exist, 0 (and an empty
 * string) when not. */
int Cpu_BrandString(OpenBGI_t* o, char* out)
{
	uint32_t r[4];
	char raw[0x40];
	char* d = out;
	const char* p;
	int i;
	if(!Cpu_Cpuid(o, r, 0x80000000u) || r[0] < 0x80000004u)
	{
		out[0] = 0;
		return 0;
	}
	memset(raw, 0, sizeof raw);
	for(i = 0; i < 3; i++)
	{
		Cpu_Cpuid(o, r, 0x80000002u + (uint32_t)i);
		memcpy(raw + i * 16, r, 16);
	}
	raw[0x30] = 0;
	for(p = raw; *p == ' '; p++)
		;
	for(; *p; p++)
	{
		if(*p == ' ' && p[1] == ' ')
			continue;
		*d++ = *p;
	}
	if(d > out && d[-1] == ' ')
		d--;
	*d = 0;
	return 1;
}
