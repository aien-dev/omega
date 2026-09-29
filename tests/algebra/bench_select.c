/* MA-2 selector run: reads MA-2 bench receipts, decides every measured cell
 * in both pack modes (per run and over all runs), checks that the per-run
 * choices agree or are recorded ties, and writes the decision receipt.
 * STAND-IN selector, not wired to rx_costmodel.
 * Usage: bench_select OUT.json RUN1.json [RUN2.json ...] */
#include "algebra/oma_select.h"
#include "algebra/realize_common.h"
#include "sha256.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static oma_sel_table g_t;

static void file_sha256(const char *path, char hex[65]) {
    FILE *f = fopen(path, "rb");
    sha256_ctx c;
    uint8_t d[32], buf[8192];
    size_t k;
    sha256_init(&c);
    if (f) {
        while ((k = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&c, buf, k);
        fclose(f);
    }
    sha256_final(&c, d);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
}

typedef struct { size_t n, m; double sp; } cell;

static int is_binary(const char *id) { return strncmp(id, "R1_", 3) == 0 && strcmp(id, "R1_plain") != 0; }
static int is_ternary(const char *id) {
    return strncmp(id, "R2", 2) == 0 || strncmp(id, "R3", 2) == 0 || strncmp(id, "R5", 2) == 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s OUT.json RUN1.json [RUN2.json ...]\n", argv[0]); return 2; }
    oma_sel_init(&g_t);
    for (int i = 2; i < argc; i++) {
        int rc = oma_sel_load(&g_t, argv[i]);
        if (rc) { fprintf(stderr, "load %s: rc %d\n", argv[i], rc); return 2; }
    }
    cell cells[256];
    size_t nc = 0;
    for (size_t i = 0; i < g_t.nrows; i++) {
        const oma_sel_row *r = &g_t.rows[i];
        size_t k;
        for (k = 0; k < nc; k++)
            if (cells[k].n == r->n && cells[k].m == r->m && fabs(cells[k].sp - r->sparsity) < 1e-9) break;
        if (k == nc && nc < 256) cells[nc++] = (cell){r->n, r->m, r->sparsity};
    }
    FILE *o = fopen(argv[1], "w");
    if (!o) { perror(argv[1]); return 2; }
    fprintf(o, "{\n  \"schema\": \"OMEGA_MIXED_ALGEBRA_MA2_SELECT_V1\",\n");
    fprintf(o, "  \"label\": \"stand-in selector, not wired to rx_costmodel\",\n");
    fprintf(o, "  \"operation\": \"Omega-X: y = W.x, W ternary m x n, x int8, y int32, exact\",\n");
    fprintf(o, "  \"policy\": \"exact-contract filter (registry exact, verified in every run used, n <= max_n); cost = mean over runs of median ns per call (+ median pack ns when packing per call); noise band = max over winner and runner-up of within-run (q75-q25)/median and across-run (max-min)/mean; tied set = every eligible candidate within the band of the cheapest; TIE when the tied set has more than one member; a TIE is resolved only inside the tied set: incumbent if in it, else the digital reference realization (" OMA_SEL_REFERENCE ") if in it, else the cheapest member (ADR 0019 section 9.1, amended); these decisions have no incumbent\",\n");
    fprintf(o, "  \"sources\": [");
    for (int r = 0; r < g_t.nruns; r++) {
        char hex[65];
        file_sha256(g_t.source[r], hex);
        fprintf(o, "%s{\"path\": \"%s\", \"run_id\": \"%s\", \"sha256\": \"%s\"}", r ? ", " : "", g_t.source[r],
                g_t.run_id[r], hex);
    }
    fprintf(o, "],\n");

    /* reproducibility: per-run decisions */
    unsigned agree = 0, tie_ok = 0, disagree = 0, total = 0;
    fprintf(o, "  \"reproducibility\": {\"runs\": %d, \"cells\": [\n", g_t.nruns);
    int first = 1;
    if (g_t.nruns >= 2) {
        for (size_t k = 0; k < nc; k++)
            for (int mode = 0; mode < 2; mode++) {
                oma_sel_decision a, b;
                oma_sel_query qa = {cells[k].n, cells[k].m, cells[k].sp, mode, 0, NULL};
                oma_sel_query qb = qa;
                qb.run = 1;
                if (oma_sel_decide(&g_t, &qa, &a) || oma_sel_decide(&g_t, &qb, &b)) continue;
                const char *st;
                total++;
                if (strcmp(a.chosen, b.chosen) == 0) { agree++; st = "agree"; }
                else if ((a.tie && oma_sel_in_tie_set(&a, b.chosen)) || (b.tie && oma_sel_in_tie_set(&b, a.chosen))) {
                    tie_ok++;
                    st = "recorded_tie";
                } else { disagree++; st = "DISAGREE"; }
                fprintf(o, "%s    {\"n\": %zu, \"m\": %zu, \"sparsity\": %.2f, \"pack\": \"%s\", \"run1\": \"%s\", "
                           "\"run1_verdict\": \"%s\", \"run1_margin\": %.4f, \"run1_band\": %.4f, \"run2\": \"%s\", "
                           "\"run2_verdict\": \"%s\", \"run2_margin\": %.4f, \"run2_band\": %.4f, \"status\": \"%s\"}",
                        first ? "" : ",\n", cells[k].n, cells[k].m, cells[k].sp, mode ? "per_call" : "once_amortized",
                        a.chosen, a.tie ? "TIE" : "CHOSEN", a.margin_rel, a.noise_band_rel, b.chosen,
                        b.tie ? "TIE" : "CHOSEN", b.margin_rel, b.noise_band_rel, st);
                first = 0;
                if (strcmp(st, "DISAGREE") == 0)
                    printf("DISAGREE n=%zu m=%zu sp=%.1f %s: run1 %s (margin %.3f band %.3f) run2 %s (margin %.3f band %.3f)\n",
                           cells[k].n, cells[k].m, cells[k].sp, mode ? "per_call" : "amortized", a.chosen,
                           a.margin_rel, a.noise_band_rel, b.chosen, b.margin_rel, b.noise_band_rel);
            }
    }
    fprintf(o, "\n  ], \"decisions\": %u, \"agree\": %u, \"recorded_ties\": %u, \"disagree\": %u},\n", total, agree,
            tie_ok, disagree);

    unsigned ties_total = 0, ties_per_call = 0, ties_once = 0, ndec = 0, ref_picks = 0;
    unsigned res_inc = 0, res_ref = 0, res_cheap = 0, outside = 0;
    /* decisions over all runs + win/lose table */
    fprintf(o, "  \"decisions\": [\n");
    first = 1;
    printf("%-6s %-5s %-4s | %-12s %-6s | %-12s %-6s | %-10s %-6s | %s\n", "n", "m", "sp", "amortized", "", "per_call",
           "", "R1 best", "%floor", "ternary best / binary best (amortized)");
    fprintf(stdout, "-----------------------------------------------------------------------------------------------\n");
    char winlose[256][512];
    for (size_t k = 0; k < nc; k++) {
        oma_sel_decision d[2];
        for (int mode = 0; mode < 2; mode++) {
            oma_sel_query q = {cells[k].n, cells[k].m, cells[k].sp, mode, -1, NULL};
            if (oma_sel_decide(&g_t, &q, &d[mode])) continue;
            ndec++;
            if (d[mode].tie) { ties_total++; if (mode) ties_per_call++; else ties_once++; }
            if (strcmp(d[mode].chosen, OMA_SEL_REFERENCE) == 0) ref_picks++;
            if (d[mode].tie) {
                const char *tr = d[mode].tie_resolution;
                if (strcmp(tr, "incumbent") == 0) res_inc++;
                else if (strcmp(tr, "reference") == 0) res_ref++;
                else if (strcmp(tr, "cheapest") == 0) res_cheap++;
            }
            int in = 0;
            for (size_t i = 0; i < d[mode].ntie; i++) in |= strcmp(d[mode].tie_set[i], d[mode].chosen) == 0;
            if (!in) outside++;
            fprintf(o, "%s", first ? "" : ",\n");
            oma_sel_decision_json(o, &d[mode], "    ");
            first = 0;
        }
        /* binary vs ternary families (amortized) */
        const oma_sel_candidate *bb = NULL, *tb = NULL;
        for (size_t c = 0; c < d[0].ncand; c++) {
            const oma_sel_candidate *x = &d[0].cand[c];
            if (!x->eligible) continue;
            if (is_binary(x->rz) && (!bb || x->cost_ns < bb->cost_ns)) bb = x;
            if (is_ternary(x->rz) && (!tb || x->cost_ns < tb->cost_ns)) tb = x;
        }
        double ratio = (bb && tb) ? bb->cost_ns / tb->cost_ns : 0;
        double band = 0;
        if (bb && tb) {
            band = fmax(fmax(bb->within_noise_rel, tb->within_noise_rel), fmax(bb->cross_run_rel, tb->cross_run_rel));
        }
        const char *verdict = !(bb && tb) ? "n/a"
                              : fabs(ratio - 1.0) <= band ? "tie"
                              : ratio > 1.0 ? "ternary wins" : "binary wins";
        snprintf(winlose[k], sizeof winlose[k],
                 "{\"n\": %zu, \"m\": %zu, \"sparsity\": %.2f, \"best_binary\": \"%s\", \"binary_ns\": %.1f, "
                 "\"binary_pct_floor\": %.1f, \"best_ternary\": \"%s\", \"ternary_ns\": %.1f, \"ternary_pct_floor\": %.1f, "
                 "\"binary_over_ternary\": %.3f, \"band\": %.4f, \"verdict\": \"%s\"}",
                 cells[k].n, cells[k].m, cells[k].sp, bb ? bb->rz : "-", bb ? bb->cost_ns : 0, bb ? bb->pct_floor : 0,
                 tb ? tb->rz : "-", tb ? tb->cost_ns : 0, tb ? tb->pct_floor : 0, ratio, band, verdict);
        printf("%-6zu %-5zu %-4.1f | %-12s %-6s | %-12s %-6s | %-10s %5.0f%% | %.2fx %s (%s)\n", cells[k].n, cells[k].m,
               cells[k].sp, d[0].chosen, d[0].tie ? "TIE" : "", d[1].chosen, d[1].tie ? "TIE" : "", bb ? bb->rz : "-",
               bb ? bb->pct_floor : 0, ratio, verdict, tb ? tb->rz : "-");
    }
    fprintf(o, "\n  ],\n  \"decision_summary\": {\"decisions\": %u, \"ties\": %u, \"ties_per_call\": %u, "
               "\"ties_once_amortized\": %u, \"chosen_reference\": %u, \"tie_resolution\": {\"incumbent\": %u, "
               "\"reference\": %u, \"cheapest\": %u}, \"chosen_outside_tied_set\": %u},\n",
            ndec, ties_total, ties_per_call, ties_once, ref_picks, res_inc, res_ref, res_cheap, outside);
    fprintf(o, "  \"binary_vs_ternary\": [\n");
    for (size_t k = 0; k < nc; k++) fprintf(o, "    %s%s\n", winlose[k], k + 1 < nc ? "," : "");
    fprintf(o, "  ],\n");

    /* off-grid queries: nearest measured cell, contract filter by n */
    static const oma_sel_query extra[] = {
        {2048, 256, 0.5, 0, -1, NULL}, {16384, 2048, 0.0, 1, -1, NULL}, {100000, 64, 0.0, 0, -1, NULL}, {1024, 1, 0.95, 0, -1, NULL}};
    fprintf(o, "  \"off_grid_queries\": [\n");
    for (size_t i = 0; i < sizeof extra / sizeof extra[0]; i++) {
        oma_sel_decision d;
        if (oma_sel_decide(&g_t, &extra[i], &d)) continue;
        oma_sel_decision_json(o, &d, "    ");
        fprintf(o, "%s\n", i + 1 < sizeof extra / sizeof extra[0] ? "," : "");
    }
    fprintf(o, "  ],\n");
    const char *gate = g_t.nruns < 2 ? "NOT_RUN" : (disagree == 0 && outside == 0) ? "PASS" : "FAIL";
    fprintf(o, "  \"gates\": {\"MA2_SELECTOR_REPRO\": \"%s\", \"criteria\": \"for every measured cell and both pack modes, the choices from run 1 alone and run 2 alone are equal, or one run records a TIE whose tied set contains the other run's choice; and every decision's chosen realization is a member of its tied set\"},\n", gate);
    fprintf(o, "  \"not_claimed\": [\"integration with rx_costmodel or the resident omega.select reaction\", \"shapes between grid points beyond nearest-cell lookup\", \"multi-core or GPU realizations\", \"energy-based selection\"]\n}\n");
    fclose(o);
    printf("decisions over all runs: %u, TIE %u (per_call %u, once_amortized %u), %s chosen %u\n", ndec, ties_total,
           ties_per_call, ties_once, OMA_SEL_REFERENCE, ref_picks);
    printf("tie resolution: incumbent %u, reference %u, cheapest %u; chosen outside tied set: %u\n", res_inc, res_ref,
           res_cheap, outside);
    printf("reproducibility: %u decisions, %u agree, %u recorded ties, %u disagree -> MA2_SELECTOR_REPRO %s\n", total,
           agree, tie_ok, disagree, gate);
    return 0;
}
