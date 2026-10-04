#include "physics0/pd0_guard.h"

int pd0_guard_step_ok(const pd0_desc *d, uint8_t channel, int64_t value) {
    if (channel == PD0_CH_NONE) return 1;
    if (channel >= d->n_channels) return 0;
    return value >= d->chan_min[channel] && value <= d->chan_max[channel];
}

int pd0_guard_reset_ok(const pd0_desc *d, const int64_t *vals, uint8_t n_vals) {
    if (n_vals != d->n_obs) return 0;
    for (unsigned i = 0; i < n_vals; i++)
        if (vals[i] < d->reset_min || vals[i] > d->reset_max) return 0;
    return 1;
}

int pd0_guard_in_box(int64_t v) { return v <= PD0_BOUND && v >= -PD0_BOUND; }

int pd0_guard_reset_ok_box(const int64_t *lo, const int64_t *hi, uint8_t n_obs, const int64_t *vals, uint8_t n_vals) {
    if (n_vals != n_obs) return 0;
    for (unsigned i = 0; i < n_vals; i++)
        if (vals[i] < lo[i] || vals[i] > hi[i]) return 0;
    return 1;
}
