/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * codec.h - the data formats of the engine's files
 *
 * Declares the interface of hash.c, dsc.c, cbg.c and sdc.c, and of the
 * encoders the archive tools add (dscenc.c, cbgenc.c):
 *
 *  * the "hash" pseudo-random generator that keys every obfuscated format
 *  * DSC FORMAT 1.00   generic Huffman + LZ compression (scripts, images, ...)
 *  * CompressedBG___   image compression (Huffman + zero RLE + delta filter)
 *  * SDC / DCFS        the save-data codecs (sdc.c)
 *
 * The decoders take a pointer to the whole file in memory and write into a
 * buffer the caller has sized with the matching *OutSize function.  The
 * struct offsets in the comments are byte offsets in the file.
 */
#ifndef BGI_CODEC_H_
#define BGI_CODEC_H_

#include "bgi/common.h"

// ---- pseudo-random generator -------------------------------------------

/* One step of the generator on a caller-owned state: advances *key and
 * returns the next 15-bit value.  Every obfuscated file is "decrypted" by
 * subtracting successive outputs from its bytes. */
uint32_t BGI_HashUpdate(uint32_t* key);

/* The same generator on a global state, used by DSC, the save scrambler and
 * the SDC/DCFS encoders.  Callers hold the engine lock around a seed-and-step
 * sequence because the loader thread decodes DSC data too. */
void BGI_HashSeed(uint32_t key);
uint32_t BGI_HashNext(void);

// ---- DSC FORMAT 1.00 ---------------------------------------------------

#define DSC_MAGIC "DSC FORMAT 1.00" // 16 bytes incl. NUL

typedef struct DscHeader // file layout, 0x20 bytes
{
	char magic[16];
	uint32_t key;      // +0x10 seed of the generator for the code lengths
	uint32_t outSize;  // +0x14 size of the decompressed data, bytes
	uint32_t decCount; // +0x18 number of Huffman symbols in the stream
	uint32_t reserved; // +0x1c not read
					   // +0x20: uint8_t codeLengths[512], then the bit stream at +0x220
} DscHeader_t;

// 1 when `data` starts with the DSC magic (16 bytes are compared)
int BGI_IsDsc(const void* data);
/* decompress a DSC stream into dst; dst must hold BGI_DscOutSize(src) bytes.
 * Returns the number of bytes written (outSize). */
uint32_t BGI_DscDecode(const void* src, void* dst);
// the decompressed size announced by the header
static inline uint32_t BGI_DscOutSize(const void* src)
{
	return ((const DscHeader_t*)src)->outSize;
}
/* compress `n` bytes into a DSC file (dscenc.c; BGI_Free the result), `*size`
 * bytes.  `key` seeds the generator that obfuscates the code lengths; any
 * value will do.  The engine has no such function: the archive tools use it
 * to store what they rebuild the way the games store it. */
uint8_t* BGI_DscEncode(const void* src, uint32_t n, uint32_t key, uint32_t* size);

// ---- CompressedBG___ ---------------------------------------------------

#define CBG_MAGIC "CompressedBG___"

typedef struct CbgHeader // file layout, 0x30 bytes
{
	char magic[16];
	uint16_t width;    // +0x10 pixels
	uint16_t height;   // +0x12 pixels
	uint32_t bpp;      // +0x14 bits per pixel: 8, 24 or 32
	uint32_t unused18; // +0x18 } copied verbatim into the
	uint32_t unused1c; // +0x1c } decoded image header
	uint32_t interLen; /* +0x20 size of the Huffman-decoded data
	                    *       (zero-RLE compressed pixels), bytes */
	uint32_t key;      // +0x24 seed of the generator for the weight table
	uint32_t encLen;   // +0x28 length of the encrypted weight table, bytes
	uint8_t sum;       // +0x2c check: byte sum of the decrypted weight table
	uint8_t xor ;      // +0x2d check: byte xor of the decrypted weight table
	uint16_t version;  // +0x2e 1 (lossless, cbg.c) or 2 (DCT, cbg2.c)
					   // +0x30: encLen encrypted bytes, then the Huffman bit stream
} CbgHeader_t;

/* Layout of a decoded image in memory: what LoadFile leaves in the buffer
 * for a CompressedBG or an uncompressed image file, and what the bitmap
 * manager consumes. */
typedef struct RawImageHeader // 0x10 bytes
{
	uint16_t width;    // pixels
	uint16_t height;   // pixels
	uint32_t bpp;      // bits per pixel: 8, 24 or 32
	uint32_t unused08; // the two words after bpp of the CompressedBG header
	uint32_t unused0c;
	// pixel rows follow, width*height*(bpp/8) bytes, no row padding
} RawImageHeader_t;

// 1 when `data` starts with the CompressedBG magic (16 bytes are compared)
int BGI_IsCbg(const void* data);

// ---- shared between cbg.c (version 1) and cbg2.c (version 2) ----

typedef struct CbgNode // a Huffman tree node
{
	uint32_t valid;    // still available for pairing
	uint32_t weight;   // sum of the leaf weights below
	uint32_t isParent; // 0 for the leaves, 1 otherwise
	uint32_t parent;   // 0xffffffff until the node has been paired
	uint32_t left;     // child for bit 0 (leaf: own index)
	uint32_t right;    // child for bit 1 (leaf: own index)
} CbgNode_t;

#define CBG_NODES 511 // version 1: 256 leaves + 255 parents

void CbgDecrypt(uint8_t* buf, uint32_t len, uint32_t key);                    // buf[i] -= the hash generator's bytes
int CbgCheck(const uint8_t* buf, uint32_t len, uint8_t sum, uint8_t xr);      // 1 when the sum / xor checks hold
int CbgBuildTree(CbgNode_t* nodes, const uint32_t* weights, uint32_t leaves); // 2 * leaves - 1 nodes; the root, -1 when all weights are 0
uint32_t Cbg2_Decode(const void* src, void* dst);                             // cbg2.c: the version 2 decoder (BGI_CbgDecode's codes)
/* decode into dst (RawImageHeader + pixels); dst must hold
 * BGI_CbgOutSize(src) bytes.  Returns 0 on success, 0x80000003 if the magic
 * is wrong, 0x80000004 when the checksum of the weight table fails. */
uint32_t BGI_CbgDecode(const void* src, void* dst);
// the size of the decoded image: header plus tightly packed pixels
static inline uint32_t BGI_CbgOutSize(const void* src)
{
	const CbgHeader_t* h = (const CbgHeader_t*)src;
	return 0x10u + (uint32_t)h->width * h->height * ((h->bpp & 0xffffu) >> 3);
}
/* encode a decoded image (RawImageHeader + pixels) as a version 1
 * CompressedBG file (cbgenc.c; BGI_Free the result), `*size` bytes: the
 * lossless codec, which every build of the engine reads.  `key` seeds the
 * generator that encrypts the weight table.  For the archive tools. */
uint8_t* BGI_CbgEncode(const void* image, uint32_t key, uint32_t* size);

// ---- SDC FORMAT 1.00 / DCFS FORMAT 1.00 (sdc.c) ------------------------

#define SDC_MAGIC  "SDC FORMAT 1.00"
#define DCFS_MAGIC "DCFS FORMAT 1.00"

typedef struct SdcHeader // file layout, 0x20 bytes
{
	char magic[16];
	uint32_t key;     // +0x10 generator seed: the millisecond field of the time of encoding
	uint32_t encLen;  // +0x14 payload bytes following the header
	uint32_t outSize; // +0x18 decoded size, bytes
	uint16_t sum;     // +0x1c 16-bit sum of the scrambled payload bytes
	uint16_t xor ;    // +0x1e 16-bit xor of the scrambled payload bytes
} SdcHeader_t;

typedef struct DcfsHeader // file layout, 0x18 bytes
{
	char magic[16];
	uint32_t frameSize;  // +0x10 bytes per frame
	uint32_t frameCount; // +0x14 frames, the first one included
						 // +0x18: the first frame raw, then the difference-coded frames
} DcfsHeader_t;

// 1 when `data` starts with the SDC magic (16 bytes are compared)
int SdcIsFormat(const void* data);
/* decode an SDC file into dst (SdcOutSize(src) bytes); returns the decoded
 * size, 0 when the magic or the checks fail */
uint32_t SdcDecode(void* dst, const void* src);
/* encode `size` bytes of src into dst as header + payload; dst must have
 * room for the header and the LZ output, which can be larger than the
 * input: a literal run costs a byte more than it holds, and a two-byte
 * match is coded as a two-byte copy that splits the literals around it, so
 * a lone literal and such a copy in turn take four bytes for three -
 * size + size / 3 + 2 bytes at the worst.  Returns the total number of
 * bytes written, 0 when the verification decode differs from the input */
uint32_t SdcEncode(void* dst, const void* src, uint32_t size);
// the decoded size announced by the header
static inline uint32_t SdcOutSize(const void* src)
{
	return ((const SdcHeader_t*)src)->outSize;
}

/* expand a DCFS file into dst (frameSize * frameCount bytes); 0 ok,
 * 0x80000003 bad magic, 0x80000004 a zero frame size or count, or a frame
 * whose runs do not add up to frameSize */
uint32_t DcfsDecode(void* dst, const void* src);
/* encode frameCount frames of frameSize bytes from src into dst; *outSize
 * receives the encoded length.  0 ok, 0xffffffff when the verification
 * decode differs, 0x80000001 for a zero frame count, 0x80000002 for a zero
 * frame size */
uint32_t DcfsEncode(void* dst, uint32_t* outSize, const void* src,
	uint32_t frameSize, uint32_t frameCount);

#endif // BGI_CODEC_H_
