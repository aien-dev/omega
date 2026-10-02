#include "omega_unwritten_trap.h"
#include <string.h>

void omega_unwritten_scan(const uint32_t *out, size_t n, uint32_t poison, uint32_t block,
                          OmegaUnwrittenReport *r) {
    memset(r, 0, sizeof(*r));
    if (!out || block == 0) return;
    uint64_t run = 0;
    for (size_t b = 0; b < n; b += block) {
        size_t end = b + block < n ? b + block : n;
        uint64_t hits = 0;
        for (size_t i = b; i < end; i++) {
            if (out[i] == poison) {
                if (r->count == 0) r->first = i;
                r->last = i;
                r->count++;
                hits++;
                if (run == 0) r->runs++;
                run++;
                if (run > r->longest_run) r->longest_run = run;
            } else {
                run = 0;
            }
        }
        if (hits == end - b) r->full_blocks++;
        else if (hits) r->partial_blocks++;
    }
    r->whole_blocks = r->count > 0 && r->partial_blocks == 0;
    r->tail = r->count > 0 && r->last == n - 1 && r->count == (uint64_t)n - r->first;
}
