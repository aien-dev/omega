/* Host unit test of the C3 unwritten-output detector, on synthetic buffers. */
#include "omega_unwritten_trap.h"
#include <stdio.h>
#include <stdlib.h>

#define POISON 0x55555555u
static int pass, fail;
static void check(int ok, const char *id) { if (ok) pass++; else fail++; printf("[%s] %s\n", ok ? "PASS" : "FAIL", id); }

static uint32_t *mk(size_t n) {
    uint32_t *b = malloc(n * 4);
    if (!b) exit(2);
    for (size_t i = 0; i < n; i++) b[i] = (uint32_t)(0x3f800000u + i);   /* never the poison */
    return b;
}
static void fillp(uint32_t *b, size_t lo, size_t hi) { for (size_t i = lo; i < hi; i++) b[i] = POISON; }

int main(void) {
    OmegaUnwrittenReport r;
    const size_t N = 1000003;   /* not a multiple of 64; last block has 3 words */
    uint32_t *b = mk(N);

    omega_unwritten_scan(b, N, POISON, 64, &r);
    check(r.count == 0 && r.runs == 0 && !r.whole_blocks && !r.tail && r.full_blocks == 0 && r.partial_blocks == 0, "NO_POISON");

    omega_unwritten_scan(b, 0, POISON, 64, &r);
    check(r.count == 0, "EMPTY_BUFFER");

    /* tail: last 100 words */
    fillp(b, N - 100, N);
    omega_unwritten_scan(b, N, POISON, 64, &r);
    check(r.count == 100 && r.first == N - 100 && r.last == N - 1 && r.runs == 1 && r.tail, "TAIL_COUNT_INDEX_RUN");
    check(!r.whole_blocks && r.partial_blocks == 1 && r.full_blocks == 2, "TAIL_STARTS_MID_BLOCK_IS_NOT_WHOLE");
    free(b); b = mk(N);

    /* tail that is exactly the short last block: whole */
    fillp(b, N - 3, N);
    omega_unwritten_scan(b, N, POISON, 64, &r);
    check(r.count == 3 && r.tail && r.whole_blocks && r.full_blocks == 1, "SHORT_LAST_BLOCK_IS_WHOLE");
    free(b);

    /* whole-block window in the middle: 1536 blocks = 98304 words (the C3 event shape) */
    size_t M = 64 * 262144 - 37;  /* 16777179: grid-sized, not a multiple of 64 */
    size_t lo = 64 * 3000, hi = lo + 98304;
    b = mk(M);
    fillp(b, lo, hi);
    omega_unwritten_scan(b, M, POISON, 64, &r);
    check(r.count == 98304 && r.first == lo && r.last == hi - 1, "WINDOW_COUNT_FIRST_LAST");
    check(r.whole_blocks && r.full_blocks == 1536 && r.partial_blocks == 0 && r.runs == 1 && r.longest_run == 98304 && !r.tail, "WINDOW_IS_1536_WHOLE_BLOCKS");

    /* two separate whole-block windows */
    fillp(b, 64 * 10, 64 * 12);
    omega_unwritten_scan(b, M, POISON, 64, &r);
    check(r.runs == 2 && r.full_blocks == 1538 && r.whole_blocks && r.first == 640, "TWO_WINDOWS");
    free(b); b = mk(N);

    /* shifted window (starts 1 word into a block): not whole blocks */
    fillp(b, 64 * 100 + 1, 64 * 102 + 1);
    omega_unwritten_scan(b, N, POISON, 64, &r);
    check(r.count == 128 && !r.whole_blocks && r.partial_blocks == 2 && r.full_blocks == 1, "SHIFTED_WINDOW_NOT_WHOLE");
    free(b); b = mk(N);

    /* scattered hits */
    size_t idx[] = { 5, 70, 71, 9000, 500000, N - 1 };
    for (size_t i = 0; i < sizeof(idx) / sizeof(idx[0]); i++) b[idx[i]] = POISON;
    omega_unwritten_scan(b, N, POISON, 64, &r);
    check(r.count == 6 && r.first == 5 && r.last == N - 1 && r.runs == 5 && r.longest_run == 2, "SCATTERED_COUNT_INDEX_RUNS");
    check(!r.whole_blocks && !r.tail && r.partial_blocks == 5 && r.full_blocks == 0, "SCATTERED_NOT_WHOLE_NOT_TAIL");

    /* a different poison value is ignored */
    omega_unwritten_scan(b, N, 0xdeadbeefu, 64, &r);
    check(r.count == 0, "OTHER_POISON_NOT_COUNTED");
    free(b);

    printf("SUMMARY passed=%d failed=%d\n", pass, fail);
    return fail ? 1 : 0;
}
