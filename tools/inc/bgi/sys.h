/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sys.h - small engine-wide services: the engine lock, ticks, idle sleep
 *         (src/core/sysutil.c), the global memory block, dynamic blocks
 *         and tagged-pointer resolution (src/core/dynmem.c) and the
 *         processor identification (src/sys/cpu.c).
 */
#ifndef BGI_SYS_H_
#define BGI_SYS_H_

#include "bgi/common.h"

// ---- engine lock --------------------------------------------
/* the critical section around the hash generator the DSC, SDC and
 * save-file codecs share; Enter / Leave are no-ops before Init and after
 * Delete */
void EngineLock_Init(void);
void EngineLock_Delete(void);
void EngineLock_Enter(void);
void EngineLock_Leave(void);

// ---- time ------------------------------------------------
void Timer_Begin(void);  // raise the timer resolution (timeBeginPeriod in the original)
void Timer_End(void);    // restore it
uint32_t GetTicks(void); // ms since boot, wrapping at 2^32 ("80 04")
void Idle_Init(void);
void Idle_Shutdown(void);
void Sys_Idle(void); // ~0.5 ms sleep, the end of a pass

// ---- global memory and tagged pointers (dynmem.c) -----------------------
/*
 * Scripts address memory through 32-bit tagged values (see docs/vm.md).
 * In the build this was verified against (ver 1.69 build 444) the tag is
 * the top 7 bits:
 *   tag 0x00        global memory         (size 0x1000 << n, "80 70")
 *   tag 0x08        thread code area      (string literals, op 05)
 *   tag 0x09        thread data area      (locals, op 04)
 *   tag 0x0A        thread local heap     (op 70)
 *   tag 0x10..0x3F  dynamic global blocks (48 of them, "80 20" / "80 21")
 * Other builds lay the tags out differently (docs/versions.md); the
 * VM_TAG_* macros come from the selected engine profile (bgi/version.h).
 */
#include "bgi/version.h"

extern uint8_t* gGlobalMem;     // the global memory block (tag 0)
extern uint32_t gGlobalMemSize; // its size in bytes
extern uint8_t* gSysArea;       // the system area of "80 82" / "80 83" (0x100000 bytes allocated)
extern uint32_t gSysAreaSize;   // the size the scripts see: 0x40000 up to 1.599, 0x100000 from 1.616

int SetGlobalMemSize(int shift); // "80 70": size = 0x1000 << shift; 0 if shift not in 0..12
void ClearGlobalMem(void);       // "80 71"
void DynMem_Init(void);
void DynMem_FreeAll(void);
uint32_t GAlloc(uint32_t size);    // "80 20": tagged pointer or 0
int GFree(uint32_t ptr);           // "80 21": 1 on success
uint8_t* DynMemBase(uint32_t ptr); // the block of a dynamic slot (tag - the first dynamic tag); NULL when free

struct Thread;
/* tagged pointer -> host pointer (NULL for 0; script error for a
 * pointer into an unmapped region) */
void* ResolvePtr(uint32_t ptr, struct Thread* t);
// pop a tagged pointer from the thread stack and resolve it
void* PopPtr(struct Thread* t);
/* resolve n consecutive tagged pointers of an array (the
 * NUL-terminated string lists of "80 38", "80 F2", "80 F4", "80 F5") */
void ResolvePtrArray(void** out, struct Thread* t, const uint32_t* in, int n);

// ---- machine identification (cpu.c) -----------------
/* The 0x40 byte block that "80 0A" copies out: nine values
 * filled by Cpu_Detect and seven unused dwords.  The caches are encoded as
 * line size << 24 | ways << 16 | size in KB. */
typedef struct SysInfo
{
	uint32_t vendor; // 0 Intel, 1 AMD, 2 Centaur, 3 Transmeta, 4 other
	uint32_t family; // with the extended family folded in
	uint32_t model;  // with the extended model folded in
	uint32_t stepping;
	uint32_t brand; // Intel: brand index; others: CPUID 0x80000001 EBX & 0xffff
	uint32_t l1;    // L1 data cache
	uint32_t l2;
	uint32_t l3;
	uint32_t mhz; // measured over one second of TSC
	uint32_t unused[7];
} SysInfo_t;
extern SysInfo_t gSysInfo;
int Cpu_Detect(void);           // fills gSysInfo; 0 without CPUID
int Cpu_HasFeature(int bit);    // CPUID.1:EDX bit
int Cpu_HasCmov(void);          // (bit 15)
int Cpu_BrandString(char* out); // "81 0A" (1.529 on): the brand string, spaces collapsed; 0x40 bytes; 1 when available
int Cpu_HasMmx(void);           // (bit 23)
int Cpu_HasSse(void);           // (bit 25)
int Cpu_HasSse2(void);          // (bit 26)

#endif // BGI_SYS_H_
