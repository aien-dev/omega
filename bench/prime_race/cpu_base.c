/* aien-cpu-c-base: conventional single-thread CPU sieve following the upstream "base" rules.
 * Faithful: a struct holds the whole state, one calloc per pass sized to the limit, odd factors searched
 * sequentially from 3 up to sqrt(limit), every multiple cleared individually starting at factor*factor.
 * Storage: 1 bit per odd number, bit k <-> number 2k+1, inverted (set = composite). Stepping 2*factor in
 * number space is stepping `factor` in odd-index space. */
#include "prime_race_impl.h"

#include <stdlib.h>
#include <string.h>

typedef struct cpu_sieve {
    uint64_t limit;
    uint64_t *composite; /* set bit = composite, indexed by (n-1)/2 */
    size_t nwords;
} cpu_sieve;

static uint64_t isqrt_u64(uint64_t n) {
    uint64_t r = 0;
    uint64_t lo = 0, hi = 1ULL << 21; /* sqrt(2^40) = 2^20 < hi */
    while (lo <= hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (mid * mid <= n) { r = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return r;
}

static inline int is_comp(const cpu_sieve *s, uint64_t n) { return (int)((s->composite[(n - 1) / 2 / 64] >> (((n - 1) / 2) % 64)) & 1u); }
static inline void mark_comp(cpu_sieve *s, uint64_t n) { s->composite[(n - 1) / 2 / 64] |= 1ULL << (((n - 1) / 2) % 64); }

static void sieve_destroy(cpu_sieve *s) {
    free(s->composite);
    s->composite = NULL;
    s->nwords = 0;
}

static int sieve_create(cpu_sieve *s, uint64_t limit) {
    s->limit = limit;
    s->nwords = pr_words64(limit);
    s->composite = (uint64_t *)calloc(s->nwords ? s->nwords : 1, sizeof(uint64_t));
    return s->composite ? 0 : -1;
}

static void sieve_run(cpu_sieve *s) {
    const uint64_t limit = s->limit;
    const uint64_t q = isqrt_u64(limit);
    uint64_t factor = 3;
    while (factor <= q) {
        /* next unmarked odd number at or above factor */
        for (uint64_t n = factor; n <= limit; n += 2) {
            if (!is_comp(s, n)) { factor = n; break; }
        }
        for (uint64_t num = factor * factor; num <= limit; num += factor * 2) mark_comp(s, num);
        factor += 2;
    }
}

static int cpu_setup(void **state, uint64_t limit) {
    (void)limit;
    cpu_sieve *s = (cpu_sieve *)calloc(1, sizeof *s);
    if (!s) return -1;
    *state = s;
    return 0;
}

static int cpu_pass(void *state, uint64_t limit) {
    cpu_sieve *s = (cpu_sieve *)state;
    sieve_destroy(s); /* previous pass's sieve is destroyed inside the timed pass */
    if (sieve_create(s, limit) != 0) return -1;
    sieve_run(s);
    return 0;
}

static int cpu_export(void *state, uint64_t limit, uint64_t *out) {
    cpu_sieve *s = (cpu_sieve *)state;
    if (!s->composite || s->limit != limit) return -1;
    const size_t nw = pr_words64(limit);
    const uint64_t odd = pr_odd_count(limit);
    for (size_t w = 0; w < nw; w++) out[w] = ~s->composite[w];
    if (nw) {
        out[0] &= ~1ULL; /* number 1 is not prime */
        if (odd % 64) out[nw - 1] &= (1ULL << (odd % 64)) - 1; /* tail bits zero */
    }
    return 0;
}

static uint64_t cpu_threads(void *state) { (void)state; return 1; }

static void cpu_teardown(void *state) {
    cpu_sieve *s = (cpu_sieve *)state;
    if (!s) return;
    sieve_destroy(s);
    free(s);
}

const pr_impl pr_cpu_base_impl = {
    .name = "cpu-c-base",
    .algorithm = "base",
    .faithful = "yes",
    .bits = 1,
    .environment = "linux-host-cpu",
    .setup = cpu_setup,
    .pass = cpu_pass,
    .export_bitmap = cpu_export,
    .threads = cpu_threads,
    .detail_json = NULL,
    .teardown = cpu_teardown,
};

#ifndef PR_NO_MAIN
int main(int argc, char **argv) { return pr_main(argc, argv, &pr_cpu_base_impl); }
#endif
