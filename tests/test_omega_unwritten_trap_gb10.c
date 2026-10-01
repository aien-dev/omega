/*
 * C3b: GB10 stress harness for the intermittent "unwritten output" event
 * (E1 gap table row 10, receipt 6ca4ee4e, omega d25d7ad).
 *
 *   --chip [--repeats N] [--sizes a,b,c] [--ops SIGMOID,SQRT]
 * Exit: 0 = clean (no unwritten output), 1 = hits or device error, 2 = refused / NOT_RUN.
 *
 * Reuses omega_ds_gb10_run (src/omega_numeric_divsqrt_gb10.c) unchanged: kernels
 * and dispatch are not touched. That function allocates its own buffers, fills
 * the device output with 0x55 (:1040 at 32ad8a5 is the memset), waits on the
 * host marker and then the release semaphore, and copies out. So the poison seen
 * here is 0x55555555, fixed by that path. To make it unambiguous, every input is
 * drawn with an exponent below 0xD0 (|x| < 2^82): SIGMOID is in [0,1] or the
 * canonical NaN, SQRT is below 2^41 or NaN, so neither can ever emit
 * 0x55555555 (about 1.5e13). The host model is checked on every input first and
 * the run refuses if any input could produce the poison.
 *
 * Device-side completion counter: NOT POSSIBLE without changing the kernels or
 * the launch path. The kernel frame has no atomic add, the argument block is
 * fixed (args[0..9] in omega_ds_gb10_run), and the semaphore and marker values
 * are private to that function (never returned). So the receipt reports only the
 * host-visible poison scan; the earlier diagnosis (C3-unwritten-output.md
 * section 4 item 5) says a counter needs a diagnostic-only kernel.
 *
 * Output: one OMEGA_UNWRITTEN_HIT line per hit, then
 *   OMEGA_UNWRITTEN_TRAP: <hits>/<runs>
 * Never run this directly: chip runs go through the forge queue and are never
 * killed or timed out (tools/run_unwritten_trap.sh).
 */
#include "omega_numeric.h"
#include "omega_numeric_divsqrt_gb10.h"
#include "omega_unwritten_trap.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define POISON 0x55555555u

static uint64_t g_rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}

/* Runs a shell command, joins its output into one line (newlines become " | "). */
static void snap(const char *cmd, char *buf, size_t cap) {
    buf[0] = '\0';
    FILE *f = popen(cmd, "r");
    if (!f) { snprintf(buf, cap, "popen failed"); return; }
    size_t n = 0;
    int c;
    while ((c = fgetc(f)) != EOF && n + 4 < cap) {
        if (c == '\n') { if (n && buf[n - 1] != '|') { buf[n++] = ' '; buf[n++] = '|'; buf[n++] = ' '; } }
        else if (c == '"') buf[n++] = '\'';
        else buf[n++] = (char)c;
    }
    buf[n] = '\0';
    pclose(f);
    if (n == 0) snprintf(buf, cap, "(none)");
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
        if (e == s || v == 0 || v > OMEGA_DS_MAX_BATCH) return -1;
        out[n++] = v;
        s = *e == ',' ? e + 1 : e;
        if (*e && *e != ',') return -1;
    }
    return n;
}

#ifdef OMEGA_NUMERIC_CPU_ONLY
int main(void) {
    printf("OMEGA_UNWRITTEN_TRAP: NOT_RUN (CPU-only build)\n");
    return 2;
}
#else
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    int chip = 0;
    long repeats = 3000;
    size_t sizes[8] = { 16777153u, 16777151u, 1000003u };   /* 2^24-63, 2^24-65, 1000003: none a multiple of 64 */
    int nsizes = 3;
    OmegaDsOp ops[2] = { OMEGA_DS_SIGMOID, OMEGA_DS_SQRT };
    int nops = 2;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--chip")) chip = 1;
        else if (!strcmp(argv[i], "--repeats") && i + 1 < argc) repeats = atol(argv[++i]);
        else if (!strcmp(argv[i], "--sizes") && i + 1 < argc) {
            nsizes = parse_sizes(argv[++i], sizes, 8);
            if (nsizes <= 0) { printf("bad --sizes\nOMEGA_UNWRITTEN_TRAP: NOT_RUN\n"); return 2; }
        } else if (!strcmp(argv[i], "--ops") && i + 1 < argc) {
            const char *s = argv[++i];
            nops = 0;
            if (strstr(s, "SIGMOID")) ops[nops++] = OMEGA_DS_SIGMOID;
            if (strstr(s, "SQRT")) ops[nops++] = OMEGA_DS_SQRT;
            if (nops == 0) { printf("bad --ops (SIGMOID,SQRT)\nOMEGA_UNWRITTEN_TRAP: NOT_RUN\n"); return 2; }
        } else { printf("usage: --chip [--repeats N] [--sizes a,b,c] [--ops SIGMOID,SQRT]\n"); return 2; }
    }
    if (!chip || repeats <= 0) { printf("usage: --chip [--repeats N] [--sizes a,b,c] [--ops SIGMOID,SQRT]\nOMEGA_UNWRITTEN_TRAP: NOT_RUN\n"); return 2; }
    for (int i = 0; i < nsizes; i++)
        if (sizes[i] % OMEGA_UNWRITTEN_BLOCK == 0) printf("note: size %zu is a multiple of 64\n", sizes[i]);

    size_t maxn = 0;
    for (int i = 0; i < nsizes; i++) if (sizes[i] > maxn) maxn = sizes[i];
    uint32_t *ain[8] = { 0 }, *o = malloc(maxn * 4);
    for (int i = 0; i < nsizes; i++) ain[i] = malloc(sizes[i] * 4);
    int alloc_ok = o != NULL;
    for (int i = 0; i < nsizes; i++) alloc_ok &= ain[i] != NULL;
    if (!alloc_ok) { printf("out of memory\nOMEGA_UNWRITTEN_TRAP: NOT_RUN\n"); return 2; }

    time_t t0 = time(NULL);
    char start_utc[40];
    utc_now(start_utc, sizeof(start_utc));
    printf("start=%s repeats_per_op=%ld ops=%d sizes=%d\n", start_utc, repeats, nops, nsizes);

    uint64_t hits = 0, runs = 0, dev_errs = 0;
    for (int oi = 0; oi < nops; oi++) {
        OmegaDsOp op = ops[oi];
        const char *on = omega_ds_op_name(op);
        /* inputs per size, fresh per op; the host model must never equal the poison */
        for (int si = 0; si < nsizes; si++)
            for (size_t k = 0; k < sizes[si]; k++) {
                uint32_t r = (uint32_t)rnd();
                ain[si][k] = (r & 0x807fffffu) | (((r >> 23) % 0xD0u) << 23);
                if (omega_ds_host_exec(op, ain[si][k], 0) == POISON) ain[si][k] = 0x3f800000u;
            }
        for (long rep = 0; rep < repeats; rep++) {
            int si = (int)(rep % nsizes);
            size_t n = sizes[si];
            uint32_t *a = ain[si];
            memset(o, 0xAB, n * 4);   /* host copy; the device buffer is filled with 0x55 inside the run */
            struct timespec s0, s1;
            clock_gettime(CLOCK_MONOTONIC, &s0);
            int rc = omega_ds_gb10_run(op, a, NULL, o, n);
            clock_gettime(CLOCK_MONOTONIC, &s1);
            runs++;
            if (rc != OMEGA_NUMERIC_OK) {
                dev_errs++;
                printf("OMEGA_UNWRITTEN_DEVERR op=%s run=%ld size=%zu rc=%d\n", on, rep, n, rc);
                if (dev_errs >= 3) goto done;   /* a jammed seat: stop, never loop on it */
                continue;
            }
            OmegaUnwrittenReport r;
            omega_unwritten_scan(o, n, POISON, OMEGA_UNWRITTEN_BLOCK, &r);
            if (r.count) {
                hits++;
                char now[40], smi[2048], procs[2048], klog[4096];
                char cmd[512];
                utc_now(now, sizeof(now));
                snap("nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader 2>&1", smi, sizeof(smi));
                snap("ps -eo pid,etimes,pcpu,comm,args --sort=-pcpu 2>&1 | grep -E 'qemu|cargo|rustc|nvidia|test_omega|omega' | grep -v grep | head -n 20", procs, sizeof(procs));
                snprintf(cmd, sizeof(cmd),
                         "(journalctl -k --since @%lld --no-pager -q 2>&1; dmesg 2>&1) | grep -iE 'xid|nvrm|fault|not permitted|permission|no journal' | tail -n 20",
                         (long long)t0);
                snap(cmd, klog, sizeof(klog));
                printf("OMEGA_UNWRITTEN_HIT op=%s run=%ld size=%zu count=%" PRIu64 " first=%" PRIu64 " last=%" PRIu64
                       " runs=%" PRIu64 " longest_run=%" PRIu64 " full_blocks=%" PRIu64 " partial_blocks=%" PRIu64
                       " whole_blocks=%d tail=%d launch_ms=%ld at=%s nvidia_smi=\"%s\" procs=\"%s\" kernel_log=\"%s\"\n",
                       on, rep, n, r.count, r.first, r.last, r.runs, r.longest_run, r.full_blocks, r.partial_blocks,
                       r.whole_blocks, r.tail,
                       (long)((s1.tv_sec - s0.tv_sec) * 1000 + (s1.tv_nsec - s0.tv_nsec) / 1000000), now, smi, procs, klog);
            }
            if ((rep + 1) % 100 == 0) { printf("  %s %ld/%ld runs, hits so far %" PRIu64 "\n", on, rep + 1, repeats, hits); fflush(stdout); }
        }
    }
done:
    printf("device_errors=%" PRIu64 "\n", dev_errs);
    printf("OMEGA_UNWRITTEN_TRAP: %" PRIu64 "/%" PRIu64 "\n", hits, runs);
    for (int i = 0; i < nsizes; i++) free(ain[i]);
    free(o);
    return (hits || dev_errs) ? 1 : 0;
}
#endif
