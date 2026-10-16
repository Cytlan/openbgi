/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * extract.h - listing and extracting the entries of the game archives
 *             (the --list / --extract options of the engine binary;
 *             implemented in src/core/extract.c)
 *
 * An entry is written as the engine sees it after the DSC layer (the
 * default), as stored (EXTRACT_RAW), or converted into a common format
 * (EXTRACT_CONVERT): a CompressedBG or raw image as a .bmp, a BW sound as
 * a .wav, a BF_Movie as one .bmp per frame; everything else - programs,
 * scenario files, text - as its bytes.  Paths are in the engine's own
 * convention (Shift-JIS, '\\' allowed) and go through the OS layer, so
 * the output directory is relative to the current directory as a game
 * file would be.
 */
#ifndef BGI_EXTRACT_H_
#define BGI_EXTRACT_H_

#include "bgi/common.h"

// the `mode` of Extract_Archive
#define EXTRACT_DECODE  0 // the DSC layer removed (what LoadFile gives the engine)
#define EXTRACT_RAW     1 // the stored bytes
#define EXTRACT_CONVERT 2 // decoded and converted: .bmp, .wav, frames

/* print a heading and one line per entry to `out`: name (UTF-8), stored
 * size, what it is; 0 on success, 1 when the archive cannot be opened */
int Extract_List(const char* arcPath, FILE* out);
/* write the entry `name` (NULL: every entry) of the archive into `outDir`
 * (created when missing) in the given mode, reporting failures on `log`;
 * returns the number of entries written, -1 when the archive could not be
 * read, the output directory not created or the named entry does not exist */
int Extract_Archive(const char* arcPath, const char* name, const char* outDir, int mode, FILE* log);

#endif // BGI_EXTRACT_H_
