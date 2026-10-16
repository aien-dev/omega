/* gpu_atomf32_probe_test.c (training R5 part 2, 2026-10-15): do the f32 global atomic adds add right on GB10?
 * One kernel shape: every thread atomically adds the f32 argument v to one word (start value s) and stores what it got
 * back (or, in the no-destination form, its own thread index). Because every thread adds the same v, the sequence of
 * values the word takes is fixed whatever order the threads land in: o0 = s, o(k+1) = ftz(ftz(ok) + ftz(v)) rounded to
 * nearest even. Each case checks the final word against o(n) bit for bit and the returned old values against
 * { o0 .. o(n-1) } as a multiset (no update lost or doubled). Cases:
 *   0  .STRONG.SYS  131072 x add 1.0 from 0                  exact sums
 *   1  .STRONG.SYS  131072 x add -0.25 from 0                exact sums, negative
 *   2  .STRONG.SYS  32 x add 2^-125 to the subnormal 2^-140  FTZ: 0x03800000; no FTZ: 0x03800008; a no-op: 0x00000200
 *   3  .STRONG.SYS  32 x add 1.0 to 2^24 + 2                 nearest-even: 2^24 + 4; toward zero: 2^24 + 2 (= no-op)
 *   4  .STRONG.GPU  131072 x add 1.0 from 0
 *   5  .STRONG.GPU  no destination (RZ): 131072 x add 1.0; each thread then stores its index, which must survive
 *   6  CONTROL: the integer ATOMG.ADD on the 1.0f bits must FAIL the same check (proves the check can say no)
 *   --host-only     every case in the warp simulator (small grids)
 *   (default)       the chip, one short window; --cases DIGITS picks cases and their order
 * Throughput printed is every thread on ONE word in GPU-uncached host-visible memory: a contention measurement, not a
 * training figure and not the hardware ceiling. Verdict line last: "GPU_ATOMF32_PROBE: PASS|FAIL". */
/* omega_bw_em.h names two helpers fadd and ffma, which collide with C23 math.h (pulled in by the simulator) */
#define fadd omega_em_fadd
#define ffma omega_em_ffma
#include "omega_bw_em.h"
#undef fadd
#undef ffma
#include "omega_gpu_session.h"
#include "bw_warp_sim_tc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { BlackwellIROpcode op; int has_dst; uint32_t threads, grid, start, v; const char *name; int control; } Case;
static const Case cs[] = {
    { BW_IR_ATOMG_ADD_F32_STRONG_SYS, 1, 256, 512, 0x00000000u, 0x3f800000u, "SYS add 1.0 from 0", 0 },
    { BW_IR_ATOMG_ADD_F32_STRONG_SYS, 1, 256, 512, 0x00000000u, 0xbe800000u, "SYS add -0.25 from 0", 0 },
    { BW_IR_ATOMG_ADD_F32_STRONG_SYS, 1, 32, 1, 0x00000200u, 0x01000000u, "SYS add 2^-125 to subnormal 2^-140 (FTZ)", 0 },
    { BW_IR_ATOMG_ADD_F32_STRONG_SYS, 1, 32, 1, 0x4b800001u, 0x3f800000u, "SYS add 1.0 to 2^24+2 (round to nearest even)", 0 },
    { BW_IR_ATOMG_ADD_F32_STRONG_GPU, 1, 256, 512, 0x00000000u, 0x3f800000u, "GPU add 1.0 from 0", 0 },
    { BW_IR_ATOMG_ADD_F32_STRONG_GPU, 0, 256, 512, 0x00000000u, 0x3f800000u, "GPU add 1.0 from 0, no destination (RZ)", 0 },
    { BW_IR_ATOMG_ADD_STRONG_SYS, 1, 256, 512, 0x00000000u, 0x3f800000u, "CONTROL integer ATOMG.ADD of 1.0f bits", 1 },
};
#define NCASES (sizeof cs / sizeof cs[0])

/* args: word (u64 at 0x380), output array (u64 at 0x388), v bits (u32 at 0x390) */
static int atomf32_prog(const Case *c, uint32_t threads, BlackwellIRProgram *prog) {
    omega_bw_ir_init(prog);
    Em em; memset(&em, 0, sizeof em); em.p = prog; Em *e = &em;
    int udesc = UV64(e), pw = V64(e), po = V64(e), val = V(e), tid = V(e), cta = V(e), t = V(e), old = c->has_dst ? V(e) : NOREG;
    if (e->err) return -1;
    ldcu64(e, udesc, 0x358); ldc64(e, pw, 0x380); ldc64(e, po, 0x388); ldc32(e, val, 0x390);
    s2r(e, tid, BW_SR_TID_X); s2r(e, cta, BW_SR_CTAID_X);
    imadi(e, t, cta, 0, threads, tid); imadw(e, po, t, 4u, po);
    BlackwellIRInsn a = I0(c->op); a.dst_vreg = old; a.src1_vreg = pw; a.src2_vreg = val; a.ureg = udesc;
    emit(e, a, K_VAR);
    stg(e, po, 0, c->has_dst ? old : t, udesc); /* no destination: t was live across the atomic and must be intact */
    tail(e);
    if (e->err) return -1;
    return omega_bw_regalloc_solve(prog);
}

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint32_t ftz(uint32_t u) { return (u & 0x7f800000u) == 0u ? (u & 0x80000000u) : u; }
static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }

/* the reference: want[0 .. n-1] = old values (sorted), returns o(n). with_ftz = 0 gives the no-FTZ alternative. */
static uint32_t reference(uint32_t s, uint32_t v, size_t n, uint32_t *want, int with_ftz) {
    uint32_t o = s;
    for (size_t k = 0; k < n; k++) {
        if (want) want[k] = o;
        o = with_ftz ? ftz(f2u(u2f(ftz(o)) + u2f(ftz(v)))) : f2u(u2f(o) + u2f(v));
    }
    if (want) qsort(want, n, 4, cmp_u32);
    return o;
}

/* faults: final word, then every output slot (old values as a sorted multiset, or the thread index if no destination) */
static size_t check(const Case *c, uint32_t word, uint32_t *out, size_t n, uint32_t *want, uint32_t *wfinal) {
    *wfinal = reference(c->start, c->v, n, want, 1);
    size_t bad = word != *wfinal;
    if (c->has_dst) { qsort(out, n, 4, cmp_u32); for (size_t i = 0; i < n; i++) bad += out[i] != want[i]; }
    else for (size_t i = 0; i < n; i++) bad += out[i] != (uint32_t)i;
    return bad;
}

static void report(const char *where, const Case *c, size_t n, uint32_t word, uint32_t wfinal, size_t bad, uint64_t ns) {
    uint32_t noftz = reference(c->start, c->v, n, NULL, 0);
    printf("%s %s x %zu: word 0x%08x = %.9g (want 0x%08x", where, c->name, n, word, (double)u2f(word), wfinal);
    if (noftz != wfinal) printf(", no-FTZ would be 0x%08x", noftz);
    if (c->start != wfinal) printf(", a no-op would leave 0x%08x", c->start);
    printf("), %zu faults", bad);
    if (ns) printf(", %.1f us = %.2f M adds/s, all on one word in uncached host-visible memory (contention, not a ceiling)",
                   ns / 1e3, (double)n * 1e3 / (double)ns);
    printf("\n");
    if (c->control) printf("  control %s: the check %s the wrong arithmetic\n", bad ? "ok" : "BROKEN", bad ? "caught" : "missed");
}

static int verdict(const Case *c, size_t bad) { return c->control ? bad == 0 : bad != 0; }

static int host_only(void) {
    static BlackwellIRProgram p; static uint32_t word, out[2 * 64], want[2 * 64]; int fail = 0;
    for (size_t i = 0; i < NCASES; i++) {
        const Case *c = &cs[i]; const uint32_t T = c->threads < 64 ? c->threads : 64, G = c->grid < 2 ? c->grid : 2; const size_t n = (size_t)T * G;
        if (atomf32_prog(c, T, &p) != 0) { printf("sim %s: codegen refused\n", c->name); fail++; continue; }
        static WsSim s; memset(&s, 0, sizeof s); s.p = &p; s.threads = T;
        word = c->start; memset(out, 0xee, sizeof out);
        uint64_t wa = (uintptr_t)&word, oa = (uintptr_t)out;
        s.cbank[0x380 / 4] = (uint32_t)wa; s.cbank[0x384 / 4] = (uint32_t)(wa >> 32);
        s.cbank[0x388 / 4] = (uint32_t)oa; s.cbank[0x38c / 4] = (uint32_t)(oa >> 32); s.cbank[0x390 / 4] = c->v;
        s.ranges[0].lo = wa; s.ranges[0].hi = wa + 4; s.ranges[1].lo = oa; s.ranges[1].hi = oa + 4u * n; s.nranges = 2;
        int err = ws_run(&s, G, 1); uint32_t wf = 0;
        size_t bad = err ? 1 : check(c, word, out, n, want, &wf);
        if (err) printf("sim %s: %s\n", c->name, s.msg); else report("sim", c, n, word, wf, bad, 0);
        if (err || verdict(c, bad)) fail++;
    }
    return fail;
}

int main(int argc, char **argv) {
    int fail = host_only();
    if (argc > 1 && !strcmp(argv[1], "--host-only")) { printf("GPU_ATOMF32_PROBE: %s\n", fail ? "FAIL" : "PASS"); return fail ? 1 : 0; }
    if (fail) { printf("GPU_ATOMF32_PROBE: FAIL\n"); return 1; }
    const char *order = argc > 2 && !strcmp(argv[1], "--cases") ? argv[2] : "0123456";
    const size_t nmax = 256u * 512u;
    uint32_t *want = malloc(nmax * 4u), *got = malloc(nmax * 4u);
    omega_gpu_session_lock();
    if (!want || !got || !omega_gpu_session_open()) { printf("open failed: %s\n", omega_gpu_session_last_error()); omega_gpu_session_unlock(); free(want); free(got); return 1; }
    static BlackwellIRProgram p;
    for (const char *q = order; *q && !fail; q++) {
        const size_t i = (size_t)(*q - '0');
        if (i >= NCASES) { printf("bad case %c\n", *q); fail++; break; }
        const Case *c = &cs[i]; const size_t n = (size_t)c->threads * c->grid;
        OmegaGpuMem code = { 0 }, wm = { 0 }, om = { 0 }; OmegaBlackwellKernel k;
        if (atomf32_prog(c, c->threads, &p) != 0) { printf("codegen refused\n"); fail++; break; }
        size_t need = omega_bw_program_code_bytes(&p);
        if (omega_gpu_session_alloc_code(need, &code) != 0 || omega_blackwell_kernel_encode_into(&p, code.cpu, need, &k) != 0 || omega_gpu_session_sync_to_gpu(&code) != 0 ||
            omega_gpu_session_alloc(4096, &wm) != 0 || omega_gpu_session_alloc(n * 4u, &om) != 0) { printf("upload failed: %s\n", omega_gpu_session_last_error()); fail++; break; }
        uint32_t *w = wm.cpu, *o = om.cpu; uint64_t ns = 0;
        w[0] = c->start; memset(o, 0xee, n * 4u);
        uint32_t args[5] = { (uint32_t)wm.va, (uint32_t)(wm.va >> 32), (uint32_t)om.va, (uint32_t)(om.va >> 32), c->v };
        OmegaGpuLaunch L = { .code_va = code.va, .gpr_count = omega_bw_gpr_declared(p.regalloc.peak_gpr_usage), .threads_x = c->threads, .threads_y = 1,
                             .grid_x = c->grid, .grid_y = 1, .num_elements = n, .args = args, .n_args = 5, .timeout_ms = 20000 };
        if (omega_gpu_session_sync_to_gpu(&wm) != 0 || omega_gpu_session_sync_to_gpu(&om) != 0 || omega_gpu_session_launch(&L, &ns, NULL) != 0 ||
            omega_gpu_session_sync_to_cpu(&wm) != 0 || omega_gpu_session_sync_to_cpu(&om) != 0) { printf("launch failed: %s\n", omega_gpu_session_last_error()); fail++; }
        else {
            memcpy(got, o, n * 4u); uint32_t wf = 0;
            size_t bad = check(c, w[0], got, n, want, &wf);
            report("chip", c, n, w[0], wf, bad, ns);
            if (verdict(c, bad)) fail++;
        }
        omega_gpu_session_free(&code); omega_gpu_session_free(&wm); omega_gpu_session_free(&om);
    }
    omega_gpu_session_unlock();
    free(want); free(got);
    printf("GPU_ATOMF32_PROBE: %s\n", fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
