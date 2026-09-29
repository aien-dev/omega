/* POLYGLOT-0 lane B3: Mojo candidates for Omega-X. spec/polyglot-0.md
 *
 * The kernels (pack validation/conversion and run) are written in Mojo 1.0
 * (polyglot/mojo/omx_mojo.mojo), compiled with `mojo build --emit object` and
 * exported with the C ABI (@export + abi("C")). This file only adapts them to
 * the MA-3 oma_rz_impl ABI: shape check, allocation (oma_rz_alloc, so
 * oma_rz_free works) and plan bookkeeping. The linked Mojo object needs no
 * Mojo runtime library and no libpython; its only external symbol is memset.
 *
 * Representations:
 *   MJ1_sdot : int8 row-major, same layout as MA-3 R1_sdot.
 *   MJ2c_crumb: 2-bit crumbs, same layout and code as MA-3 R2c_crumb.
 */
#include "polyglot/omx_lang.h"

#include <stdint.h>
#include <string.h>

/* Mojo exports (C ABI). Mojo `Int` is a 64-bit signed integer; every size
 * passed here is already bounded by oma_rz_check_shape. */
extern int64_t omx_mj_i8_pack(const int8_t *w, int64_t total, int8_t *dst);
extern int32_t omx_mj_i8_run(const int8_t *w, int64_t m, int64_t n, const int8_t *x, int32_t *y);
extern int64_t omx_mj_crumb_pack(const int8_t *w, int64_t m, int64_t n, uint8_t *dst);
extern int32_t omx_mj_crumb_run(const int8_t *w, int64_t m, int64_t n, const int8_t *x, int32_t *y);
extern int32_t omx_mj_null(const int8_t *w, int64_t m, int64_t n, const int8_t *x, int32_t *y);

static int pack_i8(oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    int rc = oma_rz_check_shape(m, n, OMA_RZ_MAX_N);
    if (rc) return rc;
    if (!p || !w) return OMA_RZ_E_ARG;
    memset(p, 0, sizeof *p);
    int8_t *buf = oma_rz_alloc(m * n);
    if (!buf) return OMA_RZ_E_NOMEM;
    int64_t nnz = omx_mj_i8_pack(w, (int64_t)(m * n), buf);
    if (nnz < 0) {
        oma_rz_free(&(oma_rz_plan){.mem = buf});
        return OMA_RZ_E_TRIT;
    }
    p->m = m;
    p->n = n;
    p->mem = buf;
    p->weight_bytes = m * n;
    p->footprint_bytes = m * n;
    p->nnz = (uint64_t)nnz;
    return OMA_RZ_OK;
}

static int run_i8(const oma_rz_plan *p, const int8_t *x, int32_t *y) {
    if (!p || !p->mem || !x || !y) return OMA_RZ_E_ARG;
    omx_mj_i8_run(p->mem, (int64_t)p->m, (int64_t)p->n, x, y);
    return OMA_RZ_OK;
}

static int pack_crumb(oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    int rc = oma_rz_check_shape(m, n, OMA_RZ_MAX_N);
    if (rc) return rc;
    if (!p || !w) return OMA_RZ_E_ARG;
    memset(p, 0, sizeof *p);
    size_t row_bytes = ((n + 63u) / 64u) * 16u;
    uint8_t *buf = oma_rz_alloc(m * row_bytes);
    if (!buf) return OMA_RZ_E_NOMEM;
    int64_t nnz = omx_mj_crumb_pack(w, (int64_t)m, (int64_t)n, buf);
    if (nnz < 0) {
        oma_rz_free(&(oma_rz_plan){.mem = buf});
        return OMA_RZ_E_TRIT;
    }
    p->m = m;
    p->n = n;
    p->mem = buf;
    p->weight_bytes = m * row_bytes;
    p->footprint_bytes = m * row_bytes;
    p->nnz = (uint64_t)nnz;
    return OMA_RZ_OK;
}

static int run_crumb(const oma_rz_plan *p, const int8_t *x, int32_t *y) {
    if (!p || !p->mem || !x || !y) return OMA_RZ_E_ARG;
    omx_mj_crumb_run(p->mem, (int64_t)p->m, (int64_t)p->n, x, y);
    return OMA_RZ_OK;
}

/* Null kernel through the same wrapper shape: measures the C -> Mojo call
 * boundary only (not registered as a candidate). */
int omx_mojo_null_run(const oma_rz_plan *p, const int8_t *x, int32_t *y) {
    if (!p || !p->mem || !x || !y) return OMA_RZ_E_ARG;
    return (int)omx_mj_null(p->mem, (int64_t)p->m, (int64_t)p->n, x, y);
}

/* Code facts for the bench (review G-B2): the realization is the C adapter
 * plus the Mojo kernels it calls (the kernels call nothing but memset). */
#define OMX_FN(f) ((void (*)(void))(f))
static void code_i8(omx_code_desc *d) {
    memset(d, 0, sizeof *d);
    d->part[0] = (omx_code_part){"run_i8 (C adapter)", OMX_FN(run_i8), 0};
    d->part[1] = (omx_code_part){"omx_mj_i8_run (Mojo)", OMX_FN(omx_mj_i8_run), 0};
    d->part[2] = (omx_code_part){"pack_i8 (C adapter)", OMX_FN(pack_i8), 0};
    d->part[3] = (omx_code_part){"omx_mj_i8_pack (Mojo)", OMX_FN(omx_mj_i8_pack), 0};
    d->nparts = 4;
    d->rule = "C adapter run + pack and the Mojo kernels they call (symbol sizes)";
}
static void code_crumb(omx_code_desc *d) {
    memset(d, 0, sizeof *d);
    d->part[0] = (omx_code_part){"run_crumb (C adapter)", OMX_FN(run_crumb), 0};
    d->part[1] = (omx_code_part){"omx_mj_crumb_run (Mojo)", OMX_FN(omx_mj_crumb_run), 0};
    d->part[2] = (omx_code_part){"pack_crumb (C adapter)", OMX_FN(pack_crumb), 0};
    d->part[3] = (omx_code_part){"omx_mj_crumb_pack (Mojo)", OMX_FN(omx_mj_crumb_pack), 0};
    d->nparts = 4;
    d->rule = "C adapter run + pack and the Mojo kernels they call (symbol sizes)";
}

static const oma_rz_impl omx_mojo_i8 = {
    "MJ1_sdot", "Mojo 1.0: int8 W row-major, SDOT 4 rows x 64 B, 8 accumulators", "binary", 1, 0,
    OMA_RZ_MAX_N, pack_i8, run_i8};
static const oma_rz_impl omx_mojo_crumb = {
    "MJ2c_crumb", "Mojo 1.0: 2-bit crumbs (R2c layout), shift-pair decode -> SDOT", "crumb2", 1, 0,
    OMA_RZ_MAX_N, pack_crumb, run_crumb};

const omx_candidate omx_lane_mojo[] = {
    {&omx_mojo_i8, "mojo", "mojo 1.0.0", 0, 0, "polyglot/mojo/omx_mojo.mojo", code_i8},
    {&omx_mojo_crumb, "mojo", "mojo 1.0.0", 0, 0, "polyglot/mojo/omx_mojo.mojo", code_crumb},
};
const size_t omx_lane_mojo_count = sizeof omx_lane_mojo / sizeof omx_lane_mojo[0];
