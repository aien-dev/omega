/* Aggregate (macroscopic) predictor and its error against the exact discrete
 * outcome. The predictor sees one ap_window record only: densities, last
 * window's arrival count, the counts in system, and running means over
 * completed items. It never sees the item list. Flow balance (v2):
 *     resident_cap = min(slots / mean_slots, memory / mean_mem)      items
 *     cap_W        = resident_cap * W / mean_demand                   max completions in a window
 *     frac         = min(1, W / mean_demand)                           share of a resident item finished within W
 *     completions  = min(cap_W, running*frac + queue*frac + A*max(0, W - mean_demand)/W)
 *     in_next      = in_system + A - completions
 *     queue        = max(0, in_next - resident_cap) / queue_max
 *     occupancy    = in_next / resident_cap
 *     blocked      = clamp((occupancy - RAMP_START) / (1 - RAMP_START), 0, 1)
 * A is the arrival forecast: the last window's count (real predictor), the
 * next window's true count (known-arrivals variant, isolates aggregation
 * error from forecast error) or a shuffled window's record (negative control).
 * These formulas are pre-registered. A first draft (v1) counted running items
 * as newly offered work and always predicted full capacity; it was replaced
 * before any result was recorded, and no v1 number is reported anywhere. */
#include "applic.h"

#include <math.h>

static double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

static ap_prediction predict_with_arrivals(const ap_window *w, double A)
{
    ap_prediction p;
    double W = (double)AP_WINDOW;
    double mean_d = w->mean_demand > 0.0 ? w->mean_demand : AP_PRIOR_DEMAND;
    double mean_s = w->mean_slots > 0.0 ? w->mean_slots : AP_PRIOR_SLOTS;
    double mean_m = w->mean_mem > 0.0 ? w->mean_mem : AP_PRIOR_MEM;
    double by_slots = (double)w->slots_capacity / mean_s;
    double by_mem = (double)AP_MEMORY / mean_m;
    double resident_cap = by_slots < by_mem ? by_slots : by_mem;
    double cap_W = resident_cap * W / mean_d;
    double frac = W / mean_d < 1.0 ? W / mean_d : 1.0;
    double late = (W - mean_d) > 0.0 ? (W - mean_d) / W : 0.0;
    double flow = (double)w->running * frac + (double)w->queue_len * frac + A * late;
    double in_next, occ;
    p.completions = flow < cap_W ? flow : cap_W;
    in_next = (double)w->in_system + A - p.completions;
    if (in_next < 0.0) in_next = 0.0;
    p.queue_density = clamp01((in_next - resident_cap) / (double)AP_MAX_QUEUE);
    occ = resident_cap > 0.0 ? in_next / resident_cap : 1.0;
    p.blocked = clamp01((occ - AP_BLOCKED_RAMP_START) / (1.0 - AP_BLOCKED_RAMP_START));
    return p;
}

ap_prediction ap_predict(const ap_window *w) { return predict_with_arrivals(w, (double)w->arrivals); }

/* mode 0: real (A = last window's arrivals); 1: known arrivals (A = next
 * window's true count); 2: shuffled (record and A from window src). */
static ap_error score(const ap_sim *s, int mode, uint32_t offset)
{
    ap_error e;
    uint32_t first = AP_WARMUP_WINDOWS, last = s->n_windows ? s->n_windows - 1u : 0u;
    uint32_t scored = last > first ? last - first : 0u;    /* predictions for windows first+1..last */
    double se_c = 0.0, se_q = 0.0, se_b = 0.0, sum_actual_c = 0.0;
    uint32_t t;
    e.completions_nrmse = e.queue_rmse = e.blocked_rmse = e.composite = 0.0;
    e.windows = scored;
    if (scored == 0) return e;
    for (t = first; t < last; t++) {
        const ap_window *a = &s->windows[t + 1];
        uint32_t src = mode == 2 ? first + ((t - first + offset) % scored) : t;
        ap_prediction p = mode == 1 ? predict_with_arrivals(&s->windows[t], (double)a->arrivals)
                                    : ap_predict(&s->windows[src]);
        double ac = (double)a->completions;
        double ab = (double)a->blocked_steps / (double)AP_WINDOW;
        se_c += (p.completions - ac) * (p.completions - ac);
        se_q += (p.queue_density - a->queue_density) * (p.queue_density - a->queue_density);
        se_b += (p.blocked - ab) * (p.blocked - ab);
        sum_actual_c += ac;
    }
    {
        double n = (double)scored, mean_c = sum_actual_c / n;
        e.completions_nrmse = mean_c > 0.0 ? sqrt(se_c / n) / mean_c : sqrt(se_c / n);
        e.queue_rmse = sqrt(se_q / n);
        e.blocked_rmse = sqrt(se_b / n);
        e.composite = (e.completions_nrmse + e.queue_rmse + e.blocked_rmse) / 3.0;
    }
    return e;
}

ap_error ap_error_real(const ap_sim *s) { return score(s, 0, 0); }
ap_error ap_error_known_arrivals(const ap_sim *s) { return score(s, 1, 0); }
ap_error ap_error_shuffled(const ap_sim *s, uint32_t offset)
{
    uint32_t first = AP_WARMUP_WINDOWS, last = s->n_windows ? s->n_windows - 1u : 0u;
    uint32_t scored = last > first ? last - first : 0u;
    if (scored < 2) return score(s, 0, 0);
    offset = 1u + (offset % (scored - 1u));     /* never the true window */
    return score(s, 2, offset);
}
