/* Test-only bridge between the planner (verifier formats, pd0_fmt.h) and the
 * PD-0 world (substrate formats, physics0/pd0_wire.h). The two header families
 * define different PD0_MAX_* limits, so they never meet in one translation
 * unit; this header carries plain arrays only. The bridge links the generator
 * on purpose (it is the HARNESS side of the efficiency comparison) and is never
 * linked into the planner objects (purity check in mk/physics0-planner.mk). */
#ifndef PD0P_BRIDGE_H
#define PD0P_BRIDGE_H
#include <stdint.h>

#define PB_MAX_OBS 8
#define PB_MAX_CH 4
#define PB_MAX_EX 16     /* exponent entries: n_vars variables then channels at index n_vars + c (verifier layout) */
#define PB_MAX_TERMS 64
#define PB_MAX_EQ 12
#define PB_STEPS 20

typedef struct {
    uint8_t n_obs, n_channels;
    int64_t chan_min[PB_MAX_CH], chan_max[PB_MAX_CH];
    int64_t reset_min[PB_MAX_OBS], reset_max[PB_MAX_OBS];
    uint32_t episode_max_steps, budget_steps, budget_episodes;
    int64_t dt_micro;
} pb_bounds;

typedef struct { uint8_t target; uint16_t n_terms; int64_t coef[PB_MAX_TERMS]; uint8_t expo[PB_MAX_TERMS][PB_MAX_EX]; } pb_eq;
typedef struct { uint8_t n_vars, n_latent, n_channels, n_eq; pb_eq eq[PB_MAX_EQ]; } pb_rel;

typedef struct { uint8_t n_obs, n_steps; int64_t reset[PB_MAX_OBS]; uint8_t channel[PB_STEPS]; int64_t value[PB_STEPS]; } pb_sched;

typedef struct pb_world pb_world;   /* opaque: holds the in-process world */

/* Open an in-process world for (level 1..6, seed); fills bounds and the true
 * relation in the verifier's exponent layout. Returns NULL on error. */
pb_world *pb_open(int level, uint64_t seed, pb_bounds *b, pb_rel *truth);
void pb_close(pb_world *w);
/* Run one schedule: reset then n_steps steps. after[s][i] is the observed
 * state after step s. Returns the number of OK steps (fewer than n_steps when
 * the episode ended early), or -1 on a refused reset / budget exhaustion. */
int pb_run(pb_world *w, const pb_sched *s, int64_t after[PB_STEPS][PB_MAX_OBS]);
uint32_t pb_steps_used(const pb_world *w);
#endif
