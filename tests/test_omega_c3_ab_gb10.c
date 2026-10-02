/*
 * C3 A/B harness for the reduce and ldst launchers (E1 gap table; omega PR #198).
 * Sibling of tests/test_omega_unwritten_trap_gb10.c (the #201 harness, left unchanged):
 * same output lines, different launchers under test.
 *
 *   --chip [--repeats N] [--sizes a,b,c] [--ops SUM,MAX,MIN,MEAN,LDST] [--seed S]
 *   --arm                 GPU build only: prints "arm=<control|fix> c3_protect=<OFF|ON>" and exits 0, no device work
 * Exit: 0 = clean, 1 = hits or device errors, 2 = refused / NOT_RUN.
 * Defaults: --repeats 200, --sizes 1000003,65, --ops SUM,MAX,MEAN,LDST, --seed 0.
 *
 * Arms. The launchers src/omega_numeric_reduce_gb10.c (run_chunk) and src/omega_numeric_ldst_gb10.c
 * (omega_ldst_gb10_run) carry the C3 protection of omega_ds_gb10_run (L2_FLUSH_DIRTY mem-op plus a second
 * release marker 0x46464646 at marker_mem+0x10, waited for before every readback). Building every source
 * with -DOMEGA_C3_PROTECT_OFF compiles that protection out of those two launchers (the control arm); the
 * default build keeps it (the fix arm). The define does not touch omega_ds_gb10_run, so the MEAN final
 * division is protected in both arms. The arm is printed on the start line.
 *
 * What counts as a hit (one OMEGA_UNWRITTEN_HIT line each, summed into OMEGA_UNWRITTEN_TRAP: <hits>/<runs>):
 *   reduce ops  the call returned OMEGA_NUMERIC_OK and the result bits differ from omega_reduce_reference.
 *               Inputs are finite with |x| in [2^-15, 1), so every legitimate sum is below 2^20 and every
 *               legitimate max/min is below 1. run_chunk fills the device output with 0x55 bytes
 *               (src/omega_numeric_reduce_gb10.c, memset of out_mem), about 1.5e13 as a float. A result
 *               with biased exponent >= 0xA0 is labelled kind=poison, any other difference kind=wrong.
 *               The label is only a hint: every difference is a hit, and a poisoned MEAN is divided
 *               by n on the chip, so it is labelled wrong.
 *   LDST        a B32 load, B32 store copy of n words (host in -> GPU load -> GPU store -> host out)
 *               returned OK and out differs from in anywhere. The host output is pre-filled with 0x55555555
 *               (the launcher copies it to the device first) and no input word equals it, so a surviving
 *               0x55555555 is an unwritten word (kind=poison).
 * A call that returns OMEGA_NUMERIC_ERR_DEVICE (-4) is NOT a hit: it is a device error (OMEGA_UNWRITTEN_DEVERR
 * line, counted in device_errors; the launcher also prints its own step line on stderr: GB10_DEVFAIL (#180) for reduce
 * and divsqrt, OMEGA_DEVERR for ldst).
 * Any other non-OK code means the launcher refused the input before touching the device: the harness prints
 * OMEGA_UNWRITTEN_REFUSED and OMEGA_UNWRITTEN_TRAP: NOT_RUN and exits 2.
 * The harness stops early only after 3 device errors in a row (a jammed seat), never on a single one.
 *
 * Never run this directly: chip runs go through the forge queue and are never killed or timed out.
 */
#include "omega_numeric.h"
#include "omega_numeric_divsqrt_gb10.h"
#include "omega_numeric_ldst_gb10.h"
#include "omega_numeric_reduce.h"
#include "omega_unwritten_trap.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef OMEGA_NUMERIC_CPU_ONLY
int main(void) {
    printf("OMEGA_UNWRITTEN_TRAP: NOT_RUN (CPU-only build)\n");
    return 2;
}

#else /* GB10 build */

#define POISON 0x55555555u
#define MAX_SIZES 8
#define STOP_AFTER_CONSECUTIVE_DEVERR 3

#ifdef OMEGA_C3_PROTECT_OFF
#define ARM_NAME "control"
#define PROTECT_STATE "OFF"
#else
#define ARM_NAME "fix"
#define PROTECT_STATE "ON"
#endif

typedef struct {
    const char *name;
    int ldst;            /* 1: the ldst round trip, 0: a reduction */
    OmegaReduceOp rop;
} TestOp;

static const TestOp ALL_OPS[] = {
    { "SUM", 0, OMEGA_RED_SUM }, { "MAX", 0, OMEGA_RED_MAX }, { "MIN", 0, OMEGA_RED_MIN },
    { "MEAN", 0, OMEGA_RED_MEAN }, { "LDST", 1, OMEGA_RED_SUM },
};
#define N_ALL_OPS (sizeof(ALL_OPS) / sizeof(ALL_OPS[0]))

static uint64_t g_rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}

static uint32_t f2b(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static float b2f(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }

/* finite, |x| in [2^-15, 1): biased exponent 0x70..0x7e, random sign and mantissa */
static float gen_small(void) {
    uint32_t r = (uint32_t)rnd();
    return b2f((r & 0x807fffffu) | ((0x70u + ((r >> 23) % 15u)) << 23));
}

static void utc_now(char *buf, size_t cap) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static int parse_sizes(const char *s, size_t *out, int max) {
    int n = 0;
    while (*s && n < max) {
        char *e;
        unsigned long v = strtoul(s, &e, 10);
        if (e == s || v == 0 || v > OMEGA_REDUCE_MEAN_MAX_N) return -1;
        out[n++] = (size_t)v;
        if (*e && *e != ',') return -1;
        s = *e == ',' ? e + 1 : e;
    }
    return *s ? -1 : n;
}

static int parse_pos_long(const char *s, long *out) {
    char *e;
    long v = strtol(s, &e, 10);
    if (e == s || *e || v <= 0) return -1;
    *out = v;
    return 0;
}

static int parse_ops(const char *arg, const TestOp **out, int max) {
    char buf[128];
    if (strlen(arg) >= sizeof buf) return -1;
    strcpy(buf, arg);
    int n = 0;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        const TestOp *found = NULL;
        for (size_t i = 0; i < N_ALL_OPS; i++)
            if (!strcmp(tok, ALL_OPS[i].name)) found = &ALL_OPS[i];
        if (!found || n >= max) return -1;
        out[n++] = found;
    }
    return n;
}

#define USAGE "usage: --chip [--repeats N] [--sizes a,b,c] [--ops SUM,MAX,MIN,MEAN,LDST] [--seed S]\n"

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    int chip = 0;
    long repeats = 200, seed = 0;
    size_t sizes[MAX_SIZES] = { 1000003u, 65u };
    int nsizes = 2;
    const TestOp *ops[N_ALL_OPS];
    int nops = 0;
    ops[nops++] = &ALL_OPS[0];   /* SUM */
    ops[nops++] = &ALL_OPS[1];   /* MAX */
    ops[nops++] = &ALL_OPS[3];   /* MEAN */
    ops[nops++] = &ALL_OPS[4];   /* LDST */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--arm")) { printf("arm=%s c3_protect=%s\n", ARM_NAME, PROTECT_STATE); return 0; }
        else if (!strcmp(argv[i], "--chip")) chip = 1;
        else if (!strcmp(argv[i], "--repeats") && i + 1 < argc) {
            if (parse_pos_long(argv[++i], &repeats) != 0) { printf("bad --repeats\nOMEGA_UNWRITTEN_TRAP: NOT_RUN\n"); return 2; }
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            char *e;
            seed = strtol(argv[++i], &e, 10);
            if (e == argv[i] || *e) { printf("bad --seed\nOMEGA_UNWRITTEN_TRAP: NOT_RUN\n"); return 2; }
        } else if (!strcmp(argv[i], "--sizes") && i + 1 < argc) {
            nsizes = parse_sizes(argv[++i], sizes, MAX_SIZES);
            if (nsizes <= 0) { printf("bad --sizes (1..%u, at most %d)\nOMEGA_UNWRITTEN_TRAP: NOT_RUN\n", (unsigned)OMEGA_REDUCE_MEAN_MAX_N, MAX_SIZES); return 2; }
        } else if (!strcmp(argv[i], "--ops") && i + 1 < argc) {
            nops = parse_ops(argv[++i], ops, (int)N_ALL_OPS);
            if (nops <= 0) { printf("bad --ops (SUM,MAX,MIN,MEAN,LDST)\nOMEGA_UNWRITTEN_TRAP: NOT_RUN\n"); return 2; }
        } else { printf(USAGE); return 2; }
    }
    if (!chip) { printf(USAGE "OMEGA_UNWRITTEN_TRAP: NOT_RUN\n"); return 2; }
    g_rng ^= (uint64_t)seed * 0xd1b54a32d192ed03ull;
    if (!g_rng) g_rng = 1;

    size_t maxn = 0;
    for (int i = 0; i < nsizes; i++) if (sizes[i] > maxn) maxn = sizes[i];
    float *fx[MAX_SIZES] = { 0 };
    uint32_t *ux[MAX_SIZES] = { 0 }, *o = malloc(maxn * 4);
    int alloc_ok = o != NULL;
    for (int i = 0; i < nsizes; i++) {
        fx[i] = malloc(sizes[i] * sizeof(float));
        ux[i] = malloc(sizes[i] * sizeof(uint32_t));
        alloc_ok &= fx[i] != NULL && ux[i] != NULL;
    }
    if (!alloc_ok) { printf("out of memory\nOMEGA_UNWRITTEN_TRAP: NOT_RUN\n"); return 2; }

    char start_utc[40];
    utc_now(start_utc, sizeof(start_utc));
    printf("start=%s arm=%s c3_protect=%s repeats_per_op=%ld ops=%d sizes=%d seed=%ld\n", start_utc, ARM_NAME, PROTECT_STATE,
           repeats, nops, nsizes, seed);

    const OmegaLdstSpec ldst_spec = { OMEGA_LD_B32, OMEGA_ST_B32, 4, 4, 0, 0 };
    uint64_t hits = 0, runs = 0, dev_errs = 0;
    int consecutive = 0;
    for (int oi = 0; oi < nops; oi++) {
        const TestOp *t = ops[oi];
        uint32_t ref_bits[MAX_SIZES] = { 0 };
        /* inputs per size, fresh per op; the reduce reference is computed once per size */
        for (int si = 0; si < nsizes; si++) {
            for (size_t k = 0; k < sizes[si]; k++) {
                fx[si][k] = gen_small();
                uint32_t w = (uint32_t)rnd();
                ux[si][k] = w == POISON ? 0x3f800000u : w;
            }
            if (!t->ldst) {
                float r = 0;
                if (omega_reduce_reference(t->rop, fx[si], sizes[si], &r) != 0) {
                    printf("reference failed op=%s size=%zu\nOMEGA_UNWRITTEN_TRAP: NOT_RUN\n", t->name, sizes[si]);
                    return 2;
                }
                ref_bits[si] = f2b(r);
            }
        }
        for (long rep = 0; rep < repeats; rep++) {
            int si = (int)(rep % nsizes);
            size_t n = sizes[si];
            int rc, bad = 0;
            uint32_t got_bits = 0;
            unsigned launches = 0;
            OmegaUnwrittenReport r;
            uint64_t mism = 0;
            memset(&r, 0, sizeof r);
            struct timespec s0, s1;
            clock_gettime(CLOCK_MONOTONIC, &s0);
            if (t->ldst) {
                for (size_t k = 0; k < n; k++) o[k] = POISON;
                omega_ldst_gb10_set_spec_id(0);
                rc = omega_ldst_gb10_run(&ldst_spec, (const uint8_t *)ux[si], n * 4, (uint8_t *)o, n * 4, 0, n);
            } else {
                float g = 0;
                rc = omega_reduce_gb10(t->rop, fx[si], n, &g);
                launches = omega_reduce_gb10_last_launches();
                got_bits = f2b(g);
            }
            clock_gettime(CLOCK_MONOTONIC, &s1);
            runs++;
            if (rc != OMEGA_NUMERIC_OK && rc != OMEGA_NUMERIC_ERR_DEVICE) {
                /* refused before any device work (bad arguments, operands, FPCR): a harness fault, not a device event */
                printf("OMEGA_UNWRITTEN_REFUSED op=%s run=%ld size=%zu rc=%d arm=%s\n", t->name, rep, n, rc, ARM_NAME);
                printf("OMEGA_UNWRITTEN_TRAP: NOT_RUN\n");
                return 2;
            }
            if (rc != OMEGA_NUMERIC_OK) {
                dev_errs++;
                consecutive++;
                printf("OMEGA_UNWRITTEN_DEVERR op=%s run=%ld size=%zu rc=%d launches=%u arm=%s\n", t->name, rep, n, rc, launches, ARM_NAME);
                if (consecutive >= STOP_AFTER_CONSECUTIVE_DEVERR) {
                    printf("OMEGA_UNWRITTEN_STOP op=%s consecutive_device_errors=%d\n", t->name, consecutive);
                    goto done;   /* a jammed seat: stop, never loop on it */
                }
                continue;
            }
            consecutive = 0;
            if (t->ldst) {
                omega_unwritten_scan(o, n, POISON, OMEGA_UNWRITTEN_BLOCK, &r);
                for (size_t k = 0; k < n; k++) if (o[k] != ux[si][k]) mism++;
                bad = mism > 0;
            } else {
                bad = got_bits != ref_bits[si];
            }
            if (bad) {
                char now[40];
                long ms = (long)((s1.tv_sec - s0.tv_sec) * 1000 + (s1.tv_nsec - s0.tv_nsec) / 1000000);
                utc_now(now, sizeof(now));
                hits++;
                if (t->ldst)
                    printf("OMEGA_UNWRITTEN_HIT op=%s run=%ld size=%zu kind=%s count=%" PRIu64 " mismatches=%" PRIu64 " first=%" PRIu64
                           " last=%" PRIu64 " whole_blocks=%d launch_ms=%ld at=%s arm=%s\n",
                           t->name, rep, n, r.count ? "poison" : "wrong", r.count, mism, r.first, r.last, r.whole_blocks, ms, now, ARM_NAME);
                else
                    printf("OMEGA_UNWRITTEN_HIT op=%s run=%ld size=%zu kind=%s got=0x%08x want=0x%08x launches=%u launch_ms=%ld at=%s arm=%s\n",
                           t->name, rep, n, ((got_bits >> 23) & 0xffu) >= 0xa0u ? "poison" : "wrong",
                           got_bits, ref_bits[si], launches, ms, now, ARM_NAME);
            }
            if ((rep + 1) % 100 == 0) {
                printf("  %s %ld/%ld runs, hits so far %" PRIu64 ", device errors so far %" PRIu64 "\n", t->name, rep + 1, repeats, hits, dev_errs);
                fflush(stdout);
            }
        }
    }
done:
    printf("device_errors=%" PRIu64 "\n", dev_errs);
    printf("OMEGA_UNWRITTEN_TRAP: %" PRIu64 "/%" PRIu64 "\n", hits, runs);
    for (int i = 0; i < nsizes; i++) { free(fx[i]); free(ux[i]); }
    free(o);
    return (hits || dev_errs) ? 1 : 0;
}
#endif
