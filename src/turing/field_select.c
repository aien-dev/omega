/* TURING Field selector V0 (docs/turing/TURING_W0_PROPOSAL.md K.2).
 *
 * Rule, in order:
 *  1. Candidates = every spec in the store. Filters with reason codes:
 *     CONTRACT_MISMATCH (spec names another contract digest), NOT_EXACT,
 *     MAX_N (query n above the spec's max_n).
 *  2. Usable evidence = rows whose receipt file still hashes to the digest the
 *     row records (re-hashed at decision time).
 *  3. Cell = the measured (n, m, sparsity) nearest the query among cells with
 *     usable evidence for an eligible candidate: distance |log2 n ratio| +
 *     |log2 m ratio| + 4 |sparsity difference|; ties -> smallest (n, m, s).
 *  4. Per candidate at that cell: NO_EVIDENCE, UNVERIFIED_RUN (a row not
 *     oracle-verified), CONTENTION_FORCED (a forced block), TIER (tier text not
 *     E2/E3/E4), else cost = mean over rows of the per-call cost.
 *  5. Noise band = max over cheapest and runner-up of within-run spread and
 *     across-run spread; tied set = costs within the band of the cheapest;
 *     TIE when it has more than one member, resolved only inside it:
 *     incumbent, else the reference R1_plain, else the cheapest.
 *  6. The decision cites every evidence digest used in step 4.
 */
#include "turing/select.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static double cell_dist(const turing_query *q, uint64_t n, uint64_t m, uint32_t s) {
    double dn = fabs(log2((double)n / (double)q->n));
    double dm = fabs(log2((double)m / (double)q->m));
    double ds = fabs((double)s - (double)q->sparsity_milli) / 1000.0;
    return dn + dm + 4.0 * ds;
}

static int cell_less(uint64_t n1, uint64_t m1, uint32_t s1, uint64_t n2, uint64_t m2, uint32_t s2) {
    if (n1 != n2) return n1 < n2;
    if (m1 != m2) return m1 < m2;
    return s1 < s2;
}

static int is_excluded(const turing_query *q, const turing_evidence *e) {
    return q->exclude_exact_cell && e->n == q->n && e->m == q->m && e->sparsity_milli == q->sparsity_milli;
}

static int tier_ok(const char *t) { return !strcmp(t, "E2") || !strcmp(t, "E3") || !strcmp(t, "E4"); }

int turing_field_select(const turing_store *st, const turing_query *q, const char *incumbent_rz,
                        const turing_digest *supersedes, turing_decision *d) {
    if (!st || !q || !d || q->n == 0 || q->m == 0 || st->nspec > TURING_MAX_CAND) return -1;
    memset(d, 0, sizeof *d);
    d->contract_digest = st->contract_digest;
    snprintf(d->selector, sizeof d->selector, "%s", TURING_FIELD_SELECTOR);
    snprintf(d->constraints, sizeof d->constraints, "exact contract; tier in {E2,E3,E4}; receipts verify");
    d->n = q->n;
    d->m = q->m;
    d->sparsity_milli = q->sparsity_milli;
    d->pack = q->pack;
    d->chosen = -1;
    if (supersedes) {
        d->has_supersedes = 1;
        d->supersedes = *supersedes;
    }
    snprintf(d->tie_resolution, sizeof d->tie_resolution, "none");

    /* Receipts re-hashed now. */
    int receipt_ok[TURING_MAX_RECEIPTS] = {0};
    for (size_t r = 0; r < st->nreceipt; ++r) {
        turing_digest fd;
        receipt_ok[r] = turing_file_digest(st->receipt[r].path, &fd) == 0 && turing_digest_eq(&fd, &st->receipt[r].digest);
    }

    int eligible[TURING_MAX_CAND] = {0};
    d->ncand = st->nspec;
    for (size_t k = 0; k < st->nspec; ++k) {
        const turing_rz_spec *s = &st->spec[k];
        d->cand[k] = st->spec_id[k];
        snprintf(d->cand_rz[k], sizeof d->cand_rz[k], "%s", s->rz_id);
        if (!turing_digest_eq(&s->contract_digest, &st->contract_digest))
            d->reason[k] = TURING_R_CONTRACT_MISMATCH;
        else if (s->exact != 1)
            d->reason[k] = TURING_R_NOT_EXACT;
        else if (q->n > s->max_n)
            d->reason[k] = TURING_R_MAX_N;
        else {
            d->reason[k] = TURING_R_NO_EVIDENCE;
            eligible[k] = 1;
        }
    }

    /* Usable evidence rows. */
    unsigned char usable[TURING_MAX_EVIDENCE];
    int any_unusable_receipt[TURING_MAX_CAND] = {0};
    for (size_t i = 0; i < st->nev; ++i) {
        const turing_evidence *e = &st->ev[i];
        usable[i] = 0;
        if (is_excluded(q, e)) continue;
        int ok = 0;
        for (size_t r = 0; r < st->nreceipt; ++r)
            if (turing_digest_eq(&st->receipt[r].digest, &e->receipt_digest)) ok = receipt_ok[r];
        for (size_t k = 0; k < st->nspec; ++k)
            if (turing_digest_eq(&st->spec_id[k], &e->spec_id)) {
                if (!ok) any_unusable_receipt[k] = 1;
                else if (eligible[k]) usable[i] = 1;
            }
    }

    /* Nearest cell. */
    int have = 0;
    double best = 0;
    for (size_t i = 0; i < st->nev; ++i) {
        if (!usable[i]) continue;
        const turing_evidence *e = &st->ev[i];
        double dd = cell_dist(q, e->n, e->m, e->sparsity_milli);
        if (!have || dd < best - 1e-12 ||
            (fabs(dd - best) <= 1e-12 &&
             cell_less(e->n, e->m, e->sparsity_milli, d->cell_n, d->cell_m, d->cell_sparsity_milli))) {
            have = 1;
            best = dd;
            d->cell_n = e->n;
            d->cell_m = e->m;
            d->cell_sparsity_milli = e->sparsity_milli;
        }
    }
    if (!have) {
        for (size_t k = 0; k < st->nspec; ++k)
            if (eligible[k] && any_unusable_receipt[k]) d->reason[k] = TURING_R_RECEIPT_UNVERIFIED;
        snprintf(d->verdict, sizeof d->verdict, "NONE");
        return 1;
    }
    d->exact_cell = d->cell_n == q->n && d->cell_m == q->m && d->cell_sparsity_milli == q->sparsity_milli;

    /* Per-candidate cost at the cell. */
    double cost[TURING_MAX_CAND] = {0}, spread[TURING_MAX_CAND] = {0};
    int measured[TURING_MAX_CAND] = {0};
    for (size_t k = 0; k < st->nspec; ++k) {
        if (!eligible[k]) continue;
        double sum = 0, lo = 0, hi = 0, within = 0;
        size_t rows = 0;
        int bad = -1;
        size_t cite0 = d->ncite;
        for (size_t i = 0; i < st->nev; ++i) {
            const turing_evidence *e = &st->ev[i];
            if (!usable[i] || !turing_digest_eq(&e->spec_id, &st->spec_id[k]) || e->n != d->cell_n ||
                e->m != d->cell_m || e->sparsity_milli != d->cell_sparsity_milli)
                continue;
            if (!e->verified) bad = TURING_R_UNVERIFIED_RUN;
            else if (e->forced) bad = TURING_R_CONTENTION_FORCED;
            else if (!tier_ok(e->tier)) bad = TURING_R_TIER;
            double c = (double)turing_ev_cost(e, q->pack);
            double w = e->noise_ppm / 1e6;
            if (q->pack == TURING_PACK_PER_CALL && c > 0)
                w = (e->noise_ppm / 1e6 * (double)e->median_ps + e->pack_noise_ppm / 1e6 * (double)e->pack_ps) / c;
            if (rows == 0 || c < lo) lo = c;
            if (rows == 0 || c > hi) hi = c;
            if (w > within) within = w;
            sum += c;
            ++rows;
            if (d->ncite >= TURING_MAX_CITE) return -1;
            d->cite[d->ncite++] = st->ev_id[i];
        }
        if (rows == 0) {
            d->reason[k] = any_unusable_receipt[k] ? TURING_R_RECEIPT_UNVERIFIED : TURING_R_NO_EVIDENCE;
            continue;
        }
        if (bad >= 0) {
            d->reason[k] = (turing_reason)bad;
            d->ncite = cite0; /* only cite what the choice rests on */
            continue;
        }
        cost[k] = sum / (double)rows;
        double across = cost[k] > 0 ? (hi - lo) / cost[k] : 0;
        spread[k] = within > across ? within : across;
        measured[k] = 1;
        d->reason[k] = TURING_R_RANKED;
        d->cost_ps[k] = (uint64_t)llround(cost[k]);
    }

    int cheap = -1, runner = -1;
    for (size_t k = 0; k < st->nspec; ++k) {
        if (!measured[k]) continue;
        if (cheap < 0 || cost[k] < cost[cheap]) {
            runner = cheap;
            cheap = (int)k;
        } else if (runner < 0 || cost[k] < cost[runner]) {
            runner = (int)k;
        }
    }
    if (cheap < 0) {
        snprintf(d->verdict, sizeof d->verdict, "NONE");
        d->ncite = 0;
        return 1;
    }
    double band = spread[cheap];
    if (runner >= 0 && spread[runner] > band) band = spread[runner];
    d->band_ppm = (uint32_t)llround(band * 1e6);
    d->margin_ppm = runner >= 0 ? (uint32_t)llround((cost[runner] / cost[cheap] - 1.0) * 1e6) : 0;

    int tied[TURING_MAX_CAND] = {0}, ntied = 0;
    for (size_t k = 0; k < st->nspec; ++k)
        if (measured[k] && cost[k] <= cost[cheap] * (1.0 + band)) tied[k] = 1, ++ntied;

    int chosen = cheap;
    if (ntied > 1) {
        snprintf(d->verdict, sizeof d->verdict, "TIE");
        int inc = incumbent_rz ? turing_find_spec(st, incumbent_rz) : -1;
        int ref = turing_find_spec(st, TURING_REFERENCE_RZ);
        if (inc >= 0 && tied[inc]) {
            chosen = inc;
            snprintf(d->tie_resolution, sizeof d->tie_resolution, "incumbent");
        } else if (ref >= 0 && tied[ref]) {
            chosen = ref;
            snprintf(d->tie_resolution, sizeof d->tie_resolution, "reference");
        } else {
            snprintf(d->tie_resolution, sizeof d->tie_resolution, "cheapest");
        }
        for (size_t k = 0; k < st->nspec; ++k)
            if (tied[k]) d->reason[k] = TURING_R_TIED;
    } else {
        snprintf(d->verdict, sizeof d->verdict, "CHOSEN");
    }
    d->chosen = chosen;
    d->reason[chosen] = TURING_R_CHOSEN;
    return 0;
}
