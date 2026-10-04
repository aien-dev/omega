/* STAND_IN range guard (AEGIS stand-in, PHYSICS0_DISCOVERY_ENGINE.md section
 * 4.2: "stand-in range check", label STAND_IN in every receipt). Pure
 * functions over the describe record: an intervention or reset request is
 * either inside the declared bounds or refused. A refusal is itself a recorded
 * outcome (status REFUSED_RANGE) and still costs the step or episode
 * (spec 2.3). No generator knowledge. */
#ifndef PD0_GUARD_H
#define PD0_GUARD_H

#include <stdint.h>

#include "physics0/pd0_wire.h"

#define PD0_GUARD_LABEL "STAND_IN"

/* 1 if (channel, value) is inside the bounds: channel PD0_CH_NONE always ok;
 * otherwise channel < n_channels and chan_min <= value <= chan_max */
int pd0_guard_step_ok(const pd0_desc *d, uint8_t channel, int64_t value);
/* 1 if n_vals == n_obs and every value is in [reset_min, reset_max] */
int pd0_guard_reset_ok(const pd0_desc *d, const int64_t *vals, uint8_t n_vals);
/* same with one inclusive box per observed variable (spec 4 rev 3: L0 s1 and L4 differ from [-2, 2]);
 * the caller supplies the boxes, so the guard still holds no generator knowledge */
int pd0_guard_reset_ok_box(const int64_t *lo, const int64_t *hi, uint8_t n_obs, const int64_t *vals, uint8_t n_vals);
/* 1 if a post-step observed or hidden value is inside |v| <= PD0_BOUND */
int pd0_guard_in_box(int64_t v);

#endif
