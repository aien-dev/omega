#include "omega_ternary_verify.h"
#include "omega_ternary_measure.h"
#include "sha256.h"
#include <stdio.h>
#include <string.h>

int omega_t_chain_eval(const TChain *chain, int64_t x, int64_t *out) {
    if (!chain || !out) return -1;
    int64_t v = x;
    for (size_t i = 0; i < chain->n; ++i) {
        if (omega_t_eval(chain->steps[i].op, v, chain->steps[i].k, &v, NULL) != 0) return -1;
    }
    *out = v;
    return 0;
}

/* ---------------- interval analysis ---------------- */

static __int128 i128_min(__int128 a, __int128 b) { return a < b ? a : b; }
static __int128 i128_max(__int128 a, __int128 b) { return a > b ? a : b; }
static __int128 i128_abs(__int128 a) { return a < 0 ? -a : a; }

static int64_t word_bound_for_len(uint32_t n) {
    if (n >= TW_TRITS) return TW_MAX;
    int64_t p = 1;
    for (uint32_t i = 0; i < n; ++i) p *= 3;
    return (p - 1) / 2;
}

int omega_t_chain_intervals(const TChain *chain, int64_t bound, TStepFlags *flags) {
    if (!chain || !flags || bound < 0 || bound > TW_MAX) return -1;
    __int128 lo = -bound, hi = bound;
    for (size_t i = 0; i < chain->n; ++i) {
        const TStep *s = &chain->steps[i];
        __int128 nlo = lo, nhi = hi;
        bool elide_mulh = false;
        switch (s->op) {
            case TOP_TNEG: nlo = -hi; nhi = -lo; break;
            case TOP_TADD: nlo = lo + s->k; nhi = hi + s->k; break;
            case TOP_TSUB: nlo = lo - s->k; nhi = hi - s->k; break;
            case TOP_TMUL:
            case TOP_TSHL: {
                __int128 k = s->k;
                if (s->op == TOP_TSHL) { k = 1; for (int64_t j = 0; j < s->k; ++j) k *= 3; }
                nlo = i128_min(lo * k, hi * k);
                nhi = i128_max(lo * k, hi * k);
                /* |x| <= TW_MAX < 2^50, so the int64 product is exact whenever it fits. */
                elide_mulh = i128_max(i128_abs(lo), i128_abs(hi)) * i128_abs(k) < ((__int128)1 << 62);
                break;
            }
            case TOP_TSHR: {
                int64_t a = 0, b = 0;
                omega_t_eval(TOP_TSHR, (int64_t)lo, s->k, &a, NULL);
                omega_t_eval(TOP_TSHR, (int64_t)hi, s->k, &b, NULL);
                nlo = a; nhi = b;
                break;
            }
            case TOP_TSIGN:
            case TOP_TCMP: nlo = -1; nhi = 1; break;
            case TOP_TAND:
            case TOP_TOR:
            case TOP_TXOR: {
                int64_t m = (int64_t)i128_max(i128_abs(lo), i128_abs(hi));
                uint32_t n = omega_t_trit_len(m);
                uint32_t nk = omega_t_trit_len(s->k < 0 ? -s->k : s->k);
                int64_t b = word_bound_for_len(n > nk ? n : nk);
                nlo = -b; nhi = b;
                break;
            }
            default: return -1;
        }
        bool in_range = nlo >= -TW_MAX && nhi <= TW_MAX;
        flags[i].elide_clamp = in_range;
        flags[i].elide_mulh = elide_mulh;
        lo = i128_max(nlo, -TW_MAX);
        hi = i128_min(nhi, TW_MAX);
    }
    return 0;
}

/* ---------------- identity ---------------- */

int omega_t_program_compute_id(TProgram *prog) {
    if (!prog) return -1;
    OmegaObject ty, obj;
    if (omega_t_build_type_word(&ty) != 0) return -1;
    sha256_ctx ctx;
    sha256_init(&ctx);
    for (size_t i = 0; i < prog->chain.n; ++i) {
        const TStep *s = &prog->chain.steps[i];
        if (omega_t_build_op(&obj, s->op, &ty.id) != 0) return -1;
        sha256_update(&ctx, obj.id.bytes, OMEGA_ID_BYTES);
        if (!omega_t_op_is_unary(s->op)) {
            if (omega_t_build_value(&obj, &ty.id, s->k) != 0) return -1;
            sha256_update(&ctx, obj.id.bytes, OMEGA_ID_BYTES);
        }
    }
    if (omega_t_build_value(&obj, &ty.id, prog->domain_bound) != 0) return -1;
    sha256_update(&ctx, obj.id.bytes, OMEGA_ID_BYTES);
    sha256_final(&ctx, prog->program_id.bytes);
    return 0;
}

int omega_t_program_realize(TProgram *prog) {
    if (!prog) return -1;
    if (omega_t_a64_lower_chain(&prog->chain, prog->reps, prog->flags, &prog->real) != 0) return -1;
    prog->is_realized = true;
    return 0;
}

/* ---------------- T-V0 ---------------- */

typedef enum { IC_NONE, IC_ALU_RD, IC_ADDSUB_IMM, IC_BRANCH19, IC_BRANCH26, IC_RET } InsnClass;

static InsnClass classify(uint32_t w) {
    if (w == 0xD65F03C0u) return IC_RET;
    if ((w & 0x1F200000u) == 0x0B000000u) return IC_ALU_RD;   /* add/sub (shifted reg) */
    if ((w & 0x1F000000u) == 0x0A000000u) return IC_ALU_RD;   /* logical (shifted reg) */
    if ((w & 0x1F000000u) == 0x11000000u) return IC_ADDSUB_IMM;
    if ((w & 0x1FE00000u) == 0x1A800000u) return IC_ALU_RD;   /* conditional select */
    if ((w & 0x1F800000u) == 0x13000000u) return IC_ALU_RD;   /* bitfield */
    if ((w & 0x1F800000u) == 0x13800000u) return IC_ALU_RD;   /* extract */
    if ((w & 0x5FE00000u) == 0x1AC00000u) return IC_ALU_RD;   /* data-proc 2-source */
    if ((w & 0x5FE00000u) == 0x5AC00000u) return IC_ALU_RD;   /* data-proc 1-source */
    if ((w & 0x1F000000u) == 0x1B000000u) return IC_ALU_RD;   /* data-proc 3-source */
    if ((w & 0x1F800000u) == 0x12800000u) return IC_ALU_RD;   /* move wide */
    if ((w & 0xFF000010u) == 0x54000000u) return IC_BRANCH19; /* b.cond */
    if ((w & 0x7E000000u) == 0x34000000u) return IC_BRANCH19; /* cbz/cbnz */
    if ((w & 0xFC000000u) == 0x14000000u) return IC_BRANCH26; /* b */
    return IC_NONE;
}

/* Callee-saved, platform and link registers must never be written. */
static bool forbidden_rd(uint32_t rd) { return rd >= 18 && rd <= 30; }

static int v0_fail(VerifyReport *r, const char *fmt, size_t at, uint32_t w) {
    r->passed = false;
    r->fail_count++;
    snprintf(r->error_detail, sizeof(r->error_detail), fmt, at, w);
    return -1;
}

int omega_t_verify_v0(const RealizationObject *real, VerifyReport *report) {
    if (!real || !report) return -1;
    memset(report, 0, sizeof(*report));
    report->tier = VERIFY_TIER_V0;
    report->passed = true;
    if (real->code_len == 0 || real->code_len % 4 != 0 || real->code_len > sizeof(real->code_bytes)) {
        report->passed = false;
        report->fail_count++;
        snprintf(report->error_detail, sizeof(report->error_detail), "T-V0: bad code length %zu", real->code_len);
        return -1;
    }
    size_t n = real->code_len / 4;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *p = &real->code_bytes[4 * i];
        uint32_t w = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        report->check_count++;
        InsnClass c = classify(w);
        if (c == IC_NONE) return v0_fail(report, "T-V0: insn %zu (0x%08x) outside the ternary lowering vocabulary", i, w);
        if (c == IC_RET && i != n - 1) return v0_fail(report, "T-V0: RET at %zu (0x%08x) is not terminal", i, w);
        if (c != IC_RET && i == n - 1) return v0_fail(report, "T-V0: last insn %zu (0x%08x) is not RET", i, w);
        uint32_t rd = w & 0x1F, rn = (w >> 5) & 0x1F;
        if (c == IC_ALU_RD && forbidden_rd(rd)) return v0_fail(report, "T-V0: insn %zu (0x%08x) writes a reserved register", i, w);
        if (c == IC_ADDSUB_IMM) {
            bool sets_flags = (w >> 29) & 1;
            /* In the immediate form register 31 is SP, not ZR. */
            if (rn == 31 || (rd == 31 && !sets_flags) || forbidden_rd(rd)) {
                return v0_fail(report, "T-V0: insn %zu (0x%08x) touches SP or a reserved register", i, w);
            }
        }
        if (c == IC_BRANCH19 || c == IC_BRANCH26) {
            int64_t off = (c == IC_BRANCH19) ? (int64_t)((int32_t)(w << 8) >> 13) : (int64_t)((int32_t)(w << 6) >> 6);
            int64_t target = (int64_t)i + off;
            if (target < 0 || target >= (int64_t)n) {
                return v0_fail(report, "T-V0: branch at %zu (0x%08x) leaves the realization", i, w);
            }
        }
    }
    return 0;
}

/* ---------------- sampling ---------------- */

static uint64_t lcg(uint64_t *s) {
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return *s;
}

/* Deterministic domain sample: exhaustive core, edges, random fill. */
static size_t domain_sample(int64_t bound, int64_t *xs, size_t max) {
    size_t n = 0;
    int64_t core = bound < 3280 ? bound : 3280;
    for (int64_t x = -core; x <= core && n < max; ++x) xs[n++] = x;
    int64_t edges[] = { bound, -bound, bound - 1, -(bound - 1), bound / 2, -(bound / 2), bound / 3, -(bound / 3) };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]) && n < max; ++i) {
        if (edges[i] >= -bound && edges[i] <= bound) xs[n++] = edges[i];
    }
    uint64_t seed = 0x7E57ULL ^ (uint64_t)bound;
    for (size_t i = 0; i < 2000 && n < max; ++i) {
        xs[n++] = (int64_t)(lcg(&seed) % (uint64_t)(2 * bound + 1)) - bound;
    }
    return n;
}

#define SAMPLE_MAX 10000
static int64_t g_xs[SAMPLE_MAX];

/* ---------------- T-V1 ---------------- */

static int v1_mapped(const TProgram *prog, const TJit *jitp, VerifyReport *report) {
    memset(report, 0, sizeof(*report));
    report->tier = VERIFY_TIER_V1;
    report->passed = true;
    const TJit jit = *jitp;
    size_t n = domain_sample(prog->domain_bound, g_xs, SAMPLE_MAX);
    for (size_t i = 0; i < n; ++i) {
        int64_t want = 0;
        if (omega_t_chain_eval(&prog->chain, g_xs[i], &want) != 0) continue;
        int64_t got = (int64_t)omega_t_jit_call(&jit, (uint64_t)g_xs[i], 0);
        report->check_count++;
        if (got != want) {
            report->passed = false;
            report->fail_count++;
            snprintf(report->error_detail, sizeof(report->error_detail),
                     "T-V1: f(%lld) native=%lld model=%lld", (long long)g_xs[i], (long long)got, (long long)want);
            break;
        }
    }
    return report->passed ? 0 : -1;
}

int omega_t_verify_v1(const TProgram *prog, VerifyReport *report) {
    if (!prog || !report || !prog->is_realized) return -1;
    TJit jit;
    if (omega_t_jit_open(&prog->real, &jit) != 0) return -1;
    int rc = v1_mapped(prog, &jit, report);
    omega_t_jit_close(&jit);
    return rc;
}

/* ---------------- T-V2 ---------------- */

static bool step_is_odd(const TStep *s) {
    switch (s->op) {
        case TOP_TNEG: case TOP_TMUL: case TOP_TSHL: case TOP_TSHR:
        case TOP_TSIGN: case TOP_TXOR: return true;
        case TOP_TADD: case TOP_TSUB: case TOP_TCMP: return s->k == 0;
        default: return false;
    }
}

/* +1 monotone non-decreasing, -1 antitone, 0 no declared direction. */
static int step_direction(const TStep *s) {
    switch (s->op) {
        case TOP_TNEG: return -1;
        case TOP_TADD: case TOP_TSUB: case TOP_TSHL: case TOP_TSHR:
        case TOP_TSIGN: case TOP_TCMP: return 1;
        case TOP_TMUL: return s->k < 0 ? -1 : 1;
        default: return 0;
    }
}

static int v2_fail(VerifyReport *r, const char *what, int64_t x, int64_t y) {
    r->passed = false;
    r->fail_count++;
    snprintf(r->error_detail, sizeof(r->error_detail), "T-V2 %s violated at x=%lld (f=%lld)",
             what, (long long)x, (long long)y);
    return -1;
}

/* Walks x from lo to hi over `count` evenly spaced points (inclusive). */
static int v2_walk(const TJit *jit, int64_t lo, int64_t hi, size_t count,
                   bool odd, int dir, VerifyReport *report) {
    int64_t prev_y = 0;
    for (size_t i = 0; i < count; ++i) {
        int64_t x = lo + (int64_t)(((__int128)(hi - lo) * (__int128)i) / (__int128)(count - 1));
        int64_t y = (int64_t)omega_t_jit_call(jit, (uint64_t)x, 0);
        report->check_count++;
        if (y < -TW_MAX || y > TW_MAX) return v2_fail(report, "symmetric range", x, y);
        if (odd) {
            int64_t yn = (int64_t)omega_t_jit_call(jit, (uint64_t)-x, 0);
            report->check_count++;
            if (yn != -y) return v2_fail(report, "odd symmetry f(-x) == -f(x)", x, y);
        }
        if (i > 0 && dir != 0) {
            report->check_count++;
            if ((dir > 0 && y < prev_y) || (dir < 0 && y > prev_y)) {
                return v2_fail(report, dir > 0 ? "monotonicity" : "antitonicity", x, y);
            }
        }
        prev_y = y;
    }
    return 0;
}

static int v2_mapped(const TProgram *prog, const TJit *jit, VerifyReport *report) {
    memset(report, 0, sizeof(*report));
    report->tier = VERIFY_TIER_V2;
    report->passed = true;
    bool odd = true;
    int dir = 1;
    for (size_t i = 0; i < prog->chain.n; ++i) {
        odd = odd && step_is_odd(&prog->chain.steps[i]);
        dir *= step_direction(&prog->chain.steps[i]);
    }
    int64_t bound = prog->domain_bound;
    int64_t core = bound < 3280 ? bound : 3280;
    /* Unit-step walk over the core, then a coarse walk across the whole domain. */
    if (v2_walk(jit, -core, core, (size_t)(2 * core + 1), odd, dir, report) != 0) return -1;
    if (bound > core && v2_walk(jit, -bound, bound, 4097, odd, dir, report) != 0) return -1;
    return 0;
}

int omega_t_verify_v2(const TProgram *prog, VerifyReport *report) {
    if (!prog || !report || !prog->is_realized) return -1;
    TJit jit;
    if (omega_t_jit_open(&prog->real, &jit) != 0) return -1;
    int rc = v2_mapped(prog, &jit, report);
    omega_t_jit_close(&jit);
    return rc;
}

int omega_t_verify_pipeline(const TProgram *prog, VerifyReport *report) {
    if (!prog || !report) return -1;
    if (omega_t_verify_v0(&prog->real, report) != 0) return -1;
    if (omega_t_verify_v1(prog, report) != 0) return -1;
    if (omega_t_verify_v2(prog, report) != 0) return -1;
    return 0;
}

int omega_t_verify_pipeline_mapped(const TProgram *prog, const TJit *jit, VerifyReport *report) {
    if (!prog || !jit || !report) return -1;
    if (omega_t_verify_v0(&prog->real, report) != 0) return -1;
    if (v1_mapped(prog, jit, report) != 0) return -1;
    if (v2_mapped(prog, jit, report) != 0) return -1;
    return 0;
}
