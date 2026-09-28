#include "omega_matvec_quad.h"

#include "aarch64_encoder.h"
#include "aarch64_target.h"
#include "omega_realize.h"
#include "omega_verify.h"

#include <string.h>

/* x0 = A, x1 = x, x2 = y, x3 = M, x4 = N. Only caller-saved x5..x17 are used. */
static int emit_quad4(uint8_t *code, size_t *pos, size_t max) {
    int rc = 0;
    /* Words 0-1: guards */
    rc |= aarch64_emit_cbz(code, pos, max, true, REG_X3, 50);          /* -> ret (50) */
    rc |= aarch64_emit_cbz(code, pos, max, true, REG_X4, 49);          /* -> ret (50) */
    /* Words 2-6: rem = N & 3, quad = N - rem, rows = M, stride = 32 bytes */
    rc |= aarch64_emit_movz(code, pos, max, true, REG_X16, 3, 0);
    rc |= aarch64_emit_and_reg(code, pos, max, true, REG_X15, REG_X4, REG_X16);
    rc |= aarch64_emit_sub_reg(code, pos, max, true, REG_X14, REG_X4, REG_X15);
    rc |= aarch64_emit_mov_reg(code, pos, max, true, REG_X5, REG_X3);
    rc |= aarch64_emit_movz(code, pos, max, true, REG_X17, 32, 0);

    /* Word 7 (row_loop) */
    rc |= aarch64_emit_mov_reg(code, pos, max, true, REG_X6, REG_X0);
    rc |= aarch64_emit_mov_reg(code, pos, max, true, REG_X7, REG_X1);
    rc |= aarch64_emit_mov_reg(code, pos, max, true, REG_X8, REG_XZR);
    rc |= aarch64_emit_mov_reg(code, pos, max, true, REG_X9, REG_XZR);
    rc |= aarch64_emit_mov_reg(code, pos, max, true, REG_X10, REG_XZR);
    rc |= aarch64_emit_mov_reg(code, pos, max, true, REG_X11, REG_XZR);
    rc |= aarch64_emit_mov_reg(code, pos, max, true, REG_X13, REG_X14);
    rc |= aarch64_emit_cbz(code, pos, max, true, REG_X13, 21);         /* -> combine (35) */

    /* Word 15 (quad_loop): four independent products, fixed offsets */
    static const uint8_t acc[4] = { REG_X8, REG_X9, REG_X10, REG_X11 };
    for (int p = 0; p < 4; p++) {
        rc |= aarch64_emit_ldr_uoff(code, pos, max, true, REG_X12, REG_X6, (uint16_t)(p * 8));
        rc |= aarch64_emit_ldr_uoff(code, pos, max, true, REG_X16, REG_X7, (uint16_t)(p * 8));
        rc |= aarch64_emit_mul_reg(code, pos, max, true, REG_X12, REG_X12, REG_X16);
        rc |= aarch64_emit_add_reg(code, pos, max, true, acc[p], acc[p], REG_X12);
    }
    /* Word 31: one base update per four elements */
    rc |= aarch64_emit_add_reg(code, pos, max, true, REG_X6, REG_X6, REG_X17);
    rc |= aarch64_emit_add_reg(code, pos, max, true, REG_X7, REG_X7, REG_X17);
    rc |= aarch64_emit_subs_imm(code, pos, max, true, REG_X13, REG_X13, 4);
    rc |= aarch64_emit_b_cond(code, pos, max, COND_NE, -19);           /* -> quad_loop (15) */

    /* Word 35 (combine) */
    rc |= aarch64_emit_add_reg(code, pos, max, true, REG_X8, REG_X8, REG_X9);
    rc |= aarch64_emit_add_reg(code, pos, max, true, REG_X10, REG_X10, REG_X11);
    rc |= aarch64_emit_add_reg(code, pos, max, true, REG_X8, REG_X8, REG_X10);
    rc |= aarch64_emit_cbz(code, pos, max, true, REG_X15, 8);          /* -> row_end (46) */
    rc |= aarch64_emit_mov_reg(code, pos, max, true, REG_X13, REG_X15);

    /* Word 40 (rem_loop) */
    rc |= aarch64_emit_ldr_x_post(code, pos, max, REG_X12, REG_X6, 8);
    rc |= aarch64_emit_ldr_x_post(code, pos, max, REG_X16, REG_X7, 8);
    rc |= aarch64_emit_mul_reg(code, pos, max, true, REG_X12, REG_X12, REG_X16);
    rc |= aarch64_emit_add_reg(code, pos, max, true, REG_X8, REG_X8, REG_X12);
    rc |= aarch64_emit_subs_imm(code, pos, max, true, REG_X13, REG_X13, 1);
    rc |= aarch64_emit_b_cond(code, pos, max, COND_NE, -5);            /* -> rem_loop (40) */

    /* Word 46 (row_end): x6 now points at the next row */
    rc |= aarch64_emit_str_x_post(code, pos, max, REG_X8, REG_X2, 8);
    rc |= aarch64_emit_mov_reg(code, pos, max, true, REG_X0, REG_X6);
    rc |= aarch64_emit_subs_imm(code, pos, max, true, REG_X5, REG_X5, 1);
    rc |= aarch64_emit_b_cond(code, pos, max, COND_NE, -42);           /* -> row_loop (7) */

    /* Word 50 (ret) */
    rc |= aarch64_emit_ret(code, pos, max);
    return rc ? -1 : 0;
}

int omega_matvec_synthesize_quad4(const MatVecSemanticSpec *spec, const OmegaMachineGraph *mg,
                                  MatVecRealization *out) {
    if (!spec || !mg || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->kind = (MatVecRealizationKind)OMEGA_MATVEC_KIND_QUAD4;
    out->name = "matvec_quad4";
    out->unroll_factor = 4;
    out->accumulators = 4;
    out->realization.target_profile = mg->target_profile;
    out->realization.entry_offset = 0;
    out->realization.semantic_id = spec->spec_id;
    out->realization.machine_id = mg->machine_id;
    out->realization.has_machine_id = true;

    size_t pos = 0;
    if (emit_quad4(out->realization.code_bytes, &pos, sizeof(out->realization.code_bytes)) != 0)
        return -1;
    if (pos != 51u * 4u) return -1;  /* the branch offsets above assume this layout */
    out->realization.code_len = pos;

    omega_realize_compute_triple_id(&spec->spec_id, &mg->machine_id, &out->realization,
                                    &out->realization_id);
    out->realization.realization_id = out->realization_id;
    out->realization.has_id = true;

    VerifyReport rep;
    memset(&rep, 0, sizeof(rep));
    omega_verify_v0_structural(NULL, &out->realization, &rep);
    out->verify_report = rep;
    out->is_verified = rep.passed;
    return rep.passed ? 0 : -1;
}
