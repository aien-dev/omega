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
#define PD0_L6_REDRAW_CAP 1000   /* spec 4.1 rev 3 */

/* one declared constant range (micro), in draw order; the single table the
 * generator draws from and the oracle range check reads (spec 4.1) */
typedef struct { const char *name; int64_t lo, hi; } pd0_crange;

typedef struct {
    int level;
    int64_t k[PD0_MAX_CONST];   /* constants in draw order (micro) */
    int64_t sigma_obs;          /* L5 observation noise, else 0 */
    int64_t sigma_null;         /* NULL world draw sd, else 0 */
    int n_hidden;               /* L6: 1 */
    int redraws;                /* L6: m,q redraws used by the 0.7*k stability rule (spec 4.1 rev 3) */
    int invalid;                /* L6: 1 if the redraw cap was reached: the seed is invalid and skipped */
} pd0_gen;

const char *pd0_gen_level_name(int level);
/* describe record for a level (spec table section 4) */
int pd0_gen_desc(int level, pd0_desc *d);
/* declared constant ranges of a level in draw order; returns the count (0: none) */
int pd0_gen_const_table(int level, const pd0_crange **t);
/* reset box of observed variable var (spec 4 rev 3) and scoring boxes (spec 6.1 rev 3:
 * in-box = reset box, extrap = 1.5 x it); the describe record carries variable 0 only */
void pd0_gen_reset_box(int level, int var, int64_t *lo, int64_t *hi);
void pd0_gen_score_box(int level, int var, int extrap, int64_t *lo, int64_t *hi);
/* draw the level's constants from the "const" stream of seed; returns -1 for an invalid
 * L6 seed (redraw cap reached, g->invalid set, g->redraws recorded) */
int pd0_gen_init(pd0_gen *g, int level, uint64_t seed);
int pd0_gen_init_cap(pd0_gen *g, int level, uint64_t seed, int redraw_cap); /* cap is a parameter for tests */
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
