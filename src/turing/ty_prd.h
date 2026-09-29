/* PRD1 prediction file reader/validator/encoder (little-endian).
 * Header: "PRD1", u32 version = 1. Then one record per held-out index:
 * u32 index, u32 family, f64 loc, f64 scale. Wave 1 accepts family 0 only;
 * every other family and every malformed byte pattern is TYQ_FAIL_PROTOCOL. */
#ifndef TURING_TY_PRD_H
#define TURING_TY_PRD_H

#include "turing/ty_qcont.h"

typedef struct { uint32_t n, first_index; tyq_pred *rec; } ty_prd;

/* Refusals: family != 0, non-finite loc or scale, scale <= 0. */
int  ty_prd_validate(const tyq_pred *p);
/* The indices must be exactly first_index .. first_index+count-1, in order. */
int  ty_prd_parse(const uint8_t *buf, size_t len, uint32_t first_index, uint32_t count, ty_prd *out);
int  ty_prd_read(const char *path, uint32_t first_index, uint32_t count, ty_prd *out);
int  ty_prd_encode(const ty_prd *p, uint8_t **buf, size_t *len);
void ty_prd_free(ty_prd *p);

#endif
