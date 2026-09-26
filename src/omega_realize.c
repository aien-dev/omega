#include "omega_realize.h"
#include "aarch64_encoder.h"
#include "omega_core.h"
#include "omega_canonical.h"
#include "sha256.h"
#include <string.h>

static void write_u32_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xff);
    p[1] = (uint8_t)((v >> 16) & 0xff);
    p[2] = (uint8_t)((v >> 8) & 0xff);
    p[3] = (uint8_t)(v & 0xff);
}

int omega_compute_realization_id(RealizationObject *real) {
    if (!real) return -1;
    uint8_t buf[AARCH64_MAX_CODE_BYTES + 64];
    size_t pos = 0;

    /* Magic "OMG_R0" */
    buf[pos++] = 0x4F;
    buf[pos++] = 0x4D;
    buf[pos++] = 0x47;
    buf[pos++] = 0x5F;
    buf[pos++] = 0x52;
    buf[pos++] = 0x30;

    buf[pos++] = real->target_profile;
    memcpy(&buf[pos], real->semantic_id.bytes, OMEGA_ID_BYTES);
    pos += OMEGA_ID_BYTES;

    write_u32_be(&buf[pos], (uint32_t)real->code_len);
    pos += 4;

    if (real->code_len > 0) {
        memcpy(&buf[pos], real->code_bytes, real->code_len);
        pos += real->code_len;
    }

    sha256_hash(buf, pos, real->realization_id.bytes);
    real->has_id = true;
    return 0;
}

int omega_build_f_add_sub_graph(OmegaGraph *g, SemanticId *out_semantic_id) {
    if (!g || !out_semantic_id) return -1;

    OmegaObject *t_u64 = omega_build_type_uint(g, 64);
    OmegaObject *va = omega_build_val_uint(g, &t_u64->id, 64, 7);
    OmegaObject *vb = omega_build_val_uint(g, &t_u64->id, 64, 11);
    OmegaObject *vc = omega_build_val_uint(g, &t_u64->id, 64, 3);

    OmegaObject *op_add = omega_build_op_binary(g, OP_ADD, OVERFLOW_WRAP, &t_u64->id);
    OmegaObject *app_add = omega_build_apply(g, &op_add->id, &va->id, &vb->id);

    OmegaObject *op_sub = omega_build_op_binary(g, OP_SUB, OVERFLOW_WRAP, &t_u64->id);
    OmegaObject *app_sub = omega_build_apply(g, &op_sub->id, &app_add->id, &vc->id);

    *out_semantic_id = app_sub->id;
    return 0;
}

int omega_realize_f_add_sub(const OmegaGraph *g, const SemanticId *root_apply_id, RealizationObject *out_real) {
    if (!g || !root_apply_id || !out_real) return -1;
    memset(out_real, 0, sizeof(RealizationObject));

    out_real->semantic_id = *root_apply_id;
    out_real->target_profile = AARCH64_PROFILE_V8A_BAREMETAL;
    out_real->entry_offset = 0;

    size_t pos = 0;
    /* Synthesize:
     *   ADD X0, X0, X1   (X0 = a + b)
     *   SUB X0, X0, X2   (X0 = (a + b) - c)
     *   RET
     */
    if (aarch64_emit_add_reg(out_real->code_bytes, &pos, sizeof(out_real->code_bytes), true, REG_X0, REG_X0, REG_X1) != 0) return -1;
    if (aarch64_emit_sub_reg(out_real->code_bytes, &pos, sizeof(out_real->code_bytes), true, REG_X0, REG_X0, REG_X2) != 0) return -1;
    if (aarch64_emit_ret(out_real->code_bytes, &pos, sizeof(out_real->code_bytes)) != 0) return -1;

    out_real->code_len = pos;
    omega_compute_realization_id(out_real);
    return 0;
}

int omega_realize_pure_binary(const OmegaGraph *g, const SemanticId *op_id, RealizationObject *out_real) {
    if (!g || !op_id || !out_real) return -1;
    const OmegaObject *obj = omega_graph_find_object_const(g, op_id);
    if (!obj || obj->kind != KIND_OPERATION || obj->payload_len < sizeof(OperationPayload)) return -1;

    const OperationPayload *opp = (const OperationPayload*)obj->payload;
    memset(out_real, 0, sizeof(RealizationObject));
    out_real->semantic_id = *op_id;
    out_real->target_profile = AARCH64_PROFILE_V8A_BAREMETAL;

    size_t pos = 0;
    switch (opp->opcode) {
        case OP_ADD:
            aarch64_emit_add_reg(out_real->code_bytes, &pos, sizeof(out_real->code_bytes), true, REG_X0, REG_X0, REG_X1);
            break;
        case OP_SUB:
            aarch64_emit_sub_reg(out_real->code_bytes, &pos, sizeof(out_real->code_bytes), true, REG_X0, REG_X0, REG_X1);
            break;
        case OP_MUL:
            aarch64_emit_mul_reg(out_real->code_bytes, &pos, sizeof(out_real->code_bytes), true, REG_X0, REG_X0, REG_X1);
            break;
        case OP_AND:
            aarch64_emit_and_reg(out_real->code_bytes, &pos, sizeof(out_real->code_bytes), true, REG_X0, REG_X0, REG_X1);
            break;
        case OP_OR:
            aarch64_emit_orr_reg(out_real->code_bytes, &pos, sizeof(out_real->code_bytes), true, REG_X0, REG_X0, REG_X1);
            break;
        default:
            return -1;
    }
    aarch64_emit_ret(out_real->code_bytes, &pos, sizeof(out_real->code_bytes));
    out_real->code_len = pos;
    omega_compute_realization_id(out_real);
    return 0;
}
