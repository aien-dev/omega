/* =========================================================================
 * TERNARY SEMANTICS EXPERIMENT - GATES AND MEASUREMENT HARNESS
 * Separate binary from omegatool; the canonical binary tool is untouched.
 * ========================================================================= */

#include "omega_ternary.h"
#include "omega_ternary_a64.h"
#include "omega_ternary_measure.h"
#include "omega_canonical.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

static void gate(const char *id, const char *what, bool ok, const char *detail) {
    printf("[%s] %s %s%s%s\n", ok ? "PASS" : "FAIL", id, what,
           detail && detail[0] ? " :: " : "", detail ? detail : "");
    if (ok) g_pass++; else g_fail++;
}

static uint64_t lcg(uint64_t *s) {
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return *s;
}

static int64_t rand_word(uint64_t *s, uint32_t max_len) {
    uint32_t len = (uint32_t)(lcg(s) >> 40) % max_len + 1;
    int64_t v = 0;
    for (uint32_t i = 0; i < len; ++i) v = v * 3 + (int64_t)((lcg(s) >> 33) % 3) - 1;
    return v;
}

static const TernaryOp OPS[TOP_COUNT] = {
    TOP_TNEG, TOP_TADD, TOP_TSUB, TOP_TMUL, TOP_TAND, TOP_TOR,
    TOP_TXOR, TOP_TCMP, TOP_TSIGN, TOP_TSHL, TOP_TSHR
};

static uint64_t enc(TernaryRep rep, int64_t v) {
    return rep == TREP_PLANES ? omega_t_to_planes(v) : (uint64_t)v;
}

/* ---------------- TG1: brute-force model agreement ---------------- */
static void tg1_models(void) {
    size_t checks = 0;
    char err[256] = "";
    size_t fails = omega_t_selfcheck_models(&checks, err, sizeof(err));
    char d[320];
    snprintf(d, sizeof(d), "%zu checks, %zu mismatches%s%s", checks, fails, fails ? "; first: " : "", fails ? err : "");
    gate("TG1", "fast model == brute-force trit model", fails == 0, d);
}

/* ---------------- TG2: OMG1 canonical identity ---------------- */
static void tg2_omg1(void) {
    OmegaObject ty, v1, v2, v3, op;
    bool ok = omega_t_build_type_word(&ty) == 0;
    ok = ok && omega_t_build_value(&v1, &ty.id, 123456789) == 0;
    /* Same meaning reached through the PLANES realization. */
    int64_t via_planes = omega_t_from_planes(omega_t_to_planes(123456789));
    ok = ok && omega_t_build_value(&v2, &ty.id, via_planes) == 0;
    bool same_id = ok && memcmp(v1.id.bytes, v2.id.bytes, 32) == 0;

    uint8_t buf[4096];
    size_t len = 0;
    bool magic = omega_t_canonical_encode(&v1, buf, sizeof(buf), &len) == 0 &&
                 buf[0] == 0x4F && buf[1] == 0x4D && buf[2] == 0x47 && buf[3] == 0x31;

    /* The OMG0 encoder over the same object must yield a different identity. */
    OmegaObject v0 = v1;
    bool omg0_differs = omega_compute_semantic_id(&v0) == 0 && memcmp(v0.id.bytes, v1.id.bytes, 32) != 0;

    /* Negation is a digit flip: -v decodes to -v and has a distinct id. */
    int64_t back = 0;
    bool neg_ok = omega_t_build_value(&v3, &ty.id, -123456789) == 0 &&
                  omega_t_value_decode(&v3, &back) == 0 && back == -123456789 &&
                  memcmp(v3.id.bytes, v1.id.bytes, 32) != 0;

    /* Non-canonical forms are refused. */
    OmegaObject bad = v1;
    bad.payload[OMEGA_ID_BYTES + 2] = 243;
    bool refuse_tryte = omega_t_canonical_encode(&bad, buf, sizeof(buf), &len) != 0;
    bad = v1;
    bad.payload[bad.payload_len - 1] = 0; /* padding trits become -1 */
    bool refuse_pad = omega_t_canonical_encode(&bad, buf, sizeof(buf), &len) != 0;

    /* Round trip and injectivity over a sample of values. */
    uint64_t seed = 99;
    bool roundtrip = true;
    size_t collisions = 0;
    static uint8_t ids[4000][32];
    static int64_t vals[4000];
    for (size_t i = 0; i < 4000; ++i) {
        int64_t v = (i < 2) ? (i ? TW_MAX : -TW_MAX) : rand_word(&seed, 32);
        OmegaObject o;
        int64_t d = 0;
        if (omega_t_build_value(&o, &ty.id, v) != 0 || omega_t_value_decode(&o, &d) != 0 || d != v) roundtrip = false;
        memcpy(ids[i], o.id.bytes, 32);
        vals[i] = v;
    }
    for (size_t i = 0; i < 4000; ++i)
        for (size_t j = i + 1; j < 4000; ++j)
            if ((memcmp(ids[i], ids[j], 32) == 0) != (vals[i] == vals[j])) collisions++;
    bool op_ok = omega_t_build_op(&op, TOP_TADD, &ty.id) == 0;

    char d[256];
    snprintf(d, sizeof(d), "planes-realized id equal=%d magic=%d omg0-differs=%d neg=%d refuse(tryte=%d,pad=%d) roundtrip=%d id-vs-value mismatches=%zu op=%d",
             same_id, magic, omg0_differs, neg_ok, refuse_tryte, refuse_pad, roundtrip, collisions, op_ok);
    gate("TG2", "OMG1 canonical encoding and identity", same_id && magic && omg0_differs && neg_ok &&
         refuse_tryte && refuse_pad && roundtrip && collisions == 0 && op_ok, d);
}

/* ---------------- TG3: extension encoders vs oracle fixture ---------------- */
static void tg3_encoders(const char *fixture) {
    uint32_t words[64];
    size_t n = omega_t_a64_encoder_samples(words, 64);
    FILE *f = fopen(fixture, "r");
    size_t matched = 0, total = 0;
    bool ok = f != NULL;
    if (f) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '\n') continue;
            uint32_t w = (uint32_t)strtoul(line, NULL, 16);
            if (total < n && words[total] == w) matched++;
            total++;
        }
        fclose(f);
    }
    char d[128];
    snprintf(d, sizeof(d), "%zu/%zu words match (emitted %zu)", matched, total, n);
    gate("TG3", "A64 extension encodings == assembler oracle fixture", ok && total == n && matched == n, d);
}

/* ---------------- TG4: per-op native differential ---------------- */
static bool diff_program(TernaryOp op, TernaryRep rep, int64_t k, size_t *checks, char *err, size_t err_len) {
    RealizationObject real;
    uint32_t body = 0, pro = 0;
    if (omega_t_a64_lower_op(op, rep, k, &real, &body, &pro) != 0) {
        snprintf(err, err_len, "%s rep=%d k=%lld: lowering refused", omega_t_op_name(op), rep, (long long)k);
        return false;
    }
    TJit jit;
    if (omega_t_jit_open(&real, &jit) != 0) { snprintf(err, err_len, "jit"); return false; }
    bool shift = (op == TOP_TSHL || op == TOP_TSHR);
    int64_t edges[] = { 0, 1, -1, 2, -2, 4, -4, 13, -13, 40, -40, TW_MAX, -TW_MAX, TW_MAX - 1, -(TW_MAX - 1),
                        TW_MAX / 3, -(TW_MAX / 3), 3486784401LL, -3486784401LL, 30941100000LL };
    size_t ne = sizeof(edges) / sizeof(edges[0]);
    uint64_t seed = 0xABCDEF0011ULL + (uint64_t)op * 7 + (uint64_t)k;
    bool ok = true;
    size_t n = shift ? 3000 : 6000;
    for (size_t i = 0; i < n && ok; ++i) {
        int64_t a, b;
        if (i < ne * ne) { a = edges[i / ne]; b = edges[i % ne]; }
        else if (i < ne * ne + 81 * 81) { size_t j = i - ne * ne; a = (int64_t)(j / 81) - 40; b = (int64_t)(j % 81) - 40; }
        else {
            a = rand_word(&seed, 32);
            b = (lcg(&seed) & 1) ? rand_word(&seed, 32) : rand_word(&seed, 6);
        }
        if (shift) b = k;
        if (omega_t_op_is_unary(op)) b = 0;
        int64_t want = 0;
        if (omega_t_eval(op, a, b, &want, NULL) != 0) continue;
        uint64_t got = omega_t_jit_call(&jit, enc(rep, a), shift ? 0 : enc(rep, b));
        (*checks)++;
        if (got != enc(rep, want)) {
            snprintf(err, err_len, "%s rep=%d k=%lld: f(%lld,%lld) native=0x%016" PRIx64 " model=%lld",
                     omega_t_op_name(op), rep, (long long)k, (long long)a, (long long)b, got, (long long)want);
            ok = false;
        }
    }
    omega_t_jit_close(&jit);
    return ok;
}

static void tg4_lowering(void) {
    size_t checks = 0, programs = 0;
    char err[256] = "";
    bool ok = true;
    for (size_t o = 0; o < TOP_COUNT && ok; ++o) {
        for (int rep = 0; rep < TREP_COUNT && ok; ++rep) {
            TernaryOp op = OPS[o];
            if (!omega_t_a64_supported(op, (TernaryRep)rep)) continue;
            int kmin = 0, kmax = 0;
            if (op == TOP_TSHL) { kmin = 1; kmax = 8; }
            if (op == TOP_TSHR) { kmin = 1; kmax = 31; }
            for (int k = kmin; k <= kmax && ok; ++k) {
                ok = diff_program(op, (TernaryRep)rep, k, &checks, err, sizeof(err));
                programs++;
            }
        }
    }
    char d[400];
    snprintf(d, sizeof(d), "%zu programs, %zu native executions%s%s", programs, checks, ok ? "" : "; ", ok ? "" : err);
    gate("TG4", "every op x realization: native AArch64 == model", ok, d);
}

/* ---------------- TG5: cost model == ptrace-measured dynamic count ---------------- */
static void random_chain(uint64_t *s, TChain *c, TernaryRep *reps, TStepFlags *fl) {
    c->n = (size_t)(lcg(s) >> 40) % 3 + 1;
    for (size_t i = 0; i < c->n; ++i) {
        TernaryOp op;
        do { op = OPS[(lcg(s) >> 40) % TOP_COUNT]; } while (0);
        TernaryRep rep = (TernaryRep)((lcg(s) >> 41) & 1);
        if (!omega_t_a64_supported(op, rep)) rep = TREP_INT;
        int64_t k = rand_word(s, 8);
        if (op == TOP_TSHL) k = (int64_t)((lcg(s) >> 40) % 8) + 1;
        if (op == TOP_TSHR) k = (int64_t)((lcg(s) >> 40) % 12) + 1;
        c->steps[i].op = op;
        c->steps[i].k = k;
        reps[i] = rep;
        fl[i].elide_clamp = false;
        fl[i].elide_mulh = false;
    }
}

static void tg5_cost_model(void) {
    uint64_t seed = 0x5EED5EEDULL;
    size_t runs = 0, exact = 0, wrong_value = 0;
    char err[256] = "";
    for (size_t t = 0; t < 120; ++t) {
        TChain c;
        TernaryRep reps[TCHAIN_MAX_STEPS];
        TStepFlags fl[TCHAIN_MAX_STEPS];
        random_chain(&seed, &c, reps, fl);
        RealizationObject real;
        if (omega_t_a64_lower_chain(&c, reps, fl, &real) != 0) continue;
        for (size_t j = 0; j < 3; ++j) {
            int64_t x = rand_word(&seed, (uint32_t)(j == 0 ? 3 : 20));
            int64_t want = x;
            for (size_t i = 0; i < c.n; ++i) omega_t_eval(c.steps[i].op, want, c.steps[i].k, &want, NULL);
            uint64_t got = 0, insns = 0;
            if (omega_t_measure_dyn(&real, (uint64_t)x, 0, &got, &insns) != 0) continue;
            runs++;
            uint64_t pred = omega_t_a64_chain_dyn(&c, reps, fl, x);
            if ((int64_t)got != want) wrong_value++;
            if (pred == insns) exact++;
            else if (!err[0]) snprintf(err, sizeof(err), "chain n=%zu op0=%s rep0=%d x=%lld predicted=%" PRIu64 " measured=%" PRIu64,
                                       c.n, omega_t_op_name(c.steps[0].op), reps[0], (long long)x, pred, insns);
        }
    }
    char d[400];
    snprintf(d, sizeof(d), "%zu/%zu traced runs predicted exactly, %zu wrong values%s%s", exact, runs, wrong_value,
             err[0] ? "; first miss: " : "", err);
    gate("TG5", "lowering cost model == ptrace dynamic instruction count", runs > 200 && exact == runs && wrong_value == 0, d);
}

int main(int argc, char **argv) {
    const char *fixture = "tests/ternary/a64_encoder_expected.txt";
    if (argc > 1 && strcmp(argv[1], "--run-gates") == 0) {
        tg1_models();
        tg2_omg1();
        tg3_encoders(fixture);
        tg4_lowering();
        tg5_cost_model();
        printf("TERNARY GATES: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail == 0 ? 0 : 1;
    }
    fprintf(stderr, "usage: %s --run-gates\n", argv[0]);
    return 2;
}
