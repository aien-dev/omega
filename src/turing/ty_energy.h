/* TURING Yield TY-4/TY-5: measured resource intervals and conserving energy
 * attribution. docs/turing/TURING_YIELD_ENERGY_PROTOCOL_V0.md.
 *
 * Companion records only. Two NEW digest domains; no existing turing.*
 * encoding, digest or golden is touched:
 *   turing.resource_interval.v0     one metered window (DIRECT totals)
 *   turing.resource_attribution.v0  one split of a window's measured total
 * Digest = SHA-256(domain || 0x00 || OMG0 bytes), the field.c convention.
 * These digests are NOT Omega semantic ids (OSC-0B). Integers only: µJ, ns.
 *
 * Nothing here reads a meter: the window tool records raw r15 JSONL and the
 * reducer fills these structs. The checks refuse, they never repair.
 */
#ifndef TURING_TY_ENERGY_H
#define TURING_TY_ENERGY_H

#include <stddef.h>
#include <stdint.h>

#include "turing/field.h" /* turing_digest */

#define TY_DOMAIN_INTERVAL "turing.resource_interval.v0"
#define TY_DOMAIN_ATTRIBUTION "turing.resource_attribution.v0"

/* Confidence scale, strongest first. */
typedef enum {
    TY_DIRECT = 0,        /* read from the meter for exactly this interval and boundary */
    TY_COUNTER_DERIVED,   /* a measured total split by measured per-participant counters */
    TY_CAUSAL_RUNTIME,    /* split by the runtime's own causal record of who ran */
    TY_COUNTERFACTUAL,    /* taken from a different window (solo or idle) */
    TY_MODELLED,          /* from a fitted or cost model, not read for this interval */
    TY_UNATTRIBUTED,      /* measured but assigned to nobody */
    TY_CONF__COUNT
} ty_conf;

/* physics FORGE V2 energy_source (forge/v2/forge_substrate_v2.h:192-194). */
#define TY_FORGE_ENERGY_NOT_MEASURED 0u
#define TY_FORGE_ENERGY_MEASURED 1u
#define TY_FORGE_ENERGY_ESTIMATED 2u

const char *ty_conf_name(ty_conf c);
/* DIRECT, COUNTER_DERIVED -> MEASURED; CAUSAL_RUNTIME, COUNTERFACTUAL,
 * MODELLED -> ESTIMATED; UNATTRIBUTED (or out of range) -> NOT_MEASURED. */
uint32_t ty_conf_forge(ty_conf c);

typedef enum {
    TYE_OK = 0,
    TYE_E_SENSOR,              /* meter unavailable or not ok at an edge */
    TYE_E_WRAP,                /* overflow flag set or a counter decreased */
    TYE_E_HEADROOM,            /* counter too close to the 32-bit mJ wrap */
    TYE_E_SAMPLING_GAP,        /* 10 Hz telemetry gap > 300 ms or < 90% of samples */
    TYE_E_PARTIAL_RUN,         /* workload failed, unpinned, early exit or < 98% of plan */
    TYE_E_INTERVAL_MISMATCH,   /* workload interval not inside the meter window */
    TYE_E_MULTIPLEXED,         /* PMU running < 99.9% of enabled */
    TYE_E_OVERLAP,             /* two meter windows overlap in time */
    TYE_E_MISSING_IDLE,        /* no idle baseline where one is required */
    TYE_E_DOUBLE_COUNT,        /* a participant or a source window used twice */
    TYE_E_OVER_ALLOCATED,      /* allocated > measured beyond uncertainty */
    TYE_E_NEGATIVE_SHARE,      /* a share below zero beyond uncertainty */
    TYE_E_MISSING_PARTICIPANT, /* a participant of the window has no share */
    TYE_E_UNKNOWN_PARTICIPANT, /* a share names no participant of the window */
    TYE_E_CONDITION,           /* windows of the wrong condition or round */
    TYE_E_ARG,
    TYE_STATUS__COUNT
} ty_energy_status;

const char *ty_energy_status_name(ty_energy_status s);

/* Meter channels used (r15_measure energy index 0..2). */
enum { TY_CH_PKG = 0, TY_CH_CPU_E = 1, TY_CH_CPU_P = 2, TY_NCH = 3 };

#define TY_MAX_PART 4
#define TY_RESOLUTION_UJ 1000u            /* SPBM step: 1 mJ (not accuracy) */
#define TY_SAMPLE_PERIOD_NS 100000000u    /* r15 sampler, 10 Hz */
#define TY_MAX_GAP_NS 300000000u
#define TY_WRAP_UJ 4294967296000ull       /* 2^32 mJ */

typedef struct {
    char tag[8];          /* "A", "B", ... */
    char rz[32];          /* oma_rz_impl id */
    char cpus[32];
    uint64_t go_ns, t0_ns, t1_ns, done_ns, planned_ns;
    uint64_t calls;
    uint64_t cycles, inst, l2refill; /* X925 PMU raw sums, never scaled */
    int pinned, oracle_ok, exited_ok, pmu_ok, multiplexed;
} ty_participant;

typedef struct {
    char run_id[64], config[16], cond[8];
    uint32_t round, pos;
    uint64_t t0_ns, t1_ns;              /* meter edges (CLOCK_MONOTONIC) */
    uint64_t planned_ns;
    int spbm_ok0, spbm_ok1;             /* raw spbm_ok at each edge */
    int overflow;                       /* any overflow_raw flag at either edge */
    uint64_t e0_uj[TY_NCH], e1_uj[TY_NCH];
    uint64_t n_samples, max_gap_ns;
    uint64_t foreign_busy_ms;           /* busy time on cores the window did not use */
    uint32_t max_temp_mc;
    size_t npart;
    ty_participant part[TY_MAX_PART];
} ty_interval;

/* Validity of one window (every rule of protocol section 8 that needs only
 * this window). */
ty_energy_status ty_interval_check(const ty_interval *w);
/* Energy on channel ch scaled to the planned length, µJ. TYE_OK or the reason. */
ty_energy_status ty_interval_energy(const ty_interval *w, int ch, int64_t *uj);
/* Pairwise overlap of meter windows; on TYE_E_OVERLAP *a, *b name the pair. */
ty_energy_status ty_intervals_disjoint(const ty_interval *const *w, size_t n, size_t *a, size_t *b);
/* I = E_AB - E_A - E_B + E_0 on channel ch, after checking conditions, round
 * and validity of all four windows. idle may be NULL (-> TYE_E_MISSING_IDLE). */
ty_energy_status ty_interaction(const ty_interval *idle, const ty_interval *a, const ty_interval *b,
                         const ty_interval *ab, int ch, int64_t *I);

/* ---- attribution ---- */
#define TY_MAX_SHARE 8
#define TY_MAX_SRC 8
#define TY_WHO_IDLE "IDLE"

typedef struct {
    char who[8];          /* a participant tag, or TY_WHO_IDLE */
    int64_t uj;
    ty_conf conf;
} ty_share;

typedef struct {
    char method[32];      /* e.g. "solo_counterfactual.v0", "counter_model.v0" */
    int ch;
    uint64_t tolerance_uj;        /* uncertainty used by the conservation check */
    int all_unattributed;         /* 1: no split is claimed at all */
    size_t nshare;
    ty_share share[TY_MAX_SHARE];
    size_t nsrc;
    turing_digest src[TY_MAX_SRC]; /* intervals the shares were taken from */
    /* filled by ty_attribution_check */
    turing_digest measured;
    int64_t measured_uj, allocated_uj, remainder_uj; /* remainder is UNATTRIBUTED */
    ty_energy_status verdict;
    ty_conf overall;              /* weakest confidence present */
} ty_attribution;

/* Conservation check against the measured window. Fills measured, sums and
 * verdict; returns the verdict. Refuses: invalid window, double count,
 * missing idle baseline, missing or unknown participant, negative share and
 * allocated > measured + tolerance. A remainder >= -tolerance is recorded as
 * UNATTRIBUTED, never spread over the participants. */
ty_energy_status ty_attribution_check(ty_attribution *at, const ty_interval *w);

/* ---- records ---- */
int ty_interval_digest(const ty_interval *w, turing_digest *out);
int ty_attribution_digest(const ty_attribution *at, turing_digest *out);
void ty_energy_hex(const turing_digest *d, char out[65]);

#endif
