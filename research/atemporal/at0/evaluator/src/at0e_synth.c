/* Synthetic result writer for evaluator self-tests. The honest fixture takes its
 * numbers from the shadow oracle (rounded to binary64, bound 1@15, ESTIMATED).
 * Every mutant below is a deliberate defect the evaluator must catch. Nothing
 * written here is AT-0 evidence: provenance fields are marked SYNTHETIC. */
#include "at0e.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const mutant_names[] = {
    /* physics-level (engine or oracle defects) */
    "axis_swap",            /* engine exchanges X and Y results */
    "y_sign_swap",          /* engine exchanges Y PLUS and MINUS */
    "conjugate_bug",        /* engine conditions on -theta (missing complex conjugate) */
    "reversed_reference",   /* oracle evolves with exp(+iHt) */
    "dephased_state",       /* engine reports the dephased (mixed) conditional state */
    "wrong_weight_hidden",  /* engine ignores povm_weight and claims povm_residual 0 */
    "hardcoded_table",      /* engine returns the KAT table whatever the case asks */
    "kernel_dim_wrong",     /* physical_state_kernel_dim off by one */
    "residual_over_bound",  /* constraint_residual within tolerance but above its own bound */
    "nonfinite_hidden",     /* a nonfinite value with check values_finite still PASS */
    /* receipt-level (forged or altered records) */
    "verdict_forged",       /* checks/outcome claim PASS regardless of values; ids recomputed honestly */
    "evidence_corrupt",     /* one hex digit of evidence_digest flipped */
    "verdict_id_corrupt",   /* one hex digit of verdict_id flipped */
    "case_id_altered",      /* embedded case_id altered */
    "tolerance_altered",    /* acceptance block edited after the fact, acceptance_id stale */
    "binding_rebound",      /* acceptance block edited and acceptance_id recomputed: ids self-consistent, binding to the case file broken */
    "contract_commit_wrong",
    "dirty_tree",
    "times_reversed",
    "oracle_is_engine",     /* oracle_sha256 == engine_sha256 */
    "bound_none_nonzero",   /* bound_kind NONE with nonzero bounds */
    "label_status_wrong",   /* a DEFINED label marked UNDEFINED with values dropped */
    "placeholder_with_values", /* NOT_RUN result carrying values */
    "crlf",                 /* CRLF line endings */
    "trailing_space",
    "label_claimed_defined", /* an UNDEFINED label marked DEFINED with fabricated (correct-looking) pauli values */
    NULL };
const char *const *synth_mutant_names(int *count) { int n = 0; while (mutant_names[n]) n++; *count = n; return mutant_names; }

static uint64_t f64_bits(double x) { uint64_t b; if (x == 0.0) x = 0.0; memcpy(&b, &x, 8); return b; }
static const char *const axis_names[3] = { "X", "Y", "Z" };
static const char *const sign_names[2] = { "PLUS", "MINUS" };
static const char *const ck_names[4] = { "PASS", "FAIL", "INDETERMINATE", "NOT_EVALUATED" };

#define EMIT(...) do { int w_ = snprintf(buf + o, cap > o ? cap - o : 0, __VA_ARGS__); if (w_ < 0) return -1; o += (size_t)w_; } while (0)
static void fmt_rval(const rval *v, char *out, size_t cap) { /* out is >= 96 bytes at every call */
    char b[48];
    { /* scaled format */
        if (v->bound.n == 0) snprintf(b, sizeof b, "0@0");
        else { char d[40]; int n = 0; u128 x = v->bound.n; while (x && n < 39) { d[n++] = (char)('0' + (int)(x % 10)); x /= 10; } char num[40]; for (int i = 0; i < n; i++) num[i] = d[n - 1 - i]; num[n] = 0; snprintf(b, sizeof b, "%s@%d", num, v->bound.k); }
    }
    if (v->is_undefined) snprintf(out, cap, "undefined %s", b);
    else if (v->is_nonfinite) snprintf(out, cap, "nonfinite %s", b);
    else snprintf(out, cap, "f64:%016llx %s", (unsigned long long)v->bits, b);
}
static long result_emit(const at0_result *r, char *buf, size_t cap) {
    size_t o = 0; char t[96];
    const at0_case *c = &r->c;
    EMIT("OMEGA-AT0-RESULT v%d\ndomain omega.at0.result.v%d\ncontract AT0_RESULT_V%d\ncase_contract AT0_CASE_V1\ncase_name %s\n", r->version, r->version, r->version, c->case_name);
    EMIT("%.*s%.*s", (int)c->sem_len, c->sem_block, (int)c->acc_len, c->acc_block);
    EMIT("case_id %s\nacceptance_id %s\ncase_file_sha256 %s\n", c->case_id, c->acceptance_id, r->case_file_sha256);
    static const char *const arith[4] = { "BINARY64", "BINARY64_INTERVAL", "BINARY64_COMPENSATED", "NONE" };
    EMIT("begin numerics\narithmetic %s\nbound_kind %s\nthreads 1\nend numerics\n", arith[r->arithmetic], r->bound_kind == BK_RIGOROUS ? "RIGOROUS" : r->bound_kind == BK_ESTIMATED ? "ESTIMATED" : "NONE");
    EMIT("begin values\nphysical_state_kernel_dim %ld\n", r->kernel_dim);
    fmt_rval(&r->constraint_residual, t, sizeof t); EMIT("constraint_residual %s\n", t);
    fmt_rval(&r->povm_residual, t, sizeof t); EMIT("povm_residual %s\n", t);
    for (int k = 0; k < c->M; k++) EMIT("label %d %s %s\n", k, c->label[k], r->label_status[k] == LS_DEFINED ? "DEFINED" : r->label_status[k] == LS_UNDEFINED ? "UNDEFINED" : "INDETERMINATE");
    for (int k = 0; k < c->M; k++) { fmt_rval(&r->clock_p[k], t, sizeof t); EMIT("clock_probability %d %s\n", k, t); }
    for (int k = 0; k < c->M; k++) for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) { fmt_rval(&r->pauli[k][a][s], t, sizeof t); EMIT("pauli %d %s %s %s\n", k, axis_names[a], sign_names[s], t); }
    for (int k = 0; k < c->M; k++) for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) { fmt_rval(&r->reference[k][a][s], t, sizeof t); EMIT("reference %d %s %s %s\n", k, axis_names[a], sign_names[s], t); }
    EMIT("end values\n");
    size_t ver_start = o;
    EMIT("begin verdict\n");
    for (int i = 0; i < AT0E_NCHECKS; i++) EMIT("check %s %s\n", at0e_check_names[i], ck_names[r->check[i]]);
    static const char *const outn[4] = { "PASS", "FAIL", "ERROR", "NOT_RUN" };
    EMIT("outcome %s\nfailure_codes ", outn[r->outcome]);
    if (r->n_failure_codes == 0) EMIT("none");
    for (int i = 0; i < r->n_failure_codes; i++) EMIT("%s%s", i ? "," : "", r->failure_codes[i]);
    EMIT("\nerror_code %s\nexpectation_met %s\nend verdict\n", r->error_code, r->expectation_met == 0 ? "YES" : r->expectation_met == 1 ? "NO" : "NOT_APPLICABLE");
    size_t ver_end = o;
    if (o >= cap) return -1;
    {
        char pre[200]; int pl = snprintf(pre, sizeof pre, "case_id %s\nacceptance_id %s\n", c->case_id, c->acceptance_id);
        uint8_t *tmp = malloc((size_t)pl + (ver_end - ver_start)); memcpy(tmp, pre, (size_t)pl); memcpy(tmp + pl, buf + ver_start, ver_end - ver_start);
        char id[65]; sha256_tagged_hex(r->version == 2 ? AT0E_VERDICT_DOMAIN_V2 : AT0E_VERDICT_DOMAIN_V1, tmp, (size_t)pl + (ver_end - ver_start), id); free(tmp);
        EMIT("verdict_id %s\n", id);
    }
    EMIT("begin provenance\nsource_repo %s\nsource_commit %s\nsource_tree_clean %s\ncontract_commit %s\nengine_sha256 %s\noracle_repo %s\noracle_commit %s\noracle_sha256 %s\nbuild_cc %s\nbuild_flags %s\nhost %s\nrun_started_utc %s\nrun_finished_utc %s\nend provenance\n",
         r->source_repo, r->source_commit, r->source_tree_clean ? "YES" : "NO", r->contract_commit, r->engine_sha256, r->oracle_repo, r->oracle_commit, r->oracle_sha256, r->build_cc, r->build_flags, r->host, r->run_started, r->run_finished);
    if (o >= cap) return -1;
    { char id[65]; sha256_tagged_hex(r->version == 2 ? AT0E_EVIDENCE_DOMAIN_V2 : AT0E_EVIDENCE_DOMAIN_V1, (const uint8_t *)buf, o, id); EMIT("evidence_digest %s\nend\n", id); }
    if (o >= cap) return -1;
    return (long)o;
}

static void set_val(rval *v, long double x, int bound_exp) { memset(v, 0, sizeof *v); v->bits = f64_bits((double)x); v->bound.n = 1; v->bound.k = bound_exp; }
static void set_undef(rval *v) { memset(v, 0, sizeof *v); v->is_undefined = 1; }

long result_synth(const at0_case *c, const uint8_t *case_bytes, size_t case_len, const char *mutant, char *buf, size_t cap, const char **why) {
    at0_result r; memset(&r, 0, sizeof r);
    shadow sh;
    *why = NULL;
    if (!shadow_compute(c, &sh, why)) return -1;
    const char *m = mutant ? mutant : "";
    int found = !mutant;
    for (int i = 0; mutant_names[i]; i++) if (!strcmp(m, mutant_names[i])) found = 1;
    if (!found) { *why = "unknown mutant"; return -1; }

    /* copy case (shallow blocks are fine: not freed here) */
    r.c = *c; r.version = AT0E_RESULT_VERSION_DEFAULT;
    sha256_hex(case_bytes, case_len, r.case_file_sha256);
    r.arithmetic = 0; r.bound_kind = BK_ESTIMATED;
    r.kernel_dim = sh.kernel_dim;
    int nontrivial = sh.kernel_dim >= 1 && sh.psi_nonzero;
    if (nontrivial) set_val(&r.constraint_residual, 0.0L, 15); else set_undef(&r.constraint_residual);
    set_val(&r.povm_residual, sh.povm_exact_identity ? 0.0L : sh.povm_residual, 15);
    if (!strcmp(m, "wrong_weight_hidden")) set_val(&r.povm_residual, 0.0L, 15);   /* lie: claims exact normalization */
    for (int k = 0; k < c->M; k++) {
        if (nontrivial) {
            long double pk = sh.clock_p[k];
            if (!strcmp(m, "wrong_weight_hidden")) pk = sh.clock_p[k] * ((long double)c->N / (long double)c->M) / ((long double)(long long)c->w.n / (long double)(long long)c->w.d);
            set_val(&r.clock_p[k], pk, 15);
        } else set_undef(&r.clock_p[k]);
        /* label status from the written value, per contract */
        if (!nontrivial) r.label_status[k] = LS_UNDEFINED;
        else {
            long double tz = (long double)(unsigned long long)c->tol_zero.n; for (int i = 0; i < c->tol_zero.k; i++) tz /= 10.0L;
            r.label_status[k] = sh.clock_p[k] - 1e-15L > tz ? LS_DEFINED : sh.clock_p[k] + 1e-15L <= tz ? LS_UNDEFINED : LS_INDETERMINATE;
        }
        long double pv[3][2], rv[3][2];
        memcpy(pv, sh.pauli[k], sizeof pv); memcpy(rv, sh.reference[k], sizeof rv);
        if (!strcmp(m, "axis_swap")) { for (int s = 0; s < 2; s++) { long double t = pv[0][s]; pv[0][s] = pv[1][s]; pv[1][s] = t; } }
        if (!strcmp(m, "y_sign_swap")) { long double t = pv[1][0]; pv[1][0] = pv[1][1]; pv[1][1] = t; }
        if (!strcmp(m, "conjugate_bug")) { pv[1][0] = 1.0L - pv[1][0]; pv[1][1] = 1.0L - pv[1][1]; } /* <Y> -> -<Y>; X, Z unchanged */
        if (!strcmp(m, "reversed_reference")) { rv[1][0] = 1.0L - rv[1][0]; rv[1][1] = 1.0L - rv[1][1]; }
        if (!strcmp(m, "dephased_state")) { pv[0][0] = pv[0][1] = pv[1][0] = pv[1][1] = 0.5L; }
        if (!strcmp(m, "hardcoded_table")) {
            static const long double kat[4][3] = { { 1, 0.5L, 0.5L }, { 0.5L, 1, 0.5L }, { 0, 0.5L, 0.5L }, { 0.5L, 0, 0.5L } };
            for (int a = 0; a < 3; a++) { pv[a][0] = kat[k % 4][a]; pv[a][1] = 1.0L - kat[k % 4][a]; }
            set_val(&r.clock_p[k], 0.25L, 15);
        }
        for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) {
            if (r.label_status[k] == LS_DEFINED) set_val(&r.pauli[k][a][s], pv[a][s], 15); else set_undef(&r.pauli[k][a][s]);
            set_val(&r.reference[k][a][s], rv[a][s], 15);
        }
    }
    if (!strcmp(m, "kernel_dim_wrong")) r.kernel_dim += 1;
    if (!strcmp(m, "residual_over_bound") && nontrivial) { set_val(&r.constraint_residual, 5e-13L, 15); }
    if (!strcmp(m, "nonfinite_hidden") && c->M > 0) { memset(&r.reference[0][2][0], 0, sizeof(rval)); r.reference[0][2][0].is_nonfinite = 1; }
    if (!strcmp(m, "label_status_wrong") && c->M > 0 && nontrivial) { r.label_status[0] = LS_UNDEFINED; for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) set_undef(&r.pauli[0][a][s]); }
    if (!strcmp(m, "label_claimed_defined") && c->M > 0 && nontrivial) { r.label_status[0] = LS_DEFINED; for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) set_val(&r.pauli[0][a][s], sh.pauli[0][a][s], 15); }
    if (!strcmp(m, "bound_none_nonzero")) r.bound_kind = BK_NONE;

    /* honest verdict from the exact checker on these values */
    rederived d; result_rederive(&r, &d);
    memcpy(r.check, d.check, sizeof r.check);
    r.outcome = d.outcome; r.n_failure_codes = d.n_failure_codes; memcpy(r.failure_codes, d.failure_codes, sizeof r.failure_codes);
    r.expectation_met = d.expectation_met;
    snprintf(r.error_code, sizeof r.error_code, "none");
    if (!strcmp(m, "nonfinite_hidden")) { r.check[1] = CK_PASS; r.outcome = OUT_PASS; r.n_failure_codes = 0; r.expectation_met = c->expected_pass ? 0 : 1; }
    if (!strcmp(m, "verdict_forged")) { for (int i = 0; i < AT0E_NCHECKS; i++) r.check[i] = CK_PASS; r.outcome = OUT_PASS; r.n_failure_codes = 0; r.expectation_met = c->expected_pass ? 0 : 1; }
    if (!strcmp(m, "placeholder_with_values")) { r.outcome = OUT_NOT_RUN; r.expectation_met = 2; r.n_failure_codes = 0; for (int i = 0; i < AT0E_NCHECKS; i++) r.check[i] = CK_NOTEVAL; }

    snprintf(r.source_repo, sizeof r.source_repo, "aien-dev/omega");
    snprintf(r.source_commit, sizeof r.source_commit, "%040d", 0);
    r.source_tree_clean = strcmp(m, "dirty_tree") != 0;
    snprintf(r.contract_commit, sizeof r.contract_commit, "%s", !strcmp(m, "contract_commit_wrong") ? "27f3b71000000000000000000000000000000000" : AT0E_CONTRACT_COMMIT);
    snprintf(r.engine_sha256, 65, "%064d", 1);
    snprintf(r.oracle_repo, sizeof r.oracle_repo, "aien-dev/omega");
    snprintf(r.oracle_commit, sizeof r.oracle_commit, "%040d", 0);
    snprintf(r.oracle_sha256, 65, "%064d", !strcmp(m, "oracle_is_engine") ? 1 : 2);
    snprintf(r.build_cc, sizeof r.build_cc, "SYNTHETIC evaluator self-test fixture, not an engine run");
    snprintf(r.build_flags, sizeof r.build_flags, "none");
    snprintf(r.host, sizeof r.host, "synthetic");
    snprintf(r.run_started, 32, !strcmp(m, "times_reversed") ? "2026-10-09T23:00:01Z" : "2026-10-09T23:00:00Z");
    snprintf(r.run_finished, 32, "2026-10-09T23:00:00Z");

    long n = result_emit(&r, buf, cap);
    if (n < 0) { *why = "buffer"; return -1; }
    /* text-level mutations after an honest emission */
    if (!strcmp(m, "evidence_corrupt") || !strcmp(m, "verdict_id_corrupt") || !strcmp(m, "case_id_altered")) {
        const char *key = !strcmp(m, "evidence_corrupt") ? "\nevidence_digest " : !strcmp(m, "verdict_id_corrupt") ? "\nverdict_id " : "\ncase_id ";
        char *p = strstr(buf, key); if (!p) { *why = "mutation anchor"; return -1; }
        p += strlen(key); *p = (*p == '0') ? '1' : '0';
    }
    if (!strcmp(m, "tolerance_altered") || !strcmp(m, "binding_rebound")) {
        char *p = strstr(buf, "\ntol_schrodinger "); if (!p) { *why = "mutation anchor"; return -1; }
        p += strlen("\ntol_schrodinger ");
        /* widen the tolerance: replace the first digit with 9 (keeps canonical form) */
        *p = '9';
        if (!strcmp(m, "binding_rebound")) {
            /* recompute acceptance_id over the edited block, verdict_id and evidence_digest; leave case_file_sha256 */
            char *bs = strstr(buf, "\nbegin acceptance\n") + 1, *be = strstr(buf, "\nend acceptance\n") + strlen("\nend acceptance\n");
            char id[65]; sha256_tagged_hex(AT0E_ACC_DOMAIN, (const uint8_t *)bs, (size_t)(be - bs), id);
            char *ap = strstr(buf, "\nacceptance_id ") + strlen("\nacceptance_id "); memcpy(ap, id, 64);
            char *vs = strstr(buf, "\nbegin verdict\n") + 1, *ve = strstr(buf, "\nend verdict\n") + strlen("\nend verdict\n");
            char pre[200]; int pl = snprintf(pre, sizeof pre, "case_id %s\nacceptance_id %s\n", c->case_id, id);
            uint8_t *tmp = malloc((size_t)pl + (size_t)(ve - vs)); memcpy(tmp, pre, (size_t)pl); memcpy(tmp + pl, vs, (size_t)(ve - vs));
            sha256_tagged_hex(r.version == 2 ? AT0E_VERDICT_DOMAIN_V2 : AT0E_VERDICT_DOMAIN_V1, tmp, (size_t)pl + (size_t)(ve - vs), id); free(tmp);
            char *vp = strstr(buf, "\nverdict_id ") + strlen("\nverdict_id "); memcpy(vp, id, 64);
        }
        /* evidence digest re-sealed in both variants (the forger re-signs the record) */
        char *ep = strstr(buf, "\nevidence_digest ") + 1;
        char id[65]; sha256_tagged_hex(r.version == 2 ? AT0E_EVIDENCE_DOMAIN_V2 : AT0E_EVIDENCE_DOMAIN_V1, (const uint8_t *)buf, (size_t)(ep - buf), id);
        memcpy(ep + strlen("evidence_digest "), id, 64);
    }
    if (!strcmp(m, "crlf")) {
        if ((size_t)n * 2 + 1 > cap) { *why = "buffer"; return -1; }
        char *tmp = malloc((size_t)n * 2 + 1); size_t o = 0;
        for (long i = 0; i < n; i++) { if (buf[i] == '\n') tmp[o++] = '\r'; tmp[o++] = buf[i]; }
        memcpy(buf, tmp, o); buf[o] = 0; free(tmp); n = (long)o;
    }
    if (!strcmp(m, "trailing_space")) {
        char *p = strstr(buf, "\nthreads 1\n"); if (!p) { *why = "mutation anchor"; return -1; }
        size_t at = (size_t)(p + strlen("\nthreads 1") - buf);
        memmove(buf + at + 1, buf + at, (size_t)n - at + 1); buf[at] = ' '; n++;
    }
    return n;
}
