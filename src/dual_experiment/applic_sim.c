/* Deterministic discrete workload simulator for the DUAL applicability
 * experiment. One step: arrivals (enqueue or REJECT when the queue is full),
 * admission in class-then-FIFO order with backfill against slot and memory
 * capacity, blocked check, then one unit of service for every running item
 * and completion. The exact ledger
 *     arrived == completed + queued + running + rejected
 * is checked after every step. No I/O, no allocation, no globals. */
#include "applic.h"

#include <math.h>
#include <string.h>

uint64_t ap_rng_next(ap_rng *r)
{
    r->s ^= r->s >> 12; r->s ^= r->s << 25; r->s ^= r->s >> 27;
    return r->s * 0x2545F4914F6CDD1Dull;
}
double ap_rng_unit(ap_rng *r) { return (double)(ap_rng_next(r) >> 11) / 9007199254740992.0; }

static uint32_t rng_range(ap_rng *r, uint32_t lo, uint32_t hi)   /* inclusive */
{
    return lo + (uint32_t)(ap_rng_next(r) % (uint64_t)(hi - lo + 1u));
}
/* Standard normal by Box-Muller; the pair's second value is discarded so the
 * draw count per item is fixed. */
static double rng_normal(ap_rng *r)
{
    double u1 = ap_rng_unit(r), u2 = ap_rng_unit(r);
    if (u1 < 1e-300) u1 = 1e-300;
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}
/* Poisson count (Knuth); rates here are at most a few per step. */
static uint32_t rng_poisson(ap_rng *r, double lambda)
{
    double L = exp(-lambda), p = 1.0;
    uint32_t k = 0;
    if (lambda <= 0.0) return 0;
    do { k++; p *= ap_rng_unit(r); } while (p > L);
    return k - 1u;
}

/* ---- item families ---- */
static void gen_homogeneous(ap_rng *r, ap_item *it)
{
    it->demand0 = rng_range(r, AP_HOM_DEMAND_MIN, AP_HOM_DEMAND_MAX);
    it->mem = (uint16_t)rng_range(r, AP_HOM_MEM_MIN, AP_HOM_MEM_MAX);
    it->slots = (uint8_t)AP_HOM_SLOTS;
    it->cls = (uint8_t)rng_range(r, 0, AP_CLASSES - 1u);
}
static void gen_heterogeneous(ap_rng *r, ap_item *it, double sigma)
{
    double d = exp(AP_HET_LN_MEDIAN + sigma * rng_normal(r));
    double u;
    if (d < 1.0) d = 1.0;
    if (d > (double)AP_HET_DEMAND_CAP) d = (double)AP_HET_DEMAND_CAP;
    it->demand0 = (uint32_t)floor(d + 0.5);
    u = ap_rng_unit(r);
    it->slots = (uint8_t)(u < AP_HET_SLOTS_P1 ? 1u : (u < AP_HET_SLOTS_P2 ? 2u : 4u));
    u = ap_rng_unit(r);
    it->mem = (uint16_t)(AP_HET_MEM_BASE + (uint32_t)floor(AP_HET_MEM_SPAN * u * u * u + 0.5));
    u = ap_rng_unit(r);
    it->cls = (uint8_t)(u < AP_HET_CLASS_P0 ? 0u : (u < AP_HET_CLASS_P1 ? 1u : (u < AP_HET_CLASS_P2 ? 2u : 3u)));
}

/* ---- workload regime: arrivals this step, item family, capacity ---- */
static int family_is_heterogeneous(ap_workload wl, uint32_t step)
{
    if (wl == AP_WL_HETEROGENEOUS || wl == AP_WL_HETERO_SWEEP) return 1;
    if (wl == AP_WL_REGIME_CHANGE) return step >= AP_SWITCH_STEP;
    return 0;
}
static uint32_t arrivals_this_step(ap_rng *r, ap_workload wl, uint32_t step, double sweep_rate)
{
    switch (wl) {
    case AP_WL_HOMOGENEOUS:
    case AP_WL_CAPACITY_DROP: return rng_poisson(r, AP_RATE_HOM);
    case AP_WL_HETEROGENEOUS: return rng_poisson(r, AP_RATE_HET);
    case AP_WL_STEP: return rng_poisson(r, step < AP_SWITCH_STEP ? AP_STEP_RATE_LOW : AP_STEP_RATE_HIGH);
    case AP_WL_SQUARE: return rng_poisson(r, ((step / AP_SQUARE_HALF_PERIOD) & 1u) ? AP_SQUARE_RATE_HIGH : AP_SQUARE_RATE_LOW);
    case AP_WL_BURSTY:
        if (ap_rng_unit(r) < AP_BURST_P) return rng_range(r, AP_BURST_MIN, AP_BURST_MAX);
        return rng_poisson(r, AP_BURST_BACKGROUND);
    case AP_WL_REGIME_CHANGE: return rng_poisson(r, step < AP_SWITCH_STEP ? AP_RATE_HOM : AP_RATE_HET);
    case AP_WL_HETERO_SWEEP: return rng_poisson(r, sweep_rate);
    default: return 0;
    }
}
static uint32_t capacity_this_step(ap_workload wl, uint32_t step)
{
    if (wl == AP_WL_CAPACITY_DROP && step >= AP_SWITCH_STEP) return AP_CAP_DROP_SLOTS;
    return AP_SLOTS;
}

uint64_t ap_seed(ap_workload wl, uint32_t seed_index)
{
    uint64_t s = AP_SEED_BASE ^ ((uint64_t)wl * 0x9E3779B97F4A7C15ull) ^ (((uint64_t)seed_index + 1u) * 0xD1B54A32D192ED03ull);
    return s ? s : 1u;
}

void ap_sim_init(ap_sim *s, ap_workload wl, uint64_t seed)
{
    memset(s, 0, sizeof *s);
    s->wl = wl;
    s->rng.s = seed ? seed : 1u;
    s->slots_capacity = AP_SLOTS;
    s->het_sigma = AP_HET_SIGMA;
    s->sweep_rate = 0.0;
}

double ap_sweep_sigma(uint32_t seed_index)
{
    double f = AP_SEEDS > 1u ? (double)seed_index / (double)(AP_SEEDS - 1u) : 0.0;
    return AP_SWEEP_SIGMA_MIN + (AP_SWEEP_SIGMA_MAX - AP_SWEEP_SIGMA_MIN) * f;
}

void ap_sim_init_indexed(ap_sim *s, ap_workload wl, uint64_t seed, uint32_t seed_index)
{
    ap_sim_init(s, wl, seed);
    if (wl == AP_WL_HETERO_SWEEP) {
        double sigma = ap_sweep_sigma(seed_index);
        double mean_demand = exp(AP_HET_LN_MEDIAN + 0.5 * sigma * sigma);
        s->het_sigma = sigma;
        s->sweep_rate = AP_SWEEP_LOAD * (double)AP_SLOTS / (mean_demand * AP_HET_MEAN_SLOTS);
    }
}

int ap_sim_ledger_ok(const ap_sim *s)
{
    return s->arrived == s->completed + (uint64_t)s->queue_len + (uint64_t)s->running_len + s->rejected;
}

static void close_window(ap_sim *s)
{
    ap_window *w = &s->windows[s->n_windows++];
    w->slots_capacity = s->slots_capacity;
    w->used_slots_frac = (double)s->used_slots / (double)s->slots_capacity;
    w->used_mem_frac = (double)s->used_mem / (double)AP_MEMORY;
    w->queue_density = (double)s->queue_len / (double)AP_MAX_QUEUE;
    w->arrivals = s->w_arrivals; w->completions = s->w_completions;
    w->blocked_steps = s->w_blocked; w->rejected = s->w_rejected;
    w->queue_len = s->queue_len; w->running = s->running_len;
    w->in_system = s->queue_len + s->running_len;
    if (s->n_completed_stats) {
        double n = (double)s->n_completed_stats;
        w->mean_demand = s->sum_demand / n; w->mean_slots = s->sum_slots / n; w->mean_mem = s->sum_mem / n;
    } else {
        w->mean_demand = AP_PRIOR_DEMAND; w->mean_slots = AP_PRIOR_SLOTS; w->mean_mem = AP_PRIOR_MEM;
    }
    s->w_arrivals = s->w_completions = s->w_blocked = s->w_rejected = 0;
}

int ap_sim_step(ap_sim *s)
{
    uint32_t n, i, c;
    if (s->step >= AP_STEPS) return 0;
    s->slots_capacity = capacity_this_step(s->wl, s->step);

    /* 1. arrivals */
    n = arrivals_this_step(&s->rng, s->wl, s->step, s->sweep_rate);
    for (i = 0; i < n; i++) {
        ap_item it;
        memset(&it, 0, sizeof it);
        if (family_is_heterogeneous(s->wl, s->step)) gen_heterogeneous(&s->rng, &it, s->het_sigma);
        else gen_homogeneous(&s->rng, &it);
        it.arrival = s->step; it.remaining = it.demand0;
        if (s->n_items >= AP_MAX_ITEMS) { s->pool_exhausted++; return 2; }
        s->arrived++; s->w_arrivals++;
        if (s->queue_len >= AP_MAX_QUEUE) { s->rejected++; s->w_rejected++; continue; }
        s->items[s->n_items] = it;
        s->queue[s->queue_len++] = s->n_items++;
    }

    /* 2. admission: class order, FIFO within class, backfill */
    for (c = 0; c < AP_CLASSES; c++) {
        i = 0;
        while (i < s->queue_len) {
            const ap_item *it = &s->items[s->queue[i]];
            if (it->cls == c && s->used_slots + it->slots <= s->slots_capacity
                && s->used_mem + it->mem <= AP_MEMORY && s->running_len < AP_SLOTS) {
                s->running[s->running_len++] = s->queue[i];
                s->used_slots += it->slots; s->used_mem += it->mem;
                memmove(&s->queue[i], &s->queue[i + 1], (s->queue_len - i - 1u) * sizeof s->queue[0]);
                s->queue_len--;
            } else {
                i++;
            }
        }
    }

    /* 3. blocked: work waited that could not be admitted this step */
    if (s->queue_len > 0) s->w_blocked++;

    /* 4. service and completion */
    i = 0;
    while (i < s->running_len) {
        ap_item *it = &s->items[s->running[i]];
        if (it->remaining) it->remaining--;
        if (it->remaining == 0) {
            s->used_slots -= it->slots; s->used_mem -= it->mem;
            s->completed++; s->w_completions++;
            s->sum_demand += (double)it->demand0; s->sum_slots += (double)it->slots; s->sum_mem += (double)it->mem;
            s->n_completed_stats++;
            s->running[i] = s->running[--s->running_len];   /* swap-remove; order irrelevant */
        } else {
            i++;
        }
    }

    s->step++;
    if (s->step % AP_WINDOW == 0 && s->n_windows < AP_WINDOWS) close_window(s);
    return ap_sim_ledger_ok(s) ? 0 : 1;
}

int ap_sim_run(ap_sim *s)
{
    int rc = 0;
    while (s->step < AP_STEPS) {
        int r = ap_sim_step(s);
        if (r && !rc) rc = r;
        if (r == 2) break;
    }
    return rc;
}

const char *ap_workload_name(ap_workload wl)
{
    static const char *const names[AP_WL_COUNT] = {
        "a_homogeneous", "b_heterogeneous", "c_step", "d_square_wave",
        "e_bursty", "f_capacity_drop", "g_regime_change", "h_hetero_sweep" };
    return (unsigned)wl < (unsigned)AP_WL_COUNT ? names[wl] : "?";
}
