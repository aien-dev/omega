/*
 * cl_crumb.h -- Omega's reader for Crumb v1 learner-visible bytes ("CRB1").
 *
 * The format is owned by the `crumbs` crate (aien-sovereign-core,
 * crates/crumbs/src/visible.rs); this is a conforming consumer. Omega never
 * computes crumb identities and never sees anything but these bytes:
 *
 *   "CRB1" | schema u16 (=1) | encoding u8 (1 decimal UTF-8, 2 raw LE) | flags u8
 *   | in_arity u8 | out_arity u8 | in_lane_bytes u8 | out_lane_bytes u8 | n u32
 *   | n x (u32 len, input bytes, u32 len, output bytes) | [budget 4 x u32]
 *
 * Decoding is strict (canonical decimal, exact lengths, no trailing bytes).
 * Errors are bare codes, numbered and ordered as in aien-protocols
 * specs/crumb-visible/CRUMB_READER_CONTRACT.md 1.0.0.
 */
#ifndef CL_CRUMB_H
#define CL_CRUMB_H

#include "cl_common.h"

#define CL_CRUMB_MAX_LANES 8
#define CL_CRUMB_MAX_EXAMPLES 64
#define CL_CRUMB_MAX_FIELD 168 /* bytes per example field: 8 lanes x 21 */
#define CL_ENC_DECIMAL 1
#define CL_ENC_RAW_LE 2

typedef struct {
    uint8_t encoding;
    uint8_t in_arity, out_arity;
    uint8_t in_lane_bytes, out_lane_bytes;
    uint32_t n;
    uint64_t in[CL_CRUMB_MAX_EXAMPLES][CL_CRUMB_MAX_LANES];
    uint64_t out[CL_CRUMB_MAX_EXAMPLES][CL_CRUMB_MAX_LANES];
    bool has_budget;
    uint32_t budget_max_candidates, budget_max_depth, budget_max_oracle_queries, budget_max_program_ops;
} ClCrumb;

typedef enum {
    CL_CRUMB_OK = 0,
    CL_CRUMB_ERR_MAGIC = -1,
    CL_CRUMB_ERR_VERSION = -2,
    CL_CRUMB_ERR_SHAPE = -3,
    CL_CRUMB_ERR_LANE = -4,
    CL_CRUMB_ERR_LENGTH = -5,
    CL_CRUMB_ERR_NONCANONICAL = -6,
} ClCrumbStatus;

int cl_crumb_decode(const uint8_t *buf, size_t len, ClCrumb *out);

/* Output mask implied by the output lane encoding. */
uint64_t cl_crumb_out_mask(const ClCrumb *c);

#endif /* CL_CRUMB_H */
