/* Independent AT0_RESULT_V1 parser, exact re-derivation of the ten checks, and
 * the evaluator's verification pass (identities, bindings, provenance, shadow). */
#include "at0e.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* aien-architecture commits verified (sha256sum) to hold AT0_CASE_V1.md d90af74b... and AT0_RESULT_V1.md dc52c573... unchanged */
const char *const at0e_contract_commits[] = { AT0E_CONTRACT_COMMIT, "00e9f2308666f74e58f524b177b2a31eccdc69ba", NULL };
const char *const at0e_check_names[AT0E_NCHECKS] = {
    "bound_kind_sufficient", "values_finite", "physical_state_nontrivial", "constraint_residual",
    "povm_normalization", "clock_probability_sum", "probability_range", "pauli_pair_sum",
    "conditional_defined", "schrodinger_agreement" };
const char *const at0e_check_codes[AT0E_NCHECKS] = {
    "BOUND_KIND_INSUFFICIENT", "NONFINITE_VALUE", "TRIVIAL_PHYSICAL_STATE", "CONSTRAINT_RESIDUAL_EXCEEDED",
    "POVM_NORMALIZATION_EXCEEDED", "PROBABILITY_SUM_EXCEEDED", "PROBABILITY_OUT_OF_RANGE", "PROBABILITY_SUM_EXCEEDED",
    "CONDITIONAL_UNDEFINED", "SCHRODINGER_DEVIATION_EXCEEDED" };
static const char *const axis_names[3] = { "X", "Y", "Z" };
static const char *const sign_names[2] = { "PLUS", "MINUS" };

/* ---------------- parse ---------------- */
typedef struct { char **lines; size_t n, i; const char *finding, *detail; } rp;
#define FAIL_WITH(code, det) do { p->finding = code; p->detail = det; return 0; } while (0)
static int rp_exact(rp *p, const char *line) {
    if (p->i >= p->n) FAIL_WITH("E4-STRUCT-TRUNCATED", "unexpected end of file");
    if (strcmp(p->lines[p->i], line) != 0) FAIL_WITH("E4-STRUCT-ORDER", p->lines[p->i]);
    p->i++; return 1;
}
static int rp_key(rp *p, const char *key, char **val) {
    char *tok[8];
    if (p->i >= p->n) FAIL_WITH("E4-STRUCT-TRUNCATED", "unexpected end of file");
    int n = split_tokens(p->lines[p->i], tok, 8);
    if (n != 2 || strcmp(tok[0], key) != 0) FAIL_WITH("E4-STRUCT-ORDER", p->lines[p->i]);
    *val = tok[1]; p->i++; return 1;
}
static int rp_text_key(rp *p, const char *key, char *dst, size_t cap, int allow_none) {
    if (p->i >= p->n) FAIL_WITH("E4-STRUCT-TRUNCATED", "unexpected end of file");
    const char *line = p->lines[p->i]; size_t kl = strlen(key);
    if (strncmp(line, key, kl) != 0 || line[kl] != ' ') FAIL_WITH("E4-STRUCT-ORDER", line);
    const char *v = line + kl + 1;
    if (!is_text200(v)) FAIL_WITH("E4-STRUCT-ENCODING", line);
    (void)allow_none;
    snprintf(dst, cap, "%s", v); p->i++; return 1;
}
static int parse_rval(rp *p, const char *vtok, const char *btok, rval *out) {
    memset(out, 0, sizeof *out);
    int canon = 1;
    if (!scaled_parse(btok, &out->bound, &canon) || !canon) FAIL_WITH("E4-STRUCT-ENCODING", p->lines[p->i]);
    if (!strcmp(vtok, "undefined")) { out->is_undefined = 1; if (out->bound.n != 0) FAIL_WITH("E4-STRUCT-ENCODING", p->lines[p->i]); return 1; }
    if (!strcmp(vtok, "nonfinite")) { out->is_nonfinite = 1; return 1; }
    if (strncmp(vtok, "f64:", 4) != 0 || !hex_to_u64(vtok + 4, &out->bits)) FAIL_WITH("E4-STRUCT-ENCODING", p->lines[p->i]);
    if (((out->bits >> 52) & 0x7ff) == 0x7ff) FAIL_WITH("E4-STRUCT-ENCODING", "nonfinite bit pattern written as f64:");
    if (out->bits == 0x8000000000000000ULL) FAIL_WITH("E4-STRUCT-ENCODING", "-0.0 must be written as +0.0");
    return 1;
}
static int rp_value_line(rp *p, const char *key, rval *out) {
    char *tok[8];
    if (p->i >= p->n) FAIL_WITH("E4-STRUCT-TRUNCATED", "unexpected end of file");
    int n = split_tokens(p->lines[p->i], tok, 8);
    if (n != 3 || strcmp(tok[0], key) != 0) FAIL_WITH("E4-STRUCT-ORDER", p->lines[p->i]);
    if (!parse_rval(p, tok[1], tok[2], out)) return 0;
    p->i++; return 1;
}
static int rp_indexed_value(rp *p, const char *key, int k, int axis, int sign, rval *out) {
    char *tok[8]; long kk;
    if (p->i >= p->n) FAIL_WITH("E4-STRUCT-TRUNCATED", "unexpected end of file");
    int n = split_tokens(p->lines[p->i], tok, 8);
    int want = axis < 0 ? 4 : 6;
    if (n != want || strcmp(tok[0], key) != 0) FAIL_WITH("E4-STRUCT-ORDER", p->lines[p->i]);
    if (!parse_int_canonical(tok[1], &kk) || kk != k) FAIL_WITH("E4-STRUCT-ORDER", p->lines[p->i]);
    if (axis >= 0) {
        if (strcmp(tok[2], axis_names[axis]) != 0 || strcmp(tok[3], sign_names[sign]) != 0) FAIL_WITH("E4-STRUCT-ORDER", p->lines[p->i]);
    }
    if (!parse_rval(p, tok[want - 2], tok[want - 1], out)) return 0;
    p->i++; return 1;
}
static int parse_codes(const char *val, char codes[16][48], int *count) {
    *count = 0;
    if (!strcmp(val, "none")) return 1;
    char buf[AT0E_MAX_LINE]; snprintf(buf, sizeof buf, "%s", val);
    size_t l = strlen(buf);
    if (l == 0 || buf[0] == ',' || buf[l - 1] == ',' || strstr(buf, ",,")) return 0;
    char *s = buf;
    while (s) {
        char *c = strchr(s, ','); if (c) *c = 0;
        if (*count >= 16 || strlen(s) >= 48 || !failure_code_valid(s)) return 0;
        if (*count > 0 && strcmp(codes[*count - 1], s) >= 0) return 0;  /* sorted, unique */
        memcpy(codes[(*count)++], s, strlen(s) + 1);
        s = c ? c + 1 : NULL;
    }
    return 1;
}

int result_parse(const textfile *tf, at0_result *r, const char **finding, const char **detail) {
    rp pp = { tf->lines, tf->n, 0, NULL, NULL }, *p = &pp;
    memset(r, 0, sizeof *r);
    char *v;
    *finding = NULL; *detail = "";
    if (p->i < p->n && !strcmp(p->lines[0], "OMEGA-AT0-RESULT v2")) r->version = 2;
    else if (p->i < p->n && !strcmp(p->lines[0], "OMEGA-AT0-RESULT v1")) r->version = 1;
    else { p->finding = "E4-STRUCT-VERSION"; p->detail = tf->n ? tf->lines[0] : "empty"; goto out; }
    p->i = 1;
    if (!rp_exact(p, r->version == 2 ? "domain omega.at0.result.v2" : "domain omega.at0.result.v1")) goto out;
    if (!rp_exact(p, r->version == 2 ? "contract AT0_RESULT_V2" : "contract AT0_RESULT_V1")) goto out;
    if (!rp_exact(p, "case_contract AT0_CASE_V1")) goto out;
    {
        size_t consumed = 0; const char *d = "";
        refusal rf = case_parse_blocks(tf->lines + p->i, tf->n - p->i, &consumed, &r->c, &d, 1);
        if (rf == REFUSE_PARSE_ERROR) { p->finding = "E4-STRUCT-CASE-SHAPE"; p->detail = d; goto out; }
        if (rf == REFUSE_ID_MISMATCH) { p->finding = "E4-ID-CASE"; p->detail = d; goto out; }
        if (rf != REFUSE_NONE) { p->finding = "E4-STRUCT-CASE-REFUSED"; p->detail = d; goto out; }
        p->i += consumed;
    }
    if (!rp_key(p, "case_file_sha256", &v)) goto out;
    if (!is_digest(v)) goto enc_err;
    snprintf(r->case_file_sha256, 65, "%s", v);
    if (!rp_exact(p, "begin numerics")) goto out;
    if (!rp_key(p, "arithmetic", &v)) goto out;
    if (!strcmp(v, "BINARY64")) r->arithmetic = 0; else if (!strcmp(v, "BINARY64_INTERVAL")) r->arithmetic = 1;
    else if (!strcmp(v, "BINARY64_COMPENSATED")) r->arithmetic = 2; else if (!strcmp(v, "NONE")) r->arithmetic = 3;
    else goto enc_err;
    if (!rp_key(p, "bound_kind", &v)) goto out;
    if (!strcmp(v, "RIGOROUS")) r->bound_kind = BK_RIGOROUS; else if (!strcmp(v, "ESTIMATED")) r->bound_kind = BK_ESTIMATED;
    else if (!strcmp(v, "NONE")) r->bound_kind = BK_NONE; else goto enc_err;
    if (!rp_exact(p, "threads 1")) goto out;
    if (!rp_exact(p, "end numerics")) goto out;
    if (!rp_exact(p, "begin values")) goto out;
    if (!rp_key(p, "physical_state_kernel_dim", &v)) goto out;
    if (!parse_int_canonical(v, &r->kernel_dim) || r->kernel_dim < 0) goto enc_err;
    if (!rp_value_line(p, "constraint_residual", &r->constraint_residual)) goto out;
    if (!rp_value_line(p, "povm_residual", &r->povm_residual)) goto out;
    for (int k = 0; k < r->c.M; k++) {
        char *tok[8]; long kk;
        if (p->i >= p->n) { p->finding = "E4-STRUCT-TRUNCATED"; p->detail = "values"; goto out; }
        int n = split_tokens(p->lines[p->i], tok, 8);
        if (n != 4 || strcmp(tok[0], "label") || !parse_int_canonical(tok[1], &kk) || kk != k || strcmp(tok[2], r->c.label[k]) != 0) { p->finding = "E4-STRUCT-ORDER"; p->detail = p->lines[p->i]; goto out; }
        if (!strcmp(tok[3], "DEFINED")) r->label_status[k] = LS_DEFINED; else if (!strcmp(tok[3], "UNDEFINED")) r->label_status[k] = LS_UNDEFINED;
        else if (!strcmp(tok[3], "INDETERMINATE")) r->label_status[k] = LS_INDETERMINATE; else { p->finding = "E4-STRUCT-ENCODING"; p->detail = p->lines[p->i]; goto out; }
        p->i++;
    }
    for (int k = 0; k < r->c.M; k++) if (!rp_indexed_value(p, "clock_probability", k, -1, 0, &r->clock_p[k])) goto out;
    for (int k = 0; k < r->c.M; k++) for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) if (!rp_indexed_value(p, "pauli", k, a, s, &r->pauli[k][a][s])) goto out;
    for (int k = 0; k < r->c.M; k++) for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) if (!rp_indexed_value(p, "reference", k, a, s, &r->reference[k][a][s])) goto out;
    if (!rp_exact(p, "end values")) goto out;
    size_t ver_start = p->i;
    if (!rp_exact(p, "begin verdict")) goto out;
    for (int i = 0; i < AT0E_NCHECKS; i++) {
        char *tok[8];
        if (p->i >= p->n) { p->finding = "E4-STRUCT-TRUNCATED"; p->detail = "verdict"; goto out; }
        int n = split_tokens(p->lines[p->i], tok, 8);
        if (n != 3 || strcmp(tok[0], "check") || strcmp(tok[1], at0e_check_names[i])) { p->finding = "E4-STRUCT-ORDER"; p->detail = p->lines[p->i]; goto out; }
        if (!strcmp(tok[2], "PASS")) r->check[i] = CK_PASS; else if (!strcmp(tok[2], "FAIL")) r->check[i] = CK_FAIL;
        else if (!strcmp(tok[2], "INDETERMINATE")) r->check[i] = CK_INDET; else if (!strcmp(tok[2], "NOT_EVALUATED")) r->check[i] = CK_NOTEVAL;
        else { p->finding = "E4-STRUCT-ENCODING"; p->detail = p->lines[p->i]; goto out; }
        p->i++;
    }
    if (!rp_key(p, "outcome", &v)) goto out;
    if (!strcmp(v, "PASS")) r->outcome = OUT_PASS; else if (!strcmp(v, "FAIL")) r->outcome = OUT_FAIL; else if (!strcmp(v, "ERROR")) r->outcome = OUT_ERROR;
    else if (!strcmp(v, "NOT_RUN")) r->outcome = OUT_NOT_RUN; else goto enc_err;
    if (!rp_key(p, "failure_codes", &v)) goto out;
    if (!parse_codes(v, r->failure_codes, &r->n_failure_codes)) goto enc_err;
    if (!rp_key(p, "error_code", &v)) goto out;
    if (strcmp(v, "none") && strcmp(v, "ORACLE_UNAVAILABLE") && strcmp(v, "RESOURCE_LIMIT") && strcmp(v, "INTERNAL_ERROR")) goto enc_err;
    snprintf(r->error_code, sizeof r->error_code, "%s", v);
    if (!rp_key(p, "expectation_met", &v)) goto out;
    if (!strcmp(v, "YES")) r->expectation_met = 0; else if (!strcmp(v, "NO")) r->expectation_met = 1; else if (!strcmp(v, "NOT_APPLICABLE")) r->expectation_met = 2; else goto enc_err;
    if (!rp_exact(p, "end verdict")) goto out;
    {
        size_t total = 0;
        for (size_t i = ver_start; i < p->i; i++) total += strlen(tf->lines[i]) + 1;
        r->ver_block = malloc(total + 1); size_t o = 0;
        for (size_t i = ver_start; i < p->i; i++) { size_t l = strlen(tf->lines[i]); memcpy(r->ver_block + o, tf->lines[i], l); o += l; r->ver_block[o++] = '\n'; }
        r->ver_block[o] = 0; r->ver_len = o;
    }
    if (!rp_key(p, "verdict_id", &v)) goto out;
    if (!is_digest(v)) goto enc_err;
    snprintf(r->verdict_id, 65, "%s", v);
    if (!rp_exact(p, "begin provenance")) goto out;
    if (!rp_key(p, "source_repo", &v)) goto out;
    if (!strchr(v, '/') || strlen(v) > 79) goto enc_err;
    snprintf(r->source_repo, sizeof r->source_repo, "%s", v);
    if (!rp_key(p, "source_commit", &v)) goto out;
    if (!is_sha1hex(v)) goto enc_err;
    snprintf(r->source_commit, sizeof r->source_commit, "%s", v);
    if (!rp_key(p, "source_tree_clean", &v)) goto out;
    if (!strcmp(v, "YES")) r->source_tree_clean = 1; else if (!strcmp(v, "NO")) r->source_tree_clean = 0; else goto enc_err;
    if (!rp_key(p, "contract_commit", &v)) goto out;
    if (!is_sha1hex(v)) goto enc_err;
    snprintf(r->contract_commit, sizeof r->contract_commit, "%s", v);
    int placeholder = r->outcome == OUT_ERROR || r->outcome == OUT_NOT_RUN;
    if (!rp_key(p, "engine_sha256", &v)) goto out;
    if (!(is_digest(v) || (placeholder && !strcmp(v, "none")))) goto enc_err;
    snprintf(r->engine_sha256, 65, "%s", v);
    if (!rp_key(p, "oracle_repo", &v)) goto out;
    if (!((strchr(v, '/') && strlen(v) < 80) || (placeholder && !strcmp(v, "none")))) goto enc_err;
    snprintf(r->oracle_repo, sizeof r->oracle_repo, "%s", v);
    if (!rp_key(p, "oracle_commit", &v)) goto out;
    if (!(is_sha1hex(v) || (placeholder && !strcmp(v, "none")))) goto enc_err;
    snprintf(r->oracle_commit, sizeof r->oracle_commit, "%s", v);
    if (!rp_key(p, "oracle_sha256", &v)) goto out;
    if (!(is_digest(v) || (placeholder && !strcmp(v, "none")))) goto enc_err;
    snprintf(r->oracle_sha256, 65, "%s", v);
    if (!rp_text_key(p, "build_cc", r->build_cc, sizeof r->build_cc, placeholder)) goto out;
    if (!rp_text_key(p, "build_flags", r->build_flags, sizeof r->build_flags, placeholder)) goto out;
    if (!rp_text_key(p, "host", r->host, sizeof r->host, placeholder)) goto out;
    for (int t = 0; t < 2; t++) {
        char *dst = t ? r->run_finished : r->run_started;
        if (!rp_key(p, t ? "run_finished_utc" : "run_started_utc", &v)) goto out;
        if (!(placeholder && !strcmp(v, "none"))) {
            if (strlen(v) != 20 || v[4] != '-' || v[7] != '-' || v[10] != 'T' || v[13] != ':' || v[16] != ':' || v[19] != 'Z') goto enc_err;
            for (int i = 0; i < 19; i++) if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && (v[i] < '0' || v[i] > '9')) goto enc_err;
        }
        snprintf(dst, 32, "%s", v);
    }
    {
        char prev[AT0E_MAX_LINE] = "";
        while (p->i < p->n && strncmp(tf->lines[p->i], "artifact ", 9) == 0) {
            char *tok[8];
            int n = split_tokens(tf->lines[p->i], tok, 8);
            if (n != 3 || !is_digest(tok[2]) || strlen(tok[1]) == 0) { p->finding = "E4-STRUCT-ENCODING"; p->detail = tf->lines[p->i]; goto out; }
            if (prev[0] && strcmp(prev, tok[1]) >= 0) { p->finding = "E4-STRUCT-ORDER"; p->detail = tf->lines[p->i]; goto out; }
            snprintf(prev, sizeof prev, "%s", tok[1]);
            r->n_artifacts++; p->i++;
        }
    }
    if (!rp_exact(p, "end provenance")) goto out;
    r->evidence_line_index = p->i;
    if (!rp_key(p, "evidence_digest", &v)) goto out;
    if (!is_digest(v)) goto enc_err;
    snprintf(r->evidence_digest, 65, "%s", v);
    if (!rp_exact(p, "end")) goto out;
    if (p->i != tf->n) { p->finding = "E4-STRUCT-TRAILING"; p->detail = tf->lines[p->i]; goto out; }
    return 0;
enc_err:
    p->finding = "E4-STRUCT-ENCODING"; p->detail = p->lines[p->i - 1]; goto out;
out:
    *finding = p->finding ? p->finding : "E4-STRUCT-PARSE";
    *detail = p->detail ? p->detail : "";
    return 1;
}
void result_free(at0_result *r) { case_free(&r->c); free(r->ver_block); r->ver_block = NULL; }

/* ---------------- exact re-derivation ---------------- */
static int worst(int a, int b) { /* FAIL > INDET > PASS ; NOT_EVALUATED is neutral */
    if (a == CK_NOTEVAL) return b;
    if (b == CK_NOTEVAL) return a;
    return a > b ? a : b;
}
static int tri_to_ck(tri t) { return t == CMP_PASS ? CK_PASS : t == CMP_FAIL ? CK_FAIL : CK_INDET; }
static int label_status_derive(const rval *p, scaled tol_zero) {
    if (p->is_undefined || p->is_nonfinite) return LS_UNDEFINED;
    sbig v, b, t, hi, lo, nb;
    sbig_from_f64_bits(&v, p->bits); sbig_from_scaled(&b, p->bound); sbig_from_scaled(&t, tol_zero);
    sbig_add(&hi, &v, &b);
    if (sbig_cmp(&hi, &t) <= 0) return LS_UNDEFINED;
    nb = b; sbig_neg(&nb); sbig_add(&lo, &v, &nb);
    if (sbig_cmp(&lo, &t) > 0) return LS_DEFINED;
    return LS_INDETERMINATE;
}
static void add_code(rederived *d, const char *code) {
    for (int i = 0; i < d->n_failure_codes; i++) if (!strcmp(d->failure_codes[i], code)) return;
    if (d->n_failure_codes >= 16) return;
    snprintf(d->failure_codes[d->n_failure_codes++], 48, "%s", code);
    for (int i = d->n_failure_codes - 1; i > 0 && strcmp(d->failure_codes[i - 1], d->failure_codes[i]) > 0; i--) {
        char t[48]; memcpy(t, d->failure_codes[i], 48); memcpy(d->failure_codes[i], d->failure_codes[i - 1], 48); memcpy(d->failure_codes[i - 1], t, 48);
    }
}
void result_rederive(const at0_result *r, rederived *d) {
    memset(d, 0, sizeof *d);
    const at0_case *c = &r->c;
    sbig tol_prob, tol_schro, tol_c, tol_p, one;
    sbig_from_scaled(&tol_prob, c->tol_prob); sbig_from_scaled(&tol_schro, c->tol_schro);
    sbig_from_scaled(&tol_c, c->tol_constraint); sbig_from_scaled(&tol_p, c->tol_povm); sbig_from_int(&one, 1);
    for (int i = 0; i < AT0E_NCHECKS; i++) d->check[i] = CK_NOTEVAL;

    /* 1 */ d->check[0] = r->bound_kind >= c->min_bound_kind ? CK_PASS : CK_FAIL;
    /* 2 */ {
        int nf = r->constraint_residual.is_nonfinite || r->povm_residual.is_nonfinite;
        for (int k = 0; k < c->M; k++) { nf |= r->clock_p[k].is_nonfinite; for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) nf |= r->pauli[k][a][s].is_nonfinite | r->reference[k][a][s].is_nonfinite; }
        d->check[1] = nf ? CK_FAIL : CK_PASS;
    }
    /* 3: exact, from the case */ shadow sh; const char *why; int have_sh = shadow_compute(c, &sh, &why);
    int nontrivial = have_sh ? (sh.kernel_dim >= 1 && sh.psi_nonzero) : (r->kernel_dim >= 1);
    d->check[2] = nontrivial ? CK_PASS : CK_FAIL;
    /* trivial kernel: checks 4..10 lose their input (charter N1, spec 9.2 N1); ambiguity A1 in README */
    if (nontrivial) {
    /* 4 */ if (!r->constraint_residual.is_undefined && !r->constraint_residual.is_nonfinite) {
        sbig v, b; sbig_from_f64_bits(&v, r->constraint_residual.bits); sbig_from_scaled(&b, r->constraint_residual.bound);
        d->check[3] = tri_to_ck(tri_absolute(&v, &b, &tol_c));
    }
    }
    /* 5 */ if (!r->povm_residual.is_undefined && !r->povm_residual.is_nonfinite) {
        sbig v, b; sbig_from_f64_bits(&v, r->povm_residual.bits); sbig_from_scaled(&b, r->povm_residual.bound);
        d->check[4] = tri_to_ck(tri_absolute(&v, &b, &tol_p));
    }
    if (nontrivial) {
    /* 6 */ {
        int ok = 1; sbig sum, bsum; sbig_zero(&sum); sbig_zero(&bsum);
        for (int k = 0; k < c->M && ok; k++) {
            if (r->clock_p[k].is_undefined || r->clock_p[k].is_nonfinite) { ok = 0; break; }
            sbig v, b; sbig_from_f64_bits(&v, r->clock_p[k].bits); sbig_from_scaled(&b, r->clock_p[k].bound);
            sbig_add(&sum, &sum, &v); sbig_add(&bsum, &bsum, &b);
        }
        if (ok) { sbig m1 = one; sbig_neg(&m1); sbig_add(&sum, &sum, &m1); d->check[5] = tri_to_ck(tri_absolute(&sum, &bsum, &tol_prob)); }
    }
    /* 7 */ {
        int st = CK_NOTEVAL;
        const rval *all[1 + AT0E_MAX_M * 13]; int n = 0;
        for (int k = 0; k < c->M; k++) { all[n++] = &r->clock_p[k]; for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) { all[n++] = &r->pauli[k][a][s]; all[n++] = &r->reference[k][a][s]; } }
        for (int i = 0; i < n; i++) {
            if (all[i]->is_undefined || all[i]->is_nonfinite) continue;
            sbig v, b, nv, vm1, m1 = one; sbig_from_f64_bits(&v, all[i]->bits); sbig_from_scaled(&b, all[i]->bound);
            nv = v; sbig_neg(&nv); st = worst(st, tri_to_ck(tri_signed(&nv, &b, &tol_prob)));
            sbig_neg(&m1); sbig_add(&vm1, &v, &m1); st = worst(st, tri_to_ck(tri_signed(&vm1, &b, &tol_prob)));
        }
        d->check[6] = st;
    }
    /* 8 */ {
        int st = CK_NOTEVAL;
        for (int k = 0; k < c->M; k++) {
            if (r->label_status[k] != LS_DEFINED) continue;
            for (int a = 0; a < 3; a++) {
                const rval *pp = &r->pauli[k][a][0], *pm = &r->pauli[k][a][1];
                if (pp->is_undefined || pp->is_nonfinite || pm->is_undefined || pm->is_nonfinite) continue;
                sbig v1, v2, b1, b2, s, bs, m1 = one;
                sbig_from_f64_bits(&v1, pp->bits); sbig_from_f64_bits(&v2, pm->bits); sbig_from_scaled(&b1, pp->bound); sbig_from_scaled(&b2, pm->bound);
                sbig_add(&s, &v1, &v2); sbig_neg(&m1); sbig_add(&s, &s, &m1); sbig_add(&bs, &b1, &b2);
                st = worst(st, tri_to_ck(tri_absolute(&s, &bs, &tol_prob)));
            }
        }
        d->check[7] = st;
    }
    /* 9 */ {
        int any_undef = 0;
        for (int k = 0; k < c->M; k++) { if (r->label_status[k] == LS_UNDEFINED) any_undef = 1; if (r->label_status[k] == LS_INDETERMINATE) d->any_label_indeterminate = 1; }
        d->check[8] = any_undef ? CK_FAIL : CK_PASS;
    }
    /* 10 */ {
        int st = CK_NOTEVAL;
        for (int k = 0; k < c->M; k++) {
            if (r->label_status[k] != LS_DEFINED) continue;
            for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) {
                const rval *pv = &r->pauli[k][a][s], *rv = &r->reference[k][a][s];
                if (pv->is_undefined || pv->is_nonfinite || rv->is_undefined || rv->is_nonfinite) continue;
                sbig v1, v2, b1, b2, dlt, bs;
                sbig_from_f64_bits(&v1, pv->bits); sbig_from_f64_bits(&v2, rv->bits); sbig_from_scaled(&b1, pv->bound); sbig_from_scaled(&b2, rv->bound);
                sbig_neg(&v2); sbig_add(&dlt, &v1, &v2); sbig_add(&bs, &b1, &b2);
                st = worst(st, tri_to_ck(tri_absolute(&dlt, &bs, &tol_schro)));
            }
        }
        d->check[9] = st;
    }
    } /* nontrivial */
    /* codes and outcome */
    int any_bad = 0;
    for (int i = 0; i < AT0E_NCHECKS; i++) {
        if (d->check[i] == CK_FAIL) { add_code(d, at0e_check_codes[i]); any_bad = 1; }
        if (d->check[i] == CK_INDET) { add_code(d, "PRECISION_INSUFFICIENT"); any_bad = 1; }
    }
    if (d->any_label_indeterminate) { add_code(d, "PRECISION_INSUFFICIENT"); any_bad = 1; }
    d->outcome = any_bad ? OUT_FAIL : OUT_PASS;
    int same = (d->outcome == OUT_PASS) == (c->expected_pass != 0) && d->n_failure_codes == c->n_expected_codes;
    for (int i = 0; same && i < d->n_failure_codes; i++) if (strcmp(d->failure_codes[i], c->expected_codes[i])) same = 0;
    d->expectation_met = same ? 0 : 1;
}

/* ---------------- verification ---------------- */
static long double f64_ld(uint64_t bits) { double x; memcpy(&x, &bits, 8); return (long double)x; }
static long double scaled_ld(scaled s) { long double v = (long double)(unsigned long long)(s.n & 0xffffffffffffffffULL); for (int i = 0; i < s.k; i++) v /= 10.0L; return v; }
static const char *const fail_codes_sev[] = {
    "E4-STRUCT-", "E4-ID-", "E4-BIND-", "E4-CHECK-", "E4-OUTCOME-", "E4-CODES-", "E4-EXPECTATION-", "E4-LABEL-",
    "E4-SHADOW-", "E4-BOUND-", "E4-PLACEHOLDER-", "E4-TIME-", "E4-PROV-ORACLE-IS-ENGINE", "E4-PROV-NONE-", NULL };
static int is_fail_code(const char *code) { if (!strcmp(code, "E4-SHADOW-UNAVAILABLE")) return 0; for (int i = 0; fail_codes_sev[i]; i++) if (!strncmp(code, fail_codes_sev[i], strlen(fail_codes_sev[i]))) return 1; return 0; }

const char *result_verify(const textfile *tf, const at0_result *r, const textfile *case_tf, int use_shadow, findings *fs) {
    const at0_case *c = &r->c;
    char id[65];
    int inconclusive = 0;
    /* identities */
    {
        char pre[200]; int pl = snprintf(pre, sizeof pre, "case_id %s\nacceptance_id %s\n", c->case_id, c->acceptance_id);
        uint8_t *buf = malloc((size_t)pl + r->ver_len); memcpy(buf, pre, (size_t)pl); memcpy(buf + pl, r->ver_block, r->ver_len);
        sha256_tagged_hex(r->version == 2 ? AT0E_VERDICT_DOMAIN_V2 : AT0E_VERDICT_DOMAIN_V1, buf, (size_t)pl + r->ver_len, id); free(buf);
        if (strcmp(id, r->verdict_id)) findings_add(fs, "E4-ID-VERDICT", "verdict_id %s does not recompute (expected %s)", r->verdict_id, id);
        size_t upto = 0;
        for (size_t i = 0; i < r->evidence_line_index; i++) upto += strlen(tf->lines[i]) + 1;
        sha256_tagged_hex(r->version == 2 ? AT0E_EVIDENCE_DOMAIN_V2 : AT0E_EVIDENCE_DOMAIN_V1, tf->bytes, upto, id);
        if (strcmp(id, r->evidence_digest)) findings_add(fs, "E4-ID-EVIDENCE", "evidence_digest does not recompute: record altered or digest forged (expected %s)", id);
    }
    /* binding to the supplied case file */
    if (case_tf) {
        at0_case cc; const char *d;
        refusal rf = case_parse_validate(case_tf, &cc, &d);
        if (rf != REFUSE_NONE) findings_add(fs, "E4-BIND-CASE-REFUSED", "supplied case file is refused: %s (%s)", refusal_name(rf), d);
        else {
            if (strcmp(cc.case_file_sha256, r->case_file_sha256)) findings_add(fs, "E4-BIND-CASE-FILE", "case_file_sha256 %s differs from the supplied case file %s", r->case_file_sha256, cc.case_file_sha256);
            if (strcmp(cc.case_id, c->case_id)) findings_add(fs, "E4-BIND-CASE-ID", "embedded case_id differs from the supplied case file");
            if (strcmp(cc.acceptance_id, c->acceptance_id)) findings_add(fs, "E4-BIND-ACCEPTANCE-ID", "embedded acceptance_id differs from the supplied case file");
            if (cc.sem_len != c->sem_len || memcmp(cc.sem_block, c->sem_block, c->sem_len)) findings_add(fs, "E4-BIND-SEMANTIC-BLOCK", "embedded semantic block is not byte-identical to the supplied case");
            if (cc.acc_len != c->acc_len || memcmp(cc.acc_block, c->acc_block, c->acc_len)) findings_add(fs, "E4-BIND-ACCEPTANCE-BLOCK", "embedded acceptance block is not byte-identical to the supplied case");
            if (strcmp(cc.case_name, c->case_name)) findings_add(fs, "E4-BIND-CASE-NAME", "case_name differs from the supplied case file");
        }
        case_free(&cc);
    }
    /* provenance */
    { int known = 0; for (int i = 0; at0e_contract_commits[i]; i++) if (!strcmp(r->contract_commit, at0e_contract_commits[i])) known = 1;
      if (!known) { findings_add(fs, "E4-PROV-CONTRACT-UNVERIFIED", "contract_commit %s is not a commit verified to hold the frozen contract bytes (verified: %s, %s)", r->contract_commit, at0e_contract_commits[0], at0e_contract_commits[1]); inconclusive = 1; } }
    if (!r->source_tree_clean) { findings_add(fs, "E4-PROV-DIRTY", "source_tree_clean NO: result may not be cited for a gate (AT0_RESULT_V1 section 7)"); inconclusive = 1; }
    int placeholder = r->outcome == OUT_ERROR || r->outcome == OUT_NOT_RUN;
    if (!placeholder) {
        if (!strcmp(r->engine_sha256, "none") || !strcmp(r->oracle_sha256, "none") || !strcmp(r->run_started, "none") || !strcmp(r->run_finished, "none"))
            findings_add(fs, "E4-PROV-NONE-IN-COMPLETED-RUN", "a completed run carries a `none` provenance token");
        if (strcmp(r->run_started, "none") && strcmp(r->run_finished, "none") && strcmp(r->run_finished, r->run_started) < 0)
            findings_add(fs, "E4-TIME-ORDER", "run_finished_utc %s precedes run_started_utc %s", r->run_finished, r->run_started);
        if (!strcmp(r->engine_sha256, r->oracle_sha256) && strcmp(r->engine_sha256, "none")) findings_add(fs, "E4-PROV-ORACLE-IS-ENGINE", "engine_sha256 equals oracle_sha256: reference values did not come from a separate executable");
    } else {
        int bad = r->kernel_dim != 0 || r->n_failure_codes != 0 || r->expectation_met != 2;
        for (int i = 0; i < AT0E_NCHECKS; i++) if (r->check[i] != CK_NOTEVAL) bad = 1;
        if (!r->constraint_residual.is_undefined || !r->povm_residual.is_undefined) bad = 1;
        for (int k = 0; k < c->M; k++) { if (!r->clock_p[k].is_undefined) bad = 1; for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) if (!r->pauli[k][a][s].is_undefined || !r->reference[k][a][s].is_undefined) bad = 1; }
        if (r->outcome == OUT_ERROR && !strcmp(r->error_code, "none")) bad = 1;
        if (r->outcome == OUT_NOT_RUN && strcmp(r->error_code, "none")) bad = 1;
        if (bad) findings_add(fs, "E4-PLACEHOLDER-SHAPE", "ERROR/NOT_RUN result violates the placeholder shape of AT0_RESULT_V1 section 5");
        return fs->n ? (is_fail_code(fs->f[0].code) ? "FAIL" : "INCONCLUSIVE") : "PASS";
    }
    if (strcmp(r->error_code, "none")) findings_add(fs, "E4-STRUCT-ERROR-CODE", "completed run carries error_code %s", r->error_code);
    /* label status and token discipline */
    {
        shadow sx; const char *wx; int triv = shadow_compute(c, &sx, &wx) ? !(sx.kernel_dim >= 1 && sx.psi_nonzero) : 0;
        if (!triv) {
            int u = r->constraint_residual.is_undefined || r->povm_residual.is_undefined;
            for (int k = 0; k < c->M; k++) { u |= r->clock_p[k].is_undefined; for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) u |= r->pauli[k][a][s].is_undefined || r->reference[k][a][s].is_undefined; }
            if (u) findings_add(fs, "E4-STRUCT-UNDEFINED-TOKEN", "undefined token in a result whose kernel is nontrivial (refusal under AT0_RESULT_V2)");
        } else {
            int bad = !r->constraint_residual.is_undefined || r->povm_residual.is_undefined;
            for (int k = 0; k < c->M; k++) { bad |= !r->clock_p[k].is_undefined || r->label_status[k] != LS_UNDEFINED; for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) bad |= !r->pauli[k][a][s].is_undefined || r->reference[k][a][s].is_undefined; }
            if (bad) findings_add(fs, "E4-STRUCT-TRIVIAL-KERNEL-SHAPE", "trivial kernel: constraint_residual, clock probabilities and pauli values must be undefined, labels UNDEFINED, povm_residual and references real");
        }
    }
    for (int k = 0; k < c->M; k++) {
        int want = label_status_derive(&r->clock_p[k], c->tol_zero);
        if (want != r->label_status[k]) findings_add(fs, "E4-LABEL-STATUS", "label %d status does not follow from clock_probability and tol_zero_probability", k);
        for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) {
            int u = r->pauli[k][a][s].is_undefined;
            if (r->label_status[k] == LS_DEFINED && u) findings_add(fs, "E4-STRUCT-UNDEFINED-ON-DEFINED", "label %d is DEFINED but pauli %s %s is undefined", k, axis_names[a], sign_names[s]);
            if (r->label_status[k] != LS_DEFINED && !u) findings_add(fs, "E4-STRUCT-VALUE-ON-UNDEFINED", "label %d is not DEFINED but pauli %s %s carries a value", k, axis_names[a], sign_names[s]);
            if (r->reference[k][a][s].is_undefined) findings_add(fs, "E4-STRUCT-REFERENCE-UNDEFINED", "reference %d %s %s is undefined; references are always written", k, axis_names[a], sign_names[s]);
        }
    }
    if (r->bound_kind == BK_NONE) {
        int nz = r->constraint_residual.bound.n || r->povm_residual.bound.n;
        for (int k = 0; k < c->M; k++) { nz |= r->clock_p[k].bound.n != 0; for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) nz |= (r->pauli[k][a][s].bound.n != 0) | (r->reference[k][a][s].bound.n != 0); }
        if (nz) findings_add(fs, "E4-BOUND-NONE-NONZERO", "bound_kind NONE but a nonzero bound is written");
    }
    /* re-derivation */
    rederived d; result_rederive(r, &d);
    for (int i = 0; i < AT0E_NCHECKS; i++) if (d.check[i] != r->check[i]) {
        static const char *const nm[] = { "PASS", "FAIL", "INDETERMINATE", "NOT_EVALUATED" };
        findings_add(fs, "E4-CHECK-MISMATCH", "check %s reported %s, re-derived %s", at0e_check_names[i], nm[r->check[i]], nm[d.check[i]]);
    }
    if (d.outcome != r->outcome) findings_add(fs, "E4-OUTCOME-MISMATCH", "outcome reported %s, re-derived %s", r->outcome == OUT_PASS ? "PASS" : "FAIL", d.outcome == OUT_PASS ? "PASS" : "FAIL");
    {
        int same = d.n_failure_codes == r->n_failure_codes;
        for (int i = 0; same && i < d.n_failure_codes; i++) if (strcmp(d.failure_codes[i], r->failure_codes[i])) same = 0;
        if (!same) findings_add(fs, "E4-CODES-MISMATCH", "failure_codes differ from the re-derived set (%d reported, %d re-derived)", r->n_failure_codes, d.n_failure_codes);
    }
    if (d.expectation_met != r->expectation_met) findings_add(fs, "E4-EXPECTATION-MISMATCH", "expectation_met reported %s, re-derived %s", r->expectation_met == 0 ? "YES" : r->expectation_met == 1 ? "NO" : "NOT_APPLICABLE", d.expectation_met ? "NO" : "YES");
    /* shadow */
    if (use_shadow) {
        shadow sh; const char *why;
        if (!shadow_compute(c, &sh, &why)) { findings_add(fs, "E4-SHADOW-UNAVAILABLE", "%s", why); inconclusive = 1; }
        else {
            if (sh.kernel_dim != r->kernel_dim) findings_add(fs, "E4-SHADOW-KERNEL-DIM", "physical_state_kernel_dim %ld, exact value %d", r->kernel_dim, sh.kernel_dim);
            int nontrivial = sh.kernel_dim >= 1 && sh.psi_nonzero;
            if (nontrivial && !r->constraint_residual.is_undefined && !r->constraint_residual.is_nonfinite) {
                long double v = fabsl(f64_ld(r->constraint_residual.bits)), b = scaled_ld(r->constraint_residual.bound);
                if (v > b) findings_add(fs, "E4-BOUND-CLAIM-FALSE", "constraint_residual %.3Le exceeds its own bound %.3Le although the exact residual of a kernel projection is 0", v, b);
            }
            if (!nontrivial && !r->constraint_residual.is_undefined) findings_add(fs, "E4-STRUCT-RESIDUAL-ON-TRIVIAL", "constraint_residual written although the kernel is trivial");
            if (!r->povm_residual.is_undefined && !r->povm_residual.is_nonfinite) {
                long double v = fabsl(f64_ld(r->povm_residual.bits)), b = scaled_ld(r->povm_residual.bound);
                if (sh.povm_exact_identity) { if (v > b) findings_add(fs, "E4-BOUND-CLAIM-FALSE", "povm_residual %.3Le exceeds its bound %.3Le although sum F_k = I exactly", v, b); }
                if (fabsl(v - sh.povm_residual) > b + sh.bound) findings_add(fs, "E4-SHADOW-POVM", "povm_residual %.6Le vs shadow %.6Le beyond bound %.1Le", v, sh.povm_residual, b);
            } else if (!r->povm_residual.is_nonfinite) findings_add(fs, "E4-STRUCT-POVM-UNDEFINED", "povm_residual is undefined; it is always computable");
            int n_clock = 0, n_pauli = 0, n_ref = 0;
            for (int k = 0; k < c->M; k++) {
                if (nontrivial && !r->clock_p[k].is_undefined && !r->clock_p[k].is_nonfinite) {
                    long double v = f64_ld(r->clock_p[k].bits), b = scaled_ld(r->clock_p[k].bound);
                    if (fabsl(v - sh.clock_p[k]) > b + sh.bound) { if (n_clock++ < 3) findings_add(fs, "E4-SHADOW-CLOCK-PROB", "clock_probability %d %.9Le vs shadow %.9Le beyond bound %.1Le", k, v, sh.clock_p[k], b); }
                }
                for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) {
                    if (r->label_status[k] == LS_DEFINED && nontrivial && !r->pauli[k][a][s].is_undefined && !r->pauli[k][a][s].is_nonfinite) {
                        long double v = f64_ld(r->pauli[k][a][s].bits), b = scaled_ld(r->pauli[k][a][s].bound);
                        if (fabsl(v - sh.pauli[k][a][s]) > b + sh.bound) { if (n_pauli++ < 6) findings_add(fs, "E4-SHADOW-PAULI", "pauli %d %s %s %.9Le vs shadow %.9Le beyond bound %.1Le", k, axis_names[a], sign_names[s], v, sh.pauli[k][a][s], b); }
                    }
                    if (!r->reference[k][a][s].is_undefined && !r->reference[k][a][s].is_nonfinite) {
                        long double v = f64_ld(r->reference[k][a][s].bits), b = scaled_ld(r->reference[k][a][s].bound);
                        if (fabsl(v - sh.reference[k][a][s]) > b + sh.bound) { if (n_ref++ < 6) findings_add(fs, "E4-SHADOW-REFERENCE", "reference %d %s %s %.9Le vs shadow %.9Le beyond bound %.1Le", k, axis_names[a], sign_names[s], v, sh.reference[k][a][s], b); }
                    }
                }
            }
            if (n_clock > 3) findings_add(fs, "E4-SHADOW-CLOCK-PROB", "... %d clock probabilities disagree in total", n_clock);
            if (n_pauli > 6) findings_add(fs, "E4-SHADOW-PAULI", "... %d pauli values disagree in total", n_pauli);
            if (n_ref > 6) findings_add(fs, "E4-SHADOW-REFERENCE", "... %d reference values disagree in total", n_ref);
        }
    } else inconclusive = 1;
    for (int i = 0; i < fs->n; i++) if (is_fail_code(fs->f[i].code)) return "FAIL";
    return inconclusive ? "INCONCLUSIVE" : "PASS";
}
