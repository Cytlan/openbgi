/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * detect.h - which game a directory holds and which engine profile runs
 *            it; implemented in src/core/detect.c
 *
 * The evidence is the game's own programs: the boot program `ipl._bp`
 * (loose or in the boot archive) and the programs of `sysprg.arc`,
 * collected in a DetectSet_t.  Detect_Game weighs it in three steps, and
 * the first one that decides wins:
 *
 *   1. The product name.  The boot programs of most games compare what
 *      "80 E8" (sys.get_product_name) returns with a literal and quit when
 *      it differs; that literal is the product name the game expects.  A
 *      product name that the games table (openbgi.json) knows, with one
 *      profile for all its entries, decides.  A product name found but
 *      not known still becomes the game's product name.
 *   2. The hash of the programs (FNV-1a over ipl._bp and the programs of
 *      sysprg.arc, in name order, decoded): an entry of the table that
 *      records it decides.
 *   3. The opcodes.  The programs are read as the disassembler reads
 *      them (src/asm/dis.c: the flows from the entry and every branch
 *      target), and every engine generation is compared with what they
 *      use: the opcodes it lacks, the constant arguments it would reject
 *      (a few instructions accept some values only from a certain build
 *      on) and the inline conditional jumps of 1.667+ count as missing.
 *      The closest generation lacks the fewest of them and, of those, has
 *      the fewest opcodes the programs never use - since a newer build
 *      mostly adds to an older one's set, the oldest that has everything;
 *      of several equally close the newest is taken.
 *
 * A product name that several profiles share (a trial and the full game)
 * narrows steps 2 and 3 to those profiles.
 */
#ifndef BGI_DETECT_H_
#define BGI_DETECT_H_

#include "bgi/common.h"
#include "bgi/version.h"

// one program of the evidence, decoded from DSC
typedef struct DetectProgram
{
	char name[0x61]; // the file or entry name (Shift-JIS as stored)
	uint8_t* data;   // the program file, header included (owned by the set)
	uint32_t size;   // its size in bytes
} DetectProgram_t;

// the programs collected for a game; the boot program, when there is one, is the first
typedef struct DetectSet
{
	DetectProgram_t* progs; // the programs (`count` of them)
	int count;
	int cap;     // the slots allocated
	int haveIpl; // progs[0] is the boot program
} DetectSet_t;

void DetectSet_Init(DetectSet_t* s);
void DetectSet_Free(DetectSet_t* s);
/* Add a program (copied): the boot program goes first.  A file that is not
 * a compiled program (a 16-byte header with the code size) or that is
 * DSC-compressed and does not decode is left out; returns 1 when it was
 * taken. */
int DetectSet_Add(OpenBGI_t* o, DetectSet_t* s, const char* name, const uint8_t* data, uint32_t size, int isIpl);
/* Add every program of an archive the caller opened (ArcRead_*; the set
 * closes it).  Returns the number taken, -1 when there is no archive (a
 * NULL `a`: the file was not one). */
struct ArcRead;
int DetectSet_AddArchive(OpenBGI_t* o, DetectSet_t* s, struct ArcRead* a);
/* Collect the evidence of a game directory (a native path): its loose
 * ipl._bp, else the ipl._bp of its system.arc, and the programs of its
 * sysprg.arc; file names match without regard to case.  Returns the
 * number of programs collected. */
int DetectSet_LoadDirectory(OpenBGI_t* o, DetectSet_t* s, const char* nativeDir);
/* The same for the game directory through the engine's file layer (fs.h),
 * which reads a game played from a zip archive as well: `dir` is its
 * engine path with a trailing separator (o->files.baseDir). */
int DetectSet_LoadGame(OpenBGI_t* o, DetectSet_t* s, const char* dir);
/* Add every program of the archive at an engine path (through fs.h);
 * the number taken, -1 when there is no such archive. */
int DetectSet_AddGameArchive(OpenBGI_t* o, DetectSet_t* s, const char* path);

// how Detect_Game decided
#define DETECT_NONE    0 // nothing to go by (no programs)
#define DETECT_PRODUCT 1 // the product name the programs check for
#define DETECT_HASH    2 // the hash of the programs
#define DETECT_OPCODES 3 // the analysis of the opcodes

typedef struct DetectResult
{
	int how;                               // DETECT_*
	int entry;                             // the games-table entry (Engine_GameAt) that decided, -1 for none
	EngineGen_t gen;                       // the generation the profile runs (GEN_COUNT: none)
	char profile[ENGINE_PROFILE_NAME_MAX]; // the profile chosen, "" for none
	char product[0x100];                   // the product name the programs check for (UTF-8), "" when none was found
	char hash[17];                         // the programs' hash, 16 hex digits ("" without programs)
	EngineGen_t fitFirst, fitLast;         // DETECT_OPCODES: the oldest and newest generations that lack the fewest opcodes
	int mismatches;                        // DETECT_OPCODES: how many opcodes and arguments the generation chosen lacks
} DetectResult_t;

/* Identify the game of `s` against the games table in use (openbgi.json,
 * version.h) and pick its profile; nothing is selected - Engine_
 * ApplyDetection does that.  `report` (may be NULL) receives one line per
 * step taken, prefixed with `prefix`.  Returns r->how. */
int Detect_Game(OpenBGI_t* o, const DetectSet_t* s, FILE* report, const char* prefix, DetectResult_t* r);

/* The hash of the programs of a set as Detect_Game computes it (16 hex
 * digits into `out`, at least 17 bytes); "" for an empty set. */
void Detect_Hash(const DetectSet_t* s, char* out);

/* The product name a program compares "80 E8" with (Shift-JIS, into
 * `out`); 1 when one was found. */
int Detect_ProductName(const uint8_t* file, uint32_t size, char* out, size_t n);

/* Step 3 alone: compare every generation `allowed` lets through
 * (GEN_COUNT flags; NULL: all) with the programs and return the closest
 * - the fewest opcodes and arguments missing, then the fewest opcodes
 * the programs never use, then the newest - or GEN_COUNT when none was
 * allowed.  *first and *last receive the oldest and newest generation
 * that lack as few as the closest (the range that has everything, when
 * one does), *mismatches how many that is. */
EngineGen_t Detect_Opcodes(const DetectSet_t* s, const int* allowed, EngineGen_t* first, EngineGen_t* last, int* mismatches);

/* Write the opcode analysis to `out`: the programs' opcodes, the
 * arguments that need a later build, and for every generation the
 * opcodes it lacks and those the programs do not use, one line each -
 * what --detect shows. */
void Detect_Explain(const DetectSet_t* s, FILE* out);

#endif // BGI_DETECT_H_
