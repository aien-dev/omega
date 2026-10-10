/* Tests of the AT-1 candidate engine (Agent 3). Portable C11 plus POSIX system() status macros.
 *
 *   build/test_model <cases-dir> <at1-model binary>
 *
 * 1. Contract worked example (AT1_CASE_V1 section 6): file digest, case_id, acceptance_id, and
 *    the generator reproduces the file byte for byte.
 * 2. Every AT1_SPEC section 13 class: exact tables compared with the engine values in exact
 *    rational arithmetic (|value - exact| <= written bound), kernel dimension, label status,
 *    full AT1_RESULT_V1 written with an oracle record built from the exact tables, and the
 *    verdict (12 checks, outcome, failure codes, expectation_met) compared with section 13.
 * 3. Refusals R0..R14 of section 13.5, the 42 inherited AT-0 rows, and charter readings d, e.
 * 4. Invariance tests T4..T9 of AT1_SPEC section 9.1; a large N = 64, M = 256 case.
 * 5. Tool behaviour: oracle record and provenance checks, ERROR results, CLI exit codes.
 * Prints the measured worst error-to-bound ratios used in README.md. */
#define _POSIX_C_SOURCE 200809L
#include "at1_model.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static int g_fail = 0, g_pass = 0;
static const char *g_cases, *g_tool;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* measured worst cases over every exact comparison */
static double g_ratio_p = 0, g_ratio_pauli = 0, g_max_err_p = 0, g_max_err_pauli = 0;
static double g_max_con = 0, g_max_con_bound_ratio = 0, g_max_povm = 0;

/* ---- text helpers ------------------------------------------------------------------------- */
typedef struct { char *p; size_t n, cap; } sb;
static void sb_put(sb *b, const char *s)
{
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) { b->cap = (b->n + l + 1) * 2 + 256; b->p = realloc(b->p, b->cap); if (!b->p) abort(); }
    memcpy(b->p + b->n, s, l); b->n += l; b->p[b->n] = 0;
}
static void sb_printf(sb *b, const char *fmt, ...)
{
    char t[8192]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap); sb_put(b, t);
}

static char *read_all(const char *path, size_t *len)
{
    uint8_t *b; size_t n;
    if (at1_read_file(path, &b, &n) != AT1_OK) return NULL;
    char *s = malloc(n + 1); memcpy(s, b, n); s[n] = 0; free(b);
    if (len) *len = n;
    return s;
}
static void write_all(const char *path, const char *s, size_t n)
{
    FILE *f = fopen(path, "wb"); if (!f) { printf("cannot write %s\n", path); exit(1); }
    fwrite(s, 1, n, f); fclose(f);
}

/* replace the first line equal to `old` (without LF) by `rep` (zero or more LF-terminated
 * lines); returns a malloc'd string or NULL if not found */
static char *replace_line(const char *text, const char *old, const char *rep)
{
    size_t lo = strlen(old);
    for (const char *p = text; *p; ) {
        const char *e = strchr(p, '\n'); if (!e) break;
        if ((size_t)(e - p) == lo && strncmp(p, old, lo) == 0) {
            size_t pre = (size_t)(p - text), rl = strlen(rep), post = strlen(e + 1);
            char *r = malloc(pre + rl + post + 1);
            memcpy(r, text, pre); memcpy(r + pre, rep, rl); memcpy(r + pre + rl, e + 1, post + 1);
            return r;
        }
        p = e + 1;
    }
    return NULL;
}
/* the first line that starts with `prefix` */
static char *line_with_prefix(const char *text, const char *prefix, char *out, size_t cap)
{
    size_t lp = strlen(prefix);
    for (const char *p = text; *p; ) {
        const char *e = strchr(p, '\n'); if (!e) break;
        if (strncmp(p, prefix, lp) == 0) { size_t l = (size_t)(e - p); if (l >= cap) l = cap - 1; memcpy(out, p, l); out[l] = 0; return out; }
        p = e + 1;
    }
    return NULL;
}

/* recompute case_id and acceptance_id from the blocks present in the text (in place) */
static void restamp(char *text)
{
    const char *bs = strstr(text, "begin semantic\n"), *es = strstr(text, "\nend semantic\n");
    const char *ba = strstr(text, "begin acceptance\n"), *ea = strstr(text, "\nend acceptance\n");
    char cid[65], aid[65];
    if (bs && es && es > bs) {
        at1_tagged_sha256("omega.at1.case.v1", bs, (size_t)(es + 14 - bs), cid);
        char *p = strstr(text, "\ncase_id "); if (p && strlen(p + 9) >= 64) memcpy(p + 9, cid, 64);
    }
    if (ba && ea && ea > ba) {
        at1_tagged_sha256("omega.at1.acceptance.v1", ba, (size_t)(ea + 16 - ba), aid);
        char *p = strstr(text, "\nacceptance_id "); if (p && strlen(p + 15) >= 64) memcpy(p + 15, aid, 64);
    }
}

/* ---- case generator ------------------------------------------------------------------------ */
typedef struct {
    const char *name; int N; const char *E; const char *h; const char *v[AT1_CLOCK_DIM_MAX];
    const char *ref; const char *psi; const char *tau; const char *w; int M;
    const char *control, *target, *expected, *codes, *minbk, *tolz;
} spec;

static void base_B(spec *s, const char *name)
{
    memset(s, 0, sizeof *s);
    s->name = name; s->N = 4; s->E = "-3/2,-1/2,1/2,3/2"; s->h = "0/1,0/1,0/1,1/2";
    s->ref = "t0"; s->psi = "(1/1;0/1),(1/1;0/1)"; s->tau = "1/4"; s->w = "1/1"; s->M = 4;
    s->control = "POSITIVE"; s->target = "INTERACTING"; s->expected = "PASS"; s->codes = "none";
    s->minbk = "ESTIMATED"; s->tolz = "1@9";
}
static const char *RHO = "3/10,0/1,-1/10";

static char *gen_case(const spec *s)
{
    sb b = { 0 };
    sb_printf(&b, "OMEGA-AT1-CASE v1\ndomain omega.at1.case.v1\ncontract AT1_CASE_V1\ncase_name %s\n", s->name);
    sb_printf(&b, "begin semantic\nmodel_family PAGE_WOOTTERS_FINITE_CLOCKDIAG\nenergy_unit DIMENSIONLESS_HBAR_1\n");
    sb_printf(&b, "clock_dim %d\nclock_energies %s\nsystem_dim 2\nsystem_hamiltonian_pauli %s\ninteraction CLOCK_DIAGONAL_PAULI\n", s->N, s->E, s->h);
    for (int j = 0; j < s->N; j++) sb_printf(&b, "interaction_pauli %d %s\n", j, (j < AT1_CLOCK_DIM_MAX && s->v[j]) ? s->v[j] : "0/1,0/1,0/1");
    sb_printf(&b, "constraint SUM_HC_HS_V\nphysical_state NULLSPACE_PROJECTION\nreference_clock_label %s\nreference_system_state %s\n", s->ref, s->psi);
    sb_printf(&b, "clock_povm COVARIANT_DISCRETE\npovm_tau_turns %s\npovm_weight %s\nclock_label_count %d\n", s->tau, s->w, s->M);
    for (int k = 0; k < s->M; k++) sb_printf(&b, "clock_label %d t%d\n", k, k);
    sb_printf(&b, "observables PAULI_X,PAULI_Y,PAULI_Z\nend semantic\nbegin acceptance\n");
    sb_printf(&b, "control_kind %s\nprediction_target %s\nexpected_outcome %s\nexpected_failure_codes %s\nmin_bound_kind %s\n", s->control, s->target, s->expected, s->codes, s->minbk);
    sb_printf(&b, "tol_constraint_residual 1@12\ntol_povm_residual 1@12\ntol_probability 1@12\ntol_zero_probability %s\ntol_schrodinger 1@12\nend acceptance\n", s->tolz);
    sb_printf(&b, "case_id %064d\nacceptance_id %064d\nend\n", 0, 0);
    restamp(b.p);
    return b.p;
}

/* ---- exact table values ------------------------------------------------------------------- */
/* "a/b", "a", or "~a" / "~b" for 1/2 +- 3 sqrt(3)/16 (N4b, irrational) */
static int parse_rat(const char *s, bq *out, double *approx)
{
    if (s[0] == '~') { *approx = s[1] == 'a' ? 0.5 + 3.0 * sqrt(3.0) / 16.0 : 0.5 - 3.0 * sqrt(3.0) / 16.0; return 0; }
    int neg = s[0] == '-'; if (neg) s++;
    const char *sl = strchr(s, '/');
    bn n, d; bn_init(&n); bn_init(&d);
    bn_from_digits(&n, s, sl ? (size_t)(sl - s) : strlen(s));
    if (sl) bn_from_digits(&d, sl + 1, strlen(sl + 1)); else bn_set_u64(&d, 1);
    if (neg) bn_neg(&n, &n);
    bq_set_bn(out, &n, &d);
    bn_free(&n); bn_free(&d);
    *approx = bq_to_double(out);
    return 1;
}

typedef struct { const char *p[4], *x[4], *y[4], *z[4]; } table4;
static const table4 T_P1 = { { "1/4", "1/4", "1/4", "1/4" }, { "1", "1/2", "0", "1/2" }, { "1/2", "1", "1/2", "0" }, { "1/2", "1/2", "1/2", "1/2" } };
static const table4 T_P2 = { { "5/28", "1/4", "9/28", "1/4" }, { "49/50", "29/70", "1/10", "29/70" }, { "1/2", "13/14", "1/2", "1/14" }, { "16/25", "26/35", "4/5", "26/35" } };
static const table4 T_P3 = { { "107/280", "53/280", "5/56", "19/56" }, { "98/107", "37/53", "8/25", "37/95" }, { "65/214", "101/106", "37/50", "17/190" }, { "65/214", "61/106", "9/10", "29/38" } };
static const table4 T_P3b = { { "41/80", "19/80", "1/80", "19/80" }, { "29/82", "1/38", "1/2", "37/38" }, { "40/41", "10/19", "0", "10/19" }, { "45/82", "25/38", "1/2", "13/38" } };
static const table4 I_P3b = { { 0 }, { "1/2", "1/2", "1/2", "1/2" }, { "1", "0", "1", "0" }, { "1/2", "1/2", "1/2", "1/2" } };
static const table4 T_P4 = { { "29/164", "37/164", "53/164", "45/164" }, { "277/290", "197/370", "37/530", "13/50" }, { "17/58", "73/74", "65/106", "1/10" }, { "73/145", "113/185", "193/265", "17/25" } };
static const table4 I_P4 = { { 0 }, { "9/10", "4/5", "1/10", "1/5" }, { "1/5", "9/10", "4/5", "1/10" }, { "1/2", "1/2", "1/2", "1/2" } };
static const table4 T_P5 = { { "524181/2096716", "522731/2096716", "524177/2096716", "525627/2096716" },
    { "274763624041/549527248074", "274761527333/548007134774", "274759430625/549523054658", "274761527333/551043167958" },
    { "274004612845/549527248074", "274004612845/548007134774", "275520532729/549523054658", "275520532729/551043167958" },
    { "524180/524181", "522730/522731", "524176/524177", "525626/525627" } };
static const table4 T_P6 = { { "5/28", "9/28", "5/28", "9/28" }, { "49/50", "1/10", "49/50", "1/10" }, { "1/2", "1/2", "1/2", "1/2" }, { "16/25", "4/5", "16/25", "4/5" } };
static const table4 T_N3 = { { "1/2", "1/4", "0", "1/4" }, { "1/2", "1/2", NULL, "1/2" }, { "1/2", "1/2", NULL, "1/2" }, { "1", "1", NULL, "1" } };
static const table4 T_N4a = { { "5/56", "1/8", "9/56", "1/8" }, { "49/50", "29/70", "1/10", "29/70" }, { "1/2", "13/14", "1/2", "1/14" }, { "16/25", "26/35", "4/5", "26/35" } };
static const table4 T_N4b = { { "5/28", "2/7", "2/7", "5/28" }, { "49/50", "19/80", "19/80", "49/50" }, { "1/2", "~a", "~b", "1/2" }, { "16/25", "31/40", "31/40", "16/25" } };
static const table4 I_N6 = { { 0 }, { "4/5", "1/2", "1/5", "1/2" }, { "1/2", "4/5", "1/2", "1/5" }, { "9/10", "9/10", "9/10", "9/10" } };
/* conditioning control (not a section 13 row): one kernel level, psi_0 nearly orthogonal to its vector */
static const table4 T_C1 = { { "1/4", "1/4", "1/4", "1/4" }, { "1/5", "1/5", "1/5", "1/5" }, { "1/2", "1/2", "1/2", "1/2" }, { "1/10", "1/10", "1/10", "1/10" } };

/* |value - exact| <= bound (exact rationals); returns the error and the error/bound ratio */
static int within(double value, double bound, const char *exact, double *err, double *ratio)
{
    bq e, v, b, d; bq_init(&e); bq_init(&v); bq_init(&b); bq_init(&d);
    double approx; int rational = parse_rat(exact, &e, &approx);
    char t[400]; at1_bound_format(bound, t, sizeof t);
    char *at = strchr(t, '@'); bn bN; bn_init(&bN); bn_from_digits(&bN, t, (size_t)(at - t));
    bq_from_scaled(&b, &bN, (unsigned)atoi(at + 1)); bn_free(&bN);
    if (!rational) {          /* irrational entry: allow the reference double's own rounding */
        bq_from_double(&e, approx);
        bq u; bq_init(&u); bq_from_double(&u, 4 * 1.1102230246251565e-16 * fabs(approx)); bq_add(&b, &b, &u); bq_free(&u);
    }
    bq_from_double(&v, value); bq_sub(&d, &v, &e); bq_abs(&d, &d);
    int ok = bq_cmp(&d, &b) <= 0;
    *err = bq_to_double(&d);
    *ratio = bound > 0 ? *err / bound : (*err == 0 ? 0 : INFINITY);
    bq_free(&e); bq_free(&v); bq_free(&b); bq_free(&d);
    return ok;
}
/* 1 - x for a table string, as a string */
static void one_minus(const char *x, char *out, size_t cap)
{
    if (x[0] == '~') { snprintf(out, cap, "~%c", x[1] == 'a' ? 'b' : 'a'); return; }
    bq e, one; bq_init(&e); bq_init(&one); double a; parse_rat(x, &e, &a); bq_set_i64(&one, 1, 1); bq_sub(&e, &one, &e);
    /* print as num/den */
    sb b = { 0 };
    bn ten, q, r, cur; bn_init(&ten); bn_init(&q); bn_init(&r); bn_init(&cur); bn_set_u64(&ten, 10);
    for (int part = 0; part < 2; part++) {
        bn_abs(&cur, part ? &e.den : &e.num);
        char dg[128]; int nd = 0;
        if (bn_is_zero(&cur)) dg[nd++] = '0';
        while (!bn_is_zero(&cur)) { bn_divmod(&q, &r, &cur, &ten); dg[nd++] = (char)('0' + (r.n ? r.d[0] : 0)); bn_copy(&cur, &q); }
        char rev[128]; for (int i = 0; i < nd; i++) rev[i] = dg[nd - 1 - i]; rev[nd] = 0;
        if (part == 0 && bn_sign(&e.num) < 0) sb_put(&b, "-");
        sb_put(&b, rev); if (part == 0) sb_put(&b, "/");
    }
    snprintf(out, cap, "%s", b.p); free(b.p);
    bn_free(&ten); bn_free(&q); bn_free(&r); bn_free(&cur); bq_free(&e); bq_free(&one);
}
static const char *tab_entry(const table4 *t, int k, int q, char *buf, size_t cap)
{
    const char *const *col = q / 2 == 0 ? t->x : q / 2 == 1 ? t->y : t->z;
    if (!col[k]) return NULL;
    if (q % 2 == 0) return col[k];
    one_minus(col[k], buf, cap); return buf;
}

/* ---- oracle record and provenance --------------------------------------------------------------- */
static const char *AX[3] = { "X", "Y", "Z" }, *SG[2] = { "PLUS", "MINUS" };
static void f64_of(const char *exact, char *out, size_t cap)
{
    bq e; bq_init(&e); double a; parse_rat(exact, &e, &a); bq_free(&e);
    at1_f64_format(a, out, cap);
}
/* reference lines from exact tables; status from the exact interacting marginal (0 -> UNDEFINED) */
static char *oracle_record(const at1_case *c, const table4 *inter, const table4 *ideal, int trivial)
{
    sb b = { 0 }; char v[64], buf[128];
    sb_printf(&b, "case_id %s\nacceptance_id %s\n", c->case_id, c->acceptance_id);
    int M = c->label_count, def[AT1_LABEL_MAX];
    for (int k = 0; k < M; k++) {
        def[k] = !trivial && strcmp(inter->p[k], "0") != 0;
        sb_printf(&b, "reference_label %d %s %s\n", k, c->labels[k], def[k] ? "DEFINED" : "UNDEFINED");
    }
    for (int k = 0; k < M; k++) {
        if (trivial) { sb_printf(&b, "reference_clock_probability %d undefined 0@0\n", k); continue; }
        f64_of(inter->p[k], v, sizeof v); sb_printf(&b, "reference_clock_probability %d %s 1@15\n", k, v);
    }
    for (int f = 0; f < 2; f++) for (int k = 0; k < M; k++) for (int q = 0; q < 6; q++) {
        const char *key = f == 0 ? "reference_ideal" : "reference_interacting";
        const char *e = tab_entry(f == 0 ? ideal : inter, k, q, buf, sizeof buf);
        if (f == 1 && (!def[k] || !e)) { sb_printf(&b, "%s %d %s %s undefined 0@0\n", key, k, AX[q / 2], SG[q % 2]); continue; }
        f64_of(e, v, sizeof v); sb_printf(&b, "%s %d %s %s %s 1@15\n", key, k, AX[q / 2], SG[q % 2], v);
    }
    return b.p;
}
static const char *PROV =
    "source_repo aien-dev/omega\n"
    "source_commit 0123456789abcdef0123456789abcdef01234567\n"
    "source_tree_clean YES\n"
    "contract_commit ea91d7c0000000000000000000000000000000aa\n"
    "engine_sha256 bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
    "oracle_repo aien-dev/omega\n"
    "oracle_commit cccccccccccccccccccccccccccccccccccccccc\n"
    "oracle_sha256 dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\n"
    "build_cc cc test harness\n"
    "build_flags -std=c11 -O2\n"
    "host test host\n"
    "run_started_utc 2026-10-09T00:00:00Z\n"
    "run_finished_utc 2026-10-09T00:00:01Z\n"
    "artifact a/one 1111111111111111111111111111111111111111111111111111111111111111\n"
    "artifact b/two 2222222222222222222222222222222222222222222222222222222222222222\n";

/* ---- verdict inspection --------------------------------------------------------------------- */
static void copy_str(char *dst, size_t cap, const char *src) { size_t l = strlen(src); if (l >= cap) l = cap - 1; memcpy(dst, src, l); dst[l] = 0; }
static void verdict_of(const char *res, char checks[13], char *outcome, size_t ocap, char *codes, size_t ccap, char *met, size_t mcap)
{
    int n = 0; char line[1024];
    for (const char *p = strstr(res, "begin verdict\n"); p && *p; ) {
        const char *e = strchr(p, '\n'); size_t l = (size_t)(e - p); if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, p, l); line[l] = 0; p = e + 1;
        if (strncmp(line, "check ", 6) == 0 && n < 12) {
            const char *st = strrchr(line, ' ') + 1;
            checks[n++] = st[0] == 'P' ? 'P' : st[0] == 'F' ? 'F' : st[0] == 'I' ? 'I' : 'N';
        }
        if (strncmp(line, "outcome ", 8) == 0) copy_str(outcome, ocap, line + 8);
        if (strncmp(line, "failure_codes ", 14) == 0) copy_str(codes, ccap, line + 14);
        if (strncmp(line, "expectation_met ", 16) == 0) copy_str(met, mcap, line + 16);
        if (strcmp(line, "end verdict") == 0) break;
    }
    checks[n] = 0;
}
/* structure: line count, verdict_id and evidence_digest recomputed */
static void check_structure(const char *name, const at1_case *c, const char *res, size_t len)
{
    int M = c->label_count, lines = 0;
    for (size_t i = 0; i < len; i++) lines += res[i] == '\n';
    int sem = 0, acc = 0;
    for (size_t i = 0; i < c->semantic_len; i++) sem += c->semantic_text[i] == '\n';
    for (size_t i = 0; i < c->acceptance_len; i++) acc += c->acceptance_text[i] == '\n';
    int expect = 5 + sem + acc + 3 + 5 + (2 + 3 + 8 * M + 14 * M) + (2 + 12 + 4) + 1 + (2 + 13 + 2) + 2;
    CHECK(lines == expect, "%s: %d result lines, expected %d", name, lines, expect);
    CHECK(res[len - 1] == '\n' && strstr(res, "\nend\n") == res + len - 5, "%s: result does not end with end", name);
    const char *bv = strstr(res, "begin verdict\n"), *ev = strstr(res, "end verdict\n");
    char vid[65], ev2[65], line[200];
    size_t vlen = (size_t)(ev + 12 - bv);
    sb pre = { 0 }; sb_printf(&pre, "case_id %s\nacceptance_id %s\n", c->case_id, c->acceptance_id);
    char *tmp = malloc(vlen + 1); memcpy(tmp, bv, vlen); tmp[vlen] = 0; sb_put(&pre, tmp); free(tmp);
    at1_tagged_sha256("omega.at1.verdict.v1", pre.p, pre.n, vid); free(pre.p);
    CHECK(line_with_prefix(res, "verdict_id ", line, sizeof line) && strcmp(line + 11, vid) == 0, "%s: verdict_id", name);
    const char *ed = strstr(res, "evidence_digest ");
    at1_tagged_sha256("omega.at1.evidence.v1", res, (size_t)(ed - res), ev2);
    CHECK(strncmp(ed + 16, ev2, 64) == 0, "%s: evidence_digest", name);
    size_t badbyte = len;
    for (size_t i = 0; i < len && badbyte == len; i++) if (!(res[i] == '\n' || (res[i] >= 0x20 && res[i] <= 0x7e))) badbyte = i;
    CHECK(badbyte == len, "%s: byte outside 0x20..0x7E at offset %zu", name, badbyte);
}

/* ---- one section-13 case end to end ------------------------------------------------------------ */
typedef struct {
    const char *id; spec s; const table4 *inter, *ideal; int kdim; int trivial;
    const char *checks, *outcome, *codes;
} kase;

static void run_case(const kase *K)
{
    char *text = gen_case(&K->s);
    at1_case c; at1_kernel k; at1_values *v = calloc(1, sizeof *v);
    at1_status st = at1_case_parse((const uint8_t *)text, strlen(text), &c);
    CHECK(st == AT1_OK, "%s: case refused %s", K->id, at1_status_name(st));
    if (st != AT1_OK) { free(text); free(v); return; }
    st = at1_kernel_build(&c, &k);
    CHECK(st == AT1_OK, "%s: kernel build %d", K->id, st);
    if (st != AT1_OK) { at1_case_free(&c); free(text); free(v); return; }
    CHECK(k.kernel_dim == K->kdim, "%s: kernel_dim %d expected %d", K->id, k.kernel_dim, K->kdim);
    st = at1_compute(&c, &k, 0, v);
    CHECK(st == AT1_OK, "%s: compute %d", K->id, st);
    CHECK(v->trivial == K->trivial, "%s: trivial %d", K->id, v->trivial);
    double err, ratio; char buf[128];
    CHECK(!v->povm.undefined && v->povm.value >= 0, "%s: povm residual written", K->id);
    if (v->povm.value < 0.5 && v->povm.value > g_max_povm) g_max_povm = v->povm.value;
    if (!K->trivial) {
        if (v->constraint.value > g_max_con) g_max_con = v->constraint.value;
        if (v->constraint.value / v->constraint.bound > g_max_con_bound_ratio) g_max_con_bound_ratio = v->constraint.value / v->constraint.bound;
        CHECK(v->constraint.value <= v->constraint.bound, "%s: constraint residual %.3g above its bound %.3g", K->id, v->constraint.value, v->constraint.bound);
        for (int q = 0; q < c.label_count; q++) {
            const char *pe = K->inter->p[q];
            int ok = within(v->clock[q].value, v->clock[q].bound, pe, &err, &ratio);
            CHECK(ok, "%s: p(%d) = %.17g, exact %s, err %.3g, bound %.3g", K->id, q, v->clock[q].value, pe, err, v->clock[q].bound);
            if (ratio > g_ratio_p) g_ratio_p = ratio;
            if (err > g_max_err_p) g_max_err_p = err;
            at1_label_status want = strcmp(pe, "0") == 0 ? AT1_LABEL_UNDEFINED : AT1_LABEL_DEFINED;
            CHECK(v->status[q] == want, "%s: label %d status %d", K->id, q, v->status[q]);
            if (want != AT1_LABEL_DEFINED) {
                for (int s = 0; s < 6; s++) CHECK(v->pauli[q][s].undefined, "%s: pauli of undefined label %d written", K->id, q);
                continue;
            }
            for (int s = 0; s < 6; s++) {
                const char *e = tab_entry(K->inter, q, s, buf, sizeof buf);
                ok = within(v->pauli[q][s].value, v->pauli[q][s].bound, e, &err, &ratio);
                CHECK(ok, "%s: pauli %d %s %s = %.17g, exact %s, err %.3g, bound %.3g", K->id, q, AX[s / 2], SG[s % 2], v->pauli[q][s].value, e, err, v->pauli[q][s].bound);
                if (ratio > g_ratio_pauli) g_ratio_pauli = ratio;
                if (err > g_max_err_pauli) g_max_err_pauli = err;
            }
        }
    } else {
        CHECK(v->constraint.undefined, "%s: trivial constraint_residual not undefined", K->id);
        for (int q = 0; q < c.label_count; q++) CHECK(v->status[q] == AT1_LABEL_UNDEFINED && v->clock[q].undefined, "%s: trivial label %d", K->id, q);
    }
    char *orec = oracle_record(&c, K->inter, K->ideal, K->trivial);
    char *res = NULL; size_t rlen = 0; char errs[256];
    st = at1_result_write(&c, v, orec, strlen(orec), PROV, strlen(PROV), &res, &rlen, errs, sizeof errs);
    CHECK(st == AT1_OK, "%s: result write: %s", K->id, errs);
    if (st == AT1_OK) {
        char checks[13], outcome[64] = "", codes[512] = "", met[32] = "";
        verdict_of(res, checks, outcome, sizeof outcome, codes, sizeof codes, met, sizeof met);
        CHECK(strcmp(checks, K->checks) == 0, "%s: checks %s expected %s", K->id, checks, K->checks);
        CHECK(strcmp(outcome, K->outcome) == 0, "%s: outcome %s", K->id, outcome);
        CHECK(strcmp(codes, K->codes) == 0, "%s: failure_codes %s expected %s", K->id, codes, K->codes);
        CHECK(strcmp(met, "YES") == 0, "%s: expectation_met %s", K->id, met);
        check_structure(K->id, &c, res, rlen);
        printf("case %-6s kernel_dim %d  checks %s  outcome %s  codes %s  expectation_met %s  constraint %.3g (bound %.3g)\n", K->id, k.kernel_dim, checks, outcome, codes, met, v->constraint.undefined ? 0.0 : v->constraint.value, v->constraint.undefined ? 0.0 : v->constraint.bound);
    }
    free(res); free(orec); at1_kernel_free(&k); at1_case_free(&c); free(text); free(v);
}

static void section13(void)
{
    kase L[32]; int n = 0;
#define NEW(idv, nm) (memset(&L[n], 0, sizeof L[n]), L[n].id = idv, base_B(&L[n].s, nm), &L[n])
    kase *K;
    K = NEW("P1a", "at1-p1a-zero-coupling"); K->inter = &T_P1; K->ideal = &T_P1; K->kdim = 2; K->checks = "PPPPPPPPPPNP"; K->outcome = "PASS"; K->codes = "none"; n++;
    K = NEW("P1b", "at1-p1b-zero-coupling-ideal"); K->s.target = "IDEAL"; K->inter = &T_P1; K->ideal = &T_P1; K->kdim = 2; K->checks = "PPPPPPPPPPPP"; K->outcome = "PASS"; K->codes = "none"; n++;
    K = NEW("P1c", "at1-p1c-spectator"); K->s.target = "IDEAL"; K->s.v[0] = "0/1,0/1,1/2"; K->s.v[3] = RHO; K->inter = &T_P1; K->ideal = &T_P1; K->kdim = 2; K->checks = "PPPPPPPPPPPP"; K->outcome = "PASS"; K->codes = "none"; n++;
    K = NEW("P2", "at1-kat-rotated-level-n4"); K->s.v[2] = RHO; K->inter = &T_P2; K->ideal = &T_P1; K->kdim = 2; K->checks = "PPPPPPPPPPNP"; K->outcome = "PASS"; K->codes = "none"; n++;
    K = NEW("P3", "at1-p3-every-level"); K->s.N = 3; K->s.E = "-1/2,1/2,3/2"; K->s.v[0] = RHO; K->s.v[1] = "0/1,3/10,-1/10"; K->s.v[2] = "6/5,0/1,2/5"; K->s.w = "3/4";
        K->inter = &T_P3; K->ideal = &T_P1; K->kdim = 3; K->checks = "PPPPPPPPPPNP"; K->outcome = "PASS"; K->codes = "none"; n++;
    K = NEW("P3b", "at1-p3b-degenerate"); K->s.N = 3; K->s.E = "-1/2,0/1,1/2"; K->s.v[1] = "0/1,0/1,-1/2"; K->s.v[2] = RHO; K->s.psi = "(1/1;0/1),(0/1;1/1)"; K->s.tau = "1/2"; K->s.w = "3/4";
        K->inter = &T_P3b; K->ideal = &I_P3b; K->kdim = 4; K->checks = "PPPPPPPPPPNP"; K->outcome = "PASS"; K->codes = "none"; n++;
    K = NEW("P4", "at1-p4-complex-psi-r1"); K->s.v[2] = RHO; K->s.psi = "(1/1;0/1),(3/5;4/5)"; K->s.ref = "t1";
        K->inter = &T_P4; K->ideal = &I_P4; K->kdim = 2; K->checks = "PPPPPPPPPPNP"; K->outcome = "PASS"; K->codes = "none"; n++;
    K = NEW("P5", "at1-p5-large-rationals"); K->s.N = 2; K->s.E = "-524289/1048574,524285/1048574"; K->s.h = "1/524287,0/1,0/1,1/2"; K->s.v[1] = "524175/1048354,724/524177,-1/2"; K->s.w = "1/2";
        K->inter = &T_P5; K->ideal = &T_P1; K->kdim = 2; K->checks = "PPPPPPPPPPNP"; K->outcome = "PASS"; K->codes = "none"; n++;
    K = NEW("P6", "at1-p6-near-miss"); K->s.v[0] = "0/1,0/1,1/1"; K->s.v[1] = "0/1,0/1,1/1048576"; K->s.v[2] = RHO;
        K->inter = &T_P6; K->ideal = &T_P1; K->kdim = 2; K->checks = "PPPPPPPPPPNP"; K->outcome = "PASS"; K->codes = "none"; n++;
    /* N1 family: the positive cases above with target IDEAL */
    static const char *n1id[6] = { "N1", "N1b", "N1c", "N1d", "N1e", "N1f" };
    static const char *n1nm[6] = { "at1-n1-p2-ideal", "at1-n1b-p3-ideal", "at1-n1c-p3b-ideal", "at1-n1d-p4-ideal", "at1-n1e-p5-ideal", "at1-n1f-p6-ideal" };
    for (int i = 0; i < 6; i++) {
        L[n] = L[3 + i]; L[n].id = n1id[i]; L[n].s.name = n1nm[i]; L[n].s.target = "IDEAL"; L[n].s.control = "NEGATIVE"; L[n].s.expected = "FAIL";
        L[n].s.codes = "SCHRODINGER_DEVIATION_EXCEEDED"; L[n].checks = "PPPPPPPPPFPP"; L[n].outcome = "FAIL"; L[n].codes = "SCHRODINGER_DEVIATION_EXCEEDED"; n++;
    }
    const char *TRIV = "PPFNPNNNNNNN";
    K = NEW("N2", "at1-n2-all-out"); K->s.v[1] = "0/1,0/1,1/2"; K->s.v[2] = "0/1,0/1,1/2"; K->s.control = "NEGATIVE"; K->s.expected = "FAIL"; K->s.codes = "TRIVIAL_PHYSICAL_STATE";
        K->inter = &T_P1; K->ideal = &T_P1; K->kdim = 0; K->trivial = 1; K->checks = TRIV; K->outcome = "FAIL"; K->codes = "TRIVIAL_PHYSICAL_STATE"; n++;
    K = NEW("N3", "at1-n3-zero-marginal"); K->s.v[2] = "0/1,0/1,-1/1"; K->s.control = "NEGATIVE"; K->s.expected = "FAIL"; K->s.codes = "CONDITIONAL_UNDEFINED";
        K->inter = &T_N3; K->ideal = &T_P1; K->kdim = 2; K->checks = "PPPPPPPPFPNP"; K->outcome = "FAIL"; K->codes = "CONDITIONAL_UNDEFINED"; n++;
    K = NEW("N4a", "at1-n4a-wrong-weight"); K->s.v[2] = RHO; K->s.w = "1/2"; K->s.control = "NEGATIVE"; K->s.expected = "FAIL"; K->s.codes = "POVM_NORMALIZATION_EXCEEDED,PROBABILITY_SUM_EXCEEDED";
        K->inter = &T_N4a; K->ideal = &T_P1; K->kdim = 2; K->checks = "PPPPFFPPPPNP"; K->outcome = "FAIL"; K->codes = "POVM_NORMALIZATION_EXCEEDED,PROBABILITY_SUM_EXCEEDED"; n++;
    /* N4b: target INTERACTING, so reference_ideal does not enter the verdict (P1's column is written) */
    K = NEW("N4b", "at1-n4b-broken-clock"); K->s.v[2] = RHO; K->s.tau = "1/3"; K->s.control = "NEGATIVE"; K->s.expected = "FAIL"; K->s.codes = "POVM_NORMALIZATION_EXCEEDED,PROBABILITY_SUM_EXCEEDED";
        K->inter = &T_N4b; K->ideal = &T_P1; K->kdim = 2; K->checks = "PPPPFFPPPPNP"; K->outcome = "FAIL"; K->codes = "POVM_NORMALIZATION_EXCEEDED,PROBABILITY_SUM_EXCEEDED"; n++;
    K = NEW("N5", "at1-n5-precision-demand"); K->s.v[2] = RHO; K->s.minbk = "RIGOROUS"; K->s.control = "NEGATIVE"; K->s.expected = "FAIL"; K->s.codes = "BOUND_KIND_INSUFFICIENT";
        K->inter = &T_P2; K->ideal = &T_P1; K->kdim = 2; K->checks = "FPPPPPPPPPNP"; K->outcome = "FAIL"; K->codes = "BOUND_KIND_INSUFFICIENT"; n++;
    K = NEW("N6", "at1-n6-zero-projection"); K->s.v[1] = "0/1,0/1,1/2"; K->s.v[2] = RHO; K->s.psi = "(3/1;0/1),(1/1;0/1)"; K->s.control = "NEGATIVE"; K->s.expected = "FAIL"; K->s.codes = "TRIVIAL_PHYSICAL_STATE";
        K->inter = &T_P1; K->ideal = &I_N6; K->kdim = 1; K->trivial = 1; K->checks = TRIV; K->outcome = "FAIL"; K->codes = "TRIVIAL_PHYSICAL_STATE"; n++;
    /* f^dagger psi_0 = 3/1048575: Psi is 1e-6 of psi_0, a cancellation trap for P_0 applied in binary64 */
    K = NEW("C1", "at1-c1-near-orthogonal-psi"); K->s.v[1] = "0/1,0/1,1/2"; K->s.v[2] = RHO; K->s.psi = "(3/1;0/1),(1048576/1048575;0/1)";
        K->inter = &T_C1; K->ideal = &T_P1; K->kdim = 1; K->checks = "PPPPPPPPPPNP"; K->outcome = "PASS"; K->codes = "none"; n++;
    /* energy shift +1000 cancelled by h0 = -1000: the matrix entries stay small, so P2 must still PASS */
    K = NEW("C2", "at1-c2-shifted-energies"); K->s.E = "1997/2,1999/2,2001/2,2003/2"; K->s.h = "-1000/1,0/1,0/1,1/2"; K->s.v[2] = RHO;
        K->inter = &T_P2; K->ideal = &T_P1; K->kdim = 2; K->checks = "PPPPPPPPPPNP"; K->outcome = "PASS"; K->codes = "none"; n++;
    /* declared limit: a kernel level with |E_j + h0| = R_j = 1025/2 on the rotated axis (3/5, 0, 4/5), so
     * |H_total| acting on Psi has row sums near 1230 and the binary64 residual bound (about 48 eps times
     * that) exceeds tol_constraint_residual: check 4 is INDETERMINATE. Same chi_k as P2, same table. */
    K = NEW("C3", "at1-c3-large-level-norm"); K->s.N = 2; K->s.E = "-1/2,1025/2"; K->s.v[1] = "615/2,0/1,819/2"; K->s.w = "1/2";
        K->s.control = "NEGATIVE"; K->s.expected = "FAIL"; K->s.codes = "PRECISION_INSUFFICIENT";
        K->inter = &T_P2; K->ideal = &T_P1; K->kdim = 2; K->checks = "PPPIPPPPPPNP"; K->outcome = "FAIL"; K->codes = "PRECISION_INSUFFICIENT"; n++;
#undef NEW
    for (int i = 0; i < n; i++) run_case(&L[i]);
    /* N4b povm_residual = sqrt(42)/4, N4a = 1 */
    for (int i = 0; i < n; i++) {
        if (strcmp(L[i].id, "N4a") && strcmp(L[i].id, "N4b")) continue;
        char *t = gen_case(&L[i].s); at1_case c; at1_kernel k; at1_values v;
        at1_case_parse((const uint8_t *)t, strlen(t), &c); at1_kernel_build(&c, &k); at1_compute(&c, &k, 0, &v);
        double want = L[i].id[2] == 'a' ? 1.0 : sqrt(42.0) / 4.0;
        CHECK(fabs(v.povm.value - want) <= v.povm.bound + 4e-16 * want, "%s: povm_residual %.17g expected %.17g", L[i].id, v.povm.value, want);
        at1_kernel_free(&k); at1_case_free(&c); free(t);
    }
}

/* ---- worked example ------------------------------------------------------------------------- */
static char *g_kat;
static void kat(void)
{
    char path[1024]; size_t len;
    snprintf(path, sizeof path, "%s/at1-kat-rotated-level-n4.case", g_cases);
    g_kat = read_all(path, &len);
    CHECK(g_kat != NULL, "cannot read %s", path);
    if (!g_kat) exit(1);
    char h[65]; at1_sha256_hex((const uint8_t *)g_kat, len, h);
    CHECK(strcmp(h, "76e282fecf21025c9dd675378244e90ff192822a4c49d6cb812c9fe84b3f9ba4") == 0, "KAT file sha256 %s", h);
    at1_case c;
    at1_status st = at1_case_parse((const uint8_t *)g_kat, len, &c);
    CHECK(st == AT1_OK, "KAT refused %s", at1_status_name(st));
    CHECK(strcmp(c.case_id, "890980a43bd189494c2c922d2bf0049cbffb07f1090f578e04e1142aee7525f1") == 0, "KAT case_id");
    CHECK(strcmp(c.acceptance_id, "d63246c391296c6cd381f1b654cc625be9f453c77c27c68d9eac240efd5b4d8f") == 0, "KAT acceptance_id");
    CHECK(strcmp(c.case_file_sha256, h) == 0, "KAT case_file_sha256");
    at1_case_free(&c);
    spec s; base_B(&s, "at1-kat-rotated-level-n4"); s.v[2] = RHO;
    char *g = gen_case(&s);
    CHECK(strcmp(g, g_kat) == 0, "generator does not reproduce the contract example byte for byte");
    free(g);
}

/* ---- refusals ----------------------------------------------------------------------------- */
typedef struct { const char *id; const char *old[3]; const char *rep[3]; int no_restamp; at1_status code; } refusal;

static void expect_refusal(const char *id, const char *text, size_t len, at1_status code)
{
    at1_case c;
    at1_status st = at1_case_parse((const uint8_t *)text, len, &c);
    CHECK(st == code, "%s: got %s expected %s", id, at1_status_name(st), at1_status_name(code));
    if (st == AT1_OK) at1_case_free(&c);
}

static int refusals(void)
{
    const at1_status P = AT1_CASE_PARSE_ERROR, NC = AT1_CASE_NONCANONICAL, UV = AT1_CASE_UNSUPPORTED_VERSION,
                     IP = AT1_CASE_INVALID_PARAMETER, IR = AT1_CASE_IRRATIONAL_SPECTRUM, ID = AT1_CASE_ID_MISMATCH;
    const char *L2 = "interaction_pauli 2 3/10,0/1,-1/10", *L1 = "interaction_pauli 1 0/1,0/1,0/1";
    const char *CID = "case_id 890980a43bd189494c2c922d2bf0049cbffb07f1090f578e04e1142aee7525f1";
    const char *AID = "acceptance_id d63246c391296c6cd381f1b654cc625be9f453c77c27c68d9eac240efd5b4d8f";
    const refusal R[] = {
        /* AT1_SPEC section 13.5 */
        { "R0", { "system_dim 2" }, { "" }, 0, P },
        { "R1", { "povm_tau_turns 1/4" }, { "povm_tau_turns 2/8\n" }, 0, NC },
        { "R2", { "OMEGA-AT1-CASE v1" }, { "OMEGA-AT1-CASE v2\n" }, 0, UV },
        { "R2b", { "domain omega.at1.case.v1" }, { "domain omega.at1.case.v2\n" }, 0, UV },
        { "R2c", { "contract AT1_CASE_V1" }, { "contract AT1_CASE_V2\n" }, 0, UV },
        { "R4", { L2 }, { "interaction_pauli 2 1/2,0/1,-1/10\n" }, 0, IR },
        { "R5", { CID }, { "case_id 990980a43bd189494c2c922d2bf0049cbffb07f1090f578e04e1142aee7525f1\n" }, 1, ID },
        { "R5b", { AID }, { "acceptance_id e63246c391296c6cd381f1b654cc625be9f453c77c27c68d9eac240efd5b4d8f\n" }, 1, ID },
        { "R6", { "expected_outcome PASS" }, { "expected_outcome FAIL\n" }, 0, IP },
        { "R6b", { "expected_outcome PASS", "control_kind POSITIVE", "expected_failure_codes none" }, { "expected_outcome FAIL\n", "control_kind NEGATIVE\n", "expected_failure_codes FOO_BAR\n" }, 0, IP },
        { "R7", { "interaction_pauli 3 0/1,0/1,0/1" }, { "" }, 0, P },
        { "R8", { L2, L1, "XX" }, { "XX\n", "interaction_pauli 2 3/10,0/1,-1/10\n", "interaction_pauli 1 0/1,0/1,0/1\n" }, 0, P },
        { "R9", { "interaction CLOCK_DIAGONAL_PAULI" }, { "interaction NONE\n" }, 0, P },
        { "R10", { "model_family PAGE_WOOTTERS_FINITE_CLOCKDIAG" }, { "model_family PAGE_WOOTTERS_FINITE_IDEAL\n" }, 0, P },
        { "R11", { "constraint SUM_HC_HS_V" }, { "constraint SUM_HC_HS\n" }, 0, P },
        { "R12", { "prediction_target INTERACTING" }, { "prediction_target BOTH\n" }, 0, P },
        { "R12b", { "prediction_target INTERACTING" }, { "" }, 0, P },
        { "R13", { "interaction_pauli 0 0/1,0/1,0/1" }, { "interaction_pauli 0 2/10,0/1,0/1\n" }, 0, NC },
        { "R14", { "interaction_pauli 0 0/1,0/1,0/1" }, { "interaction_pauli 0 1048577/1,0/1,0/1\n" }, 0, IP },
        /* charter readings d and e */
        { "d-at0-header", { "OMEGA-AT1-CASE v1" }, { "OMEGA-AT0-CASE v1\n" }, 0, P },
        { "d-at0-domain", { "domain omega.at1.case.v1" }, { "domain omega.at0.case.v1\n" }, 0, UV },
        { "e-index-01", { L1 }, { "interaction_pauli 01 0/1,0/1,0/1\n" }, 0, NC },
        { "e-index-x", { L1 }, { "interaction_pauli x 0/1,0/1,0/1\n" }, 0, P },
        { "e-index-5", { L1 }, { "interaction_pauli 5 0/1,0/1,0/1\n" }, 0, P },
        { "e-label-01", { "clock_label 1 t1" }, { "clock_label 01 t1\n" }, 0, NC },
        /* inherited AT-0 rows (omega bacc4b6 MANIFEST.tsv), re-expressed on P2 */
        { "R08-tab", { "clock_dim 4" }, { "clock_dim\t4\n" }, 0, P },
        { "R09-blank-line", { "begin semantic" }, { "begin semantic\n\n" }, 0, P },
        { "R10-trailing-space", { "system_dim 2" }, { "system_dim 2 \n" }, 0, P },
        { "R11-double-space", { "system_dim 2" }, { "system_dim  2\n" }, 0, P },
        { "R12-missing-key", { "interaction CLOCK_DIAGONAL_PAULI" }, { "" }, 0, P },
        { "R13-unknown-key", { "interaction CLOCK_DIAGONAL_PAULI" }, { "interaction CLOCK_DIAGONAL_PAULI\nfoo bar\n" }, 0, P },
        { "R14-duplicate-key", { "interaction CLOCK_DIAGONAL_PAULI" }, { "interaction CLOCK_DIAGONAL_PAULI\ninteraction CLOCK_DIAGONAL_PAULI\n" }, 0, P },
        { "R15-out-of-order", { "interaction CLOCK_DIAGONAL_PAULI", "constraint SUM_HC_HS_V" }, { "", "constraint SUM_HC_HS_V\ninteraction CLOCK_DIAGONAL_PAULI\n" }, 0, P },
        { "R16-float-text", { "povm_tau_turns 1/4" }, { "povm_tau_turns 0.25\n" }, 0, P },
        { "R19-label-uppercase", { "clock_label 0 t0", "reference_clock_label t0" }, { "clock_label 0 T0\n", "reference_clock_label T0\n" }, 0, P },
        { "R31-control-kind-bad", { "control_kind POSITIVE" }, { "control_kind MAYBE\n" }, 0, P },
        { "R32-case-name-two-tokens", { "case_name at1-kat-rotated-level-n4" }, { "case_name at1 kat\n" }, 0, P },
        { "R35-trailing-line", { "end" }, { "end\nextra\n" }, 0, P },
        { "R36-label-count-mismatch", { "clock_label_count 4" }, { "clock_label_count 5\n" }, 0, P },
        { "R37-model-family-bad", { "model_family PAGE_WOOTTERS_FINITE_CLOCKDIAG" }, { "model_family PAGE_WOOTTERS_INTERACTING\n" }, 0, P },
        { "R01-noncanonical-rational", { "povm_weight 1/1" }, { "povm_weight 2/2\n" }, 0, NC },
        { "R17-noncanonical-integer", { "clock_dim 4" }, { "clock_dim 04\n" }, 0, NC },
        { "R18-noncanonical-scaled", { "tol_probability 1@12" }, { "tol_probability 10@13\n" }, 0, NC },
        { "R30-negative-zero", { "system_hamiltonian_pauli 0/1,0/1,0/1,1/2" }, { "system_hamiltonian_pauli -0/1,0/1,0/1,1/2\n" }, 0, NC },
        { "R02-header-v2", { "OMEGA-AT1-CASE v1" }, { "OMEGA-AT1-CASE v2\n" }, 0, UV },
        { "R24-wrong-domain", { "domain omega.at1.case.v1" }, { "domain omega.at1.case.v2\n" }, 0, UV },
        { "R25-wrong-contract", { "contract AT1_CASE_V1" }, { "contract AT1_CASE_V2\n" }, 0, UV },
        { "R20-energies-not-increasing", { "clock_energies -3/2,-1/2,1/2,3/2" }, { "clock_energies -1/2,-3/2,1/2,3/2\n" }, 0, IP },
        { "R21-psi-zero", { "reference_system_state (1/1;0/1),(1/1;0/1)" }, { "reference_system_state (0/1;0/1),(0/1;0/1)\n" }, 0, IP },
        { "R22-tau-zero", { "povm_tau_turns 1/4" }, { "povm_tau_turns 0/1\n" }, 0, IP },
        { "R23-weight-negative", { "povm_weight 1/1" }, { "povm_weight -1/1\n" }, 0, IP },
        { "R27-rational-over-limit", { "clock_energies -3/2,-1/2,1/2,3/2" }, { "clock_energies -1048577/1,-1/2,1/2,3/2\n" }, 0, IP },
        { "R28-duplicate-label", { "clock_label 2 t2" }, { "clock_label 2 t1\n" }, 0, IP },
        { "R29-ref-label-missing", { "reference_clock_label t0" }, { "reference_clock_label t9\n" }, 0, IP },
        { "R39-huge-digits-tau", { "povm_tau_turns 1/4" }, { "povm_tau_turns 340282366920938463463374607431768211457/4\n" }, 0, IP },
        { "R06-codes-with-pass", { "expected_failure_codes none" }, { "expected_failure_codes TRIVIAL_PHYSICAL_STATE\n" }, 0, IP },
        { "R06b-codes-unsorted", { "expected_outcome PASS", "control_kind POSITIVE", "expected_failure_codes none" }, { "expected_outcome FAIL\n", "control_kind NEGATIVE\n", "expected_failure_codes PROBABILITY_SUM_EXCEEDED,POVM_NORMALIZATION_EXCEEDED\n" }, 0, IP },
        { "R06c-codes-unknown", { "expected_outcome PASS", "control_kind POSITIVE", "expected_failure_codes none" }, { "expected_outcome FAIL\n", "control_kind NEGATIVE\n", "expected_failure_codes FOO_BAR\n" }, 0, IP },
        { "R06d-fail-without-codes", { "expected_outcome PASS", "control_kind POSITIVE" }, { "expected_outcome FAIL\n", "control_kind NEGATIVE\n" }, 0, IP },
        { "R04-irrational-spectrum", { "system_hamiltonian_pauli 0/1,0/1,0/1,1/2" }, { "system_hamiltonian_pauli 0/1,1/1,0/1,1/1\n" }, 0, IR },
        { "R05-wrong-case-id", { CID }, { "case_id 790980a43bd189494c2c922d2bf0049cbffb07f1090f578e04e1142aee7525f1\n" }, 1, ID },
        { "R38-acceptance-id-wrong", { AID }, { "acceptance_id c63246c391296c6cd381f1b654cc625be9f453c77c27c68d9eac240efd5b4d8f\n" }, 1, ID },
    };
    int count = 0;
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++) {
        char *t = strdup(g_kat);
        for (int e = 0; e < 3 && R[i].old[e]; e++) {
            char *u = replace_line(t, R[i].old[e], R[i].rep[e]);
            CHECK(u != NULL, "%s: edit %d did not apply", R[i].id, e);
            if (u) { free(t); t = u; }
        }
        if (!R[i].no_restamp) restamp(t);
        expect_refusal(R[i].id, t, strlen(t), R[i].code);
        free(t); count++;
    }
    /* byte-level rows */
    size_t n = strlen(g_kat);
    sb crlf = { 0 };
    for (size_t i = 0; i < n; i++) { if (g_kat[i] == '\n') sb_put(&crlf, "\r\n"); else { char ch[2] = { g_kat[i], 0 }; sb_put(&crlf, ch); } }
    expect_refusal("R07-crlf", crlf.p, crlf.n, P); free(crlf.p); count++;
    expect_refusal("R33-missing-final-lf", g_kat, n - 1, P); count++;
    expect_refusal("R34-empty", "", 0, P); count++;
    /* R3, R3b, R26, R03: clock_dim 65 and 1 with matching energy and coupling lines */
    spec s; base_B(&s, "at1-kat-rotated-level-n4"); s.v[2] = RHO;
    sb e65 = { 0 };
    for (int j = 0; j < 65; j++) sb_printf(&e65, "%s%d/1", j ? "," : "", j - 32);
    s.N = 65; s.E = e65.p; s.v[2] = NULL;
    { char *g = gen_case(&s); expect_refusal("R3-clock-dim-65", g, strlen(g), IP); free(g); count++; }
    free(e65.p);
    base_B(&s, "at1-kat-rotated-level-n4"); s.N = 1; s.E = "-1/2";
    { char *g = gen_case(&s); expect_refusal("R3b-clock-dim-1", g, strlen(g), IP); free(g); count++; }
    /* a token over the engine's digit limit is an engine error, not a refusal */
    {
        sb big = { 0 }; sb_put(&big, "povm_tau_turns 1");
        for (int i = 0; i < 4999; i++) sb_put(&big, "0");
        sb_put(&big, "1");
        sb_put(&big, "/4\n");
        char *t = replace_line(g_kat, "povm_tau_turns 1/4", big.p); restamp(t);
        expect_refusal("engine-limit-digits", t, strlen(t), AT1_ERR_RESOURCE); free(t); free(big.p);
    }
    return count;
}

/* ---- invariance tests (AT1_SPEC section 9.1) ---------------------------------------------------- */
static int values_of(const spec *s, int reversed, at1_values *v, at1_case *c_out)
{
    char *t = gen_case(s); at1_case c; at1_kernel k;
    at1_status st = at1_case_parse((const uint8_t *)t, strlen(t), &c); free(t);
    if (st != AT1_OK) return 0;
    if (at1_kernel_build(&c, &k) != AT1_OK) { at1_case_free(&c); return 0; }
    memset(v, 0, sizeof *v);
    st = at1_compute(&c, &k, reversed, v);
    at1_kernel_free(&k);
    if (c_out) *c_out = c; else at1_case_free(&c);
    return st == AT1_OK;
}
static int close_val(const at1_val *a, const at1_val *b)
{
    if (a->undefined || b->undefined) return a->undefined == b->undefined;
    return fabs(a->value - b->value) <= a->bound + b->bound;
}
/* every value of a equals the value of b at label k + shift, within the summed bounds */
static int same_values(const at1_values *a, const at1_values *b, int shift, int from, int to)
{
    for (int k = from; k < to; k++) {
        if (a->status[k] != b->status[k + shift] || !close_val(&a->clock[k], &b->clock[k + shift])) return 0;
        for (int q = 0; q < 6; q++) if (!close_val(&a->pauli[k][q], &b->pauli[k + shift][q])) return 0;
    }
    return 1;
}
static void invariance(void)
{
    at1_values *a = calloc(1, sizeof *a), *b = calloc(1, sizeof *b);
    spec p2; base_B(&p2, "at1-t-p2"); p2.v[2] = RHO;
    spec p4 = p2; p4.psi = "(1/1;0/1),(3/5;4/5)"; p4.ref = "t1";
    const spec *base[2] = { &p2, &p4 };
    for (int i = 0; i < 2; i++) {
        CHECK(values_of(base[i], 0, a, NULL), "T: base %d", i);
        /* T4: psi_0 -> i psi_0 */
        spec s = *base[i]; s.psi = i == 0 ? "(0/1;1/1),(0/1;1/1)" : "(0/1;1/1),(-4/5;3/5)";
        CHECK(values_of(&s, 0, b, NULL) && same_values(a, b, 0, 0, 4), "T4 phase invariance (base %d)", i);
        /* T5: psi_0 -> 2 psi_0 */
        s = *base[i]; s.psi = i == 0 ? "(2/1;0/1),(2/1;0/1)" : "(2/1;0/1),(6/5;8/5)";
        CHECK(values_of(&s, 0, b, NULL) && same_values(a, b, 0, 0, 4), "T5 scale invariance (base %d)", i);
        /* T6: purity of each rho_k: |Bloch|^2 = 1 */
        for (int k = 0; k < 4; k++) {
            double x = 2 * a->pauli[k][0].value - 1, y = 2 * a->pauli[k][2].value - 1, z = 2 * a->pauli[k][4].value - 1;
            double tol = 6 * (a->pauli[k][0].bound + a->pauli[k][2].bound + a->pauli[k][4].bound);
            CHECK(fabs(x * x + y * y + z * z - 1) <= tol, "T6 purity k=%d (base %d): %.3g", k, i, x * x + y * y + z * z - 1);
        }
        /* T8: reversed label order, bit-identical */
        CHECK(values_of(base[i], 1, b, NULL) && memcmp(a, b, sizeof *a) == 0, "T8 reversed order not bit-identical (base %d)", i);
        /* T9: run twice, bit-identical */
        CHECK(values_of(base[i], 0, b, NULL) && memcmp(a, b, sizeof *a) == 0, "T9 second run not bit-identical (base %d)", i);
    }
    /* T7: shifting r by one shifts the labels by one (values depend on k - r) */
    spec r1 = p2; r1.ref = "t1";
    CHECK(values_of(&p2, 0, a, NULL) && values_of(&r1, 0, b, NULL) && same_values(a, b, 1, 0, 3), "T7 reference shift");
    free(a); free(b);
}

/* ---- large case: N = 64, M = 256 ------------------------------------------------------------- */
static void large(void)
{
    spec s; base_B(&s, "at1-large-n64-m256");
    sb e = { 0 };
    for (int j = 0; j < 64; j++) {
        int num = j - 32;   /* E_j = (j - 32)/2 */
        if (num % 2 == 0) sb_printf(&e, "%s%d/1", j ? "," : "", num / 2); else sb_printf(&e, "%s%d/2", j ? "," : "", num);
    }
    s.N = 64; s.E = e.p; s.v[33] = RHO; s.v[40] = "0/1,0/1,1/2"; s.tau = "1/128"; s.w = "1/4"; s.M = 256;
    at1_values *v = calloc(1, sizeof *v); at1_case c;
    int ok = values_of(&s, 0, v, &c);
    CHECK(ok, "large case did not run");
    if (!ok) { free(e.p); free(v); return; }
    CHECK(v->kernel_dim == 2 && !v->trivial, "large: kernel_dim %d", v->kernel_dim);
    double sum = 0; for (int k = 0; k < 256; k++) sum += v->clock[k].value;
    /* oracle record mirroring the engine's own values: exercises the writer and judge at M = 256 */
    sb o = { 0 }; char f[64];
    sb_printf(&o, "case_id %s\n", c.case_id);
    for (int k = 0; k < 256; k++) sb_printf(&o, "reference_label %d t%d %s\n", k, k, v->status[k] == AT1_LABEL_DEFINED ? "DEFINED" : "UNDEFINED");
    for (int k = 0; k < 256; k++) { at1_f64_format(v->clock[k].value, f, sizeof f); sb_printf(&o, "reference_clock_probability %d %s 1@15\n", k, f); }
    for (int fam = 0; fam < 2; fam++) for (int k = 0; k < 256; k++) for (int q = 0; q < 6; q++) {
        if (v->pauli[k][q].undefined) { sb_printf(&o, "%s %d %s %s undefined 0@0\n", fam ? "reference_interacting" : "reference_ideal", k, AX[q / 2], SG[q % 2]); continue; }
        at1_f64_format(v->pauli[k][q].value, f, sizeof f);
        sb_printf(&o, "%s %d %s %s %s 1@15\n", fam ? "reference_interacting" : "reference_ideal", k, AX[q / 2], SG[q % 2], f);
    }
    char *res; size_t rl; char err[256];
    at1_status st = at1_result_write(&c, v, o.p, o.n, PROV, strlen(PROV), &res, &rl, err, sizeof err);
    CHECK(st == AT1_OK, "large: result write %s", err);
    if (st == AT1_OK) {
        char checks[13], outcome[64], codes[512], met[32];
        verdict_of(res, checks, outcome, sizeof outcome, codes, sizeof codes, met, sizeof met);
        CHECK(strcmp(checks, "PPPPPPPPPPNP") == 0 && strcmp(outcome, "PASS") == 0, "large: checks %s outcome %s codes %s", checks, outcome, codes);
        check_structure("large", &c, res, rl);
        printf("case large  N=64 M=256 kernel_dim %d  sum p - 1 = %.3g  constraint %.3g (bound %.3g)  povm %.3g (bound %.3g)  checks %s\n",
               v->kernel_dim, sum - 1, v->constraint.value, v->constraint.bound, v->povm.value, v->povm.bound, checks);
        free(res);
    }
    free(o.p); free(e.p); free(v); at1_case_free(&c);
}

/* ---- oracle record, provenance, ERROR results ------------------------------------------------- */
/* rewrite label k of an oracle record consistently: its status, its clock line and its six
 * reference_interacting lines (undefined unless the new status is DEFINED) */
static char *label_edit(const char *o, int k, const char *status, const char *clock)
{
    char pre[96], old[256], rep[256], *cur = strdup(o), *u;
    snprintf(pre, sizeof pre, "reference_label %d ", k);
    if (line_with_prefix(cur, pre, old, sizeof old)) { snprintf(rep, sizeof rep, "reference_label %d t%d %s\n", k, k, status); u = replace_line(cur, old, rep); free(cur); cur = u; }
    snprintf(pre, sizeof pre, "reference_clock_probability %d ", k);
    if (line_with_prefix(cur, pre, old, sizeof old)) { snprintf(rep, sizeof rep, "reference_clock_probability %d %s\n", k, clock); u = replace_line(cur, old, rep); free(cur); cur = u; }
    for (int q = 0; q < 6 && strcmp(status, "DEFINED") != 0; q++) {
        snprintf(pre, sizeof pre, "reference_interacting %d %s %s ", k, AX[q / 2], SG[q % 2]);
        if (line_with_prefix(cur, pre, old, sizeof old)) { snprintf(rep, sizeof rep, "%sundefined 0@0\n", pre); u = replace_line(cur, old, rep); free(cur); cur = u; }
    }
    return cur;
}
static char *(*g_mut)(const char *) = NULL;   /* optional whole-record edit inside verdict_with */
static void verdict_with(const char *id, const spec *s, const table4 *inter, const table4 *ideal, const char *old, const char *rep,
                         const char *prov, const char *want_checks, const char *want_codes, const char *want_met, at1_status want_st)
{
    at1_values *v = calloc(1, sizeof *v); at1_case c;
    if (!values_of(s, 0, v, &c)) { CHECK(0, "%s: did not run", id); free(v); return; }
    char *o = oracle_record(&c, inter, ideal, 0);
    if (g_mut) { char *u = g_mut(o); free(o); o = u; }
    if (old) { char *u = replace_line(o, old, rep); CHECK(u != NULL, "%s: edit did not apply", id); if (u) { free(o); o = u; } }
    char *res = NULL; size_t rl = 0; char err[256] = "";
    at1_status st = at1_result_write(&c, v, o, strlen(o), prov, strlen(prov), &res, &rl, err, sizeof err);
    CHECK(st == want_st, "%s: status %d (%s)", id, st, err);
    if (st == AT1_OK && want_checks) {
        char checks[13], outcome[64], codes[512], met[32];
        verdict_of(res, checks, outcome, sizeof outcome, codes, sizeof codes, met, sizeof met);
        CHECK(strcmp(checks, want_checks) == 0 && strcmp(codes, want_codes) == 0 && strcmp(met, want_met) == 0,
              "%s: checks %s codes %s met %s", id, checks, codes, met);
    }
    free(res); free(o); free(v); at1_case_free(&c);
}

static char *mut_label1_undefined(const char *o) { return label_edit(o, 1, "UNDEFINED", "f64:0000000000000000 1@15"); }
static char *mut_label1_indeterminate(const char *o)
{
    char f[64], cl[96]; at1_f64_format(1e-9, f, sizeof f); snprintf(cl, sizeof cl, "%s 1@15", f);
    return label_edit(o, 1, "INDETERMINATE", cl);
}
static char *mut_oracle_trivial(const char *o)
{
    char *cur = strdup(o), *u;
    for (int k = 0; k < 4; k++) { u = label_edit(cur, k, "UNDEFINED", "undefined 0@0"); free(cur); cur = u; }
    return cur;
}
static char *mut_one_clock_undefined(const char *o) { return label_edit(o, 1, "UNDEFINED", "undefined 0@0"); }
static char *mut_status_rule_broken(const char *o)
{
    char f[64], cl[96]; f64_of("1/4", f, sizeof f); snprintf(cl, sizeof cl, "%s 1@15", f);
    return label_edit(o, 1, "UNDEFINED", cl);   /* clock 1/4 says DEFINED */
}
static char *mut_all_undefined(const char *o)
{
    /* every value line undefined, every label DEFINED (the review's PASS reproducer) */
    sb b = { 0 }; char line[512];
    for (const char *p = o; *p; ) {
        const char *e = strchr(p, '\n'); size_t l = (size_t)(e - p);
        if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, p, l); line[l] = 0; p = e + 1;
        int nt = 0; for (char *q = line; *q; q++) nt += *q == ' ';
        if (strncmp(line, "reference_clock_probability ", 28) == 0 || strncmp(line, "reference_ideal ", 16) == 0
            || strncmp(line, "reference_interacting ", 22) == 0) {
            char *sp = line; for (int s = 0; s < nt - 1; s++) sp = strchr(sp, ' ') + 1;
            strcpy(sp, "undefined 0@0");
        }
        sb_printf(&b, "%s\n", line);
    }
    return b.p;
}
static void tool_rules(void)
{
    spec p2; base_B(&p2, "at1-kat-rotated-level-n4"); p2.v[2] = RHO;
    spec n1 = p2; n1.target = "IDEAL"; n1.control = "NEGATIVE"; n1.expected = "FAIL"; n1.codes = "SCHRODINGER_DEVIATION_EXCEEDED";
    char f[64], line[256];
    /* label status disagreement: check 12 FAIL */
    g_mut = mut_label1_undefined;
    verdict_with("oracle-status-disagree", &p2, &T_P2, &T_P1, NULL, NULL, PROV, "PPPPPPPPPPNF", "ORACLE_DISAGREEMENT", "NO", AT1_OK);
    g_mut = mut_label1_indeterminate;
    verdict_with("oracle-status-indeterminate", &p2, &T_P2, &T_P1, NULL, NULL, PROV, "PPPPPPPPPPNI", "PRECISION_INSUFFICIENT", "NO", AT1_OK);
    /* a consistent trivial-shape oracle record against a nontrivial engine: every label disagrees */
    g_mut = mut_oracle_trivial;
    verdict_with("oracle-trivial-shape", &p2, &T_P2, &T_P1, NULL, NULL, PROV, "PPPPPPPPPNNF", "ORACLE_DISAGREEMENT", "NO", AT1_OK);
    /* undefined placement and the status rule are enforced on the oracle record (review finding 1) */
    g_mut = mut_one_clock_undefined;
    verdict_with("oracle-one-clock-undefined", &p2, &T_P2, &T_P1, NULL, NULL, PROV, NULL, NULL, NULL, AT1_ERR_ARGUMENT);
    g_mut = mut_status_rule_broken;
    verdict_with("oracle-status-rule-broken", &p2, &T_P2, &T_P1, NULL, NULL, PROV, NULL, NULL, NULL, AT1_ERR_ARGUMENT);
    g_mut = mut_all_undefined;
    verdict_with("oracle-all-undefined", &p2, &T_P2, &T_P1, NULL, NULL, PROV, NULL, NULL, NULL, AT1_ERR_ARGUMENT);
    g_mut = NULL;
    verdict_with("oracle-status-without-lines", &p2, &T_P2, &T_P1, "reference_label 1 t1 DEFINED", "reference_label 1 t1 UNDEFINED\n", PROV, NULL, NULL, NULL, AT1_ERR_ARGUMENT);
    {
        char a[64], l1[160], l2[160];
        f64_of("1", a, sizeof a); snprintf(l1, sizeof l1, "reference_ideal 0 X PLUS %s 1@15", a);
        verdict_with("oracle-ideal-undefined", &p2, &T_P2, &T_P1, l1, "reference_ideal 0 X PLUS undefined 0@0\n", PROV, NULL, NULL, NULL, AT1_ERR_ARGUMENT);
        f64_of("49/50", a, sizeof a); snprintf(l2, sizeof l2, "reference_interacting 0 X PLUS %s 1@15", a);
        verdict_with("oracle-interacting-undefined", &p2, &T_P2, &T_P1, l2, "reference_interacting 0 X PLUS undefined 0@0\n", PROV, NULL, NULL, NULL, AT1_ERR_ARGUMENT);
    }
    /* INTERACTING target, wrong oracle clock marginal: check 10 FAIL with INTERACTING_DEVIATION_EXCEEDED */
    f64_of("1/4", f, sizeof f); snprintf(line, sizeof line, "reference_clock_probability 0 %s 1@15", (f64_of("5/28", f, sizeof f), f));
    char rep[256]; char f2[64]; f64_of("1/4", f2, sizeof f2); snprintf(rep, sizeof rep, "reference_clock_probability 0 %s 1@15\n", f2);
    verdict_with("interacting-deviation", &p2, &T_P2, &T_P1, line, rep, PROV, "PPPPPPPPPFNP", "INTERACTING_DEVIATION_EXCEEDED", "NO", AT1_OK);
    /* IDEAL target, wrong reference_interacting: check 11 FAIL in addition to check 10 */
    f64_of("49/50", f, sizeof f); snprintf(line, sizeof line, "reference_interacting 0 X PLUS %s 1@15", f);
    f64_of("48/50", f2, sizeof f2); snprintf(rep, sizeof rep, "reference_interacting 0 X PLUS %s 1@15\n", f2);
    verdict_with("oracle-cross-check", &n1, &T_P2, &T_P1, line, rep, PROV, "PPPPPPPPPFFP", "ORACLE_DISAGREEMENT,SCHRODINGER_DEVIATION_EXCEEDED", "NO", AT1_OK);
    /* a nonfinite reference value: check 2 FAIL */
    snprintf(line, sizeof line, "reference_ideal 0 X PLUS %s 1@15", (f64_of("1", f, sizeof f), f));
    verdict_with("nonfinite-reference", &p2, &T_P2, &T_P1, line, "reference_ideal 0 X PLUS nonfinite 0@0\n", PROV, "PFPPPPPPPPNP", "NONFINITE_VALUE", "NO", AT1_OK);
    /* malformed oracle records and refused provenance */
    verdict_with("oracle-missing-line", &p2, &T_P2, &T_P1, "reference_label 3 t3 DEFINED", "", PROV, NULL, NULL, NULL, AT1_ERR_ARGUMENT);
    verdict_with("oracle-wrong-label", &p2, &T_P2, &T_P1, "reference_label 3 t3 DEFINED", "reference_label 3 t2 DEFINED\n", PROV, NULL, NULL, NULL, AT1_ERR_ARGUMENT);
    verdict_with("oracle-negative-zero", &p2, &T_P2, &T_P1, line, "reference_ideal 0 X PLUS f64:8000000000000000 1@15\n", PROV, NULL, NULL, NULL, AT1_ERR_ARGUMENT);
    {
        at1_values *v = calloc(1, sizeof *v); at1_case c; values_of(&p2, 0, v, &c);
        char *o = oracle_record(&c, &T_P2, &T_P1, 0);
        char *bad = replace_line(o, line_with_prefix(o, "case_id ", line, sizeof line), "case_id 0000000000000000000000000000000000000000000000000000000000000000\n");
        char *res; size_t rl; char err[256];
        CHECK(at1_result_write(&c, v, bad, strlen(bad), PROV, strlen(PROV), &res, &rl, err, sizeof err) == AT1_ERR_ARGUMENT, "oracle case_id mismatch accepted");
        char *p1 = replace_line(PROV, "build_cc cc test harness", "build_cc oracle cargo\n");
        CHECK(at1_result_write(&c, v, o, strlen(o), p1, strlen(p1), &res, &rl, err, sizeof err) == AT1_ERR_ARGUMENT, "oracle build_cc accepted");
        char *p2s = replace_line(PROV, "oracle_sha256 dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
                                 "oracle_sha256 bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n");
        CHECK(at1_result_write(&c, v, o, strlen(o), p2s, strlen(p2s), &res, &rl, err, sizeof err) == AT1_ERR_ARGUMENT, "engine_sha256 == oracle_sha256 accepted");
        char *p3 = replace_line(PROV, "artifact b/two 2222222222222222222222222222222222222222222222222222222222222222", "artifact a/one 2222222222222222222222222222222222222222222222222222222222222222\n");
        CHECK(at1_result_write(&c, v, o, strlen(o), p3, strlen(p3), &res, &rl, err, sizeof err) == AT1_ERR_ARGUMENT, "duplicate artifact path accepted");
        char *p4 = replace_line(PROV, "run_started_utc 2026-10-09T00:00:00Z", "run_started_utc 2026-10-09 00:00:00\n");
        CHECK(at1_result_write(&c, v, o, strlen(o), p4, strlen(p4), &res, &rl, err, sizeof err) == AT1_ERR_ARGUMENT, "bad timestamp accepted");
        /* ORACLE_UNAVAILABLE: ERROR shape */
        CHECK(at1_result_write(&c, v, NULL, 0, PROV, strlen(PROV), &res, &rl, err, sizeof err) == AT1_OK, "oracle unavailable: %s", err);
        char checks[13], outcome[64], codes[512], met[32];
        verdict_of(res, checks, outcome, sizeof outcome, codes, sizeof codes, met, sizeof met);
        CHECK(strcmp(checks, "NNNNNNNNNNNN") == 0 && strcmp(outcome, "ERROR") == 0 && strcmp(codes, "none") == 0 && strcmp(met, "NOT_APPLICABLE") == 0,
              "oracle unavailable verdict %s %s %s %s", checks, outcome, codes, met);
        CHECK(strstr(res, "\nerror_code ORACLE_UNAVAILABLE\n") && strstr(res, "\noracle_repo none\noracle_commit none\noracle_sha256 none\n")
              && strstr(res, "\nphysical_state_kernel_dim 0\nconstraint_residual undefined 0@0\npovm_residual undefined 0@0\n"), "oracle unavailable shape");
        check_structure("oracle-unavailable", &c, res, rl);
        free(res);
        CHECK(at1_result_write_error(&c, "INTERNAL_ERROR", PROV, strlen(PROV), &res, &rl, err, sizeof err) == AT1_OK && strstr(res, "\nerror_code INTERNAL_ERROR\n")
              && strstr(res, "\noracle_sha256 dddd"), "INTERNAL_ERROR result");
        free(res);
        free(o); free(bad); free(p1); free(p2s); free(p3); free(p4); free(v); at1_case_free(&c);
    }
}

/* ---- review findings: values block, POVM accumulation, bound formatting, boundaries ---------- */
static void scaled_of(const char *t, bq *out)
{
    const char *at = strchr(t, '@'); bn n; bn_init(&n); bn_from_digits(&n, t, (size_t)(at - t));
    bq_from_scaled(out, &n, (unsigned)atoi(at + 1)); bn_free(&n);
}
static void bound_minimal(double x)
{
    char t[400]; at1_bound_format(x, t, sizeof t);
    const char *at = strchr(t, '@'); size_t d = (size_t)(at - t); unsigned k = (unsigned)atoi(at + 1);
    bq X, B, Bm; bq_init(&X); bq_init(&B); bq_init(&Bm);
    bq_from_double(&X, x); scaled_of(t, &B);
    int ok = d <= 7 || k == 0;
    ok = ok && bq_cmp(&B, &X) >= 0;
    /* one unit less in the seventh significant digit (or in the last digit when k is clamped) is below x */
    unsigned s = d < 7 ? (unsigned)(7 - d) : 0;
    if (k + s > AT1_SCALED_K_MAX) s = AT1_SCALED_K_MAX - k;
    bn n, p, one; bn_init(&n); bn_init(&p); bn_init(&one);
    bn_from_digits(&n, t, d); bn_pow10(&p, s); bn_mul(&n, &n, &p); bn_set_u64(&one, 1); bn_sub(&n, &n, &one);
    bq_from_scaled(&Bm, &n, k + s);
    ok = ok && bq_cmp(&Bm, &X) < 0;
    CHECK(ok, "bound %.17g written %s is not the smallest 7-digit scaled decimal >= x", x, t);
    bn_free(&n); bn_free(&p); bn_free(&one); bq_free(&X); bq_free(&B); bq_free(&Bm);
}
static void review(void)
{
    char t[400];
    /* finding 5: minimal bound tokens */
    at1_bound_format(1.0, t, sizeof t); CHECK(strcmp(t, "1@0") == 0, "bound 1 written %s", t);
    at1_bound_format(0.25, t, sizeof t); CHECK(strcmp(t, "25@2") == 0, "bound 0.25 written %s", t);
    at1_bound_format(3.0, t, sizeof t); CHECK(strcmp(t, "3@0") == 0, "bound 3 written %s", t);
    at1_bound_format(0.0, t, sizeof t); CHECK(strcmp(t, "0@0") == 0, "bound 0 written %s", t);
    at1_bound_format(1e-300, t, sizeof t); CHECK(strcmp(t, "1@40") == 0, "bound 1e-300 written %s", t);
    const double xs[] = { 1.0, 0.25, 1.0 / 3.0, 1e-15, 8.17e-15, 2.31e-13, 7.0e-17, 6.56e-12, 123456789.0, 9999999.5, 1e-40, 3e-41 };
    for (size_t i = 0; i < sizeof xs / sizeof xs[0]; i++) bound_minimal(xs[i]);

    /* label status rule at its exact boundaries, tol_zero = 1/2 */
    at1_scaled tz; bn_init(&tz.n); bn_set_u64(&tz.n, 5); tz.k = 1;
    CHECK(at1_status_rule(0.25, 0.25, &tz) == AT1_LABEL_UNDEFINED, "status: p + b == tol must be UNDEFINED");
    CHECK(at1_status_rule(0.5, 0.0, &tz) == AT1_LABEL_UNDEFINED, "status: p == tol, b = 0 must be UNDEFINED");
    CHECK(at1_status_rule(nextafter(0.5, 1.0), 0.0, &tz) == AT1_LABEL_DEFINED, "status: p one ulp above tol must be DEFINED");
    CHECK(at1_status_rule(0.75, 0.25, &tz) == AT1_LABEL_INDETERMINATE, "status: p - b == tol must be INDETERMINATE");
    CHECK(at1_status_rule(0.75, 0.125, &tz) == AT1_LABEL_DEFINED, "status: p - b > tol must be DEFINED");
    bn_free(&tz.n);

    /* finding 2: POVM accumulation, N = 64, M = 1, w = 1/3: exact value sqrt(571)/3 */
    {
        spec s; base_B(&s, "at1-review-povm-n64"); sb e = { 0 };
        for (int j = 0; j < 64; j++) sb_printf(&e, "%s%d/1", j ? "," : "", j);
        s.N = 64; s.E = e.p; s.h = "0/1,0/1,0/1,0/1"; s.tau = "1/1"; s.w = "1/3"; s.M = 1;
        at1_values *v = calloc(1, sizeof *v);
        int ok = values_of(&s, 0, v, NULL);
        CHECK(ok, "povm reproducer did not run");
        if (ok) {
            char bt[400]; at1_bound_format(v->povm.bound, bt, sizeof bt);
            bq V, B, lo, hi, ex; bq_init(&V); bq_init(&B); bq_init(&lo); bq_init(&hi); bq_init(&ex);
            bq_from_double(&V, v->povm.value); scaled_of(bt, &B); bq_set_i64(&ex, 571, 9);
            bq_sub(&lo, &V, &B); bq_add(&hi, &V, &B);
            int in = bq_sign(&lo) >= 0; bq_mul(&lo, &lo, &lo); bq_mul(&hi, &hi, &hi);
            in = in && bq_cmp(&lo, &ex) <= 0 && bq_cmp(&ex, &hi) <= 0;
            double err = fabs(v->povm.value - sqrt(571.0) / 3.0);
            CHECK(in, "povm reproducer: value outside [exact - bound, exact + bound], error about %.3g bound %s", err, bt);
            printf("review povm N=64 M=1 w=1/3: value %.17g, |value - sqrt(571)/3| about %.3g, bound %s (ratio %.3g)\n",
                   v->povm.value, err, bt, err / v->povm.bound);
            bq_free(&V); bq_free(&B); bq_free(&lo); bq_free(&hi); bq_free(&ex);
        }
        free(e.p); free(v);
    }

    spec p2; base_B(&p2, "at1-kat-rotated-level-n4"); p2.v[2] = RHO;
    at1_values *v = calloc(1, sizeof *v); at1_case c;
    if (!values_of(&p2, 0, v, &c)) { CHECK(0, "review: P2 did not run"); free(v); return; }
    char *o = oracle_record(&c, &T_P2, &T_P1, 0), *res = NULL, err[256], checks[13], outcome[64], codes[512], met[32];
    size_t rl;

    /* finding 3: an oracle record that is a full result file; lines outside its values block are ignored */
    {
        const char *first = strstr(o, "reference_label ");
        sb b = { 0 };
        sb_printf(&b, "%.*s", (int)(first - o), o);
        sb_printf(&b, "reference_label 0 t0 BOGUS\nbegin values\n%send values\nend\n", first);
        at1_status st = at1_result_write(&c, v, b.p, b.n, PROV, strlen(PROV), &res, &rl, err, sizeof err);
        CHECK(st == AT1_OK, "values-block record refused: %s", err);
        if (st == AT1_OK) {
            verdict_of(res, checks, outcome, sizeof outcome, codes, sizeof codes, met, sizeof met);
            CHECK(strcmp(checks, "PPPPPPPPPPNP") == 0 && strcmp(outcome, "PASS") == 0, "values-block record: %s %s", checks, outcome);
            free(res);
        }
        free(b.p);
    }

    /* finding 6: absolute-form comparison boundary on check 10 (|d| + b == tol is PASS, one ulp more is INDETERMINATE) */
    {
        CHECK(at1_result_write(&c, v, o, strlen(o), PROV, strlen(PROV), &res, &rl, err, sizeof err) == AT1_OK, "boundary base: %s", err);
        char line[256], vt[64], bt[64];
        line_with_prefix(res, "pauli 0 X PLUS ", line, sizeof line);
        sscanf(line + 15, "%63s %63s", vt, bt);
        free(res);
        const char *at = strchr(bt, '@'); unsigned k = (unsigned)atoi(at + 1);
        uint64_t nb = strtoull(bt, NULL, 10), p = 1;
        CHECK(k >= 12 && k - 12 <= 19, "boundary: engine bound %s out of the test's range", bt);
        for (unsigned i = 12; i < k; i++) p *= 10;
        uint64_t ob = p - nb; unsigned okk = k;
        while (okk > 0 && ob % 10 == 0) { ob /= 10; okk--; }
        char old[160], rep[200], a[64];
        f64_of("49/50", a, sizeof a); snprintf(old, sizeof old, "reference_interacting 0 X PLUS %s 1@15", a);
        snprintf(rep, sizeof rep, "reference_interacting 0 X PLUS %s %llu@%u\n", vt, (unsigned long long)ob, okk);
        char *o2 = replace_line(o, old, rep);
        CHECK(o2 && at1_result_write(&c, v, o2, strlen(o2), PROV, strlen(PROV), &res, &rl, err, sizeof err) == AT1_OK, "boundary equal: %s", err);
        if (o2) {
            verdict_of(res, checks, outcome, sizeof outcome, codes, sizeof codes, met, sizeof met);
            CHECK(strcmp(checks, "PPPPPPPPPPNP") == 0 && strcmp(codes, "none") == 0, "boundary equal: %s %s", checks, codes);
            free(res); free(o2);
        }
        uint64_t bits = 0; for (int i = 4; i < 20; i++) bits = (bits << 4) | (uint64_t)(vt[i] <= '9' ? vt[i] - '0' : vt[i] - 'a' + 10);
        double dv; memcpy(&dv, &bits, sizeof dv); at1_f64_format(nextafter(dv, 2.0), a, sizeof a);
        snprintf(rep, sizeof rep, "reference_interacting 0 X PLUS %s %llu@%u\n", a, (unsigned long long)ob, okk);
        o2 = replace_line(o, old, rep);
        CHECK(o2 && at1_result_write(&c, v, o2, strlen(o2), PROV, strlen(PROV), &res, &rl, err, sizeof err) == AT1_OK, "boundary ulp: %s", err);
        if (o2) {
            verdict_of(res, checks, outcome, sizeof outcome, codes, sizeof codes, met, sizeof met);
            CHECK(strcmp(checks, "PPPPPPPPPINP") == 0 && strcmp(codes, "PRECISION_INSUFFICIENT") == 0, "boundary ulp: %s %s", checks, codes);
            free(res); free(o2);
        }
    }
    free(o); free(v); at1_case_free(&c);
}

/* ---- the command-line tool -------------------------------------------------------------------- */
static int run(const char *cmd)
{
    int st = system(cmd);
    return st == -1 ? -1 : WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static void cli(void)
{
    spec p2; base_B(&p2, "at1-kat-rotated-level-n4"); p2.v[2] = RHO;
    at1_values *v = calloc(1, sizeof *v); at1_case c; values_of(&p2, 0, v, &c);
    char *o = oracle_record(&c, &T_P2, &T_P1, 0);
    write_all("build/t.case", g_kat, strlen(g_kat));
    write_all("build/t.oracle", o, strlen(o));
    write_all("build/t.prov", PROV, strlen(PROV));
    char *lib; size_t ll; char err[256];
    at1_result_write(&c, v, o, strlen(o), PROV, strlen(PROV), &lib, &ll, err, sizeof err);
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "%s result build/t.case build/t.oracle build/t.prov > build/t.out 2> build/t.err", g_tool);
    CHECK(run(cmd) == 0, "cli result exit status");
    size_t n; char *out = read_all("build/t.out", &n);
    CHECK(out && n == ll && memcmp(out, lib, n) == 0, "cli output differs from the library output");
    snprintf(cmd, sizeof cmd, "%s result build/t.case build/t.oracle build/t.prov --reversed > build/t.out2 2> build/t.err", g_tool);
    CHECK(run(cmd) == 0, "cli reversed exit status");
    char *out2 = read_all("build/t.out2", NULL);
    CHECK(out && out2 && strcmp(out, out2) == 0, "cli --reversed output differs");
    char line[256];
    printf("KAT verdict_id %s\n", line_with_prefix(out, "verdict_id ", line, sizeof line) ? line + 11 : "?");
    free(out); free(out2);
    snprintf(cmd, sizeof cmd, "%s result build/t.case none build/t.prov > build/t.out 2> build/t.err", g_tool);
    CHECK(run(cmd) == 0, "cli oracle none exit status");
    out = read_all("build/t.out", NULL);
    CHECK(out && strstr(out, "\noutcome ERROR\nfailure_codes none\nerror_code ORACLE_UNAVAILABLE\n"), "cli oracle none result");
    free(out);
    char *r1 = replace_line(g_kat, "povm_tau_turns 1/4", "povm_tau_turns 2/8\n"); restamp(r1);
    write_all("build/t-r1.case", r1, strlen(r1)); free(r1);
    snprintf(cmd, sizeof cmd, "%s result build/t-r1.case build/t.oracle build/t.prov > build/t.out 2> build/t.err", g_tool);
    CHECK(run(cmd) == 2, "cli refusal exit status");
    out = read_all("build/t.err", NULL); char *so = read_all("build/t.out", &n);
    CHECK(out && strcmp(out, "AT1_CASE_REFUSED CASE_NONCANONICAL\n") == 0 && n == 0, "cli refusal stderr/stdout");
    free(out); free(so);
    /* finding 4: an unreadable oracle record is an ERROR ORACLE_UNAVAILABLE result, and a refused
     * case is reported before any other input is read */
    snprintf(cmd, sizeof cmd, "%s result build/t.case build/no-such.oracle build/t.prov > build/t.out 2> build/t.err", g_tool);
    CHECK(run(cmd) == 0, "cli unreadable oracle exit status");
    out = read_all("build/t.out", NULL);
    CHECK(out && strstr(out, "\noutcome ERROR\nfailure_codes none\nerror_code ORACLE_UNAVAILABLE\n"), "cli unreadable oracle result");
    free(out);
    snprintf(cmd, sizeof cmd, "%s result build/t-r1.case build/no-such.oracle build/no-such.prov > build/t.out 2> build/t.err", g_tool);
    CHECK(run(cmd) == 2, "cli refusal with unreadable inputs exit status");
    out = read_all("build/t.err", NULL);
    CHECK(out && strcmp(out, "AT1_CASE_REFUSED CASE_NONCANONICAL\n") == 0, "cli refusal masked by unreadable inputs");
    free(out);
    snprintf(cmd, sizeof cmd, "%s result build/t.case build/t.oracle build/no-such.prov > build/t.out 2> build/t.err", g_tool);
    CHECK(run(cmd) == 1, "cli unreadable provenance exit status");
    char *bp = replace_line(PROV, "source_tree_clean YES", "source_tree_clean MAYBE\n");
    write_all("build/t-bad.prov", bp, strlen(bp)); free(bp);
    snprintf(cmd, sizeof cmd, "%s result build/t.case build/t.oracle build/t-bad.prov > build/t.out 2> build/t.err", g_tool);
    CHECK(run(cmd) == 1, "cli bad provenance exit status");
    so = read_all("build/t.out", &n); out = read_all("build/t.err", NULL);
    CHECK(n == 0 && out && strncmp(out, "AT1_ENGINE_ERROR ", 17) == 0, "cli bad provenance output");
    free(so); free(out);
    snprintf(cmd, sizeof cmd, "%s values build/t.case > build/t.out 2> build/t.err", g_tool);
    CHECK(run(cmd) == 0, "cli values exit status");
    free(lib); free(o); free(v); at1_case_free(&c);
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: test_model <cases-dir> <at1-model binary>\n"); return 2; }
    g_cases = argv[1]; g_tool = argv[2];
    kat();
    section13();
    int nref = refusals();
    invariance();
    large();
    tool_rules();
    review();
    cli();
    printf("refusal files: %d\n", nref);
    printf("measured: max |p - exact| %.3g, max ratio to its bound %.3g; max |pauli - exact| %.3g, max ratio %.3g\n",
           g_max_err_p, g_ratio_p, g_max_err_pauli, g_ratio_pauli);
    printf("measured: max constraint_residual %.3g (max ratio to its bound %.3g); max povm_residual on complete clocks %.3g\n",
           g_max_con, g_max_con_bound_ratio, g_max_povm);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    free(g_kat);
    return g_fail ? 1 : 0;
}
