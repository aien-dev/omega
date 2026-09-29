/* turing-field: TURING Field V0 view over stored receipts (no timed runs).
 *
 *   turing-field [receipt.json ...]   history view, winners, selector comparison
 *   turing-field --spec-ids           contract digest + spec ids (rebuild check)
 *
 * Default receipts: evidence/MIXED_ALGEBRA/ma3_bench_run{1,2}.json.
 * docs/turing/TURING_W0_PROPOSAL.md sections F, K. */
#include "turing/select.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const default_receipts[] = {"evidence/MIXED_ALGEBRA/ma3_bench_run1.json",
                                               "evidence/MIXED_ALGEBRA/ma3_bench_run2.json"};

/* Pre-registered shapes (proposal K.4), sparsity 0.30. */
static const struct {
    const char *name, *where;
    uint64_t n, m;
} shapes[4] = {
    {"S1", "L2-fit (256 KiB int8 W)", 4096, 64},
    {"S2", "L2 edge (1 MiB int8 W)", 16384, 64},
    {"S3", "L3/DRAM (16 MiB int8 W)", 4096, 4096},
    {"S4", "DRAM-bound (64 MiB int8 W)", 16384, 4096},
};

static void print_decision(const turing_store *st, const turing_decision *d, const turing_digest *id) {
    char h[65], c[13];
    turing_hex(id, h);
    printf("decision %s\n", h);
    if (d->has_supersedes) {
        turing_hex_short(&d->supersedes, c);
        printf("    supersedes  %s\n", c);
    } else {
        printf("    supersedes  none (root)\n");
    }
    turing_hex_short(&d->contract_digest, c);
    printf("    selector    %s   contract %s (PROVISIONAL)\n", d->selector, c);
    printf("    query       n=%" PRIu64 " m=%" PRIu64 " sparsity=%.2f pack=%s  cell %s\n", d->n, d->m,
           d->sparsity_milli / 1000.0, d->pack ? "per_call" : "once", d->exact_cell ? "exact" : "nearest");
    if (d->chosen >= 0) {
        turing_hex_short(&d->cand[d->chosen], c);
        printf("    chosen      %s (spec %s)  %s  margin %.1f%%  band %.1f%%  tie=%s\n", d->cand_rz[d->chosen], c,
               d->verdict, d->margin_ppm / 1e4, d->band_ppm / 1e4, d->tie_resolution);
    }
    printf("    candidates ");
    for (size_t i = 0; i < d->ncand; ++i)
        printf(" %s:%s", d->cand_rz[i], turing_reason_name(d->reason[i]));
    printf("\n");
    char why[200];
    int v = turing_decision_verify(st, d, why, sizeof why);
    printf("    cites       %zu evidence records over %zu receipts -> %s\n", d->ncite, st->nreceipt,
           v == 0 ? "all receipts verify" : why);
    for (size_t r = 0; r < st->nreceipt; ++r) {
        turing_hex_short(&st->receipt[r].digest, c);
        printf("                receipt %s  %s\n", c, st->receipt[r].path);
    }
}

static int load(turing_store *st, const char *const *paths, size_t np) {
    if (turing_ingest_registry(st) < 0) return -1;
    for (size_t i = 0; i < np; ++i)
        if (turing_ingest_receipt(st, paths[i]) < 0) {
            fprintf(stderr, "turing-field: cannot ingest %s\n", paths[i]);
            return -1;
        }
    return 0;
}

static void print_regret(const char *label, const turing_regret *r, int checked) {
    char fc[32] = "n/a", hc[32] = "n/a";
    if (checked) {
        snprintf(fc, sizeof fc, "%zu/%zu", r->field_cites_ok, r->decisions);
        snprintf(hc, sizeof hc, "%zu/%zu", r->hist_cites_ok, r->decisions);
    }
    printf("  %-40s n=%3zu | Field regret mean %6.2f%% max %6.2f%% receipts-verify %-7s | control mean %6.2f%% "
           "max %8.2f%% receipts-verify %-7s | same pick %zu\n",
           label, r->decisions, 100 * r->field_mean, 100 * r->field_max, fc, 100 * r->hist_mean, 100 * r->hist_max,
           hc, r->agree);
}

int main(int argc, char **argv) {
    int spec_ids = argc > 1 && !strcmp(argv[1], "--spec-ids");
    const char *const *paths = default_receipts;
    size_t np = 2;
    if (!spec_ids && argc > 1) {
        paths = (const char *const *)(argv + 1);
        np = (size_t)(argc - 1);
    }
    turing_store *st = turing_store_new(), *st1 = turing_store_new();
    int rc = 1;
    if (!st || !st1) goto out;
    char h[65], c[13];
    if (spec_ids) {
        if (turing_ingest_registry(st) < 0) goto out;
        turing_hex(&st->contract_digest, h);
        printf("contract %s PROVISIONAL\n", h);
        for (size_t k = 0; k < st->nspec; ++k) {
            turing_hex(&st->spec_id[k], h);
            printf("spec %-12s %s\n", st->spec[k].rz_id, h);
        }
        rc = 0;
        goto out;
    }
    if (load(st, paths, np) != 0 || load(st1, paths, 1) != 0) goto out;

    turing_hex(&st->contract_digest, h);
    printf("TURING Field V0 (post hoc over stored receipts; no timed runs)\n");
    printf("contract_digest %s\n  PROVISIONAL: SHA-256(\"%s\" 0x00 || OMG0 contract bytes); not an Omega semantic id\n",
           h, TURING_DOMAIN_CONTRACT);
    printf("specs %zu (one contract), evidence %zu, receipts %zu\n", st->nspec, st->nev, st->nreceipt);
    for (size_t k = 0; k < st->nspec; ++k) {
        turing_hex_short(&st->spec_id[k], c);
        printf("  %-12s spec %s  %s\n", st->spec[k].rz_id, c, st->spec[k].representation);
    }

    printf("\n== History view (git log analogue: commit = decision, parent = supersedes, tree = cited evidence,\n"
           "   blob = receipt). Root decision from the first receipt, then a superseding one from all receipts.\n\n");
    for (size_t s = 0; s < 4; ++s) {
        turing_query q = {shapes[s].n, shapes[s].m, 300, TURING_PACK_ONCE, 0};
        turing_decision d1, d2;
        turing_digest id1, id2;
        if (turing_field_select(st1, &q, NULL, NULL, &d1) != 0 || turing_decision_digest(&d1, &id1) != 0) goto out;
        if (turing_field_select(st, &q, NULL, &id1, &d2) != 0 || turing_decision_digest(&d2, &id2) != 0) goto out;
        printf("-- %s %s\n", shapes[s].name, shapes[s].where);
        print_decision(st, &d2, &id2);
        print_decision(st1, &d1, &id1);
        printf("\n");
    }

    printf("== Winners from stored evidence alone (Field selector, all receipts)\n");
    for (int pack = 0; pack < 2; ++pack)
        for (size_t s = 0; s < 4; ++s) {
            turing_query q = {shapes[s].n, shapes[s].m, 300, pack, 0};
            turing_decision d;
            if (turing_field_select(st, &q, NULL, NULL, &d) != 0) goto out;
            printf("  %s %-28s pack=%-8s -> %-11s %s (tie=%s)\n", shapes[s].name, shapes[s].where,
                   pack ? "per_call" : "once", d.cand_rz[d.chosen], d.verdict, d.tie_resolution);
        }

    turing_query named[8], all[160];
    size_t nn = 0, na = 0;
    for (int pack = 0; pack < 2; ++pack)
        for (size_t s = 0; s < 4; ++s) named[nn++] = (turing_query){shapes[s].n, shapes[s].m, 300, pack, 0};
    turing_query cells[80];
    size_t nc = turing_cells(st, cells, 80);
    for (int pack = 0; pack < 2; ++pack)
        for (size_t i = 0; i < nc; ++i) {
            all[na] = cells[i];
            all[na++].pack = pack;
        }
    const uint64_t seed = 0x7475726967303031ull; /* "turig001" */
    turing_regret r;
    printf("\n== Selector comparison on stored data (regret vs oracle = cheapest mean cost of the exact cell)\n");
    printf("   seed 0x%016" PRIx64 "; control arm = StarPU-style history per (spec, footprint n x m x pack)\n", seed);
    if (turing_compare(st, named, nn, 0, seed, &r) != 0) goto out;
    print_regret("S1-S4 x 2 pack, in-sample (warm)", &r, 1);
    turing_regret prim = r;
    if (turing_compare(st, named, nn, 1, seed, &r) != 0) goto out;
    print_regret("S1-S4 x 2 pack, leave-one-cell-out", &r, 1);
    turing_regret loo = r;
    if (turing_compare(st, named, 4, 0, seed, &r) != 0) goto out;
    print_regret("  of which pack once (S1-S4), in-sample", &r, 1);
    if (turing_compare(st, named + 4, 4, 0, seed, &r) != 0) goto out;
    print_regret("  of which pack per call (S1-S4), in-sample", &r, 1);
    if (turing_compare(st, all, na, 0, seed, &r) != 0) goto out;
    print_regret("all cells x 2 pack, in-sample", &r, 1);
    if (turing_compare(st, all, nc, 0, seed, &r) != 0) goto out;
    print_regret("  of which pack once (36 cells), in-sample", &r, 1);
    if (turing_compare(st, all + nc, nc, 0, seed, &r) != 0) goto out;
    print_regret("  of which per call (36 cells), in-sample", &r, 1);
    if (turing_compare(st, all, na, 1, seed, &r) != 0) goto out;
    print_regret("all cells x 2 pack, leave-one-cell-out", &r, 1);
    if (turing_compare_online(st, all, na, 10, seed, &r) != 0) goto out;
    print_regret("all cells x 2 pack, cold online x10", &r, 0);

    /* Pre-registered kill test (proposal K.4), dry run on stored data. */
    const double eq = 0.02, cap = 0.10;
    int q1 = prim.field_mean <= prim.hist_mean + eq && prim.field_max <= cap;
    int q1l = loo.field_mean <= loo.hist_mean + eq && loo.field_max <= cap;
    int q2 = prim.field_cites_ok == prim.decisions && loo.field_cites_ok == loo.decisions;
    int better = prim.field_mean < prim.hist_mean - eq || loo.field_mean < loo.hist_mean - eq;
    printf("\n== Kill test (pre-registered, dry run on stored data)\n");
    printf("  Q1 quality in-sample: Field mean <= control mean + 2%% and Field max <= 10%%: %s\n", q1 ? "PASS" : "FAIL");
    printf("  Q1 quality leave-one-out:                                             %s\n", q1l ? "PASS" : "FAIL");
    printf("  Q2 every Field decision's cited receipts verify:                      %s\n", q2 ? "PASS" : "FAIL");
    printf("  verdict: %s\n", !(q1 && q1l && q2) ? "FAIL (Turing selection is integration-only)"
                                  : better ? "BEAT on selection quality"
                                           : "SURVIVES on explainability at equal quality (not a quality win)");
    rc = 0;
out:
    turing_store_free(st);
    turing_store_free(st1);
    return rc;
}
