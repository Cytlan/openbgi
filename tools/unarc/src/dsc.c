#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Cleaned-up and refactored version of the decompression routine.
 * Original was a Ghidra decompile of what appears to be an LZ77 + Huffman
 * (or prefix-code) decompressor. The 512-byte table at offset 0x20 encodes
 * Huffman code lengths for 512 possible symbols (0-255 = literals,
 * 256-511 = match codes with length = (sym & 0xff) + 2).
 *
 * The bitstream starts at offset 0x220. Matches use a 12-bit distance
 * and are copied with a -2 bias in the lookback (as in the original).
 */

typedef struct {
    int32_t type;      /* 0 = leaf, 1 = internal */
    int32_t symbol;    /* symbol id for leaves (0-511) */
    int32_t child[2];  /* child node indices for internal nodes */
} decode_node_t;

static int compare_packed(const void *a, const void *b)
{
    uint32_t va = *(const uint32_t *)a;
    uint32_t vb = *(const uint32_t *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

/* Build the decode tree from the length table at file_buf+0x20 (decrypted with
 * the stream cipher seeded at file_buf+0x10). Uses the classic level-by-level
 * construction from code lengths (stable sort by length then symbol id).
 */
static void build_decode_tree(decode_node_t *tree, const uint8_t *file_buf)
{
    uint32_t key = *(const uint32_t *)(file_buf + 0x10);

    uint32_t packed[513];
    int num_sym = 0;

    for (int i = 0; i < 0x200; i++) {
        uint32_t u1 = (key & 0xffff) * 0x4e35u;
        uint32_t u2 = (key >> 16) * 0x4e35u + key * 0x15au + (u1 >> 16);
        key = (u2 << 16) + 1u + (u1 & 0xffff);
        uint8_t r = (uint8_t)(u2 & 0x7fff);
        uint8_t val = file_buf[0x20 + i] - r;
        if (val != 0) {
            packed[num_sym++] = ((uint32_t)val << 16) | (uint32_t)i;
        }
    }

    if (num_sym > 1) {
        qsort(packed, (size_t)num_sym, sizeof(uint32_t), compare_packed);
    }

    memset(tree, 0, sizeof(decode_node_t) * 1024);

    int node_list[1024];
    node_list[0] = 0;

    int curr_list_base = 0;
    int curr_list_len = 1;
    int next_node_id = 1;
    int sym_idx = 0;
    int curr_len = 0;

    int *curr_ptr = node_list + curr_list_base;

    while (sym_idx < num_sym) {
        int next_base = (curr_list_base == 0 ? 512 : 0);
        int *next_ptr = node_list + next_base;

        int leaves_here = 0;

        while (sym_idx < num_sym && curr_len == (int)(packed[sym_idx] >> 16)) {
            int nid = *curr_ptr++;
            tree[nid].type = 0;
            tree[nid].symbol = (int32_t)(packed[sym_idx] & 0x1ff);
            sym_idx++;
            leaves_here++;
        }

        int remaining = curr_list_len - leaves_here;

        if (leaves_here < curr_list_len) {
            for (int j = 0; j < remaining; j++) {
                int nid = *curr_ptr++;
                tree[nid].type = 1;
                for (int b = 0; b < 2; b++) {
                    int cid = next_node_id++;
                    tree[nid].child[b] = cid;
                    *next_ptr++ = cid;
                }
            }
        }

        curr_list_base = next_base;
        curr_list_len = remaining * 2;
        curr_ptr = node_list + curr_list_base;
        curr_len++;
    }
}

/* Core bitstream decompressor (LZ + prefix code).
 * Reads Huffman-coded symbols until the count at file_buf+0x18 is reached.
 * Literals are output directly. Matches (symbol >= 256) read a 12-bit distance
 * and copy (length+2) bytes from (current_pos - 2 - distance).
 */
static size_t decompress_lz_huff(uint8_t *out, const uint8_t *in, const decode_node_t *tree)
{
    const uint8_t *src = in + 0x220;
    size_t out_pos = 0;
    int symbols_left = *(const int32_t *)(in + 0x18);

    if (symbols_left <= 0)
        return 0;

    int bit_count = 0;
    uint8_t bit_buf = 0;

    while (symbols_left-- > 0) {
        int node_id = 0;

        while (tree[node_id].type != 0) {  /* walk internal nodes */
            if (bit_count == 0) {
                bit_buf = *src++;
                bit_count = 8;
            }
            int bit = (bit_buf >> 7) & 1;
            bit_buf <<= 1;
            bit_count--;
            node_id = tree[node_id].child[bit];
        }

        uint16_t sym = (uint16_t)tree[node_id].symbol;

        if ((sym >> 8) == 1) {
            /* match */
            int len = (sym & 0xff) + 2;

            /* --- read 12-bit distance (careful bit-buffer management) --- */
            int bc = bit_count;
            uint32_t u6 = 0;

            if (bc > 0) {
                u6 = (uint32_t)(bit_buf >> (8 - bc));
            }

            if (bc < 12) {
                int extra = (19 - bc) >> 3;
                bc += extra * 8;
                for (int k = 0; k < extra; k++) {
                    u6 = (u6 << 8) | (uint32_t)(*src++);
                }
            }

            bc -= 12;

            bit_count = bc;
            if (bc > 0) {
                uint32_t mask = (1u << bc) - 1u;
                bit_buf = (uint8_t)((u6 & mask) << (8 - bc));
            } else {
                bit_buf = 0;
            }

            uint32_t dist = (u6 >> bc) & 0xffffu;

            uint8_t *copy_from = out + out_pos - 2 - dist;
            for (int k = 0; k < len; k++) {
                out[out_pos++] = *copy_from++;
            }
        } else {
            /* literal */
            out[out_pos++] = (uint8_t)sym;
        }
    }

    return out_pos;
}

/* Public entry point matching the original FUN_00426540 signature and behaviour.
 * The caller is responsible for allocating out_buf to the size stored at
 * file_buf + 0x14.  No critical sections are needed because we keep the
 * PRNG state local to each call.
 */
void decompressDSC(uint8_t *out_buf, const uint8_t *file_buf)
{
    decode_node_t tree[1024];
    build_decode_tree(tree, file_buf);
    decompress_lz_huff(out_buf, file_buf, tree);
}
