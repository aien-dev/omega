/* DUAL-1b tests: digest-bound traces, deterministic replay, pre-registered
 * measures (evidence/DUAL/1b-replay/receipt.md) and the negative control.
 * Every input is UNCALIBRATED (calibrated = 0): every replayed state must be
 * UNCALIBRATED and no price here may influence production. */
#include "dual_fixtures.h"
#include "dual_traces.h"
#include "sha256.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_ST(expr, want) do { RxDualStatus s_ = (expr); g_checks++; if (s_ != (want)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s -> %d, want %d\n", __FILE__, __LINE__, #expr, (int)s_, (int)(want)); } } while (0)

/* Golden digests (D2): committed after the first run, checked across processes ever after. */
static const char *GOLDEN_TRACE_2  = "f9c3944ab952dd5e2beefebbb2499f98ec7be28157b571a47e406ca40e4d93fb";
static const char *GOLDEN_REPLAY_2 = "46345146d1213fdd2888fd0ace3f7ab31287565fd42adf950ad58edeb11bf5ae";
static const char *GOLDEN_TRACE_4  = "0a463faef33ab2a59da6c854eabfa2ec9e189f3e5e6b00f07c773e490e52edd5";
static const char *GOLDEN_REPLAY_4 = "64a30ce77dc7decfedae15c6c8e8e66fa75693df049916deab7ca2abbfaaa888";

static RxDualTrace T, T2;
static RxDualReplay R, R2;
static RxDualDigest BC, BC2;
static int g_ref_fail;      /* any pre-registered measure failed for the reference controller */
static int g_neg_as_declared = 1;

static void hex(const uint8_t *b, size_t n, char *out) { static const char *h = "0123456789abcdef"; for (size_t i = 0; i < n; i++) { out[2*i] = h[b[i] >> 4]; out[2*i+1] = h[b[i] & 15]; } out[2*n] = 0; }
static void dhex(const RxDualDigest *d, char *out) { hex(d->b, 32, out); }

/* A measure result line for the receipt. */
static void measure(const char *scenario, const char *id, int pass, const char *detail, int reference)
{
    printf("MEASURE %-14s %-4s %s  %s\n", scenario, id, pass ? "PASS" : "FAIL", detail);
    if (reference) { CHECK(pass); if (!pass) g_ref_fail = 1; }
}

static const RxDualController *ref_ctl(void) { static RxDualController c; static int init; if (!init) { c = fx_ctl(1, 0.1, 0.05, 2.0, 10.0, 4); init = 1; } return &c; }
static const RxDualController *hostile_ctl(void) { static RxDualController c; static int init; if (!init) { c = fx_ctl(1, 5.0, 0.0, 2.0, 10.0, 4); init = 1; } return &c; }

/* Run a chain and emit the digest line; returns status. */
static RxDualStatus run(const char *name, const RxDualTrace *tr, const RxDualController *ctl, const RxDualResource *res,
                        double budget, const RxDualDigest *bc, RxDualReplay *out)
{
    char th[65], ch[65], rh[65];
    RxDualStatus st = rx_dual_replay_run(tr, ctl, res, RX_DUAL_CLASS_SOFT, budget, bc, NULL, out);
    if (st != RX_DUAL_OK) { printf("REPLAY %-14s REFUSED status %d\n", name, (int)st); return st; }
    dhex(&out->trace, th); dhex(&out->controller_id, ch); dhex(&out->replay, rh);
    printf("REPLAY %-14s n=%u trace=%s controller=%s replay=%s final_lambda=%.9g\n", name, out->n, th, ch, rh, out->lambda[out->n - 1]);
    return st;
}

/* M1, M7, M8 on any chain; D1 determinism. */
static void common_measures(const char *name, const RxDualTrace *tr, const RxDualController *ctl, const RxDualResource *res,
                            double budget, const RxDualDigest *bc, RxDualReplay *rp, int reference)
{
    char d[160]; int ok; uint32_t rl, re; double lmax = 0;
    CHECK_ST(rx_dual_controller_lambda_max(ctl, res->resource_id, &lmax), RX_DUAL_OK);
    ok = 1;
    for (uint32_t i = 0; i < rp->n; i++) if (!isfinite(rp->lambda[i]) || rp->lambda[i] < 0.0 || rp->lambda[i] > lmax) ok = 0;
    { double mx = 0; for (uint32_t i = 0; i < rp->n; i++) if (rp->lambda[i] > mx) mx = rp->lambda[i]; snprintf(d, sizeof d, "lambda_max=%g max_lambda=%.9g", lmax, mx); }
    measure(name, "M1", ok, d, 1);
    ok = 1;
    for (uint32_t i = 0; i < rp->n; i++) if (tr->t[i].calibrated == 0 && rp->state[i] != (tr->t[i].regime_change ? RX_DUAL_LAMBDA_FROZEN : RX_DUAL_LAMBDA_UNCALIBRATED)) ok = 0;
    for (uint32_t i = 0; i < rp->n; i++) if (rp->state[i] == RX_DUAL_LAMBDA_FRESH) ok = 0;
    measure(name, "M7", ok, "every calibrated=0 tick is UNCALIBRATED (FROZEN exactly where regime_change = 1); none FRESH", 1);
    rl = rx_dual_reversals(rp->lambda, 0, rp->n);
    { static double est[RX_DUAL_TRACE_MAX_TICKS]; for (uint32_t i = 0; i < tr->n_ticks; i++) est[i] = tr->t[i].estimate; re = rx_dual_reversals(est, 0, tr->n_ticks); }
    snprintf(d, sizeof d, "R(lambda)=%u R(estimate)=%u bound=%u", rl, re, re + 1);
    measure(name, "M8", rl <= re + 1, d, 1);
    { RxDualStatus st = rx_dual_replay_run(tr, ctl, res, RX_DUAL_CLASS_SOFT, budget, bc, NULL, &R2);
      measure(name, "D1", st == RX_DUAL_OK && rx_dual_digest_eq(&rp->replay, &R2.replay), "second replay gives identical replay digest", 1); }
    (void)reference;
}

static void tail_zero(const char *name, const RxDualReplay *rp)
{
    char d[96]; uint32_t r = rx_dual_reversals(rp->lambda, rp->n - 50, rp->n);
    snprintf(d, sizeof d, "R(lambda) over final 50 ticks = %u", r);
    measure(name, "M2", r == 0, d, 1);
}
static void monotone_from(const char *name, const RxDualReplay *rp, uint32_t from)
{
    char d[96]; int ok = 1; uint32_t bad = 0;
    for (uint32_t i = from + 1; i < rp->n; i++) if (rp->lambda[i] < rp->lambda[i - 1] - 1e-12 * (1.0 + rp->lambda[i - 1])) { ok = 0; if (!bad) bad = i; }
    if (ok) snprintf(d, sizeof d, "non-decreasing from tick %u; no violation", from);
    else snprintf(d, sizeof d, "non-decreasing from tick %u; first violation at tick %u", from, bad);
    measure(name, "M4", ok, d, 1);
}

static void scenarios(void)
{
    const RxDualController *ctl = ref_ctl();
    RxDualResource res = fx_res(1, RX_DUAL_UNIT_BYTES, DUAL_SCALE);
    char d[200];
    /* 1 constant low */
    CHECK_ST(dual_trace_scenario(1, 0, 0, &T), RX_DUAL_OK);
    CHECK_ST(run("1-constant-low", &T, ctl, &res, DUAL_BUDGET, &BC, &R), RX_DUAL_OK);
    common_measures("1-constant-low", &T, ctl, &res, DUAL_BUDGET, &BC, &R, 1); tail_zero("1-constant-low", &R);
    CHECK(R.lambda[R.n - 1] == 0.0);
    /* 2 binding (+ golden, tamper, calibrated variant, hostile class) */
    CHECK_ST(dual_trace_scenario(2, 0, 0, &T), RX_DUAL_OK);
    CHECK_ST(run("2-binding", &T, ctl, &res, DUAL_BUDGET, &BC, &R), RX_DUAL_OK);
    common_measures("2-binding", &T, ctl, &res, DUAL_BUDGET, &BC, &R, 1); tail_zero("2-binding", &R); monotone_from("2-binding", &R, 0);
    snprintf(d, sizeof d, "final lambda %.9g vs fixed point 0.6", R.lambda[R.n - 1]);
    measure("2-binding", "A", fabs(R.lambda[R.n - 1] - 0.6) < 1e-3, d, 1);
    { char th[65], rh[65]; dhex(&R.trace, th); dhex(&R.replay, rh);
      measure("2-binding", "D2", strcmp(th, GOLDEN_TRACE_2) == 0 && strcmp(rh, GOLDEN_REPLAY_2) == 0, "trace and replay digests equal the committed golden constants", 1); }
    { static uint8_t enc[RX_DUAL_TRACE_ENCODED_MAX]; size_t len; RxDualDigest d0 = R.trace, r0 = R.replay;
      CHECK_ST(rx_dual_encode_trace(&T, enc, sizeof enc, &len), RX_DUAL_OK);
      CHECK(len == RX_DUAL_TRACE_HEADER_ENCODED + 200u * RX_DUAL_TRACE_TICK_ENCODED);
      CHECK_ST(rx_dual_decode_trace(enc, len, &T2), RX_DUAL_OK);
      CHECK(memcmp(&T2, &T, sizeof T) == 0);
      enc[RX_DUAL_TRACE_HEADER_ENCODED + 17u * RX_DUAL_TRACE_TICK_ENCODED + 8u] ^= 0x01;   /* lowest mantissa bit of estimate, tick 17 */
      CHECK_ST(rx_dual_decode_trace(enc, len, &T2), RX_DUAL_OK);
      CHECK(T2.t[17].estimate != T.t[17].estimate && T2.t[16].estimate == T.t[16].estimate);
      CHECK_ST(rx_dual_replay_run(&T2, ctl, &res, RX_DUAL_CLASS_SOFT, DUAL_BUDGET, &BC, NULL, &R2), RX_DUAL_OK);
      measure("2-binding", "D3", !rx_dual_digest_eq(&R2.trace, &d0) && !rx_dual_digest_eq(&R2.replay, &r0), "one flipped bit changes trace digest and replay digest", 1);
      /* decoder refusals */
      enc[0] = 2; CHECK_ST(rx_dual_decode_trace(enc, len, &T2), RX_DUAL_ERR_KIND); enc[0] = 8;
      enc[1] = 2; CHECK_ST(rx_dual_decode_trace(enc, len, &T2), RX_DUAL_ERR_ENCODING); enc[1] = 1;
      CHECK_ST(rx_dual_decode_trace(enc, len - 1, &T2), RX_DUAL_ERR_ENCODING);
      CHECK_ST(rx_dual_decode_trace(enc, len + 1, &T2), RX_DUAL_ERR_ENCODING); }
    { RxDualDigest cal = fx_dig(0x44); int same = 1, fresh = 1;
      T2 = T; for (uint32_t i = 0; i < T2.n_ticks; i++) T2.t[i].calibrated = 1;
      CHECK_ST(rx_dual_replay_run(&T2, ctl, &res, RX_DUAL_CLASS_SOFT, DUAL_BUDGET, &BC, &cal, &R2), RX_DUAL_OK);
      for (uint32_t i = 0; i < R2.n; i++) { if (R2.lambda[i] != R.lambda[i]) same = 0; if (R2.state[i] != RX_DUAL_LAMBDA_FRESH) fresh = 0; }
      measure("2-binding", "D4", same && fresh && !rx_dual_digest_eq(&R2.replay, &R.replay), "calibrated=1 (stand-in digest, format exercise): same lambdas, FRESH, different replay digest", 1);
      CHECK_ST(rx_dual_replay_run(&T2, ctl, &res, RX_DUAL_CLASS_SOFT, DUAL_BUDGET, &BC, NULL, &R2), RX_DUAL_ERR_CALIBRATION); }
    { RxDualStatus a = rx_dual_replay_run(&T, ctl, &res, RX_DUAL_CLASS_INVARIANT, DUAL_BUDGET, &BC, NULL, &R2);
      RxDualStatus b = rx_dual_replay_run(&T, ctl, &res, RX_DUAL_CLASS_UNDECLARED, DUAL_BUDGET, &BC, NULL, &R2);
      measure("2-binding", "H1", a == RX_DUAL_ERR_CLASS && b == RX_DUAL_ERR_CLASS, "INVARIANT and UNDECLARED class refused by the replay entry point", 1); }
    { RxDualController m = *ctl; m.eta = 0.2; RxDualDigest c1, c2; rx_dual_digest_controller(ctl, &c1); rx_dual_digest_controller(&m, &c2);
      CHECK(!rx_dual_digest_eq(&c1, &c2)); }
    /* 3 step */
    CHECK_ST(dual_trace_scenario(3, 0, 0, &T), RX_DUAL_OK);
    CHECK_ST(run("3-step", &T, ctl, &res, DUAL_BUDGET, &BC, &R), RX_DUAL_OK);
    common_measures("3-step", &T, ctl, &res, DUAL_BUDGET, &BC, &R, 1); tail_zero("3-step", &R); monotone_from("3-step", &R, 100);
    CHECK(R.lambda[99] == 0.0 && R.lambda[100] > 0.0);
    /* 4 square: reference and hostile */
    CHECK_ST(dual_trace_scenario(4, 0, 0, &T), RX_DUAL_OK);
    CHECK_ST(run("4-square", &T, ctl, &res, DUAL_BUDGET, &BC, &R), RX_DUAL_OK);
    common_measures("4-square", &T, ctl, &res, DUAL_BUDGET, &BC, &R, 1);
    { uint32_t r = rx_dual_reversals(R.lambda, 0, R.n); double pp = rx_dual_peak_to_peak(R.lambda, 150, 200);
      snprintf(d, sizeof d, "R(lambda)=%u bound 8", r); measure("4-square", "M3a", r <= 8, d, 1);
      snprintf(d, sizeof d, "peak-to-peak over t 150..199 = %.9g bound 2.5 (analytic 1.156)", pp); measure("4-square", "M3b", pp <= 2.5, d, 1);
      { char th[65], rh[65]; dhex(&R.trace, th); dhex(&R.replay, rh);
        measure("4-square", "D2", strcmp(th, GOLDEN_TRACE_4) == 0 && strcmp(rh, GOLDEN_REPLAY_4) == 0, "trace and replay digests equal the committed golden constants", 1); } }
    { const RxDualController *h = hostile_ctl(); uint32_t r, rl, re; double pp, mx = 0; int ok;
      static double est[RX_DUAL_TRACE_MAX_TICKS];
      CHECK_ST(run("4-square-HOST", &T, h, &res, DUAL_BUDGET, &BC, &R2), RX_DUAL_OK);
      ok = 1; for (uint32_t i = 0; i < R2.n; i++) { if (!isfinite(R2.lambda[i]) || R2.lambda[i] < 0.0 || R2.lambda[i] > 10.0) ok = 0; if (R2.lambda[i] > mx) mx = R2.lambda[i]; }
      snprintf(d, sizeof d, "lambda_max=10 max_lambda=%.9g (hostile: declared PASS)", mx); measure("4-square-HOST", "M1", ok, d, 0); if (!ok) g_neg_as_declared = 0;
      r = rx_dual_reversals(R2.lambda, 0, R2.n); snprintf(d, sizeof d, "R(lambda)=%u bound 8 (hostile: declared PASS)", r); measure("4-square-HOST", "M3a", r <= 8, d, 0); if (r > 8) g_neg_as_declared = 0;
      pp = rx_dual_peak_to_peak(R2.lambda, 150, 200); snprintf(d, sizeof d, "peak-to-peak over t 150..199 = %.9g bound 2.5 (hostile: declared FAIL)", pp);
      measure("4-square-HOST", "M3b", pp <= 2.5, d, 0); if (pp <= 2.5) g_neg_as_declared = 0;
      for (uint32_t i = 0; i < T.n_ticks; i++) est[i] = T.t[i].estimate;
      rl = rx_dual_reversals(R2.lambda, 0, R2.n); re = rx_dual_reversals(est, 0, T.n_ticks);
      snprintf(d, sizeof d, "R(lambda)=%u R(estimate)=%u bound=%u (hostile: declared PASS)", rl, re, re + 1); measure("4-square-HOST", "M8", rl <= re + 1, d, 0); if (rl > re + 1) g_neg_as_declared = 0;
      printf("NEGATIVE-CONTROL hostile eta=5 rho=0 on square wave: gate verdict for this configuration = %s (expected FAIL)\n", pp <= 2.5 ? "PASS (SURPRISE)" : "FAIL"); }
    /* 5/6 competing: one controller n = 2 */
    { RxDualController c2; RxDualResource rm = fx_res(1, RX_DUAL_UNIT_BYTES, DUAL_SCALE), rl = fx_res(2, RX_DUAL_UNIT_BYTES, DUAL_SCALE); double lm, ll;
      memset(&c2, 0, sizeof c2); c2.eta = 0.1; c2.rho = 0.05; c2.k_sigma = 2.0; c2.max_age = 4; c2.cadence = 1; c2.n = 2;
      c2.resource_id[0] = 1; c2.resource_id[1] = 2; c2.lambda_max[0] = 10.0; c2.lambda_max[1] = 10.0;
      CHECK_ST(dual_trace_scenario(5, 0, 0, &T), RX_DUAL_OK);
      CHECK_ST(run("5-memory", &T, &c2, &rm, DUAL_BUDGET, &BC, &R), RX_DUAL_OK);
      common_measures("5-memory", &T, &c2, &rm, DUAL_BUDGET, &BC, &R, 1); tail_zero("5-memory", &R); lm = R.lambda[R.n - 1];
      CHECK_ST(dual_trace_scenario(6, 0, 0, &T), RX_DUAL_OK);
      CHECK_ST(run("6-latency", &T, &c2, &rl, DUAL_BUDGET, &BC, &R), RX_DUAL_OK);
      common_measures("6-latency", &T, &c2, &rl, DUAL_BUDGET, &BC, &R, 1); tail_zero("6-latency", &R); ll = R.lambda[R.n - 1];
      snprintf(d, sizeof d, "final lambda memory %.9g (> 0), latency %.9g (= 0)", lm, ll); measure("5/6-competing", "M9", lm > 0.0 && ll == 0.0, d, 1); }
    /* 7 capacity reduction: chain A budget 5000, chain B budget 3000 (new contract) */
    CHECK_ST(dual_trace_scenario(7, 'A', 0, &T), RX_DUAL_OK);
    CHECK_ST(run("7A-capacity", &T, ctl, &res, DUAL_BUDGET, &BC, &R), RX_DUAL_OK);
    common_measures("7A-capacity", &T, ctl, &res, DUAL_BUDGET, &BC, &R, 1); CHECK(R.lambda[R.n - 1] == 0.0);
    CHECK_ST(dual_trace_scenario(7, 'B', 0, &T), RX_DUAL_OK);
    CHECK_ST(run("7B-capacity", &T, ctl, &res, 3000.0, &BC2, &R), RX_DUAL_OK);
    common_measures("7B-capacity", &T, ctl, &res, 3000.0, &BC2, &R, 1); tail_zero("7B-capacity", &R); monotone_from("7B-capacity", &R, 0);
    CHECK(R.last.generation == 200 && R.last.tick == 99 && R.lambda[0] > 0.0);
    /* 8 regime change */
    CHECK_ST(dual_trace_scenario(8, 0, 0, &T), RX_DUAL_OK);
    CHECK_ST(run("8-regime", &T, ctl, &res, DUAL_BUDGET, &BC, &R), RX_DUAL_OK);
    common_measures("8-regime", &T, ctl, &res, DUAL_BUDGET, &BC, &R, 1); tail_zero("8-regime", &R);
    { int ok = 1, after = 1;
      for (uint32_t i = 80; i < 120; i++) if (R.state[i] != RX_DUAL_LAMBDA_FROZEN || R.lambda[i] != R.lambda[i - 1] || R.estimate[i] != R.estimate[i - 1] || R.generation[i] != R.generation[i - 1]) ok = 0;
      for (uint32_t i = 120; i < R.n; i++) if (R.state[i] != RX_DUAL_LAMBDA_UNCALIBRATED) after = 0;
      snprintf(d, sizeof d, "ticks 80..119 FROZEN with lambda/estimate/generation held: %s; UNCALIBRATED resumes at 120: %s", ok ? "yes" : "no", after ? "yes" : "no");
      measure("8-regime", "M6", ok && after, d, 1); CHECK(R.generation[119] == 80 && R.lambda[120] > R.lambda[119]); }
    /* 9 bursty */
    CHECK_ST(dual_trace_scenario(9, 0, 0, &T), RX_DUAL_OK);
    CHECK_ST(run("9-bursty", &T, ctl, &res, DUAL_BUDGET, &BC, &R), RX_DUAL_OK);
    common_measures("9-bursty", &T, ctl, &res, DUAL_BUDGET, &BC, &R, 1);
    { uint32_t bursts = 0; for (uint32_t i = 0; i < T.n_ticks; i++) if (T.t[i].estimate > 4200.0) bursts++; printf("INFO 9-bursty bursts=%u of %u ticks\n", bursts, T.n_ticks); CHECK(bursts > 5 && bursts < 60); }
    /* 10 fan-out: one controller n = 32, 32 chains */
    { RxDualController c32; double fin[RX_DUAL_MAX_RESOURCES], mn = 1e300, mx = -1e300; char name[24];
      memset(&c32, 0, sizeof c32); c32.eta = 0.1; c32.rho = 0.05; c32.k_sigma = 2.0; c32.max_age = 4; c32.cadence = 1; c32.n = RX_DUAL_MAX_RESOURCES;
      for (uint32_t r = 0; r < RX_DUAL_MAX_RESOURCES; r++) { c32.resource_id[r] = r + 1; c32.lambda_max[r] = 10.0; }
      for (uint32_t r = 0; r < RX_DUAL_MAX_RESOURCES; r++) {
          RxDualResource rr = fx_res(r + 1, RX_DUAL_UNIT_BYTES, DUAL_SCALE);
          snprintf(name, sizeof name, "10-fanout-%02u", r);
          CHECK_ST(dual_trace_scenario(10, 0, r, &T), RX_DUAL_OK);
          CHECK_ST(run(name, &T, &c32, &rr, DUAL_BUDGET, &BC, &R), RX_DUAL_OK);
          common_measures(name, &T, &c32, &rr, DUAL_BUDGET, &BC, &R, 1); tail_zero(name, &R);
          fin[r] = R.lambda[R.n - 1]; if (fin[r] < mn) mn = fin[r]; if (fin[r] > mx) mx = fin[r]; }
      snprintf(d, sizeof d, "final lambdas in [%.9g, %.9g], spread %.9g bound 0.25", mn, mx, mx - mn); measure("10-fanout", "M10", mx - mn <= 0.25, d, 1); }
    /* 11 KV saturation */
    CHECK_ST(dual_trace_scenario(11, 0, 0, &T), RX_DUAL_OK);
    CHECK_ST(run("11-kv", &T, ctl, &res, DUAL_BUDGET, &BC, &R), RX_DUAL_OK);
    common_measures("11-kv", &T, ctl, &res, DUAL_BUDGET, &BC, &R, 1);
    { uint32_t cross = 0; int ok = 1;
      for (uint32_t i = 0; i < T.n_ticks; i++) { if (T.t[i].estimate <= 5200.0) { if (R.lambda[i] != 0.0) ok = 0; } else if (!cross) cross = i; }
      snprintf(d, sizeof d, "lambda = 0 while estimate <= 5200 (first crossing at tick %u)", cross); measure("11-kv", "M11", ok, d, 1);
      monotone_from("11-kv", &R, cross); CHECK(cross == 110); }
    /* 12 recovery */
    CHECK_ST(dual_trace_scenario(12, 0, 0, &T), RX_DUAL_OK);
    CHECK_ST(run("12-recovery", &T, ctl, &res, DUAL_BUDGET, &BC, &R), RX_DUAL_OK);
    common_measures("12-recovery", &T, ctl, &res, DUAL_BUDGET, &BC, &R, 1); tail_zero("12-recovery", &R);
    { uint32_t K = (uint32_t)ceil(log(100.0) / -log(1.0 - 0.05)); double peak = R.lambda[99], at = R.lambda[99 + K];
      snprintf(d, sizeof d, "K=%u peak=%.9g lambda[%u]=%.9g <= 0.01*peak=%.9g", K, peak, 99 + K, at, 0.01 * peak);
      measure("12-recovery", "M5", K == 90 && at <= 0.01 * peak, d, 1); }
}

/* ---- recorded R15 traces ---- */
static uint8_t g_file[16u << 20];
static size_t read_file(const char *path)
{
    FILE *f = fopen(path, "rb"); size_t n;
    if (!f) { fprintf(stderr, "FAIL cannot open %s\n", path); g_fail++; return 0; }
    n = fread(g_file, 1, sizeof g_file, f);
    if (!feof(f)) { fprintf(stderr, "FAIL %s larger than buffer\n", path); g_fail++; n = 0; }
    fclose(f);
    return n;
}

static void recorded_one(const char *dir, const char *file, const char *cpu, const char *event)
{
    char path[512], sums[512], name[64], raw[65], d[200]; size_t n; uint8_t dig[32];
    RxDualResource res = fx_res(1, RX_DUAL_UNIT_DIMENSIONLESS, 1e9);
    snprintf(path, sizeof path, "evidence/R15/raw/%s/%s", dir, file);
    snprintf(sums, sizeof sums, "evidence/R15/raw/%s/SHA256SUMS", dir);
    snprintf(name, sizeof name, "13-%.12s-%s-%s", dir, file[0] == 'm' ? "machine" : "preflight", cpu);
    n = read_file(path); if (!n) return;
    sha256_hash(g_file, n, dig); hex(dig, 32, raw);
    printf("RECORDED %s bytes=%zu sha256=%s\n", path, n, raw);
    CHECK_ST(rx_dual_trace_from_perf_csv(g_file, n, cpu, event, &T), RX_DUAL_OK);
    printf("RECORDED %s series cpu=%s event=%s ticks=%u (UNCALIBRATED counter stream)\n", path, cpu, event, T.n_ticks);
    CHECK(T.trace_kind == RX_DUAL_TRACE_RECORDED && T.n_ticks > 10);
    /* the R15 run's own SHA256SUMS must list this digest (binds the trace to the sealed R15 evidence) */
    { size_t m = read_file(sums); int found = 0, listed = 0;
      if (m) { g_file[m < sizeof g_file ? m : sizeof g_file - 1] = 0; listed = strstr((char *)g_file, file) != NULL; found = strstr((char *)g_file, raw) != NULL; }
      if (listed) { snprintf(d, sizeof d, "raw sha256 equals the entry in the run directory SHA256SUMS: %s", found ? "yes" : "no"); measure(name, "SUM", found, d, 1); }
      else printf("INFO %s %s is not listed in the run directory SHA256SUMS; its raw sha256 is recorded in this receipt only\n", name, file); }
    CHECK_ST(run(name, &T, ref_ctl(), &res, 1e9, &BC, &R), RX_DUAL_OK);
    common_measures(name, &T, ref_ctl(), &res, 1e9, &BC, &R, 1);
    { uint32_t above = 0; for (uint32_t i = 0; i < T.n_ticks; i++) if (T.t[i].estimate > 1e9) above++;
      printf("INFO %s ticks above budget 1e9: %u of %u; final lambda %.9g\n", name, above, T.n_ticks, R.lambda[R.n - 1]); }
}

static void recorded(void)
{
    static const char *dirs[2] = { "20260929T020536Z-ad8e1f2ea4e4-silicon", "20260929T025735Z-3e9e53be3358-silicon" };
    static const uint8_t bad[] = "# c\n1.0,CPU0,<not counted>,,armv8_pmuv3_0/cycles/,1,100.00,,\n";
    static const uint8_t none[] = "# c\n1.0,CPU1,5,,armv8_pmuv3_0/cycles/,1,100.00,,\n";
    static const uint8_t two[] = "# c\n\n1.0,CPU0,5,,armv8_pmuv3_0/cycles/,1,100.00,,\n1.0,CPU0,7,,x/cycles/,1,100.00,,\n2.0,CPU0,9,,armv8_pmuv3_0/cycles/,1,100.00,,";
    for (int k = 0; k < 2; k++) {
        recorded_one(dirs[k], "machine-perf.csv", "CPU0", "armv8_pmuv3_0/cycles/");
        recorded_one(dirs[k], "machine-perf.csv", "CPU7", "armv8_pmuv3_1/cycles/");
        recorded_one(dirs[k], "preflight-perf.csv", "CPU7", "armv8_pmuv3_1/cycles/");
    }
    /* importer refusals and exactness */
    CHECK_ST(rx_dual_trace_from_perf_csv(bad, sizeof bad - 1, "CPU0", "armv8_pmuv3_0/cycles/", &T), RX_DUAL_ERR_ENCODING);
    CHECK_ST(rx_dual_trace_from_perf_csv(none, sizeof none - 1, "CPU0", "armv8_pmuv3_0/cycles/", &T), RX_DUAL_ERR_RANGE);
    CHECK_ST(rx_dual_trace_from_perf_csv(two, sizeof two - 1, "CPU0", "armv8_pmuv3_0/cycles/", &T), RX_DUAL_OK);
    CHECK(T.n_ticks == 2 && T.t[0].estimate == 5.0 && T.t[1].estimate == 9.0 && T.t[1].generation == 2 && T.t[0].calibrated == 0);
}

static void trace_refusals(void)
{
    CHECK_ST(dual_trace_scenario(2, 0, 0, &T), RX_DUAL_OK);
    { RxDualTrace *t = &T; RxDualDigest d; double keep = t->t[3].estimate;
      t->t[3].estimate = NAN; CHECK_ST(rx_dual_digest_trace(t, &d), RX_DUAL_ERR_NONFINITE); t->t[3].estimate = keep;
      t->t[3].uncertainty = -1; CHECK_ST(rx_dual_digest_trace(t, &d), RX_DUAL_ERR_RANGE); t->t[3].uncertainty = 100;
      t->t[3].regime_change = 2; CHECK_ST(rx_dual_digest_trace(t, &d), RX_DUAL_ERR_RANGE); t->t[3].regime_change = 0;
      t->trace_kind = 0; CHECK_ST(rx_dual_digest_trace(t, &d), RX_DUAL_ERR_KIND); t->trace_kind = 2;
      t->n_ticks = 0; CHECK_ST(rx_dual_digest_trace(t, &d), RX_DUAL_ERR_RANGE); t->n_ticks = RX_DUAL_TRACE_MAX_TICKS + 1;
      CHECK_ST(rx_dual_digest_trace(t, &d), RX_DUAL_ERR_RANGE); t->n_ticks = 200;
      CHECK_ST(rx_dual_digest_trace(t, &d), RX_DUAL_OK);
      /* -0.0 canonicalized: same digest as +0.0 */
      { RxDualDigest a, b; t->t[5].estimate = 0.0; rx_dual_digest_trace(t, &a); t->t[5].estimate = -0.0; rx_dual_digest_trace(t, &b); CHECK(rx_dual_digest_eq(&a, &b)); t->t[5].estimate = keep; } }
    { static const double x[] = { 0, 1, 2, 2, 1, 1, 3, 3 }; CHECK(rx_dual_reversals(x, 0, 8) == 2); CHECK(rx_dual_reversals(x, 0, 3) == 0); CHECK(rx_dual_peak_to_peak(x, 0, 8) == 3.0); CHECK(rx_dual_reversals(x, 4, 4) == 0); }
}

int main(void)
{
    BC = fx_dig(0x22); BC2 = fx_dig(0x23);
    trace_refusals();
    scenarios();
    recorded();
    printf("NOTE every replayed price above is UNCALIBRATED (EST-3 has not passed for any signal); none may influence production.\n");
    printf("GATE DUAL_PRICE_REFERENCE = %s (reference measures %s, negative control %s)\n",
           (!g_ref_fail && g_neg_as_declared && !g_fail) ? "PASS" : "FAIL",
           g_ref_fail ? "FAILED" : "all pass", g_neg_as_declared ? "failed as declared" : "DID NOT behave as declared");
    printf("test_dual_replay: %d checks, %d failures: %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
