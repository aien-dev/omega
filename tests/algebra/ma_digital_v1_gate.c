/* MIXED_ALGEBRA_DIGITAL_V1 gate driver (spec/mixed-algebra-digital-v1.md).
 *
 * Runs the pre-registered selection queries through the EXISTING TURING Field
 * selector v1 using only public turing_* functions, checks the gate criteria
 * that live in the decisions, and writes the wrapper receipt JSON.
 * No timed runs here; timing comes from the stored bench receipts.
 *
 *   ma_digital_v1_gate digests FRESH1 FRESH2
 *       one line per query: "<id> <decision digest hex>" (reproduction)
 *   ma_digital_v1_gate receipt OUT.json FRESH1 FRESH2 CONT1 CONT2
 *       full gate over the stored receipts; environment (set by
 *       ma_digital_v1_gate.sh): DV1_COMMIT, DV1_TREE_DIRTY (0|1), DV1_MODE
 *       (fresh|reuse), DV1_RUN_ID, DV1_CORRECTNESS (file of summary lines,
 *       one "PASS|FAIL <label>: <line>" per command), DV1_REPRO_FILES
 *       (space-separated digest files from separate processes, "label=path"),
 *       DV1_UNTRACKED (file listing untracked paths at run time).
 *   Exit: 0 verdict PASS, 3 verdict FAIL (receipt still written), 2 error.
 */
#include "algebra/realize_common.h"
#include "turing/select.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *id, *what;
    uint64_t n, m;
    int pack;
    int expect_exact;
    int above_max_n; /* named-rejection query */
} dv1_query;

/* Pre-registered, spec section 5. Sparsity 0.30 for all. */
static const dv1_query Q[] = {
    {"Q1", "L2-resident (256 KiB int8 W)", 4096, 64, TURING_PACK_ONCE, 1, 0},
    {"Q2", "L2-resident (256 KiB int8 W)", 4096, 64, TURING_PACK_PER_CALL, 1, 0},
    {"Q3", "DRAM-bound (64 MiB int8 W)", 16384, 4096, TURING_PACK_ONCE, 1, 0},
    {"Q4", "DRAM-bound (64 MiB int8 W)", 16384, 4096, TURING_PACK_PER_CALL, 1, 0},
    {"Q5", "above RNS and sparse max_n", 65537, 64, TURING_PACK_ONCE, 0, 1},
    {"Q6", "above RNS and sparse max_n", 65537, 64, TURING_PACK_PER_CALL, 0, 1},
};
#define NQ (sizeof Q / sizeof Q[0])
#define SPARSITY_MILLI 300u

static const char *pack_name(int p) { return p == TURING_PACK_PER_CALL ? "per_call" : "once"; }

static int load(turing_store *st, const char *a, const char *b) {
    if (turing_ingest_registry(st) < 0) return -1;
    if (turing_ingest_receipt(st, a) <= 0) { fprintf(stderr, "cannot ingest %s\n", a); return -1; }
    if (turing_ingest_receipt(st, b) <= 0) { fprintf(stderr, "cannot ingest %s\n", b); return -1; }
    return 0;
}

static int select_q(const turing_store *st, size_t i, turing_decision *d, turing_digest *id) {
    turing_query q = {Q[i].n, Q[i].m, SPARSITY_MILLI, Q[i].pack, 0};
    int rc = turing_field_select(st, &q, NULL, d);
    if (rc < 0) return -1;
    if (turing_decision_digest(d, id) != 0) return -1;
    return rc;
}

static int is_eligible(turing_reason r) { return r == TURING_R_CHOSEN || r == TURING_R_RANKED || r == TURING_R_TIED; }

static const char *family_of(const char *rz) {
    const oma_rz_impl *im = oma_rz_find(rz);
    return im ? im->family : "?";
}

static int fam_class(const char *f) { /* 1 = R1 binary, 2 = R2 packed ternary, 3 = R3 residue, 0 other */
    if (!strcmp(f, "binary")) return 1;
    if (!strcmp(f, "bitplane") || !strcmp(f, "lut") || !strcmp(f, "crumb2") || !strcmp(f, "dense5")) return 2;
    if (!strcmp(f, "rns")) return 3;
    return 0;
}

/* ------------------------------------------------------------ small IO */

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    size_t r;
    while ((r = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += r;
        if (cap - len < 2) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
            cap *= 2;
        }
    }
    fclose(f);
    buf[len] = 0;
    return buf;
}

/* "key": "value" -> value; "key": true/false/number -> token. */
static int jget(const char *text, const char *key, char *out, size_t cap) {
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\": ", key);
    const char *p = strstr(text, pat);
    if (!p) return -1;
    p += strlen(pat);
    size_t k = 0;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"' && k + 1 < cap) out[k++] = *p++;
    } else {
        while (*p && *p != ',' && *p != '}' && *p != '\n' && k + 1 < cap) out[k++] = *p++;
    }
    out[k] = 0;
    return 0;
}

static void jstr(FILE *o, const char *s) {
    fputc('"', o);
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(o, "\\%c", c);
        else if (c < 0x20) fprintf(o, "\\u%04x", c);
        else fputc(c, o);
    }
    fputc('"', o);
}

typedef struct {
    const char *path, *role;
    char hex[65], schema[96], run_id[128], run_commit[48], tree_dirty[8], mismatches[24];
} rinfo;

static int receipt_info(rinfo *r) {
    turing_digest d;
    if (turing_file_digest(r->path, &d) != 0) return -1;
    turing_hex(&d, r->hex);
    char *t = slurp(r->path);
    if (!t) return -1;
    strcpy(r->schema, "?"), strcpy(r->run_id, "?"), strcpy(r->run_commit, "?");
    strcpy(r->tree_dirty, "?"), strcpy(r->mismatches, "?");
    (void)jget(t, "schema", r->schema, sizeof r->schema);
    (void)jget(t, "run_id", r->run_id, sizeof r->run_id);
    (void)jget(t, "run_commit", r->run_commit, sizeof r->run_commit);
    (void)jget(t, "tree_dirty", r->tree_dirty, sizeof r->tree_dirty);
    const char *c = strstr(t, "\"correctness\": {");
    if (c) (void)jget(c, "mismatches", r->mismatches, sizeof r->mismatches);
    free(t);
    return 0;
}

/* ------------------------------------------------------------- digests */

static int print_digests(FILE *o, const turing_store *st) {
    for (size_t i = 0; i < NQ; ++i) {
        turing_decision d;
        turing_digest id;
        char h[65];
        if (select_q(st, i, &d, &id) < 0) return -1;
        turing_hex(&id, h);
        fprintf(o, "%s %s\n", Q[i].id, h);
    }
    return 0;
}

/* Mean per-call cost of spec k in footprint (cn, cm) from one receipt. */
static double receipt_cost(const turing_store *st, const turing_digest *spec, const turing_digest *rd, uint64_t cn,
                           uint64_t cm, int pack) {
    double s = 0;
    unsigned c = 0;
    for (size_t i = 0; i < st->nev; ++i) {
        const turing_evidence *e = &st->ev[i];
        if (e->n != cn || e->m != cm || !turing_digest_eq(&e->spec_id, spec) || !turing_digest_eq(&e->receipt_digest, rd))
            continue;
        s += (double)turing_ev_cost(e, pack);
        c++;
    }
    return c ? s / c : -1;
}

static uint64_t weight_bytes_of(const turing_store *st, const turing_digest *spec, uint64_t cn, uint64_t cm) {
    for (size_t i = 0; i < st->nev; ++i) {
        const turing_evidence *e = &st->ev[i];
        if (e->n == cn && e->m == cm && e->sparsity_milli == SPARSITY_MILLI && turing_digest_eq(&e->spec_id, spec))
            return e->weight_bytes;
    }
    return 0;
}

#define FAILS_MAX 64
static char g_fails[FAILS_MAX][200];
static size_t g_nfail;
static void fail(const char *fmt, const char *a, const char *b) {
    if (g_nfail < FAILS_MAX) snprintf(g_fails[g_nfail], sizeof g_fails[0], fmt, a, b);
    g_nfail++;
}

static int do_receipt(const char *out, const char *f1, const char *f2, const char *c1, const char *c2) {
    const char *commit = getenv("DV1_COMMIT"), *dirty = getenv("DV1_TREE_DIRTY"), *mode = getenv("DV1_MODE"),
               *run_id = getenv("DV1_RUN_ID"), *corr = getenv("DV1_CORRECTNESS"), *repro = getenv("DV1_REPRO_FILES"),
               *untracked = getenv("DV1_UNTRACKED");
    if (!commit || !dirty || !mode || !run_id || !corr) {
        fprintf(stderr, "missing DV1_* environment\n");
        return 2;
    }
    int tree_dirty = strcmp(dirty, "0") != 0;
    turing_store *st = turing_store_new(), *st2 = turing_store_new(), *stc = turing_store_new();
    if (!st || !st2 || !stc) return 2;
    if (load(st, f1, f2) || load(st2, f1, f2) || load(stc, c1, c2)) return 2;

    rinfo ri[4] = {{f1, "primary", {0}, {0}, {0}, {0}, {0}, {0}},
                   {f2, "primary", {0}, {0}, {0}, {0}, {0}, {0}},
                   {c1, "continuity", {0}, {0}, {0}, {0}, {0}, {0}},
                   {c2, "continuity", {0}, {0}, {0}, {0}, {0}, {0}}};
    for (size_t r = 0; r < 4; ++r)
        if (receipt_info(&ri[r]) != 0) return 2;

    /* criterion 1 (bench part) and 2 (fresh receipts at HEAD, clean) */
    if (tree_dirty) fail("tree_dirty is true%s%s", "", "");
    for (size_t r = 0; r < 2; ++r) {
        if (strcmp(ri[r].mismatches, "0")) fail("fresh receipt %s reports mismatches=%s", ri[r].path, ri[r].mismatches);
        if (strcmp(ri[r].run_commit, commit)) fail("fresh receipt %s run_commit %s is not HEAD", ri[r].path, ri[r].run_commit);
        if (strcmp(ri[r].tree_dirty, "false")) fail("fresh receipt %s tree_dirty=%s", ri[r].path, ri[r].tree_dirty);
    }
    /* correctness file: every line must start with PASS */
    char *ctext = slurp(corr);
    if (!ctext) return 2;
    for (char *l = ctext; *l;) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        if (*l && strncmp(l, "PASS ", 5)) fail("correctness: %s%s", l, "");
        if (!nl) break;
        *nl = '\n';
        l = nl + 1;
    }

    turing_decision D[NQ], D2[NQ], DC[NQ];
    turing_digest ID[NQ], ID2[NQ], IDC[NQ];
    int rc[NQ], verify_ok[NQ], cites_fresh[NQ], repro_in[NQ];
    char why[NQ][200];
    size_t nrej = 0;
    for (size_t i = 0; i < NQ; ++i) {
        rc[i] = select_q(st, i, &D[i], &ID[i]);
        int rc2 = select_q(st2, i, &D2[i], &ID2[i]);
        int rcc = select_q(stc, i, &DC[i], &IDC[i]);
        if (rc[i] < 0 || rc2 < 0 || rcc < 0) return 2;
        repro_in[i] = rc[i] == rc2 && turing_digest_eq(&ID[i], &ID2[i]);
        if (!repro_in[i]) fail("%s: in-process re-selection digest differs%s", Q[i].id, "");
        if (rc[i] != 0 || D[i].chosen < 0) fail("%s: no realization chosen (verdict %s)", Q[i].id, D[i].verdict);
        why[i][0] = 0;
        verify_ok[i] = turing_decision_verify(st, &D[i], why[i], sizeof why[i]) == 0;
        if (!verify_ok[i]) fail("%s: decision verify failed: %s", Q[i].id, why[i]);
        cites_fresh[i] = D[i].ncite > 0;
        for (size_t c = 0; c < D[i].ncite; ++c) {
            int e = turing_find_evidence(st, &D[i].cite[c]);
            char h[65] = "";
            if (e >= 0) turing_hex(&st->ev[e].receipt_digest, h);
            if (e < 0 || (strcmp(h, ri[0].hex) && strcmp(h, ri[1].hex))) cites_fresh[i] = 0;
        }
        if (!cites_fresh[i]) fail("%s: a cited evidence row is not from a fresh receipt%s", Q[i].id, "");
        if ((D[i].exact_cell != 0) != (Q[i].expect_exact != 0))
            fail("%s: exact_cell differs from pre-registration (%s)", Q[i].id, D[i].exact_cell ? "exact" : "nearest");
        /* families */
        char fams[TURING_MAX_CAND][32];
        size_t nf = 0, nel = 0;
        int cls[4] = {0};
        for (size_t k = 0; k < D[i].ncand; ++k) {
            if (D[i].reason[k] == TURING_R_MAX_N) nrej++;
            if (!is_eligible(D[i].reason[k])) {
                int expected = Q[i].above_max_n && D[i].reason[k] == TURING_R_MAX_N &&
                               (!strcmp(D[i].cand_rz[k], "R3_sparse") || !strcmp(D[i].cand_rz[k], "R4_rns"));
                if (!expected)
                    fail("%s: unexpected filter on %s", Q[i].id, D[i].cand_rz[k]);
                continue;
            }
            nel++;
            const char *f = family_of(D[i].cand_rz[k]);
            cls[fam_class(f)] = 1;
            size_t j = 0;
            while (j < nf && strcmp(fams[j], f)) ++j;
            if (j == nf) snprintf(fams[nf++], sizeof fams[0], "%s", f);
        }
        if (nf < 3) fail("%s: verified-eligible set spans fewer than 3 families%s", Q[i].id, "");
        if (!Q[i].above_max_n && (nel != oma_rz_count() || !cls[1] || !cls[2] || !cls[3]))
            fail("%s: not all realizations / classes R1,R2,R3 verified-eligible%s", Q[i].id, "");
        if (Q[i].above_max_n) {
            size_t mx = 0;
            for (size_t k = 0; k < D[i].ncand; ++k) mx += D[i].reason[k] == TURING_R_MAX_N;
            if (mx != 2 || nel != oma_rz_count() - 2) fail("%s: expected exactly R3_sparse and R4_rns MAX_N%s", Q[i].id, "");
        }
    }
    if (nrej == 0) fail("no named rejection%s%s", "", "");

    /* separate-process reproduction files */
    char repro_json[1024] = "";
    size_t rl = 0;
    int repro_all = 1;
    {
        char mine[NQ * 80] = "";
        size_t ml = 0;
        for (size_t i = 0; i < NQ; ++i) {
            char h[65];
            turing_hex(&ID[i], h);
            ml += (size_t)snprintf(mine + ml, sizeof mine - ml, "%s %s\n", Q[i].id, h);
        }
        char list[512];
        snprintf(list, sizeof list, "%s", repro ? repro : "");
        size_t nfiles = 0;
        for (char *tok = strtok(list, " "); tok; tok = strtok(NULL, " ")) {
            char *eq = strchr(tok, '=');
            if (!eq) return 2;
            *eq = 0;
            char *t = slurp(eq + 1);
            int same = t && !strcmp(t, mine);
            free(t);
            if (!same) { repro_all = 0; fail("separate-process reproduction '%s' differs%s", tok, ""); }
            rl += (size_t)snprintf(repro_json + rl, sizeof repro_json - rl, "%s\"%s\": \"%s\"", nfiles ? ", " : "", tok,
                                   same ? "identical" : "DIFFERS");
            nfiles++;
        }
        if (nfiles < 2) { repro_all = 0; fail("fewer than 2 separate-process reproductions%s%s", "", ""); }
    }

    int pass = g_nfail == 0;
    FILE *o = fopen(out, "wx");
    if (!o) { perror(out); return 2; }
    char h[65];
    turing_hex(&st->contract_digest, h);
    fprintf(o, "{\n  \"schema\": \"OMEGA_MIXED_ALGEBRA_DIGITAL_V1_GATE_V1\",\n  \"gate\": \"MIXED_ALGEBRA_DIGITAL_V1\",\n");
    fprintf(o, "  \"spec\": \"spec/mixed-algebra-digital-v1.md\",\n  \"run_commit\": ");
    jstr(o, commit);
    fprintf(o, ",\n  \"tree_dirty\": %s,\n  \"mode\": ", tree_dirty ? "true" : "false");
    jstr(o, mode);
    fprintf(o, ",\n  \"bench_run_id\": ");
    jstr(o, run_id);
    fprintf(o, ",\n  \"contract_digest\": \"%s\",\n  \"selector\": \"%s\",\n", h, TURING_FIELD_SELECTOR);
    fprintf(o, "  \"energy\": \"not measured by this gate; bench energy rows are not ingested; pending omega#86\",\n");
    fprintf(o, "  \"receipts\": [\n");
    for (size_t r = 0; r < 4; ++r) {
        fprintf(o, "    {\"role\": \"%s\", \"path\": ", ri[r].role);
        jstr(o, ri[r].path);
        fprintf(o, ", \"sha256\": \"%s\", \"schema\": ", ri[r].hex);
        jstr(o, ri[r].schema);
        fprintf(o, ", \"run_id\": ");
        jstr(o, ri[r].run_id);
        fprintf(o, ", \"run_commit\": ");
        jstr(o, ri[r].run_commit);
        fprintf(o, ", \"tree_dirty\": ");
        jstr(o, ri[r].tree_dirty);
        fprintf(o, ", \"mismatches\": ");
        jstr(o, ri[r].mismatches);
        fprintf(o, "}%s\n", r + 1 < 4 ? "," : "");
    }
    fprintf(o, "  ],\n  \"correctness\": [\n");
    {
        size_t first = 1;
        for (char *l = ctext; *l;) {
            char *nl = strchr(l, '\n');
            if (nl) *nl = 0;
            if (*l) {
                fprintf(o, "%s    ", first ? "" : ",\n");
                jstr(o, l);
                first = 0;
            }
            if (!nl) break;
            l = nl + 1;
        }
        fprintf(o, "\n  ],\n");
    }
    fprintf(o, "  \"untracked_at_run\": [");
    if (untracked) {
        char *u = slurp(untracked);
        size_t first = 1;
        for (char *l = u; l && *l;) {
            char *nl = strchr(l, '\n');
            if (nl) *nl = 0;
            if (*l) {
                fprintf(o, "%s", first ? "" : ", ");
                jstr(o, l);
                first = 0;
            }
            if (!nl) break;
            l = nl + 1;
        }
        free(u);
    }
    fprintf(o, "],\n  \"queries\": [\n");
    for (size_t i = 0; i < NQ; ++i) {
        const turing_decision *d = &D[i];
        char id[65], idc[65];
        turing_hex(&ID[i], id);
        turing_hex(&IDC[i], idc);
        int ch = d->chosen, run = -1;
        for (size_t k = 0; k < d->ncand; ++k)
            if (is_eligible(d->reason[k]) && (int)k != ch && (run < 0 || d->cost_ps[k] < d->cost_ps[run])) run = (int)k;
        double c1 = -1, c2 = -1;
        if (ch >= 0) {
            turing_digest rd1, rd2;
            (void)turing_file_digest(f1, &rd1);
            (void)turing_file_digest(f2, &rd2);
            c1 = receipt_cost(st, &d->cand[ch], &rd1, d->cell_n, d->cell_m, d->pack);
            c2 = receipt_cost(st, &d->cand[ch], &rd2, d->cell_n, d->cell_m, d->pack);
        }
        double rdiff = (c1 > 0 && c2 > 0) ? (c1 > c2 ? c1 / c2 - 1 : c2 / c1 - 1) : -1;
        fprintf(o, "    {\"id\": \"%s\", \"shape\": ", Q[i].id);
        jstr(o, Q[i].what);
        fprintf(o, ", \"n\": %" PRIu64 ", \"m\": %" PRIu64 ", \"sparsity\": 0.30, \"pack\": \"%s\",\n", Q[i].n, Q[i].m,
                pack_name(Q[i].pack));
        fprintf(o, "     \"decision_digest\": \"%s\", \"verdict\": \"%s\", \"cell\": \"%s\", \"cell_n\": %" PRIu64
                   ", \"cell_m\": %" PRIu64 ",\n",
                id, d->verdict, d->exact_cell ? "exact" : "nearest", d->cell_n, d->cell_m);
        fprintf(o, "     \"chosen\": \"%s\", \"chosen_family\": \"%s\", \"runner_up\": \"%s\", \"margin_pct\": %.2f, "
                   "\"noise_band_pct\": %.2f, \"margin_inside_band\": %s,\n",
                ch >= 0 ? d->cand_rz[ch] : "none", ch >= 0 ? family_of(d->cand_rz[ch]) : "none",
                run >= 0 ? d->cand_rz[run] : "none", d->margin_ppm / 1e4, d->band_ppm / 1e4,
                d->margin_ppm <= d->band_ppm ? "true" : "false");
        fprintf(o, "     \"winner_cost_ns_run1\": %.3f, \"winner_cost_ns_run2\": %.3f, \"winner_run1_vs_run2_pct\": %.2f,\n",
                c1 / 1e3, c2 / 1e3, rdiff * 100);
        fprintf(o, "     \"verify\": \"%s\", \"cites\": %zu, \"cites_fresh_only\": %s, \"reproduced_in_process\": %s,\n",
                verify_ok[i] ? "all cited receipts verify" : why[i], d->ncite, cites_fresh[i] ? "true" : "false",
                repro_in[i] ? "true" : "false");
        fprintf(o, "     \"continuity_ma3_decision_digest\": \"%s\", \"continuity_ma3_chosen\": \"%s\", "
                   "\"continuity_agrees\": %s,\n",
                idc, DC[i].chosen >= 0 ? DC[i].cand_rz[DC[i].chosen] : "none",
                (DC[i].chosen >= 0 && ch >= 0 && !strcmp(DC[i].cand_rz[DC[i].chosen], d->cand_rz[ch])) ? "true" : "false");
        fprintf(o, "     \"candidates\": [\n");
        for (size_t k = 0; k < d->ncand; ++k) {
            char sh[65];
            turing_hex(&d->cand[k], sh);
            fprintf(o, "       {\"rz\": \"%s\", \"family\": \"%s\", \"spec\": \"%s\", \"reason\": \"%s\", "
                       "\"mean_cost_ns\": %.3f, \"weight_bytes\": %" PRIu64 "}%s\n",
                    d->cand_rz[k], family_of(d->cand_rz[k]), sh, turing_reason_name(d->reason[k]),
                    d->cost_ps[k] / 1e3, weight_bytes_of(st, &d->cand[k], d->cell_n, d->cell_m),
                    k + 1 < d->ncand ? "," : "");
        }
        fprintf(o, "     ]}%s\n", i + 1 < NQ ? "," : "");
    }
    fprintf(o, "  ],\n  \"named_rejections\": %zu,\n  \"reproduction_separate_process\": {%s},\n", nrej, repro_json);
    fprintf(o, "  \"reproduction_all_identical\": %s,\n  \"failed_checks\": [", repro_all ? "true" : "false");
    for (size_t f = 0; f < g_nfail && f < FAILS_MAX; ++f) {
        fprintf(o, "%s", f ? ", " : "");
        jstr(o, g_fails[f]);
    }
    fprintf(o, "],\n  \"verdict\": \"MIXED_ALGEBRA_DIGITAL_V1 %s\"\n}\n", pass ? "PASS" : "FAIL");
    fclose(o);
    free(ctext);
    for (size_t f = 0; f < g_nfail && f < FAILS_MAX; ++f) fprintf(stderr, "FAILED CHECK: %s\n", g_fails[f]);
    printf("MIXED_ALGEBRA_DIGITAL_V1 %s (%zu failed checks)\n", pass ? "PASS" : "FAIL", g_nfail);
    turing_store_free(st);
    turing_store_free(st2);
    turing_store_free(stc);
    return pass ? 0 : 3;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "digests")) {
        turing_store *st = turing_store_new();
        if (!st || load(st, argv[2], argv[3]) || print_digests(stdout, st)) return 2;
        turing_store_free(st);
        return 0;
    }
    if (argc == 7 && !strcmp(argv[1], "receipt")) return do_receipt(argv[2], argv[3], argv[4], argv[5], argv[6]);
    fprintf(stderr, "usage: %s digests FRESH1 FRESH2 | receipt OUT FRESH1 FRESH2 CONT1 CONT2\n", argv[0]);
    return 2;
}
