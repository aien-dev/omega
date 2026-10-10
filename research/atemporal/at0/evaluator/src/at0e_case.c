/* Independent AT0_CASE_V1 codec: parse, validate (section 4 order), identities
 * (section 5), canonical emission. Written from the contract text only. */
#include "at0e.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const failure_codes_v1[] = {
    "BOUND_KIND_INSUFFICIENT", "CONDITIONAL_UNDEFINED", "CONSTRAINT_RESIDUAL_EXCEEDED",
    "NONFINITE_VALUE", "POVM_NORMALIZATION_EXCEEDED", "PRECISION_INSUFFICIENT",
    "PROBABILITY_OUT_OF_RANGE", "PROBABILITY_SUM_EXCEEDED", "SCHRODINGER_DEVIATION_EXCEEDED",
    "TRIVIAL_PHYSICAL_STATE", NULL };
int failure_code_valid(const char *code) { for (int i = 0; failure_codes_v1[i]; i++) if (!strcmp(code, failure_codes_v1[i])) return 1; return 0; }

const char *refusal_name(refusal r) {
    switch (r) {
    case REFUSE_NONE: return "NONE";
    case REFUSE_PARSE_ERROR: return "CASE_PARSE_ERROR";
    case REFUSE_NONCANONICAL: return "CASE_NONCANONICAL";
    case REFUSE_UNSUPPORTED_VERSION: return "CASE_UNSUPPORTED_VERSION";
    case REFUSE_INVALID_PARAMETER: return "CASE_INVALID_PARAMETER";
    case REFUSE_IRRATIONAL_SPECTRUM: return "CASE_IRRATIONAL_SPECTRUM";
    case REFUSE_ID_MISMATCH: return "CASE_ID_MISMATCH";
    }
    return "?";
}

/* Parsing state. Shape problems are fatal at once (rule 1 comes first); value
 * problems are recorded in order of the rules and reported after the shape pass. */
typedef struct {
    char **lines; size_t n, i;
    const char *detail;
    int noncanonical; const char *noncanon_detail;
    int bad_version; const char *version_detail;
    int bad_param; const char *param_detail;
} ps;

static int expect_key(ps *p, const char *key, char **val) {
    char *tok[8];
    if (p->i >= p->n) { p->detail = "unexpected end of file"; return 0; }
    const char *line = p->lines[p->i];
    int n = split_tokens(line, tok, 8);
    if (n != 2 || strcmp(tok[0], key) != 0) { p->detail = line; return 0; }
    *val = tok[1];
    p->i++;
    return 1;
}
static int expect_exact(ps *p, const char *line) {
    if (p->i >= p->n) { p->detail = "unexpected end of file"; return 0; }
    if (strcmp(p->lines[p->i], line) != 0) { p->detail = p->lines[p->i]; return 0; }
    p->i++;
    return 1;
}
static void mark_nc(ps *p, const char *d) { if (!p->noncanonical) { p->noncanonical = 1; p->noncanon_detail = d; } }
static void mark_param(ps *p, const char *d) { if (!p->bad_param) { p->bad_param = 1; p->param_detail = d; } }

/* fixed-value key: a mismatch is INVALID_PARAMETER (rule 3), not a parse error */
static int fixed_value(ps *p, const char *key, const char *want) {
    char *v;
    if (!expect_key(p, key, &v)) return 0;
    if (strcmp(v, want) != 0) mark_param(p, p->lines[p->i - 1]);
    return 1;
}
static int parse_rat_list(ps *p, const char *val, rat *out, int max, int *count) {
    char buf[AT0E_MAX_LINE]; snprintf(buf, sizeof buf, "%s", val);
    size_t l = strlen(buf);
    if (l == 0 || buf[0] == ',' || buf[l - 1] == ',' || strstr(buf, ",,")) return 0;
    int n = 0; char *s = buf;
    while (s) {
        char *c = strchr(s, ','); if (c) *c = 0;
        int canon = 1; rat tmp;
        if (!rat_parse(s, &tmp, &canon)) return 0;
        if (n >= 4096) return 0;
        if (n < max) out[n] = tmp; else mark_param(p, p->lines[p->i - 1]);
        if (canon < 0) mark_param(p, p->lines[p->i - 1]); else if (!canon) mark_nc(p, p->lines[p->i - 1]);
        n++;
        s = c ? c + 1 : NULL;
    }
    *count = n < max ? n : max;
    return 1;
}
static int parse_code_list(const char *val, char codes[16][48], int *count) {
    *count = 0;
    if (!strcmp(val, "none")) return 1;
    char buf[AT0E_MAX_LINE]; snprintf(buf, sizeof buf, "%s", val);
    size_t l = strlen(buf);
    if (l == 0 || buf[0] == ',' || buf[l - 1] == ',' || strstr(buf, ",,")) return 0;
    char *s = buf;
    while (s) {
        char *c = strchr(s, ','); if (c) *c = 0;
        if (*count >= 16 || strlen(s) >= 48 || strlen(s) == 0) return 0;
        for (const char *q = s; *q; q++) if (!((*q >= 'A' && *q <= 'Z') || *q == '_')) return 0;
        memcpy(codes[(*count)++], s, strlen(s) + 1);
        s = c ? c + 1 : NULL;
    }
    return 1;
}
static int parse_scaled_field(ps *p, const char *key, scaled *out) {
    char *v; int canon = 1;
    if (!expect_key(p, key, &v)) return 0;
    if (!scaled_parse(v, out, &canon)) { p->detail = p->lines[p->i - 1]; return 0; }
    if (!canon) mark_nc(p, p->lines[p->i - 1]);
    return 1;
}
static int parse_rat_field(ps *p, const char *key, rat *out) {
    char *v; int canon = 1;
    if (!expect_key(p, key, &v)) return 0;
    if (!rat_parse(v, out, &canon)) { p->detail = p->lines[p->i - 1]; return 0; }
    if (canon < 0) mark_param(p, p->lines[p->i - 1]); else if (!canon) mark_nc(p, p->lines[p->i - 1]);
    return 1;
}
static int parse_int_field(ps *p, const char *key, long *out) {
    char *v;
    if (!expect_key(p, key, &v)) return 0;
    /* canonical integer: shape-level parse, canonicity as NONCANONICAL */
    size_t l = strlen(v); int neg = v[0] == '-'; size_t s = neg ? 1 : 0;
    if (l == s) { p->detail = p->lines[p->i - 1]; return 0; }
    if (l - s > 12) { p->detail = p->lines[p->i - 1]; return 0; }
    long val = 0;
    for (size_t i = s; i < l; i++) { if (v[i] < '0' || v[i] > '9') { p->detail = p->lines[p->i - 1]; return 0; } val = val * 10 + (v[i] - '0'); }
    if ((l - s > 1 && v[s] == '0') || (neg && val == 0)) mark_nc(p, p->lines[p->i - 1]);
    *out = neg ? -val : val;
    return 1;
}

static char *join_lines(char **lines, size_t from, size_t to, size_t *len) {
    size_t total = 0;
    for (size_t i = from; i < to; i++) total += strlen(lines[i]) + 1;
    char *b = malloc(total + 1); size_t o = 0;
    for (size_t i = from; i < to; i++) { size_t l = strlen(lines[i]); memcpy(b + o, lines[i], l); o += l; b[o++] = '\n'; }
    b[o] = 0; *len = o;
    return b;
}

refusal case_parse_blocks(char **lines, size_t n, size_t *consumed, at0_case *c, const char **detail, int in_result) {
    ps p; memset(&p, 0, sizeof p); p.lines = lines; p.n = n;
    memset(c, 0, sizeof *c);
    char *v; long lv;
    *detail = "";
    (void)in_result;

    if (!expect_key(&p, "case_name", &v)) goto parse_err;
    if (!is_name(v)) goto parse_err_cur;
    snprintf(c->case_name, sizeof c->case_name, "%s", v);

    size_t sem_start = p.i;
    if (!expect_exact(&p, "begin semantic")) goto parse_err;
    if (!fixed_value(&p, "model_family", "PAGE_WOOTTERS_FINITE_IDEAL")) goto parse_err;
    if (!fixed_value(&p, "energy_unit", "DIMENSIONLESS_HBAR_1")) goto parse_err;
    if (!parse_int_field(&p, "clock_dim", &lv)) goto parse_err;
    if (lv < 2 || lv > AT0E_MAX_N) { mark_param(&p, p.lines[p.i - 1]); lv = lv < 2 ? 2 : AT0E_MAX_N; }
    c->N = (int)lv;
    {
        int cnt = 0;
        if (!expect_key(&p, "clock_energies", &v)) goto parse_err;
        if (!parse_rat_list(&p, v, c->E, AT0E_MAX_N, &cnt)) goto parse_err_cur;
        if (cnt != c->N) mark_param(&p, p.lines[p.i - 1]);
        for (int i = 1; i < cnt; i++) if (rat_cmp(c->E[i - 1], c->E[i]) >= 0) { mark_param(&p, p.lines[p.i - 1]); break; }
        if (cnt < c->N) c->N = cnt;
    }
    if (!fixed_value(&p, "system_dim", "2")) goto parse_err;
    {
        rat hs[4]; int cnt = 0;
        if (!expect_key(&p, "system_hamiltonian_pauli", &v)) goto parse_err;
        if (!parse_rat_list(&p, v, hs, 4, &cnt)) goto parse_err_cur;
        if (cnt != 4) goto parse_err_cur;
        c->h0 = hs[0]; c->hx = hs[1]; c->hy = hs[2]; c->hz = hs[3];
    }
    if (!fixed_value(&p, "interaction", "NONE")) goto parse_err;
    if (!fixed_value(&p, "constraint", "SUM_HC_HS")) goto parse_err;
    if (!fixed_value(&p, "physical_state", "NULLSPACE_PROJECTION")) goto parse_err;
    if (!expect_key(&p, "reference_clock_label", &v)) goto parse_err;
    if (!is_label(v)) goto parse_err_cur;
    snprintf(c->ref_label, sizeof c->ref_label, "%s", v);
    {
        if (!expect_key(&p, "reference_system_state", &v)) goto parse_err;
        char buf[AT0E_MAX_LINE]; snprintf(buf, sizeof buf, "%s", v);
        char *comma = strchr(buf, ',');
        if (!comma || strchr(comma + 1, ',')) goto parse_err_cur;
        *comma = 0;
        int c1 = 1, c2 = 1;
        if (!crat_parse(buf, &c->psi0[0], &c1) || !crat_parse(comma + 1, &c->psi0[1], &c2)) goto parse_err_cur;
        if (c1 < 0 || c2 < 0) mark_param(&p, p.lines[p.i - 1]); else if (!c1 || !c2) mark_nc(&p, p.lines[p.i - 1]);
    }
    if (!fixed_value(&p, "clock_povm", "COVARIANT_DISCRETE")) goto parse_err;
    if (!parse_rat_field(&p, "povm_tau_turns", &c->tau)) goto parse_err;
    if (!parse_rat_field(&p, "povm_weight", &c->w)) goto parse_err;
    if (!parse_int_field(&p, "clock_label_count", &lv)) goto parse_err;
    if (lv < 1 || lv > AT0E_MAX_M) { mark_param(&p, p.lines[p.i - 1]); lv = lv < 1 ? 1 : AT0E_MAX_M; }
    c->M = (int)lv;
    for (int k = 0; k < c->M; k++) {
        char *tok[8];
        if (p.i >= p.n) goto parse_err_eof;
        int nt = split_tokens(p.lines[p.i], tok, 8);
        if (nt != 3 || strcmp(tok[0], "clock_label") != 0) {
            p.detail = p.lines[p.i]; goto parse_err;
        }
        long kk;
        if (!parse_int_canonical(tok[1], &kk)) { p.detail = p.lines[p.i]; goto parse_err; }
        if (kk != k) { p.detail = p.lines[p.i]; goto parse_err; }
        if (!is_label(tok[2])) { p.detail = p.lines[p.i]; goto parse_err; }
        snprintf(c->label[k], sizeof c->label[k], "%s", tok[2]);
        p.i++;
    }
    if (!fixed_value(&p, "observables", "PAULI_X,PAULI_Y,PAULI_Z")) goto parse_err;
    if (!expect_exact(&p, "end semantic")) goto parse_err;
    size_t sem_end = p.i;

    size_t acc_start = p.i;
    if (!expect_exact(&p, "begin acceptance")) goto parse_err;
    if (!expect_key(&p, "control_kind", &v)) goto parse_err;
    if (!strcmp(v, "POSITIVE")) c->control_positive = 1; else if (!strcmp(v, "NEGATIVE")) c->control_positive = 0; else { p.detail = p.lines[p.i - 1]; goto parse_err; }  /* token set is line grammar: shape (Agent 0 ruling D3) */
    if (!expect_key(&p, "expected_outcome", &v)) goto parse_err;
    if (!strcmp(v, "PASS")) c->expected_pass = 1; else if (!strcmp(v, "FAIL")) c->expected_pass = 0; else mark_param(&p, p.lines[p.i - 1]);
    if (!expect_key(&p, "expected_failure_codes", &v)) goto parse_err;
    if (!parse_code_list(v, c->expected_codes, &c->n_expected_codes)) goto parse_err_cur;
    const char *codes_line = p.lines[p.i - 1];
    if (!expect_key(&p, "min_bound_kind", &v)) goto parse_err;
    if (!strcmp(v, "RIGOROUS")) c->min_bound_kind = BK_RIGOROUS; else if (!strcmp(v, "ESTIMATED")) c->min_bound_kind = BK_ESTIMATED; else if (!strcmp(v, "NONE")) c->min_bound_kind = BK_NONE; else mark_param(&p, p.lines[p.i - 1]);
    if (!parse_scaled_field(&p, "tol_constraint_residual", &c->tol_constraint)) goto parse_err;
    if (!parse_scaled_field(&p, "tol_povm_residual", &c->tol_povm)) goto parse_err;
    if (!parse_scaled_field(&p, "tol_probability", &c->tol_prob)) goto parse_err;
    if (!parse_scaled_field(&p, "tol_zero_probability", &c->tol_zero)) goto parse_err;
    if (!parse_scaled_field(&p, "tol_schrodinger", &c->tol_schro)) goto parse_err;
    if (!expect_exact(&p, "end acceptance")) goto parse_err;
    size_t acc_end = p.i;

    if (!expect_key(&p, "case_id", &v)) goto parse_err;
    if (!is_digest(v)) goto parse_err_cur;
    snprintf(c->case_id, sizeof c->case_id, "%s", v);
    if (!expect_key(&p, "acceptance_id", &v)) goto parse_err;
    if (!is_digest(v)) goto parse_err_cur;
    snprintf(c->acceptance_id, sizeof c->acceptance_id, "%s", v);
    *consumed = p.i;

    c->sem_block = join_lines(lines, sem_start, sem_end, &c->sem_len);
    c->acc_block = join_lines(lines, acc_start, acc_end, &c->acc_len);

    /* rule 1 (value-level canonicity) */
    if (p.noncanonical) { *detail = p.noncanon_detail; return REFUSE_NONCANONICAL; }
    /* rule 2 is checked by the caller for standalone files (header lines) */
    /* rule 3 */
    if (p.bad_param) { *detail = p.param_detail; return REFUSE_INVALID_PARAMETER; }
    if (crat_is_zero(c->psi0[0]) && crat_is_zero(c->psi0[1])) { *detail = "reference_system_state is zero"; return REFUSE_INVALID_PARAMETER; }
    if (rat_cmp(c->w, rat_make(0, 1)) <= 0) { *detail = "povm_weight <= 0"; return REFUSE_INVALID_PARAMETER; }
    if (rat_cmp(c->tau, rat_make(0, 1)) <= 0) { *detail = "povm_tau_turns <= 0"; return REFUSE_INVALID_PARAMETER; }
    c->ref_index = -1;
    for (int k = 0; k < c->M; k++) {
        for (int j = 0; j < k; j++) if (!strcmp(c->label[j], c->label[k])) { *detail = "duplicate clock label"; return REFUSE_INVALID_PARAMETER; }
        if (!strcmp(c->label[k], c->ref_label)) c->ref_index = k;
    }
    if (c->ref_index < 0) { *detail = "reference_clock_label not among clock labels"; return REFUSE_INVALID_PARAMETER; }
    /* rule 4 */
    {
        rat sq = rat_add(rat_add(rat_mul(c->hx, c->hx), rat_mul(c->hy, c->hy)), rat_mul(c->hz, c->hz));
        if (!rat_sqrt(sq, &c->radius)) { *detail = "hx^2+hy^2+hz^2 is not a rational square"; return REFUSE_IRRATIONAL_SPECTRUM; }
    }
    /* rule 5 */
    {
        char id[65];
        sha256_tagged_hex(AT0E_CASE_DOMAIN, (const uint8_t *)c->sem_block, c->sem_len, id);
        if (strcmp(id, c->case_id) != 0) { *detail = "case_id does not match the semantic block"; return REFUSE_ID_MISMATCH; }
        sha256_tagged_hex(AT0E_ACC_DOMAIN, (const uint8_t *)c->acc_block, c->acc_len, id);
        if (strcmp(id, c->acceptance_id) != 0) { *detail = "acceptance_id does not match the acceptance block"; return REFUSE_ID_MISMATCH; }
    }
    /* rule 6 */
    if (c->expected_pass) {
        if (c->n_expected_codes != 0) { *detail = codes_line; return REFUSE_INVALID_PARAMETER; }
    } else {
        if (c->n_expected_codes == 0) { *detail = codes_line; return REFUSE_INVALID_PARAMETER; }
        for (int i = 0; i < c->n_expected_codes; i++) {
            if (!failure_code_valid(c->expected_codes[i])) { *detail = codes_line; return REFUSE_INVALID_PARAMETER; }
            if (i > 0 && strcmp(c->expected_codes[i - 1], c->expected_codes[i]) >= 0) { *detail = codes_line; return REFUSE_INVALID_PARAMETER; }
        }
    }
    return REFUSE_NONE;

parse_err_cur:
    p.detail = p.lines[p.i - 1];
    goto parse_err;
parse_err_eof:
    p.detail = "unexpected end of file";
parse_err:
    *detail = p.detail ? p.detail : "shape";
    *consumed = p.i;
    return REFUSE_PARSE_ERROR;
}

refusal case_parse_validate(const textfile *tf, at0_case *c, const char **detail) {
    memset(c, 0, sizeof *c);
    *detail = "";
    if (tf->n < 4) { *detail = "too few lines"; return REFUSE_PARSE_ERROR; }
    char *tok[8];
    /* lines 0..2: shape now, content after the shape pass (rule 2 after rule 1) */
    int bad_version = 0;
    if (split_tokens(tf->lines[0], tok, 8) != 2) { *detail = tf->lines[0]; return REFUSE_PARSE_ERROR; }
    if (strcmp(tf->lines[0], "OMEGA-AT0-CASE v1") != 0) bad_version = 1;
    if (split_tokens(tf->lines[1], tok, 8) != 2 || strcmp(tok[0], "domain") != 0) { *detail = tf->lines[1]; return REFUSE_PARSE_ERROR; }
    if (strcmp(tok[1], AT0E_CASE_DOMAIN) != 0) bad_version = 1;
    if (split_tokens(tf->lines[2], tok, 8) != 2 || strcmp(tok[0], "contract") != 0) { *detail = tf->lines[2]; return REFUSE_PARSE_ERROR; }
    if (strcmp(tok[1], "AT0_CASE_V1") != 0) bad_version = 1;

    size_t consumed = 0;
    refusal r = case_parse_blocks(tf->lines + 3, tf->n - 3, &consumed, c, detail, 0);
    if (r == REFUSE_PARSE_ERROR) return r;
    /* the trailing `end` line and nothing after it are shape */
    size_t idx = 3 + consumed;
    if (idx >= tf->n || strcmp(tf->lines[idx], "end") != 0) { *detail = idx < tf->n ? tf->lines[idx] : "missing end"; return REFUSE_PARSE_ERROR; }
    if (idx + 1 != tf->n) { *detail = tf->lines[idx + 1]; return REFUSE_PARSE_ERROR; }
    if (r == REFUSE_NONCANONICAL) return r;
    if (bad_version) { *detail = "header, domain or contract line"; return REFUSE_UNSUPPORTED_VERSION; }
    if (r == REFUSE_NONE) sha256_hex(tf->bytes, tf->len, c->case_file_sha256);
    return r;
}

void case_free(at0_case *c) { free(c->sem_block); free(c->acc_block); c->sem_block = c->acc_block = NULL; }

/* ---------------- emission ---------------- */
static void scaled_format(scaled s, char *buf, size_t cap) {
    char digits[64]; int n = 0; u128 v = s.n;
    if (v == 0) { snprintf(buf, cap, "0@0"); return; }
    while (v) { digits[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    char num[64]; for (int i = 0; i < n; i++) num[i] = digits[n - 1 - i]; num[n] = 0;
    snprintf(buf, cap, "%s@%d", num, s.k);
}
static void crat_format(crat z, char *buf, size_t cap) {
    char a[64], b[64]; rat_format(z.re, a, sizeof a); rat_format(z.im, b, sizeof b);
    size_t o = 0;
    if (o + 1 < cap) buf[o++] = (char)40;
    for (char *p = a; *p && o + 1 < cap; p++) buf[o++] = *p;
    if (o + 1 < cap) buf[o++] = (char)59;
    for (char *p = b; *p && o + 1 < cap; p++) buf[o++] = *p;
    if (o + 1 < cap) buf[o++] = (char)41;
    buf[o] = 0;
}
#define EMIT(...) do { int w_ = snprintf(buf + o, cap > o ? cap - o : 0, __VA_ARGS__); if (w_ < 0) return -1; o += (size_t)w_; } while (0)
long case_emit(const at0_case *c, char *buf, size_t cap) {
    size_t o = 0; char t[8][80];
    EMIT("OMEGA-AT0-CASE v1\ndomain %s\ncontract AT0_CASE_V1\ncase_name %s\n", AT0E_CASE_DOMAIN, c->case_name);
    size_t sem_start = o;
    EMIT("begin semantic\nmodel_family PAGE_WOOTTERS_FINITE_IDEAL\nenergy_unit DIMENSIONLESS_HBAR_1\nclock_dim %d\nclock_energies ", c->N);
    for (int i = 0; i < c->N; i++) { rat_format(c->E[i], t[0], sizeof t[0]); EMIT("%s%s", i ? "," : "", t[0]); }
    rat_format(c->h0, t[0], 80); rat_format(c->hx, t[1], 80); rat_format(c->hy, t[2], 80); rat_format(c->hz, t[3], 80);
    EMIT("\nsystem_dim 2\nsystem_hamiltonian_pauli %s,%s,%s,%s\ninteraction NONE\nconstraint SUM_HC_HS\nphysical_state NULLSPACE_PROJECTION\n", t[0], t[1], t[2], t[3]);
    crat_format(c->psi0[0], t[0], 80); crat_format(c->psi0[1], t[1], 80);
    EMIT("reference_clock_label %s\nreference_system_state %s,%s\nclock_povm COVARIANT_DISCRETE\n", c->ref_label, t[0], t[1]);
    rat_format(c->tau, t[0], 80); rat_format(c->w, t[1], 80);
    EMIT("povm_tau_turns %s\npovm_weight %s\nclock_label_count %d\n", t[0], t[1], c->M);
    for (int k = 0; k < c->M; k++) EMIT("clock_label %d %s\n", k, c->label[k]);
    EMIT("observables PAULI_X,PAULI_Y,PAULI_Z\nend semantic\n");
    size_t sem_end = o;
    size_t acc_start = o;
    EMIT("begin acceptance\ncontrol_kind %s\nexpected_outcome %s\nexpected_failure_codes ", c->control_positive ? "POSITIVE" : "NEGATIVE", c->expected_pass ? "PASS" : "FAIL");
    if (c->n_expected_codes == 0) EMIT("none");
    for (int i = 0; i < c->n_expected_codes; i++) EMIT("%s%s", i ? "," : "", c->expected_codes[i]);
    EMIT("\nmin_bound_kind %s\n", c->min_bound_kind == BK_RIGOROUS ? "RIGOROUS" : c->min_bound_kind == BK_ESTIMATED ? "ESTIMATED" : "NONE");
    scaled_format(c->tol_constraint, t[0], 80); EMIT("tol_constraint_residual %s\n", t[0]);
    scaled_format(c->tol_povm, t[0], 80); EMIT("tol_povm_residual %s\n", t[0]);
    scaled_format(c->tol_prob, t[0], 80); EMIT("tol_probability %s\n", t[0]);
    scaled_format(c->tol_zero, t[0], 80); EMIT("tol_zero_probability %s\n", t[0]);
    scaled_format(c->tol_schro, t[0], 80); EMIT("tol_schrodinger %s\n", t[0]);
    EMIT("end acceptance\n");
    size_t acc_end = o;
    if (o >= cap) return -1;
    char id[65];
    sha256_tagged_hex(AT0E_CASE_DOMAIN, (const uint8_t *)buf + sem_start, sem_end - sem_start, id);
    EMIT("case_id %s\n", id);
    sha256_tagged_hex(AT0E_ACC_DOMAIN, (const uint8_t *)buf + acc_start, acc_end - acc_start, id);
    EMIT("acceptance_id %s\nend\n", id);
    if (o >= cap) return -1;
    return (long)o;
}
