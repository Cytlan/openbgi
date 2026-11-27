/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * perf.h - the performance display: what the engine measures of its own
 *          run, shown as a panel in the screen layer and written to
 *          standard error (src/sys/perf.c); not part of the original
 *          engine
 *
 * It is off until the player turns it on - the settings over a game have
 * a switch for it, kept as "perf": "on" among the launcher's own settings
 * of openbgi.json - or the environment says BGI_PERF=1.  While it is on:
 *
 *   * every frame of OpenBGI_RunFrame is timed: from its start to the
 *     start of the one before (the frame's period: what the host's loop
 *     and its screen make of it), and from its start to the point where
 *     the picture is taken (the engine's own work in it);
 *   * the files' reads are counted and timed (Host_FileRead, which
 *     keeps the three counts here itself), the sound
 *     that was mixed and not yet read is looked at, with the frames the
 *     ring had no room for and those the host asked for in vain
 *     (SndOut_Stats), the process's memory is asked of the host
 *     (OpenBGI_Machine_t.processMemory) and the bitmaps' pixels are
 *     summed up;
 *   * twice a second the figures of that half second are put into a
 *     panel - text and two graphs: the last frames' periods, and the
 *     memory over the last minute - which the screen layer shows
 *     (overlay.c: under the settings and the on-screen controls) where
 *     the host said a surface;
 *   * every five seconds the figures of those five seconds are written
 *     to standard error as one line, "openbgi: perf: ...", on any host.
 *
 * An engine that never has it on pays one test a frame and one a read.
 */
#ifndef BGI_PERF_H_
#define BGI_PERF_H_

#include "bgi/common.h"
#include "openbgi.h"

#define PERF_FRAMES   240  // the frames whose periods are kept for the graph
#define PERF_MEMS     120  // the memory samples kept: one each time the panel is made, so a minute
#define PERF_PANEL_MS 500  // the panel's figures are of this long,
#define PERF_LOG_MS   5000 // .. the log's of this long
#define PERF_COLS     40   // the panel's width in half-width characters
#define PERF_GLYPHS   95   // the characters its texts are made of: ' ' .. '~'

// what was counted since a span began
typedef struct PerfSpan
{
	int64_t t0;                  // its start (ns)
	uint32_t frames, drawn;      // the frames, and those whose picture changed
	int64_t engineNs;            // the engine's work in them,
	int64_t maxPeriodNs;         // .. the longest period,
	int64_t maxEngineNs;         // .. and the longest work
	uint32_t aheadMin, aheadMax; // sound frames mixed and not yet read, the fewest and the most at a frame's end
	// the running counts as they were at its start
	uint64_t diskBytes;
	uint32_t diskReads;
	int64_t diskNs;
	uint32_t dropped, starved;
} PerfSpan_t;

// a span's figures, as they are shown and written
typedef struct PerfFigures
{
	int valid;
	int fps10, drawn10;            // frames a second and pictures a second, in tenths
	int frameUs, frameMaxUs;       // a frame's period: the mean and the longest (microseconds)
	int engineUs, engineMaxUs;     // the engine's work in a frame
	int busy;                      // .. as percent of the time
	uint32_t memKb, peakKb, bmpKb; // the process's memory (0: not known), its most so far, the bitmaps' pixels
	int voices;                    // voices playing
	int aheadMinMs, aheadMaxMs;    // sound mixed ahead of the output
	uint32_t dropped, starved;     // sound frames lost to a full ring, and asked for in vain
	uint32_t diskKbs, diskReads;   // read from files: kilobytes a second, and the reads in the span
	int diskUs;                    // .. and the time they took
} PerfFigures_t;

/* What a pass of the engine spends its time on, as far as it is told
 * apart (BGI_PERF=2): the log says how long the span's passes, and its
 * longest one, were at each.  What is left is the programs and everything
 * else. */
enum
{
	PERF_PART_LOAD,  // LoadFile: a file read and unpacked on the engine's thread (the count is the files)
	PERF_PART_PAINT, // the compositor painting what changed (the count is the paintings)
	PERF_PART_SOUND, // the mixer run on the engine's thread
	PERF_PART_JOB,   // waiting for a job of the helpers to end (Async_Finish)
	PERF_PARTS
};

typedef struct PerfParts
{
	int64_t ns[PERF_PARTS];
	uint32_t n[PERF_PARTS];
	int64_t all; // the pass, or the passes
} PerfParts_t;

typedef struct PerfState
{
	int on;                       // it measures, shows and writes
	int loaded;                   // the setting was read for this run
	int forced;                   // BGI_PERF: on whatever the setting says
	int watch;                    // BGI_PERF=2: the log also says what had the picture painted again
	int freeRun;                  // "frames": "free" - the host was told not to hold its frames to the screen's rate
	int64_t begin, last;          // this frame's start and the one's before (ns); 0: none yet
	uint16_t period[PERF_FRAMES]; // the last frames' periods in tenths of a millisecond (0xffff: longer), a ring
	int at, count;
	uint32_t mem[PERF_MEMS]; // the process's memory (KB) each time the panel was made, a ring
	int memAt, memCount;
	uint32_t peakKb;
	PerfSpan_t panel, log;
	PerfFigures_t shown; // the panel's figures
	// counted where it happens (Host_FileRead): the bytes read, the reads, the time in them
	uint64_t diskBytes;
	uint32_t diskReads;
	int64_t diskNs;
	// BGI_PERF=2: where the time goes - this pass, the span's passes together and its longest one
	int64_t partT0[PERF_PARTS]; // when the part began; 0: it is not running
	PerfParts_t pass, sum, worst;
	uint32_t helped; // the frames the sound's reader had mixed itself at the span's start
	// the panel's picture: 0x00RRGGBB, w x h; NULL until it was made
	int cell;                     // the text's cell height the picture was made for
	uint8_t* glyphs;              // the characters ' ' .. '~' at that height, rendered once: PERF_GLYPHS cells of cell x cell coverage (NULL: no font)
	uint8_t advance[PERF_GLYPHS]; // .. and how far each moves the pen
	uint32_t* pix;
	int w, h;
	int changed; // it was made anew since Perf_Picture last handed it over
} PerfState_t;

// OpenBGI_RunFrame's start, and the point where it has the picture (f: the frame as far as it is)
void Perf_Begin(OpenBGI_t* o);
void Perf_End(OpenBGI_t* o, const OpenBGI_Frame_t* f);
// the run's end: the panel's characters and picture go (the figures stay until the engine starts again)
void Perf_Stop(OpenBGI_t* o);

// a part of the pass begins and ends (PERF_PART_*; the engine's thread only, and nothing unless BGI_PERF=2)
void Perf_PartBegin(OpenBGI_t* o, int part);
void Perf_PartEnd(OpenBGI_t* o, int part);

int Perf_On(OpenBGI_t* o);
// on or off, kept in the settings ("perf") when `keep`
void Perf_Set(OpenBGI_t* o, int on, int keep);
/* The frames a second, for a host that can hold them to its screen's
 * refresh rate (MainWin_FrameLimits): 1 held (the start), 0 as many as
 * there are; -1 where the host cannot.  Kept in the settings ("frames":
 * "free") when `keep`. */
int Perf_Limit(OpenBGI_t* o);
// "OpenBGI 1.0  SIMD NEON  threads 1 of 8": the panel's last line, and a line of the log when the display comes on
void Perf_Engine(OpenBGI_t* o, char* out, size_t n);
void Perf_SetLimit(OpenBGI_t* o, int on, int keep);

/* The panel for a surface of `dpi` pixels an inch: its pixels (0x00RRGGBB,
 * *w x *h, the pitch *w), *changed when they are others than at the last
 * call; NULL while the display is off or nothing was measured yet. */
const uint32_t* Perf_Picture(OpenBGI_t* o, int dpi, int* w, int* h, int* changed);

// the line the log gets for these figures (without a line end); for the tests
void Perf_Line(const PerfFigures_t* g, char* out, size_t n);

#endif // BGI_PERF_H_
