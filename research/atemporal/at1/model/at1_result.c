/* AT1_RESULT_V1 writer: splices the oracle's four reference families (charter reading c),
 * judges the twelve checks of AT1_RESULT_V1 section 4 in exact rational arithmetic on the
 * written tokens (AT0_RESULT_V2 section 4 comparison forms), and writes the complete file with
 * verdict_id and evidence_digest (AT1_RESULT_V1 section 6).
 *
 * Exactness: every f64 token and every scaled decimal is an exact rational (at1_bn.c); the exact
 * rational w/N is the ideal clock marginal of check 10. No comparison here uses floating point.
 * Aggregation inside a check: FAIL if any item fails, else INDETERMINATE if any item is, else
 * PASS; NOT_EVALUATED when no input remains (per-input NOT_EVALUATED reading). Check 9 is FAIL
 * exactly when some engine label is UNDEFINED (as the qualified AT-0 judge). */
#include "at1_model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- growable text buffer ----------------------------------------------------------------- */
typedef struct { char *p; size_t n, cap; } buf_t;
static void put(buf_t *b, const char *s)
{
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (b->n + l + 1 > cap) cap *= 2;
        b->p = at1_xrealloc(b->p, cap); b->cap = cap;
    }
    memcpy(b->p + b->n, s, l); b->n += l; b->p[b->n] = 0;
}
static void putn(buf_t *b, const char *s, size_t l)
{
    char *t = at1_xmalloc(l + 1); memcpy(t, s, l); t[l] = 0; put(b, t); free(t);
}

/* ---- judged value ---------------------------------------------------------------------------- */
enum { JV_VALUE = 0, JV_NONFINITE = 1, JV_UNDEFINED = 2 };
typedef struct { int kind; double v; bn bN; unsigned bk; char btxt[400]; } jv;

static void jv_init(jv *x) { x->kind = JV_UNDEFINED; x->v = 0; bn_init(&x->bN); x->bk = 0; strcpy(x->btxt, "0@0"); }
static void jv_free(jv *x) { bn_free(&x->bN); }
static void jv_from_val(jv *x, const at1_val *v)
{
    char t[400];
    if (v->undefined) { x->kind = JV_UNDEFINED; bn_set_u64(&x->bN, 0); x->bk = 0; return; }
    /* a value whose bound overflowed is written nonfinite (it forces NONFINITE_VALUE) */
    x->kind = isfinite(v->value) && isfinite(v->bound) ? JV_VALUE : JV_NONFINITE;
    x->v = v->value;
    /* the bound exactly as it is written */
    at1_bound_format(x->kind == JV_VALUE ? v->bound : 0.0, x->btxt, sizeof x->btxt);
    strcpy(t, x->btxt);
    char *at = strchr(t, '@');
    bn_from_digits(&x->bN, t, (size_t)(at - t));
    x->bk = (unsigned)atoi(at + 1);
}
static void jv_text(const jv *x, char *out, size_t cap)
{
    if (x->kind == JV_UNDEFINED) { snprintf(out, cap, "undefined 0@0"); return; }
    char v[64];
    if (x->kind == JV_NONFINITE) snprintf(v, sizeof v, "nonfinite"); else at1_f64_format(x->v, v, sizeof v);
    snprintf(out, cap, "%s %s", v, x->btxt);   /* the bound exactly as at1_bound_format wrote it */
}
static void jv_exact(const jv *x, bq *val, bq *bound)
{
    bq_from_double(val, x->v);
    bq_from_scaled(bound, &x->bN, x->bk);
}

/* ---- oracle reference lines ----------------------------------------------------------------- */
static const char *AXES[3] = { "X", "Y", "Z" };
static const char *SIGNS[2] = { "PLUS", "MINUS" };
static const char *STATUS_NAME[3] = { "DEFINED", "UNDEFINED", "INDETERMINATE" };

typedef struct {
    int M;
    char **lines; int nlines;              /* the spliced lines, verbatim */
    at1_label_status *status;
    jv *clock;                             /* M */
    jv (*ideal)[6], (*inter)[6];           /* M x 6 */
} refs_t;

static void refs_free(refs_t *R)
{
    for (int i = 0; i < R->nlines; i++) free(R->lines[i]);
    free(R->lines);
    if (R->clock) for (int k = 0; k < R->M; k++) jv_free(&R->clock[k]);
    if (R->ideal) for (int k = 0; k < R->M; k++) for (int s = 0; s < 6; s++) { jv_free(&R->ideal[k][s]); jv_free(&R->inter[k][s]); }
    free(R->status); free(R->clock); free(R->ideal); free(R->inter);
}

static int is_hex_lower(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}
static int is_digits(const char *s, size_t n)
{
    if (n == 0) return 0;
    for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* "<value> <bound>" -> jv; 0 on a grammar failure. undefined only with 0@0. */
static int parse_value_bound(const char *v, const char *b, jv *x)
{
    size_t lb = strlen(b);
    const char *at = strchr(b, '@');
    if (!at) return 0;
    size_t ln = (size_t)(at - b), lk = lb - ln - 1;
    if (!is_digits(b, ln) || !is_digits(at + 1, lk) || lk > 2) return 0;
    if ((ln > 1 && b[0] == '0') || (lk > 1 && at[1] == '0')) return 0;
    unsigned k = (unsigned)atoi(at + 1);
    if (k > AT1_SCALED_K_MAX) return 0;
    if (ln == 1 && b[0] == '0' && k != 0) return 0;
    if (k != 0 && b[ln - 1] == '0') return 0;
    if (!bn_from_digits(&x->bN, b, ln)) return 0;
    x->bk = k;
    if (strcmp(v, "undefined") == 0) { x->kind = JV_UNDEFINED; return bn_is_zero(&x->bN) && k == 0; }
    if (strcmp(v, "nonfinite") == 0) { x->kind = JV_NONFINITE; return 1; }
    if (strlen(v) != 20 || strncmp(v, "f64:", 4) != 0 || !is_hex_lower(v + 4, 16)) return 0;
    uint64_t bits = 0;
    for (int i = 4; i < 20; i++) bits = (bits << 4) | (uint64_t)(v[i] <= '9' ? v[i] - '0' : v[i] - 'a' + 10);
    double d; memcpy(&d, &bits, sizeof d);
    if (!isfinite(d) || bits == 0x8000000000000000ull) return 0;   /* -0.0 is never written */
    x->kind = JV_VALUE; x->v = d;
    return 1;
}

/* split a line into at most cap space-separated tokens (in a scratch copy) */
static int tokens(char *s, char **t, int cap)
{
    int n = 0;
    for (char *p = s; ; ) {
        if (n == cap) return cap + 1;
        t[n++] = p;
        char *sp = strchr(p, ' ');
        if (!sp) break;
        *sp = 0; p = sp + 1;
    }
    return n;
}

static int key_is(const char *line, size_t len, const char *key)
{
    size_t k = strlen(key);
    return len > k && strncmp(line, key, k) == 0 && line[k] == ' ';
}

static at1_status refs_parse(const at1_case *c, const char *text, size_t len, refs_t *R, char *err, size_t cap)
{
    const int M = c->label_count;
    memset(R, 0, sizeof *R);
    R->M = M;
    R->status = at1_xcalloc((size_t)M, sizeof *R->status);
    R->clock = at1_xmalloc((size_t)M * sizeof(jv));
    R->ideal = at1_xmalloc((size_t)M * sizeof *R->ideal);
    R->inter = at1_xmalloc((size_t)M * sizeof *R->inter);
    for (int k = 0; k < M; k++) { jv_init(&R->clock[k]); for (int s = 0; s < 6; s++) { jv_init(&R->ideal[k][s]); jv_init(&R->inter[k][s]); } }
    R->lines = at1_xmalloc((size_t)(14 * M) * sizeof(char *));

    if (len == 0 || text[len - 1] != '\n') { snprintf(err, cap, "oracle record: not LF-terminated"); return AT1_ERR_ARGUMENT; }
    /* is there a values block? then take reference lines only from inside it */
    int has_values = 0;
    for (size_t i = 0; i < len; ) {
        const char *e = memchr(text + i, '\n', len - i);
        size_t l = (size_t)(e - (text + i));
        if (l == 11 && strncmp(text + i, "begin values", 12) == 0) has_values = 1;
        i += l + 1;
    }
    static const char *fam[4] = { "reference_label", "reference_clock_probability", "reference_ideal", "reference_interacting" };
    int expect_total = 14 * M, got = 0, inside = !has_values;
    for (size_t i = 0; i < len; ) {
        const char *ln = text + i;
        const char *e = memchr(ln, '\n', len - i);
        size_t l = (size_t)(e - ln);
        i += l + 1;
        if (l == 12 && strncmp(ln, "begin values", 12) == 0) { inside = 1; continue; }
        if (l == 10 && strncmp(ln, "end values", 10) == 0) { inside = 0; continue; }
        if (key_is(ln, l, "case_id") && (l != 8 + 64 || strncmp(ln + 8, c->case_id, 64) != 0)) {
            snprintf(err, cap, "oracle record: case_id differs from the case"); return AT1_ERR_ARGUMENT;
        }
        if (key_is(ln, l, "acceptance_id") && (l != 14 + 64 || strncmp(ln + 14, c->acceptance_id, 64) != 0)) {
            snprintf(err, cap, "oracle record: acceptance_id differs from the case"); return AT1_ERR_ARGUMENT;
        }
        if (!inside) continue;
        int f = -1;
        for (int q = 0; q < 4; q++) if (key_is(ln, l, fam[q])) f = q;
        if (f < 0) continue;
        if (got >= expect_total) { snprintf(err, cap, "oracle record: too many reference lines"); return AT1_ERR_ARGUMENT; }
        /* expected position */
        int fam_expected, k, s;
        if (got < M) { fam_expected = 0; k = got; s = 0; }
        else if (got < 2 * M) { fam_expected = 1; k = got - M; s = 0; }
        else if (got < 8 * M) { fam_expected = 2; k = (got - 2 * M) / 6; s = (got - 2 * M) % 6; }
        else { fam_expected = 3; k = (got - 8 * M) / 6; s = (got - 8 * M) % 6; }
        char *copy = at1_xmalloc(l + 1); memcpy(copy, ln, l); copy[l] = 0;
        R->lines[R->nlines++] = copy;
        char *scratch = at1_xmalloc(l + 1); memcpy(scratch, ln, l); scratch[l] = 0;
        char *t[8]; int nt = tokens(scratch, t, 7);
        char kbuf[16]; snprintf(kbuf, sizeof kbuf, "%d", k);
        int ok = f == fam_expected && nt >= 2 && strcmp(t[1], kbuf) == 0;
        if (ok && f == 0) {
            ok = nt == 4 && strcmp(t[2], c->labels[k]) == 0;
            int st = -1;
            for (int q = 0; ok && q < 3; q++) if (strcmp(t[3], STATUS_NAME[q]) == 0) st = q;
            ok = ok && st >= 0;
            if (ok) R->status[k] = (at1_label_status)st;
        } else if (ok && f == 1) {
            ok = nt == 4 && parse_value_bound(t[2], t[3], &R->clock[k]);
        } else if (ok) {
            jv *dst = f == 2 ? &R->ideal[k][s] : &R->inter[k][s];
            ok = nt == 6 && strcmp(t[2], AXES[s / 2]) == 0 && strcmp(t[3], SIGNS[s % 2]) == 0 && parse_value_bound(t[4], t[5], dst);
        }
        free(scratch);
        if (!ok) { snprintf(err, cap, "oracle record: malformed or out-of-order reference line %d", got + 1); return AT1_ERR_ARGUMENT; }
        got++;
    }
    if (got != expect_total) { snprintf(err, cap, "oracle record: %d reference lines, expected %d", got, expect_total); return AT1_ERR_ARGUMENT; }
    return AT1_OK;
}

/* ---- provenance ------------------------------------------------------------------------------ */
static const char *PROV_KEYS[13] = { "source_repo", "source_commit", "source_tree_clean", "contract_commit",
    "engine_sha256", "oracle_repo", "oracle_commit", "oracle_sha256", "build_cc", "build_flags", "host",
    "run_started_utc", "run_finished_utc" };

static int valid_repo(const char *s)
{
    const char *sl = strchr(s, '/');
    if (!sl || sl == s || !sl[1] || strchr(sl + 1, '/')) return 0;
    for (const char *p = s; *p; p++) {
        if (p == sl) continue;
        char ch = *p;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-')) return 0;
    }
    return 1;
}
static int valid_text(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > 200 || s[0] == ' ' || s[n - 1] == ' ') return 0;
    for (size_t i = 0; i < n; i++) if (s[i] < 0x20 || s[i] > 0x7E) return 0;
    return 1;
}
static int valid_utc(const char *s)
{
    if (strlen(s) != 20) return 0;
    static const char pat[] = "dddd-dd-ddTdd:dd:ddZ";
    for (int i = 0; i < 20; i++) {
        if (pat[i] == 'd') { if (s[i] < '0' || s[i] > '9') return 0; }
        else if (s[i] != pat[i]) return 0;
    }
    int mo = (s[5] - '0') * 10 + s[6] - '0', d = (s[8] - '0') * 10 + s[9] - '0';
    int h = (s[11] - '0') * 10 + s[12] - '0', mi = (s[14] - '0') * 10 + s[15] - '0', se = (s[17] - '0') * 10 + s[18] - '0';
    return mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h <= 23 && mi <= 59 && se <= 59;
}
static int valid_path(const char *s, size_t n)
{
    if (n < 1 || n > 200) return 0;
    for (size_t i = 0; i < n; i++) if (s[i] < 0x21 || s[i] > 0x7E) return 0;
    return 1;
}

/* Validates the provenance lines and returns them normalized in *out (each LF-terminated).
 * oracle_none: the oracle fields are written as "none" (ERROR results). */
static at1_status prov_parse(const char *text, size_t len, int oracle_none, buf_t *out, char *err, size_t cap)
{
    if (len == 0 || text[len - 1] != '\n') { snprintf(err, cap, "provenance: not LF-terminated"); return AT1_ERR_ARGUMENT; }
    for (size_t i = 0; i < len; i++) if (text[i] != '\n' && (text[i] < 0x20 || text[i] > 0x7E)) { snprintf(err, cap, "provenance: byte outside 0x20..0x7E"); return AT1_ERR_ARGUMENT; }
    char *copy = at1_xmalloc(len + 1); memcpy(copy, text, len); copy[len] = 0;
    char *line = copy; int idx = 0; at1_status st = AT1_OK;
    char engine_sha[65] = "", oracle_sha[65] = "", prev_path[256] = ""; int have_prev = 0;
    while (*line && st == AT1_OK) {
        char *nl = strchr(line, '\n'); *nl = 0;
        if (idx < 13) {
            const char *key = PROV_KEYS[idx];
            size_t kl = strlen(key);
            if (strncmp(line, key, kl) != 0 || line[kl] != ' ') { snprintf(err, cap, "provenance: line %d must be %s", idx + 1, key); st = AT1_ERR_ARGUMENT; break; }
            const char *v = line + kl + 1;
            int ok;
            int is_oracle_field = idx >= 5 && idx <= 7;
            if (oracle_none && is_oracle_field) { v = "none"; ok = 1; }
            else switch (idx) {
            case 0: case 5: ok = valid_repo(v); break;
            case 1: case 3: case 6: ok = strlen(v) == 40 && is_hex_lower(v, 40); break;
            case 2: ok = strcmp(v, "YES") == 0 || strcmp(v, "NO") == 0; break;
            case 4: case 7: ok = strlen(v) == 64 && is_hex_lower(v, 64); break;
            case 8: case 9: case 10: ok = valid_text(v); break;
            default: ok = valid_utc(v); break;
            }
            if (!ok) { snprintf(err, cap, "provenance: bad value for %s", key); st = AT1_ERR_ARGUMENT; break; }
            if (idx == 4) strcpy(engine_sha, v);
            if (idx == 7 && strcmp(v, "none") != 0) strcpy(oracle_sha, v);
            if (idx == 8 && strncmp(v, "oracle ", 7) == 0) { snprintf(err, cap, "provenance: build_cc marks an oracle-written record; a candidate result is refused"); st = AT1_ERR_ARGUMENT; break; }
            put(out, key); put(out, " "); put(out, v); put(out, "\n");
        } else {
            /* artifact <path> <digest>, sorted strictly by path */
            char *sp1 = strncmp(line, "artifact ", 9) == 0 ? line + 9 : NULL;
            char *sp2 = sp1 ? strchr(sp1, ' ') : NULL;
            if (!sp2 || !valid_path(sp1, (size_t)(sp2 - sp1)) || strlen(sp2 + 1) != 64 || !is_hex_lower(sp2 + 1, 64) || strchr(sp2 + 1, ' ')) {
                snprintf(err, cap, "provenance: bad artifact line"); st = AT1_ERR_ARGUMENT; break;
            }
            *sp2 = 0;
            if (have_prev && strcmp(prev_path, sp1) >= 0) { snprintf(err, cap, "provenance: artifact paths not strictly increasing"); st = AT1_ERR_ARGUMENT; break; }
            strcpy(prev_path, sp1); have_prev = 1;
            *sp2 = ' ';
            put(out, line); put(out, "\n");
        }
        idx++;
        line = nl + 1;
    }
    if (st == AT1_OK && idx < 13) { snprintf(err, cap, "provenance: %d lines, expected at least 13", idx); st = AT1_ERR_ARGUMENT; }
    if (st == AT1_OK && oracle_sha[0] && strcmp(engine_sha, oracle_sha) == 0) {
        snprintf(err, cap, "provenance: engine_sha256 equals oracle_sha256 (oracle-written record); refused"); st = AT1_ERR_ARGUMENT;
    }
    free(copy);
    return st;
}

/* ---- exact checks ---------------------------------------------------------------------------- */
enum { C_PASS = 0, C_FAIL = 1, C_INDET = 2, C_NE = 3 };
static const char *CHECK_STATE[4] = { "PASS", "FAIL", "INDETERMINATE", "NOT_EVALUATED" };
static const char *CHECK_NAME[12] = { "bound_kind_sufficient", "values_finite", "physical_state_nontrivial",
    "constraint_residual", "povm_normalization", "clock_probability_sum", "probability_range", "pauli_pair_sum",
    "conditional_defined", "target_agreement", "oracle_cross_check", "label_status_agreement" };
/* failure code index (at1_failure_codes order) of each check; check 10 depends on the target */
enum { K_BOUND = 0, K_COND = 1, K_CON = 2, K_INTER = 3, K_NONFIN = 4, K_ORACLE = 5, K_POVM = 6, K_PREC = 7,
       K_RANGE = 8, K_SUM = 9, K_SCHRO = 10, K_TRIV = 11 };

typedef struct { int seen, fail, indet; } agg_t;
static void agg_add(agg_t *g, int r) { g->seen = 1; if (r == C_FAIL) g->fail = 1; else if (r == C_INDET) g->indet = 1; }
static int agg_res(const agg_t *g) { return !g->seen ? C_NE : g->fail ? C_FAIL : g->indet ? C_INDET : C_PASS; }

/* absolute form: |v| + b <= tol PASS; |v| - b > tol FAIL; else INDETERMINATE */
static int absform(const bq *v, const bq *b, const bq *tol)
{
    bq a, s; bq_init(&a); bq_init(&s);
    bq_abs(&a, v);
    bq_add(&s, &a, b);
    int r;
    if (bq_cmp(&s, tol) <= 0) r = C_PASS;
    else { bq_sub(&s, &a, b); r = bq_cmp(&s, tol) > 0 ? C_FAIL : C_INDET; }
    bq_free(&a); bq_free(&s);
    return r;
}
static int signedform(const bq *v, const bq *b, const bq *tol)
{
    bq s; bq_init(&s);
    bq_add(&s, v, b);
    int r;
    if (bq_cmp(&s, tol) <= 0) r = C_PASS;
    else { bq_sub(&s, v, b); r = bq_cmp(&s, tol) > 0 ? C_FAIL : C_INDET; }
    bq_free(&s);
    return r;
}
/* compare a - b (bounds summed) */
static int absdiff(const jv *a, const jv *b, const bq *tol)
{
    bq va, ba, vb, bb, d, s; bq_init(&va); bq_init(&ba); bq_init(&vb); bq_init(&bb); bq_init(&d); bq_init(&s);
    jv_exact(a, &va, &ba); jv_exact(b, &vb, &bb);
    bq_sub(&d, &va, &vb); bq_add(&s, &ba, &bb);
    int r = absform(&d, &s, tol);
    bq_free(&va); bq_free(&ba); bq_free(&vb); bq_free(&bb); bq_free(&d); bq_free(&s);
    return r;
}

typedef struct {
    int M;
    int trivial, kernel_dim;
    jv constraint, povm;
    at1_label_status *status;
    jv *clock;
    jv (*pauli)[6];
} eng_t;

static void judge(const at1_case *c, const eng_t *E, const refs_t *R, int res[12], unsigned *codes)
{
    const int M = E->M, triv = E->trivial;
    bq tol_con, tol_povm, tol_p, tol_s, one, wn, v, b, s, t;
    bq_init(&tol_con); bq_init(&tol_povm); bq_init(&tol_p); bq_init(&tol_s); bq_init(&one); bq_init(&wn);
    bq_init(&v); bq_init(&b); bq_init(&s); bq_init(&t);
    bq_from_scaled(&tol_con, &c->tol_constraint.n, c->tol_constraint.k);
    bq_from_scaled(&tol_povm, &c->tol_povm.n, c->tol_povm.k);
    bq_from_scaled(&tol_p, &c->tol_probability.n, c->tol_probability.k);
    bq_from_scaled(&tol_s, &c->tol_schrodinger.n, c->tol_schrodinger.k);
    bq_set_i64(&one, 1, 1);
    { bq n; bq_init(&n); bq_set_i64(&n, c->clock_dim, 1); bq_div(&wn, &c->weight, &n); bq_free(&n); }
    agg_t g;

    /* 1 */
    res[0] = AT1_BOUND_ESTIMATED >= c->min_bound_kind ? C_PASS : C_FAIL;
    /* 2: no nonfinite token anywhere in the values block */
    int nonfin = E->constraint.kind == JV_NONFINITE || E->povm.kind == JV_NONFINITE;
    for (int k = 0; k < M; k++) {
        if (E->clock[k].kind == JV_NONFINITE || R->clock[k].kind == JV_NONFINITE) nonfin = 1;
        for (int q = 0; q < 6; q++)
            if (E->pauli[k][q].kind == JV_NONFINITE || R->ideal[k][q].kind == JV_NONFINITE || R->inter[k][q].kind == JV_NONFINITE) nonfin = 1;
    }
    res[1] = nonfin ? C_FAIL : C_PASS;
    /* 3 */
    res[2] = triv ? C_FAIL : C_PASS;
    /* 4 */
    memset(&g, 0, sizeof g);
    if (!triv && E->constraint.kind == JV_VALUE) { jv_exact(&E->constraint, &v, &b); agg_add(&g, absform(&v, &b, &tol_con)); }
    res[3] = triv ? C_NE : agg_res(&g);
    /* 5 */
    memset(&g, 0, sizeof g);
    if (E->povm.kind == JV_VALUE) { jv_exact(&E->povm, &v, &b); agg_add(&g, absform(&v, &b, &tol_povm)); }
    res[4] = agg_res(&g);
    /* 6 */
    memset(&g, 0, sizeof g);
    if (!triv) {
        int all = 1;
        bq sv, sb; bq_init(&sv); bq_init(&sb);
        for (int k = 0; k < M; k++) {
            if (E->clock[k].kind != JV_VALUE) { all = 0; break; }
            jv_exact(&E->clock[k], &v, &b); bq_add(&sv, &sv, &v); bq_add(&sb, &sb, &b);
        }
        if (all) { bq_sub(&sv, &sv, &one); agg_add(&g, absform(&sv, &sb, &tol_p)); }
        bq_free(&sv); bq_free(&sb);
    }
    res[5] = triv ? C_NE : agg_res(&g);
    /* 7: every clock_probability and pauli value, reference lines excluded */
    memset(&g, 0, sizeof g);
    if (!triv) {
        for (int k = 0; k < M; k++) for (int q = -1; q < 6; q++) {
            const jv *x = q < 0 ? &E->clock[k] : &E->pauli[k][q];
            if (x->kind != JV_VALUE) continue;
            jv_exact(x, &v, &b);
            bq_neg(&t, &v); agg_add(&g, signedform(&t, &b, &tol_p));
            bq_sub(&t, &v, &one); agg_add(&g, signedform(&t, &b, &tol_p));
        }
    }
    res[6] = triv ? C_NE : agg_res(&g);
    /* 8 */
    memset(&g, 0, sizeof g);
    if (!triv) for (int k = 0; k < M; k++) {
        if (E->status[k] != AT1_LABEL_DEFINED) continue;
        for (int a = 0; a < 3; a++) {
            const jv *pl = &E->pauli[k][2 * a], *mi = &E->pauli[k][2 * a + 1];
            if (pl->kind != JV_VALUE || mi->kind != JV_VALUE) continue;
            bq v2, b2; bq_init(&v2); bq_init(&b2);
            jv_exact(pl, &v, &b); jv_exact(mi, &v2, &b2);
            bq_add(&s, &v, &v2); bq_sub(&s, &s, &one); bq_add(&b, &b, &b2);
            agg_add(&g, absform(&s, &b, &tol_p));
            bq_free(&v2); bq_free(&b2);
        }
    }
    res[7] = triv ? C_NE : agg_res(&g);
    /* 9 */
    if (triv) res[8] = C_NE;
    else { res[8] = C_PASS; for (int k = 0; k < M; k++) if (E->status[k] == AT1_LABEL_UNDEFINED) res[8] = C_FAIL; }
    /* 10 and 11 */
    for (int chk = 0; chk < 2; chk++) {
        memset(&g, 0, sizeof g);
        int use_ideal = chk == 0 && c->target == AT1_TARGET_IDEAL;
        if (!triv && !(chk == 1 && c->target == AT1_TARGET_INTERACTING)) {
            for (int k = 0; k < M; k++) {
                if (E->status[k] != AT1_LABEL_DEFINED || R->status[k] != AT1_LABEL_DEFINED) continue;
                for (int q = 0; q < 6; q++) {
                    const jv *ref = use_ideal ? &R->ideal[k][q] : &R->inter[k][q];
                    if (E->pauli[k][q].kind != JV_VALUE || ref->kind != JV_VALUE) continue;
                    agg_add(&g, absdiff(&E->pauli[k][q], ref, &tol_s));
                }
                if (E->clock[k].kind != JV_VALUE) continue;
                if (use_ideal) {
                    jv_exact(&E->clock[k], &v, &b); bq_sub(&s, &v, &wn);
                    agg_add(&g, absform(&s, &b, &tol_p));
                } else if (R->clock[k].kind == JV_VALUE) {
                    agg_add(&g, absdiff(&E->clock[k], &R->clock[k], &tol_p));
                }
            }
        }
        res[9 + chk] = triv ? C_NE : agg_res(&g);
    }
    /* 12 */
    memset(&g, 0, sizeof g);
    if (!triv) for (int k = 0; k < M; k++) {
        at1_label_status a = E->status[k], r = R->status[k];
        if (a == AT1_LABEL_INDETERMINATE || r == AT1_LABEL_INDETERMINATE) agg_add(&g, C_INDET);
        else agg_add(&g, a != r ? C_FAIL : C_PASS);
    }
    res[11] = triv ? C_NE : agg_res(&g);

    const int code_of[12] = { K_BOUND, K_NONFIN, K_TRIV, K_CON, K_POVM, K_SUM, K_RANGE, K_SUM, K_COND,
                              c->target == AT1_TARGET_IDEAL ? K_SCHRO : K_INTER, K_ORACLE, K_ORACLE };
    int indet = 0;
    for (int k = 0; k < M; k++) if (E->status[k] == AT1_LABEL_INDETERMINATE || R->status[k] == AT1_LABEL_INDETERMINATE) indet = 1;
    *codes = 0;
    for (int i = 0; i < 12; i++) { if (res[i] == C_FAIL) *codes |= 1u << code_of[i]; if (res[i] == C_INDET) indet = 1; }
    if (indet) *codes |= 1u << K_PREC;
    bq_free(&tol_con); bq_free(&tol_povm); bq_free(&tol_p); bq_free(&tol_s); bq_free(&one); bq_free(&wn);
    bq_free(&v); bq_free(&b); bq_free(&s); bq_free(&t);
}

/* ---- writer ------------------------------------------------------------------------------------- */
static void eng_from_values(eng_t *E, const at1_values *V)
{
    const int M = V->M;
    E->M = M; E->trivial = V->trivial; E->kernel_dim = V->kernel_dim;
    jv_init(&E->constraint); jv_init(&E->povm);
    jv_from_val(&E->constraint, &V->constraint); jv_from_val(&E->povm, &V->povm);
    E->status = at1_xmalloc((size_t)M * sizeof *E->status);
    E->clock = at1_xmalloc((size_t)M * sizeof(jv));
    E->pauli = at1_xmalloc((size_t)M * sizeof *E->pauli);
    for (int k = 0; k < M; k++) {
        E->status[k] = V->status[k];
        jv_init(&E->clock[k]); jv_from_val(&E->clock[k], &V->clock[k]);
        for (int q = 0; q < 6; q++) { jv_init(&E->pauli[k][q]); jv_from_val(&E->pauli[k][q], &V->pauli[k][q]); }
    }
}
static void eng_free(eng_t *E)
{
    jv_free(&E->constraint); jv_free(&E->povm);
    for (int k = 0; k < E->M; k++) { jv_free(&E->clock[k]); for (int q = 0; q < 6; q++) jv_free(&E->pauli[k][q]); }
    free(E->status); free(E->clock); free(E->pauli);
}

static void write_engine_values(buf_t *o, const at1_case *c, const eng_t *E)
{
    char t[640], vb[480];
    snprintf(t, sizeof t, "physical_state_kernel_dim %d\n", E->kernel_dim); put(o, t);
    jv_text(&E->constraint, vb, sizeof vb); snprintf(t, sizeof t, "constraint_residual %s\n", vb); put(o, t);
    jv_text(&E->povm, vb, sizeof vb); snprintf(t, sizeof t, "povm_residual %s\n", vb); put(o, t);
    for (int k = 0; k < E->M; k++) { snprintf(t, sizeof t, "label %d %s %s\n", k, c->labels[k], STATUS_NAME[E->status[k]]); put(o, t); }
    for (int k = 0; k < E->M; k++) { jv_text(&E->clock[k], vb, sizeof vb); snprintf(t, sizeof t, "clock_probability %d %s\n", k, vb); put(o, t); }
    for (int k = 0; k < E->M; k++) for (int q = 0; q < 6; q++) {
        jv_text(&E->pauli[k][q], vb, sizeof vb);
        snprintf(t, sizeof t, "pauli %d %s %s %s\n", k, AXES[q / 2], SIGNS[q % 2], vb); put(o, t);
    }
}

static at1_status write_common_head(buf_t *o, const at1_case *c)
{
    char cid[65], aid[65], t[256];
    /* a writer refuses blocks that do not reproduce the copied identities */
    at1_tagged_sha256("omega.at1.case.v1", c->semantic_text, c->semantic_len, cid);
    at1_tagged_sha256("omega.at1.acceptance.v1", c->acceptance_text, c->acceptance_len, aid);
    if (strcmp(cid, c->case_id) != 0 || strcmp(aid, c->acceptance_id) != 0) return AT1_ERR_INTERNAL;
    put(o, "OMEGA-AT1-RESULT v1\ndomain omega.at1.result.v1\ncontract AT1_RESULT_V1\ncase_contract AT1_CASE_V1\n");
    snprintf(t, sizeof t, "case_name %s\n", c->name); put(o, t);
    putn(o, c->semantic_text, c->semantic_len);
    putn(o, c->acceptance_text, c->acceptance_len);
    snprintf(t, sizeof t, "case_id %s\nacceptance_id %s\ncase_file_sha256 %s\n", c->case_id, c->acceptance_id, c->case_file_sha256); put(o, t);
    return AT1_OK;
}

static void write_tail(buf_t *o, const at1_case *c, const char *verdict, const char *prov)
{
    char vid[65], ev[65];
    size_t pre_len = 8 + 64 + 1 + 14 + 64 + 1;
    char *pre = at1_xmalloc(pre_len + strlen(verdict) + 1);
    snprintf(pre, pre_len + strlen(verdict) + 1, "case_id %s\nacceptance_id %s\n%s", c->case_id, c->acceptance_id, verdict);
    at1_tagged_sha256("omega.at1.verdict.v1", pre, strlen(pre), vid);
    free(pre);
    put(o, verdict);
    put(o, "verdict_id "); put(o, vid); put(o, "\n");
    put(o, "begin provenance\n"); put(o, prov); put(o, "end provenance\n");
    at1_tagged_sha256("omega.at1.evidence.v1", o->p, o->n, ev);
    put(o, "evidence_digest "); put(o, ev); put(o, "\nend\n");
}

at1_status at1_result_write(const at1_case *c, const at1_values *v, const char *oracle_text, size_t oracle_len,
                            const char *prov_text, size_t prov_len, char **out, size_t *out_len, char *err, size_t cap)
{
    *out = NULL; *out_len = 0; err[0] = 0;
    if (!oracle_text) return at1_result_write_error(c, "ORACLE_UNAVAILABLE", prov_text, prov_len, out, out_len, err, cap);
    refs_t R; buf_t prov = { 0 }, o = { 0 }, verdict = { 0 };
    at1_status st = refs_parse(c, oracle_text, oracle_len, &R, err, cap);
    if (st == AT1_OK) st = prov_parse(prov_text, prov_len, 0, &prov, err, cap);
    if (st == AT1_OK) st = write_common_head(&o, c);
    if (st != AT1_OK) { refs_free(&R); free(prov.p); free(o.p); return st; }
    eng_t E; eng_from_values(&E, v);
    put(&o, "begin numerics\narithmetic BINARY64\nbound_kind ESTIMATED\nthreads 1\nend numerics\nbegin values\n");
    write_engine_values(&o, c, &E);
    for (int i = 0; i < R.nlines; i++) { put(&o, R.lines[i]); put(&o, "\n"); }
    put(&o, "end values\n");
    int res[12]; unsigned codes;
    judge(c, &E, &R, res, &codes);
    char t[256];
    put(&verdict, "begin verdict\n");
    for (int i = 0; i < 12; i++) { snprintf(t, sizeof t, "check %s %s\n", CHECK_NAME[i], CHECK_STATE[res[i]]); put(&verdict, t); }
    int pass = codes == 0;
    for (int i = 0; i < 12; i++) if (res[i] == C_FAIL || res[i] == C_INDET) pass = 0;
    put(&verdict, pass ? "outcome PASS\n" : "outcome FAIL\n");
    put(&verdict, "failure_codes ");
    if (codes == 0) put(&verdict, "none");
    for (int i = 0, first = 1; i < AT1_FAILURE_CODE_COUNT; i++) if (codes & (1u << i)) { if (!first) put(&verdict, ","); put(&verdict, at1_failure_codes[i]); first = 0; }
    put(&verdict, "\nerror_code none\n");
    int met = (pass ? AT1_EXPECT_PASS : AT1_EXPECT_FAIL) == c->expected_outcome && codes == c->expected_codes;
    put(&verdict, met ? "expectation_met YES\n" : "expectation_met NO\n");
    put(&verdict, "end verdict\n");
    write_tail(&o, c, verdict.p, prov.p);
    eng_free(&E); refs_free(&R); free(prov.p); free(verdict.p);
    *out = o.p; *out_len = o.n;
    return AT1_OK;
}

at1_status at1_result_write_error(const at1_case *c, const char *error_code, const char *prov_text, size_t prov_len,
                                  char **out, size_t *out_len, char *err, size_t cap)
{
    *out = NULL; *out_len = 0; err[0] = 0;
    buf_t prov = { 0 }, o = { 0 }, verdict = { 0 };
    int oracle_none = strcmp(error_code, "ORACLE_UNAVAILABLE") == 0;
    at1_status st = prov_parse(prov_text, prov_len, oracle_none, &prov, err, cap);
    if (st == AT1_OK) st = write_common_head(&o, c);
    if (st != AT1_OK) { free(prov.p); free(o.p); return st; }
    const int M = c->label_count;
    char t[256];
    put(&o, "begin numerics\narithmetic BINARY64\nbound_kind ESTIMATED\nthreads 1\nend numerics\nbegin values\n");
    put(&o, "physical_state_kernel_dim 0\nconstraint_residual undefined 0@0\npovm_residual undefined 0@0\n");
    for (int k = 0; k < M; k++) { snprintf(t, sizeof t, "label %d %s UNDEFINED\n", k, c->labels[k]); put(&o, t); }
    for (int k = 0; k < M; k++) { snprintf(t, sizeof t, "clock_probability %d undefined 0@0\n", k); put(&o, t); }
    for (int k = 0; k < M; k++) for (int q = 0; q < 6; q++) { snprintf(t, sizeof t, "pauli %d %s %s undefined 0@0\n", k, AXES[q / 2], SIGNS[q % 2]); put(&o, t); }
    for (int k = 0; k < M; k++) { snprintf(t, sizeof t, "reference_label %d %s UNDEFINED\n", k, c->labels[k]); put(&o, t); }
    for (int k = 0; k < M; k++) { snprintf(t, sizeof t, "reference_clock_probability %d undefined 0@0\n", k); put(&o, t); }
    for (int f = 0; f < 2; f++) for (int k = 0; k < M; k++) for (int q = 0; q < 6; q++) {
        snprintf(t, sizeof t, "%s %d %s %s undefined 0@0\n", f == 0 ? "reference_ideal" : "reference_interacting", k, AXES[q / 2], SIGNS[q % 2]);
        put(&o, t);
    }
    put(&o, "end values\n");
    put(&verdict, "begin verdict\n");
    for (int i = 0; i < 12; i++) { snprintf(t, sizeof t, "check %s NOT_EVALUATED\n", CHECK_NAME[i]); put(&verdict, t); }
    snprintf(t, sizeof t, "outcome ERROR\nfailure_codes none\nerror_code %s\nexpectation_met NOT_APPLICABLE\nend verdict\n", error_code);
    put(&verdict, t);
    write_tail(&o, c, verdict.p, prov.p);
    free(prov.p); free(verdict.p);
    *out = o.p; *out_len = o.n;
    return AT1_OK;
}

at1_status at1_values_write(const at1_case *c, const at1_values *v, char **out, size_t *out_len)
{
    buf_t o = { 0 };
    eng_t E; eng_from_values(&E, v);
    write_engine_values(&o, c, &E);
    eng_free(&E);
    *out = o.p; *out_len = o.n;
    return AT1_OK;
}
