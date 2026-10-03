/* PD-0 hidden generators (spec section 4 and 9.1 NC-1). THE ONLY place the
 * equations live. Included by pd0_world.c (world process), pd0_oracle.c and
 * pd0_score.c (harness side). The learner, ladder checker and planner must
 * never include this header or link pd0_gen.o: tests/physics0/isolation.sh
 * greps their symbol tables for every pd0_gen_* symbol (spec G5). */
#ifndef PD0_GEN_H
#define PD0_GEN_H

#include <stdint.h>

#include "physics0/pd0_relation.h"
#include "physics0/pd0_rng.h"
#include "physics0/pd0_wire.h"

enum { PD0_L0 = 0, PD0_L1, PD0_L2, PD0_L3, PD0_L4, PD0_L5, PD0_L6, PD0_LEVEL_NULL = 7, PD0_LEVELS = 8 };
#define PD0_MAX_CONST 4

typedef struct {
    int level;
    int64_t k[PD0_MAX_CONST];   /* constants in draw order (micro) */
    int64_t sigma_obs;          /* L5 observation noise, else 0 */
    int64_t sigma_null;         /* NULL world draw sd, else 0 */
    int n_hidden;               /* L6: 1 */
} pd0_gen;

const char *pd0_gen_level_name(int level);
/* describe record for a level (spec table section 4) */
int pd0_gen_desc(int level, pd0_desc *d);
/* draw the level's constants from the "const" stream of seed */
int pd0_gen_init(pd0_gen *g, int level, uint64_t seed);
/* one tick: old state in, new state out (explicit Euler, simultaneous).
 * u[] has one entry per channel (undriven channels 0). noise/null streams are
 * the world's; obs_out receives what the learner sees (state + noise, or the
 * null draw). Returns 1 if any observed or hidden variable left +-PD0_BOUND
 * (state_out then holds the offending values but must not be revealed). */
int pd0_gen_step(const pd0_gen *g, const int64_t *s_in, int64_t h_in, const int64_t *u, int64_t dt,
                 int64_t *s_out, int64_t *h_out, int64_t *obs_out, pd0_rng *noise, pd0_rng *null);
/* observation of a reset (noise for L5, else the state) */
void pd0_gen_observe_reset(const pd0_gen *g, const int64_t *s, int64_t *obs_out, pd0_rng *noise);
/* the true relation in PDLAW1 delta form (coefficients in micro, dt folded
 * in), for the oracle (section 4.2 V1) and the 6.3 structure check.
 * NULL world: 0 equations. */
int pd0_gen_true_relation(const pd0_gen *g, int64_t dt, pd0_relation *out);
/* S* of section 6.2 */
int pd0_gen_true_size(int level);

#endif
