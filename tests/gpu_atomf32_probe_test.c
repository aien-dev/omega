/* gpu_atomf32_probe_test.c (training R5 part 2, 2026-10-15): does ATOMG.E.ADD.F32.FTZ.RN.STRONG.SYS add right on GB10?
 * One kernel: every thread atomically adds the f32 argument to one word and stores the old value it got back.
 *   --host-only  the kernel in the warp simulator
 *   (default)    the chip (one short window): N = 512 x 256 threads add 1.0 and then -0.25 to one word; the final word
 *                must be exactly N * value (every partial sum is exact in f32) and the returned old values must be
 *                exactly { k * value : k = 0 .. N-1 }, each once (no lost or doubled update). Then 32 threads add the
 *                subnormal 2^-140 to 0.0: flush-to-zero must leave the word 0 (without FTZ it would be 2^-135).
 *                Negative control last: the integer ATOMG.ADD on the 1.0f bits must fail the same check.
 * Verdict line last: "GPU_ATOMF32_PROBE: PASS|FAIL". */
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

/* args: word (u64 at 0x380), old-value array (u64 at 0x388), value bits (u32 at 0x390) */
static int atomf32_prog(BlackwellIROpcode op, uint32_t threads, BlackwellIRProgram *prog) {
    omega_bw_ir_init(prog);
    Em em; memset(&em, 0, sizeof em); em.p = prog; Em *e = &em;
    int udesc = UV64(e), pw = V64(e), po = V64(e), val = V(e), tid = V(e), cta = V(e), t = V(e), old = V(e);
    if (e->err) return -1;
    ldcu64(e, udesc, 0x358); ldc64(e, pw, 0x380); ldc64(e, po, 0x388); ldc32(e, val, 0x390);
    s2r(e, tid, BW_SR_TID_X); s2r(e, cta, BW_SR_CTAID_X);
    imadi(e, t, cta, 0, threads, tid); imadw(e, po, t, 4u, po);
    BlackwellIRInsn a = I0(op); a.dst_vreg = old; a.src1_vreg = pw; a.src2_vreg = val; a.ureg = udesc;
    emit(e, a, K_VAR);
    stg(e, po, 0, old, udesc);
    tail(e);
    if (e->err) return -1;
    return omega_bw_regalloc_solve(prog);
}

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* word must be n * v; olds must be { k * v }, each k in 0 .. n-1 exactly once. Returns the number of faults. */
static size_t check(float word, const float *olds, size_t n, float v, unsigned char *seen) {
    size_t bad = word != (float)n * v;
    memset(seen, 0, n);
    for (size_t i = 0; i < n; i++) {
        float k = olds[i] / v;
        if (!(k >= 0.0f && k < (float)n) || k != (float)(size_t)k || olds[i] != k * v || seen[(size_t)k]++) bad++;
    }
    return bad;
}

static int host_only(const BlackwellIRProgram *p, uint32_t threads) {
    if (threads > 256) return 1;
    static float word, olds[2 * 256]; static unsigned char seen[2 * 256]; int fail = 0;
    const float vals[2] = { 1.0f, -0.25f };
    for (int c = 0; c < 2; c++) {
        static WsSim s; memset(&s, 0, sizeof s); s.p = (BlackwellIRProgram *)p; s.threads = threads;
        word = 0.0f; for (size_t i = 0; i < 2u * threads; i++) olds[i] = 99.0f;
        uint64_t wa = (uintptr_t)&word, oa = (uintptr_t)olds;
        s.cbank[0x380 / 4] = (uint32_t)wa; s.cbank[0x384 / 4] = (uint32_t)(wa >> 32);
        s.cbank[0x388 / 4] = (uint32_t)oa; s.cbank[0x38c / 4] = (uint32_t)(oa >> 32); s.cbank[0x390 / 4] = f2u(vals[c]);
        s.ranges[0].lo = wa; s.ranges[0].hi = wa + 4; s.ranges[1].lo = oa; s.ranges[1].hi = oa + 8u * threads; s.nranges = 2;
        int err = ws_run(&s, 2, 1);
        size_t bad = err ? 1 : check(word, olds, 2u * threads, vals[c], seen);
        printf("sim add %g x %u: %s, word %g, %zu faults\n", (double)vals[c], 2u * threads, err ? s.msg : "ran", (double)word, bad);
        if (bad) fail++;
    }
    return fail;
}

int main(int argc, char **argv) {
    const uint32_t T = 256, grid = 512; const size_t n = (size_t)T * grid;
    static BlackwellIRProgram p;
    if (atomf32_prog(BW_IR_ATOMG_ADD_F32_STRONG_SYS, T, &p) != 0) { printf("codegen refused\nGPU_ATOMF32_PROBE: FAIL\n"); return 1; }
    printf("kernel: %zu instructions, %u registers\n", p.count, p.regalloc.peak_gpr_usage);
    static BlackwellIRProgram p32, pint;
    if (atomf32_prog(BW_IR_ATOMG_ADD_F32_STRONG_SYS, 32, &p32) != 0 || atomf32_prog(BW_IR_ATOMG_ADD_STRONG_SYS, T, &pint) != 0) { printf("codegen refused (32)\nGPU_ATOMF32_PROBE: FAIL\n"); return 1; }
    int fail = host_only(&p32, 32) + host_only(&p, T);
    if (argc > 1 && !strcmp(argv[1], "--host-only")) { printf("GPU_ATOMF32_PROBE: %s\n", fail ? "FAIL" : "PASS"); return fail ? 1 : 0; }
    if (fail) { printf("GPU_ATOMF32_PROBE: FAIL\n"); return 1; }

    unsigned char *seen = malloc(n);
    omega_gpu_session_lock();
    if (!seen || !omega_gpu_session_open()) { printf("open failed: %s\n", omega_gpu_session_last_error()); omega_gpu_session_unlock(); free(seen); return 1; }
    static const struct { uint32_t threads, grid; uint32_t vbits; const char *name; } cs[] = {
        { 256, 512, 0x3f800000u, "add 1.0" }, { 256, 512, 0xbe800000u, "add -0.25" }, { 32, 1, 0x00000200u, "add 2^-140 (subnormal)" },
        /* negative control: the integer atomic on the same float bits must be caught as wrong by the same check */
        { 256, 512, 0x3f800000u, "CONTROL integer ATOMG.ADD of 1.0f bits" },
    };
    /* --cases DIGITS runs the listed cases in that order in one process (default 0123) */
    const char *order = argc > 2 && !strcmp(argv[1], "--cases") ? argv[2] : "0123";
    for (const char *q = order; *q && !fail; q++) {
        const size_t c = (size_t)(*q - '0');
        if (c >= sizeof cs / sizeof cs[0]) { printf("bad case %c\n", *q); fail++; break; }
        const int control = c == 3;
        BlackwellIRProgram *kp = control ? &pint : cs[c].threads == 32 ? &p32 : &p;
        OmegaGpuMem code = { 0 }, wm = { 0 }, om = { 0 }; OmegaBlackwellKernel k;
        size_t need = omega_bw_program_code_bytes(kp), cn = (size_t)cs[c].threads * cs[c].grid;
        if (omega_gpu_session_alloc_code(need, &code) != 0 || omega_blackwell_kernel_encode_into(kp, code.cpu, need, &k) != 0 || omega_gpu_session_sync_to_gpu(&code) != 0 ||
            omega_gpu_session_alloc(4096, &wm) != 0 || omega_gpu_session_alloc(cn * 4u, &om) != 0) { printf("upload failed: %s\n", omega_gpu_session_last_error()); fail++; break; }
        float *w = wm.cpu, *o = om.cpu; uint64_t ns = 0;
        w[0] = 0.0f; for (size_t i = 0; i < cn; i++) o[i] = 99.0f;
        uint32_t args[5] = { (uint32_t)wm.va, (uint32_t)(wm.va >> 32), (uint32_t)om.va, (uint32_t)(om.va >> 32), cs[c].vbits };
        OmegaGpuLaunch L = { .code_va = code.va, .gpr_count = omega_bw_gpr_declared(kp->regalloc.peak_gpr_usage), .threads_x = cs[c].threads, .threads_y = 1,
                             .grid_x = cs[c].grid, .grid_y = 1, .num_elements = cn, .args = args, .n_args = 5, .timeout_ms = 20000 };
        if (omega_gpu_session_sync_to_gpu(&wm) != 0 || omega_gpu_session_sync_to_gpu(&om) != 0 || omega_gpu_session_launch(&L, &ns, NULL) != 0 ||
            omega_gpu_session_sync_to_cpu(&wm) != 0 || omega_gpu_session_sync_to_cpu(&om) != 0) { printf("launch failed: %s\n", omega_gpu_session_last_error()); fail++; }
        else if (cs[c].threads == 32) {
            size_t bad = f2u(w[0]) != 0u; for (size_t i = 0; i < cn; i++) bad += f2u(o[i]) != 0u;
            printf("chip %s x %zu: word bits 0x%08x (FTZ wants 0x00000000, no-FTZ would be 0x%08x), %zu faults, %.1f us\n", cs[c].name, cn, f2u(w[0]),
                   f2u(u2f(cs[c].vbits) * 32.0f), bad, ns / 1e3);
            if (bad) fail++;
        } else {
            size_t bad = check(w[0], o, cn, u2f(cs[c].vbits), seen);
            printf("chip %s x %zu: word %.2f (want %.2f), %zu faults, %.1f us (%.1f M atomic adds/s to one word)\n", cs[c].name, cn, (double)w[0],
                   (double)((float)cn * u2f(cs[c].vbits)), bad, ns / 1e3, cn * 1e3 / (double)ns);
            if (control) printf("  control %s: the check %s the wrong arithmetic\n", bad ? "ok" : "BROKEN", bad ? "caught" : "missed");
            if (control ? !bad : bad) fail++;
        }
        omega_gpu_session_free(&code); omega_gpu_session_free(&wm); omega_gpu_session_free(&om);
    }
    omega_gpu_session_unlock();
    free(seen);
    printf("GPU_ATOMF32_PROBE: %s\n", fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
