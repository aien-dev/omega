/* Omega mixed algebra MA-2: shared realization helpers and registry. */
#include "algebra/realize_common.h"

#include <stdlib.h>
#include <string.h>

static const oma_rz_impl *const k_impls[] = {
    &oma_rz_r1_plain, &oma_rz_r1_sdot, &oma_rz_r1_sdot_il, &oma_rz_r1_smmla,
    &oma_rz_r2_bitplane, &oma_rz_r2b_lut, &oma_rz_r2c_crumb,
    &oma_rz_r3_sparse,
    &oma_rz_r4_rns,
    &oma_rz_r5_dense5,
};

size_t oma_rz_count(void) { return sizeof k_impls / sizeof k_impls[0]; }

const oma_rz_impl *oma_rz_get(size_t i) { return i < oma_rz_count() ? k_impls[i] : NULL; }

const oma_rz_impl *oma_rz_find(const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; i < oma_rz_count(); i++)
        if (strcmp(k_impls[i]->id, id) == 0) return k_impls[i];
    return NULL;
}

void oma_rz_free(oma_rz_plan *p) {
    if (!p) return;
    free(p->mem);
    free(p->aux);
    free(p->scratch);
    memset(p, 0, sizeof *p);
}

const char *oma_rz_strerror(int rc) {
    switch (rc) {
    case OMA_RZ_OK: return "ok";
    case OMA_RZ_E_ARG: return "bad argument";
    case OMA_RZ_E_TRIT: return "weight outside {-1,0,+1}";
    case OMA_RZ_E_NOMEM: return "out of memory";
    case OMA_RZ_E_OVERFLOW: return "size overflow";
    default: return "unknown error";
    }
}

int oma_rz_check_shape(size_t m, size_t n, size_t max_n) {
    if (m == 0 || n == 0) return OMA_RZ_E_ARG;
    if (n > max_n || n > OMA_RZ_MAX_N) return OMA_RZ_E_ARG;
    if (m > SIZE_MAX / n / 4u) return OMA_RZ_E_OVERFLOW;
    return OMA_RZ_OK;
}

int oma_rz_validate(const int8_t *w, size_t m, size_t n) {
    if (!w) return OMA_RZ_E_ARG;
    size_t total = m * n;
    for (size_t i = 0; i < total; i++) {
        int8_t v = w[i];
        if (v < -1 || v > 1) return OMA_RZ_E_TRIT;
    }
    return OMA_RZ_OK;
}

void *oma_rz_alloc(size_t bytes) {
    if (bytes == 0) bytes = 64;
    size_t rounded = (bytes + 63u) & ~(size_t)63u;
    if (rounded < bytes) return NULL;
    void *p = aligned_alloc(64, rounded);
    if (p) memset(p, 0, rounded);
    return p;
}

int oma_rz_oracle(const int8_t *w, size_t m, size_t n, const int8_t *x, int32_t *y) {
    if (!w || !x || !y) return OMA_RZ_E_ARG;
    int rc = oma_rz_check_shape(m, n, OMA_RZ_MAX_N);
    if (rc) return rc;
    for (size_t i = 0; i < m; i++) {
        int64_t acc = 0;
        const int8_t *row = w + i * n;
        for (size_t j = 0; j < n; j++) acc += (int64_t)row[j] * (int64_t)x[j];
        y[i] = (int32_t)acc;
    }
    return OMA_RZ_OK;
}
