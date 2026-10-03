/* PD-0 randomness (spec PD0_HIDDEN_EQUATION_BENCHMARK.md section 3).
 *
 * SplitMix64 with Vigna's published constants (increment 0x9E3779B97F4A7C15,
 * mixers 0xBF58476D1CE4E5B9 and 0x94D049BB133111EB). The same constants are
 * already in this repo at src/turing/history_selector.c:23-28
 * (turing_splitmix64) and src/compiler/model/osc_model_gen.c:44; the test
 * tests/physics0/test_pd0_world.c checks this stream against
 * turing_splitmix64 symbol for symbol, which is the "record the check"
 * the spec asks for (section 3, first bullet).
 *
 * stream(seed, tag) = SplitMix64 seeded with seed XOR fnv1a64(tag).
 * uniform: next() % 1_000_001 (micro-units in [0, 1.0]).
 * const draw in [lo, hi]: lo + ((hi - lo) * u) / 1_000_000.
 * Irwin-Hall noise (four uniforms, sd of the sum 577_350 micro):
 *   e = ((u1+u2+u3+u4) - 2_000_000) * sigma / 577_350.
 * Shared by the world (constants, noise, null) and the harness (scorer,
 * planner seeds). Contains no generator knowledge. */
#ifndef PD0_RNG_H
#define PD0_RNG_H

#include <stdint.h>

#define PD0_MICRO 1000000LL
#define PD0_IH_SD 577350LL   /* sd of the sum of four uniforms on [0, 1e6] */

/* same pedantic-safe spelling as ty_math.h:30 (ty_u128) */
__extension__ typedef __int128 pd0_i128;

typedef struct { uint64_t s; } pd0_rng;

uint64_t pd0_fnv1a64(const char *tag);
uint64_t pd0_splitmix64(uint64_t *s);
void     pd0_stream(pd0_rng *r, uint64_t seed, const char *tag);
uint64_t pd0_next(pd0_rng *r);
int64_t  pd0_uniform(pd0_rng *r);                       /* 0 .. 1_000_000 */
int64_t  pd0_const(pd0_rng *r, int64_t lo, int64_t hi); /* micro-units */
int64_t  pd0_noise(pd0_rng *r, int64_t sigma_micro);
/* product rescale: (a*b)/1e6 in 128-bit, truncating toward zero (spec 2.1) */
int64_t  pd0_mul(int64_t a, int64_t b);
int64_t  pd0_div(int64_t a, int64_t b);                 /* truncating, b != 0 */

#endif
