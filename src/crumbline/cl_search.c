#include "cl_search.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W_PROBES CL_PROBE_COUNT
#define MAX_PENDING 64
#define MAX_HYP 64

void cl_search_default_config(ClSearchConfig *cfg) {
    cfg->max_candidates = 20000;
    cfg->max_depth = 6;
    cfg->frontier_cap = 4000;
    cfg->max_oracle_queries = 4;
    cfg->ambiguity_scan = 400;
    cfg->robust_min_pct = 75;
    cfg->max_program_insns = 80;
}

uint8_t cl_crumb_input_bits(const ClCrumb *c) {
    uint64_t m = 0;
    for (uint32_t i = 0; i < c->n; ++i)
        if (c->in[i][0] > m) m = c->in[i][0];
    uint8_t b = 0;
    while (m) {
        b++;
        m >>= 1;
    }
    return b;
}

typedef struct {
    uint32_t id;
    uint32_t parent; /* index into nodes, UINT32_MAX for the root */
    uint16_t op;
    uint8_t depth;
    uint8_t nsteps;
    uint32_t insns; /* realized instructions excluding the final RET */
} Node;

typedef struct {
    uint64_t h1, h2;
    uint32_t insns;
    uint8_t used;
} Slot;

typedef struct {
    uint32_t node_id;
    ClSteps steps;
} Pending;

typedef struct {
    const ClCrumb *crumb;
    const ClBank *bank;
    const ClSearchConfig *cfg;
    const ClOracle *oracle;
    const ClEventSink *sink;
    ClSearchResult *res;
    uint32_t n;      /* visible examples */
    uint32_t w;      /* values per node: n + probes */
    uint64_t mask;
    Node *nodes;
    uint64_t *vals;
    size_t count, cap;
    Slot *table;
    size_t tmask;
    uint64_t inputs[CL_CRUMB_MAX_EXAMPLES];
    Pending pending[MAX_PENDING];
    size_t npending;
    uint32_t pending_since;
    uint8_t hyps[MAX_HYP][CL_DIGEST_BYTES];
    size_t nhyp;
    uint32_t max_queries;
    bool stop, solved, first_submit_done;
} S;

static void emit(S *s, const ClEvent *ev) {
    if (s->sink && s->sink->emit) s->sink->emit(s->sink->ctx, ev);
}

static void hash_vals(const uint64_t *v, uint32_t w, uint64_t *h1, uint64_t *h2) {
    uint64_t a = 0x9E3779B97F4A7C15ULL, b = 0xC2B2AE3D27D4EB4FULL;
    for (uint32_t i = 0; i < w; ++i) {
        a = (a ^ v[i]) * 0x100000001B3ULL;
        a ^= a >> 29;
        b = (b + v[i]) * 0xFF51AFD7ED558CCDULL;
        b ^= b >> 33;
    }
    *h1 = a;
    *h2 = b;
}

/* true if an equivalent state with cost <= insns already exists */
static bool equiv_seen(S *s, uint64_t h1, uint64_t h2, uint32_t insns) {
    size_t i = (size_t)h1 & s->tmask;
    for (;;) {
        Slot *sl = &s->table[i];
        if (!sl->used) {
            sl->used = 1;
            sl->h1 = h1;
            sl->h2 = h2;
            sl->insns = insns;
            return false;
        }
        if (sl->h1 == h1 && sl->h2 == h2) {
            if (sl->insns <= insns) return true;
            sl->insns = insns;
            return false;
        }
        i = (i + 1) & s->tmask;
    }
}

static uint32_t op_insns(const ClOp *op) { return cl_steps_insns(&op->steps) - 1; }

/* Steps of the chain: ancestors of nodes[parent_idx], then bank op `op`. */
static int build_steps(S *s, uint32_t parent_idx, uint16_t op, ClSteps *out) {
    uint16_t ops[64];
    int k = 0;
    for (uint32_t i = parent_idx; i != UINT32_MAX && s->nodes[i].parent != UINT32_MAX; i = s->nodes[i].parent) {
        if (k >= 63) return -1;
        ops[k++] = s->nodes[i].op;
    }
    memset(out, 0, sizeof(*out));
    for (int j = k - 1; j >= 0; --j)
        if (cl_steps_concat(out, &s->bank->ops[ops[j]].steps, out) != 0) return -1;
    return cl_steps_concat(out, &s->bank->ops[op].steps, out);
}

/* Omega verification of a visible fit: realize, V0+V2, native-vs-reference. */
static uint8_t verify_fit(S *s, const ClSteps *steps) {
    OmegaProgram prog;
    if (cl_steps_build_program(steps, &prog) != 0) return CL_VERIFY_FAIL_BUILD;
    VerifyReport rep;
    uint8_t out = CL_VERIFY_PASS;
    if (omega_program_verify(&prog, &rep) != 0) {
        out = CL_VERIFY_FAIL_OMEGA;
    } else {
        uint32_t execs = 0;
        int bad = cl_steps_differential(steps, &prog, s->inputs, s->n, &execs);
        s->res->exec_count += execs;
        if (bad != 0) out = CL_VERIFY_FAIL_DIFFERENTIAL;
    }
    omega_program_destroy(&prog);
    return out;
}

static void submit_one(S *s, uint32_t node_id, const ClSteps *steps, uint8_t flags) {
    if (s->res->oracle_queries >= s->max_queries) {
        s->stop = true;
        return;
    }
    s->res->oracle_queries++;
    if (!s->first_submit_done) {
        s->first_submit_done = true;
        s->res->hypotheses_at_first_submit = (uint32_t)s->nhyp;
    }
    ClEvent ev = {CL_EV_SUBMIT, 0, CL_VERIFY_PASS, (flags & CL_SUBMIT_ROBUST) ? CL_FIT_ROBUST : CL_FIT_EXACT, 0,
                  node_id, node_id, 0, (uint16_t)s->res->oracle_queries};
    emit(s, &ev);
    uint8_t verdict = CL_VERDICT_REJECT;
    uint8_t hyp = s->nhyp > 255 ? 255 : (uint8_t)s->nhyp;
    if (s->oracle->submit(s->oracle->ctx, node_id, steps, flags, hyp, &verdict) != 0) {
        s->stop = true;
        return;
    }
    if (verdict == CL_VERDICT_ACCEPT) {
        s->solved = true;
        s->stop = true;
        s->res->outcome = CL_OUTCOME_SOLVED;
        s->res->solution = *steps;
        s->res->solution_node = node_id;
        s->res->candidates_to_solution = s->res->generated;
    } else if (verdict == CL_VERDICT_EXHAUSTED) {
        s->stop = true;
    } else {
        s->res->oracle_rejections++;
    }
}

static void flush_pending(S *s) {
    for (size_t i = 0; i < s->npending && !s->stop; ++i) submit_one(s, s->pending[i].node_id, &s->pending[i].steps, 0);
    s->npending = 0;
}

int cl_search_run(const ClCrumb *crumb, const ClBank *bank, const ClSearchConfig *cfg_in, const ClOracle *oracle,
                  const ClEventSink *sink, ClSearchResult *res) {
    if (!crumb || !bank || !cfg_in || !oracle || !res) return -1;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    memset(res, 0, sizeof(*res));
    res->outcome = CL_OUTCOME_UNSOLVED;
    res->solution_node = UINT32_MAX;

    ClSearchConfig cfg = *cfg_in;
    if (crumb->has_budget) {
        if (crumb->budget_max_candidates && crumb->budget_max_candidates < cfg.max_candidates)
            cfg.max_candidates = crumb->budget_max_candidates;
        if (crumb->budget_max_depth && crumb->budget_max_depth < cfg.max_depth) cfg.max_depth = crumb->budget_max_depth;
        if (crumb->budget_max_oracle_queries && crumb->budget_max_oracle_queries < cfg.max_oracle_queries)
            cfg.max_oracle_queries = crumb->budget_max_oracle_queries;
    }
    /* Omega's unary synthesis handles one input lane and one output lane. */
    if (crumb->in_arity != 1 || crumb->out_arity != 1) {
        res->outcome = CL_OUTCOME_UNSUPPORTED;
        return 0;
    }

    S s;
    memset(&s, 0, sizeof(s));
    s.crumb = crumb;
    s.bank = bank;
    s.cfg = &cfg;
    s.oracle = oracle;
    s.sink = sink;
    s.res = res;
    s.n = crumb->n;
    s.w = crumb->n + W_PROBES;
    s.mask = cl_crumb_out_mask(crumb);
    s.max_queries = cfg.max_oracle_queries;
    for (uint32_t i = 0; i < s.n; ++i) s.inputs[i] = crumb->in[i][0];

    s.cap = (size_t)cfg.frontier_cap * cfg.max_depth + 1;
    if (s.cap > (size_t)cfg.max_candidates + 1) s.cap = (size_t)cfg.max_candidates + 1;
    size_t tsize = 1024;
    while (tsize < 2 * ((size_t)cfg.max_candidates + 2)) tsize <<= 1;
    s.tmask = tsize - 1;
    s.nodes = calloc(s.cap, sizeof(Node));
    s.vals = calloc(s.cap * s.w, sizeof(uint64_t));
    s.table = calloc(tsize, sizeof(Slot));
    uint64_t *scratch = calloc(s.w, sizeof(uint64_t));
    if (!s.nodes || !s.vals || !s.table || !scratch) {
        free(s.nodes);
        free(s.vals);
        free(s.table);
        free(scratch);
        return -1;
    }

    const uint64_t *probes = cl_probe_inputs();
    Node *root = &s.nodes[0];
    root->id = 0;
    root->parent = UINT32_MAX;
    root->op = UINT16_MAX;
    for (uint32_t k = 0; k < s.n; ++k) s.vals[k] = s.inputs[k];
    for (uint32_t k = 0; k < W_PROBES; ++k) s.vals[s.n + k] = probes[k];
    uint64_t h1, h2;
    hash_vals(s.vals, s.w, &h1, &h2);
    equiv_seen(&s, h1, h2, 0);
    s.count = 1;

    uint32_t next_id = 1;
    size_t level_start = 0, level_end = 1;
    int best_matches = -1;
    uint32_t best_node = 0, best_parent = 0;
    uint16_t best_op = 0;
    uint32_t robust_need = cfg.robust_min_pct ? (s.n * cfg.robust_min_pct + 99) / 100 : UINT32_MAX;

    for (uint32_t depth = 1; depth <= cfg.max_depth && !s.stop; ++depth) {
        size_t level_kept = 0;
        for (size_t pi = level_start; pi < level_end && !s.stop; ++pi) {
            res->expansions++;
            for (size_t oi = 0; oi < bank->count && !s.stop; ++oi) {
                if (res->generated >= cfg.max_candidates) {
                    s.stop = true;
                    break;
                }
                res->generated++;
                const Node *parent = &s.nodes[pi];
                const ClOp *op = &bank->ops[oi];
                ClEvent ev = {CL_EV_EXPAND, CL_PRUNE_NONE, CL_VERIFY_NOT_REACHED, CL_FIT_NONE, (uint16_t)oi,
                              parent->id, next_id++, 0, 0};
                uint32_t nsteps = parent->nsteps + op->steps.n;
                uint32_t insns = parent->insns + op_insns(op);
                if (nsteps > CL_MAX_STEPS) {
                    ev.prune = CL_PRUNE_STEP_CAP;
                    emit(&s, &ev);
                    continue;
                }
                if (insns + 1 > cfg.max_program_insns) {
                    ev.prune = CL_PRUNE_COST;
                    res->pruned_cost++;
                    emit(&s, &ev);
                    continue;
                }
                const uint64_t *pv = &s.vals[pi * s.w];
                for (uint32_t k = 0; k < s.w; ++k) scratch[k] = cl_steps_eval(&op->steps, pv[k]);
                ev.exec_cost = s.w;
                hash_vals(scratch, s.w, &h1, &h2);
                if (equiv_seen(&s, h1, h2, insns)) {
                    ev.prune = CL_PRUNE_EQUIV;
                    res->pruned_equiv++;
                    emit(&s, &ev);
                    continue;
                }
                res->evaluated++;
                uint32_t mismatch = 0;
                for (uint32_t k = 0; k < s.n; ++k)
                    if ((scratch[k] & s.mask) != crumb->out[k][0]) mismatch++;

                if (level_kept < cfg.frontier_cap && s.count < s.cap) {
                    Node *c = &s.nodes[s.count];
                    c->id = ev.child;
                    c->parent = (uint32_t)pi;
                    c->op = (uint16_t)oi;
                    c->depth = (uint8_t)depth;
                    c->nsteps = (uint8_t)nsteps;
                    c->insns = insns;
                    memcpy(&s.vals[s.count * s.w], scratch, s.w * sizeof(uint64_t));
                    s.count++;
                    level_kept++;
                } else {
                    ev.prune = CL_PRUNE_FRONTIER_CAP;
                    res->pruned_cap++;
                }

                if (mismatch == 0) {
                    ev.fit = CL_FIT_EXACT;
                    res->visible_fits++;
                    ClSteps steps;
                    if (build_steps(&s, (uint32_t)pi, (uint16_t)oi, &steps) != 0) {
                        ev.verify = CL_VERIFY_FAIL_BUILD;
                    } else {
                        ev.verify = verify_fit(&s, &steps);
                    }
                    if (ev.verify != CL_VERIFY_PASS) {
                        res->verify_failures++;
                    } else {
                        uint8_t sig[CL_DIGEST_BYTES];
                        cl_steps_behavior(&steps, sig);
                        bool fresh = true;
                        for (size_t h = 0; h < s.nhyp; ++h)
                            if (memcmp(s.hyps[h], sig, CL_DIGEST_BYTES) == 0) fresh = false;
                        if (fresh && s.nhyp < MAX_HYP) memcpy(s.hyps[s.nhyp++], sig, CL_DIGEST_BYTES);
                        if (fresh && s.npending < MAX_PENDING) {
                            if (s.npending == 0) s.pending_since = res->generated;
                            s.pending[s.npending].node_id = ev.child;
                            s.pending[s.npending].steps = steps;
                            s.npending++;
                        }
                    }
                } else if (s.n - mismatch >= robust_need && (int)(s.n - mismatch) > best_matches) {
                    best_matches = (int)(s.n - mismatch);
                    best_node = ev.child;
                    best_parent = (uint32_t)pi;
                    best_op = (uint16_t)oi;
                }
                emit(&s, &ev);

                if (s.npending && res->generated >= s.pending_since + cfg.ambiguity_scan) flush_pending(&s);
            }
        }
        level_start = level_end;
        level_end = s.count;
        if (level_start == level_end) break;
    }
    if (!s.solved && s.npending) {
        s.stop = false;
        flush_pending(&s);
    }
    if (!s.solved && best_matches >= 0 && res->oracle_queries < s.max_queries) {
        ClSteps steps;
        if (build_steps(&s, best_parent, best_op, &steps) == 0 && verify_fit(&s, &steps) == CL_VERIFY_PASS) {
            s.stop = false;
            submit_one(&s, best_node, &steps, CL_SUBMIT_ROBUST);
        }
    }

    free(s.nodes);
    free(s.vals);
    free(s.table);
    free(scratch);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    res->wall_ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
    return 0;
}
