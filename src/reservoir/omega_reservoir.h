/* Omega ionic-droplet reservoir: deterministic CPU reference (omega#440, Gate 1).
 *
 * A software simulation of the current-driven droplet-network model of
 * Armendarez et al., Sci. Adv. 12(30) eaed0971 (2026), Text S4. What the
 * authors' MATLAB code actually integrates, the provenance, every
 * paper-versus-code discrepancy and the Gate 2 comparison contract are in
 * docs/reservoir/RESERVOIR_REFERENCE.md. The authors' code is a correctness
 * check only; this module is not a port of it.
 *
 * Model (SI units throughout: s, V, A, F, S, m, m^2; pore densities are the
 * dimensionless numbers the reference uses):
 *   L droplets (nodes) with voltages V_i; ground nodes are held at V = 0.
 *   Each membrane (undirected edge a-b) has two pore populations, one per
 *   side: N_ab (droplet a's side) and N_ba. dV_ab = V_a - V_b.
 *     C_m = eps * A / h_m               membrane capacitance
 *     g_ab = G_u * A * (N_ab + N_ba)    membrane conductance
 *     M V' = B,  M = C_m * Laplacian (ground rows and columns removed)
 *     B_i = I_i - sum_j g_ij (V_i - V_j)
 *     alpha_ab = rate_scale / (tau0_a * e^(dV_ab / v_tau_a))
 *     Nss_ab   = n0_scale * e^(n0_rate * c_a) * e^(dV_ab / v_e_a)
 *     N_ab'    = alpha_ab * N_ab * (1 - (N_ab / Nss_ab)^Z)
 *   Parameters of side a-b are those of droplet a (the reference does the
 *   same). The constants of the two reference tasks are in
 *   omega_res_params_reference().
 *
 * State variable change (exact for N > 0, which every reference initial
 * condition satisfies): the module integrates y = ln N, so
 *     y_ab' = alpha_ab * (1 - e^w),  w = Z * (y_ab - ln Nss_ab).
 * Reasons: N spans ~40 orders of magnitude (Nss grows e-fold every 5.5 mV),
 * e^y > 0 keeps every density positive under an explicit integrator, and
 * with Z = 1.5e-4 the factor 1 - (N/Nss)^Z computed directly in binary32
 * keeps only ~3 significant digits; 1 - e^w is evaluated by a series for
 * |w| <= 1/2 without cancellation.
 *
 * Precision and arithmetic (declared, frozen in OMEGA_RES_NUMERIC_POLICY):
 *   - binary32 everywhere in the state transition, round to nearest even,
 *     no flush to zero (omega_numeric_fpenv_ok() is checked; refusal
 *     OMEGA_RES_ERR_FPENV). Build with -ffp-contract=off -fno-fast-math:
 *     no fused multiply-add, every operation rounds once.
 *   - exp, log, divide, sqrt: omega_math_exp / omega_math_log /
 *     omega_math_div / omega_math_sqrt (src/omega_numeric.h). No libm.
 *   - 1 - e^w: w * P(w) with P the degree-9 Taylor polynomial of
 *     (e^w - 1)/w in Horner form, negated, for |w| <= 0.5;
 *     1 - omega_math_exp(w) otherwise.
 *
 * Integrator: classical fourth-order Runge-Kutta, fixed step h seconds
 * (OmegaResIntegrator). Time is the integer step index n; t = n * h is only
 * reported. Stage order: k1 = f(S); k2 = f(S + (h/2) k1); k3 = f(S + (h/2) k2);
 * k4 = f(S + h k3); d = (h/6) * (((k1 + 2 k2) + 2 k3) + k4), element by
 * element, V before y. h/2 = h * 0.5 (exact), h/6 = omega_math_div(h, 6).
 * The update S' = S + d is compensated: with carry c (OmegaResState.vc/yc),
 * a = d + c; S' = fl(S + a); c' = the exact rounding error of S + a (TwoSum,
 * six binary32 operations). Without it the increments near an equilibrium
 * fall below half an ulp of S and are lost, so the error grew as h shrank
 * (measured against the binary64 oracle, one-node case: 2.1e-6 V at
 * h = 1e-5 s, 6.0e-6 V at h = 5e-6 s; with the carry 1.5e-8 V, about two
 * ulps of V, flat from h = 1e-4 s to 5e-6 s). Stages use S, not S + c.
 *
 * Order inside one derivative evaluation f:
 *   1. B_i = I_i(t) for every node, then edges in canonical order (a < b,
 *      sorted by (a, b)): dv = V_a - V_b; side ab then side ba compute
 *      alpha, ln Nss, w, y'; g = (G_u * A) * (e^y_ab + e^y_ba);
 *      q = g * dv; B_a = B_a - q; B_b = B_b + q.
 *   2. r_u = B_u * (1 / C_m) for every non-ground node u in node order.
 *   3. Laplacian (reduced, integer entries) = R R^T, R lower triangular,
 *      factored once at omega_res_config_build in binary32; forward then
 *      backward substitution with the stored reciprocals of R's diagonal.
 *   4. V'_ground = 0, so ground voltages stay exactly 0.
 *
 * Input: piecewise-linear injected current on a uniform knot grid,
 * steps_per_knot integration steps per knot interval (the reference
 * interpolates its 0.1 ms current samples linearly). At stage time
 * t_n + theta h (theta in {0, 1/2, 1}) inside interval k with phase j:
 * I = I_k + f * (I_(k+1) - I_k), f = omega_math_div(2j + 2 theta, 2 K).
 * Injected current at a ground node is ignored (the reference drops it).
 *
 * Stiffness guard: before each step the module bounds the fastest decay
 * rate lambda = max( max_edges G_u h_m (N_ab + N_ba) / eps,
 *                    max_sides alpha Z e^w )
 * (the first term is a Rayleigh-quotient bound on the eigenvalues of
 * M^-1 G; membrane area cancels) and refuses with OMEGA_RES_ERR_STIFF when
 * h * lambda > OMEGA_RES_STABILITY_LIMIT (2.0; RK4's real-axis limit is
 * ~2.785). The guard covers the diagonal blocks only. Accuracy is shown by
 * the convergence test against an independent binary64 oracle
 * (tests/reservoir), not by this guard.
 *
 * Refusals, never silent repair: invalid topology or parameters, NaN or
 * infinity anywhere, a membrane voltage |V_a - V_b| > OMEGA_RES_VM_LIMIT, y
 * outside [OMEGA_RES_Y_MIN, OMEGA_RES_Y_MAX], current outside +-OMEGA_RES_MAX_CURRENT_A, time past
 * the last knot, a state bound to another configuration. A refused step
 * leaves the caller's state unchanged.
 *
 * Bounded, no allocation, no globals: every buffer is a fixed-size member of
 * a caller-owned struct. Nothing here reads or writes an OmegaTensor; Gate 2
 * adapters copy into new tensors. Identity: SHA-256(domain || 0x00 ||
 * canonical little-endian encoding) via src/sha256.c, the same rule as
 * src/estimation/est_types.h. */
#ifndef OMEGA_RESERVOIR_H
#define OMEGA_RESERVOIR_H

#include <stddef.h>
#include <stdint.h>

#define OMEGA_RES_MAX_NODES 64u
#define OMEGA_RES_MAX_DEGREE 6u /* six-neighbour lattice; chains use <= 2 */
#define OMEGA_RES_MAX_EDGES (OMEGA_RES_MAX_NODES * OMEGA_RES_MAX_DEGREE / 2u)
#define OMEGA_RES_MAX_SIDES (2u * OMEGA_RES_MAX_EDGES)

#define OMEGA_RES_STABILITY_LIMIT 2.0f  /* max h * lambda accepted per step */
/* Voltage guard (orchestrator decision 2026-10-14, after the first Gate 2 run,
 * RESERVOIR_REFERENCE.md section 12): the refusal applies to the voltage
 * across each membrane, |V_a - V_b|, which is what the pore kinetics see.
 * Node voltages are referenced to the common ground and add up along a
 * chain, so they get no bound beyond finiteness. 1 V is a numerical guard
 * that catches blowups; it is not a physical limit. Membranes above
 * OMEGA_RES_VM_PLAUSIBLE are counted as physically implausible (the paper:
 * rupture "when voltages exceed ~200 to 300 mV"), recorded, never refused. */
#define OMEGA_RES_VM_LIMIT 1.0f      /* |V_a - V_b| above 1 V: refused */
#define OMEGA_RES_VM_PLAUSIBLE 0.3f  /* |V_a - V_b| above 0.3 V: flagged */
#define OMEGA_RES_Y_MIN (-80.0f)        /* ln N bounds: e^y stays a normal float */
#define OMEGA_RES_Y_MAX 80.0f
#define OMEGA_RES_MAX_CURRENT_A 1.0e-6f /* 1 uA; the reference uses <= 1 nA */
#define OMEGA_RES_MAX_STEP_S 1.0e-3f
#define OMEGA_RES_DIGEST_SIZE 32u

#define OMEGA_RES_DOMAIN_CONFIG "omega.reservoir.config.v1"
#define OMEGA_RES_DOMAIN_INPUT "omega.reservoir.input.v1"
#define OMEGA_RES_DOMAIN_STATE "omega.reservoir.state.v1"
#define OMEGA_RES_DOMAIN_RUN "omega.reservoir.run.v1"
#define OMEGA_RES_NUMERIC_POLICY \
    "omega.reservoir.numeric.v2: binary32 RNE no-FTZ; ffp-contract=off; " \
    "omega_math_exp/log/div/sqrt; state y=lnN; rk4 fixed step, TwoSum-compensated update; " \
    "1-e^w Taylor9 |w|<=0.5; dense Cholesky of reduced Laplacian; " \
    "refuse |Va-Vb|>1 V per membrane, node V finite only"

typedef enum {
    OMEGA_RES_OK = 0,
    OMEGA_RES_ERR_NULL = -1,
    OMEGA_RES_ERR_TOPOLOGY = -2,  /* bad node/edge count, self loop, duplicate,
                                     degree > 6, isolated node, no ground, or a
                                     connected component without a ground */
    OMEGA_RES_ERR_PARAM = -3,     /* parameter non-finite or out of its range */
    OMEGA_RES_ERR_NONFINITE = -4, /* NaN or infinity in input, state or result */
    OMEGA_RES_ERR_RANGE = -5,     /* membrane |V_a - V_b| or ln N outside the bounds */
    OMEGA_RES_ERR_STIFF = -6,     /* h * lambda above OMEGA_RES_STABILITY_LIMIT */
    OMEGA_RES_ERR_INPUT = -7,     /* input shape, current bound, time past last knot */
    OMEGA_RES_ERR_MISMATCH = -8,  /* state bound to another configuration */
    OMEGA_RES_ERR_FPENV = -9,     /* FPCR is not round-to-nearest with FZ clear */
    OMEGA_RES_ERR_SINGULAR = -10, /* reduced Laplacian or normal matrix not PD */
    OMEGA_RES_ERR_ALIAS = -11     /* input and output buffers overlap */
} OmegaResStatus;

/* ------------------------------------------------------------- topology */

/* Undirected membrane between nodes a < b (0-based). */
typedef struct {
    uint16_t a, b;
} OmegaResEdge;

/* Built only by omega_res_topology_build / omega_res_topology_chain, which
 * validate and canonicalize: edges sorted by (a, b), degree[] filled. A node
 * is a boundary node of a six-neighbour lattice when degree < 6. */
typedef struct {
    uint32_t node_count;
    uint32_t edge_count;
    OmegaResEdge edge[OMEGA_RES_MAX_EDGES];
    uint8_t degree[OMEGA_RES_MAX_NODES];
    uint8_t is_ground[OMEGA_RES_MAX_NODES];
} OmegaResTopology;

/* Edges may be listed in any order and either orientation; grounds are node
 * indices. Refuses (OMEGA_RES_ERR_TOPOLOGY) as listed in OmegaResStatus. */
OmegaResStatus omega_res_topology_build(uint32_t node_count, const OmegaResEdge *edges, uint32_t edge_count,
                                        const uint32_t *grounds, uint32_t ground_count, OmegaResTopology *out);

/* Path 0-1-...-(n-1) with one ground node (the reference Length5/10/20
 * chains are this with ground = n-1). */
OmegaResStatus omega_res_topology_chain(uint32_t node_count, uint32_t ground, OmegaResTopology *out);

/* ----------------------------------------------------------- parameters */

/* Per-node arrays hold node_count meaningful entries. All values binary32. */
typedef struct {
    uint32_t node_count;
    float eps_f_per_m;      /* permittivity, F/m (> 0) */
    float h_m;              /* membrane thickness, m (> 0) */
    float area_m2;          /* membrane area, m^2, same for every membrane (> 0) */
    float g_u_s;            /* single-pore conductance G_u, S (> 0) */
    float z;                /* insertion shape exponent Z, 0 < Z <= 1 */
    float rate_scale;       /* multiplies 1/tau0 (the reference's 1e4), > 0 */
    float n0_scale;         /* Nss prefactor (0.002), > 0 */
    float n0_rate;          /* Nss concentration slope (3.7919 per uM), finite */
    float conc_um[OMEGA_RES_MAX_NODES];  /* Alm concentration c_i, uM, >= 0 */
    float tau0_s[OMEGA_RES_MAX_NODES];   /* > 0 */
    float v_tau_v[OMEGA_RES_MAX_NODES];  /* > 0 */
    float v_e_v[OMEGA_RES_MAX_NODES];    /* > 0 */
} OmegaResParams;

typedef enum {
    OMEGA_RES_REF_NARMA = 1,  /* HyperParameterTuningOptimized.m constants */
    OMEGA_RES_REF_LORENZ = 2  /* Lorenz63SolverCustomRegression*.m constants */
} OmegaResReference;

/* The reference constants for node_count nodes, concentrations 1,2,3,1,2,3..
 * by node index (file:line for every value in RESERVOIR_REFERENCE.md). */
OmegaResStatus omega_res_params_reference(OmegaResReference which, uint32_t node_count, OmegaResParams *out);

/* ----------------------------------------------------------- integrator */

typedef struct {
    float step_s;            /* RK4 step h, 0 < h <= OMEGA_RES_MAX_STEP_S */
    uint32_t steps_per_knot; /* K: input knot spacing = K * h, K >= 1 */
} OmegaResIntegrator;

/* --------------------------------------------------------- configuration */

/* Immutable after omega_res_config_build: validated copies of topology,
 * parameters and integrator, derived constants, the Cholesky factor and the
 * content-addressed identity. Callers must not modify it. */
typedef struct {
    OmegaResTopology topo;
    OmegaResParams par;
    OmegaResIntegrator integ;
    uint32_t reduced_count;                 /* non-ground nodes */
    int16_t reduced_index[OMEGA_RES_MAX_NODES]; /* -1 for ground */
    uint16_t reduced_node[OMEGA_RES_MAX_NODES];
    float chol[OMEGA_RES_MAX_NODES * OMEGA_RES_MAX_NODES]; /* R, row-major lower */
    float chol_inv_diag[OMEGA_RES_MAX_NODES];
    float ln_n0[OMEGA_RES_MAX_NODES];       /* ln n0_scale + n0_rate * c_i */
    float ln_alpha0[OMEGA_RES_MAX_NODES];   /* ln(rate_scale / tau0_i) */
    float inv_v_tau[OMEGA_RES_MAX_NODES];
    float inv_v_e[OMEGA_RES_MAX_NODES];
    float inv_cap;                          /* 1 / (eps * A / h_m) */
    float g_unit;                           /* G_u * A */
    float lambda_unit;                      /* G_u * h_m / eps */
    float half_step, sixth_step;
    uint8_t id[OMEGA_RES_DIGEST_SIZE];      /* omega_res_config_id */
} OmegaResConfig;

OmegaResStatus omega_res_config_build(const OmegaResTopology *topo, const OmegaResParams *par,
                                      const OmegaResIntegrator *integ, OmegaResConfig *out);

/* Identity = SHA-256(OMEGA_RES_DOMAIN_CONFIG || 0x00 || encoding), encoding
 * (little-endian): u32 node_count, u32 edge_count, edge_count x (u16 a, u16 b),
 * node_count x u8 is_ground, 8 global parameter float bit patterns in struct
 * order, node_count x 4 per-node float bit patterns (c, tau0, v_tau, v_e per
 * node), f32 step_s, u32 steps_per_knot, then the bytes of
 * OMEGA_RES_NUMERIC_POLICY. -0.0 is encoded as +0.0. Equal canonical
 * configurations give equal ids whatever order the edges were listed in. */
const uint8_t *omega_res_config_id(const OmegaResConfig *cfg);

/* ---------------------------------------------------------------- state */

typedef struct {
    uint8_t config_id[OMEGA_RES_DIGEST_SIZE]; /* the configuration it belongs to */
    uint64_t step;                            /* integer time n; t = n * h */
    uint32_t node_count, side_count;
    float v[OMEGA_RES_MAX_NODES];             /* node voltages, V */
    float y[OMEGA_RES_MAX_SIDES];             /* ln N; side 2e = a->b, 2e+1 = b->a */
    float vc[OMEGA_RES_MAX_NODES];            /* update carries (see "compensated"): */
    float yc[OMEGA_RES_MAX_SIDES];            /* |c| <= 2^-24 |value|, 0 at grounds */
} OmegaResState;

typedef enum {
    OMEGA_RES_INIT_N0 = 1,   /* N_ab(0) = n0_scale e^(n0_rate c_a) (NARMA reference) */
    OMEGA_RES_INIT_CONST = 2 /* N_ab(0) = value > 0 for every side (Lorenz: 1) */
} OmegaResInit;

/* V(0) = 0 at every node, step 0. */
OmegaResStatus omega_res_state_init(const OmegaResConfig *cfg, OmegaResInit kind, float value,
                                    OmegaResState *out);

/* Pore density of one side, N = e^y (omega_math_exp). */
float omega_res_state_pore(const OmegaResState *s, uint32_t side);

/* ---------------------------------------------------------------- input */

/* current_a[k * node_count + i] = current injected into node i at knot k,
 * amperes. The table is only read. Stepping from step n needs knots
 * n / K and n / K + 1, so a table of knot_count knots supports steps
 * 0 .. (knot_count - 1) * K. */
typedef struct {
    uint32_t node_count;
    uint32_t knot_count; /* >= 2 */
    const float *current_a;
} OmegaResInput;

/* -------------------------------------------------------------- stepping */

/* Scratch for one RK4 step plus the second state buffer (double buffering). */
typedef struct {
    float b[OMEGA_RES_MAX_NODES];
    float r[OMEGA_RES_MAX_NODES];
    float kv[4][OMEGA_RES_MAX_NODES];
    float ky[4][OMEGA_RES_MAX_SIDES];
    float sv[OMEGA_RES_MAX_NODES];
    float sy[OMEGA_RES_MAX_SIDES];
    float cur[OMEGA_RES_MAX_NODES];
    OmegaResState spare;
} OmegaResWorkspace;

/* One RK4 step from *cur into *next (distinct buffers). On refusal *next is
 * unspecified and *cur untouched. */
OmegaResStatus omega_res_step(const OmegaResConfig *cfg, const OmegaResInput *in, OmegaResWorkspace *ws,
                              const OmegaResState *cur, OmegaResState *next);

/* steps RK4 steps, ping-ponging between *s and ws->spare. On success *s is
 * the state after the last step; on refusal *s is the last accepted state
 * (s->step says where it stopped) and the refusal code is returned. */
OmegaResStatus omega_res_advance(const OmegaResConfig *cfg, const OmegaResInput *in, OmegaResWorkspace *ws,
                                 OmegaResState *s, uint64_t steps);

/* Physical-plausibility record of a run (recorded, never refused): every
 * accepted state passed to omega_res_membrane_observe is counted. A
 * membrane is implausible when |V_a - V_b| > OMEGA_RES_VM_PLAUSIBLE. */
typedef struct {
    uint64_t states_observed;
    uint64_t states_implausible;     /* states with at least one implausible membrane */
    uint32_t membranes_implausible;  /* distinct membranes implausible at least once */
    float max_membrane_abs_v;        /* largest |V_a - V_b| seen */
    float max_node_abs_v;            /* largest |V_i| seen (no bound applies) */
    uint8_t edge_flag[OMEGA_RES_MAX_EDGES];
} OmegaResMembraneStats;

/* Zeroes *st. */
void omega_res_membrane_stats_init(OmegaResMembraneStats *st);

/* Adds state s to *st. Refuses NULL and a state of another configuration.
 * Does not check finiteness: pass only states the simulator accepted (as
 * omega_res_advance_observed does); a NaN voltage would go uncounted. */
OmegaResStatus omega_res_membrane_observe(const OmegaResConfig *cfg, const OmegaResState *s,
                                          OmegaResMembraneStats *st);

/* omega_res_advance, observing every accepted state into *st (NULL: none). */
OmegaResStatus omega_res_advance_observed(const OmegaResConfig *cfg, const OmegaResInput *in, OmegaResWorkspace *ws,
                                          OmegaResState *s, uint64_t steps, OmegaResMembraneStats *st);

/* Fastest decay rate bound lambda (1/s) of the stiffness guard at state s. */
OmegaResStatus omega_res_stiffness(const OmegaResConfig *cfg, const OmegaResState *s, float *lambda);

/* ------------------------------------------------------------- receipts */

/* SHA-256(OMEGA_RES_DOMAIN_INPUT || 0x00 || u32 node_count || u32 knot_count
 * || float bit patterns of the table, knot-major). */
OmegaResStatus omega_res_input_digest(const OmegaResInput *in, uint8_t out[OMEGA_RES_DIGEST_SIZE]);

/* SHA-256(OMEGA_RES_DOMAIN_STATE || 0x00 || config_id || u64 step || u32
 * node_count || u32 side_count || v, y, vc, yc bit patterns), -0.0 as +0.0. */
OmegaResStatus omega_res_state_digest(const OmegaResState *s, uint8_t out[OMEGA_RES_DIGEST_SIZE]);

/* Run receipt: binds what was run to what came out. It proves only that
 * this configuration, this input and this numeric policy produced this
 * final state on this CPU tier; agreement with the published reference is a
 * separate Gate 2 comparison, and nothing here says anything about physical
 * droplets. run_id = SHA-256(OMEGA_RES_DOMAIN_RUN || 0x00 || config_id ||
 * input_digest || u64 steps || final_state_digest). */
typedef struct {
    uint8_t config_id[OMEGA_RES_DIGEST_SIZE];
    uint8_t input_digest[OMEGA_RES_DIGEST_SIZE];
    uint8_t final_state_digest[OMEGA_RES_DIGEST_SIZE];
    uint64_t steps;
    uint8_t run_id[OMEGA_RES_DIGEST_SIZE];
} OmegaResRunReceipt;

OmegaResStatus omega_res_run_receipt(const OmegaResConfig *cfg, const OmegaResInput *in,
                                     const OmegaResState *final_state, OmegaResRunReceipt *out);

#endif /* OMEGA_RESERVOIR_H */
