/* Phase-domain Z3 digital twin: gates 1-4 (spec/mixed-algebra-phase-twin.md).
 * Usage: test_phase_twin [receipt.json]
 * Exit 0 only when every gate passes. Deterministic: fixed seeds, no clock
 * or host data in the receipt, so plain and sanitizer builds must write
 * byte-identical receipts. */
#include "algebra/oma_z3.h"
#include "algebra/phase_twin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Z999 3.2905267314919255 /* two-sided 99.9 % */
#define Z95 1.959963984540054   /* two-sided 95 % */

static int g_fail = 0;
static FILE *R = NULL; /* receipt */
static int g_quick = 0; /* --quick: trial counts / 50, for sanitizer runs; not evidence */
static uint64_t S(uint64_t n) { return g_quick ? n / 50 : n; }

static void check(int ok, const char *what) {
    if (!ok) {
        g_fail++;
        printf("FAIL: %s\n", what);
    }
}

/* ------------------------------------------------------------ designs */
/* Design A: research-note reference design, 125 MS/s, f_c = f_s/64, N = 4096, 14-bit. */
static pt_config design_a(void) {
    pt_config c;
    memset(&c, 0, sizeof c);
    c.ch.fs_hz = 125e6;
    c.ch.cycle_samples = 64;
    c.ch.n_samples = 4096;
    c.ch.amplitude = 0.5; /* -6.02 dBFS */
    c.ch.dac_bits = 14;
    c.ch.adc_bits = 14;
    c.ch.frame_s = 2.0 * 4096 / 125e6;
    c.mode = PT_ADD_CORR_PRODUCT;
    c.flags.erasure_frac = 0.3;
    c.flags.reject_deg = 30.0;
    return c;
}

/* Design B: low-rate open-toolchain version, 1 MS/s, N = 1024, 12-bit. */
static pt_config design_b(void) {
    pt_config c = design_a();
    c.ch.fs_hz = 1e6;
    c.ch.n_samples = 1024;
    c.ch.dac_bits = 12;
    c.ch.adc_bits = 12;
    c.ch.frame_s = 2.0 * 1024 / 1e6;
    return c;
}

static const char *mode_name(pt_add_mode m) {
    return m == PT_ADD_CORR_PRODUCT ? "corr_product" : m == PT_ADD_MIXER ? "mixer" : "nco";
}

static void jnum(const char *k, double v, int comma) {
    fprintf(R, "\"%s\": %.6e%s", k, v, comma ? ", " : "");
}

static void jcfg(const pt_config *c) {
    fprintf(R, "\"fs_hz\": %.6e, \"fc_hz\": %.6e, \"N\": %u, \"amplitude_fs\": %.4f, "
               "\"dac_bits\": %d, \"adc_bits\": %d, \"noise_sigma_fs\": %.6e, \"jitter_deg_rms\": %.4e, "
               "\"offset_deg\": [%.3f, %.3f], \"drift_deg_per_s\": [%.3f, %.3f], \"frame_s\": %.6e, "
               "\"calibration\": {\"enabled\": %s, \"interval_ops\": %u, \"reps\": %u, \"check_tol_deg\": %.1f}, "
               "\"flags\": {\"erasure_frac\": %.2f, \"reject_deg\": %.1f}",
            c->ch.fs_hz, c->ch.fs_hz / c->ch.cycle_samples, c->ch.n_samples, c->ch.amplitude,
            c->ch.dac_bits, c->ch.adc_bits, c->ch.noise_sigma, c->ch.jitter_deg,
            c->ch.offset_deg[0], c->ch.offset_deg[1], c->ch.drift_deg_per_s[0], c->ch.drift_deg_per_s[1],
            c->ch.frame_s, c->cal.enabled ? "true" : "false", c->cal.interval_trials, c->cal.reps,
            c->cal.check_tol_deg, c->flags.erasure_frac, c->flags.reject_deg);
}

typedef struct {
    uint64_t trials, mismatches, flagged, failures;
    double max_dist_deg;
} tally;

/* Run pairs x reps through add (op = 0) or mul_const (op = 1), comparing with the oracle. */
static tally run_pairs(const pt_config *cfg, uint64_t seed, int op, int reps) {
    tally t;
    memset(&t, 0, sizeof t);
    pt_twin tw;
    if (pt_twin_init(&tw, cfg, seed) != PT_OK) {
        check(0, "pt_twin_init");
        return t;
    }
    for (int r = 0; r < reps; r++) {
        for (int a = 0; a < 3; a++) {
            for (int b = 0; b < 3; b++) {
                pt_result res;
                oma_z3 want;
                int rc = op ? pt_twin_mul_const(&tw, a, b, &res) : pt_twin_add(&tw, a, b, &res);
                int orc = op ? oma_z3_mul(a, b, &want) : oma_z3_add(a, b, &want);
                if (rc != PT_OK || orc != OMA_OK) {
                    check(0, "twin/oracle call");
                    continue;
                }
                double dist;
                (void)pt_decode(res.phase_deg * (PT_PI / 180.0), &dist);
                if (dist > t.max_dist_deg) t.max_dist_deg = dist;
                t.trials++;
                int bad = 0;
                if (res.decoded != (int)want) { t.mismatches++; bad = 1; }
                if (res.flags) { t.flagged++; bad = 1; }
                t.failures += (uint64_t)bad;
            }
        }
    }
    pt_twin_free(&tw);
    return t;
}

static void jtally(const tally *t) {
    fprintf(R, "\"checks\": %llu, \"mismatches\": %llu, \"flagged\": %llu, \"max_phase_err_deg\": %.6e",
            (unsigned long long)t->trials, (unsigned long long)t->mismatches,
            (unsigned long long)t->flagged, t->max_dist_deg);
}

/* ------------------------------------------------------------- gate 1 */
static int gate1(void) {
    int ok = 1;
    const int REPS = g_quick ? 20 : 1000;
    fprintf(R, "  \"gate1_noiseless\": {\n    \"pairs\": 9, \"reps\": %d,\n    \"add\": [\n", REPS);
    struct { const char *name; pt_config c; } designs[3];
    designs[0].name = "A_125MSps_N4096_14bit"; designs[0].c = design_a();
    designs[1].name = "B_1MSps_N1024_12bit"; designs[1].c = design_b();
    designs[2].name = "B_1MSps_N1024_ideal"; designs[2].c = design_b();
    designs[2].c.ch.dac_bits = designs[2].c.ch.adc_bits = 0;
    pt_add_mode modes[3] = { PT_ADD_CORR_PRODUCT, PT_ADD_MIXER, PT_ADD_NCO };
    int first = 1;
    uint64_t seed = 0x5A31;
    for (int d = 0; d < 3; d++) {
        for (int m = 0; m < 3; m++) {
            pt_config c = designs[d].c;
            c.mode = modes[m];
            tally t = run_pairs(&c, seed++, 0, REPS);
            int pass = t.trials == 9u * REPS && t.failures == 0;
            ok &= pass;
            printf("gate1 add %-22s %-12s checks=%llu mismatches=%llu flagged=%llu max_err=%.3e deg %s\n",
                   designs[d].name, mode_name(c.mode), (unsigned long long)t.trials,
                   (unsigned long long)t.mismatches, (unsigned long long)t.flagged, t.max_dist_deg,
                   pass ? "PASS" : "FAIL");
            fprintf(R, "%s      {\"design\": \"%s\", \"mode\": \"%s\", ", first ? "" : ",\n", designs[d].name,
                    mode_name(c.mode));
            jtally(&t);
            fprintf(R, ", \"pass\": %s}", pass ? "true" : "false");
            first = 0;
        }
    }
    fprintf(R, "\n    ],\n");

    /* Multiply by a known constant (theory 6.4): design B, calibration on (no-op offsets). */
    pt_config c = design_b();
    c.cal.enabled = 1;
    c.cal.interval_trials = 1000;
    c.cal.reps = 1;
    c.cal.check_tol_deg = 5.0;
    tally tm = run_pairs(&c, 0x5A40, 1, REPS);
    int mpass = tm.trials == 9u * REPS && tm.failures == 0;
    ok &= mpass;
    printf("gate1 mul_const B_1MSps_N1024_12bit     checks=%llu mismatches=%llu flagged=%llu max_err=%.3e deg %s\n",
           (unsigned long long)tm.trials, (unsigned long long)tm.mismatches, (unsigned long long)tm.flagged,
           tm.max_dist_deg, mpass ? "PASS" : "FAIL");
    fprintf(R, "    \"mul_const\": {\"design\": \"B_1MSps_N1024_12bit\", \"realization\": "
               "\"b=0 constant k=0 source, b=1 identity, b=2 frequency doubler (squarer, 2 f_c bin)\", ");
    jtally(&tm);
    fprintf(R, ", \"pass\": %s},\n", mpass ? "true" : "false");

    /* Quantization sweep (informational; decoding gated only at the declared 12 and 14 bits). */
    fprintf(R, "    \"quantization_sweep_corr_product_N1024\": [");
    int bits_list[] = { 16, 14, 12, 10, 8, 6, 4, 3, 2, 1 };
    for (size_t i = 0; i < sizeof bits_list / sizeof bits_list[0]; i++) {
        pt_config q = design_b();
        q.ch.dac_bits = q.ch.adc_bits = bits_list[i];
        tally tq = run_pairs(&q, 0x5A50 + i, 0, 2);
        printf("gate1 quant bits=%-2d max_err=%.3e deg mismatches=%llu flagged=%llu (info)\n", bits_list[i],
               tq.max_dist_deg, (unsigned long long)tq.mismatches, (unsigned long long)tq.flagged);
        fprintf(R, "%s{\"bits\": %d, \"max_phase_err_deg\": %.6e, \"mismatches\": %llu, \"flagged\": %llu}",
                i ? ", " : "", bits_list[i], tq.max_dist_deg, (unsigned long long)tq.mismatches,
                (unsigned long long)tq.flagged);
    }
    fprintf(R, "],\n");
    fprintf(R, "    \"evidence_tier\": \"E3 (exhaustive over the 9-pair Z3 domain, per declared model "
               "configuration; the ideal map is E4 by theory section 6.1)\",\n");
    fprintf(R, "    \"pass\": %s\n  },\n", ok ? "true" : "false");
    printf("gate1 %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* Gate 1b: the research note's pass condition at the reference design's noise. */
static int gate1b(void) {
    pt_config c = design_a();
    double snr1 = 1.0e8; /* 80 dB per sample at -6 dBFS, ideal 14-bit */
    c.ch.noise_sigma = c.ch.amplitude / sqrt(2.0 * snr1);
    c.ch.jitter_deg = 360.0 * (c.ch.fs_hz / 64.0) * 1e-12; /* 1 ps rms */
    c.ch.offset_deg[1] = 3.35;                              /* 1 m cable, VF 0.70 */
    c.cal.enabled = 1;
    c.cal.interval_trials = 1000;
    c.cal.reps = 8;
    c.cal.check_tol_deg = 5.0;
    tally t = run_pairs(&c, 0x1B00, 0, g_quick ? 20 : 1000);
    int pass = t.trials == (g_quick ? 180u : 9000u) && t.failures == 0;
    printf("gate1b add A_125MSps_N4096_14bit SNR1=80dB offset 3.35deg calibrated: checks=%llu mismatches=%llu "
           "flagged=%llu max_err=%.3e deg %s\n",
           (unsigned long long)t.trials, (unsigned long long)t.mismatches, (unsigned long long)t.flagged,
           t.max_dist_deg, pass ? "PASS" : "FAIL");
    fprintf(R, "  \"gate1b_reference_operating_point\": {\"condition\": \"research note pass condition: 9 pairs x "
               "1000, zero mismatches\", \"snr1_db\": 80.0, ");
    jcfg(&c);
    fprintf(R, ", ");
    jtally(&t);
    fprintf(R, ", \"pass\": %s},\n", pass ? "true" : "false");
    return pass;
}

/* ------------------------------------------------------------- gate 2 */
static int gate2(void) {
    int ok = 1;
    const double rho_db[] = { 0.0, 6.0, 8.0, 10.0, 12.0 };
    const uint64_t trials_full[] = { 100000, 200000, 400000, 1000000, 4000000 };
    uint64_t trials[5];
    for (int i = 0; i < 5; i++) trials[i] = S(trials_full[i]);
    const double note_add[] = { 3.52e-1, 5.76e-2, 1.52e-2, 1.96e-3, 8.46e-5 };
    const double note_single_exact[] = { 1.834e-1, 1.371e-2, 2.037e-3, 1.065e-4, 1.082e-6 };
    const int np = 5;
    fprintf(R, "  \"gate2_snr_curve\": {\n    \"model\": \"corr_product add, both inputs at symbol SNR rho "
               "(correlator output, rho = N SNR_1 / 2), ideal converters, no jitter, no offset, flags off; "
               "sampled AWGN per sample\",\n");
    pt_config base = design_b();
    base.ch.n_samples = 64;
    base.ch.dac_bits = base.ch.adc_bits = 0;
    base.flags.erasure_frac = base.flags.reject_deg = 0.0;
    base.ch.frame_s = 128e-6;
    fprintf(R, "    \"config\": {");
    jcfg(&base);
    fprintf(R, "},\n    \"tolerance\": \"exact reference inside the measured Wilson 99.9%% interval; "
               "research-note Monte Carlo within 5%% of the exact reference at 10 and 12 dB\",\n");
    fprintf(R, "    \"points\": [\n");
    for (int i = 0; i < np; i++) {
        double rho = pow(10.0, rho_db[i] / 10.0);
        pt_config c = base;
        c.ch.noise_sigma = pt_sigma_for_rho(c.ch.amplitude, c.ch.n_samples, rho);
        pt_twin tw;
        if (pt_twin_init(&tw, &c, 0x2000 + (uint64_t)i) != PT_OK) {
            check(0, "gate2 init");
            return 0;
        }
        uint64_t err = 0, err_single = 0;
        for (uint64_t t = 0; t < trials[i]; t++) {
            int a = (int)(t % 3), b = (int)((t / 3) % 3);
            pt_result r;
            oma_z3 want;
            pt_twin_add(&tw, a, b, &r);
            oma_z3_add(a, b, &want);
            err += r.decoded != (int)want;
            err_single += r.decoded_a != a;
        }
        pt_twin_free(&tw);
        double exact = pt_ref_add_exact(rho, 7200);
        double craig = pt_ref_single_craig(rho);
        double ph = (double)err / (double)trials[i];
        double ps = (double)err_single / (double)trials[i];
        pt_interval w = pt_wilson(err, trials[i], Z999);
        pt_interval ws = pt_wilson(err_single, trials[i], Z999);
        int in_exact = exact >= w.lo && exact <= w.hi;
        double rel_note = fabs(note_add[i] - exact) / exact;
        int note_ok = (rho_db[i] < 9.0) || rel_note <= 0.05;
        /* single-phasor: gated only where errors are countable (>= 100 expected) */
        int single_gated = craig * (double)trials[i] >= 100.0;
        int single_ok = !single_gated || (craig >= ws.lo && craig <= ws.hi);
        int craig_note = fabs(craig - note_single_exact[i]) / note_single_exact[i] <= 0.01;
        int pass = in_exact && note_ok && single_ok && craig_note;
        ok &= pass;
        printf("gate2 rho=%4.1f dB trials=%-8llu add: err=%-7llu p=%.4e wilson99.9=[%.3e, %.3e] exact=%.4e "
               "note=%.3e (rel %.3f) | single: p=%.4e craig=%.4e %s\n",
               rho_db[i], (unsigned long long)trials[i], (unsigned long long)err, ph, w.lo, w.hi, exact,
               note_add[i], rel_note, ps, craig, pass ? "PASS" : "FAIL");
        fprintf(R, "%s      {\"rho_db\": %.1f, \"trials\": %llu, \"add_errors\": %llu, ", i ? ",\n" : "", rho_db[i],
                (unsigned long long)trials[i], (unsigned long long)err);
        jnum("add_p_hat", ph, 1);
        fprintf(R, "\"add_wilson999\": [%.6e, %.6e], ", w.lo, w.hi);
        jnum("add_exact_reference", exact, 1);
        jnum("add_research_note_mc", note_add[i], 1);
        jnum("rel_diff_note_vs_exact", rel_note, 1);
        fprintf(R, "\"single_errors\": %llu, ", (unsigned long long)err_single);
        jnum("single_p_hat", ps, 1);
        fprintf(R, "\"single_wilson999\": [%.6e, %.6e], ", ws.lo, ws.hi);
        jnum("single_craig_exact", craig, 1);
        fprintf(R, "\"single_gated\": %s, \"pass\": %s}", single_gated ? "true" : "false", pass ? "true" : "false");
    }
    fprintf(R, "\n    ],\n");

    /* Pure phase jitter, 15 deg per input: 2 Q(60 / (sqrt2 s)). */
    pt_config c = base;
    c.ch.jitter_deg = 15.0;
    c.ch.noise_sigma = 0.0;
    pt_twin tw;
    uint64_t jt = S(400000), jerr = 0;
    if (pt_twin_init(&tw, &c, 0x2100) != PT_OK) {
        check(0, "gate2 jitter init");
        return 0;
    }
    for (uint64_t t = 0; t < jt; t++) {
        int a = (int)(t % 3), b = (int)((t / 3) % 3);
        pt_result r;
        oma_z3 want;
        pt_twin_add(&tw, a, b, &r);
        oma_z3_add(a, b, &want);
        jerr += r.decoded != (int)want;
    }
    pt_twin_free(&tw);
    double jref = pt_ref_add_jitter(15.0);
    pt_interval jw = pt_wilson(jerr, jt, Z999);
    int jok = jref >= jw.lo && jref <= jw.hi;
    ok &= jok;
    printf("gate2 jitter 15 deg/input trials=%llu p=%.4e wilson99.9=[%.3e, %.3e] formula=%.4e %s\n",
           (unsigned long long)jt, (double)jerr / (double)jt, jw.lo, jw.hi, jref, jok ? "PASS" : "FAIL");
    fprintf(R, "    \"jitter_check\": {\"jitter_deg_per_input\": 15.0, \"trials\": %llu, \"errors\": %llu, ",
            (unsigned long long)jt, (unsigned long long)jerr);
    jnum("p_hat", (double)jerr / (double)jt, 1);
    fprintf(R, "\"wilson999\": [%.6e, %.6e], ", jw.lo, jw.hi);
    jnum("formula_2Q", jref, 1);
    fprintf(R, "\"pass\": %s},\n", jok ? "true" : "false");
    fprintf(R, "    \"evidence_tier\": \"model validation (statistical agreement with analytic reference); "
               "not a contract\",\n");
    fprintf(R, "    \"pass\": %s\n  },\n", ok ? "true" : "false");
    printf("gate2 %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* ------------------------------------------------------------- gate 3 */
#define CONTRACT_P 1e-6
#define CONTRACT_ALPHA 0.05
#define CONTRACT_RHO_DB 25.0

static pt_config contract_point(void) {
    pt_config c = design_b();
    c.ch.n_samples = 64;
    c.ch.frame_s = 128e-6; /* one window + one guard window at 1 MS/s */
    c.ch.noise_sigma = pt_sigma_for_rho(c.ch.amplitude, c.ch.n_samples, pow(10.0, CONTRACT_RHO_DB / 10.0));
    c.ch.jitter_deg = 1.0;
    c.cal.interval_trials = 1000;
    c.cal.reps = 32; /* 8 reps at rho 25 dB gives ~6e-5 false k=1 check faults per check */
    c.cal.check_tol_deg = 5.0;
    return c;
}

typedef struct {
    tally t;
    uint64_t cal_count, cal_faults;
    double sim_time_s;
} case_out;

static case_out run_case(const pt_config *c, uint64_t seed, uint64_t trials) {
    case_out o;
    memset(&o, 0, sizeof o);
    pt_twin tw;
    if (pt_twin_init(&tw, c, seed) != PT_OK) {
        check(0, "gate3 init");
        return o;
    }
    pt_rng in;
    pt_rng_seed(&in, seed ^ 0xC0FFEEull);
    for (uint64_t i = 0; i < trials; i++) {
        uint64_t x = pt_rng_next(&in);
        int a = (int)(x % 3), b = (int)((x >> 32) % 3);
        pt_result r;
        oma_z3 want;
        pt_twin_add(&tw, a, b, &r);
        oma_z3_add(a, b, &want);
        o.t.trials++;
        int bad = 0;
        if (r.decoded != (int)want) { o.t.mismatches++; bad = 1; }
        if (r.flags) { o.t.flagged++; bad = 1; }
        o.t.failures += (uint64_t)bad;
    }
    o.cal_count = tw.cal_count;
    o.cal_faults = tw.cal_faults;
    o.sim_time_s = tw.t_s;
    pt_twin_free(&tw);
    return o;
}

static int gate3(void) {
    int ok = 1;
    uint64_t need = (uint64_t)ceil(-log(CONTRACT_ALPHA) / CONTRACT_P);
    const uint64_t CONTRACT_TRIALS = S(4000000);
    pt_config cp = contract_point();
    fprintf(R, "  \"gate3_calibration\": {\n");
    fprintf(R, "    \"contract\": {\"kind\": \"BOUNDED_STOCHASTIC\", \"operation\": \"Z3 add (a+b) mod 3, "
               "corr_product realization\", \"bound\": \"symbol failure probability (mismatch or any flag) <= "
               "%.0e\", \"confidence\": %.2f, \"required_trials_zero_failures\": %llu, "
               "\"operating_point\": {\"rho_db_per_input\": %.1f, ",
            CONTRACT_P, 1.0 - CONTRACT_ALPHA, (unsigned long long)need, CONTRACT_RHO_DB);
    jcfg(&cp);
    fprintf(R, "}},\n    \"cases\": [\n");
    struct {
        const char *name;
        double off[2], drift[2];
        uint64_t uncal_trials, cal_trials;
        int is_contract;
    } cases[] = {
        { "offset_only", { 50.0, 20.0 }, { 0.0, 0.0 }, S(200000), S(200000), 0 },
        { "drift_only", { 0.0, 0.0 }, { 2.0, 1.5 }, 1000000 /* unscaled: drift needs simulated time */, S(1000000), 0 },
        { "offset_and_drift", { 50.0, 20.0 }, { 2.0, 1.5 }, S(200000), CONTRACT_TRIALS, 1 },
    };
    for (int k = 0; k < 3; k++) {
        for (int cal = 0; cal < 2; cal++) {
            pt_config c = cp;
            c.ch.offset_deg[0] = cases[k].off[0];
            c.ch.offset_deg[1] = cases[k].off[1];
            c.ch.drift_deg_per_s[0] = cases[k].drift[0];
            c.ch.drift_deg_per_s[1] = cases[k].drift[1];
            c.cal.enabled = cal;
            uint64_t n = cal ? cases[k].cal_trials : cases[k].uncal_trials;
            case_out o = run_case(&c, 0x3000 + (uint64_t)(k * 2 + cal), n);
            pt_interval w = pt_wilson(o.t.failures, o.t.trials, Z95);
            double e1 = pt_e1_bound(o.t.trials, CONTRACT_ALPHA);
            const char *verdict;
            int expect_ok;
            if (cal) {
                /* calibrated: zero failures; the contract case must also meet the bound */
                int meets = o.t.failures == 0 && (!(cases[k].is_contract && !g_quick) || (e1 <= CONTRACT_P && w.hi <= CONTRACT_P));
                verdict = meets ? ((cases[k].is_contract && !g_quick) ? "CONTRACT_MET" : "ZERO_FAILURES") : "FAILED";
                expect_ok = meets;
            } else {
                /* uncalibrated must be shown to violate the contract: Wilson lower bound above it */
                int violates = w.lo > CONTRACT_P;
                verdict = violates ? "CONTRACT_VIOLATED" : "NOT_DISTINGUISHED";
                expect_ok = violates;
            }
            ok &= expect_ok;
            printf("gate3 %-16s %-12s trials=%-8llu failures=%-8llu (mismatch %llu, flagged %llu) p=%.3e "
                   "wilson95=[%.3e, %.3e] E1=%.3e cal=%llu calfault=%llu sim_t=%.1fs -> %s %s\n",
                   cases[k].name, cal ? "calibrated" : "uncalibrated", (unsigned long long)o.t.trials,
                   (unsigned long long)o.t.failures, (unsigned long long)o.t.mismatches,
                   (unsigned long long)o.t.flagged, (double)o.t.failures / (double)o.t.trials, w.lo, w.hi,
                   o.t.failures ? 0.0 : e1, (unsigned long long)o.cal_count, (unsigned long long)o.cal_faults,
                   o.sim_time_s, verdict, expect_ok ? "PASS" : "FAIL");
            fprintf(R, "%s      {\"case\": \"%s\", \"calibrated\": %s, \"offset_deg\": [%.1f, %.1f], "
                       "\"drift_deg_per_s\": [%.2f, %.2f], \"trials\": %llu, \"failures\": %llu, "
                       "\"mismatches\": %llu, \"flagged\": %llu, ",
                    (k || cal) ? ",\n" : "", cases[k].name, cal ? "true" : "false", cases[k].off[0], cases[k].off[1],
                    cases[k].drift[0], cases[k].drift[1], (unsigned long long)o.t.trials,
                    (unsigned long long)o.t.failures, (unsigned long long)o.t.mismatches,
                    (unsigned long long)o.t.flagged);
            jnum("p_hat", (double)o.t.failures / (double)o.t.trials, 1);
            fprintf(R, "\"wilson95\": [%.6e, %.6e], ", w.lo, w.hi);
            if (o.t.failures == 0) jnum("e1_bound_95", e1, 1);
            fprintf(R, "\"calibrations\": %llu, \"calibration_faults\": %llu, \"simulated_time_s\": %.3f, "
                       "\"contract_case\": %s, \"verdict\": \"%s\", \"pass\": %s}",
                    (unsigned long long)o.cal_count, (unsigned long long)o.cal_faults, o.sim_time_s,
                    (cases[k].is_contract && !g_quick) ? "true" : "false", verdict, expect_ok ? "true" : "false");
        }
    }
    fprintf(R, "\n    ],\n    \"evidence_tier\": \"E1 for the contract case (zero failures in %llu independent "
               "trials, p <= -ln(0.05)/N at 95%% confidence), under the declared model only\",\n",
            (unsigned long long)CONTRACT_TRIALS);
    fprintf(R, "    \"pass\": %s\n  },\n", ok ? "true" : "false");
    printf("gate3 %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* ---------------------------------------------------- self-checks */
static int self_checks(void) {
    int ok = 1;
    /* decoder boundaries */
    double d;
    ok &= pt_decode(0.0, &d) == 0 && d == 0.0;
    ok &= pt_decode(2.0 * PT_PI / 3.0, NULL) == 1;
    ok &= pt_decode(-2.0 * PT_PI / 3.0, NULL) == 2;
    ok &= pt_decode(PT_PI, NULL) != 0;
    ok &= pt_decode(59.9 * PT_PI / 180.0, NULL) == 0 && pt_decode(60.1 * PT_PI / 180.0, NULL) == 1;
    /* NCO wrap pairs */
    uint32_t w22 = 2u * PT_NCO_THIRD + 2u * PT_NCO_THIRD;
    ok &= pt_decode(2.0 * PT_PI * ((double)w22 / 4294967296.0), NULL) == 1;
    /* argument rejection */
    pt_twin tw;
    pt_config c = design_b();
    pt_result r;
    ok &= pt_twin_init(&tw, &c, 1) == PT_OK;
    ok &= pt_twin_add(&tw, 3, 0, &r) == PT_E_ARG && pt_twin_add(&tw, 0, -1, &r) == PT_E_ARG;
    ok &= pt_twin_mul_const(&tw, 0, 3, &r) == PT_E_ARG && pt_twin_add(&tw, 0, 0, NULL) == PT_E_ARG;
    pt_twin_free(&tw);
    c.ch.n_samples = 100; /* not a multiple of the cycle */
    ok &= pt_twin_init(&tw, &c, 1) == PT_E_ARG;
    /* RNG determinism and moments */
    pt_rng a, b;
    pt_rng_seed(&a, 42);
    pt_rng_seed(&b, 42);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < 200000; i++) {
        double g = pt_rng_gauss(&a);
        ok &= g == pt_rng_gauss(&b);
        s1 += g;
        s2 += g * g;
    }
    ok &= fabs(s1 / 200000) < 0.01 && fabs(s2 / 200000 - 1.0) < 0.02;
    /* analytic references agree with each other: add exact at grid 3600 vs 7200, single vs Craig */
    double e36 = pt_ref_add_exact(pow(10.0, 1.0), 3600), e72 = pt_ref_add_exact(pow(10.0, 1.0), 7200);
    ok &= fabs(e36 - e72) / e72 < 1e-3;
    /* Craig at 0 dB vs research-note table 1.834e-1 */
    ok &= fabs(pt_ref_single_craig(1.0) - 0.1834) < 5e-4;
    /* Wilson sanity */
    pt_interval w = pt_wilson(0, 4000000, Z95);
    ok &= w.lo == 0.0 && w.hi < 1e-6;
    printf("self-checks %s (add exact grid 3600=%.6e 7200=%.6e at 10 dB)\n", ok ? "PASS" : "FAIL", e36, e72);
    fprintf(R, "  \"self_checks\": {\"decoder_boundaries\": true, \"nco_wrap\": true, \"arg_rejection\": true, "
               "\"rng_moments\": true, \"add_exact_grid_3600_vs_7200_at_10db\": [%.6e, %.6e], \"pass\": %s},\n",
            e36, e72, ok ? "true" : "false");
    return ok;
}

int main(int argc, char **argv) {
    const char *path = "phase_twin_receipt.json";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quick") == 0) g_quick = 1;
        else path = argv[i];
    }
    R = fopen(path, "w");
    if (!R) {
        perror(path);
        return 2;
    }
    const char *commit = getenv("PT_COMMIT");
    const char *dirty = getenv("PT_DIRTY");
    const char *cc = getenv("PT_TOOLCHAIN");
    if (!commit || !*commit) commit = "unrecorded";
    fprintf(R, "{\n  \"schema\": \"OMEGA_MIXED_ALGEBRA_PHASE_TWIN_V1\",\n");
    fprintf(R, "  \"stage\": \"ADR 0019 MA-8 step 0: simulated precursor of the physical phase realization "
               "(ADR 0018 AR2-style simulation); not MA-8 itself\",\n");
    fprintf(R, "  \"run_mode\": \"%s\",\n",
            g_quick ? "quick (trial counts / 50; sanitizer coverage run; NOT evidence)" : "full");
    fprintf(R, "  \"provenance\": \"SIMULATED_DEVELOPMENT\",\n");
    fprintf(R, "  \"label\": \"digital twin of phase-domain Z3 (software model). Under the declared model "
               "profile only: candidate phase-domain realizations vs the oma_z3 oracle. No physical, speed, "
               "latency or energy claim (ARCH-0018). No selection, no ranking, no semantic id or digest minted. "
               "Does not claim the Turing gate DIGITAL_PHASE_EQUIVALENCE.\",\n");
    fprintf(R, "  \"git_commit\": \"%s\",\n", commit);
    fprintf(R, "  \"tree_dirty\": %d,\n", dirty && *dirty ? atoi(dirty) : -1);
    fprintf(R, "  \"run_id\": \"phase_twin-%s\",\n", commit);
    fprintf(R, "  \"toolchain\": \"%s\",\n", cc && *cc ? cc : "unrecorded");
    fprintf(R, "  \"seeds\": \"fixed per scenario in tests/algebra/test_phase_twin.c (0x5A31.., 0x1B00, "
               "0x2000.., 0x2100, 0x3000..)\",\n");
    fprintf(R, "  \"turing_fields\": {\"note\": \"field names follow src/turing/field.h; values are text only, "
               "no digest is computed here\", "
               "\"contract\": {\"name\": \"z3_add\", \"statement\": \"c = (a + b) mod 3, a, b in {0,1,2}\", "
               "\"exactness\": \"BOUNDED_STOCHASTIC (noisy model); EXACT in the noiseless model\", "
               "\"overflow\": \"none (Z3 is closed)\", \"oracle\": \"oma_z3_add\", "
               "\"source\": \"spec/mixed-algebra-phase-twin.md\", \"max_n\": 1}, "
               "\"realizations\": ["
               "{\"rz_id\": \"PT_corr_product\", \"algorithm\": \"per-channel I/Q correlation, phasor product\", "
               "\"algebra\": \"Z3\", \"representation\": \"phase.z3\", \"precision\": \"declared ADC/DAC bits\", "
               "\"language\": \"C11\", \"backend\": \"cpu_model\", \"machine_class\": \"SIMULATED_DEVELOPMENT\", "
               "\"exact\": 0}, "
               "{\"rz_id\": \"PT_mixer\", \"algorithm\": \"analog mixer, 2 f_c correlation\", "
               "\"algebra\": \"Z3\", \"representation\": \"phase.z3\", \"precision\": \"declared ADC/DAC bits\", "
               "\"language\": \"C11\", \"backend\": \"cpu_model\", \"machine_class\": \"SIMULATED_DEVELOPMENT\", "
               "\"exact\": 0}, "
               "{\"rz_id\": \"PT_nco\", \"algorithm\": \"32-bit NCO phase-word add, one tone\", "
               "\"algebra\": \"Z3\", \"representation\": \"phase.z3\", \"precision\": \"declared ADC/DAC bits\", "
               "\"language\": \"C11\", \"backend\": \"cpu_model\", \"machine_class\": \"SIMULATED_DEVELOPMENT\", "
               "\"exact\": 0}], "
               "\"tier_source\": \"ADR 0019 section 8, aien-architecture origin/main 1438799\"},\n");
    fprintf(R, "  \"oracle\": \"oma_z3_add / oma_z3_mul (src/algebra/oma_z3.c)\",\n");
    fprintf(R, "  \"rng\": \"xoshiro256** seeded by splitmix64 (fixed seeds per scenario); Gaussian by "
               "Marsaglia polar method\",\n");
    fprintf(R, "  \"model\": {\"encoding\": \"k -> A cos(2 pi n / 64 + 2 pi k / 3), f_c = f_s / 64\", "
               "\"measurement\": \"I/Q correlation at the carrier bin (2 f_c for mixer and doubler)\", "
               "\"decision\": \"k = mod(round(theta / 120 deg), 3), cells +/-60 deg\", "
               "\"calibration\": \"reference frames: k = 0 averaged over reps sets each path offset and "
               "magnitude; k = 1 must read 120 +/- tol; repeated every interval\", "
               "\"flags\": \"erasure (magnitude < frac x reference), reject (> reject_deg from a center), "
               "calibration fault; any flag fails the trial\", "
               "\"simplifications\": [\"analog path is a pure phase shift at f_c, applied before the DAC\", "
               "\"AWGN added at the ADC input; white, Gaussian, independent per channel\", "
               "\"phase jitter is one Gaussian phase per channel per frame\", "
               "\"drift is linear in simulated time\", "
               "\"converters are ideal mid-tread quantizers with clipping; no INL/DNL, no spurs\", "
               "\"mixer and doubler are ideal multipliers\", "
               "\"shared clock: no sample slip, no frequency offset\"]},\n");
    int s = self_checks();
    int g1 = gate1();
    int g1b = gate1b();
    int g2 = gate2();
    int g3 = gate3();
    fprintf(R, "  \"z3_multiply\": {\"two_unknown_operands\": \"NOT_REALIZABLE in this model: phasor "
               "multiplication realizes Z3 addition; Z3 a*b corresponds to w^(ab) = (w^a)^b, exponentiation, and "
               "no bilinear phasor operation computes it (theory section 6.4). A decode-then-select circuit "
               "(decode b digitally, then identity / doubler / constant) is a hybrid digital realization, not a "
               "phase-domain one, and is not claimed.\", \"known_constant\": \"REALIZED (gate 1 mul_const): "
               "b = 0 constant source, b = 1 identity, b = 2 frequency doubler (w^(2a) = conj(w^a))\"},\n");
    fprintf(R, "  \"evidence_tiers_adr0019_s8\": {\"add_noiseless\": \"E3\", \"mul_const_noiseless\": \"E3\", "
               "\"add_bounded_contract\": \"E1\", \"mul_two_unknowns\": \"E0 (not realizable, no realization "
               "exists)\", \"scope\": \"all tiers hold for the twin model only; physical transfer needs AR4 "
               "evidence on a real board\"},\n");
    int all = s && g1 && g1b && g2 && g3 && g_fail == 0;
    fprintf(R, "  \"verified\": %d,\n", all ? 1 : 0);
    fprintf(R, "  \"overall\": \"%s\"\n}\n", all ? "PASS" : "FAIL");
    fclose(R);
    printf("PHASE_TWIN %s (receipt %s)\n", all ? "PASS" : "FAIL", path);
    return all ? 0 : 1;
}
