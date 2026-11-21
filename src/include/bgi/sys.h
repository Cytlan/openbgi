/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sys.h - small engine-wide services: the clocks (src/core/sysutil.c),
 *         the global memory block, dynamic blocks and tagged-pointer
 *         resolution (src/core/dynmem.c), the processor identification
 *         (src/sys/cpu.c) and the scripts' file accessor
 *         (src/sys/fileacc.c).
 */
#ifndef BGI_SYS_H_
#define BGI_SYS_H_

#include "bgi/common.h"
#include "openbgi.h" // OpenBGI_DateTime_t

// ---- time (the host's clocks, openbgi.h) ------------------
uint32_t Clock_HostMs(OpenBGI_t* o);                         // the host's ms clock, wrapping at 2^32; any thread
int Clock_PerfNs(OpenBGI_t* o, int64_t* out);                // the host's ns counter; 0 when it has none
void Clock_LocalTime(OpenBGI_t* o, OpenBGI_DateTime_t* out); // the local date and time
uint32_t GetTicks(OpenBGI_t* o);                             // ms, wrapping at 2^32 ("80 04"): since boot; from 1.535 on less the stopped time

int Clock_Stop(OpenBGI_t* o, int force);             // 1.535 on: stop the clock (force, 1.547 on: whatever "81 68" says); 1 when stopped here
int Clock_Resume(OpenBGI_t* o);                      // let it run on; 1 when it had been stopped
void Clock_BoxBegin(OpenBGI_t* o);                   // 1.547 on: Clock_Stop(1) around a message box or a dialog
void Clock_BoxEnd(OpenBGI_t* o);                     // ... and Clock_Resume after it
int Clock_SetStopWhenInactive(OpenBGI_t* o, int on); // "81 68": the previous setting
int Clock_StopsWhenInactive(OpenBGI_t* o);           // the "81 68" setting
int Clock_SetMaxStep(OpenBGI_t* o, uint32_t ms);     // "81 04" (1.616 on): 50 .. 60000; 1 when accepted

// ---- global memory and tagged pointers (dynmem.c) -----------------------
/*
 * Scripts address memory through 32-bit tagged values (see docs/dev/vm.md).
 * In the build this was verified against (ver 1.69 build 444) the tag is
 * the top 7 bits:
 *   tag 0x00        global memory         (size 0x1000 << n, "80 70")
 *   tag 0x08        thread code area      (string literals, op 05)
 *   tag 0x09        thread data area      (locals, op 04)
 *   tag 0x0A        thread local heap     (op 70)
 *   tag 0x10..0x3F  dynamic global blocks (48 of them, "80 20" / "80 21")
 * Other builds lay the tags out differently (docs/dev/versions.md); the
 * VM_TAG_* macros come from the selected engine profile (bgi/version.h).
 */
#include "bgi/version.h"

int SetGlobalMemSize(OpenBGI_t* o, int shift); // "80 70": size = 0x1000 << shift; 0 if shift not in 0..12
void ClearGlobalMem(OpenBGI_t* o);             // "80 71"
void DynMem_Init(OpenBGI_t* o);
void DynMem_FreeAll(OpenBGI_t* o);
uint32_t GAlloc(OpenBGI_t* o, uint32_t size);    // "80 20": tagged pointer or 0
int GFree(OpenBGI_t* o, uint32_t ptr);           // "80 21": 1 on success
uint8_t* DynMemBase(OpenBGI_t* o, uint32_t ptr); // the block of a dynamic slot (tag - the first dynamic tag); NULL when free

struct Thread;
/* tagged pointer -> host pointer (NULL for 0; script error for a
 * pointer into an unmapped region) */
void* ResolvePtr(OpenBGI_t* o, uint32_t ptr, struct Thread* t);
// pop a tagged pointer from the thread stack and resolve it
void* PopPtr(OpenBGI_t* o, struct Thread* t);
/* resolve n consecutive tagged pointers of an array (the
 * NUL-terminated string lists of "80 38", "80 F2", "80 F4", "80 F5") */
void ResolvePtrArray(OpenBGI_t* o, void** out, struct Thread* t, const uint32_t* in, int n);

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

int Cpu_Detect(OpenBGI_t* o);                 // fills o->util.sysInfo but the clock and starts measuring that; 0 without CPUID
int Cpu_MeasureStep(OpenBGI_t* o);            // 1 once the clock is measured (a second after Cpu_Detect), 0 before
int Cpu_HasFeature(OpenBGI_t* o, int bit);    // CPUID.1:EDX bit
int Cpu_HasCmov(OpenBGI_t* o);                // (bit 15)
int Cpu_BrandString(OpenBGI_t* o, char* out); // "81 0A" (1.529 on): the brand string, spaces collapsed; 0x40 bytes; 1 when available
int Cpu_HasMmx(OpenBGI_t* o);                 // (bit 23)
int Cpu_HasSse(OpenBGI_t* o);                 // (bit 25)
int Cpu_HasSse2(OpenBGI_t* o);                // (bit 26)

// ---- file accessor (fileacc.c) ---------------------------------------------
/* "81 28" .. "81 2B" (1.547 on): files a script opens by path and reads
 * or writes in blocks.  The results are the codes the opcodes push; the
 * status words receive the final value of each request (see fileacc.c). */
int FileAcc_Open(OpenBGI_t* o, uint32_t* outHandle, const char* path, int mode);                // "81 28": 0, 1 bad mode, 2 already open, 3 cannot open
int FileAcc_Close(OpenBGI_t* o, int32_t* status, uint32_t handle);                              // "81 29": 0, 4 unknown (1.547 / 1.553: no status; 0, -1 unknown)
int FileAcc_Transfer(OpenBGI_t* o, int32_t* status, uint32_t handle, void* buf, uint32_t size); // "81 2A": 0, 4 unknown, 6 size 0 (1.547 / 1.553)
int FileAcc_Seek(OpenBGI_t* o, int32_t* status, uint32_t handle, uint32_t pos);                 // "81 2B" (1.573 on): 0, 4 unknown; pos -1 = the end
void FileAcc_CloseAll(OpenBGI_t* o);                                                            // at engine exit

// a pool of the dynamic memory's fixed-size slots (1.535 on; src/core/dynmem.c)
typedef struct DynPool
{
	uint32_t count, bits, maxSize, firstTag; // slots; offset bits per slot; largest block, bytes; first tag
	uint8_t** blocks;                        // the slots, NULL when free (made on first use)
	uint32_t* sizes;                         // .. and the bytes each block has
} DynPool_t;

#endif // BGI_SYS_H_
