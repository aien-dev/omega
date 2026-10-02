/* omega_blake3.c -- see omega_blake3.h. Follows the BLAKE3 reference construction:
 * 1024-byte chunks of 64-byte blocks, a binary tree of parent nodes, the ROOT flag on the
 * last compression only. Mutation tags (B3M:<tag>) mark lines make test-resolve breaks. */
#include "omega_blake3.h"

#include <string.h>

#define CHUNK_START 1u
#define CHUNK_END   2u
#define PARENT      4u
#define ROOT        8u
#define CHUNK_LEN   1024u
#define BLOCK_LEN   64u

static const uint32_t IV[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u
};
static const uint8_t PERM[16] = { 2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8 };

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void g(uint32_t *s, int a, int b, int c, int d, uint32_t mx, uint32_t my)
{
    s[a] = s[a] + s[b] + mx;
    s[d] = rotr(s[d] ^ s[a], 16);
    s[c] = s[c] + s[d];
    s[b] = rotr(s[b] ^ s[c], 12);
    s[a] = s[a] + s[b] + my;
    s[d] = rotr(s[d] ^ s[a], 8);
    s[c] = s[c] + s[d];
    s[b] = rotr(s[b] ^ s[c], 7);
}

static void round_fn(uint32_t *s, const uint32_t *m)
{
    g(s, 0, 4, 8, 12, m[0], m[1]);
    g(s, 1, 5, 9, 13, m[2], m[3]);
    g(s, 2, 6, 10, 14, m[4], m[5]);
    g(s, 3, 7, 11, 15, m[6], m[7]);
    g(s, 0, 5, 10, 15, m[8], m[9]);
    g(s, 1, 6, 11, 12, m[10], m[11]);
    g(s, 2, 7, 8, 13, m[12], m[13]);
    g(s, 3, 4, 9, 14, m[14], m[15]);
}

/* out: the 16-word state after the final xor; words 0..7 are the chaining value */
static void compress(const uint32_t cv[8], const uint8_t block[BLOCK_LEN], uint32_t block_len,
                     uint64_t counter, uint32_t flags, uint32_t out[16])
{
    uint32_t m[16], t[16];
    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)block[4 * i] | (uint32_t)block[4 * i + 1] << 8 |
               (uint32_t)block[4 * i + 2] << 16 | (uint32_t)block[4 * i + 3] << 24;
    uint32_t s[16] = {
        cv[0], cv[1], cv[2], cv[3], cv[4], cv[5], cv[6], cv[7],
        IV[0], IV[1], IV[2], IV[3],
        (uint32_t)counter, (uint32_t)(counter >> 32), block_len, flags
    };
    for (int r = 0; r < 7; r++) {
        round_fn(s, m);
        for (int i = 0; i < 16; i++) t[i] = m[PERM[i]];
        memcpy(m, t, sizeof m);
    }
    for (int i = 0; i < 8; i++) {
        out[i] = s[i] ^ s[i + 8];
        out[i + 8] = s[i + 8] ^ cv[i];
    }
}

/* The inputs of the last compression of a chunk or a parent: kept unfinished so the ROOT flag
 * can still be added to whichever compression turns out to be the last. */
typedef struct {
    uint32_t cv[8];
    uint8_t block[BLOCK_LEN];
    uint32_t block_len;
    uint64_t counter;
    uint32_t flags;
} Node;

static void node_cv(const Node *n, uint32_t out[8])
{
    uint32_t st[16];
    compress(n->cv, n->block, n->block_len, n->counter, n->flags, st);
    memcpy(out, st, 8 * sizeof(uint32_t));
}

/* the (unfinished) last compression of chunk number `counter` holding data[0..len) */
static Node chunk_node(const uint8_t *data, size_t len, uint64_t counter)
{
    Node n;
    memcpy(n.cv, IV, sizeof n.cv);
    size_t nblocks = len ? (len + BLOCK_LEN - 1) / BLOCK_LEN : 1;
    for (size_t b = 0; b + 1 < nblocks; b++) {
        uint32_t fl = (b == 0) ? CHUNK_START : 0;
        uint32_t st[16];
        compress(n.cv, data + b * BLOCK_LEN, BLOCK_LEN, counter, fl, st);
        memcpy(n.cv, st, 8 * sizeof(uint32_t));
    }
    size_t off = (nblocks - 1) * BLOCK_LEN, rem = len - off;
    memset(n.block, 0, sizeof n.block);
    if (rem) memcpy(n.block, data + off, rem);
    n.block_len = (uint32_t)rem;
    n.counter = counter;
    n.flags = CHUNK_END | (nblocks == 1 ? CHUNK_START : 0);
    return n;
}

static Node parent_node(const uint32_t left[8], const uint32_t right[8])
{
    Node n;
    memcpy(n.cv, IV, sizeof n.cv);
    for (int i = 0; i < 8; i++) {
        for (int k = 0; k < 4; k++) {
            n.block[4 * i + k] = (uint8_t)(left[i] >> (8 * k));
            n.block[32 + 4 * i + k] = (uint8_t)(right[i] >> (8 * k));
        }
    }
    n.block_len = BLOCK_LEN;
    n.counter = 0;
    n.flags = PARENT;
    return n;
}

void omega_blake3_hash(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint32_t stack[64][8];
    int sp = 0;
    size_t nchunks = len ? (len + CHUNK_LEN - 1) / CHUNK_LEN : 1;
    for (size_t c = 0; c + 1 < nchunks; c++) {
        Node n = chunk_node(data + c * CHUNK_LEN, CHUNK_LEN, c);
        uint32_t cv[8];
        node_cv(&n, cv);
        uint64_t total = c + 1;
        while ((total & 1) == 0) { /* B3M:merge */
            Node p = parent_node(stack[--sp], cv);
            node_cv(&p, cv);
            total >>= 1;
        }
        memcpy(stack[sp++], cv, sizeof cv);
    }
    size_t last = (nchunks - 1) * CHUNK_LEN;
    Node cur = chunk_node(data + last, len - last, nchunks - 1);
    while (sp > 0) {
        uint32_t cv[8];
        node_cv(&cur, cv);
        cur = parent_node(stack[--sp], cv);
    }
    cur.flags |= ROOT; /* B3M:root */
    uint32_t st[16];
    compress(cur.cv, cur.block, cur.block_len, cur.counter, cur.flags, st);
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 4; k++) out[4 * i + k] = (uint8_t)(st[i] >> (8 * k));
}
