/*
 * world_explore.c -- breadth-first search over every reachable model state
 * (bounded only by WM_EXT_MAX outside publications and WM_TICK_MAX clock
 * steps). Breadth-first, so the first trace found for a violation is a
 * shortest one.
 */
#include "world_explore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_MAX 48
#define TABLE_BITS 20u
#define TABLE_SIZE (1u << TABLE_BITS)

typedef struct {
    WmState s;
    uint32_t parent;
    WmOp op;
    uint8_t key[KEY_MAX];
    uint8_t klen;
} Node;

int wx_bit_index(uint32_t bit) {
    for (int i = 0; i < 5; i++) if (bit == (1u << i)) return i;
    return -1;
}

static uint64_t fnv(const uint8_t *k, int n) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++) { h ^= k[i]; h *= 1099511628211ull; }
    return h;
}

static int trace_of(const Node *nodes, uint32_t idx, WmOp *out) {
    WmOp rev[WX_MAX_TRACE];
    int n = 0;
    while (idx != 0 && n < WX_MAX_TRACE) { rev[n++] = nodes[idx].op; idx = nodes[idx].parent; }
    for (int i = 0; i < n; i++) out[i] = rev[n - 1 - i];
    return n;
}

int wx_explore(const WmProfile *p, WxResult *res) {
    memset(res, 0, sizeof *res);
    for (int i = 0; i < 5; i++) res->trace_len[i] = -1;
    uint32_t cap = TABLE_SIZE / 2;
    Node *nodes = calloc(cap, sizeof *nodes);
    uint32_t *table = malloc(sizeof(uint32_t) * TABLE_SIZE);
    if (!nodes || !table) { free(nodes); free(table); return -1; }
    memset(table, 0xff, sizeof(uint32_t) * TABLE_SIZE);
    uint32_t n = 0;
    wm_init(&nodes[0].s);
    nodes[0].klen = (uint8_t)wm_key(&nodes[0].s, nodes[0].key);
    table[fnv(nodes[0].key, nodes[0].klen) & (TABLE_SIZE - 1)] = 0;
    n = 1;
    WmOp ops[16];
    int nops = wm_all_ops(ops);
    for (uint32_t head = 0; head < n; head++) {
        for (int o = 0; o < nops; o++) {
            if (!wm_enabled(p, &nodes[head].s, ops[o])) continue;
            WmState t = nodes[head].s;
            uint32_t v = wm_step(p, &t, ops[o]);
            res->transitions++;
            uint8_t key[KEY_MAX];
            int kl = wm_key(&t, key);
            uint64_t h = fnv(key, kl);
            uint32_t slot = (uint32_t)(h & (TABLE_SIZE - 1));
            int found = 0;
            while (table[slot] != UINT32_MAX) {
                const Node *q = &nodes[table[slot]];
                if (q->klen == kl && memcmp(q->key, key, (size_t)kl) == 0) { found = 1; break; }
                slot = (slot + 1) & (TABLE_SIZE - 1);
            }
            uint32_t at;
            if (found) {
                at = UINT32_MAX;
            } else {
                if (n >= cap) { res->truncated = 1; continue; }
                at = n++;
                nodes[at].s = t;
                nodes[at].parent = head;
                nodes[at].op = ops[o];
                memcpy(nodes[at].key, key, (size_t)kl);
                nodes[at].klen = (uint8_t)kl;
                table[slot] = at;
            }
            if (!v) continue;
            res->viol_seen |= v;
            for (int b = 0; b < 5; b++) {
                if (!(v & (1u << b)) || res->trace_len[b] >= 0) continue;
                /* Path to head, then this op. */
                int len = trace_of(nodes, head, res->trace[b]);
                if (len < WX_MAX_TRACE) res->trace[b][len++] = ops[o];
                res->trace_len[b] = len;
            }
            (void)at;
        }
    }
    res->states = n;
    free(nodes);
    free(table);
    return 0;
}

void wx_print_trace(const WmOp *t, int n) {
    for (int i = 0; i < n; i++) {
        char b[32];
        wm_op_str(t[i], b, sizeof b);
        printf("%s%s", i ? " -> " : "", b);
    }
}
