/* at0-eval: command-line front end of the AT-0 Agent 4 evaluator.
 *
 *   at0-eval case <case-file> [--detail]
 *       validates a case; prints AT0_CASE_OK <case_id> (exit 0) or, to stderr,
 *       AT0_CASE_REFUSED <code> (exit 2), the same protocol the contract fixes
 *       for the candidate tool, so both can be compared line for line.
 *   at0-eval result <result-file> [--case <case-file>] [--no-shadow] [--json]
 *       verifies a result independently; prints AT0E_VERIFY <PASS|FAIL|INCONCLUSIVE>
 *       and one line per finding (exit 0 PASS, 1 FAIL, 3 INCONCLUSIVE, 2 unreadable).
 *   at0-eval shadow <case-file>
 *       prints the shadow oracle's table (hand-derivation aid).
 *   at0-eval synth <case-file> [mutant]      synthetic result to stdout (self-test only)
 *   at0-eval mutants                          lists mutant names
 *   at0-eval gen key=value ...                emits a canonical case file to stdout
 */
#include "at0e.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmd_case(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: at0-eval case <file> [--detail]\n"); return 64; }
    int detail = argc > 1 && !strcmp(argv[1], "--detail");
    textfile tf; const char *why;
    int rc = textfile_read(argv[0], &tf, &why);
    if (rc >= 100) { fprintf(stderr, "at0-eval: %s: %s\n", argv[0], why); return 2; }
    if (rc) { fprintf(stderr, "AT0_CASE_REFUSED CASE_PARSE_ERROR\n"); if (detail) fprintf(stderr, "  byte rule: %s\n", why); textfile_free(&tf); return 2; }
    at0_case c; const char *d;
    refusal r = case_parse_validate(&tf, &c, &d);
    if (r != REFUSE_NONE) { fprintf(stderr, "AT0_CASE_REFUSED %s\n", refusal_name(r)); if (detail) fprintf(stderr, "  at: %s\n", d); case_free(&c); textfile_free(&tf); return 2; }
    printf("AT0_CASE_OK %s\n", c.case_id);
    if (detail) printf("  acceptance_id %s\n  case_file_sha256 %s\n", c.acceptance_id, c.case_file_sha256);
    case_free(&c); textfile_free(&tf);
    return 0;
}

static void json_escape(const char *s, FILE *f) {
    fputc('"', f);
    for (; *s; s++) { if (*s == '"' || *s == '\\') fputc('\\', f); if ((unsigned char)*s < 0x20) fputc(' ', f); else fputc(*s, f); }
    fputc('"', f);
}
static int cmd_result(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: at0-eval result <file> [--case <case>] [--no-shadow] [--json]\n"); return 64; }
    const char *case_path = NULL; int shadow_on = 1, json = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--case") && i + 1 < argc) case_path = argv[++i];
        else if (!strcmp(argv[i], "--no-shadow")) shadow_on = 0;
        else if (!strcmp(argv[i], "--json")) json = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 64; }
    }
    findings fs; fs.n = 0;
    textfile tf, ctf; const char *why; const char *status;
    int have_case = 0;
    if (case_path) {
        int rc = textfile_read(case_path, &ctf, &why);
        if (rc) { findings_add(&fs, "E4-BIND-CASE-UNREADABLE", "case file %s: %s", case_path, why); } else have_case = 1;
    }
    int rc = textfile_read(argv[0], &tf, &why);
    at0_result r; memset(&r, 0, sizeof r);
    int parsed = 0;
    if (rc >= 100) { findings_add(&fs, "E4-STRUCT-UNREADABLE", "%s", why); status = "FAIL"; }
    else if (rc) { findings_add(&fs, "E4-STRUCT-BYTES", "byte rule violated: %s", why); status = "FAIL"; }
    else {
        const char *fcode, *fdetail;
        if (result_parse(&tf, &r, &fcode, &fdetail)) { findings_add(&fs, fcode, "%s", fdetail); status = "FAIL"; }
        else { parsed = 1; status = result_verify(&tf, &r, have_case ? &ctf : NULL, shadow_on, &fs); }
    }
    if (json) {
        printf("{\"status\":\"%s\",\"result_file\":", status); json_escape(argv[0], stdout);
        printf(",\"case_file\":"); if (case_path) json_escape(case_path, stdout); else printf("null");
        printf(",\"shadow\":%s,\"outcome_reported\":", shadow_on ? "true" : "false");
        if (parsed) { static const char *const o[4] = { "PASS", "FAIL", "ERROR", "NOT_RUN" }; printf("\"%s\"", o[r.outcome]); } else printf("null");
        printf(",\"findings\":[");
        for (int i = 0; i < fs.n; i++) { printf("%s{\"code\":\"%s\",\"detail\":", i ? "," : "", fs.f[i].code); json_escape(fs.f[i].detail, stdout); printf("}"); }
        printf("]}\n");
    } else {
        printf("AT0E_VERIFY %s\n", status);
        for (int i = 0; i < fs.n; i++) printf("  %s: %s\n", fs.f[i].code, fs.f[i].detail);
    }
    if (parsed) result_free(&r);
    if (rc < 100) textfile_free(&tf);
    if (have_case) textfile_free(&ctf);
    return !strcmp(status, "PASS") ? 0 : !strcmp(status, "FAIL") ? 1 : 3;
}

static int load_case(const char *path, textfile *tf, at0_case *c) {
    const char *why, *d;
    int rc = textfile_read(path, tf, &why);
    if (rc) { fprintf(stderr, "at0-eval: %s: %s\n", path, why); return 0; }
    refusal r = case_parse_validate(tf, c, &d);
    if (r != REFUSE_NONE) { fprintf(stderr, "AT0_CASE_REFUSED %s (%s)\n", refusal_name(r), d); return 0; }
    return 1;
}
static int cmd_shadow(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: at0-eval shadow <case>\n"); return 64; }
    textfile tf; at0_case c; shadow s; const char *why;
    if (!load_case(argv[0], &tf, &c)) return 2;
    if (!shadow_compute(&c, &s, &why)) { fprintf(stderr, "shadow: %s\n", why); return 2; }
    char rb[64]; rat_format(c.radius, rb, sizeof rb);
    printf("case %s\n|h| %s  kernel_dim %d  matched(e+,e-) %d %d  psi_nonzero %d  povm_exact_identity %d  povm_residual %.6Le\n",
           c.case_name, rb, s.kernel_dim, s.matched[0], s.matched[1], s.psi_nonzero, s.povm_exact_identity, s.povm_residual);
    printf("label  p(k)        P(X+)       P(Y+)       P(Z+)      | ref X+      ref Y+      ref Z+\n");
    for (int k = 0; k < c.M; k++)
        printf("%-6s %.9Lf %.9Lf %.9Lf %.9Lf | %.9Lf %.9Lf %.9Lf\n", c.label[k], s.psi_nonzero ? s.clock_p[k] : 0.0L,
               s.psi_nonzero ? s.pauli[k][0][0] : 0.0L, s.psi_nonzero ? s.pauli[k][1][0] : 0.0L, s.psi_nonzero ? s.pauli[k][2][0] : 0.0L,
               s.reference[k][0][0], s.reference[k][1][0], s.reference[k][2][0]);
    case_free(&c); textfile_free(&tf);
    return 0;
}
static int cmd_synth(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: at0-eval synth <case> [mutant]\n"); return 64; }
    textfile tf; at0_case c; const char *why;
    if (!load_case(argv[0], &tf, &c)) return 2;
    size_t cap = 1u << 22; char *buf = malloc(cap);
    long n = result_synth(&c, tf.bytes, tf.len, argc > 1 ? argv[1] : NULL, buf, cap, &why);
    if (n < 0) { fprintf(stderr, "synth: %s\n", why); return 2; }
    fwrite(buf, 1, (size_t)n, stdout);
    free(buf); case_free(&c); textfile_free(&tf);
    return 0;
}
static int cmd_mutants(void) { int n; const char *const *m = synth_mutant_names(&n); for (int i = 0; i < n; i++) printf("%s\n", m[i]); return 0; }

static const char *kv(int argc, char **argv, const char *key, const char *dflt) {
    size_t kl = strlen(key);
    for (int i = 0; i < argc; i++) if (!strncmp(argv[i], key, kl) && argv[i][kl] == '=') return argv[i] + kl + 1;
    return dflt;
}
static int gen_rat(const char *s, rat *out) { int canon; if (!rat_parse(s, out, &canon)) { fprintf(stderr, "gen: bad rational %s\n", s); return 0; } return 1; }
static int gen_scaled(const char *s, scaled *out) { int canon; if (!scaled_parse(s, out, &canon) || !canon) { fprintf(stderr, "gen: bad scaled %s\n", s); return 0; } return 1; }
static int cmd_gen(int argc, char **argv) {
    at0_case c; memset(&c, 0, sizeof c);
    snprintf(c.case_name, sizeof c.case_name, "%s", kv(argc, argv, "name", "at0-gen"));
    char buf[AT0E_MAX_LINE];
    /* clock energies */
    snprintf(buf, sizeof buf, "%s", kv(argc, argv, "E", "-3/2,-1/2,1/2,3/2"));
    c.N = 0; for (char *s = strtok(buf, ","); s; s = strtok(NULL, ",")) { if (c.N >= AT0E_MAX_N || !gen_rat(s, &c.E[c.N])) return 64; c.N++; }
    snprintf(buf, sizeof buf, "%s", kv(argc, argv, "h", "0/1,0/1,0/1,1/2"));
    { rat *hs[4] = { &c.h0, &c.hx, &c.hy, &c.hz }; int i = 0; for (char *s = strtok(buf, ","); s && i < 4; s = strtok(NULL, ","), i++) if (!gen_rat(s, hs[i])) return 64; if (i != 4) { fprintf(stderr, "gen: h needs 4 rationals\n"); return 64; } }
    snprintf(c.ref_label, sizeof c.ref_label, "%s", kv(argc, argv, "ref", "t0"));
    snprintf(buf, sizeof buf, "%s", kv(argc, argv, "psi", "(1/1;0/1),(1/1;0/1)"));
    { char *comma = strchr(buf, ','); int canon; if (!comma) { fprintf(stderr, "gen: psi\n"); return 64; } *comma = 0;
      if (!crat_parse(buf, &c.psi0[0], &canon) || !crat_parse(comma + 1, &c.psi0[1], &canon)) { fprintf(stderr, "gen: psi\n"); return 64; } }
    if (!gen_rat(kv(argc, argv, "tau", "1/4"), &c.tau) || !gen_rat(kv(argc, argv, "w", "1/1"), &c.w)) return 64;
    c.M = atoi(kv(argc, argv, "M", "4"));
    if (c.M < 1 || c.M > AT0E_MAX_M) { fprintf(stderr, "gen: M out of range\n"); return 64; }
    const char *labels = kv(argc, argv, "labels", NULL);
    if (labels) { snprintf(buf, sizeof buf, "%s", labels); int k = 0; for (char *s = strtok(buf, ","); s && k < c.M; s = strtok(NULL, ","), k++) snprintf(c.label[k], sizeof c.label[k], "%s", s); if (k != c.M) { fprintf(stderr, "gen: labels count\n"); return 64; } }
    else for (int k = 0; k < c.M; k++) snprintf(c.label[k], sizeof c.label[k], "%s%d", kv(argc, argv, "prefix", "t"), k);
    c.control_positive = strcmp(kv(argc, argv, "control", "POSITIVE"), "NEGATIVE") != 0;
    c.expected_pass = strcmp(kv(argc, argv, "expected", "PASS"), "FAIL") != 0;
    snprintf(buf, sizeof buf, "%s", kv(argc, argv, "codes", "none"));
    c.n_expected_codes = 0;
    if (strcmp(buf, "none")) for (char *s = strtok(buf, ","); s; s = strtok(NULL, ",")) { if (c.n_expected_codes >= 16) return 64; snprintf(c.expected_codes[c.n_expected_codes++], 48, "%s", s); }
    const char *bk = kv(argc, argv, "minbk", "ESTIMATED");
    c.min_bound_kind = !strcmp(bk, "RIGOROUS") ? BK_RIGOROUS : !strcmp(bk, "NONE") ? BK_NONE : BK_ESTIMATED;
    if (!gen_scaled(kv(argc, argv, "tolc", "1@12"), &c.tol_constraint) || !gen_scaled(kv(argc, argv, "tolp", "1@12"), &c.tol_povm) ||
        !gen_scaled(kv(argc, argv, "tolprob", "1@12"), &c.tol_prob) || !gen_scaled(kv(argc, argv, "tolz", "1@9"), &c.tol_zero) ||
        !gen_scaled(kv(argc, argv, "tols", "1@12"), &c.tol_schro)) return 64;
    size_t cap = 1u << 20; char *out = malloc(cap);
    long n = case_emit(&c, out, cap);
    if (n < 0) { fprintf(stderr, "gen: emit failed\n"); return 2; }
    fwrite(out, 1, (size_t)n, stdout);
    free(out);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: at0-eval <case|result|shadow|synth|mutants|gen> ...\n"); return 64; }
    if (!strcmp(argv[1], "case")) return cmd_case(argc - 2, argv + 2);
    if (!strcmp(argv[1], "result")) return cmd_result(argc - 2, argv + 2);
    if (!strcmp(argv[1], "shadow")) return cmd_shadow(argc - 2, argv + 2);
    if (!strcmp(argv[1], "synth")) return cmd_synth(argc - 2, argv + 2);
    if (!strcmp(argv[1], "mutants")) return cmd_mutants();
    if (!strcmp(argv[1], "gen")) return cmd_gen(argc - 2, argv + 2);
    fprintf(stderr, "unknown command %s\n", argv[1]);
    return 64;
}
