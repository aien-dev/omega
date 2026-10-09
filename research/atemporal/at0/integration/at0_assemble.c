/* AT-0 Agent 5: result assembler and exact judge (runner side, C11, -lm not needed).
 *
 * Joins one engine component output (OMEGA-AT0-ENGINE v1) with the `reference` lines of one
 * oracle record, judges the ten AT0_RESULT_V2 section 4 checks in exact integer arithmetic,
 * and writes one complete AT0_RESULT_V2 file (verdict block, verdict_id, provenance,
 * evidence_digest) to standard output. Shares no code with model/, oracle/ or evaluator/
 * except omega src/sha256.c (charter section 3).
 *
 *   at0-assemble assemble <case> <engine-out> <oracle-record> <provenance-lines>
 *   at0-assemble judge    <case> <values-file>    (calibration: prints verdict block + verdict_id)
 *
 * Exact arithmetic: every binary64 and every scaled decimal is lifted to an integer multiple of
 * the unit 2^-1074 * 10^-40 (all binary64 exponents >= -1074, all scaled decimals have k <= 40),
 * held in a 2304-bit two's complement integer. Nothing is ever compared as a float.
 * A binary64 magnitude of 2^100 or more is clamped to 2^100: it exceeds every tolerance
 * (all <= 1) either way, so no verdict changes. */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"

#define NL 72
typedef struct { uint32_t w[NL]; } Big;

static void b_zero(Big *a) { memset(a, 0, sizeof *a); }
static int b_isneg(const Big *a) { return (int)(a->w[NL - 1] >> 31); }
static void b_add(Big *r, const Big *a, const Big *b)
{
    uint64_t c = 0;
    for (int i = 0; i < NL; i++) { c += (uint64_t)a->w[i] + b->w[i]; r->w[i] = (uint32_t)c; c >>= 32; }
}
static void b_neg(Big *r, const Big *a)
{
    uint64_t c = 1;
    for (int i = 0; i < NL; i++) { c += (uint64_t)(uint32_t)~a->w[i]; r->w[i] = (uint32_t)c; c >>= 32; }
}
static void b_sub(Big *r, const Big *a, const Big *b) { Big n; b_neg(&n, b); b_add(r, a, &n); }
static int b_cmp(const Big *a, const Big *b)
{
    int na = b_isneg(a), nb = b_isneg(b);
    if (na != nb) return na ? -1 : 1;
    for (int i = NL - 1; i >= 0; i--) if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}
static void b_abs(Big *r, const Big *a) { if (b_isneg(a)) b_neg(r, a); else *r = *a; }
static void b_mul_small(Big *a, uint32_t m)
{
    uint64_t c = 0;
    for (int i = 0; i < NL; i++) { c += (uint64_t)a->w[i] * m; a->w[i] = (uint32_t)c; c >>= 32; }
}
static void b_add_small(Big *a, uint32_t m)
{
    uint64_t c = m;
    for (int i = 0; i < NL && c; i++) { c += a->w[i]; a->w[i] = (uint32_t)c; c >>= 32; }
}
static void b_shl(Big *a, unsigned n)
{
    unsigned ws = n / 32, bs = n % 32;
    for (int i = NL - 1; i >= 0; i--) {
        uint64_t v = 0;
        if ((int)(i - ws) >= 0) v = (uint64_t)a->w[i - ws] << bs;
        if (bs && (int)(i - ws - 1) >= 0) v |= (uint64_t)a->w[i - ws - 1] >> (32 - bs);
        a->w[i] = (uint32_t)v;
    }
}
static void b_pow10(Big *a, int n) { while (n >= 9) { b_mul_small(a, 1000000000u); n -= 9; } while (n-- > 0) b_mul_small(a, 10); }

/* value kinds */
enum { V_REAL = 0, V_NONFINITE = 1, V_UNDEFINED = 2, V_BAD = 3 };
typedef struct { int kind; Big v; Big b; } Val;

static Big g_one;     /* the integer 1 in units */
static char g_casesha[65];

static void from_f64(Big *r, uint64_t bits)
{
    int neg = (int)(bits >> 63), ex = (int)((bits >> 52) & 0x7ff);
    uint64_t fr = bits & 0xfffffffffffffULL, mant = ex ? (fr | (1ULL << 52)) : fr;
    int e = ex ? ex - 1075 : -1074, shift = e + 1074;
    if (shift > 1074 + 40) { mant = 1; shift = 1074 + 100; }   /* clamp, see header */
    b_zero(r);
    r->w[0] = (uint32_t)mant; r->w[1] = (uint32_t)(mant >> 32);
    b_shl(r, (unsigned)shift);
    b_pow10(r, 40);
    if (neg) b_neg(r, r);
}
/* "N@k": N decimal digits, 0 <= k <= 40. Returns 0 on success. */
static int from_scaled(Big *r, const char *s)
{
    const char *at = strchr(s, '@');
    if (!at || at == s || at - s > 60) return -1;
    char *end; long k = strtol(at + 1, &end, 10);
    if (*end || k < 0 || k > 40) return -1;
    b_zero(r);
    for (const char *p = s; p < at; p++) {
        if (!isdigit((unsigned char)*p)) return -1;
        b_mul_small(r, 10); b_add_small(r, (uint32_t)(*p - '0'));
    }
    b_pow10(r, (int)(40 - k));
    b_shl(r, 1074);
    return 0;
}
static int parse_value(Val *x, const char *vt, const char *bt)
{
    memset(x, 0, sizeof *x);
    if (!strcmp(vt, "nonfinite")) x->kind = V_NONFINITE;
    else if (!strcmp(vt, "undefined")) x->kind = V_UNDEFINED;
    else if (!strncmp(vt, "f64:", 4) && strlen(vt) == 20) {
        uint64_t bits = 0;
        for (int i = 4; i < 20; i++) {
            char c = vt[i]; int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (d < 0) return -1;
            bits = (bits << 4) | (uint64_t)d;
        }
        if (((bits >> 52) & 0x7ff) == 0x7ff) return -1;
        from_f64(&x->v, bits); x->kind = V_REAL;
    } else return -1;
    if (from_scaled(&x->b, bt)) return -1;
    return 0;
}

/* tri-state comparisons, section 4 */
enum { C_PASS = 0, C_FAIL = 1, C_INDET = 2, C_NE = 3 };
static int absform(const Big *v, const Big *b, const Big *tol)
{
    Big a, t; b_abs(&a, v);
    b_add(&t, &a, b); if (b_cmp(&t, tol) <= 0) return C_PASS;
    b_sub(&t, &a, b); if (b_cmp(&t, tol) > 0) return C_FAIL;
    return C_INDET;
}
static int signedform(const Big *v, const Big *b, const Big *tol)
{
    Big t;
    b_add(&t, v, b); if (b_cmp(&t, tol) <= 0) return C_PASS;
    b_sub(&t, v, b); if (b_cmp(&t, tol) > 0) return C_FAIL;
    return C_INDET;
}
typedef struct { int seen, fail, indet; } Agg;
static void agg_add(Agg *g, int c) { g->seen = 1; if (c == C_FAIL) g->fail = 1; else if (c == C_INDET) g->indet = 1; }
static int agg_res(const Agg *g) { return !g->seen ? C_NE : g->fail ? C_FAIL : g->indet ? C_INDET : C_PASS; }

/* ---------- text helpers ---------- */
typedef struct { char *data; size_t len; char **line; int n; } Text;
static int read_file(const char *path, Text *t)
{
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    size_t cap = 1 << 16; t->data = malloc(cap + 1); t->len = 0;
    for (;;) {
        if (t->len == cap) { cap *= 2; t->data = realloc(t->data, cap + 1); }
        size_t k = fread(t->data + t->len, 1, cap - t->len, f); if (!k) break; t->len += k;
    }
    fclose(f); t->data[t->len] = 0;
    int n = 0; for (size_t i = 0; i < t->len; i++) if (t->data[i] == '\n') n++;
    t->line = malloc(sizeof(char *) * (size_t)(n + 2)); t->n = 0;
    char *p = t->data;
    for (size_t i = 0; i < t->len; i++) if (t->data[i] == '\n') { t->data[i] = 0; t->line[t->n++] = p; p = t->data + i + 1; }
    if (*p) t->line[t->n++] = p;
    return 0;
}
static int tokens(const char *line, char tok[][96], int max)
{
    int n = 0; const char *p = line;
    while (*p && n < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        int k = 0; while (*p && *p != ' ') { if (k < 95) tok[n][k++] = *p; p++; }
        tok[n][k] = 0; n++;
    }
    return n;
}
static int find_line(const Text *t, const char *s, int from)
{
    for (int i = from; i < t->n; i++) if (!strcmp(t->line[i], s)) return i;
    return -1;
}
static const char *key_value(const Text *t, int from, int to, const char *key)
{
    size_t kl = strlen(key);
    for (int i = from; i < to && i < t->n; i++)
        if (!strncmp(t->line[i], key, kl) && t->line[i][kl] == ' ') return t->line[i] + kl + 1;
    return NULL;
}

/* ---------- values ---------- */
#define MAXM 256
typedef struct {
    int kernel_dim, M;
    Val constraint, povm;
    int status[MAXM];                 /* 0 DEFINED 1 UNDEFINED 2 INDETERMINATE */
    Val clock[MAXM], pauli[MAXM][6], ref[MAXM][6];
    int have_ref;
} Values;
static int axis_index(const char *a, const char *s)
{
    int ai = !strcmp(a, "X") ? 0 : !strcmp(a, "Y") ? 1 : !strcmp(a, "Z") ? 2 : -1;
    int si = !strcmp(s, "PLUS") ? 0 : !strcmp(s, "MINUS") ? 1 : -1;
    return (ai < 0 || si < 0) ? -1 : ai * 2 + si;
}
/* lines[from..to) are the lines strictly between "begin values" and "end values" */
static int parse_values(char **lines, int from, int to, Values *V, char *err, size_t ecap)
{
    memset(V, 0, sizeof *V); V->kernel_dim = -1;
    int nlabel = 0, nclock = 0, npauli = 0, nref = 0;
    char t[8][96];
    for (int i = from; i < to; i++) {
        int n = tokens(lines[i], t, 8);
        if (n < 1) continue;
        if (!strcmp(t[0], "physical_state_kernel_dim") && n == 2) V->kernel_dim = atoi(t[1]);
        else if (!strcmp(t[0], "constraint_residual") && n == 3) { if (parse_value(&V->constraint, t[1], t[2])) goto bad; }
        else if (!strcmp(t[0], "povm_residual") && n == 3) { if (parse_value(&V->povm, t[1], t[2])) goto bad; }
        else if (!strcmp(t[0], "label") && n == 4) {
            int k = atoi(t[1]); if (k != nlabel || k >= MAXM) goto bad;
            V->status[k] = !strcmp(t[3], "DEFINED") ? 0 : !strcmp(t[3], "UNDEFINED") ? 1 : !strcmp(t[3], "INDETERMINATE") ? 2 : -1;
            if (V->status[k] < 0) goto bad;
            nlabel++;
        } else if (!strcmp(t[0], "clock_probability") && n == 4) {
            int k = atoi(t[1]); if (k != nclock || k >= MAXM || parse_value(&V->clock[k], t[2], t[3])) goto bad;
            nclock++;
        } else if ((!strcmp(t[0], "pauli") || !strcmp(t[0], "reference")) && n == 6) {
            int k = atoi(t[1]), s = axis_index(t[2], t[3]); int isref = t[0][0] == 'r';
            int *cnt = isref ? &nref : &npauli;
            if (s < 0 || k >= MAXM || k * 6 + s != *cnt) goto bad;
            if (parse_value(isref ? &V->ref[k][s] : &V->pauli[k][s], t[4], t[5])) goto bad;
            (*cnt)++;
        } else goto bad;
        continue;
    bad:
        snprintf(err, ecap, "unparseable or out-of-order values line: %.80s", lines[i]); return -1;
    }
    V->M = nlabel;
    if (V->kernel_dim < 0 || nclock != nlabel || npauli != 6 * nlabel || (nref != 0 && nref != 6 * nlabel)) {
        snprintf(err, ecap, "values block counts: labels %d clock %d pauli %d reference %d", nlabel, nclock, npauli, nref); return -1;
    }
    V->have_ref = nref != 0;
    return 0;
}

/* ---------- the judge ---------- */
typedef struct {
    Big tol_con, tol_povm, tol_prob, tol_zero, tol_schro;
    int min_kind;                    /* 0 NONE 1 ESTIMATED 2 RIGOROUS */
    char expected_outcome[16], expected_codes[512];
} Accept;
static const char *CHECKS[10] = { "bound_kind_sufficient", "values_finite", "physical_state_nontrivial", "constraint_residual",
    "povm_normalization", "clock_probability_sum", "probability_range", "pauli_pair_sum", "conditional_defined", "schrodinger_agreement" };
static const char *CODES[10] = { "BOUND_KIND_INSUFFICIENT", "CONDITIONAL_UNDEFINED", "CONSTRAINT_RESIDUAL_EXCEEDED", "NONFINITE_VALUE",
    "POVM_NORMALIZATION_EXCEEDED", "PRECISION_INSUFFICIENT", "PROBABILITY_OUT_OF_RANGE", "PROBABILITY_SUM_EXCEEDED",
    "SCHRODINGER_DEVIATION_EXCEEDED", "TRIVIAL_PHYSICAL_STATE" };      /* bytewise sorted */
enum { K_BOUND, K_COND, K_CON, K_NONFIN, K_POVM, K_PREC, K_RANGE, K_SUM, K_SCHRO, K_TRIV };
static const char *STATE[4] = { "PASS", "FAIL", "INDETERMINATE", "NOT_EVALUATED" };

static int is_fin(const Val *x) { return x->kind == V_REAL; }

static void judge(const Values *V, int bound_kind, const Accept *A, int res[10], unsigned *codes, int *label_indet)
{
    int trivial = V->kernel_dim == 0 || V->constraint.kind == V_UNDEFINED;
    int M = V->M; Agg g;
    *codes = 0; *label_indet = 0;
    for (int k = 0; k < M; k++) if (V->status[k] == 2) *label_indet = 1;
    res[0] = bound_kind >= A->min_kind ? C_PASS : C_FAIL;
    int nonfin = V->constraint.kind == V_NONFINITE || V->povm.kind == V_NONFINITE;
    for (int k = 0; k < M; k++) {
        if (V->clock[k].kind == V_NONFINITE) nonfin = 1;
        for (int s = 0; s < 6; s++) if (V->pauli[k][s].kind == V_NONFINITE || V->ref[k][s].kind == V_NONFINITE) nonfin = 1;
    }
    res[1] = nonfin ? C_FAIL : C_PASS;
    res[2] = trivial ? C_FAIL : C_PASS;
    memset(&g, 0, sizeof g);
    if (!trivial && is_fin(&V->constraint)) agg_add(&g, absform(&V->constraint.v, &V->constraint.b, &A->tol_con));
    res[3] = trivial ? C_NE : agg_res(&g);
    memset(&g, 0, sizeof g);
    if (is_fin(&V->povm)) agg_add(&g, absform(&V->povm.v, &V->povm.b, &A->tol_povm));
    res[4] = agg_res(&g);
    /* 6: sum of clock probabilities */
    memset(&g, 0, sizeof g);
    if (!trivial) {
        Big sv, sb, d; int all = 1; b_zero(&sv); b_zero(&sb);
        for (int k = 0; k < M; k++) { if (!is_fin(&V->clock[k])) { all = 0; break; } b_add(&sv, &sv, &V->clock[k].v); b_add(&sb, &sb, &V->clock[k].b); }
        if (all && M > 0) { b_sub(&d, &sv, &g_one); agg_add(&g, absform(&d, &sb, &A->tol_prob)); }
    }
    res[5] = trivial ? C_NE : agg_res(&g);
    /* 7: every written probability, signed form both ways (clock, pauli, reference) */
    memset(&g, 0, sizeof g);
    if (!trivial) {
        for (int k = 0; k < M; k++) {
            const Val *p[13]; int np = 0;
            p[np++] = &V->clock[k];
            for (int s = 0; s < 6; s++) { p[np++] = &V->pauli[k][s]; p[np++] = &V->ref[k][s]; }
            for (int i = 0; i < np; i++) {
                if (!is_fin(p[i])) continue;
                Big neg, over; b_neg(&neg, &p[i]->v); b_sub(&over, &p[i]->v, &g_one);
                agg_add(&g, signedform(&neg, &p[i]->b, &A->tol_prob));
                agg_add(&g, signedform(&over, &p[i]->b, &A->tol_prob));
            }
        }
    }
    res[6] = trivial ? C_NE : agg_res(&g);
    /* 8: PLUS + MINUS - 1 per defined label and axis */
    memset(&g, 0, sizeof g);
    if (!trivial)
        for (int k = 0; k < M; k++) {
            if (V->status[k] != 0) continue;
            for (int a = 0; a < 3; a++) {
                const Val *pl = &V->pauli[k][2 * a], *mi = &V->pauli[k][2 * a + 1];
                if (!is_fin(pl) || !is_fin(mi)) continue;
                Big s, d, b; b_add(&s, &pl->v, &mi->v); b_sub(&d, &s, &g_one); b_add(&b, &pl->b, &mi->b);
                agg_add(&g, absform(&d, &b, &A->tol_prob));
            }
        }
    res[7] = trivial ? C_NE : agg_res(&g);
    /* 9 */
    if (trivial) res[8] = C_NE;
    else { res[8] = C_PASS; for (int k = 0; k < M; k++) if (V->status[k] == 1) res[8] = C_FAIL; }
    /* 10 */
    memset(&g, 0, sizeof g);
    if (!trivial)
        for (int k = 0; k < M; k++) {
            if (V->status[k] != 0) continue;
            for (int s = 0; s < 6; s++) {
                const Val *p = &V->pauli[k][s], *r = &V->ref[k][s];
                if (!is_fin(p) || !is_fin(r)) continue;
                Big d, b; b_sub(&d, &p->v, &r->v); b_add(&b, &p->b, &r->b);
                agg_add(&g, absform(&d, &b, &A->tol_schro));
            }
        }
    res[9] = trivial ? C_NE : agg_res(&g);
    static const int code_of[10] = { K_BOUND, K_NONFIN, K_TRIV, K_CON, K_POVM, K_SUM, K_RANGE, K_SUM, K_COND, K_SCHRO };
    int indet = *label_indet;
    for (int i = 0; i < 10; i++) { if (res[i] == C_FAIL) *codes |= 1u << code_of[i]; if (res[i] == C_INDET) indet = 1; }
    if (indet) *codes |= 1u << K_PREC;
}

/* label status derived from written clock probability and tol_zero_probability (section 3) */
static int derive_status(const Val *c, const Big *tol_zero)
{
    Big t;
    b_add(&t, &c->v, &c->b); if (b_cmp(&t, tol_zero) <= 0) return 1;
    b_sub(&t, &c->v, &c->b); if (b_cmp(&t, tol_zero) > 0) return 0;
    return 2;
}

/* ---------- output buffer ---------- */
typedef struct { char *p; size_t n, cap; } Buf;
static void bput(Buf *b, const char *s)
{
    size_t k = strlen(s);
    if (b->n + k + 1 > b->cap) { b->cap = (b->n + k + 1) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, k + 1); b->n += k;
}
static void hex(const uint8_t *d, char *out) { for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]); }
static void sha_tagged(const char *tag, const char *pre, const char *body, size_t blen, char out[65])
{
    sha256_ctx c; uint8_t d[32]; sha256_init(&c);
    sha256_update(&c, (const uint8_t *)tag, strlen(tag)); uint8_t z = 0; sha256_update(&c, &z, 1);
    if (pre) sha256_update(&c, (const uint8_t *)pre, strlen(pre));
    sha256_update(&c, (const uint8_t *)body, blen);
    sha256_final(&c, d); hex(d, out);
}

static int die(const char *m, const char *a) { fprintf(stderr, "AT0_ASSEMBLE_ERROR %s%s%s\n", m, a ? " " : "", a ? a : ""); return 3; }

static int load_case(const char *path, Text *c, Accept *A, int *sem_a, int *sem_b, int *acc_a, int *acc_b)
{
    if (read_file(path, c)) return die("cannot read case", path);
    *sem_a = find_line(c, "begin semantic", 0); *sem_b = find_line(c, "end semantic", 0);
    *acc_a = find_line(c, "begin acceptance", 0); *acc_b = find_line(c, "end acceptance", 0);
    if (*sem_a < 0 || *sem_b < *sem_a || *acc_a != *sem_b + 1 || *acc_b < *acc_a) return die("case blocks not found in", path);
    const char *v;
    if (!(v = key_value(c, *acc_a, *acc_b, "expected_outcome")) || strlen(v) > 15) return die("case lacks expected_outcome", NULL);
    strcpy(A->expected_outcome, v);
    if (!(v = key_value(c, *acc_a, *acc_b, "expected_failure_codes")) || strlen(v) > 500) return die("case lacks expected_failure_codes", NULL);
    strcpy(A->expected_codes, v);
    if (!(v = key_value(c, *acc_a, *acc_b, "min_bound_kind"))) return die("case lacks min_bound_kind", NULL);
    A->min_kind = !strcmp(v, "NONE") ? 0 : !strcmp(v, "ESTIMATED") ? 1 : !strcmp(v, "RIGOROUS") ? 2 : -1;
    if (A->min_kind < 0) return die("bad min_bound_kind", v);
    static const char *keys[5] = { "tol_constraint_residual", "tol_povm_residual", "tol_probability", "tol_zero_probability", "tol_schrodinger" };
    Big *dst[5] = { &A->tol_con, &A->tol_povm, &A->tol_prob, &A->tol_zero, &A->tol_schro };
    for (int i = 0; i < 5; i++) {
        if (!(v = key_value(c, *acc_a, *acc_b, keys[i])) || from_scaled(dst[i], v)) return die("case lacks or mangles", keys[i]);
    }
    return 0;
}

static void verdict_block(Buf *o, const int res[10], unsigned codes, int label_indet, const Accept *A, char *codes_txt)
{
    int any_indet = label_indet; for (int i = 0; i < 10; i++) if (res[i] == C_INDET) any_indet = 1;
    int outcome_pass = codes == 0 && !any_indet;
    codes_txt[0] = 0;
    for (int i = 0; i < 10; i++) if (codes & (1u << i)) { if (codes_txt[0]) strcat(codes_txt, ","); strcat(codes_txt, CODES[i]); }
    if (!codes_txt[0]) strcpy(codes_txt, "none");
    int met = !strcmp(A->expected_outcome, outcome_pass ? "PASS" : "FAIL") && !strcmp(A->expected_codes, codes_txt);
    bput(o, "begin verdict\n");
    for (int i = 0; i < 10; i++) { bput(o, "check "); bput(o, CHECKS[i]); bput(o, " "); bput(o, STATE[res[i]]); bput(o, "\n"); }
    bput(o, "outcome "); bput(o, outcome_pass ? "PASS" : "FAIL"); bput(o, "\nfailure_codes "); bput(o, codes_txt);
    bput(o, "\nerror_code none\nexpectation_met "); bput(o, met ? "YES" : "NO"); bput(o, "\nend verdict\n");
}

int main(int argc, char **argv)
{
    memset(&g_one, 0, sizeof g_one); g_one.w[0] = 1; b_pow10(&g_one, 40); b_shl(&g_one, 1074);
    if (argc >= 4 && !strcmp(argv[1], "judge") && argc == 4) {
        Text c, v; Accept A; int sa, sb, aa, ab; char err[200];
        int rc = load_case(argv[2], &c, &A, &sa, &sb, &aa, &ab); if (rc) return rc;
        if (read_file(argv[3], &v)) return die("cannot read", argv[3]);
        int vb = find_line(&v, "begin values", 0), ve = find_line(&v, "end values", 0);
        const char *bk = NULL; int nk = find_line(&v, "begin numerics", 0);
        if (nk >= 0) bk = key_value(&v, nk, nk + 5, "bound_kind");
        if (vb < 0 || ve < vb) return die("no values block in", argv[3]);
        Values *V = malloc(sizeof *V);
        if (parse_values(v.line, vb + 1, ve, V, err, sizeof err)) return die(err, NULL);
        int bkind = bk ? (!strcmp(bk, "RIGOROUS") ? 2 : !strcmp(bk, "ESTIMATED") ? 1 : 0) : 1;
        int res[10]; unsigned codes; int li; char ct[600]; Buf o = { 0 }; bput(&o, "");
        judge(V, bkind, &A, res, &codes, &li);
        verdict_block(&o, res, codes, li, &A, ct);
        const char *cid = key_value(&c, 0, c.n, "case_id"), *aid = key_value(&c, 0, c.n, "acceptance_id");
        char pre[300]; snprintf(pre, sizeof pre, "case_id %s\nacceptance_id %s\n", cid, aid);
        char vid[65]; sha_tagged("omega.at0.verdict.v2", pre, o.p, o.n, vid);
        printf("%sverdict_id %s\n", o.p, vid);
        return 0;
    }
    if (argc != 6 || strcmp(argv[1], "assemble")) {
        fprintf(stderr, "usage: at0-assemble assemble <case> <engine-out> <oracle-record> <provenance-lines>\n       at0-assemble judge <case> <values-file>\n");
        return 1;
    }
    Text c, e, r, pv; Accept A; int sa, sb, aa, ab; char err[200];
    int rc = load_case(argv[2], &c, &A, &sa, &sb, &aa, &ab); if (rc) return rc;
    if (read_file(argv[3], &e)) return die("cannot read", argv[3]);
    if (read_file(argv[4], &r)) return die("cannot read", argv[4]);
    if (read_file(argv[5], &pv)) return die("cannot read", argv[5]);
    /* case identity lines and file digest */
    const char *cname = key_value(&c, 0, 6, "case_name");
    const char *cid = key_value(&c, aa, c.n, "case_id"), *aid = key_value(&c, aa, c.n, "acceptance_id");
    if (!cname || !cid || !aid) return die("case lacks case_name/case_id/acceptance_id", NULL);
    { FILE *f = fopen(argv[2], "rb"); sha256_ctx s; uint8_t d[32], b[4096]; size_t k; char h[65];
      sha256_init(&s); while ((k = fread(b, 1, sizeof b, f)) > 0) sha256_update(&s, b, k); fclose(f); sha256_final(&s, d); hex(d, h);
      const char *ef = key_value(&e, 0, 12, "case_file_sha256");
      if (!ef || strcmp(ef, h)) return die("engine case_file_sha256 differs from the case file", NULL);
      memcpy(g_casesha, h, 65); }
    const char *ecid = key_value(&e, 0, 12, "case_id"), *eaid = key_value(&e, 0, 12, "acceptance_id"), *ename = key_value(&e, 0, 12, "case_name");
    if (!ecid || !eaid || !ename || strcmp(ecid, cid) || strcmp(eaid, aid) || strcmp(ename, cname)) return die("engine identities differ from the case file", NULL);
    if (strcmp(e.line[0], "OMEGA-AT0-ENGINE v1") || strcmp(key_value(&e, 0, 4, "contract") ? key_value(&e, 0, 4, "contract") : "", "AT0_RESULT_V2"))
        return die("engine output is not OMEGA-AT0-ENGINE v1 / AT0_RESULT_V2", NULL);
    /* oracle record: identities, oracle marking (Agent 0 condition on omega#359) */
    const char *ocid = key_value(&r, 0, r.n, "case_id");
    if (!ocid || strcmp(ocid, cid)) return die("oracle record is for a different case", NULL);
    { const char *bc = key_value(&r, 0, r.n, "build_cc"), *es = key_value(&r, 0, r.n, "engine_sha256"), *os = key_value(&r, 0, r.n, "oracle_sha256");
      if (!bc || strncmp(bc, "oracle ", 7)) return die("oracle record build_cc does not begin with 'oracle '", NULL);
      if (!es || !os || strcmp(es, os)) return die("oracle record engine_sha256 != oracle_sha256", NULL); }
    int nb = find_line(&e, "begin numerics", 0), ne = find_line(&e, "end numerics", 0);
    int vb = find_line(&e, "begin values", 0), ve = find_line(&e, "end values", 0);
    int ob = find_line(&r, "begin values", 0), oe = find_line(&r, "end values", 0);
    if (nb < 0 || ne != nb + 4 || vb < ne || ve < vb || ob < 0 || oe < ob) return die("block structure of engine or oracle output", NULL);
    /* values block = engine lines (no reference lines allowed) + oracle reference lines only */
    int nref = 0; char **vl = malloc(sizeof(char *) * (size_t)(e.n + r.n + 4)); int nv = 0;
    for (int i = vb + 1; i < ve; i++) { if (!strncmp(e.line[i], "reference ", 10)) return die("engine output contains reference lines", NULL); vl[nv++] = e.line[i]; }
    for (int i = ob + 1; i < oe; i++) if (!strncmp(r.line[i], "reference ", 10)) { vl[nv++] = r.line[i]; nref++; }
    Values *V = malloc(sizeof *V);
    if (parse_values(vl, 0, nv, V, err, sizeof err)) return die(err, NULL);
    if (!V->have_ref) return die("oracle supplied no reference lines", NULL);
    const char *bk = key_value(&e, nb, ne, "bound_kind"); const char *ar = key_value(&e, nb, ne, "arithmetic"); const char *th = key_value(&e, nb, ne, "threads");
    if (!bk || !ar || !th || strcmp(th, "1")) return die("engine numerics block", NULL);
    int bkind = !strcmp(bk, "RIGOROUS") ? 2 : !strcmp(bk, "ESTIMATED") ? 1 : 0;
    /* label status consistency note (not fatal: the independent verifier is the arbiter) */
    if (V->kernel_dim > 0 && V->constraint.kind != V_UNDEFINED)
        for (int k = 0; k < V->M; k++) if (V->clock[k].kind == V_REAL && derive_status(&V->clock[k], &A.tol_zero) != V->status[k])
            fprintf(stderr, "AT0_ASSEMBLE_NOTE label %d written status differs from the section 3 rule\n", k);
    int res[10]; unsigned codes; int li; char ct[600];
    judge(V, bkind, &A, res, &codes, &li);
    Buf o = { 0 }; bput(&o, "");
    bput(&o, "OMEGA-AT0-RESULT v2\ndomain omega.at0.result.v2\ncontract AT0_RESULT_V2\ncase_contract AT0_CASE_V1\ncase_name ");
    bput(&o, cname); bput(&o, "\n");
    for (int i = sa; i <= ab; i++) { bput(&o, c.line[i]); bput(&o, "\n"); }
    bput(&o, "case_id "); bput(&o, cid); bput(&o, "\nacceptance_id "); bput(&o, aid);
    bput(&o, "\ncase_file_sha256 "); bput(&o, g_casesha); bput(&o, "\n");
    for (int i = nb; i <= ne; i++) { bput(&o, e.line[i]); bput(&o, "\n"); }
    bput(&o, "begin values\n");
    for (int i = 0; i < nv; i++) {
        /* ordering: engine lines up to the pauli lines, then the oracle's reference lines */
        bput(&o, vl[i]); bput(&o, "\n");
    }
    bput(&o, "end values\n");
    Buf vbk = { 0 }; bput(&vbk, "");
    verdict_block(&vbk, res, codes, li, &A, ct);
    bput(&o, vbk.p);
    char pre[300]; snprintf(pre, sizeof pre, "case_id %s\nacceptance_id %s\n", cid, aid);
    char vid[65]; sha_tagged("omega.at0.verdict.v2", pre, vbk.p, vbk.n, vid);
    bput(&o, "verdict_id "); bput(&o, vid); bput(&o, "\nbegin provenance\n");
    for (int i = 0; i < pv.n; i++) { bput(&o, pv.line[i]); bput(&o, "\n"); }
    bput(&o, "end provenance\n");
    char ev[65]; sha_tagged("omega.at0.evidence.v2", NULL, o.p, o.n, ev);
    bput(&o, "evidence_digest "); bput(&o, ev); bput(&o, "\nend\n");
    fwrite(o.p, 1, o.n, stdout);
    return 0;
}
