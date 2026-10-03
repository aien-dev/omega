/* PRD2: mixture-capable prediction file (EXP-002D Stage A). New files only;
 * PRD1 (ty_prd.*) and the qint.v1 Gaussian scorer (ty_qcont.*) are not changed.
 * Format: docs/turing/PRD2_PREDICTION_FORMAT.md. Header "PRD2", u32 version 2,
 * u32 count, u32 first_index; record: u32 index, u32 family, u32 K, u32 reserved,
 * then K x (f64 pi, f64 loc, f64 scale). Family 0 needs K = 1 and pi = 1 exactly;
 * family 2 needs 1 <= K <= 8, each 0 < pi <= 1, |sum pi - 1| <= 1e-9. Nothing is
 * renormalized. Every refusal is TYQ_FAIL_PROTOCOL plus a reason code. */
#ifndef TURING_TY_PRD2_H
#define TURING_TY_PRD2_H

#include "turing/ty_qcont.h"

enum tyq_prd2_reason {
    PRD2_R_NONE = 0,
    PRD2_R_MAGIC, PRD2_R_VERSION, PRD2_R_SIZE, PRD2_R_COUNT, PRD2_R_INDEX,
    PRD2_R_FAMILY, PRD2_R_K, PRD2_R_RESERVED,
    PRD2_R_WEIGHT_NONFINITE, PRD2_R_WEIGHT_RANGE, PRD2_R_WEIGHT_SUM, PRD2_R_GAUSS_WEIGHT,
    PRD2_R_LOC, PRD2_R_SCALE
};
const char *tyq_prd2_reason_name(int r);

typedef struct { uint32_t n, first_index; tyq_pred *rec; } ty_prd2;

/* Family 0: comp[0] holds pi/loc/scale and K == 1 (loc/scale mirrored into p->loc/scale).
 * Family 2: comp[0..K-1]. reason may be NULL. */
int  ty_prd2_validate(const tyq_pred *p, int *reason);
int  ty_prd2_parse(const uint8_t *buf, size_t len, uint32_t first_index, uint32_t count,
                   ty_prd2 *out, int *reason);
int  ty_prd2_encode(const ty_prd2 *p, uint8_t **buf, size_t *len);
void ty_prd2_free(ty_prd2 *p);

/* qint.v1 bits of a validated PRD2 record (Gaussian delegates to ty_qcont_bits;
 * mixture = -log2 sum_j pi_j 2^-b_j, floor hit if any component was raised). */
int  ty_qcont2_bits(const tyq_pred *p, int64_t k, double *bits, int *floor_hit);
int  ty_qcont2_point_ub(const tyq_pred *p, int64_t k, int64_t *ub, int *floor_hit);
int  ty_qcont2_sum_ub(const tyq_pred *p, const int64_t *k, size_t n,
                      int64_t *ld_ub, uint64_t *floor_hits);

#endif
