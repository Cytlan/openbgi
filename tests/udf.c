/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/udf.c - unit test of the reader of a disc image's UDF part
 *               (src/fs/udf.c) behind the volumes (src/include/bgi/vol.h);
 *               built as test_udf by `make test`
 *
 * The images are written by the test itself into a scratch directory in
 * the temporary directory.  The groups, in the order they run:
 *
 *   names    - a disc with a UDF part alone, laid out like a game's: the
 *              names as recorded (one byte a character, two bytes, a
 *              character above U+FFFF), the game's folder found and made
 *              the root, an identifier marked as deleted left out
 *   read     - the bytes of a file in one extent, in two that follow each
 *              other, in its own entry, of an empty one; long extent
 *              descriptors, an extended entry, an entry with attributes
 *              before its extents; a directory in its own entry, one in
 *              two runs, one whose identifiers cross a block's end; the
 *              time stamp (also of a disc recorded in another time zone)
 *   refused  - listed but not readable: extents apart, an extent not
 *              recorded, extents that go on in another block, a file past
 *              the end of a cut image; a folder that holds itself ends
 *   metadata - the same disc with the entries and directories in a
 *              metadata partition whose file lies in two runs
 *   bridge   - beside ISO 9660: the UDF names instead of the short ones,
 *              but the Joliet names when there are any; the short names
 *              when the UDF part cannot be followed (blocks of another
 *              size, a virtual partition); a sparable partition read as a
 *              plain one; no ISO 9660 and nothing to follow: no image
 *
 * The scratch directory is removed at the end, unless OPENBGI_UDF_KEEP is
 * set (to look at the images with another program).
 */
#include "bgi/core.h"
#include "bgi/bytes.h"
#include "bgi/host.h"
#include "host/os.h"
#include "bgi/vol.h"

#include "testhost.h"

#include <stdio.h>
#include <stdlib.h>

#include "check.h"

// ---- writing images ------------------------------------------------------------------------

#define SECTOR      2048u
#define PART        288u // the partition's first sector
#define DATA0       100u // the partition's first block of file data
#define IMG_SECTORS 700u
#define MAX_NODES   64

// how a file's bytes (or a folder's identifiers) are recorded
enum
{
	FORM_ONE,    // one extent
	FORM_INSIDE, // in the entry itself
	FORM_SPLIT,  // two extents, the second right after the first
	FORM_APART,  // two extents with a block between them
	FORM_HOLE,   // an extent allocated, not recorded
	FORM_MORE,   // the extents go on in another block
};

#define NODE_LONG    0x01 // long extent descriptors
#define NODE_EXT     0x02 // an extended entry
#define NODE_ATTRS   0x04 // attributes before the extents
#define NODE_DELETED 0x08 // its identifier is marked as deleted
#define NODE_LOOP    0x10 // an identifier that names the folder it is in
#define NODE_USE     0x20 // its identifier has implementation use bytes before the name

// a file or folder of an image to write
typedef struct Node
{
	int parent;       // the index of its folder (-1: the root itself, node 0)
	const char* name; // UTF-8
	const char* data; // NULL for a folder
	uint32_t size;    // bytes of data
	int form, flags;
	// set by the writer
	uint32_t entry; // the block of its entry
	uint32_t first; // the sector of its first byte (a file in extents)
} Node_t;

static Node_t gNodes[MAX_NODES];
static int gNodeCount;
static int gZone;          // the time zone of the entries written: minutes east of UTC
static int gMeta;          // the entries and directories are in a metadata partition
static uint32_t gBlock;    // the block size the logical volume states
static int gMap;           // the kind of the first partition map: 0 plain, 1 sparable, 2 virtual
static uint32_t gNextMeta; // the next free block for an entry or a directory
static uint32_t gNextData; // .. and for file data
static uint8_t* gImg;      // the image being written

static int Add(int parent, const char* name, const char* data, uint32_t size, int form, int flags)
{
	Node_t* n = &gNodes[gNodeCount];
	memset(n, 0, sizeof *n);
	n->parent = parent;
	n->name = name;
	n->data = data;
	n->size = size;
	n->form = form;
	n->flags = flags;
	return gNodeCount++;
}

// the sector of a block that holds entries or directories
static uint32_t MetaSector(uint32_t lbn)
{
	if(!gMeta)
		return PART + lbn;
	return PART + (lbn < 8 ? 2 + lbn : 40 + (lbn - 8)); // the two runs of the metadata file
}

static unsigned MetaPart(void)
{
	return gMeta ? 1 : 0;
}

static uint32_t AllocMeta(uint32_t blocks)
{
	uint32_t lbn;
	if(gMeta && gNextMeta < 8 && gNextMeta + blocks > 8)
		gNextMeta = 8; // (not across the two runs)
	lbn = gNextMeta;
	gNextMeta += blocks;
	return lbn;
}

static unsigned Crc16(const uint8_t* p, size_t n)
{
	unsigned crc = 0;
	int i;
	while(n--)
	{
		crc ^= (unsigned)*p++ << 8;
		for(i = 0; i < 8; i++)
			crc = ((crc & 0x8000u) ? (crc << 1) ^ 0x1021u : crc << 1) & 0xffffu;
	}
	return crc;
}

// the tag of the descriptor of `len` bytes at `d`, recorded in block `loc`
static void Tag(uint8_t* d, unsigned id, uint32_t loc, size_t len)
{
	unsigned sum = 0, i;
	PutLe16(d, id);
	PutLe16(d + 2, 2);
	d[4] = 0;
	PutLe16(d + 8, Crc16(d + 16, len - 16));
	PutLe16(d + 10, (uint32_t)(len - 16));
	PutLe32(d + 12, loc);
	for(i = 0; i < 16; i++)
		sum += d[i];
	d[4] = (uint8_t)sum;
}

// a UTF-8 name as an identifier's: a byte that says one byte a character (8) or two (16), then the characters; the bytes
static size_t Name(const char* s, uint8_t* out)
{
	const uint8_t* u = (const uint8_t*)s;
	uint32_t cps[0x100];
	size_t count = 0, n = 1, i;
	int wide = 0;
	while(*u)
	{
		uint32_t c;
		if(*u < 0x80)
			c = *u++;
		else if(*u < 0xe0)
			c = ((u[0] & 0x1fu) << 6) | (u[1] & 0x3fu), u += 2;
		else if(*u < 0xf0)
			c = ((u[0] & 0x0fu) << 12) | ((u[1] & 0x3fu) << 6) | (u[2] & 0x3fu), u += 3;
		else
			c = ((u[0] & 0x07u) << 18) | ((u[1] & 0x3fu) << 12) | ((u[2] & 0x3fu) << 6) | (u[3] & 0x3fu), u += 4;
		if(c > 0xff)
			wide = 1;
		cps[count++] = c;
	}
	out[0] = wide ? 16 : 8;
	for(i = 0; i < count; i++)
	{
		uint32_t c = cps[i];
		if(!wide)
			out[n++] = (uint8_t)c;
		else if(c < 0x10000)
			out[n++] = (uint8_t)(c >> 8), out[n++] = (uint8_t)c;
		else
		{
			uint32_t hi = 0xd800 + ((c - 0x10000) >> 10), lo = 0xdc00 + ((c - 0x10000) & 0x3ff);
			out[n++] = (uint8_t)(hi >> 8), out[n++] = (uint8_t)hi;
			out[n++] = (uint8_t)(lo >> 8), out[n++] = (uint8_t)lo;
		}
	}
	return n;
}

// an extent descriptor at `a`, short or long; the bytes it takes
static uint32_t Extent(uint8_t* a, int isLong, uint32_t len, uint32_t lbn, unsigned part)
{
	PutLe32(a, len);
	PutLe32(a + 4, lbn);
	if(isLong)
		PutLe16(a + 8, part);
	return isLong ? 16 : 8;
}

// the entry in block `lbn` (sector `sector`) of a file of the kind `kind`, with its extent descriptors (or its bytes: adKind 3)
static void Entry(uint32_t lbn, uint32_t sector, unsigned kind, int flags, unsigned adKind, const uint8_t* ads, uint32_t adLen, uint64_t size)
{
	uint8_t* d = gImg + (size_t)sector * SECTOR;
	uint32_t head = (flags & NODE_EXT) ? 216 : 176, attrs = (flags & NODE_ATTRS) ? 24 : 0;
	uint8_t* t = d + ((flags & NODE_EXT) ? 92 : 84);
	PutLe16(d + 20, 4);
	PutLe16(d + 24, 1);
	d[27] = (uint8_t)kind;
	PutLe16(d + 34, adKind);
	PutLe16(d + 48, 1);
	PutLe32(d + 56, (uint32_t)size);
	PutLe32(d + 60, (uint32_t)(size >> 32));
	PutLe16(t, 0x1000u | ((unsigned)gZone & 0xfffu)); // 2020-05-17 12:34:56 there
	PutLe16(t + 2, 2020);
	t[4] = 5, t[5] = 17, t[6] = 12, t[7] = 34, t[8] = 56;
	PutLe32(d + head - 8, attrs);
	PutLe32(d + head - 4, adLen);
	if(attrs)
	{ // the header of the attributes, with none after it
		PutLe32(d + head + 16, 24);
		PutLe32(d + head + 20, 24);
		Tag(d + head, 262, lbn, 24);
	}
	memcpy(d + head + attrs, ads, adLen);
	Tag(d, (flags & NODE_EXT) ? 266 : 261, lbn, head + attrs + adLen);
}

// a file's bytes and its entry
static void WriteFile(Node_t* nd)
{
	uint8_t ads[64];
	int isLong = (nd->flags & NODE_LONG) || gMeta;
	uint32_t adLen = 0, blocks = (nd->size + SECTOR - 1) / SECTOR, lbn = gNextData;
	uint8_t* at = gImg + (size_t)(PART + lbn) * SECTOR;
	memset(ads, 0, sizeof ads);
	nd->first = PART + lbn;
	if(nd->form == FORM_INSIDE)
	{
		Entry(nd->entry, MetaSector(nd->entry), 5, nd->flags, 3, (const uint8_t*)nd->data, nd->size, nd->size);
		return;
	}
	if(nd->form == FORM_ONE || nd->form == FORM_HOLE)
	{
		memcpy(at, nd->data, nd->size);
		if(nd->size)
			adLen = Extent(ads, isLong, nd->size | (nd->form == FORM_HOLE ? 1u << 30 : 0), lbn, 0);
		gNextData += blocks;
	}
	else if(nd->form == FORM_MORE)
	{ // the first block, and a pointer to where the descriptors would go on
		memcpy(at, nd->data, nd->size);
		adLen = Extent(ads, isLong, SECTOR, lbn, 0);
		adLen += Extent(ads + adLen, isLong, SECTOR | (3u << 30), lbn + blocks, 0);
		gNextData += blocks + 1;
	}
	else
	{
		uint32_t gap = nd->form == FORM_APART ? 1 : 0;
		memcpy(at, nd->data, SECTOR);
		memcpy(at + (size_t)(1 + gap) * SECTOR, nd->data + SECTOR, nd->size - SECTOR);
		adLen = Extent(ads, isLong, SECTOR, lbn, 0);
		adLen += Extent(ads + adLen, isLong, nd->size - SECTOR, lbn + 1 + gap, 0);
		gNextData += blocks + gap;
	}
	Entry(nd->entry, MetaSector(nd->entry), 5, nd->flags, isLong ? 1 : 0, ads, adLen, nd->size);
}

// an identifier at `p` of a directory whose bytes begin in block `lbn`, `off` bytes before `p`; the bytes it takes
static size_t Ident(uint8_t* p, size_t off, uint32_t lbn, unsigned chars, const char* name, uint32_t entry, int use)
{
	uint8_t raw[0x300];
	size_t nameLen = name ? Name(name, raw) : 0, useLen = use ? 8 : 0;
	size_t len = (38 + useLen + nameLen + 3) & ~(size_t)3;
	memset(p, 0, len);
	PutLe16(p + 16, 1);
	p[18] = (uint8_t)chars;
	p[19] = (uint8_t)nameLen;
	PutLe32(p + 20, SECTOR);
	PutLe32(p + 24, entry);
	PutLe16(p + 28, MetaPart());
	PutLe16(p + 36, (uint32_t)useLen);
	memcpy(p + 38 + useLen, raw, nameLen);
	Tag(p, 257, lbn + (uint32_t)(off / SECTOR), len);
	return len;
}

// a folder's identifiers and its entry
static void WriteDir(int dir)
{
	static uint8_t ids[4 * SECTOR];
	Node_t* nd = &gNodes[dir];
	uint8_t ads[64];
	size_t len = 0;
	uint32_t blocks, lbn, adLen = 0;
	int i, pass, inside = nd->form == FORM_INSIDE;
	memset(ads, 0, sizeof ads);
	lbn = nd->entry;
	for(pass = 0; pass < 2; pass++)
	{ // (twice: the identifiers' tags say the block they are in, known once their size is)
		len = Ident(ids, 0, lbn, 0x0a, NULL, gNodes[nd->parent < 0 ? 0 : nd->parent].entry, 0);
		for(i = 1; i < gNodeCount; i++)
			if(gNodes[i].parent == dir)
			{
				const Node_t* c = &gNodes[i];
				unsigned chars = (c->data ? 0 : 2u) | ((c->flags & NODE_DELETED) ? 4u : 0);
				// (in two runs the second half lies a block further on)
				size_t tagOff = nd->form == FORM_APART && len >= SECTOR ? len + SECTOR : len;
				len += Ident(ids + len, tagOff, lbn, chars, c->name, (c->flags & NODE_LOOP) ? nd->entry : c->entry, c->flags & NODE_USE);
			}
		if(inside || pass)
			break;
		blocks = (uint32_t)((len + SECTOR - 1) / SECTOR) + (nd->form == FORM_APART ? 1 : 0);
		lbn = AllocMeta(blocks);
	}
	if(inside)
	{
		Entry(nd->entry, MetaSector(nd->entry), 4, nd->flags, 3, ids, (uint32_t)len, len);
		return;
	}
	if(nd->form == FORM_APART)
	{
		memcpy(gImg + (size_t)MetaSector(lbn) * SECTOR, ids, SECTOR);
		memcpy(gImg + (size_t)MetaSector(lbn + 2) * SECTOR, ids + SECTOR, len - SECTOR);
		adLen = Extent(ads, 0, SECTOR, lbn, 0);
		adLen += Extent(ads + adLen, 0, (uint32_t)len - SECTOR, lbn + 2, 0);
	}
	else
	{
		for(i = 0; (size_t)i * SECTOR < len; i++)
			memcpy(gImg + (size_t)MetaSector(lbn + (uint32_t)i) * SECTOR, ids + (size_t)i * SECTOR,
				len - (size_t)i * SECTOR < SECTOR ? len - (size_t)i * SECTOR : SECTOR);
		adLen = Extent(ads, 0, (uint32_t)len, lbn, 0);
	}
	Entry(nd->entry, MetaSector(nd->entry), 4, nd->flags, 0, ads, adLen, len);
}

// the identifier of a standard or of a partition kind, in its 32 bytes
static void Ident32(uint8_t* p, const char* name)
{
	memcpy(p + 1, name, strlen(name));
}

static void Both32(uint8_t* p, uint32_t v)
{
	PutLe32(p, v);
	PutBe32(p + 4, v);
}

// a directory record of ISO 9660 at `p`; the bytes it takes
static size_t Record(uint8_t* p, uint32_t lba, uint32_t size, unsigned flags, const uint8_t* name, size_t nameLen)
{
	size_t len = 33 + nameLen + ((nameLen & 1) ? 0 : 1);
	memset(p, 0, len);
	p[0] = (uint8_t)len;
	Both32(p + 2, lba);
	Both32(p + 10, size);
	p[18] = 120, p[19] = 5, p[20] = 17, p[21] = 12, p[22] = 34, p[23] = 56;
	p[25] = (uint8_t)flags;
	p[28] = 1, p[31] = 1;
	p[32] = (uint8_t)nameLen;
	memcpy(p + 33, name, nameLen);
	return len;
}

/* the ISO 9660 part of a bridge disc: a root with the one file `nd` under
 * the short name `plain` and, with `joliet`, a second tree in which it is
 * called "Joliet Name.arc"; the sector after its descriptors */
static uint32_t WriteIsoPart(const Node_t* nd, const char* plain, int joliet)
{
	static const uint8_t kSelf[1] = {0}, kUp[1] = {1};
	static const uint8_t kJoliet[] = "\0J\0o\0l\0i\0e\0t\0 \0N\0a\0m\0e\0.\0a\0r\0c";
	int tree;
	for(tree = 0; tree <= joliet; tree++)
	{
		uint8_t* d = gImg + (size_t)(16 + tree) * SECTOR;
		uint8_t* r = gImg + (size_t)(20 + tree) * SECTOR;
		d[0] = (uint8_t)(tree + 1);
		memcpy(d + 1, "CD001\1", 6);
		Both32(d + 80, IMG_SECTORS);
		if(tree)
			d[88] = 0x25, d[89] = 0x2f, d[90] = 0x45;
		Record(d + 156, 20 + (uint32_t)tree, SECTOR, 2, kSelf, 1);
		r += Record(r, 20 + (uint32_t)tree, SECTOR, 2, kSelf, 1);
		r += Record(r, 20 + (uint32_t)tree, SECTOR, 2, kUp, 1);
		if(tree)
			Record(r, nd->first, nd->size, 0, kJoliet, sizeof kJoliet - 1);
		else
			Record(r, nd->first, nd->size, 0, (const uint8_t*)plain, strlen(plain));
	}
	gImg[(size_t)(16 + tree) * SECTOR] = 255;
	memcpy(gImg + (size_t)(16 + tree) * SECTOR + 1, "CD001\1", 6);
	return 17 + (uint32_t)tree;
}

/* The nodes as an image at `path`, the last `cut` bytes left off.  With
 * `plain` the disc is a bridge disc: its ISO 9660 part lists the node
 * `isoNode` under that short name (and, with `joliet`, under a long one). */
static void WriteUdf(const char* path, size_t cut, int isoNode, const char* plain, int joliet)
{
	uint8_t* d;
	uint32_t seq = 16;
	int i;
	if(!gImg)
		gImg = (uint8_t*)malloc((size_t)IMG_SECTORS * SECTOR);
	memset(gImg, 0, (size_t)IMG_SECTORS * SECTOR);
	gNextMeta = 1; // (block 0 is the file set's)
	gNextData = DATA0;
	for(i = 0; i < gNodeCount; i++)
		if(!(gNodes[i].flags & NODE_LOOP))
			gNodes[i].entry = AllocMeta(1);
	for(i = 0; i < gNodeCount; i++)
		if(gNodes[i].data)
			WriteFile(&gNodes[i]);
	for(i = 0; i < gNodeCount; i++)
		if(!gNodes[i].data && !(gNodes[i].flags & NODE_LOOP))
			WriteDir(i);
	if(plain)
		seq = WriteIsoPart(&gNodes[isoNode], plain, joliet);
	// the standards the disc follows
	memcpy(gImg + (size_t)seq * SECTOR + 1, "BEA01\1", 6);
	memcpy(gImg + (size_t)(seq + 1) * SECTOR + 1, "NSR02\1", 6);
	memcpy(gImg + (size_t)(seq + 2) * SECTOR + 1, "TEA01\1", 6);
	// the anchor, and the sequence it points to: the partition, the logical volume, the end
	d = gImg + (size_t)256 * SECTOR;
	PutLe32(d + 16, 16 * SECTOR);
	PutLe32(d + 20, 257);
	PutLe32(d + 24, 16 * SECTOR);
	PutLe32(d + 28, 257);
	Tag(d, 2, 256, 512);
	d = gImg + (size_t)257 * SECTOR;
	PutLe32(d + 16, 1);
	PutLe16(d + 20, 1);
	PutLe16(d + 22, 7); // the partition's number
	Ident32(d + 24, "+NSR02");
	PutLe32(d + 184, 1);
	PutLe32(d + 188, PART);
	PutLe32(d + 192, IMG_SECTORS - PART);
	Tag(d, 5, 257, 512);
	d = gImg + (size_t)258 * SECTOR;
	PutLe32(d + 16, 2);
	Ident32(d + 20, "OSTA Compressed Unicode");
	d[84] = 8;
	memcpy(d + 85, "TEST", 4);
	d[84 + 127] = 5;
	PutLe32(d + 212, gBlock);
	Ident32(d + 216, "*OSTA UDF Compliant");
	PutLe32(d + 248, SECTOR); // the file set: block 0 of the partition the entries are in
	PutLe16(d + 256, MetaPart());
	{
		uint8_t* m = d + 440;
		uint32_t len;
		if(gMap == 0)
		{
			m[0] = 1, m[1] = 6;
			PutLe16(m + 2, 1);
			PutLe16(m + 4, 7);
		}
		else
		{
			m[0] = 2, m[1] = 64;
			Ident32(m + 4, gMap == 1 ? "*UDF Sparable Partition" : "*UDF Virtual Partition");
			PutLe16(m + 36, 1);
			PutLe16(m + 38, 7);
		}
		len = m[1];
		if(gMeta)
		{ // a second map: the metadata partition, whose file's entry is in block 1 of the first
			uint8_t ads[16];
			m += len;
			m[0] = 2, m[1] = 64;
			Ident32(m + 4, "*UDF Metadata Partition");
			PutLe16(m + 36, 1);
			PutLe16(m + 38, 7);
			PutLe32(m + 40, 1);
			PutLe32(m + 44, 0xffffffffu);
			PutLe32(m + 48, 0xffffffffu);
			PutLe32(m + 52, 32);
			PutLe16(m + 56, 32);
			len += 64;
			Extent(ads, 0, 8 * SECTOR, 2, 0);
			Extent(ads + 8, 0, 24 * SECTOR, 40, 0);
			gMeta = 0; // (the entry itself is in the plain partition)
			Entry(1, PART + 1, 250, 0, 0, ads, 16, 32 * SECTOR);
			gMeta = 1;
		}
		PutLe32(d + 264, len);
		PutLe32(d + 268, gMeta ? 2 : 1);
		Tag(d, 6, 258, 440 + len);
	}
	d = gImg + (size_t)259 * SECTOR;
	Tag(d, 8, 259, 512);
	// the file set, with the root directory's entry
	d = gImg + (size_t)MetaSector(0) * SECTOR;
	PutLe32(d + 400, SECTOR);
	PutLe32(d + 404, gNodes[0].entry);
	PutLe16(d + 408, MetaPart());
	Ident32(d + 416, "*OSTA UDF Compliant");
	Tag(d, 256, 0, 512);
	OS_WriteNativeFile(path, gImg, (uint32_t)((size_t)IMG_SECTORS * SECTOR - cut));
}

// ---- the images ------------------------------------------------------------------------------

static const char kIpl[] = "the boot program";
static const char kSys[] = "an archive of the system's programs";
static const char kText[] = "a text beside the game";
static char gBig[5000];          // longer than two sectors
static char gLong[12][0x100];    // twelve long names
static char gDir[0x400];         // the scratch directory
static char gPath[0x440];        // the disc of the first groups
static int gApart, gHole, gMore; // the nodes that cannot be read

#define JP_NAME     "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0.txt" // ゲーム.txt
#define LATIN_NAME  "caf\xc3\xa9.txt"                          // one byte a character, one of them above 0x7f
#define ASTRAL_NAME "\xf0\x9f\x8e\xae game.txt"                // a character above U+FFFF
#define ASTRAL_KEY  "\xf0\x9f\x8e\xae game.txt"

// a disc as a game's is laid out: the game in a folder beside Autorun.inf
static void GameDisc(void)
{
	int game, sub, many, two, i;
	gNodeCount = 0;
	Add(-1, NULL, NULL, 0, FORM_ONE, 0);
	Add(0, "Autorun.inf", kText, sizeof kText - 1, FORM_ONE, 0);
	game = Add(0, "TheGame", NULL, 0, FORM_ONE, 0);
	Add(game, "system.arc", kSys, sizeof kSys - 1, FORM_ONE, 0);
	Add(game, "ipl._bp", kIpl, sizeof kIpl - 1, FORM_INSIDE, 0);
	Add(game, "data02100.arc", gBig, sizeof gBig, FORM_ONE, NODE_LONG);
	Add(game, "data02150.arc", gBig, sizeof gBig, FORM_SPLIT, NODE_EXT);
	Add(game, JP_NAME, kText, sizeof kText - 1, FORM_ONE, NODE_ATTRS | NODE_USE);
	Add(game, LATIN_NAME, kText, 4, FORM_INSIDE, NODE_EXT | NODE_ATTRS);
	Add(game, ASTRAL_NAME, kText, 5, FORM_ONE, 0);
	Add(game, "TheGame", kText, 16, FORM_ONE, 0); // the disc marker
	Add(game, "gone.bin", kText, 8, FORM_ONE, NODE_DELETED);
	sub = Add(game, "Sub", NULL, 0, FORM_INSIDE, NODE_EXT);
	Add(sub, "empty.bin", "", 0, FORM_ONE, 0);
	Add(sub, "again", NULL, 0, FORM_ONE, NODE_LOOP);
	gApart = Add(sub, "apart.bin", gBig, sizeof gBig, FORM_APART, 0);
	gHole = Add(sub, "hole.bin", gBig, sizeof gBig, FORM_HOLE, 0);
	gMore = Add(sub, "more.bin", gBig, sizeof gBig, FORM_MORE, 0);
	many = Add(game, "Many", NULL, 0, FORM_ONE, 0); // identifiers of more than a block
	for(i = 0; i < 12; i++)
	{
		memset(gLong[i], 'a' + i, 200);
		snprintf(gLong[i] + 200, 0x100 - 200, "-%02d.bin", i);
		Add(many, gLong[i], kText, (uint32_t)i + 1, FORM_INSIDE, 0);
	}
	two = Add(game, "Two", NULL, 0, FORM_APART, 0); // identifiers in two runs
	for(i = 0; i < 12; i++)
		Add(two, gLong[i], kText, (uint32_t)i + 1, FORM_INSIDE, 0);
}

static const VolEntry_t* Find(const Vol_t* z, const char* key)
{
	return z ? Vol_Find(z, key) : NULL;
}

// whether the entry `key` of `z` reads as the `n` bytes at `want`
static int Reads(Vol_t* z, const char* key, const char* want, size_t n)
{
	const VolEntry_t* e = Find(z, key);
	size_t got = 0;
	uint8_t* data = e ? Vol_Load(z, e, &got) : NULL;
	int ok = data && e->readable && e->stored && e->size == n && got == n && memcmp(data, want, n) == 0;
	free(data);
	return ok;
}

// ---- the groups -----------------------------------------------------------------------------

static void TestNames(OpenBGI_t* o)
{
	Vol_t* z = Vol_Open(o, gPath);
	const VolEntry_t* e;
	CHECK(z != NULL);
	if(!z)
		return;
	// the game's folder is the root: Autorun.inf is gone; 11 in it (the deleted one is none), 4 in Sub (the loop is none), 12 in Many and in Two
	CHECK(Vol_Count(z) == 11 + 4 + 12 + 12);
	e = Find(z, "data02100.arc");
	CHECK(e && strcmp(e->path, "data02100.arc") == 0 && e->size == sizeof gBig && !e->isDir && e->readable && e->stored);
	e = Find(z, JP_NAME);
	CHECK(e && strcmp(e->name, JP_NAME) == 0);
	e = Find(z, LATIN_NAME);
	CHECK(e && e->size == 4);
	e = Find(z, ASTRAL_KEY);
	CHECK(e && strcmp(e->name, ASTRAL_NAME) == 0 && e->size == 5);
	e = Find(z, "thegame"); // the marker file, which has the folder's name
	CHECK(e && !e->isDir && e->size == 16);
	e = Find(z, "sub");
	CHECK(e && e->isDir && strcmp(e->name, "Sub") == 0);
	e = Find(z, "sub/empty.bin");
	CHECK(e && strcmp(e->path, "Sub/empty.bin") == 0);
	CHECK(Find(z, "autorun.inf") == NULL && Find(z, "thegame/system.arc") == NULL);
	CHECK(Find(z, "gone.bin") == NULL);
	Vol_Close(z);
}

static void TestRead(OpenBGI_t* o)
{
	Vol_t* z = Vol_Open(o, gPath);
	const VolEntry_t* e;
	char key[0x140];
	size_t n = 0;
	int i;
	CHECK(z != NULL);
	if(!z)
		return;
	CHECK(Reads(z, "system.arc", kSys, sizeof kSys - 1));
	CHECK(Reads(z, "ipl._bp", kIpl, sizeof kIpl - 1));   // in its entry
	CHECK(Reads(z, "data02100.arc", gBig, sizeof gBig)); // long descriptors
	CHECK(Reads(z, "data02150.arc", gBig, sizeof gBig)); // two extents, one after the other; an extended entry
	CHECK(Reads(z, JP_NAME, kText, sizeof kText - 1));   // attributes before the extent
	CHECK(Reads(z, LATIN_NAME, kText, 4));               // .. and before the bytes themselves, in an extended entry
	CHECK(Reads(z, "sub/empty.bin", "", 0));
	e = Find(z, "sub");
	CHECK(e && Vol_Load(z, e, &n) == NULL); // a folder
	for(i = 0; i < 12; i++)
	{
		snprintf(key, sizeof key, "many/%s", gLong[i]);
		CHECK(Reads(z, key, kText, (size_t)i + 1));
		snprintf(key, sizeof key, "two/%s", gLong[i]);
		CHECK(Reads(z, key, kText, (size_t)i + 1));
	}
	e = Find(z, "ipl._bp");
	CHECK(e && e->mtime == UINT64_C(132341924960000000)); // 2020-05-17 12:34:56 UTC as a FILETIME
	CHECK(e && e->dosTime == (((40u << 9) | (5u << 5) | 17u) << 16 | ((12u << 11) | (34u << 5) | 28u)));
	e = Find(z, LATIN_NAME); // an extended entry's time is elsewhere
	CHECK(e && e->mtime == UINT64_C(132341924960000000));
	Vol_Close(z);
	{ // recorded nine hours east of UTC (Japan), and five hours west of it
		char path[0x460];
		snprintf(path, sizeof path, "%s/zone.iso", gDir);
		gZone = 540;
		WriteUdf(path, 0, 0, NULL, 0);
		z = Vol_Open(o, path);
		e = Find(z, "system.arc");
		CHECK(e && e->mtime == UINT64_C(132341924960000000) - UINT64_C(9) * 3600 * 10000000);
		Vol_Close(z);
		gZone = -300;
		WriteUdf(path, 0, 0, NULL, 0);
		z = Vol_Open(o, path);
		e = Find(z, "system.arc");
		CHECK(e && e->mtime == UINT64_C(132341924960000000) + UINT64_C(5) * 3600 * 10000000);
		Vol_Close(z);
		gZone = 0;
	}
}

static void TestRefused(OpenBGI_t* o)
{
	static const char* const kKeys[] = {"sub/apart.bin", "sub/hole.bin", "sub/more.bin"};
	Vol_t* z = Vol_Open(o, gPath);
	const VolEntry_t* e;
	char path[0x460];
	size_t n = 0, i;
	CHECK(z != NULL);
	if(!z)
		return;
	for(i = 0; i < 3; i++)
	{
		e = Find(z, kKeys[i]);
		CHECK(e && !e->isDir && e->size == sizeof gBig && !e->readable && Vol_Load(z, e, &n) == NULL);
	}
	CHECK(Find(z, "sub/again") == NULL && Find(z, "sub/again/empty.bin") == NULL); // the folder inside itself
	Vol_Close(z);
	// the image ends inside the last file recorded (more.bin; the two before it are whole)
	snprintf(path, sizeof path, "%s/cut.iso", gDir);
	WriteUdf(path, (size_t)(IMG_SECTORS - (gNodes[gMore].first + 1)) * SECTOR, 0, NULL, 0);
	z = Vol_Open(o, path);
	CHECK(z != NULL);
	e = Find(z, "data02150.arc");
	CHECK(e && e->readable);
	gNodes[gMore].form = FORM_ONE; // (one extent this time, so that only the cut is in its way)
	WriteUdf(path, (size_t)(IMG_SECTORS - (gNodes[gMore].first + 1)) * SECTOR, 0, NULL, 0);
	gNodes[gMore].form = FORM_MORE;
	Vol_Close(z);
	z = Vol_Open(o, path);
	e = Find(z, "sub/more.bin");
	CHECK(e && e->size == sizeof gBig && !e->readable);
	CHECK(Reads(z, "data02150.arc", gBig, sizeof gBig));
	Vol_Close(z);
}

static void TestMetadata(OpenBGI_t* o)
{
	Vol_t* z;
	const VolEntry_t* e;
	char path[0x460], key[0x140];
	int i;
	// (a smaller disc: the metadata partition written here has 32 blocks)
	gNodeCount = 0;
	Add(-1, NULL, NULL, 0, FORM_ONE, 0);
	Add(0, "Autorun.inf", kText, sizeof kText - 1, FORM_ONE, 0);
	i = Add(0, "TheGame", NULL, 0, FORM_ONE, 0);
	Add(i, "system.arc", kSys, sizeof kSys - 1, FORM_ONE, 0);
	Add(i, "ipl._bp", kIpl, sizeof kIpl - 1, FORM_INSIDE, NODE_EXT);
	Add(i, "data02100.arc", gBig, sizeof gBig, FORM_ONE, NODE_EXT);
	Add(i, "data02150.arc", gBig, sizeof gBig, FORM_SPLIT, 0);
	Add(i, JP_NAME, kText, sizeof kText - 1, FORM_ONE, 0);
	i = Add(i, "Many", NULL, 0, FORM_ONE, NODE_EXT);
	Add(i, gLong[0], kText, 1, FORM_INSIDE, 0);
	Add(i, gLong[1], kText, 2, FORM_ONE, 0);
	for(i = 2; i < 12; i++)
		Add(8, gLong[i], kText, (uint32_t)i + 1, FORM_INSIDE, 0);
	snprintf(path, sizeof path, "%s/meta.iso", gDir);
	gMeta = 1;
	WriteUdf(path, 0, 0, NULL, 0);
	gMeta = 0;
	CHECK(gNextMeta > 8 && gNextMeta <= 32); // entries in both runs of the metadata file
	z = Vol_Open(o, path);
	CHECK(z != NULL);
	if(!z)
		return;
	CHECK(Vol_Count(z) == 6 + 12);
	CHECK(Reads(z, "system.arc", kSys, sizeof kSys - 1));
	CHECK(Reads(z, "ipl._bp", kIpl, sizeof kIpl - 1));
	CHECK(Reads(z, "data02100.arc", gBig, sizeof gBig));
	CHECK(Reads(z, "data02150.arc", gBig, sizeof gBig));
	CHECK(Reads(z, JP_NAME, kText, sizeof kText - 1));
	for(i = 0; i < 12; i++)
	{
		snprintf(key, sizeof key, "many/%s", gLong[i]);
		CHECK(Reads(z, key, kText, (size_t)i + 1));
	}
	e = Find(z, "many");
	CHECK(e && e->isDir);
	Vol_Close(z);
}

static void TestBridge(OpenBGI_t* o)
{
	Vol_t* z;
	char path[0x460];
	int file;
	gNodeCount = 0;
	Add(-1, NULL, NULL, 0, FORM_ONE, 0);
	Add(0, "system.arc", kSys, sizeof kSys - 1, FORM_ONE, 0);
	file = Add(0, "data02100.arc", gBig, sizeof gBig, FORM_ONE, 0);
	snprintf(path, sizeof path, "%s/bridge.iso", gDir);

	WriteUdf(path, 0, file, "DATA0210.ARC;1", 0); // short names and UDF: the names as they are
	z = Vol_Open(o, path);
	CHECK(z && Vol_Count(z) == 2 && Reads(z, "data02100.arc", gBig, sizeof gBig) && Reads(z, "system.arc", kSys, sizeof kSys - 1));
	CHECK(Find(z, "data0210.arc") == NULL);
	Vol_Close(z);

	WriteUdf(path, 0, file, "DATA0210.ARC;1", 1); // Joliet as well: its names
	z = Vol_Open(o, path);
	CHECK(z && Vol_Count(z) == 1 && Reads(z, "joliet name.arc", gBig, sizeof gBig));
	Vol_Close(z);

	gBlock = 512; // blocks of another size: not followed, the short names are left
	WriteUdf(path, 0, file, "DATA0210.ARC;1", 0);
	gBlock = SECTOR;
	z = Vol_Open(o, path);
	CHECK(z && Vol_Count(z) == 1 && Reads(z, "data0210.arc", gBig, sizeof gBig));
	Vol_Close(z);

	gMap = 2; // a virtual partition: the same
	WriteUdf(path, 0, file, "DATA0210.ARC;1", 0);
	z = Vol_Open(o, path);
	CHECK(z && Vol_Count(z) == 1 && Reads(z, "data0210.arc", gBig, sizeof gBig));
	Vol_Close(z);
	WriteUdf(path, 0, 0, NULL, 0); // .. and without ISO 9660 there is no image
	CHECK(Vol_Open(o, path) == NULL);

	gMap = 1; // a sparable partition: a plain one
	WriteUdf(path, 0, 0, NULL, 0);
	gMap = 0;
	z = Vol_Open(o, path);
	CHECK(z && Vol_Count(z) == 2 && Reads(z, "data02100.arc", gBig, sizeof gBig));
	Vol_Close(z);
}

int main(void)
{
	size_t i;
	OpenBGI_t* o = OpenBGI_New(OS_Host()); // the engine whose code the test runs
	if(!o)
		return 1;
	OS_Init();
	for(i = 0; i < sizeof gBig; i++)
		gBig[i] = (char)('a' + (i * 7 + i / 1000) % 26);
	TestHost_ScratchName(gDir, sizeof gDir, "openbgi_udf");
	TestHost_Mkdir(gDir);
	snprintf(gPath, sizeof gPath, "%s/Disc One.ISO", gDir);
	gBlock = SECTOR;
	GameDisc();
	WriteUdf(gPath, 0, 0, NULL, 0);

	printf("names\n");
	TestNames(o);
	printf("read\n");
	TestRead(o);
	printf("refused\n");
	TestRefused(o);
	printf("metadata\n");
	TestMetadata(o);
	printf("bridge\n");
	TestBridge(o);

	if(!getenv("OPENBGI_UDF_KEEP"))
		TestHost_RemoveTree(gDir);
	free(gImg);
	if(gFails)
	{
		printf("udf: %d checks failed\n", gFails);
		return 1;
	}
	printf("udf: all checks passed\n");
	return 0;
}
