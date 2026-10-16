/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * version.h - the engine version profile and the per-version opcode sets
 *             (src/core/version.c, src/vm/opset.c)
 *
 * The games run the same engine in different builds (ver 1.58 of 2005 to
 * 1.669 of 2023, see docs/versions.md).  Most differences are additional
 * opcodes, but a few are parameters of the virtual machine itself that
 * every pointer-handling service depends on: the layout of the tagged
 * script pointers and the size of the thread-local heap.  They are kept
 * in a profile selected once at start-up: from the game's `ipl._bp`
 * through games.json (Engine_DetectGame), from the command line
 * (--engine=NAME), the environment (BGI_ENGINE=NAME) or the launcher's
 * dialog; the default is the build this reimplementation was verified
 * against, Tayutama ver 1.69 build 444.  The profiles themselves are
 * built in and written out as the default games.json, which may then
 * change or add some (src/core/version.c).
 *
 * A tagged pointer is  tag << tagShift | offset.  Tag 0 is the global
 * memory; the code, data (locals) and local-heap areas of the running
 * thread have their own tags; the tags from dynFirst on address the
 * dynamic global blocks of "80 20" (slot = tag - dynFirst).
 *
 * Which opcodes a build defines comes from the survey of the executables
 * (kOpSet, generated into src/vm/opset.c): the VM tables are filled at
 * start-up with the handlers of the selected generation, so a script
 * using an opcode its engine did not have gets the original's "undefined
 * instruction" error, and an opcode the engine had but this
 * reimplementation lacks yet gets a "not implemented" error naming it.
 */
#ifndef BGI_VERSION_H_
#define BGI_VERSION_H_

#include "bgi/common.h"

// the builds in the order they were released (docs/versions.md)
typedef enum EngineGen
{
	GEN_1_58,     // NurseryRhymeTrial, 2005-08
	GEN_1_64,     // NurseryRhyme
	GEN_1_66,     // Itsusora
	GEN_1_69_444, // Tayutama (trial) - the reference build
	GEN_1_69_451, // Tayutama (the retail game)
	GEN_1_69_472, // TayutamaHD
	GEN_1_494,    // PrismRhythm
	GEN_1_529,    // DiamicDays
	GEN_1_535,    // Gackoh
	GEN_1_547,    // Gackoh_HD
	GEN_1_553,    // Heptagram
	GEN_1_573,    // magicha
	GEN_1_588,    // sekachu
	GEN_1_599,    // unmei
	GEN_1_616,    // kodomo
	GEN_1_640,    // Tayutama2AS
	GEN_1_653,    // WakabaironoQuartet
	GEN_1_654,    // Nekotsuku
	GEN_1_659,    // madokino
	GEN_1_662,    // yumahorome
	GEN_1_667,    // arcana
	GEN_1_669,    // haruyome
	GEN_COUNT
} EngineGen_t;
#define GEN_FIRST GEN_1_58
#define GEN_LAST  (GEN_COUNT - 1)

// the opcode families (the index into kOpSet)
typedef enum OpFamily
{
	OPFAM_MAIN, // the main table: 00 .. 7E and the family dispatchers
	OPFAM_7F,   // the sub-tables, one per dispatcher byte
	OPFAM_80,
	OPFAM_81,
	OPFAM_90,
	OPFAM_91,
	OPFAM_92,
	OPFAM_A0,
	OPFAM_B0,
	OPFAM_C0,
	OPFAM_D0,
	OPFAM_E0,
	OPFAM_EF, // OpenBGI's own extension family (src/vm/ops_obgi.c): not in any original build
	OPFAM_COUNT
} OpFamily_t;

typedef struct EngineProfile
{
	const char* name;   // "1.69/444" etc. (the key for Engine_SelectProfile)
	EngineGen_t gen;    // the generation: selects the opcode set
	int compat;         // the script compatibility level x 100: 169, 171, 172
	int tagShift;       // bits of the offset: 24, 25 or 26
	uint32_t tagCode;   // tag of the thread's code area
	uint32_t tagData;   // tag of the thread's data area (locals)
	uint32_t tagHeap;   // tag of the thread's local heap
	uint32_t dynFirst;  // first tag of the dynamic global blocks
	uint32_t dynCount;  // number of dynamic block slots (0: the size-class pools of 1.535 on, see dynmem.c)
	uint32_t heapLimit; // size limit of the local heap ("70"), bytes
	uint32_t bootCode;  // size of the boot thread's code area, bytes
	uint32_t bootData;  // ... and of its data area
} EngineProfile_t;

extern const EngineProfile_t* gEngine; // the selected profile, never NULL

#define ENGINE_PROFILES_MAX     64 // profiles games.json may list
#define ENGINE_PROFILE_NAME_MAX 32 // a profile name, NUL included

/* The profiles in use: the built-in table (one per generation, named
 * after it) until a games.json with a "profiles" list replaces it.
 * Engine_SelectProfile returns 1 when the profile `name` exists and was
 * selected; Engine_ProfileAt / Engine_ProfileCount walk the table;
 * Engine_ProfileOfGen is the built-in profile of a generation (the tools,
 * which load no games.json, take the layouts from it); Engine_ListProfiles
 * prints the table for --list-engines. */
int Engine_SelectProfile(const char* name);
int Engine_ProfileCount(void);
const EngineProfile_t* Engine_ProfileAt(int i); // NULL past the end
const EngineProfile_t* Engine_ProfileOfGen(EngineGen_t gen);
void Engine_ListProfiles(FILE* out);

/* games.json: {"profiles": [...], "games": [...]} - the format its own
 * header describes (Engine_FormatGamesJson writes it) - or a bare list of
 * game entries.  A game entry is {"match": "<string in ipl._bp>",
 * "profile": "<name>", "product": "<name>"} (UTF-8; "product" is optional
 * and defaults to "match"), plus what the launcher records: "directory",
 * "name" and "options"; an entry without "match" lists a directory only.
 * Engine_LoadGamesJson replaces the lists and returns the number of game
 * entries (0 when the file cannot be read or parsed; nothing changes
 * then); Engine_ResetGames puts the built-in defaults back; Engine_
 * SaveGamesJson writes the lists to a file (1 when written) and
 * Engine_FormatGamesJson gives the text (free() it).  Engine_FindGame
 * scans the NUL-terminated strings of a decoded ipl._bp for an entry's
 * match (the index, -1 for none); Engine_DetectGame also selects that
 * entry's profile and product name and returns the profile name (NULL
 * when nothing matched or the profile is unknown).  Engine_RecordGame
 * updates entry `index` (or appends one for -1) with what the launcher
 * learnt and returns the index. */
typedef struct EngineGame
{
	const char* match;     // NULL for a directory-only entry
	const char* profile;   // may be NULL for a directory-only entry
	const char* product;   // never NULL (the match when not given)
	const char* name;      // what the launcher shows: the name, else the product
	const char* directory; // NULL when none was recorded (a native path as UTF-8)
	const char* options;   // NULL for none
} EngineGame_t;
int Engine_LoadGamesJson(const char* path);
void Engine_ResetGames(void);
int Engine_SaveGamesJson(const char* path);
char* Engine_FormatGamesJson(size_t* len);
int Engine_GameCount(void);
int Engine_GameAt(int i, EngineGame_t* out); // 0 past the end
int Engine_FindGame(const uint8_t* ipl, size_t size);
const char* Engine_DetectGame(const uint8_t* ipl, size_t size);
int Engine_RecordGame(int index, const char* name, const char* directory, const char* profile, const char* options);

/* The product name every build carries as a literal ("Tayutama",
 * "NurseryRhyme", ...): what "80 E8" hands to the scripts and what the
 * uninstaller mutex is named after.  The detected game's "product" entry,
 * or the default of the reference build. */
const char* Engine_ProductName(void);
void Engine_SetProductName(const char* name); // copied, at most 0xFF characters

// the generated table and the generation names (src/vm/opset.c)
extern const uint32_t kOpSet[OPFAM_COUNT][256]; // bit `gen` set when the generation defines the opcode
extern const char* const kGenNames[GEN_COUNT];

// 1 when the generation `gen` defines opcode `op` of family `fam`
static inline int OpSet_Has(OpFamily_t fam, int op, EngineGen_t gen)
{
	return (kOpSet[fam][op] >> gen) & 1u;
}

/* --enable=FAM:OP / --disable=FAM:OP overrides of the opcode set ("80:63",
 * "main:7E"; the opcode in hex); applied on top of the generation's set
 * when the tables are built.  Engine_OverrideOpcode returns 1 when the
 * spec was understood. */
int Engine_OverrideOpcode(const char* spec, int enable);
int Engine_OpcodeOverride(OpFamily_t fam, int op); // -1 none, 0 disabled, 1 enabled

// the tag layout in use (shifted values and masks)
#define VM_TAG_SHIFT     (gEngine->tagShift)
#define VM_TAG_CODE      (gEngine->tagCode << gEngine->tagShift)
#define VM_TAG_DATA      (gEngine->tagData << gEngine->tagShift)
#define VM_TAG_HEAP      (gEngine->tagHeap << gEngine->tagShift)
#define VM_TAG_MASK      (~0u << gEngine->tagShift)
#define VM_OFFSET_MASK   ((1u << gEngine->tagShift) - 1u)
#define VM_DYN_SLOTS_MAX 192 // the most dynamic slots of any layout: 1.58 .. 1.66 have tags 0x40 .. 0xFF

#endif // BGI_VERSION_H_
