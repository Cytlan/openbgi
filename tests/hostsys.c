/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/hostsys.c - unit test of the host's machine, shell, CD audio and
 *                   movies as the engine asks for them (src/core/hostsys.c,
 *                   src/core/hostmedia.c) and of the engine's code over
 *                   them: the processor checks (src/sys/cpu.c), the
 *                   machine key (src/sys/install/machine.c), the program
 *                   runs (src/sys/process.c), CD audio (src/snd/sound.c),
 *                   the movie player's front end (src/gfx/movie.c) and the
 *                   decoded movies (src/sys/moviefx.c); and of the OS
 *                   layer's decoder (src/host/os_ffmpeg.c); built as
 *                   test_hostsys by `make test` (the movie of the ffmpeg
 *                   group is written to bin/ and removed)
 *
 * The groups, in the order they run:
 *
 *   plain   - without any of the tables: the ordinary machine's facts, no
 *             processor to check, every shell call failing, no CD drive,
 *             no player and no decoder
 *   machine - a host's machine: every member reached with the host's
 *             user; the processor identified from its CPUID (vendor,
 *             family, model, stepping, the caches of leaf 2, the features,
 *             the brand string; an AMD one's extended fields and leaves)
 *             and its clock measured over a second of the time stamp
 *             counter; the machine key built from the facts, the Windows
 *             versions in it
 *   shell   - a host's shell: a program a game asks for is refused and
 *             the host hears nothing of it; "80 E3" opens a web link and
 *             a document and refuses everything else; the instance lock,
 *             the clipboard, the wallpaper, the shortcuts
 *   cd      - a host's CD audio: the device opened on the game's CD-ROM
 *             drive (or the disc directory's) in TMSF, a track played
 *             with its notification and again, the modes mapped, stopped
 *             and closed; a device that cannot be set up is closed again;
 *             a disc without tracks plays nothing
 *   player  - a host's movie player: the movie opened and started in the
 *             rectangle, its length, running, the volume, stopped
 *   decoder - a host's decoder: the frames shown on the engine's clock
 *             (none before the first is decoded, the late ones dropped),
 *             the sound mixed into the engine's at the volume once the
 *             first frame shows (taken while silent), the end once both
 *             streams are over (a sound longer than the picture played
 *             out); without the mixer no sound asked for or waited for;
 *             stopped before the end
 *   both    - a host with a player and a decoder: the decoder plays;
 *             the player when it is asked for (--movies=player, or
 *             "movies": "player" in openbgi.json, which --movies=decoder
 *             overrides), and then the decoder what the player cannot;
 *             the option as the command line gives it; and --lang=,
 *             which a run out of sight (--preview) does not carry out
 *   partial - tables with members left out: the ordinary machine's facts
 *             for those, a count that is not one, a failing computer
 *             name; a program that idles and ends at once, a lock that
 *             does not say whether it was held; a decoder with neither
 *             frames nor sound
 *   ffmpeg  - the OS layer's decoder (os_ffmpeg.c: the movie library
 *             over this layer's files) on a second of movie that the
 *             ffmpeg program makes, or that test_ffmovie left (skipped
 *             without either, and in a build without the decoder): the rate and the length, every frame
 *             red, at most four pictures held, the frames let go and
 *             not yet decoded refused, the second of sound; closed while it
 *             runs; a file that is not there, and one that is no movie
 */
#include "bgi/core.h"
#include "bgi/engine.h"
#include "bgi/host.h"
#include "bgi/msg.h"
#include "bgi/sys.h"
#include "bgi/install.h"
#include "bgi/sound.h"
#include "bgi/gfx.h"
#include "bgi/display.h"
#include "bgi/snd/wavemaster.h"
#include "host/os.h"
#include "host/os_common.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

// ---- the host's clock and the record of its calls -------------------------------------------

static uint32_t gNow = 1000; // the host's clock (ms)

static struct
{
	char log[0x800]; // the calls, "name(args);" each
	int badUser;     // calls with another user than the host's
} gRec;

static void Note(void* user, const char* fmt, ...)
{
	char buf[0x100];
	va_list ap;
	if(user != &gRec)
		gRec.badUser++;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	snprintf(gRec.log + strlen(gRec.log), sizeof gRec.log - strlen(gRec.log), "%s;", buf);
}

static uint32_t HostTicksMs(void* user)
{
	(void)user;
	return gNow;
}

static int HostPerfNs(void* user, int64_t* out)
{
	(void)user;
	*out = (int64_t)gNow * 1000000;
	return 1;
}

static void HostLocalTime(void* user, OpenBGI_DateTime_t* out)
{
	(void)user;
	memset(out, 0, sizeof *out);
	out->year = 2026;
	out->month = 1;
	out->day = 1;
}

// the files: every path under C:\movies\ exists (for Fs_LocalPath), nothing else
static uint32_t FAttrs(void* user, const char* path)
{
	(void)user;
	return strncmp(path, "C:\\movies\\", 10) == 0 ? OPENBGI_ATTR_ARCHIVE : OPENBGI_INVALID_ATTRS;
}

/* .. and can be read: each is the bytes of kMovieFile, opened as often as
 * it is asked for.  The decoder of the tests reads its movie through
 * what the engine hands it, which ends here. */
static const char kMovieFile[] = "MOVIE: the bytes of a file a host has, read through the engine. END";
#define MOVIE_FILE_SIZE (sizeof kMovieFile - 1)
static struct
{
	uint64_t pos;
	int open;           // streams open right now
	int opens, closes;  // .. and ever opened and closed
	int closedTooEarly; // one was closed while the decoder still had its movie
} gFile;
static int gDecOpen; // the decoder of the tests has a movie open

static void* FOpen(void* user, const char* path, int mode)
{
	(void)user;
	if(mode != OPENBGI_OPEN_READ || strncmp(path, "C:\\movies\\", 10) != 0)
		return NULL;
	gFile.pos = 0;
	gFile.open++;
	gFile.opens++;
	return &gFile;
}

static void FClose(void* user, void* f)
{
	(void)user;
	CHECK(f == &gFile);
	if(gDecOpen)
		gFile.closedTooEarly = 1;
	gFile.open--;
	gFile.closes++;
}

static uint32_t FRead(void* user, void* f, void* buf, uint32_t n)
{
	(void)user;
	CHECK(f == &gFile);
	if(gFile.pos >= MOVIE_FILE_SIZE)
		return 0;
	if(n > MOVIE_FILE_SIZE - gFile.pos)
		n = (uint32_t)(MOVIE_FILE_SIZE - gFile.pos);
	memcpy(buf, kMovieFile + gFile.pos, n);
	gFile.pos += n;
	return n;
}

static int FSeek(void* user, void* f, uint64_t pos)
{
	(void)user;
	CHECK(f == &gFile);
	if(pos > MOVIE_FILE_SIZE)
		return 0;
	gFile.pos = pos;
	return 1;
}

static uint64_t FLength(void* user, void* f)
{
	(void)user;
	CHECK(f == &gFile);
	return MOVIE_FILE_SIZE;
}

static OpenBGI_Files_t gFiles; // `attrs` and the five calls above (set in main)

// ---- a machine: an Intel processor of 2008 at 2 GHz (or an AMD one) ------------------------------

static void Regs(uint32_t out[4], uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	out[0] = a;
	out[1] = b;
	out[2] = c;
	out[3] = d;
}

static uint32_t Str4(const char* s) // four characters as CPUID returns them in a register
{
	return (uint32_t)(uint8_t)s[0] | (uint32_t)(uint8_t)s[1] << 8 | (uint32_t)(uint8_t)s[2] << 16 | (uint32_t)(uint8_t)s[3] << 24;
}

static int gAmd;    // the machine's processor is the AMD one
static int gOldCpu; // the Intel one has no brand string (the extended leaves end at 0x80000001)

// an AMD processor with both extended fields of its signature set, its caches in the extended leaves
static int AmdCpuid(uint32_t leaf, uint32_t out[4])
{
	switch(leaf)
	{
		case 0: Regs(out, 1, Str4("Auth"), Str4("cAMD"), Str4("enti")); return 1;
		// family f + 1, model f + 2, stepping 2, type 1; CMOV, MMX, SSE
		case 1: Regs(out, 0x00121ff2, 0, 0, (1u << 15) | (1u << 23) | (1u << 25)); return 1;
		case 0x80000000u: Regs(out, 0x80000006u, 0, 0, 0); return 1;
		case 0x80000001u: Regs(out, 0x00121ff2, 0xabcd1234u, 0, 0); return 1; // the brand id in EBX's low word
		case 0x80000002u:
		case 0x80000003u:
		case 0x80000004u:
		{
			static const char brand[49] = "AMD Athlon(tm) 64 X2 Dual Core Processor 4800+\0"; // 48 bytes, NUL-padded
			const char* p = brand + (leaf - 0x80000002u) * 16;
			Regs(out, Str4(p), Str4(p + 4), Str4(p + 8), Str4(p + 12));
			return 1;
		}
		case 0x80000005u: Regs(out, 0, 0, 0x40020140, 0); return 1; // L1 data: 64 KB, 2 ways, 64-byte lines
		case 0x80000006u: Regs(out, 0, 0, 0x02008140, 0); return 1; // L2: 512 KB, 16 ways (code 8), 64-byte lines
		default: return 0;
	}
}

static int MCpuid(void* user, uint32_t leaf, uint32_t out[4])
{
	static const char brand[49] = "  Intel(R)  Core(TM)2   CPU  T7200              "; // 48 bytes, runs of spaces
	(void)user;
	if(gAmd)
		return AmdCpuid(leaf, out);
	if(gOldCpu && leaf >= 0x80000000u)
	{
		if(leaf != 0x80000000u)
			return 0;
		Regs(out, 0x80000001u, 0, 0, 0);
		return 1;
	}
	switch(leaf)
	{
		case 0: Regs(out, 2, Str4("Genu"), Str4("ntel"), Str4("ineI")); return 1;
		case 1:
			Regs(out, 0x6f6, 0x05, 0, (1u << 15) | (1u << 23) | (1u << 25) | (1u << 26));
			return 1; // CMOV, MMX, SSE, SSE2
		// one round: 0x2c (L1 32 KB), 0x7d (L2 2 MB), then 0x49 (L2 4 MB below family f); EDX is reserved (bit 31)
		case 2: Regs(out, 0x00002c01, 0, 0x0000497d, 0x80000043); return 1;
		case 0x80000000u: Regs(out, 0x80000004u, 0, 0, 0); return 1;
		case 0x80000002u:
		case 0x80000003u:
		case 0x80000004u:
		{
			const char* p = brand + (leaf - 0x80000002u) * 16;
			Regs(out, Str4(p), Str4(p + 4), Str4(p + 8), Str4(p + 12));
			return 1;
		}
		default: return 0;
	}
}

static uint64_t MReadTsc(void* user)
{
	(void)user;
	return (uint64_t)gNow * 2000000u; // 2 GHz
}

static int MCpuCount(void* user)
{
	Note(user, "cpus");
	return 2;
}

static void MMemoryStatus(void* user, OpenBGI_MemoryStatus_t* out)
{
	Note(user, "mem");
	out->totalPhys = 0x20000000u;
	out->availPhys = 0x10000000u;
}

static void MMemoryStatusEx(void* user, uint64_t* totalPhys, uint64_t* availPhys)
{
	Note(user, "memex");
	*totalPhys = (uint64_t)3 << 30;
	*availPhys = (uint64_t)1 << 30;
}

static int MComputerName(void* user, char* buf, size_t n)
{
	Note(user, "computer(%d)", buf[0]);
	snprintf(buf, n, "WORKSTATION");
	return 1;
}

static int MUserName(void* user, char* buf, size_t n)
{
	Note(user, "user");
	snprintf(buf, n, "alice");
	return 1;
}

static uint32_t gVersion[4] = {6, 1, 7601, 2}; // Windows 7 SP1: major, minor, build, platform

static void MVersion(void* user, uint32_t out[4], char* csd, size_t n)
{
	Note(user, "version");
	memcpy(out, gVersion, sizeof gVersion);
	snprintf(csd, n, "Service Pack 1");
}

static int MUiLanguage(void* user)
{
	Note(user, "lang");
	return 0x409;
}

static int MDisplayAdapter(void* user, char* desc, size_t n, uint32_t version[4])
{
	Note(user, "adapter");
	snprintf(desc, n, "Fake Adapter");
	Regs(version, 1, 2, 3, 4);
	return 1;
}

static int MAudioAvailable(void* user)
{
	Note(user, "audio");
	return 0;
}

static const OpenBGI_Machine_t kMachine = {sizeof(OpenBGI_Machine_t), MCpuid, MReadTsc, MCpuCount, MMemoryStatus, MMemoryStatusEx, MComputerName, MUserName, MVersion,
	MUiLanguage, MDisplayAdapter, MAudioAvailable};

// ---- a shell ------------------------------------------------------------------------------------

static struct
{
	int lockHeld; // another copy holds the instance lock
} gSh;

static int gLock;

static void* SInstanceLock(void* user, const char* name, int* alreadyExists)
{
	Note(user, "lock(%s)", name);
	*alreadyExists = gSh.lockHeld;
	return &gLock;
}

static void SInstanceUnlock(void* user, void* lock)
{
	Note(user, "unlock");
	CHECK(lock == &gLock);
}

static int SSendToRunning(void* user, const char* windowClass, const char* data)
{
	Note(user, "send(%s,%s)", windowClass, data);
	return 1;
}

static int SOpenDocument(void* user, const char* target)
{
	Note(user, "open(%s)", target);
	return 1;
}

static int SClipboardSetText(void* user, const char* text)
{
	Note(user, "clip(%s)", text);
	return 1;
}

static int SSetWallpaper(void* user, const char* path, int stretch, int tile)
{
	Note(user, "wall(%s,%d,%d)", path, stretch, tile);
	return 7; // SystemParametersInfo's result as it is
}

static int SCreateShortcut(void* user, const char* linkPath, const char* target, const char* args)
{
	Note(user, "link(%s,%s,%s)", linkPath, target, args ? args : "NULL");
	return 1;
}

static void SNotifyAssocChanged(void* user)
{
	Note(user, "assoc");
}

static const OpenBGI_Shell_t kShell = {sizeof(OpenBGI_Shell_t), SInstanceLock, SInstanceUnlock, SSendToRunning, SOpenDocument, SClipboardSetText,
	SSetWallpaper, SCreateShortcut, SNotifyAssocChanged};

// ---- a CD drive ---------------------------------------------------------------------------------

static int gCdSetFails;          // setTimeFormatTmsf fails
static uint32_t gCdTracks = 12;  // the tracks on the disc
static uint32_t gCdMode = 0x20e; // the device's MCI mode (MCI_MODE_PLAY)

static uint32_t CdOpen(void* user, const char* element)
{
	Note(user, "open(%s)", element ? element : "NULL");
	return 7;
}

static int CdSetTimeFormatTmsf(void* user, uint32_t device)
{
	Note(user, "tmsf(%u)", (unsigned)device);
	return !gCdSetFails;
}

static int CdStatus(void* user, uint32_t device, uint32_t item, uint32_t* out)
{
	Note(user, "status(%u,%u)", (unsigned)device, (unsigned)item);
	*out = item == OPENBGI_MCI_STATUS_TRACKS ? gCdTracks : gCdMode;
	return 1;
}

static int CdPlay(void* user, uint32_t device, uint32_t from, uint32_t to, int notify)
{
	Note(user, "play(%u,%u,%u,%d)", (unsigned)device, (unsigned)from, (unsigned)to, notify);
	return 1;
}

static int CdStop(void* user, uint32_t device)
{
	Note(user, "stop(%u)", (unsigned)device);
	return 1;
}

static int CdClose(void* user, uint32_t device)
{
	Note(user, "close(%u)", (unsigned)device);
	return 1;
}

static const OpenBGI_CdAudio_t kCd = {sizeof(OpenBGI_CdAudio_t), CdOpen, CdSetTimeFormatTmsf, CdStatus, CdPlay, CdStop, CdClose};

// ---- a movie player and a movie decoder ------------------------------------------------------

static int gPlayerFails; // the player cannot play the file

static int PlOpen(void* user, const char* path)
{
	Note(user, "popen(%s)", path);
	return gPlayerFails ? OPENBGI_MOVIE_FAILED : OPENBGI_MOVIE_OK;
}

static int PlStart(void* user, int x, int y, int w, int h, int32_t volume, int32_t* lengthMs)
{
	Note(user, "pstart(%d,%d,%d,%d,%d)", x, y, w, h, (int)volume);
	*lengthMs = 1234;
	return 1;
}

static void PlStop(void* user)
{
	Note(user, "pstop");
}

static int PlRunning(void* user)
{
	Note(user, "prunning");
	return 1;
}

static void PlSetVolume(void* user, int32_t volume)
{
	Note(user, "pvol(%d)", (int)volume);
}

static const OpenBGI_MoviePlayer_t kPlayer = {sizeof(OpenBGI_MoviePlayer_t), PlOpen, PlStart, PlStop, PlRunning, PlSetVolume};

#define DEC_FRAMES 5     // the frames of the decoded movie (500 ms)
#define DEC_SOUND  26460 // its stereo frames of sound (600 ms: longer than the picture)

static struct
{
	int w, h, audio; // as asked for
	int hold;        // no frame decoded yet
	int32_t taken;   // the frames let go
	int soundLeft;   // the stereo frames still to come
	int opened, closed;
	int32_t tooOld; // takes of a frame that was let go already
} gDec;

/* a movie with sound: the sound is there even when it was not asked for,
 * and the engine must not take it then.  Its file is the engine's to
 * read: the whole of it comes through `file`, from any place. */
static void* DecOpen(void* user, const OpenBGI_MovieFile_t* file, const char* path, int w, int h, int audio, OpenBGI_MovieInfo_t* info)
{
	uint8_t buf[128];
	Note(user, "dopen(%s,%d,%d,%d)", path, w, h, audio);
	CHECK(file && file->size == sizeof *file && file->read && file->seek && file->length == (int64_t)MOVIE_FILE_SIZE);
	if(file && file->read && file->seek)
	{
		CHECK(file->read(file->ctx, buf, 5) == 5 && memcmp(buf, "MOVIE", 5) == 0);
		CHECK(file->seek(file->ctx, (int64_t)MOVIE_FILE_SIZE - 3) && file->read(file->ctx, buf, 100) == 3 && memcmp(buf, "END", 3) == 0);
		CHECK(file->read(file->ctx, buf, 100) == 0 && file->read(file->ctx, buf, 0) == 0); // the end; nothing asked for
		CHECK(!file->seek(file->ctx, -1) && !file->seek(file->ctx, (int64_t)MOVIE_FILE_SIZE + 1));
		CHECK(file->seek(file->ctx, 0) && file->read(file->ctx, buf, (int)sizeof buf) == (int)MOVIE_FILE_SIZE &&
			memcmp(buf, kMovieFile, MOVIE_FILE_SIZE) == 0);
	}
	gDecOpen = 1;
	memset(&gDec, 0, sizeof gDec);
	gDec.w = w;
	gDec.h = h;
	gDec.audio = audio;
	gDec.soundLeft = DEC_SOUND;
	gDec.opened = 1;
	info->fps = 10.0; // a frame every 100 ms
	info->lengthMs = DEC_FRAMES * 100;
	info->audio = 1;
	return &gDec;
}

// four frames ahead of the ones taken, as a host holds them (none while held)
static int32_t DecFrames(void* user, void* movie, int* ended)
{
	int32_t n = gDec.hold ? gDec.taken : gDec.taken + 4;
	(void)user;
	CHECK(movie == &gDec);
	if(n > DEC_FRAMES)
		n = DEC_FRAMES;
	*ended = n == DEC_FRAMES;
	return n;
}

static int DecTake(void* user, void* movie, int32_t index, uint8_t* out)
{
	int i;
	(void)user;
	(void)movie;
	if(index < gDec.taken)
	{
		gDec.tooOld++;
		return 0;
	}
	for(i = 0; i < gDec.w * gDec.h; i++)
		((uint32_t*)out)[i] = 0x101010u * (uint32_t)(index + 1); // frame n is grey (n + 1) * 0x10
	gDec.taken = index + 1;
	return 1;
}

static int DecSamples(void* user, void* movie, int16_t* out, int frames, int* ended)
{
	int n = frames < gDec.soundLeft ? frames : gDec.soundLeft, i;
	(void)user;
	(void)movie;
	for(i = 0; out && i < n; i++)
	{
		out[i * 2] = 1000;
		out[i * 2 + 1] = -1000;
	}
	gDec.soundLeft -= n;
	*ended = gDec.soundLeft == 0;
	return n;
}

static void DecClose(void* user, void* movie)
{
	Note(user, "dclose");
	CHECK(movie == &gDec);
	CHECK(gFile.open > 0); // its file is still there for it
	gDec.closed = 1;
	gDecOpen = 0;
}

static const OpenBGI_MovieDecoder_t kDecoder = {sizeof(OpenBGI_MovieDecoder_t), DecOpen, DecFrames, DecTake, DecSamples, DecClose};

// ---- helpers -------------------------------------------------------------------------------------

static OpenBGI_t* New(const OpenBGI_Host_t* host)
{
	memset(&gRec, 0, sizeof gRec);
	return OpenBGI_New(host);
}

static OpenBGI_Host_t Bare(void)
{
	OpenBGI_Host_t h;
	memset(&h, 0, sizeof h);
	h.size = sizeof h;
	h.api = OPENBGI_API_VERSION;
	h.user = &gRec;
	h.ticksMs = HostTicksMs;
	h.perfNs = HostPerfNs;
	h.localTime = HostLocalTime;
	h.caps = OPENBGI_CAP_DIALOGS;
	h.files = &gFiles;
	return h;
}

// ---- the tests ------------------------------------------------------------------------------

static void TestPlain(void)
{
	OpenBGI_Host_t host = Bare();
	OpenBGI_t* o = New(&host);
	OpenBGI_MemoryStatus_t ms;
	OpenBGI_MovieInfo_t info;
	uint32_t r[4], ver[4];
	uint64_t tp, ap;
	char buf[0x100];
	int exists = 7;
	printf("plain\n");
	CHECK(o != NULL);
	if(!o)
		return;
	CHECK(!Host_HasCpuid(o) && !Host_Cpuid(o, 0, r) && Host_ReadTsc(o) == 0 && Host_CpuSignature(o) == 0);
	CHECK(!Cpu_Detect(o) && !Cpu_HasMmx(o) && !Cpu_BrandString(o, buf) && buf[0] == 0);
	CHECK(Host_CpuCount(o) == 1);
	Host_MemoryStatus(o, &ms);
	CHECK(ms.totalPhys == 0x40000000u && ms.availPhys == 0x20000000u && ms.loadPercent == 50 && ms.totalVirtual == 0x7ffe0000u);
	Host_MemoryStatusEx(o, &tp, &ap);
	CHECK(tp == 0x40000000u && ap == 0x20000000u);
	Host_ComputerName(o, buf, sizeof buf);
	CHECK(strcmp(buf, "OPENBGI") == 0);
	Host_UserName(o, buf, sizeof buf);
	CHECK(strcmp(buf, "user") == 0);
	Host_Version(o, ver, buf, sizeof buf);
	CHECK(ver[0] == 5 && ver[1] == 1 && ver[2] == 2600 && ver[3] == 2 && strcmp(buf, "Service Pack 3") == 0);
	CHECK(Host_UiLanguage(o) == 0x411);
	memset(ver, 0xff, sizeof ver);
	strcpy(buf, "x");
	Host_DisplayAdapter(o, buf, sizeof buf, ver);
	CHECK(buf[0] == 0 && ver[0] == 0 && ver[3] == 0);
	CHECK(Host_AudioAvailable(o) && DSound8_IsAvailable(o));
	// the key of the ordinary machine: no signature, XP, "u" and "O"
	CHECK(GetMachineKey(o) == ((0x8000u << 16) | ('u' << 8) | 'O'));
	// no shell
	CHECK(Host_InstanceLock(o, "m", &exists) == NULL && exists == 0);
	Host_InstanceUnlock(o, NULL);
	CHECK(!Host_SendToRunning(o, "c", "d") && !Host_OpenDocument(o, "http://openbgi.net/") && !Host_ClipboardSetText(o, "t"));
	CHECK(!ShellOpen(o, "http://openbgi.net/") && !ShellOpen(o, "C:\\movies\\manual.txt") && !Program_Refuse(o, NULL, "a.exe"));
	CHECK(!Host_SetWallpaper(o, "w", 0, 0) && !Host_CreateShortcut(o, "l", "t", NULL));
	Host_NotifyAssocChanged(o);
	// no CD drive, no player, no decoder
	CHECK(Host_CdOpen(o, NULL) == 0 && !Cd_Open(o));
	CHECK(Host_MovieOpen(o, "C:\\movies\\a.mpg") == OPENBGI_MOVIE_FAILED && !Host_MovieRunning(o));
	{
		OpenBGI_MovieFile_t file;
		memset(&file, 0, sizeof file);
		file.size = sizeof file;
		CHECK(Host_DecoderOpen(o, &file, "C:\\movies\\a.mpg", 4, 4, 0, &info) == NULL && info.fps == 30.0);
	}
	{
		int32_t len = -1;
		CHECK(!MovieFx_Start(o, "C:\\movies\\a.mpg", 0, 0, 4, 4, 0, &len) && !MovieFx_IsRunning(o));
	}
	CHECK(gRec.log[0] == 0);
	OpenBGI_Delete(o);
}

static void TestMachine(void)
{
	OpenBGI_Host_t host = Bare();
	OpenBGI_t* o;
	OpenBGI_MemoryStatus_t ms;
	SysInfo_t* s;
	uint32_t ver[4];
	uint64_t tp, ap;
	char buf[0x100];
	printf("machine\n");
	host.machine = &kMachine;
	o = New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	s = &o->util.sysInfo;
	CHECK(Host_HasCpuid(o) && Host_CpuSignature(o) == 0x6f6);
	CHECK(Cpu_Detect(o));
	CHECK(s->vendor == 0 && s->family == 6 && s->model == 15 && s->stepping == 6 && s->brand == 5);
	CHECK(s->l1 == 0x40080020u && s->l2 == 0x40101000u && s->l3 == 0);
	CHECK(Cpu_HasCmov(o) && Cpu_HasMmx(o) && Cpu_HasSse(o) && Cpu_HasSse2(o));
	CHECK(!Cpu_MeasureStep(o)); // a second of the counter first
	gNow += 999;
	CHECK(!Cpu_MeasureStep(o));
	gNow += 1;
	CHECK(Cpu_MeasureStep(o) && s->mhz == 2000);
	CHECK(Cpu_BrandString(o, buf) && strcmp(buf, "Intel(R) Core(TM)2 CPU T7200") == 0);
	gOldCpu = 1;
	strcpy(buf, "x");
	CHECK(!Cpu_BrandString(o, buf) && buf[0] == 0);
	gOldCpu = 0;
	// the AMD processor: the extended fields folded in, the brand id and the caches of the extended leaves
	gAmd = 1;
	CHECK(Cpu_Detect(o));
	CHECK(s->vendor == 1 && s->family == 0x1f && s->model == 0x2f && s->stepping == 2 && s->brand == 0x1234);
	CHECK(s->l1 == 0x40020040u && s->l2 == 0x40100200u && s->l3 == 0 && s->mhz == 0);
	CHECK(Cpu_HasCmov(o) && Cpu_HasMmx(o) && Cpu_HasSse(o) && !Cpu_HasSse2(o));
	CHECK(Cpu_BrandString(o, buf) && strcmp(buf, "AMD Athlon(tm) 64 X2 Dual Core Processor 4800+") == 0);
	gNow += 1000;
	CHECK(Cpu_MeasureStep(o) && s->mhz == 2000);
	// an engine that starts again (a game the launcher starts, the launcher after it) measures no second time
	Engine_Renew(o);
	CHECK(s->mhz == 0 && Cpu_Detect(o) && s->mhz == 2000 && Cpu_MeasureStep(o));
	Engine_Renew(o);
	CHECK(Cpu_Detect(o) && s->mhz == 2000 && Cpu_MeasureStep(o));
	CHECK(GetMachineKey(o) == ((((0xff2u | 0x8000u) << 8) | 'a') << 8 | 'W')); // the signature's low 12 bits only (not the type)
	gAmd = 0;
	gRec.log[0] = 0;
	CHECK(Host_CpuCount(o) == 2);
	memset(&ms, 0xff, sizeof ms);
	Host_MemoryStatus(o, &ms);
	CHECK(ms.totalPhys == 0x20000000u && ms.availPhys == 0x10000000u && ms.loadPercent == 0); // the host's, emptied first
	Host_MemoryStatusEx(o, &tp, &ap);
	CHECK(tp == (uint64_t)3 << 30 && ap == (uint64_t)1 << 30);
	Host_Version(o, ver, buf, sizeof buf);
	CHECK(ver[0] == 6 && ver[2] == 7601 && strcmp(buf, "Service Pack 1") == 0);
	CHECK(Host_UiLanguage(o) == 0x409);
	Host_DisplayAdapter(o, buf, sizeof buf, ver);
	CHECK(strcmp(buf, "Fake Adapter") == 0 && ver[0] == 1 && ver[3] == 4);
	CHECK(!DSound8_IsAvailable(o));
	CHECK(strcmp(gRec.log, "cpus;mem;memex;version;lang;adapter;audio;") == 0);
	// the key: the signature's low 12 bits, Windows 7 (0x8000), "a" of alice, "W" of WORKSTATION (asked into an emptied buffer)
	gRec.log[0] = 0;
	CHECK(GetMachineKey(o) == ((((0x6f6u | 0x8000u) << 8) | 'a') << 8 | 'W'));
	CHECK(GetMachineKeyXor(o, 0xffffffffu) == ~((((0x6f6u | 0x8000u) << 8) | 'a') << 8 | 'W'));
	CHECK(strstr(gRec.log, "user;computer(0);version;") != NULL);
	// the Windows versions in the key (bits 12 to 15 of its high half)
	{
		static const struct
		{
			uint32_t platform, major, minor, bits;
		} kVersions[] = {{0, 3, 1, 0x7000}, {1, 4, 0, 0x1000}, {1, 4, 10, 0x2000}, {1, 4, 90, 0x3000}, {1, 5, 0, 0x3000}, {1, 3, 0, 0xf000},
			{2, 3, 51, 0x5000}, {2, 4, 0, 0x6000}, {2, 5, 0, 0x4000}, {2, 5, 1, 0x8000}, {2, 5, 2, 0x8000}, {2, 6, 0, 0x8000}, {2, 10, 0, 0x8000},
			{2, 2, 0, 0xf000}, {3, 6, 1, 0xf000}};
		size_t i;
		for(i = 0; i < sizeof kVersions / sizeof kVersions[0]; i++)
		{
			gVersion[0] = kVersions[i].major;
			gVersion[1] = kVersions[i].minor;
			gVersion[3] = kVersions[i].platform;
			if((GetMachineKey(o) >> 16 & 0xf000u) != kVersions[i].bits)
			{
				printf("  FAIL: Windows %u.%u on platform %u: key bits %x\n", (unsigned)gVersion[0], (unsigned)gVersion[1],
					(unsigned)gVersion[3], (unsigned)(GetMachineKey(o) >> 16 & 0xf000u));
				gFails++;
			}
		}
		Regs(gVersion, 6, 1, 7601, 2);
	}
	CHECK(gRec.badUser == 0);
	OpenBGI_Delete(o);
}

static void TestShell(void)
{
	OpenBGI_Host_t host = Bare();
	OpenBGI_t* o;
	int exists = 0;
	printf("shell\n");
	host.shell = &kShell;
	o = New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	memset(&gSh, 0, sizeof gSh);
	/* a program a game asks for is not started, whatever it is and wherever:
	 * the answer is 0 and the host hears nothing of it (each said on stderr) */
	CHECK(Program_Refuse(o, "C:\\game", "reg.exe") == 0 && Program_Refuse(o, NULL, "uninst.exe") == 0 && Program_Refuse(o, "C:\\game", NULL) == 0);
	CHECK(gRec.log[0] == 0);
	// "80 E3": a web link and a document are opened, the link as it is, and nothing else is
	CHECK(ShellOpen_IsLink("http://openbgi.net/") && ShellOpen_IsLink("HTTPS://OpenBGI.net") && !ShellOpen_IsLink("ftp://x/") && !ShellOpen_IsLink("file:///C:/a.exe") &&
		!ShellOpen_IsLink("mailto:a@b") && !ShellOpen_IsLink("httpx://a") && !ShellOpen_IsLink("http:/a") && !ShellOpen_IsLink(""));
	CHECK(ShellOpen_IsDocument(o, "C:\\game\\Manual.TXT") && ShellOpen_IsDocument(o, "readme.html") && ShellOpen_IsDocument(o, "C:\\a.b\\guide.pdf") &&
		ShellOpen_IsDocument(o, "map.jpeg") && ShellOpen_IsDocument(o, "x.Htm"));
	CHECK(!ShellOpen_IsDocument(o, "C:\\game\\reg.exe") && !ShellOpen_IsDocument(o, "run.bat") && !ShellOpen_IsDocument(o, "a.txt.exe") &&
		!ShellOpen_IsDocument(o, "C:\\dir.txt\\tool") && !ShellOpen_IsDocument(o, "manual.txt ") && !ShellOpen_IsDocument(o, "link.lnk") &&
		!ShellOpen_IsDocument(o, "page.url") && !ShellOpen_IsDocument(o, "help.chm") && !ShellOpen_IsDocument(o, "setup.msi") &&
		!ShellOpen_IsDocument(o, "txt") && !ShellOpen_IsDocument(o, "") && !ShellOpen_IsDocument(o, "C:\\game\\") && !ShellOpen_IsDocument(o, "a.docm"));
	CHECK(!ShellOpen_IsDocument(o, "file:///C:/game/a.txt") && !ShellOpen_IsDocument(o, "\\\\server\\share\\a.txt") && !ShellOpen_IsDocument(o, "//server/a.txt") &&
		!ShellOpen_IsDocument(o, "mailto:a@b.txt") && !ShellOpen_IsDocument(o, "C:\\game\\a.txt:stream.txt") && !ShellOpen_IsDocument(o, "1:\\a.txt") &&
		ShellOpen_IsDocument(o, "d:/game/a.txt") && ShellOpen_IsDocument(o, "..\\a.txt"));
	CHECK(ShellOpen(o, "http://openbgi.net/") && ShellOpen(o, "C:\\movies\\manual.txt"));
	CHECK(strcmp(gRec.log, "open(http://openbgi.net/);open(C:\\movies\\manual.txt);") == 0);
	gRec.log[0] = 0;
	CHECK(!ShellOpen(o, "C:\\movies\\ESS.exe") && !ShellOpen(o, "cmd /c del x") && !ShellOpen(o, "file:///C:/movies/a.txt") && !ShellOpen(o, NULL));
	CHECK(gRec.log[0] == 0);
	// the instance lock, the IPC, the clipboard, the wallpaper, the shortcuts, the association
	gSh.lockHeld = 1;
	CHECK(Host_InstanceLock(o, "name", &exists) == &gLock && exists == 1);
	Host_InstanceUnlock(o, &gLock);
	CHECK(Host_SendToRunning(o, "BGI - Main window", "C:\\game\\x.arc"));
	CHECK(Host_ClipboardSetText(o, "copied") && Host_SetWallpaper(o, "C:\\w.bmp", 1, 0) == 7);
	CHECK(Host_CreateShortcut(o, "C:\\l.lnk", "C:\\t.exe", NULL) && Host_CreateShortcut(o, "C:\\l.lnk", "C:\\t.exe", "/x"));
	Host_NotifyAssocChanged(o);
	Host_InstanceUnlock(o, NULL); // nothing to release
	CHECK(strcmp(gRec.log, "lock(name);unlock;send(BGI - Main window,C:\\game\\x.arc);clip(copied);"
						   "wall(C:\\w.bmp,1,0);link(C:\\l.lnk,C:\\t.exe,NULL);link(C:\\l.lnk,C:\\t.exe,/x);assoc;") == 0);
	CHECK(gRec.badUser == 0);
	OpenBGI_Delete(o);
}

static void TestCd(void)
{
	OpenBGI_Host_t host = Bare();
	OpenBGI_t* o;
	int32_t mode = -5;
	printf("cd\n");
	host.cdAudio = &kCd;
	o = New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	strcpy(o->files.baseDir, "D:\\GAME\\");
	o->files.driveType['d' - 'a'] = OPENBGI_DRIVE_CDROM;
	CHECK(Cd_Open(o) && Cd_Open(o)); // the second finds it open
	CHECK(Cd_Play(o, 3, 1));
	CHECK(Cd_Status(o, &mode) && mode == 2); // playing
	Cd_Stop(o);
	Sound_CdClose(o);
	CHECK(strcmp(gRec.log, "open(d:);tmsf(7);status(7,7);play(7,3,4,1);status(7,4);stop(7);close(7);") == 0);
	CHECK(!Cd_Play(o, 3, 0) && !Cd_Status(o, &mode)); // closed
	// on a fixed disk: the default drive; a device that cannot be set up is closed again
	gRec.log[0] = 0;
	strcpy(o->files.baseDir, "C:\\GAME\\");
	gCdSetFails = 1;
	CHECK(!Cd_Open(o) && strcmp(gRec.log, "open(NULL);tmsf(7);close(7);") == 0);
	gCdSetFails = 0;
	// the disc directory on the CD-ROM drive; the modes; the last track; the replay; a disc without tracks
	gRec.log[0] = 0;
	strcpy(o->files.altDir, "E:\\DISC\\");
	o->files.driveType['e' - 'a'] = OPENBGI_DRIVE_CDROM;
	CHECK(Cd_Open(o));
	gCdMode = 0x20c;
	CHECK(Cd_Status(o, &mode) && mode == 0); // not ready
	gCdMode = 0x212;
	CHECK(Cd_Status(o, &mode) && mode == 6); // open
	gCdMode = 0x213;
	CHECK(Cd_Status(o, &mode) && mode == -1);
	gCdMode = 0x20b;
	CHECK(Cd_Status(o, &mode) && mode == -1);
	gCdMode = 0x20e;
	CHECK(Cd_Play(o, 255, 0));
	Cd_Replay(o);
	gCdTracks = 0;
	CHECK(!Cd_Play(o, 2, 0));
	gCdTracks = 12;
	Sound_CdClose(o);
	CHECK(strcmp(gRec.log, "open(e:);tmsf(7);status(7,4);status(7,4);status(7,4);status(7,4);status(7,7);play(7,255,0,0);status(7,7);"
						   "play(7,255,0,1);status(7,7);close(7);") == 0);
	CHECK(gRec.badUser == 0);
	OpenBGI_Delete(o);
}

static void TestPlayer(void)
{
	OpenBGI_Host_t host = Bare();
	OpenBGI_t* o;
	int32_t len = 0;
	printf("player\n");
	host.moviePlayer = &kPlayer;
	o = New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	Gfx_Create(o, 0xffffffffu, 1);
	Screen_Setup(o, 320, 240, PM_RGB32);
	CHECK(Movie_SetVolume(o, 0x80) && o->movie.movieVolume == 0);
	CHECK(Movie_Play(o, &len, "C:\\movies\\op.mpg", 10, 20, 300, 200) && len == 1234);
	CHECK(Movie_IsPlaying(o) && o->movie.moviePlaying && !o->movie.movieFx);
	CHECK(Movie_SetVolume(o, 0) && o->movie.movieVolume == -10000);
	CHECK(Movie_Stop(o) && !o->movie.moviePlaying);
	CHECK(strcmp(gRec.log, "pvol(0);popen(C:\\movies\\op.mpg);pstart(10,20,300,200,0);prunning;pvol(-10000);pstop;") == 0);
	CHECK(gRec.badUser == 0);
	Gfx_Destroy(o);
	OpenBGI_Delete(o);
}

// read out everything the engine mixed: the stereo frames read; *match counts those that are (l, r)
static int Drain(OpenBGI_t* o, int16_t l, int16_t r, int* match)
{
	static int16_t buf[8192 * 2];
	int n, i, total = 0;
	*match = 0;
	while((n = OpenBGI_AudioRead(o, buf, 8192)) > 0)
	{
		for(i = 0; i < n; i++)
			if(buf[i * 2] == l && buf[i * 2 + 1] == r)
				(*match)++;
		total += n;
	}
	return total;
}

// the grey of the movie's frame at (10, 10) of the picture
static uint32_t Shown(OpenBGI_t* o)
{
	OpenBGI_Frame_t f = {.size = sizeof f};
	OpenBGI_RunFrame(o, &f);
	return f.pixels ? ((const uint32_t*)(f.pixels + 10 * f.pitch))[10] & 0xffffff : 0xdeadu;
}

static void TestDecoder(void)
{
	OpenBGI_Host_t host = Bare();
	OpenBGI_t* o;
	int32_t len = 0;
	int k;
	printf("decoder\n");
	host.movieDecoder = &kDecoder;
	o = New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	Gfx_Create(o, 0xffffffffu, 1);
	Screen_Setup(o, 320, 240, PM_RGB32);
	o->display.windowAlive = 1;
	CHECK(Wm_Init(o) == 0); // the mixer runs: the sound is asked for too
	CHECK(Movie_SetVolume(o, 0x80));
	// a file that is not there: the decoder is not asked
	memset(&gFile, 0, sizeof gFile);
	CHECK(!Movie_Play(o, &len, "C:\\none\\op.mpg", 8, 8, 16, 16) && !o->movie.movieFx && gRec.log[0] == 0 && gFile.opens == 0);
	// the file is opened by the engine and read by the decoder through it; no file without a movie file to read
	{
		OpenBGI_MovieInfo_t info;
		CHECK(Host_DecoderOpen(o, NULL, "C:\\movies\\op.mpg", 16, 16, 1, &info) == NULL && gRec.log[0] == 0);
	}
	CHECK(Movie_Play(o, &len, "C:\\movies\\op.mpg", 8, 8, 16, 16) && len == 500);
	CHECK(o->movie.movieFx && gDec.opened && gDec.w == 16 && gDec.h == 16 && gDec.audio);
	CHECK(strcmp(gRec.log, "dopen(C:\\movies\\op.mpg,16,16,1);") == 0);
	CHECK(gFile.opens == 1 && gFile.open == 1); // it stays open while the movie runs
	// nothing decoded yet: the clock does not start and none of the sound is taken
	gDec.hold = 1;
	gNow += 20;
	Movie_Tick(o);
	OpenBGI_RunAudio(o);
	CHECK(Drain(o, 0, 0, &k) == 882 && k == 882);
	CHECK(gDec.taken == 0 && gDec.soundLeft == DEC_SOUND && o->movie.moviePlaying);
	// the first frame: shown, and the clock starts
	gDec.hold = 0;
	Movie_Tick(o);
	CHECK(gDec.taken == 1 && Shown(o) == 0x101010u);
	// 50 ms: the sound mixed at full volume; frame 1 not due yet
	gNow += 50;
	OpenBGI_RunAudio(o);
	CHECK(Drain(o, 1000, -1000, &k) == 2205 && k == 2205 && gDec.soundLeft == DEC_SOUND - 2205);
	Movie_Tick(o);
	CHECK(gDec.taken == 1);
	// 100 ms: silent, the sound taken all the same
	CHECK(Movie_SetVolume(o, 0));
	gNow += 50;
	OpenBGI_RunAudio(o);
	CHECK(Drain(o, 0, 0, &k) == 2205 && k == 2205 && gDec.soundLeft == DEC_SOUND - 4410);
	// 150 ms: half the volume (-24 dB: a gain of 4135/65536); frame 1 due
	CHECK(Movie_SetVolume(o, 0x40));
	gNow += 50;
	OpenBGI_RunAudio(o);
	CHECK(Drain(o, 63, -64, &k) == 2205 && k == 2205);
	Movie_Tick(o);
	CHECK(gDec.taken == 2 && Shown(o) == 0x202020u);
	// 350 ms: frame 2 dropped, frame 3 shown
	CHECK(Movie_SetVolume(o, 0x80));
	gNow += 200;
	Movie_Tick(o);
	CHECK(gDec.taken == 4 && gDec.tooOld == 0 && Shown(o) == 0x404040u);
	OpenBGI_RunAudio(o);
	CHECK(Drain(o, 1000, -1000, &k) == 8820 && k == 8820 && gDec.soundLeft == 11025);
	// 500 ms: the last frame; the sound plays on, and so does the movie
	gNow += 150;
	OpenBGI_RunAudio(o);
	CHECK(Drain(o, 1000, -1000, &k) == 6615 && k == 6615);
	Movie_Tick(o);
	CHECK(gDec.taken == DEC_FRAMES && Shown(o) == 0x505050u);
	CHECK(gDec.soundLeft == 4410 && o->movie.moviePlaying && !gDec.closed);
	gNow += 50;
	OpenBGI_RunAudio(o);
	CHECK(Drain(o, 1000, -1000, &k) == 2205 && k == 2205);
	Movie_Tick(o);
	CHECK(gDec.soundLeft == 2205 && o->movie.moviePlaying && !gDec.closed);
	// 610 ms: the end of the sound (silence after it), then of the movie
	gNow += 60;
	OpenBGI_RunAudio(o);
	CHECK(Drain(o, 1000, -1000, &k) == 2646 && k == 2205 && gDec.soundLeft == 0);
	Movie_Tick(o);
	CHECK(!o->movie.moviePlaying && !o->movie.movieFx && gDec.closed && !MovieFx_IsRunning(o) && o->snd.externalMix == NULL);
	// the engine's sound goes on without it
	gNow += 20;
	OpenBGI_RunAudio(o);
	CHECK(Drain(o, 0, 0, &k) == 882 && k == 882);
	// without the mixer the sound is not asked for, not taken and not waited for
	Wm_Shutdown(o);
	gRec.log[0] = 0;
	CHECK(Movie_Play(o, &len, "C:\\movies\\op.mpg", 8, 8, 16, 16) && !gDec.audio && o->snd.externalMix == NULL);
	Movie_Tick(o);
	gNow += 400;
	Movie_Tick(o);
	CHECK(gDec.taken == DEC_FRAMES && gDec.soundLeft == DEC_SOUND && gDec.closed && !o->movie.moviePlaying);
	// stopped before its end: closed
	CHECK(Movie_Play(o, &len, "C:\\movies\\op.mpg", 8, 8, 16, 16));
	Movie_Tick(o);
	CHECK(Movie_Stop(o) && gDec.closed && !MovieFx_IsRunning(o));
	CHECK(strcmp(gRec.log, "dopen(C:\\movies\\op.mpg,16,16,0);dclose;dopen(C:\\movies\\op.mpg,16,16,0);dclose;") == 0);
	CHECK(gRec.badUser == 0);
	Gfx_Destroy(o);
	OpenBGI_Delete(o);
}

// ---- partial tables: the members a host leaves out ------------------------------------------------

static int PCpuCount(void* user)
{
	Note(user, "cpus");
	return 0; // (not a count)
}

static int PComputerName(void* user, char* buf, size_t n)
{
	Note(user, "computer");
	(void)buf;
	(void)n;
	return 0; // fails, leaving the buffer as it was
}

static const OpenBGI_Machine_t kPartialMachine = {.size = sizeof(OpenBGI_Machine_t), .cpuCount = PCpuCount, .computerName = PComputerName};

static void* PInstanceLock(void* user, const char* name, int* alreadyExists)
{
	Note(user, "lock(%s)", name);
	(void)alreadyExists; // not said
	return &gLock;
}

static const OpenBGI_Shell_t kPartialShell = {.size = sizeof(OpenBGI_Shell_t), .instanceLock = PInstanceLock};

static const OpenBGI_MovieDecoder_t kPartialDecoder = {.size = sizeof(OpenBGI_MovieDecoder_t), .open = DecOpen, .close = DecClose}; // neither frames nor sound

static void TestBoth(void)
{
	OpenBGI_Host_t host = Bare();
	OpenBGI_t* o;
	int32_t len = 0;
	printf("both\n");
	host.moviePlayer = &kPlayer;
	host.movieDecoder = &kDecoder;
	o = New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	Gfx_Create(o, 0xffffffffu, 1);
	Screen_Setup(o, 320, 240, PM_RGB32);
	o->display.windowAlive = 1;
	// the decoder, and nothing asked of the player
	CHECK(Movie_Play(o, &len, "C:\\movies\\op.mpg", 8, 8, 16, 16) && o->movie.movieFx && !o->movie.movieGraph);
	CHECK(Movie_Stop(o) && strcmp(gRec.log, "dopen(C:\\movies\\op.mpg,16,16,0);dclose;") == 0);
	// the player, asked for on the command line
	gRec.log[0] = 0;
	o->engine.optMovies = 2;
	CHECK(Movie_Play(o, &len, "C:\\movies\\op.mpg", 8, 8, 16, 16) && !o->movie.movieFx && o->movie.movieGraph && len == 1234);
	CHECK(Movie_Stop(o) && strcmp(gRec.log, "popen(C:\\movies\\op.mpg);pstart(8,8,16,16,0);pstop;") == 0);
	// or in openbgi.json, unless the command line says the decoder
	gRec.log[0] = 0;
	o->engine.optMovies = 0;
	o->engine.moviesPlayer = 1;
	CHECK(Movie_Play(o, &len, "C:\\movies\\op.mpg", 8, 8, 16, 16) && o->movie.movieGraph);
	CHECK(Movie_Stop(o) && strncmp(gRec.log, "popen(", 6) == 0);
	gRec.log[0] = 0;
	o->engine.optMovies = 1;
	CHECK(Movie_Play(o, &len, "C:\\movies\\op.mpg", 8, 8, 16, 16) && o->movie.movieFx);
	CHECK(Movie_Stop(o) && strncmp(gRec.log, "dopen(", 6) == 0);
	// a file the player cannot play goes to the decoder
	gRec.log[0] = 0;
	o->engine.optMovies = 2;
	gPlayerFails = 1;
	CHECK(Movie_Play(o, &len, "C:\\movies\\op.mpg", 8, 8, 16, 16) && o->movie.movieFx && !o->movie.movieGraph);
	CHECK(Movie_Stop(o) && strcmp(gRec.log, "popen(C:\\movies\\op.mpg);dopen(C:\\movies\\op.mpg,16,16,0);dclose;") == 0);
	gPlayerFails = 0;
	CHECK(gRec.badUser == 0);
	Gfx_Destroy(o);
	OpenBGI_Delete(o);
	// the command line's --movies= (with --version, which ends the start at once)
	o = New(&host);
	if(!o)
		return;
	OpenBGI_Start(o, "--movies=player --version");
	CHECK(o->engine.optMovies == 2);
	OpenBGI_Stop(o);
	OpenBGI_Start(o, "--movies=decoder --version");
	CHECK(o->engine.optMovies == 1);
	OpenBGI_Stop(o);
	OpenBGI_Start(o, "--movies=other --version");
	CHECK(o->engine.optMovies == 0);
	OpenBGI_Stop(o);
	// the language of the messages is the process's: --lang= sets it; a run out of sight (--preview),
	// which goes on beside the engine whose language it is, leaves it as it finds it
	OpenBGI_Start(o, "--lang=ja --version");
	CHECK(!gMsgEnglish);
	OpenBGI_Stop(o);
	OpenBGI_Start(o, "--lang=en --preview --version");
	CHECK(!gMsgEnglish && o->engine.optPreview);
	OpenBGI_Stop(o);
	OpenBGI_Start(o, "--lang=en --version");
	CHECK(gMsgEnglish);
	OpenBGI_Stop(o);
	OpenBGI_Delete(o);
}

static void TestPartial(void)
{
	OpenBGI_Host_t host = Bare();
	OpenBGI_t* o;
	OpenBGI_MemoryStatus_t ms;
	uint32_t ver[4];
	char buf[0x100];
	int exists = 5;
	printf("partial\n");
	host.machine = &kPartialMachine;
	host.shell = &kPartialShell;
	host.movieDecoder = &kPartialDecoder;
	o = New(&host);
	CHECK(o != NULL);
	if(!o)
		return;
	// the machine: no CPUID, one processor, no computer name; the rest the ordinary machine's
	CHECK(!Host_HasCpuid(o) && Host_ReadTsc(o) == 0 && !Cpu_Detect(o));
	CHECK(Host_CpuCount(o) == 1);
	strcpy(buf, "stale");
	Host_ComputerName(o, buf, sizeof buf);
	CHECK(buf[0] == 0);
	Host_UserName(o, buf, sizeof buf);
	CHECK(strcmp(buf, "user") == 0);
	Host_MemoryStatus(o, &ms);
	CHECK(ms.totalPhys == 0x40000000u && ms.loadPercent == 50);
	Host_Version(o, ver, buf, sizeof buf);
	CHECK(ver[0] == 5 && ver[2] == 2600 && strcmp(buf, "Service Pack 3") == 0);
	CHECK(Host_UiLanguage(o) == 0x411 && Host_AudioAvailable(o));
	CHECK(GetMachineKey(o) == ((0x8000u << 16) | ('u' << 8))); // no computer name: a zero
	// the shell: a lock that does not say is not held; nothing opens a link
	CHECK(!ShellOpen(o, "http://openbgi.net/"));
	CHECK(Host_InstanceLock(o, "n", &exists) == &gLock && exists == 0);
	Host_InstanceUnlock(o, &gLock);
	CHECK(!Host_ClipboardSetText(o, "t") && !Host_SendToRunning(o, "c", "d"));
	// the decoder: a movie without frames or sound (asked for) is over at once
	{
		int32_t len = 0;
		Gfx_Create(o, 0xffffffffu, 1);
		Screen_Setup(o, 320, 240, PM_RGB32);
		o->display.windowAlive = 1;
		CHECK(Wm_Init(o) == 0);
		CHECK(Movie_Play(o, &len, "C:\\movies\\op.mpg", 0, 0, 16, 16) && o->movie.moviePlaying);
		gNow += 20;
		OpenBGI_RunAudio(o);
		Movie_Tick(o);
		OpenBGI_RunAudio(o);
		CHECK(!o->movie.moviePlaying && gDec.closed && gDec.taken == 0);
		Wm_Shutdown(o);
		Gfx_Destroy(o);
	}
	CHECK(strcmp(gRec.log, "cpus;computer;computer;lock(n);dopen(C:\\movies\\op.mpg,16,16,1);dclose;") == 0);
	CHECK(gRec.badUser == 0);
	OpenBGI_Delete(o);
}

// ---- the OS layer's decoder: the movie library over its files -----------------------------------

#if !defined(BGI_WASM)
#define FF_MOVIE "bin/test_hostsys.mpg"
#define FF_KEPT  "bin/test_ffmovie/red.mpg" // the same movie as tests/ffmovie.c makes and keeps it

/* A second of a red 32 x 24 picture at 25 frames a second and a 440 Hz
 * tone: FF_MOVIE, written by the ffmpeg program; where that cannot be
 * run (a Windows build under wine), the one test_ffmovie left behind in
 * a Linux build's run.  NULL without either. */
static const char* MakeMovie(void)
{
	const char* argv[] = {"ffmpeg", "-v", "quiet", "-nostdin", "-y", "-f", "lavfi", "-i", "color=c=red:s=32x24:r=25:d=1", "-f", "lavfi",
		"-i", "sine=frequency=440:sample_rate=44100:duration=1", "-c:v", "mpeg1video", "-c:a", "mp2", FF_MOVIE, NULL};
	OsPipeProc_t* p;
	char tmp[64];
	FILE* f;
	remove(FF_MOVIE);
	p = OS_PipeProcStart(argv);
	while(p && OS_PipeProcRead(p, tmp, sizeof tmp) > 0)
		;
	OS_PipeProcStop(p);
	f = fopen(FF_MOVIE, "rb");
	if(f)
	{
		fclose(f);
		return FF_MOVIE;
	}
	f = fopen(FF_KEPT, "rb");
	if(f)
	{
		fclose(f);
		return FF_KEPT;
	}
	return NULL;
}

// a file of the test's own for the OS layer's decoder, as the engine would hand it one
static int StdRead(void* ctx, uint8_t* buf, int n)
{
	return n > 0 ? (int)fread(buf, 1, (size_t)n, (FILE*)ctx) : 0;
}

static int StdSeek(void* ctx, int64_t pos)
{
	return pos >= 0 && fseek((FILE*)ctx, (long)pos, SEEK_SET) == 0;
}

static FILE* StdOpen(const char* path, OpenBGI_MovieFile_t* file)
{
	FILE* f = fopen(path, "rb");
	memset(file, 0, sizeof *file);
	file->size = sizeof *file;
	file->ctx = f;
	file->read = StdRead;
	file->seek = StdSeek;
	file->length = -1;
	if(f && fseek(f, 0, SEEK_END) == 0)
	{
		file->length = ftell(f);
		fseek(f, 0, SEEK_SET);
	}
	return f;
}

static void TestFfmpeg(void)
{
	const OpenBGI_MovieDecoder_t* d = OsCommon_MovieDecoder();
	OpenBGI_MovieInfo_t info;
	static uint8_t px[16 * 12 * 4];
	static int16_t snd[4096 * 2];
	OpenBGI_MovieFile_t file;
	const char* movie;
	FILE* f;
	void* m;
	int32_t frames = 0, last = -1, n;
	int vEnd = 0, aEnd = 0, red = 0, loud = 0, i, spins;
	long samples = 0;
	printf("ffmpeg\n");
	if(!d)
	{
		printf("  (skipped: a build without the movie decoder)\n");
		return;
	}
	movie = MakeMovie();
	if(!movie)
	{
		printf("  (skipped: no ffmpeg program to make a movie with, and none left by test_ffmovie)\n");
		return;
	}
	memset(&info, 0, sizeof info);
	f = StdOpen(movie, &file);
	CHECK(f != NULL);
	if(!f)
		return;
	m = d->open(NULL, &file, movie, 16, 12, 1, &info);
	CHECK(m != NULL);
	if(!m)
	{
		fclose(f);
		return;
	}
	// (an MPEG stream's frames are numbered at twice its frame rate: every other number has a picture)
	CHECK(info.fps == 50.0 && info.lengthMs >= 950 && info.lengthMs <= 1100 && info.audio);
	// the frames as they come (the host holds four pictures), and the sound
	for(spins = 0; spins < 4000 && !(vEnd && aEnd && last + 1 >= frames); spins++)
	{
		n = d->frames(NULL, m, &vEnd);
		if(n > last + 1)
		{
			CHECK(n - (last + 1) <= 8);
			CHECK(d->take(NULL, m, n - 1, px));
			red += px[2] > 200 && px[1] < 50 && px[0] < 50;
			if(last >= 0)
				CHECK(!d->take(NULL, m, last, px)); // let go
			CHECK(!d->take(NULL, m, n, px));        // not decoded
			last = n - 1;
			frames = n;
		}
		while((n = d->samples(NULL, m, snd, 4096, &aEnd)) > 0)
		{
			for(i = 0; i < n * 2; i++)
				loud += snd[i] > 2000 || snd[i] < -2000;
			samples += n;
		}
	}
	CHECK(vEnd && aEnd && frames >= 48 && frames <= 52 && red > 0);
	CHECK(samples >= 44100 - 2205 && samples <= 44100 + 2205 && loud > 0);
	d->close(NULL, m);
	fclose(f);
	// closed while it runs
	f = StdOpen(movie, &file);
	m = f ? d->open(NULL, &file, movie, 16, 12, 1, &info) : NULL;
	CHECK(m != NULL);
	if(m)
		d->close(NULL, m);
	if(f)
		fclose(f);
	// a file that is no movie: no decoder for it (said on stderr)
	f = StdOpen("Makefile", &file);
	CHECK(f != NULL && d->open(NULL, &file, "Makefile", 16, 12, 1, &info) == NULL);
	if(f)
		fclose(f);
	d->close(NULL, NULL);
	remove(FF_MOVIE);
}
#endif

int main(void)
{
	gFiles.size = sizeof gFiles;
	gFiles.attrs = FAttrs;
	gFiles.open = FOpen;
	gFiles.close = FClose;
	gFiles.read = FRead;
	gFiles.seek = FSeek;
	gFiles.length = FLength;
	TestPlain();
	TestMachine();
	TestShell();
	TestCd();
	TestPlayer();
	TestDecoder();
	TestBoth();
	TestPartial();
	// every movie's file was closed again, and none before its decoder was
	CHECK(gFile.opens > 3 && gFile.opens == gFile.closes && gFile.open == 0 && !gFile.closedTooEarly);
#if !defined(BGI_WASM)
	TestFfmpeg();
#endif
	if(gFails)
	{
		printf("hostsys: %d checks failed\n", gFails);
		return 1;
	}
	printf("hostsys: all tests passed\n");
	return 0;
}
