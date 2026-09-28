/*
 * rx_cog_engines.h -- stand-in cognition engines for the cognitive routing
 * qualification.
 *
 * They are real computations on tasks whose right answer is known, so the
 * test can measure quality. They are not AIEN. "General" here is the largest
 * trained network in the set, named for the role it plays in the ladder.
 *
 * RECOGNIZE: label a 16-dimensional observation by a fixed nonlinear rule
 * the engines never see; they learn it from labelled examples.
 * PLAN: find a path on a grid within a length bound, or say there is none.
 */
#ifndef RX_COG_ENGINES_H
#define RX_COG_ENGINES_H

#include "runtime/rx_route.h"

#include <stdint.h>

#define COG_DIM    16
#define GRID_W     96
#define GRID_H     96
#define GRID_CELLS (GRID_W * GRID_H)
#define PATH_MAX_STEPS 2048

typedef struct {
    float x[COG_DIM];
} RecInput;

typedef struct {
    int label;
} RecOutput;

typedef struct {
    const uint8_t *blocked; /* GRID_CELLS, shared by all requests of one map */
    uint16_t start, goal;
    uint16_t bound;         /* the path may have at most this many steps */
} PlanInput;

typedef struct {
    int status;             /* 1 path, 0 proven none, -1 unknown */
    uint16_t len;
    uint16_t path[PATH_MAX_STEPS];
} PlanOutput;

enum { ENG_POLICY = 11, ENG_NEURAL = 12, ENG_GENERAL = 13, ENG_ENSEMBLE = 14,
       ENG_GREEDY = 21, ENG_FOCUSED = 22, ENG_JSPACE = 23 };

/* Ground truth. */
int rec_truth(const RecInput *in);
void rec_sample(uint64_t *rng, RecInput *in);
/* Shortest path length, or -1 when unreachable. */
int plan_truth(const PlanInput *in);
int plan_correct(const PlanInput *in, const PlanOutput *out, int truth_len);
void plan_map(uint64_t seed, uint8_t *blocked);
void plan_sample(uint64_t *rng, const uint8_t *blocked, PlanInput *in);

/* Train the learned engines (deterministic from the seed). */
void cog_engines_train(uint64_t seed, uint32_t examples, uint32_t epochs);
/* Perturb the neural module's weights (a damaged module). */
void cog_damage_neural(uint64_t seed, float scale);
void cog_restore_neural(void);

int cog_register_all(RxCogRouter *r);
RxCogEngineFn cog_engine_fn(uint32_t id);

uint64_t cog_rng(uint64_t *s);

#endif
