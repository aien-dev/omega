/* EST-3c protocol v3 tools (docs/estimation/EST3C_PROTOCOL_V3.md): shared
 * replay, statistics, pre-check and parameter-file code for est3c_fit and
 * est3c_eval. Tools only: may open files and print. The estimators are
 * src/estimation/est_pred.c (contract est_pred.h); this file only drives them.
 *
 * Documented choices (fixed before any D1 value was read):
 *  - Assumption shared by every family: unit mC, quantum 100 mC, valid range
 *    lo = -40000 mC, hi = 150000 mC (C3_LO, C3_HI: wider than any plausible
 *    die temperature, so the range class only catches garbage).
 *  - F1/F2 initial level variance: p0 = r (the fitted observation variance;
 *    the first valid reading is the prior mean).
 *  - F5 initial scale: s0 = sqrt(floor) (protocol: s0^2 = floor).
 *  - Tick mapping: one logical tick per ndjson line. Before a line whose wall
 *    gap to the previous parsed t rounds (half up) to h > 1 s, h - 1 MISSING
 *    ticks are inserted, so the effective horizon of that line's observation
 *    is round(gap) as in protocol v1 section 2. A line whose t does not parse
 *    is one nominal tick and a missing observation (v1 rule); a duplicate or
 *    backward t is one tick. Inserted ticks per gap are capped at
 *    C3_MAX_INSERT (counted). Evidence digest of a line tick = SHA-256 of the
 *    raw line bytes (no newline); of an inserted tick = SHA-256 of
 *    "est3c.inserted-missing" || 0x00 || that following line's bytes || u64le
 *    insertion index.
 *  - Burn-in: tick index L counted from the first valid observation (L = 0);
 *    ticks with L < 30 are not scored. Missing / bad ticks are never scored.
 *    A valid tick whose effective horizon exceeds EST_PRED_MAX_H (predict
 *    refuses) is counted as unscorable and becomes the new anchor.
 *  - Horizon-1 changes (F4 EM data, E0 histogram): y(t) - y(t-1) for scored
 *    ticks t (L >= 30) whose previous tick is also a valid observation.
 *  - E0: histogram of those changes in quanta smoothed by one pseudo-observation
 *    spread uniformly, clipped to [-400, 400] (outside values counted in the
 *    edge bin), p(k) = (c_k + 1/801) / (N + 1); persistence centre;
 *    for effective horizon h > 1 the pmf is
 *    est_pmf_conv_pow(h) then est_pmf_floor; h = 1 uses the smoothed pmf as is.
 *    The counts are stored in params.txt (e0_count lines) and est3c_eval
 *    rebuilds E0 from them only (it never reopens D1 for E0).
 *  - Regime of a tick: in-trial when the line's wall t lies inside a
 *    begin/end pair of the marks file (v1 section 1), else idle.
 *  - Quarters: four equal spans of the scored one-step steps by index
 *    (i * n / 4 boundaries).
 *  - Ten-step: origins are valid-observation ticks with L >= 30; the target is
 *    tick origin + 10 when that tick is a valid observation; the forecast is
 *    est_pred_predict(h = 10) right after the origin's update (E0: conv to the
 *    effective horizon).
 */
#ifndef OMEGA_EST3C_COMMON_H
#define OMEGA_EST3C_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "est_pred.h"
#include "est_replay.h"

/* 1 when the working tree in the current directory is not clean (git status). */
int c3_tree_dirty_now(void);

#define C3_BURN_IN 30u
#define C3_QUANTUM 100.0
#define C3_LO (-40000.0)
#define C3_HI 150000.0
#define C3_MAX_INSERT 100000u
#define C3_TEN 10u
#define C3_MIN_LINES 2030u
#define C3_NCORES 20.0

#define C3_D1_ID "20261001T025159Z-est3c-fit-silicon"
#define C3_D1_DIR_DEFAULT "evidence/EST3C/raw/" C3_D1_ID
#define C3_HELDOUT_TAG "-est3c-heldout-"
#define C3_DEV_A "evidence/R15/raw/20260929T020536Z-ad8e1f2ea4e4-silicon"
#define C3_DEV_B "evidence/R15/raw/20260929T025735Z-3e9e53be3358-silicon"
#define C3_DEV_C1 "evidence/EST3B/raw/20261001T020412Z-est3b-fit-silicon"
#define C3_PROTOCOL_DOC "docs/estimation/EST3C_PROTOCOL_V3.md"

/* ---- protocol section 6 bands (copied from the protocol text) ---- */
#define C3_COV50_LO 0.46
#define C3_COV50_HI 0.54
#define C3_COV80_LO 0.76
#define C3_COV80_HI 0.84
#define C3_COV95_LO 0.93
#define C3_COV95_HI 0.97
#define C3_PIT_LO 0.07
#define C3_PIT_HI 0.13
#define C3_BIAS_MAX 0.10
#define C3_LAG1_MAX 0.20
#define C3_SUB95_LO 0.90
#define C3_SUB95_HI 0.99
#define C3_REGIME_MIN 100u
#define C3_MIN_N 2000u
#define C3_TIE_NATS 0.01

/* ---- tick stream ---- */
typedef struct {
    size_t line;          /* source line index */
    int inserted;         /* 1: MISSING tick inserted for a wall gap */
    int present;          /* thermal zone 0 value present (parsed or not) */
    double value;         /* raw value; NaN when present but unparsable */
    int t_ok;
    int64_t wall_ns;      /* line t (inserted ticks: 0) */
    est_digest ev;
} c3_tick;

typedef struct {
    c3_tick *t;
    size_t n, cap;
    size_t lines, bad_t, absent_value, unparsable_value;
    size_t inserted, gaps_capped, gap_zero, gap_backward, lines_multi;
} c3_ticks;

int c3_ticks_build(const est_file *f, c3_ticks *out, char *err, size_t cap);
void c3_ticks_free(c3_ticks *tk);

/* ---- marks ---- */
#define C3_MAX_MARKS 512
#define C3_MAX_EV 1024
typedef struct {
    size_t n;                          /* closed or run-to-eof pairs */
    int64_t b[C3_MAX_MARKS], e[C3_MAX_MARKS];
    int level[C3_MAX_MARKS], index[C3_MAX_MARKS]; /* from "load-L<l>-<i>", -1 if not that shape */
    size_t begins, ends, unclosed, nested_begins, stray_ends, bad_lines;
    /* every begin/end line in file order (for the section 7 pair rule) */
    size_t nev; int64_t ev_t[C3_MAX_EV]; int ev_end[C3_MAX_EV], ev_level[C3_MAX_EV], ev_index[C3_MAX_EV];
    char sha[65];
} c3_marks;
/* verify = 1: est_file_load (SHA256SUMS); 0: raw read. */
int c3_marks_load(const char *path, int verify, c3_marks *mk, char *err, size_t cap);
int c3_in_trial(const c3_marks *mk, int64_t t);

/* ---- models ---- */
typedef struct {
    int id;                    /* 1..5 = F1..F5, 0 = E0 */
    est_assumption a;          /* F1..F5 */
    double e0_one[EST_PRED_N]; /* E0 one-step pmf */
} c3_model;

void c3_assumption_base(est_assumption *a, est_family fam);
void c3_make_f1(c3_model *m, double q_proc, double r);
void c3_make_f2(c3_model *m, double q_proc, double r, double c);
void c3_make_f3(c3_model *m, double nu, double s);
int c3_make_f4(c3_model *m, const est_mix *mx);
void c3_make_f5(c3_model *m, double lambda, double nu, double c, double floor_var);
/* counts[EST_PRED_N] (offset index EST_PRED_K = change 0) -> E0 model */
void c3_make_e0(c3_model *m, const uint32_t *counts, uint64_t n);
const char *c3_name(int id);   /* "E0", "F1".."F5" */

/* horizon-1 changes (mC) of scored ticks; returns count (<= cap) */
size_t c3_h1_changes(const c3_ticks *tk, double *e, size_t cap);
void c3_e0_counts(const double *e, size_t n, uint32_t *counts);

/* ---- statistics ---- */
enum { C3_ST_COV50, C3_ST_COV80, C3_ST_COV95, C3_ST_PIT0, C3_ST_BIAS = C3_ST_PIT0 + 10, C3_ST_LAG1,
       C3_ST_Q0, C3_ST_REG_TRIAL = C3_ST_Q0 + 4, C3_ST_REG_IDLE, C3_ST_TEN, C3_ST_N, C3_ST_COUNT };
/* order = protocol section 6 rule order (rule 5: in-trial before idle); c3_judge
 * names the first failing statistic in this order */
const char *c3_stat_name(int s);

typedef struct {
    int error; char errmsg[160];
    size_t n;                 /* scored one-step steps */
    size_t unscorable;        /* valid, L >= burn-in, predict refused (gap) */
    double logsum, logscore;  /* mean log score */
    est_calib c;
    size_t cap; double *cov95_step; double *width_step; unsigned char *regime_step;
    double ten_cov; size_t ten_n;
    double value[C3_ST_COUNT];
    double band_lo[C3_ST_COUNT], band_hi[C3_ST_COUNT];
    int gated[C3_ST_COUNT], pass[C3_ST_COUNT];
    size_t sub_n[C3_ST_COUNT];          /* quarter / regime / ten-step counts */
    double width_mean, width_median;
    int calibrated; int first_fail;     /* -1 when calibrated */
} c3_stats;

/* full = 0: log score only (grid search). full = 1: every section 6 statistic. */
int c3_score(const c3_ticks *tk, const c3_marks *mk, const c3_model *m, int full, c3_stats *st);
void c3_stats_free(c3_stats *st);
/* applies the section 6 rules to the accumulated values (called by c3_score) */
void c3_judge(c3_stats *st);

/* ---- precheck (section 7); reads only t, loadavg, cpu, marks, schedule ---- */
typedef struct {
    size_t lines, gaps, gaps_big, bad_t; double gap_big_frac;
    int have_schedule; size_t sched_segments, sched_trials;
    int marks_ok; char marks_why[160];
    size_t foreign_n, foreign_unmeasured, foreign_over3;
    double foreign_mean, foreign_over3_frac, loadavg1_mean; size_t loadavg_n;
    int ok_lines, ok_gaps, ok_marks, ok_foreign_mean, ok_foreign_tail, ok_foreign_measured;
    int valid; char why[256];
    char raw_sha[65], marks_sha[65], sched_sha[65];
} c3_pre;
int c3_precheck(const char *dir, c3_pre *p, char *err, size_t cap);
/* field extractor: finds top-level "key": value in one ndjson line without
 * converting any other field's value. Returns 0 and the value span (quotes
 * stripped) when found. */
int c3_json_field(const char *line, size_t len, const char *key, const char **v, size_t *vl);

/* ---- parameter file ---- */
typedef struct {
    int phase_a_pass;           /* 1 PASS, 0 PHASE_A_FAIL */
    int selected;               /* 1..5, 0 none */
    int have[6];
    est_assumption fam[6];
    double logscore[6];
    int screen_pass[6];
    char protocol_sha[65], tool_commit[80];
    int tool_dirty, synthetic;
    char fit_raw_path[1024], fit_raw_sha[65], fit_marks_path[1024], fit_marks_sha[65];
    size_t fit_lines;
    uint64_t e0_n;
    uint32_t e0_count[EST_PRED_N];
} c3_params;
int c3_params_read(const char *path, c3_params *p, char *err, size_t cap);
int c3_model_from_params(const c3_params *p, int id, c3_model *m);

/* ---- helpers ---- */
int c3_path_is_heldout(const char *path);
/* writes SHA256SUMS for the named files in dir (sha256sum text format) */
int c3_write_sums(const char *dir, const char *const *names, size_t n);
/* synthetic run: iid grid changes, schedule + marks + cpu consistent with it */
typedef struct {
    uint64_t seed;
    size_t lines;
    double foreign;          /* extra busy cores on every sample */
    size_t gap_every;        /* every Nth line comes after a 3 s gap (0: none) */
    int drop_end_mark;       /* drop the end mark of the first trial */
    int heavy_trial_noise;   /* 1: trials have wider changes */
    size_t spike_every;      /* every Nth sample gets 4 extra foreign busy cores (0: none) */
} c3_synth;
int c3_synth_write(const char *dir, const c3_synth *s);
void c3_json_str(FILE *fp, const char *s);
/* Strict-JSON receipt rules (est-json-1, docs/estimation/RECEIPT_JSON.md): a receipt never
 * contains inf or nan. c3_json_num writes a finite double with %.17g, anything else as null.
 * c3_json_stats writes one score block: infinite thresholds as null + lo/hi_unbounded true,
 * unmeasured values as null + a status, and every non-finite MEASURED value as null + listed
 * in "invalid_fields". c3_stats_invalid counts those invalid fields; a caller must make its
 * verdict fail when it is non-zero. ten_mode: ABSENT omits the ten-step score field,
 * UNAVAILABLE writes null + status "unavailable" (a baseline that has none), MEASURED writes
 * the number, or null + status "invalid" when it is not finite. */
enum { C3_TEN_ABSENT = 0, C3_TEN_UNAVAILABLE = 1, C3_TEN_MEASURED = 2 };
void c3_json_num(FILE *fp, double v);
size_t c3_stats_invalid(const c3_stats *st, int ten_mode, double ls10);
size_t c3_json_stats(FILE *o, const char *name, const c3_stats *st, int ten_mode, double ls10);
/* grid search: take a point when it is the first finite one or strictly better
 * (ties keep the earlier point in listed order). */
int c3_grid_take(int have, double ls, double best);
/* section 5 selection: pass[1..5], ls[1..5]; best log score among passers, an
 * earlier family within C3_TIE_NATS preferred. 0 = none (PHASE_A_FAIL). */
int c3_select(const int *avail, const int *pass, const double *ls);

#endif
