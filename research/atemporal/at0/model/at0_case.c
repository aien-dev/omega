/* AT0_CASE_V1 codec: parse, validate (rules 1..6 in contract order), compute
 * identities, emit canonical bytes. The only shared omega source is sha256. */
#include "at0_model.h"
#include "sha256.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

const char *const at0_failure_codes[AT0_FAILURE_CODE_COUNT] = {
    "BOUND_KIND_INSUFFICIENT", "CONDITIONAL_UNDEFINED", "CONSTRAINT_RESIDUAL_EXCEEDED",
    "NONFINITE_VALUE", "POVM_NORMALIZATION_EXCEEDED", "PRECISION_INSUFFICIENT",
    "PROBABILITY_OUT_OF_RANGE", "PROBABILITY_SUM_EXCEEDED", "SCHRODINGER_DEVIATION_EXCEEDED",
    "TRIVIAL_PHYSICAL_STATE"
};

#define LINE_CAP 4096

typedef struct {
    const char *line[LINE_CAP];
    size_t len[LINE_CAP];
    int count;
    int pos;
} lines_t;

/* rule 1: byte shape. Fills line table pointing into a NUL-free copy. */
static at0_status split_lines(const uint8_t *bytes, size_t len, char *copy, lines_t *L)
{
    if (len == 0 || bytes[len - 1] != '\n') return AT0_CASE_PARSE_ERROR;
    memcpy(copy, bytes, len);
    L->count = 0; L->pos = 0;
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = bytes[i];
        if (b == '\n') {
            size_t n = i - start;
            if (n == 0) return AT0_CASE_PARSE_ERROR;                 /* blank line */
            if (copy[start] == ' ' || copy[i - 1] == ' ') return AT0_CASE_PARSE_ERROR;
            for (size_t j = start + 1; j < i; j++)
                if (copy[j] == ' ' && copy[j - 1] == ' ') return AT0_CASE_PARSE_ERROR;
            if (L->count >= LINE_CAP) return AT0_CASE_PARSE_ERROR;
            copy[i] = 0;
            L->line[L->count] = copy + start; L->len[L->count] = n; L->count++;
            start = i + 1;
        } else if (b < 0x20 || b > 0x7E) {
            return AT0_CASE_PARSE_ERROR;
        }
    }
    return AT0_OK;
}

static const char *next_line(lines_t *L) { return L->pos < L->count ? L->line[L->pos++] : NULL; }

/* "<key> <rest>": returns rest or NULL if key does not match */
static const char *key_rest(const char *line, const char *key)
{
    size_t k = strlen(key);
    if (!line || strncmp(line, key, k) != 0 || line[k] != ' ') return NULL;
    return line + k + 1;
}

static int valid_label(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > AT0_LABEL_LEN || !(s[0] >= 'a' && s[0] <= 'z')) return 0;
    for (size_t i = 1; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return 0;
    return 1;
}
static int valid_name(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > AT0_NAME_LEN || !isalnum((unsigned char)s[0])) return 0;
    for (size_t i = 1; i < n; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '.' || s[i] == '-')) return 0;
    return 1;
}
static int valid_digest(const char *s)
{
    if (strlen(s) != AT0_DIGEST_HEX) return 0;
    for (int i = 0; i < AT0_DIGEST_HEX; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}
/* canonical integer token for counts and indices (same tri-state as the rational tokens:
 * 1 canonical, 0 shape error, -1 noncanonical such as "04" or "-0"), bounded so counts cannot
 * overflow; a negative value is left to rule 3. */
static int parse_small_int(const char *s, long *out)
{
    size_t n = strlen(s), i = 0; int neg = 0;
    if (n > 0 && s[0] == '-') { neg = 1; i = 1; }
    if (n == i || n - i > 9) return 0;
    for (size_t j = i; j < n; j++) if (!isdigit((unsigned char)s[j])) return 0;
    int canonical = !(s[i] == '0' && (n - i > 1 || neg));
    long v = 0;
    for (; i < n; i++) v = v * 10 + (s[i] - '0');
    *out = neg ? -v : v;
    return canonical ? 1 : -1;
}

/* split a comma list in place (no spaces, no empty items, no trailing comma) */
static int split_list(char *s, char **items, int cap, int *count)
{
    *count = 0;
    if (strcmp(s, "none") == 0) return 1;
    if (*s == 0) return 0;
    char *p = s;
    for (;;) {
        char *c = strchr(p, ',');
        if (c) *c = 0;
        if (*p == 0) return 0;
        if (*count < cap) items[*count] = p;
        (*count)++;
        if (!c) break;
        p = c + 1;
    }
    return 1;
}

typedef struct {
    /* raw values that are only judged after the shape pass */
    char header[64], domain[64], contract[64];
    char model_family[64], energy_unit[64], system_dim[16], interaction[32], constraint[32],
         physical_state[48], clock_povm[48], observables[64];
    long clock_dim, label_count;
    int energies_seen;
    int expected_codes_raw_count;
    char expected_codes_raw[AT0_FAILURE_CODE_COUNT + 1][48];
    int expected_codes_overflow;
    int noncanonical;               /* a well-formed but noncanonical token was seen (rule 2, judged after the shape pass) */
} raw_t;

static at0_status copy_tok(const char *src, char *dst, size_t cap)
{
    if (!src || strlen(src) >= cap) return AT0_CASE_PARSE_ERROR;
    strcpy(dst, src); return AT0_OK;
}

/* token parse: a shape error (rule 1) returns at once; a noncanonical token (rule 2) is recorded
 * and judged after the whole shape pass, so rule 1 anywhere in the file wins over rule 2 */
#define NEED(expr) do { at0_status st_ = (expr); if (st_ == AT0_CASE_NONCANONICAL) r->noncanonical = 1; else if (st_ != AT0_OK) return st_; } while (0)
#define SMALL_INT(tok, out) do { int c_ = parse_small_int((tok), (out)); if (c_ == 0) return AT0_CASE_PARSE_ERROR; if (c_ < 0) r->noncanonical = 1; } while (0)
#define FIXED(L, text) do { const char *l_ = next_line(L); if (!l_ || strcmp(l_, text) != 0) return AT0_CASE_PARSE_ERROR; } while (0)
#define KEYED(L, key, rest) do { rest = key_rest(next_line(L), key); if (!rest) return AT0_CASE_PARSE_ERROR; } while (0)

/* rule 1 (shape + token canonicality) for the whole file */
static at0_status parse_shape(lines_t *L, at0_case *c, raw_t *r)
{
    const char *rest;
    char tmp[AT0_LINE_MAX];
    memset(c, 0, sizeof *c); memset(r, 0, sizeof *r);

    KEYED(L, "OMEGA-AT0-CASE", rest); NEED(copy_tok(rest, r->header, sizeof r->header));
    KEYED(L, "domain", rest);         NEED(copy_tok(rest, r->domain, sizeof r->domain));
    KEYED(L, "contract", rest);       NEED(copy_tok(rest, r->contract, sizeof r->contract));
    KEYED(L, "case_name", rest);
    if (!valid_name(rest)) return AT0_CASE_PARSE_ERROR;
    strcpy(c->name, rest);
    FIXED(L, "begin semantic");
    KEYED(L, "model_family", rest);   NEED(copy_tok(rest, r->model_family, sizeof r->model_family));
    KEYED(L, "energy_unit", rest);    NEED(copy_tok(rest, r->energy_unit, sizeof r->energy_unit));
    KEYED(L, "clock_dim", rest);      SMALL_INT(rest, &r->clock_dim);
    KEYED(L, "clock_energies", rest);
    {
        if (strlen(rest) >= sizeof tmp) return AT0_CASE_PARSE_ERROR;
        strcpy(tmp, rest);
        char *items[AT0_CLOCK_DIM_MAX]; int n;
        if (!split_list(tmp, items, AT0_CLOCK_DIM_MAX, &n) || n == 0) return AT0_CASE_PARSE_ERROR;
        if (n != r->clock_dim) return AT0_CASE_PARSE_ERROR;
        for (int i = 0; i < n && i < AT0_CLOCK_DIM_MAX; i++) NEED(at0_rat_parse(items[i], &c->clock_energies[i]));
        r->energies_seen = n;
    }
    KEYED(L, "system_dim", rest);     NEED(copy_tok(rest, r->system_dim, sizeof r->system_dim));
    KEYED(L, "system_hamiltonian_pauli", rest);
    {
        if (strlen(rest) >= sizeof tmp) return AT0_CASE_PARSE_ERROR;
        strcpy(tmp, rest);
        char *items[4]; int n;
        if (!split_list(tmp, items, 4, &n) || n != 4) return AT0_CASE_PARSE_ERROR;
        NEED(at0_rat_parse(items[0], &c->h0)); NEED(at0_rat_parse(items[1], &c->hx));
        NEED(at0_rat_parse(items[2], &c->hy)); NEED(at0_rat_parse(items[3], &c->hz));
    }
    KEYED(L, "interaction", rest);    NEED(copy_tok(rest, r->interaction, sizeof r->interaction));
    KEYED(L, "constraint", rest);     NEED(copy_tok(rest, r->constraint, sizeof r->constraint));
    KEYED(L, "physical_state", rest); NEED(copy_tok(rest, r->physical_state, sizeof r->physical_state));
    KEYED(L, "reference_clock_label", rest);
    if (!valid_label(rest)) return AT0_CASE_PARSE_ERROR;
    strcpy(c->reference_clock_label, rest);
    KEYED(L, "reference_system_state", rest);
    {
        if (strlen(rest) >= sizeof tmp) return AT0_CASE_PARSE_ERROR;
        strcpy(tmp, rest);
        char *items[2]; int n;
        if (!split_list(tmp, items, 2, &n) || n != 2) return AT0_CASE_PARSE_ERROR;
        NEED(at0_crat_parse(items[0], &c->psi0[0])); NEED(at0_crat_parse(items[1], &c->psi0[1]));
    }
    KEYED(L, "clock_povm", rest);     NEED(copy_tok(rest, r->clock_povm, sizeof r->clock_povm));
    KEYED(L, "povm_tau_turns", rest); NEED(at0_rat_parse(rest, &c->povm_tau_turns));
    KEYED(L, "povm_weight", rest);    NEED(at0_rat_parse(rest, &c->povm_weight));
    KEYED(L, "clock_label_count", rest); SMALL_INT(rest, &r->label_count);
    for (long k = 0; k < r->label_count; k++) {
        KEYED(L, "clock_label", rest);
        if (strlen(rest) >= sizeof tmp) return AT0_CASE_PARSE_ERROR;
        strcpy(tmp, rest);
        char *sp = strchr(tmp, ' ');
        if (!sp) return AT0_CASE_PARSE_ERROR;
        *sp = 0;
        long idx;
        SMALL_INT(tmp, &idx);
        if (idx != k) return AT0_CASE_PARSE_ERROR;
        if (!valid_label(sp + 1)) return AT0_CASE_PARSE_ERROR;
        if (k < AT0_LABEL_MAX) strcpy(c->labels[k], sp + 1);
    }
    KEYED(L, "observables", rest);    NEED(copy_tok(rest, r->observables, sizeof r->observables));
    FIXED(L, "end semantic");
    FIXED(L, "begin acceptance");
    KEYED(L, "control_kind", rest);
    if (strcmp(rest, "POSITIVE") == 0) c->control_kind = AT0_CTRL_POSITIVE;
    else if (strcmp(rest, "NEGATIVE") == 0) c->control_kind = AT0_CTRL_NEGATIVE;
    else return AT0_CASE_PARSE_ERROR;
    KEYED(L, "expected_outcome", rest);
    if (strcmp(rest, "PASS") == 0) c->expected_outcome = AT0_EXPECT_PASS;
    else if (strcmp(rest, "FAIL") == 0) c->expected_outcome = AT0_EXPECT_FAIL;
    else return AT0_CASE_PARSE_ERROR;
    KEYED(L, "expected_failure_codes", rest);
    {
        if (strlen(rest) >= sizeof tmp) return AT0_CASE_PARSE_ERROR;
        strcpy(tmp, rest);
        char *items[AT0_FAILURE_CODE_COUNT + 1]; int n;
        if (!split_list(tmp, items, AT0_FAILURE_CODE_COUNT + 1, &n)) return AT0_CASE_PARSE_ERROR;
        r->expected_codes_raw_count = n;
        if (n > AT0_FAILURE_CODE_COUNT) r->expected_codes_overflow = 1;
        for (int i = 0; i < n && i <= AT0_FAILURE_CODE_COUNT; i++)
            NEED(copy_tok(items[i], r->expected_codes_raw[i], sizeof r->expected_codes_raw[i]));
    }
    KEYED(L, "min_bound_kind", rest);
    if (strcmp(rest, "RIGOROUS") == 0) c->min_bound_kind = AT0_BOUND_RIGOROUS;
    else if (strcmp(rest, "ESTIMATED") == 0) c->min_bound_kind = AT0_BOUND_ESTIMATED;
    else if (strcmp(rest, "NONE") == 0) c->min_bound_kind = AT0_BOUND_NONE;
    else return AT0_CASE_PARSE_ERROR;
    KEYED(L, "tol_constraint_residual", rest); NEED(at0_scaled_parse(rest, &c->tol_constraint_residual));
    KEYED(L, "tol_povm_residual", rest);       NEED(at0_scaled_parse(rest, &c->tol_povm_residual));
    KEYED(L, "tol_probability", rest);         NEED(at0_scaled_parse(rest, &c->tol_probability));
    KEYED(L, "tol_zero_probability", rest);    NEED(at0_scaled_parse(rest, &c->tol_zero_probability));
    KEYED(L, "tol_schrodinger", rest);         NEED(at0_scaled_parse(rest, &c->tol_schrodinger));
    FIXED(L, "end acceptance");
    KEYED(L, "case_id", rest);
    if (!valid_digest(rest)) return AT0_CASE_PARSE_ERROR;
    strcpy(c->case_id, rest);
    KEYED(L, "acceptance_id", rest);
    if (!valid_digest(rest)) return AT0_CASE_PARSE_ERROR;
    strcpy(c->acceptance_id, rest);
    FIXED(L, "end");
    if (L->pos != L->count) return AT0_CASE_PARSE_ERROR;      /* trailing lines */
    return r->noncanonical ? AT0_CASE_NONCANONICAL : AT0_OK;  /* rule 2, after the whole shape pass */
}

/* rule 3: ranges and fixed values */
static at0_status check_parameters(at0_case *c, const raw_t *r)
{
    /* contract limits on written tokens (AT0_CASE_V1 section 2), judged here so that rules 1 and 2 come first */
    for (int j = 0; j < r->energies_seen && j < AT0_CLOCK_DIM_MAX; j++)
        if (!at0_rat_in_limits(c->clock_energies[j])) return AT0_CASE_INVALID_PARAMETER;
    if (!at0_rat_in_limits(c->h0) || !at0_rat_in_limits(c->hx) || !at0_rat_in_limits(c->hy) || !at0_rat_in_limits(c->hz)) return AT0_CASE_INVALID_PARAMETER;
    for (int s = 0; s < AT0_SYSTEM_DIM; s++)
        if (!at0_rat_in_limits(c->psi0[s].re) || !at0_rat_in_limits(c->psi0[s].im)) return AT0_CASE_INVALID_PARAMETER;
    if (!at0_rat_in_limits(c->povm_tau_turns) || !at0_rat_in_limits(c->povm_weight)) return AT0_CASE_INVALID_PARAMETER;
    if (!at0_scaled_in_limits(c->tol_constraint_residual) || !at0_scaled_in_limits(c->tol_povm_residual) ||
        !at0_scaled_in_limits(c->tol_probability) || !at0_scaled_in_limits(c->tol_zero_probability) ||
        !at0_scaled_in_limits(c->tol_schrodinger)) return AT0_CASE_INVALID_PARAMETER;
    if (strcmp(r->model_family, "PAGE_WOOTTERS_FINITE_IDEAL") != 0) return AT0_CASE_INVALID_PARAMETER;
    if (strcmp(r->energy_unit, "DIMENSIONLESS_HBAR_1") != 0) return AT0_CASE_INVALID_PARAMETER;
    if (strcmp(r->system_dim, "2") != 0) return AT0_CASE_INVALID_PARAMETER;
    if (strcmp(r->interaction, "NONE") != 0) return AT0_CASE_INVALID_PARAMETER;
    if (strcmp(r->constraint, "SUM_HC_HS") != 0) return AT0_CASE_INVALID_PARAMETER;
    if (strcmp(r->physical_state, "NULLSPACE_PROJECTION") != 0) return AT0_CASE_INVALID_PARAMETER;
    if (strcmp(r->clock_povm, "COVARIANT_DISCRETE") != 0) return AT0_CASE_INVALID_PARAMETER;
    if (strcmp(r->observables, "PAULI_X,PAULI_Y,PAULI_Z") != 0) return AT0_CASE_INVALID_PARAMETER;
    if (r->clock_dim < AT0_CLOCK_DIM_MIN || r->clock_dim > AT0_CLOCK_DIM_MAX) return AT0_CASE_INVALID_PARAMETER;
    c->clock_dim = (int)r->clock_dim;
    for (int j = 1; j < c->clock_dim; j++)
        if (at0_rat_cmp(c->clock_energies[j - 1], c->clock_energies[j]) >= 0) return AT0_CASE_INVALID_PARAMETER;
    if (at0_rat_is_zero(c->psi0[0].re) && at0_rat_is_zero(c->psi0[0].im) &&
        at0_rat_is_zero(c->psi0[1].re) && at0_rat_is_zero(c->psi0[1].im)) return AT0_CASE_INVALID_PARAMETER;
    if (c->povm_weight.n <= 0) return AT0_CASE_INVALID_PARAMETER;
    if (c->povm_tau_turns.n <= 0) return AT0_CASE_INVALID_PARAMETER;
    if (r->label_count < 1 || r->label_count > AT0_LABEL_MAX) return AT0_CASE_INVALID_PARAMETER;
    c->label_count = (int)r->label_count;
    c->reference_index = -1;
    for (int k = 0; k < c->label_count; k++) {
        for (int l = 0; l < k; l++)
            if (strcmp(c->labels[k], c->labels[l]) == 0) return AT0_CASE_INVALID_PARAMETER;
        if (strcmp(c->labels[k], c->reference_clock_label) == 0) c->reference_index = k;
    }
    if (c->reference_index < 0) return AT0_CASE_INVALID_PARAMETER;
    return AT0_OK;
}

/* rule 4: |h|^2 must be the square of a rational */
static at0_status check_spectrum(at0_case *c)
{
    /* |h|^2 is formed from unreduced 128-bit integers: the reduced rational |h|^2 can have a
     * denominator up to 2^120 for in-limit tokens with distinct denominators. */
    int sq;
    at0_status st = at0_rat_norm_exact(c->hx, c->hy, c->hz, &c->h_norm, &sq);
    if (st != AT0_OK) return st;
    if (!sq) return AT0_CASE_IRRATIONAL_SPECTRUM;
    return AT0_OK;
}

/* rule 6: expected failure code list */
static at0_status check_acceptance(at0_case *c, const raw_t *r)
{
    c->expected_failure_count = 0;
    if (c->expected_outcome == AT0_EXPECT_PASS) {
        return r->expected_codes_raw_count == 0 ? AT0_OK : AT0_CASE_INVALID_PARAMETER;
    }
    if (r->expected_codes_raw_count == 0 || r->expected_codes_overflow) return AT0_CASE_INVALID_PARAMETER;
    int prev = -1;
    for (int i = 0; i < r->expected_codes_raw_count; i++) {
        int idx = -1;
        for (int j = 0; j < AT0_FAILURE_CODE_COUNT; j++)
            if (strcmp(r->expected_codes_raw[i], at0_failure_codes[j]) == 0) { idx = j; break; }
        if (idx < 0 || idx <= prev) return AT0_CASE_INVALID_PARAMETER;   /* unknown, duplicate or unsorted */
        c->expected_failure_idx[i] = idx; prev = idx;
    }
    c->expected_failure_count = r->expected_codes_raw_count;
    return AT0_OK;
}

/* ---- emission ------------------------------------------------------------ */
typedef struct { char *buf; size_t cap; size_t len; int overflow; } out_t;

static void put(out_t *o, const char *s)
{
    size_t n = strlen(s);
    if (o->len + n >= o->cap) { o->overflow = 1; return; }
    memcpy(o->buf + o->len, s, n); o->len += n; o->buf[o->len] = 0;
}
static void putf_rat(out_t *o, at0_rat r) { char t[64]; at0_rat_format(r, t, sizeof t); put(o, t); }

static void emit_semantic(const at0_case *c, out_t *o)
{
    char t[128];
    put(o, "begin semantic\n");
    put(o, "model_family PAGE_WOOTTERS_FINITE_IDEAL\n");
    put(o, "energy_unit DIMENSIONLESS_HBAR_1\n");
    snprintf(t, sizeof t, "clock_dim %d\n", c->clock_dim); put(o, t);
    put(o, "clock_energies ");
    for (int j = 0; j < c->clock_dim; j++) { if (j) put(o, ","); putf_rat(o, c->clock_energies[j]); }
    put(o, "\n");
    put(o, "system_dim 2\n");
    put(o, "system_hamiltonian_pauli ");
    putf_rat(o, c->h0); put(o, ","); putf_rat(o, c->hx); put(o, ","); putf_rat(o, c->hy); put(o, ","); putf_rat(o, c->hz);
    put(o, "\n");
    put(o, "interaction NONE\n");
    put(o, "constraint SUM_HC_HS\n");
    put(o, "physical_state NULLSPACE_PROJECTION\n");
    snprintf(t, sizeof t, "reference_clock_label %s\n", c->reference_clock_label); put(o, t);
    put(o, "reference_system_state ");
    at0_crat_format(c->psi0[0], t, sizeof t); put(o, t); put(o, ",");
    at0_crat_format(c->psi0[1], t, sizeof t); put(o, t); put(o, "\n");
    put(o, "clock_povm COVARIANT_DISCRETE\n");
    put(o, "povm_tau_turns "); putf_rat(o, c->povm_tau_turns); put(o, "\n");
    put(o, "povm_weight "); putf_rat(o, c->povm_weight); put(o, "\n");
    snprintf(t, sizeof t, "clock_label_count %d\n", c->label_count); put(o, t);
    for (int k = 0; k < c->label_count; k++) { snprintf(t, sizeof t, "clock_label %d %s\n", k, c->labels[k]); put(o, t); }
    put(o, "observables PAULI_X,PAULI_Y,PAULI_Z\n");
    put(o, "end semantic\n");
}

static void emit_acceptance(const at0_case *c, out_t *o)
{
    char t[128];
    put(o, "begin acceptance\n");
    put(o, c->control_kind == AT0_CTRL_POSITIVE ? "control_kind POSITIVE\n" : "control_kind NEGATIVE\n");
    put(o, c->expected_outcome == AT0_EXPECT_PASS ? "expected_outcome PASS\n" : "expected_outcome FAIL\n");
    put(o, "expected_failure_codes ");
    if (c->expected_failure_count == 0) put(o, "none");
    for (int i = 0; i < c->expected_failure_count; i++) { if (i) put(o, ","); put(o, at0_failure_codes[c->expected_failure_idx[i]]); }
    put(o, "\n");
    put(o, c->min_bound_kind == AT0_BOUND_RIGOROUS ? "min_bound_kind RIGOROUS\n"
         : c->min_bound_kind == AT0_BOUND_ESTIMATED ? "min_bound_kind ESTIMATED\n" : "min_bound_kind NONE\n");
    at0_scaled_format(c->tol_constraint_residual, t, sizeof t); put(o, "tol_constraint_residual "); put(o, t); put(o, "\n");
    at0_scaled_format(c->tol_povm_residual, t, sizeof t);       put(o, "tol_povm_residual "); put(o, t); put(o, "\n");
    at0_scaled_format(c->tol_probability, t, sizeof t);         put(o, "tol_probability "); put(o, t); put(o, "\n");
    at0_scaled_format(c->tol_zero_probability, t, sizeof t);    put(o, "tol_zero_probability "); put(o, t); put(o, "\n");
    at0_scaled_format(c->tol_schrodinger, t, sizeof t);         put(o, "tol_schrodinger "); put(o, t); put(o, "\n");
    put(o, "end acceptance\n");
}

long at0_case_emit(const at0_case *c, char *buf, size_t cap)
{
    if (!c || !buf || cap == 0) return -1;
    out_t o = { buf, cap, 0, 0 };
    char t[512];
    buf[0] = 0;
    put(&o, "OMEGA-AT0-CASE v1\ndomain omega.at0.case.v1\ncontract AT0_CASE_V1\n");
    snprintf(t, sizeof t, "case_name %s\n", c->name); put(&o, t);
    emit_semantic(c, &o);
    emit_acceptance(c, &o);
    snprintf(t, sizeof t, "case_id %s\nacceptance_id %s\nend\n", c->case_id, c->acceptance_id); put(&o, t);
    return o.overflow ? -1 : (long)o.len;
}

static void hex_digest(const uint8_t d[32], char out[AT0_DIGEST_HEX + 1])
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = hx[d[i] >> 4]; out[2 * i + 1] = hx[d[i] & 15]; }
    out[AT0_DIGEST_HEX] = 0;
}

static void tagged_digest(const char *tag, const char *text, size_t len, char out[AT0_DIGEST_HEX + 1])
{
    sha256_ctx ctx; uint8_t d[32]; uint8_t zero = 0;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)tag, strlen(tag));
    sha256_update(&ctx, &zero, 1);
    sha256_update(&ctx, (const uint8_t *)text, len);
    sha256_final(&ctx, d);
    hex_digest(d, out);
}

at0_status at0_case_identities(const at0_case *c, char case_id[AT0_DIGEST_HEX + 1],
                               char acceptance_id[AT0_DIGEST_HEX + 1])
{
    if (!c || !case_id || !acceptance_id) return AT0_ERR_ARGUMENT;
    char *buf = malloc(AT0_LINE_MAX * 4);
    if (!buf) return AT0_ERR_INTERNAL;
    out_t o = { buf, AT0_LINE_MAX * 4, 0, 0 };
    buf[0] = 0;
    emit_semantic(c, &o);
    if (o.overflow) { free(buf); return AT0_ERR_INTERNAL; }
    tagged_digest("omega.at0.case.v1", buf, o.len, case_id);
    o.len = 0; buf[0] = 0;
    emit_acceptance(c, &o);
    if (o.overflow) { free(buf); return AT0_ERR_INTERNAL; }
    tagged_digest("omega.at0.acceptance.v1", buf, o.len, acceptance_id);
    free(buf);
    return AT0_OK;
}

at0_status at0_case_parse(const uint8_t *bytes, size_t len, at0_case *out)
{
    if (!bytes || !out) return AT0_ERR_ARGUMENT;
    if (len > (size_t)LINE_CAP * 256) return AT0_CASE_PARSE_ERROR;
    char *copy = malloc(len + 1);
    lines_t *L = malloc(sizeof *L);
    raw_t *r = malloc(sizeof *r);
    char *re = malloc(len + 1);
    at0_status st = AT0_ERR_INTERNAL;
    if (!copy || !L || !r || !re) goto done;

    /* rule 1 */
    if ((st = split_lines(bytes, len, copy, L)) != AT0_OK) goto done;
    if ((st = parse_shape(L, out, r)) != AT0_OK) goto done;
    /* rule 2 */
    if (strcmp(r->header, "v1") != 0 || strcmp(r->domain, "omega.at0.case.v1") != 0 ||
        strcmp(r->contract, "AT0_CASE_V1") != 0) { st = AT0_CASE_UNSUPPORTED_VERSION; goto done; }
    /* rule 3 */
    if ((st = check_parameters(out, r)) != AT0_OK) goto done;
    /* canonical-form guard: the parsed case must re-emit byte for byte */
    {
        /* the emitted text uses parsed codes; copy raw codes so the guard sees the file's list */
        at0_case tmp = *out;
        tmp.expected_failure_count = 0;
        int ok = 1;
        if (r->expected_codes_raw_count > 0 && !r->expected_codes_overflow) {
            for (int i = 0; i < r->expected_codes_raw_count; i++) {
                int idx = -1;
                for (int j = 0; j < AT0_FAILURE_CODE_COUNT; j++)
                    if (strcmp(r->expected_codes_raw[i], at0_failure_codes[j]) == 0) idx = j;
                if (idx < 0) { ok = 0; break; }
                tmp.expected_failure_idx[i] = idx;
            }
            tmp.expected_failure_count = r->expected_codes_raw_count;
        }
        if (ok && !r->expected_codes_overflow) {
            long n = at0_case_emit(&tmp, re, len + 1);
            if (n != (long)len || memcmp(re, bytes, len) != 0) { st = AT0_CASE_NONCANONICAL; goto done; }
        }
    }
    /* rule 4 */
    if ((st = check_spectrum(out)) != AT0_OK) goto done;
    /* rule 5 */
    {
        char cid[AT0_DIGEST_HEX + 1], aid[AT0_DIGEST_HEX + 1];
        at0_case tmp = *out;
        /* acceptance text must be hashed exactly as written; raw codes are canonical only if rule 6 passes,
         * so hash the file's own acceptance lines instead of re-emitting them */
        (void)tmp;
        size_t a0 = 0, a1 = 0, s0 = 0, s1 = 0;
        for (int i = 0; i < L->count; i++) {
            if (strcmp(L->line[i], "begin semantic") == 0) s0 = (size_t)(L->line[i] - copy);
            if (strcmp(L->line[i], "end semantic") == 0) s1 = (size_t)(L->line[i] - copy) + L->len[i] + 1;
            if (strcmp(L->line[i], "begin acceptance") == 0) a0 = (size_t)(L->line[i] - copy);
            if (strcmp(L->line[i], "end acceptance") == 0) a1 = (size_t)(L->line[i] - copy) + L->len[i] + 1;
        }
        tagged_digest("omega.at0.case.v1", (const char *)bytes + s0, s1 - s0, cid);
        tagged_digest("omega.at0.acceptance.v1", (const char *)bytes + a0, a1 - a0, aid);
        if (strcmp(cid, out->case_id) != 0 || strcmp(aid, out->acceptance_id) != 0) { st = AT0_CASE_ID_MISMATCH; goto done; }
    }
    /* rule 6 */
    if ((st = check_acceptance(out, r)) != AT0_OK) goto done;
    {
        sha256_ctx ctx; uint8_t d[32];
        sha256_init(&ctx); sha256_update(&ctx, bytes, len); sha256_final(&ctx, d);
        hex_digest(d, out->case_file_sha256);
    }
    st = AT0_OK;
done:
    free(copy); free(L); free(r); free(re);
    return st;
}

