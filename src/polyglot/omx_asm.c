/* POLYGLOT-0 lane B1: hand AArch64 assembly candidates for Omega-X.
 * spec/polyglot-0.md. Run kernels are assembly (src/polyglot/asm/omx_sdot.S, omx_crumb.S);
 * pack and validation are C here (allowed by the lane brief) and reproduce
 * the MA-3 packed forms exactly:
 *   asm_sdot : int8 row-major, no padding (same form as R1_sdot)
 *   asm_crumb: 2-bit crumbs, 64-weight chunks of 16 bytes (same form as
 *              R2c_crumb)
 * Error contract (section 1): E_ARG for NULL / bad shape / n > max_n (shape
 * is checked before any weight is read), E_TRIT for a weight outside
 * {-1,0,+1}, E_NOMEM, E_OVERFLOW. On any error *p is left all-zero, so
 * oma_rz_free(p) is safe and no partial plan is kept.
 *
 * Derivation (section 5): the assembly was written by hand from the
 * algorithm; gcc -S output of the MA-3 C kernels was not generated or read.
 */
#include "polyglot/omx_lang.h"

#include <stddef.h>
#include <string.h>

/* The assembly reads m, n, mem at fixed offsets. */
_Static_assert(offsetof(oma_rz_plan, m) == 0, "asm ABI: plan.m at 0");
_Static_assert(offsetof(oma_rz_plan, n) == 8, "asm ABI: plan.n at 8");
_Static_assert(offsetof(oma_rz_plan, mem) == 16, "asm ABI: plan.mem at 16");
_Static_assert(sizeof(size_t) == 8 && sizeof(void *) == 8, "asm ABI: LP64");

int omx_asm_sdot_run(const oma_rz_plan *p, const int8_t *x, int32_t *y);
int omx_asm_crumb_run(const oma_rz_plan *p, const int8_t *x, int32_t *y);

static int pack_begin(oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    if (!p) return OMA_RZ_E_ARG;
    memset(p, 0, sizeof *p);
    int rc = oma_rz_check_shape(m, n, OMA_RZ_MAX_N);
    if (rc) return rc;
    if (!w) return OMA_RZ_E_ARG;
    return oma_rz_validate(w, m, n);
}

static int pack_sdot(oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    int rc = pack_begin(p, w, m, n);
    if (rc) return rc;
    int8_t *buf = oma_rz_alloc(m * n);
    if (!buf) return OMA_RZ_E_NOMEM;
    memcpy(buf, w, m * n);
    uint64_t nnz = 0;
    for (size_t i = 0; i < m * n; i++) nnz += (w[i] != 0);
    p->m = m;
    p->n = n;
    p->mem = buf;
    p->weight_bytes = m * n;
    p->footprint_bytes = m * n;
    p->nnz = nnz;
    return OMA_RZ_OK;
}

static int pack_crumb(oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    int rc = pack_begin(p, w, m, n);
    if (rc) return rc;
    size_t chunks = (n + 63u) / 64u, row_bytes = chunks * 16u;
    if (row_bytes != 0 && m > SIZE_MAX / row_bytes) return OMA_RZ_E_OVERFLOW;
    uint8_t *buf = oma_rz_alloc(m * row_bytes);
    if (!buf) return OMA_RZ_E_NOMEM;
    uint64_t nnz = 0;
    for (size_t i = 0; i < m; i++) {
        const int8_t *row = w + i * n;
        uint8_t *dst = buf + i * row_bytes;
        for (size_t c = 0; c < chunks; c++)
            for (unsigned j = 0; j < 16; j++) {
                unsigned byte = 0;
                for (unsigned k = 0; k < 4; k++) {
                    size_t col = c * 64u + 16u * k + j;
                    int v = col < n ? row[col] : 0;
                    nnz += (v != 0);
                    byte |= ((unsigned)v & 3u) << (2u * k);
                }
                dst[c * 16u + j] = (uint8_t)byte;
            }
    }
    p->m = m;
    p->n = n;
    p->mem = buf;
    p->weight_bytes = m * row_bytes;
    p->footprint_bytes = m * row_bytes;
    p->nnz = nnz;
    return OMA_RZ_OK;
}

const oma_rz_impl omx_rz_asm_sdot = {
    "asm_sdot", "int8 W row-major, hand AArch64 asm SDOT 4 rows x 64 B, masked overlap tail", "binary", 1, 0,
    OMA_RZ_MAX_N, pack_sdot, omx_asm_sdot_run};
const oma_rz_impl omx_rz_asm_crumb = {
    "asm_crumb", "2-bit crumbs (2 b/w), hand AArch64 asm shift-pair decode -> SDOT", "crumb2", 1, 0,
    OMA_RZ_MAX_N, pack_crumb, omx_asm_crumb_run};

const omx_candidate omx_lane_asm[] = {
    {&omx_rz_asm_sdot, "asm-aarch64", "gnu-as 2.42", 0, 0, "src/polyglot/asm/omx_sdot.S"},
    {&omx_rz_asm_crumb, "asm-aarch64", "gnu-as 2.42", 0, 0, "src/polyglot/asm/omx_crumb.S"},
};
const size_t omx_lane_asm_count = sizeof omx_lane_asm / sizeof omx_lane_asm[0];
