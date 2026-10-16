/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dsc.c - "DSC FORMAT 1.00" decompressor; interface in codec.h
 *
 * The general-purpose compression of the game files: programs, scenario
 * files, images and sounds may all be stored this way, and LoadFile removes
 * the layer transparently.  Decoding runs in three steps: the code lengths
 * are deobfuscated with the hash generator seeded from the header, the
 * canonical Huffman tree is laid out from them (DscBuildTree), and decCount
 * symbols are read from the bit stream (DscDecodeStream).
 *
 * File layout (see DscHeader in codec.h): a 0x20-byte header, 512 code-length
 * bytes (one per symbol, obfuscated with the hash generator seeded by
 * header.key), then the MSB-first bit stream.
 *
 * Symbols 0x000..0x0FF are literal bytes.  Symbols 0x100..0x1FF are back
 * references: the low byte + 2 is the copy length and 12 further bits give the
 * distance; the copy starts at  out - distance - 2.
 */
#include "bgi/codec.h"
#include "bgi/sys.h"

int BGI_IsDsc(const void* data)
{
	return memcmp(data, DSC_MAGIC, 16) == 0;
}

/* One node of the decoding tree, 16 bytes; the tree is an array of
 * DSC_MAX_NODES of them with the root at index 0. */
typedef struct DscNode
{
	uint32_t isNode;   // 0 = leaf, 1 = internal node
	uint32_t value;    // leaf: symbol (low 9 bits of the entry)
	uint32_t child[2]; // internal: node indices for bit 0 / 1
} DscNode_t;

#define DSC_MAX_NODES 0x3ff // enough for 512 leaves

/*
 * Build the tree from the 512 obfuscated code lengths behind the header:
 * collect the non-zero code lengths as (length << 16 | symbol), sort them
 * ascending and lay the canonical tree out level by level.  Two 0x200-entry
 * work buffers alternate per level; the node count for the next level is
 * twice the number of internal nodes that were left over on the current
 * level.  Takes the engine lock while the global hash generator is in use.
 */
static void DscBuildTree(DscNode_t* nodes, const uint8_t* src)
{
	uint32_t list[513];
	uint32_t level[2][0x200];
	uint32_t count = 0, i, j;

	EngineLock_Enter(); // the hash state is a global
	BGI_HashSeed(((const DscHeader_t*)src)->key);
	for(i = 0; i < 512; i++)
	{
		uint8_t len = (uint8_t)(src[0x20 + i] - (uint8_t)BGI_HashNext());
		if(len != 0)
			list[count++] = ((uint32_t)len << 16) | i;
	}
	list[count] = 0;

	// bubble sort, ascending by (length, symbol), as in the original
	for(i = 1; i < count; i++)
		for(j = i; j < count; j++)
			if(list[i - 1] > list[j])
			{
				uint32_t t = list[i - 1];
				list[i - 1] = list[j];
				list[j] = t;
			}

	{
		uint32_t depth = 0;          // code length of the current level
		uint32_t nodesThisLevel = 1; // slots available on this level
		uint32_t nextFree = 1;       // next node index to hand out
		uint32_t li = 0;             // index into list
		int cur = 0;                 // which of the two level buffers

		level[0][0] = 0; // the root occupies slot 0
		while(li < count)
		{
			uint32_t* curBuf = level[cur];
			uint32_t* nxtBuf = level[cur ^ 1];
			uint32_t used = 0, k = 0, remaining;

			// leaves whose code length equals the current depth
			while(li < count && (list[li] >> 16) == depth)
			{
				DscNode_t* n = &nodes[curBuf[k++]];
				n->isNode = 0;
				n->value = list[li] & 0x1ff;
				li++;
				used++;
			}
			// every slot left over becomes an internal node with two children
			remaining = nodesThisLevel - used;
			if(used < nodesThisLevel)
			{
				uint32_t r;
				for(r = 0; r < remaining; r++)
				{
					DscNode_t* n = &nodes[curBuf[k++]];
					n->isNode = 1;
					n->child[0] = nextFree;
					n->child[1] = nextFree + 1;
					*nxtBuf++ = nextFree;
					*nxtBuf++ = nextFree + 1;
					nextFree += 2;
				}
			}
			nodesThisLevel = remaining * 2;
			depth++;
			cur ^= 1;
		}
	}
	EngineLock_Leave();
}

/*
 * Decode the decCount symbols of the bit stream (which starts at 0x220 in the
 * file) into dst, walking the tree bit by bit for each one and expanding back
 * references.  Returns the number of bytes written.
 */
static uint32_t DscDecodeStream(uint8_t* dst, const uint8_t* src, const DscNode_t* nodes)
{
	const DscHeader_t* h = (const DscHeader_t*)src;
	const uint8_t* in = src + 0x220;
	uint8_t* out = dst;
	uint32_t bits = 0; // current byte, consumed MSB first
	int nbits = 0;
	uint32_t n;

	for(n = 0; n < h->decCount; n++)
	{
		uint32_t node = 0, code;
		do
		{
			uint32_t bit;
			if(nbits == 0)
			{
				bits = *in++;
				nbits = 8;
			}
			bit = (bits >> 7) & 1;
			bits = (bits << 1) & 0xff;
			nbits--;
			node = nodes[node].child[bit];
		} while(nodes[node].isNode != 0);

		code = nodes[node].value & 0xffff;
		if((code >> 8) == 1)
		{
			// back reference: gather at least 12 bits for the distance
			uint32_t v = nbits ? (bits >> (8 - nbits)) : 0;
			int have = nbits;
			uint32_t offset, count, k;
			const uint8_t* from;
			if(nbits < 12)
			{
				int fetch = ((11 - nbits) >> 3) + 1;
				have = nbits + fetch * 8;
				while(fetch--)
					v = (v << 8) | *in++;
			}
			{
				int extra = have - 12; // bits left over
				offset = (v >> extra) & 0xffff;
				bits = (v << (8 - extra)) & 0xff; // leftovers back to MSB
				nbits = extra;
			}
			count = (code & 0xff) + 2;
			from = out - offset - 2;
			for(k = 0; k < count; k++) // byte-wise: source and destination may overlap
				*out++ = from[k];
		}
		else
		{
			*out++ = (uint8_t)code;
		}
	}
	return (uint32_t)(out - dst);
}

/* decompress the DSC file at src into dst (BGI_DscOutSize(src) bytes);
 * returns the number of bytes written, which is outSize for a well-formed
 * stream */
uint32_t BGI_DscDecode(const void* src, void* dst)
{
	DscNode_t* nodes = (DscNode_t*)BGI_Calloc(sizeof(DscNode_t) * DSC_MAX_NODES);
	uint32_t n;
	DscBuildTree(nodes, (const uint8_t*)src);
	n = DscDecodeStream((uint8_t*)dst, (const uint8_t*)src, nodes);
	BGI_Free(nodes);
	return n;
}
