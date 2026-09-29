/* TURING selector comparison on stored receipts (post hoc, no timed runs).
 * Regret = cost(chosen) / cost(oracle) - 1, with costs = mean per-call cost
 * over every receipt row of the exact queried cell. */
#include "turing/select.h"

#include <stdlib.h>
#include <string.h>

static int spec_index(const turing_store *st, const turing_digest *id) {
    for (size_t k = 0; k < st->nspec; ++k)
        if (turing_digest_eq(&st->spec_id[k], id)) return (int)k;
    return -1;
}

double turing_cell_cost(const turing_store *st, size_t k, const turing_query *q) {
    double sum = 0;
    size_t rows = 0;
    for (size_t i = 0; i < st->nev; ++i) {
        const turing_evidence *e = &st->ev[i];
        if (e->n != q->n || e->m != q->m || e->sparsity_milli != q->sparsity_milli) continue;
        if (!turing_digest_eq(&e->spec_id, &st->spec_id[k])) continue;
        if (!e->verified) return -1;
        sum += (double)turing_ev_cost(e, q->pack);
        ++rows;
    }
    return rows ? sum / (double)rows : -1;
}

int turing_oracle(const turing_store *st, const turing_query *q, double *best_ps) {
    int best = -1;
    double bc = 0;
    for (size_t k = 0; k < st->nspec; ++k) {
        if (!st->spec[k].exact || q->n > st->spec[k].max_n) continue;
        double c = turing_cell_cost(st, k, q);
        if (c < 0) continue;
        if (best < 0 || c < bc) best = (int)k, bc = c;
    }
    if (best >= 0 && best_ps) *best_ps = bc;
    return best;
}

size_t turing_cells(const turing_store *st, turing_query *cells, size_t cap) {
    size_t nc = 0;
    for (size_t i = 0; i < st->nev; ++i) {
        const turing_evidence *e = &st->ev[i];
        size_t j;
        for (j = 0; j < nc; ++j)
            if (cells[j].n == e->n && cells[j].m == e->m && cells[j].sparsity_milli == e->sparsity_milli) break;
        if (j < nc) continue;
        if (nc >= cap) return nc;
        memset(&cells[nc], 0, sizeof cells[nc]);
        cells[nc].n = e->n;
        cells[nc].m = e->m;
        cells[nc].sparsity_milli = e->sparsity_milli;
        ++nc;
    }
    /* insertion sort by (m, n, s) for a stable, readable order */
    for (size_t i = 1; i < nc; ++i) {
        turing_query t = cells[i];
        size_t j = i;
        while (j > 0 && (cells[j - 1].m > t.m || (cells[j - 1].m == t.m && (cells[j - 1].n > t.n ||
                         (cells[j - 1].n == t.n && cells[j - 1].sparsity_milli > t.sparsity_milli))))) {
            cells[j] = cells[j - 1];
            --j;
        }
        cells[j] = t;
    }
    return nc;
}

static void acc(double r, double *mean, double *mx) {
    *mean += r;
    if (r > *mx) *mx = r;
}

static double regret_of(const turing_store *st, const turing_query *q, int k, double oracle) {
    if (k < 0) return 1e9;
    double c = turing_cell_cost(st, (size_t)k, q);
    return c < 0 ? 1e9 : c / oracle - 1.0;
}

int turing_compare(const turing_store *st, const turing_query *qs, size_t nq, int loo, uint64_t seed,
                   turing_regret *out) {
    memset(out, 0, sizeof *out);
    turing_history *h = malloc(sizeof *h);
    turing_decision *fd = malloc(sizeof *fd), *hd = malloc(sizeof *hd);
    int rc = -1;
    if (!h || !fd || !hd) goto done;
    if (!loo) {
        turing_history_init(h, seed);
        if (turing_history_load(h, st, NULL) != 0) goto done;
    }
    for (size_t i = 0; i < nq; ++i) {
        turing_query q = qs[i];
        q.exclude_exact_cell = loo;
        double oracle;
        if (turing_oracle(st, &q, &oracle) < 0) goto done;
        if (loo) {
            turing_history_init(h, seed);
            if (turing_history_load(h, st, &q) != 0) goto done;
        }
        if (turing_field_select(st, &q, NULL, NULL, fd) != 0) goto done;
        int hk = turing_history_select(h, st, &q, hd);
        int fk = spec_index(st, &fd->cand[fd->chosen]);
        acc(regret_of(st, &q, fk, oracle), &out->field_mean, &out->field_max);
        acc(regret_of(st, &q, hk, oracle), &out->hist_mean, &out->hist_max);
        char why[160];
        out->field_cites_ok += turing_decision_verify(st, fd, why, sizeof why) == 0;
        out->hist_cites_ok += turing_decision_verify(st, hd, why, sizeof why) == 0;
        out->agree += fk == hk;
        ++out->decisions;
    }
    if (out->decisions) {
        out->field_mean /= (double)out->decisions;
        out->hist_mean /= (double)out->decisions;
    }
    rc = 0;
done:
    free(h);
    free(fd);
    free(hd);
    return rc;
}

int turing_compare_online(const turing_store *st, const turing_query *qs, size_t nq, size_t rounds, uint64_t seed,
                          turing_regret *out) {
    memset(out, 0, sizeof *out);
    if (nq == 0 || nq > 256 || st->nreceipt == 0) return -1;
    turing_history *h = malloc(sizeof *h);
    turing_decision *fd = malloc(sizeof *fd), *hd = malloc(sizeof *hd);
    size_t *order = malloc(nq * sizeof *order);
    int rc = -1;
    if (!h || !fd || !hd || !order) goto done;
    turing_history_init(h, seed);
    uint64_t s = seed;
    /* Observation counter per (spec, query) picks which receipt row to reveal. */
    static unsigned seen[TURING_MAX_SPECS][256];
    memset(seen, 0, sizeof seen);
    for (size_t r = 0; r < rounds; ++r) {
        for (size_t i = 0; i < nq; ++i) order[i] = i;
        for (size_t i = nq; i > 1; --i) {
            size_t j = (size_t)(turing_splitmix64(&s) % i);
            size_t t = order[i - 1];
            order[i - 1] = order[j];
            order[j] = t;
        }
        for (size_t t = 0; t < nq; ++t) {
            const turing_query *q = &qs[order[t]];
            double oracle;
            if (turing_oracle(st, q, &oracle) < 0) goto done;
            if (turing_field_select(st, q, NULL, NULL, fd) != 0) goto done;
            int hk = turing_history_select(h, st, q, hd);
            if (hk < 0) goto done;
            /* Reveal one stored row for the control arm's pick, rotating receipts. */
            uint64_t rows[TURING_MAX_RECEIPTS];
            size_t nrows = 0;
            for (size_t i = 0; i < st->nev && nrows < TURING_MAX_RECEIPTS; ++i) {
                const turing_evidence *e = &st->ev[i];
                if (e->n == q->n && e->m == q->m && e->sparsity_milli == q->sparsity_milli &&
                    turing_digest_eq(&e->spec_id, &st->spec_id[hk]))
                    rows[nrows++] = turing_ev_cost(e, q->pack);
            }
            if (nrows == 0) goto done;
            uint64_t cost = rows[seen[hk][order[t]]++ % nrows];
            if (turing_history_observe(h, st, (size_t)hk, q->n, q->m, q->pack, cost) != 0) goto done;
            int fk = spec_index(st, &fd->cand[fd->chosen]);
            acc(regret_of(st, q, fk, oracle), &out->field_mean, &out->field_max);
            acc(regret_of(st, q, hk, oracle), &out->hist_mean, &out->hist_max);
            out->agree += fk == hk;
            ++out->decisions;
        }
    }
    out->field_mean /= (double)out->decisions;
    out->hist_mean /= (double)out->decisions;
    rc = 0;
done:
    free(h);
    free(fd);
    free(hd);
    free(order);
    return rc;
}
