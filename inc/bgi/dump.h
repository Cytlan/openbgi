/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dump.h - dumping every archive of a game into editable files and
 *          building the archives again from them: the working copy of a
 *          translation (docs/dump.md).  The commands are bin/bgidump and
 *          bin/bgipack (`make tools`); implemented in src/dump/.
 *
 * Dump_Run reads every "*.arc" of a game directory.  Each archive gets a
 * directory of its own under the output directory, and each entry becomes
 * the most useful common form of what it is: a program ("*._bp") its
 * decompiled source (bpc.h), a scenario file its decompiled source (scn.h),
 * an image (raw or CompressedBG) a PNG, a sound its Ogg Vorbis stream (or a
 * WAV for the other codecs), a BF_Movie its frames as PNG files, the name
 * and scenario tables JSON, an SDC-coded table its decoded bytes, anything
 * else its bytes.  What the common form cannot hold - a sound's loop, a
 * movie's frame rate, the extra header words of an image - goes into a
 * JSON file beside it.  The engine build, which decides how the programs
 * and scenario files read, is found through games.json (version.h) from
 * the game's boot program.
 *
 * The dump's description, "bgidump.json" in the output directory, lists
 * for every archive its entries in order, with what each one is, where it
 * lies in the original archive and the files it became, each with size,
 * modification time and CRC-32.  Pack_Run reads it and writes the archives
 * again: an entry whose files are as they were dumped is copied from the
 * original archive byte for byte, so an untouched dump packs to identical
 * archives; an entry whose files changed is compiled or encoded again and
 * stored the way the original was (DSC-compressed or not).  Without the
 * original archives every entry is built from its files.
 */
#ifndef BGI_DUMP_H_
#define BGI_DUMP_H_

#include "bgi/common.h"

#define DUMP_META_NAME    "bgidump.json" // the description in the output directory
#define DUMP_META_FORMAT  "openbgi-dump" // its "format" member
#define DUMP_META_VERSION 1              // and "version"

typedef struct DumpOptions
{
	const char* gameDir;     // the directory with the archives (a native path, os.h)
	const char* outDir;      // where the dump goes (created when missing; its parent has to exist)
	const char* gamesJson;   // the games.json to identify the game with (NULL: the game directory's, else the one beside the binary, else the built-in table)
	const char* engine;      // an engine profile to use instead of the detected one (NULL: detect)
	const char* const* only; // archive names to dump instead of all of them (NULL: all)
	int onlyCount;
	int force;    // 1: write into a directory that holds a dump already (0: refuse, so that edited files are not overwritten)
	int jobs;     // worker processes per archive (0: as many as the machine has processors, where the system can fork; else 1)
	int pngLevel; // the compression level of the PNG files, 1 .. 9 (0: the default, 4)
	int verbose;  // 0: problems and the summary, 1: a line per archive, 2: a line per entry
	FILE* log;    // where the messages go (NULL: stdout)
} DumpOptions_t;

/* Dump the game.  Returns 0 when every entry was written, 1 when the dump
 * is complete but something had to be kept as plain bytes that should have
 * converted or a script did not survive the round trip (the messages say
 * what), 2 when the dump could not be made (no archives, no output
 * directory, an archive unreadable). */
int Dump_Run(const DumpOptions_t* opt);

typedef struct PackOptions
{
	const char* dumpDir;     // the dump: the directory with bgidump.json
	const char* outDir;      // where the archives are written (created when missing; never the directory of the originals)
	const char* sourceDir;   // the original archives (NULL: the directory the dump recorded)
	const char* const* only; // archive names to build instead of all of them (NULL: all)
	int onlyCount;
	int changedOnly; // 1: write only the archives with an entry that changed
	int rebuildAll;  // 1: build every entry from its files, also the unchanged ones (the original archives are not read)
	int checksum;    // 1: compare the files by CRC even when size and time are those of the dump
	int verbose;     // 0: problems and the summary, 1: a line per archive, 2: a line per changed entry
	FILE* log;       // where the messages go (NULL: stdout)
} PackOptions_t;

/* Build the archives.  Returns 0 on success, 1 when an archive was written
 * with an entry taken from the original because its files did not build
 * (the messages say which), 2 when nothing could be written (no
 * description, no output directory, a missing file with no original to
 * fall back on). */
int Pack_Run(const PackOptions_t* opt);

#endif // BGI_DUMP_H_
