/* AT1_CASE_V1 codec: parse and validate a case in the order of AT1_CASE_V1 section 4, first
 * failure wins:
 *   1. shape and encodings: CASE_PARSE_ERROR for any shape failure anywhere in the file, then
 *      CASE_NONCANONICAL for a parsed but non-canonical token (decided after the whole shape pass);
 *   2. values of the three version-bearing lines: CASE_UNSUPPORTED_VERSION;
 *   3. ranges and fixed values: CASE_INVALID_PARAMETER;
 *   4. rational spectrum per clock level: CASE_IRRATIONAL_SPECTRUM;
 *   5. identities: CASE_ID_MISMATCH;
 *   6. acceptance codes: CASE_INVALID_PARAMETER.
 * Readings applied (AT1_CHARTER.md section 8 and the inherited AT-0 readings):
 *   - (d) the first token of each version-bearing line is its key (checked in step 1), the rest
 *     of the line is its value (checked in step 2);
 *   - (e) an index token (interaction_pauli, clock_label) is compared with its zero-based
 *     position by parsed integer value in the shape pass; canonical form is decided afterwards;
 *   - shape before range: count lines that disagree with their repeated block, fixed literals
 *     and enumeration tokens are shape errors; token limits are rule 3;
 *   - token parse classes follow the qualified AT-0 codec: a denominator below 1 (including a
 *     negative one) is non-canonical, a scaled decimal with a sign is non-canonical.
 * Every exact decision uses at1_bn.c (no fixed-width arithmetic can overflow). */
#include "at1_model.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

const char *const at1_failure_codes[AT1_FAILURE_CODE_COUNT] = {
    "BOUND_KIND_INSUFFICIENT", "CONDITIONAL_UNDEFINED", "CONSTRAINT_RESIDUAL_EXCEEDED",
    "INTERACTING_DEVIATION_EXCEEDED", "NONFINITE_VALUE", "ORACLE_DISAGREEMENT",
    "POVM_NORMALIZATION_EXCEEDED", "PRECISION_INSUFFICIENT", "PROBABILITY_OUT_OF_RANGE",
    "PROBABILITY_SUM_EXCEEDED", "SCHRODINGER_DEVIATION_EXCEEDED", "TRIVIAL_PHYSICAL_STATE"
};

const char *at1_status_name(at1_status s)
{
    switch (s) {
    case AT1_OK: return "OK";
    case AT1_CASE_PARSE_ERROR: return "CASE_PARSE_ERROR";
    case AT1_CASE_NONCANONICAL: return "CASE_NONCANONICAL";
    case AT1_CASE_UNSUPPORTED_VERSION: return "CASE_UNSUPPORTED_VERSION";
    case AT1_CASE_INVALID_PARAMETER: return "CASE_INVALID_PARAMETER";
    case AT1_CASE_IRRATIONAL_SPECTRUM: return "CASE_IRRATIONAL_SPECTRUM";
    case AT1_CASE_ID_MISMATCH: return "CASE_ID_MISMATCH";
    case AT1_ERR_RESOURCE: return "RESOURCE_LIMIT";
    case AT1_ERR_INTERNAL: return "INTERNAL_ERROR";
    case AT1_ERR_ARGUMENT: return "ARGUMENT";
    }
    return "UNKNOWN";
}

/* ---- digests ------------------------------------------------------------------------------- */
static void hex32(const uint8_t d[32], char out[65])
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = hx[d[i] >> 4]; out[2 * i + 1] = hx[d[i] & 15]; }
    out[64] = 0;
}
void at1_tagged_sha256(const char *tag, const char *data, size_t len, char out[65])
{
    sha256_ctx ctx; uint8_t d[32]; uint8_t zero = 0;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)tag, strlen(tag));
    sha256_update(&ctx, &zero, 1);
    sha256_update(&ctx, (const uint8_t *)data, len);
    sha256_final(&ctx, d);
    hex32(d, out);
}
void at1_sha256_hex(const uint8_t *data, size_t len, char out[65])
{
    uint8_t d[32];
    sha256_hash(data, len, d);
    hex32(d, out);
}

/* ---- line table ---------------------------------------------------------------------------- */
typedef struct { char **line; size_t *off; int count, pos; char *copy; } lines_t;

static at1_status split_lines(const uint8_t *b, size_t len, lines_t *L)
{
    L->line = NULL; L->off = NULL; L->count = 0; L->pos = 0; L->copy = NULL;
    if (len == 0 || b[len - 1] != '\n') return AT1_CASE_PARSE_ERROR;
    for (size_t i = 0; i < len; i++) if (b[i] != '\n' && (b[i] < 0x20 || b[i] > 0x7E)) return AT1_CASE_PARSE_ERROR;
    int n = 0;
    for (size_t i = 0; i < len; i++) if (b[i] == '\n') n++;
    L->copy = at1_xmalloc(len + 1);
    memcpy(L->copy, b, len); L->copy[len] = 0;
    L->line = at1_xmalloc((size_t)n * sizeof(char *));
    L->off = at1_xmalloc((size_t)(n + 1) * sizeof(size_t));
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (b[i] != '\n') continue;
        size_t ln = i - start;
        if (ln == 0) return AT1_CASE_PARSE_ERROR;                               /* blank line */
        if (b[start] == ' ' || b[i - 1] == ' ') return AT1_CASE_PARSE_ERROR;   /* leading/trailing space */
        for (size_t j = start + 1; j < i; j++) if (b[j] == ' ' && b[j - 1] == ' ') return AT1_CASE_PARSE_ERROR;
        L->copy[i] = 0;
        L->line[L->count] = L->copy + start; L->off[L->count] = start; L->count++;
        start = i + 1;
    }
    L->off[L->count] = len;
    return AT1_OK;
}
static void lines_free(lines_t *L) { free(L->line); free(L->off); free(L->copy); }
static const char *peek(const lines_t *L) { return L->pos < L->count ? L->line[L->pos] : NULL; }
static const char *take(lines_t *L) { return L->pos < L->count ? L->line[L->pos++] : NULL; }

/* line "<key> <rest>" -> rest, or NULL when the key differs or the line has no value */
static const char *key_rest(const char *line, const char *key)
{
    size_t k = strlen(key);
    if (!line || strncmp(line, key, k) != 0 || line[k] != ' ') return NULL;
    return line + k + 1;
}
static int line_key_is(const char *line, const char *key) { return key_rest(line, key) != NULL; }

/* ---- token grammar ------------------------------------------------------------------------- */
static int is_digit(char c) { return c >= '0' && c <= '9'; }

typedef struct {
    int shape;          /* a shape failure was seen (PARSE_ERROR) */
    int noncanonical;   /* a parsed, non-canonical token was seen */
    int resource;       /* a token beyond the engine's digit limit was seen */
    int over_limit;     /* a rational token outside the section 2 limits (rule 3) */
    int scaled_k_over;  /* a scaled decimal with k > 40 (rule 3) */
} flags_t;

/* integer token "-?[0-9]+": returns 0 shape failure, 1 parsed. *canon, *val (bn, exact). */
static int parse_int_tok(const char *s, size_t len, int *canon, bn *val, flags_t *f)
{
    size_t i = 0; int neg = 0;
    if (len == 0) return 0;
    if (s[0] == '-') { neg = 1; i = 1; }
    if (i == len) return 0;
    for (size_t j = i; j < len; j++) if (!is_digit(s[j])) return 0;
    *canon = !(s[i] == '0' && (len - i > 1 || neg));
    size_t z = i;
    while (z < len - 1 && s[z] == '0') z++;                 /* value of the significant digits */
    if (!bn_from_digits(val, s + z, len - z)) { f->resource = 1; bn_set_u64(val, 0); return 1; }
    if (neg && val->n) val->neg = 1;
    return 1;
}

/* small integer for counts and indices; saturates far above every limit */
static int parse_small_int(const char *s, size_t len, long *out, flags_t *f)
{
    int canon; bn v; bn_init(&v);
    if (!parse_int_tok(s, len, &canon, &v, f)) { bn_free(&v); return 0; }
    int64_t x;
    if (!bn_fits_i64(&v, &x) || x > 1000000000 || x < -1000000000) x = v.neg ? -1000000001 : 1000000001;
    *out = (long)x;
    if (!canon) f->noncanonical = 1;
    bn_free(&v);
    return 1;
}

static int bn_within(const bn *a, int64_t lim)
{
    bn l; bn_init(&l); bn_set_i64(&l, lim);
    int ok = bn_cmp_abs(a, &l) <= 0;
    bn_free(&l);
    return ok;
}

/* rational "n/d": 0 on shape failure; sets flags for non-canonical and over-limit tokens */
static int parse_rat_tok(const char *s, size_t len, bq *out, flags_t *f)
{
    const char *slash = memchr(s, '/', len);
    if (!slash) return 0;
    size_t ln = (size_t)(slash - s), ld = len - ln - 1;
    int cn, cd; bn n, d, g; bn_init(&n); bn_init(&d); bn_init(&g);
    int ok = parse_int_tok(s, ln, &cn, &n, f) && parse_int_tok(slash + 1, ld, &cd, &d, f);
    if (!ok) { bn_free(&n); bn_free(&d); bn_free(&g); return 0; }
    int canonical = cn && cd && bn_sign(&d) > 0;
    if (canonical) { bn_gcd(&g, &n, &d); if (!(g.n == 1 && g.d[0] == 1)) canonical = 0; }
    if (!canonical) f->noncanonical = 1;
    if (!bn_within(&n, AT1_TOKEN_LIMIT) || !bn_within(&d, AT1_TOKEN_LIMIT) || bn_sign(&d) <= 0) f->over_limit = 1;
    if (bn_sign(&d) > 0) bq_set_bn(out, &n, &d); else bq_set_i64(out, 0, 1);
    bn_free(&n); bn_free(&d); bn_free(&g);
    return 1;
}

static int parse_crat_tok(const char *s, size_t len, gq *out, flags_t *f)
{
    if (len < 2 || s[0] != '(' || s[len - 1] != ')') return 0;
    const char *semi = memchr(s, ';', len);
    if (!semi) return 0;
    size_t la = (size_t)(semi - s - 1), lb = len - 2 - la - 1;
    if (memchr(semi + 1, ';', lb)) return 0;
    return parse_rat_tok(s + 1, la, &out->re, f) && parse_rat_tok(semi + 1, lb, &out->im, f);
}

static int parse_scaled_tok(const char *s, size_t len, at1_scaled *out, flags_t *f)
{
    const char *at = memchr(s, '@', len);
    if (!at) return 0;
    size_t ln = (size_t)(at - s), lk = len - ln - 1;
    int cn, ck; bn n, k; bn_init(&n); bn_init(&k);
    int ok = parse_int_tok(s, ln, &cn, &n, f) && parse_int_tok(at + 1, lk, &ck, &k, f);
    if (!ok) { bn_free(&n); bn_free(&k); return 0; }
    int canonical = cn && ck && bn_sign(&n) >= 0 && bn_sign(&k) >= 0;
    int64_t kv;
    if (!bn_fits_i64(&k, &kv) || kv > 1000) kv = 1000;
    if (kv < 0) kv = 0;
    if (canonical && bn_is_zero(&n) && kv != 0) canonical = 0;
    if (canonical && !bn_is_zero(&n) && kv != 0) {
        bn ten, r; bn_init(&ten); bn_init(&r); bn_set_u64(&ten, 10);
        bn_divmod(NULL, &r, &n, &ten);
        if (bn_is_zero(&r)) canonical = 0;
        bn_free(&ten); bn_free(&r);
    }
    if (!canonical) f->noncanonical = 1;
    if (kv > AT1_SCALED_K_MAX) f->scaled_k_over = 1;
    bn_abs(&out->n, &n); out->k = (unsigned)kv;
    bn_free(&n); bn_free(&k);
    return 1;
}

static int valid_label(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > AT1_LABEL_LEN || !(s[0] >= 'a' && s[0] <= 'z')) return 0;
    for (size_t i = 1; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || is_digit(s[i]) || s[i] == '_')) return 0;
    return 1;
}
static int is_alnum(char c) { return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int valid_name(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > AT1_NAME_LEN || !is_alnum(s[0])) return 0;
    for (size_t i = 1; i < n; i++) if (!(is_alnum(s[i]) || s[i] == '_' || s[i] == '.' || s[i] == '-')) return 0;
    return 1;
}
static int valid_digest(const char *s)
{
    if (strlen(s) != 64) return 0;
    for (int i = 0; i < 64; i++) if (!(is_digit(s[i]) || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

/* split "a,b,c" (no empty items); returns item count or -1 on shape failure; "none" is 0 items */
static int split_list(const char *s, const char ***items, size_t **lens)
{
    *items = NULL; *lens = NULL;
    if (strcmp(s, "none") == 0) return 0;
    size_t n = 1;
    for (const char *p = s; *p; p++) if (*p == ',') n++;
    *items = at1_xmalloc(n * sizeof(char *));
    *lens = at1_xmalloc(n * sizeof(size_t));
    const char *p = s; size_t i = 0;
    for (;;) {
        const char *c = strchr(p, ',');
        size_t l = c ? (size_t)(c - p) : strlen(p);
        if (l == 0) { free(*items); free(*lens); *items = NULL; *lens = NULL; return -1; }
        (*items)[i] = p; (*lens)[i] = l; i++;
        if (!c) break;
        p = c + 1;
    }
    return (int)n;
}

/* ---- raw parse state ----------------------------------------------------------------------- */
typedef struct {
    const char *header_v, *domain_v, *contract_v;
    long clock_dim, label_count;
    int n_energies; bq *energies;
    int n_coupling; bq (*coupling)[3];
    int n_labels; char (*labels)[AT1_LABEL_LEN + 1];
    const char **codes; size_t *code_lens; int n_codes;
    int sem_begin, sem_end, acc_begin, acc_end;    /* line indices */
} raw_t;

static void raw_free(raw_t *r)
{
    if (r->energies) { for (int i = 0; i < r->n_energies; i++) bq_free(&r->energies[i]); free(r->energies); }
    if (r->coupling) { for (int i = 0; i < r->n_coupling; i++) for (int a = 0; a < 3; a++) bq_free(&r->coupling[i][a]); free(r->coupling); }
    free(r->labels); free(r->codes); free(r->code_lens);
}

static void case_init(at1_case *c)
{
    memset(c, 0, sizeof *c);
    for (int j = 0; j < AT1_CLOCK_DIM_MAX; j++) { bq_init(&c->energies[j]); for (int a = 0; a < 3; a++) bq_init(&c->v[j][a]); }
    bq_init(&c->h0); for (int a = 0; a < 3; a++) bq_init(&c->h[a]);
    gq_init(&c->psi0[0]); gq_init(&c->psi0[1]);
    bq_init(&c->tau); bq_init(&c->weight);
    at1_scaled *t[5] = { &c->tol_constraint, &c->tol_povm, &c->tol_probability, &c->tol_zero, &c->tol_schrodinger };
    for (int i = 0; i < 5; i++) { bn_init(&t[i]->n); t[i]->k = 0; }
    c->initialized = 1;
}

void at1_case_free(at1_case *c)
{
    if (!c || !c->initialized) return;
    for (int j = 0; j < AT1_CLOCK_DIM_MAX; j++) { bq_free(&c->energies[j]); for (int a = 0; a < 3; a++) bq_free(&c->v[j][a]); }
    bq_free(&c->h0); for (int a = 0; a < 3; a++) bq_free(&c->h[a]);
    gq_free(&c->psi0[0]); gq_free(&c->psi0[1]);
    bq_free(&c->tau); bq_free(&c->weight);
    at1_scaled *t[5] = { &c->tol_constraint, &c->tol_povm, &c->tol_probability, &c->tol_zero, &c->tol_schrodinger };
    for (int i = 0; i < 5; i++) bn_free(&t[i]->n);
    free(c->semantic_text); free(c->acceptance_text);
    memset(c, 0, sizeof *c);
}

#define SHAPE() do { return AT1_CASE_PARSE_ERROR; } while (0)
#define FIXED(text) do { const char *l_ = take(L); if (!l_ || strcmp(l_, (text)) != 0) SHAPE(); } while (0)
#define KEYED(key, rest) do { rest = key_rest(take(L), (key)); if (!rest) SHAPE(); } while (0)
#define ONE_TOKEN(rest) do { if (strchr((rest), ' ')) SHAPE(); } while (0)

static at1_status parse_shape(lines_t *L, at1_case *c, raw_t *r, flags_t *f)
{
    const char *rest;
    /* version-bearing lines: key and placement here, value in step 2 (reading d) */
    KEYED("OMEGA-AT1-CASE", rest); r->header_v = rest;
    KEYED("domain", rest); r->domain_v = rest;
    KEYED("contract", rest); r->contract_v = rest;
    KEYED("case_name", rest);
    if (!valid_name(rest)) SHAPE();
    strcpy(c->name, rest);
    r->sem_begin = L->pos;
    FIXED("begin semantic");
    FIXED("model_family PAGE_WOOTTERS_FINITE_CLOCKDIAG");
    FIXED("energy_unit DIMENSIONLESS_HBAR_1");
    KEYED("clock_dim", rest); ONE_TOKEN(rest);
    if (!parse_small_int(rest, strlen(rest), &r->clock_dim, f)) SHAPE();
    KEYED("clock_energies", rest); ONE_TOKEN(rest);
    {
        const char **it; size_t *ln;
        int n = split_list(rest, &it, &ln);
        if (n < 0 || n != r->clock_dim) { free(it); free(ln); SHAPE(); }
        r->energies = at1_xmalloc((size_t)(n ? n : 1) * sizeof(bq));
        for (int i = 0; i < n; i++) bq_init(&r->energies[i]);
        r->n_energies = n;
        for (int i = 0; i < n; i++) if (!parse_rat_tok(it[i], ln[i], &r->energies[i], f)) { free(it); free(ln); SHAPE(); }
        free(it); free(ln);
    }
    FIXED("system_dim 2");
    KEYED("system_hamiltonian_pauli", rest); ONE_TOKEN(rest);
    {
        const char **it; size_t *ln;
        int n = split_list(rest, &it, &ln);
        if (n != 4) { free(it); free(ln); SHAPE(); }
        int ok = parse_rat_tok(it[0], ln[0], &c->h0, f) && parse_rat_tok(it[1], ln[1], &c->h[0], f) &&
                 parse_rat_tok(it[2], ln[2], &c->h[1], f) && parse_rat_tok(it[3], ln[3], &c->h[2], f);
        free(it); free(ln);
        if (!ok) SHAPE();
    }
    FIXED("interaction CLOCK_DIAGONAL_PAULI");
    {
        /* repeated block; clock_dim is its count line (AT1_CASE_V1 section 1) */
        int start = L->pos, n = 0;
        while (peek(L) && line_key_is(peek(L), "interaction_pauli")) { L->pos++; n++; }
        if (n != r->clock_dim) SHAPE();
        L->pos = start;
        r->coupling = at1_xmalloc((size_t)(n ? n : 1) * sizeof *r->coupling);
        for (int i = 0; i < n; i++) for (int a = 0; a < 3; a++) bq_init(&r->coupling[i][a]);
        r->n_coupling = n;
        for (int j = 0; j < n; j++) {
            KEYED("interaction_pauli", rest);
            const char *sp = strchr(rest, ' ');
            if (!sp || strchr(sp + 1, ' ')) SHAPE();
            long idx;
            if (!parse_small_int(rest, (size_t)(sp - rest), &idx, f) || idx != j) SHAPE();   /* reading e */
            const char **it; size_t *ln;
            int m = split_list(sp + 1, &it, &ln);
            if (m != 3) { free(it); free(ln); SHAPE(); }
            int ok = 1;
            for (int a = 0; a < 3; a++) ok = ok && parse_rat_tok(it[a], ln[a], &r->coupling[j][a], f);
            free(it); free(ln);
            if (!ok) SHAPE();
        }
    }
    FIXED("constraint SUM_HC_HS_V");
    FIXED("physical_state NULLSPACE_PROJECTION");
    KEYED("reference_clock_label", rest);
    if (!valid_label(rest)) SHAPE();
    strcpy(c->ref_label, rest);
    KEYED("reference_system_state", rest); ONE_TOKEN(rest);
    {
        const char **it; size_t *ln;
        int n = split_list(rest, &it, &ln);
        int ok = n == 2 && parse_crat_tok(it[0], ln[0], &c->psi0[0], f) && parse_crat_tok(it[1], ln[1], &c->psi0[1], f);
        free(it); free(ln);
        if (!ok) SHAPE();
    }
    FIXED("clock_povm COVARIANT_DISCRETE");
    KEYED("povm_tau_turns", rest); ONE_TOKEN(rest);
    if (!parse_rat_tok(rest, strlen(rest), &c->tau, f)) SHAPE();
    KEYED("povm_weight", rest); ONE_TOKEN(rest);
    if (!parse_rat_tok(rest, strlen(rest), &c->weight, f)) SHAPE();
    KEYED("clock_label_count", rest); ONE_TOKEN(rest);
    if (!parse_small_int(rest, strlen(rest), &r->label_count, f)) SHAPE();
    {
        int start = L->pos, n = 0;
        while (peek(L) && line_key_is(peek(L), "clock_label")) { L->pos++; n++; }
        if (n != r->label_count) SHAPE();
        L->pos = start;
        r->labels = at1_xmalloc((size_t)(n ? n : 1) * sizeof *r->labels);
        r->n_labels = n;
        for (int k = 0; k < n; k++) {
            KEYED("clock_label", rest);
            const char *sp = strchr(rest, ' ');
            if (!sp || strchr(sp + 1, ' ')) SHAPE();
            long idx;
            if (!parse_small_int(rest, (size_t)(sp - rest), &idx, f) || idx != k) SHAPE();
            if (!valid_label(sp + 1)) SHAPE();
            strcpy(r->labels[k], sp + 1);
        }
    }
    FIXED("observables PAULI_X,PAULI_Y,PAULI_Z");
    r->sem_end = L->pos;
    FIXED("end semantic");
    r->acc_begin = L->pos;
    FIXED("begin acceptance");
    KEYED("control_kind", rest);
    if (strcmp(rest, "POSITIVE") == 0) c->control_positive = 1;
    else if (strcmp(rest, "NEGATIVE") == 0) c->control_positive = 0;
    else SHAPE();
    KEYED("prediction_target", rest);
    if (strcmp(rest, "IDEAL") == 0) c->target = AT1_TARGET_IDEAL;
    else if (strcmp(rest, "INTERACTING") == 0) c->target = AT1_TARGET_INTERACTING;
    else SHAPE();
    KEYED("expected_outcome", rest);
    if (strcmp(rest, "PASS") == 0) c->expected_outcome = AT1_EXPECT_PASS;
    else if (strcmp(rest, "FAIL") == 0) c->expected_outcome = AT1_EXPECT_FAIL;
    else SHAPE();
    KEYED("expected_failure_codes", rest); ONE_TOKEN(rest);
    r->n_codes = split_list(rest, &r->codes, &r->code_lens);
    if (r->n_codes < 0) SHAPE();
    KEYED("min_bound_kind", rest);
    if (strcmp(rest, "RIGOROUS") == 0) c->min_bound_kind = AT1_BOUND_RIGOROUS;
    else if (strcmp(rest, "ESTIMATED") == 0) c->min_bound_kind = AT1_BOUND_ESTIMATED;
    else if (strcmp(rest, "NONE") == 0) c->min_bound_kind = AT1_BOUND_NONE;
    else SHAPE();
    static const char *tol_keys[5] = { "tol_constraint_residual", "tol_povm_residual", "tol_probability",
                                       "tol_zero_probability", "tol_schrodinger" };
    at1_scaled *tols[5] = { &c->tol_constraint, &c->tol_povm, &c->tol_probability, &c->tol_zero, &c->tol_schrodinger };
    for (int i = 0; i < 5; i++) {
        KEYED(tol_keys[i], rest); ONE_TOKEN(rest);
        if (!parse_scaled_tok(rest, strlen(rest), tols[i], f)) SHAPE();
    }
    r->acc_end = L->pos;
    FIXED("end acceptance");
    KEYED("case_id", rest);
    if (!valid_digest(rest)) SHAPE();
    strcpy(c->case_id, rest);
    KEYED("acceptance_id", rest);
    if (!valid_digest(rest)) SHAPE();
    strcpy(c->acceptance_id, rest);
    FIXED("end");
    if (L->pos != L->count) SHAPE();                    /* trailing lines */
    return AT1_OK;
}

static at1_status check_parameters(at1_case *c, raw_t *r, const flags_t *f)
{
    if (f->over_limit || f->scaled_k_over) return AT1_CASE_INVALID_PARAMETER;
    if (r->clock_dim < AT1_CLOCK_DIM_MIN || r->clock_dim > AT1_CLOCK_DIM_MAX) return AT1_CASE_INVALID_PARAMETER;
    int N = (int)r->clock_dim;
    for (int j = 1; j < N; j++) if (bq_cmp(&r->energies[j - 1], &r->energies[j]) >= 0) return AT1_CASE_INVALID_PARAMETER;
    if (gq_is_zero(&c->psi0[0]) && gq_is_zero(&c->psi0[1])) return AT1_CASE_INVALID_PARAMETER;
    if (bq_sign(&c->weight) <= 0 || bq_sign(&c->tau) <= 0) return AT1_CASE_INVALID_PARAMETER;
    if (r->label_count < 1 || r->label_count > AT1_LABEL_MAX) return AT1_CASE_INVALID_PARAMETER;
    int M = (int)r->label_count;
    c->ref_index = -1;
    for (int k = 0; k < M; k++) {
        for (int l = 0; l < k; l++) if (strcmp(r->labels[k], r->labels[l]) == 0) return AT1_CASE_INVALID_PARAMETER;
        if (strcmp(r->labels[k], c->ref_label) == 0) c->ref_index = k;
    }
    if (c->ref_index < 0) return AT1_CASE_INVALID_PARAMETER;
    c->clock_dim = N; c->label_count = M;
    for (int j = 0; j < N; j++) {
        bq_copy(&c->energies[j], &r->energies[j]);
        for (int a = 0; a < 3; a++) bq_copy(&c->v[j][a], &r->coupling[j][a]);
    }
    for (int k = 0; k < M; k++) strcpy(c->labels[k], r->labels[k]);
    return AT1_OK;
}

/* rule 4: |h + v_j|^2 is the square of a rational for every level, decided exactly */
static at1_status check_spectrum(const at1_case *c)
{
    at1_status st = AT1_OK;
    bq s, t, comp; bn root; bq_init(&s); bq_init(&t); bq_init(&comp); bn_init(&root);
    for (int j = 0; j < c->clock_dim && st == AT1_OK; j++) {
        bq_set_i64(&s, 0, 1);
        for (int a = 0; a < 3; a++) {
            bq_add(&comp, &c->h[a], &c->v[j][a]);
            bq_mul(&t, &comp, &comp);
            bq_add(&s, &s, &t);
        }
        /* s is reduced with den > 0, so it is a rational square iff num and den are squares */
        if (!bn_isqrt_exact(&s.num, &root) || !bn_isqrt_exact(&s.den, &root)) st = AT1_CASE_IRRATIONAL_SPECTRUM;
    }
    bq_free(&s); bq_free(&t); bq_free(&comp); bn_free(&root);
    return st;
}

static at1_status check_acceptance(at1_case *c, const raw_t *r)
{
    c->expected_codes = 0;
    if (c->expected_outcome == AT1_EXPECT_PASS) return r->n_codes == 0 ? AT1_OK : AT1_CASE_INVALID_PARAMETER;
    if (r->n_codes == 0) return AT1_CASE_INVALID_PARAMETER;
    int prev = -1;
    for (int i = 0; i < r->n_codes; i++) {
        int idx = -1;
        for (int j = 0; j < AT1_FAILURE_CODE_COUNT; j++)
            if (strlen(at1_failure_codes[j]) == r->code_lens[i] && memcmp(r->codes[i], at1_failure_codes[j], r->code_lens[i]) == 0) { idx = j; break; }
        if (idx < 0 || idx <= prev) return AT1_CASE_INVALID_PARAMETER;   /* unknown, duplicate or unsorted */
        c->expected_codes |= 1u << idx; prev = idx;
    }
    return AT1_OK;
}

at1_status at1_case_parse(const uint8_t *bytes, size_t len, at1_case *c)
{
    lines_t L; raw_t r; flags_t f;
    memset(&r, 0, sizeof r); memset(&f, 0, sizeof f);
    case_init(c);
    at1_status st;
    if (len > AT1_FILE_MAX) { st = AT1_ERR_RESOURCE; L.line = NULL; L.off = NULL; L.copy = NULL; goto done; }
    st = split_lines(bytes, len, &L);
    if (st == AT1_OK) st = parse_shape(&L, c, &r, &f);
    if (st == AT1_OK && f.resource) st = AT1_ERR_RESOURCE;
    if (st == AT1_OK && f.noncanonical) st = AT1_CASE_NONCANONICAL;       /* still step 1 */
    if (st == AT1_OK) {                                                   /* step 2 */
        if (strcmp(r.header_v, "v1") != 0 || strcmp(r.domain_v, "omega.at1.case.v1") != 0 ||
            strcmp(r.contract_v, "AT1_CASE_V1") != 0) st = AT1_CASE_UNSUPPORTED_VERSION;
    }
    if (st == AT1_OK) st = check_parameters(c, &r, &f);                    /* step 3 */
    if (st == AT1_OK) st = check_spectrum(c);                              /* step 4 */
    if (st == AT1_OK) {                                                   /* step 5 */
        size_t s0 = L.off[r.sem_begin], s1 = L.off[r.sem_end + 1];
        size_t a0 = L.off[r.acc_begin], a1 = L.off[r.acc_end + 1];
        char cid[65], aid[65];
        at1_tagged_sha256("omega.at1.case.v1", (const char *)bytes + s0, s1 - s0, cid);
        at1_tagged_sha256("omega.at1.acceptance.v1", (const char *)bytes + a0, a1 - a0, aid);
        if (strcmp(cid, c->case_id) != 0 || strcmp(aid, c->acceptance_id) != 0) st = AT1_CASE_ID_MISMATCH;
        else {
            c->semantic_len = s1 - s0; c->semantic_text = at1_xmalloc(c->semantic_len + 1);
            memcpy(c->semantic_text, bytes + s0, c->semantic_len); c->semantic_text[c->semantic_len] = 0;
            c->acceptance_len = a1 - a0; c->acceptance_text = at1_xmalloc(c->acceptance_len + 1);
            memcpy(c->acceptance_text, bytes + a0, c->acceptance_len); c->acceptance_text[c->acceptance_len] = 0;
        }
    }
    if (st == AT1_OK) st = check_acceptance(c, &r);                        /* step 6 */
    if (st == AT1_OK) at1_sha256_hex(bytes, len, c->case_file_sha256);
done:
    raw_free(&r);
    lines_free(&L);
    if (st != AT1_OK) at1_case_free(c);
    return st;
}
