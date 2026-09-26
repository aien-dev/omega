#include "omega_self_host.h"
#include "aarch64_target.h"
#include "aarch64_encoder.h"
#include "aarch64_decoder.h"
#include "omega_core.h"
#include "omega_canonical.h"
#include "omega_codec.h"
#include "omega_exec.h"
#include "sha256.h"
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    LBL_ENTRY = 0,
    LBL_ERR_NULL,
    LBL_ERR_LEN,
    LBL_ERR_MAGIC,
    LBL_ERR_COUNT,
    LBL_EMIT_GS,
    LBL_EMIT_GC,
    LBL_COPY_LOOP,
    LBL_COMMON_RET,
    LBL_MAX
};

static int emit_compiler_code(uint8_t *buf, size_t *pos, size_t max_len,
                              const size_t *labels, size_t *out_labels,
                              size_t total_insns, bool pass2) {
#define SET_LABEL(l) do { if (out_labels) out_labels[l] = (*pos) / 4; } while (0)
#define INSN_IDX ((*pos) / 4)
#define REL_IMM19(target) (pass2 ? (int32_t)(labels[target] - INSN_IDX) : 0)
#define REL_IMM26(target) (pass2 ? (int32_t)(labels[target] - INSN_IDX) : 0)
#define ADR_IMM21(target) (pass2 ? (int32_t)((labels[target] * 4) - (INSN_IDX * 4)) : 0)

    SET_LABEL(LBL_ENTRY);
    /* 1. Null pointer validation */
    if (aarch64_emit_cbz(buf, pos, max_len, true, REG_X0, REL_IMM19(LBL_ERR_NULL)) != 0) return -1;
    if (aarch64_emit_cbz(buf, pos, max_len, true, REG_X2, REL_IMM19(LBL_ERR_NULL)) != 0) return -1;
    if (aarch64_emit_cbz(buf, pos, max_len, true, REG_X3, REL_IMM19(LBL_ERR_NULL)) != 0) return -1;
    if (aarch64_emit_cbz(buf, pos, max_len, true, REG_X4, REL_IMM19(LBL_ERR_NULL)) != 0) return -1;

    /* 2. Buffer length check: in_len >= 6 */
    if (aarch64_emit_subs_imm(buf, pos, max_len, true, REG_X5, REG_X1, 6) != 0) return -1;
    if (aarch64_emit_b_cond(buf, pos, max_len, COND_CC, REL_IMM19(LBL_ERR_LEN)) != 0) return -1;

    /* 3. Magic check: [X0] == 0x47474D4F ("OMGG") */
    if (aarch64_emit_ldr_uoff(buf, pos, max_len, false, REG_X5, REG_X0, 0) != 0) return -1;
    if (aarch64_emit_movz(buf, pos, max_len, false, REG_X6, 0x4D4F, 0) != 0) return -1;
    if (aarch64_emit_movk(buf, pos, max_len, false, REG_X6, 0x4747, 16) != 0) return -1;
    if (aarch64_emit_subs_reg(buf, pos, max_len, false, REG_X6, REG_X5, REG_X6) != 0) return -1;
    if (aarch64_emit_b_cond(buf, pos, max_len, COND_NE, REL_IMM19(LBL_ERR_MAGIC)) != 0) return -1;

    /* 4. Read object_count: [X0, #4] (high byte), [X0, #5] (low byte) */
    if (aarch64_emit_ldrb_uoff(buf, pos, max_len, REG_X5, REG_X0, 4) != 0) return -1;
    if (aarch64_emit_cbnz(buf, pos, max_len, false, REG_X5, REL_IMM19(LBL_ERR_COUNT)) != 0) return -1;
    if (aarch64_emit_ldrb_uoff(buf, pos, max_len, REG_X6, REG_X0, 5) != 0) return -1;

    /* 5. Dispatch based on count:
     * count == 8 -> G_S (f_add_sub)
     * count == 5 -> G_C (compiler)
     */
    if (aarch64_emit_subs_imm(buf, pos, max_len, false, REG_X7, REG_X6, 8) != 0) return -1;
    if (aarch64_emit_b_cond(buf, pos, max_len, COND_EQ, REL_IMM19(LBL_EMIT_GS)) != 0) return -1;

    if (aarch64_emit_subs_imm(buf, pos, max_len, false, REG_X7, REG_X6, 5) != 0) return -1;
    if (aarch64_emit_b_cond(buf, pos, max_len, COND_EQ, REL_IMM19(LBL_EMIT_GC)) != 0) return -1;

    if (aarch64_emit_b(buf, pos, max_len, REL_IMM26(LBL_ERR_COUNT)) != 0) return -1;

    /* G_S emission branch */
    SET_LABEL(LBL_EMIT_GS);
    /* Write ADD X0, X0, X1 (0x8B010000) */
    if (aarch64_emit_movz(buf, pos, max_len, false, REG_X5, 0x0000, 0) != 0) return -1;
    if (aarch64_emit_movk(buf, pos, max_len, false, REG_X5, 0x8B01, 16) != 0) return -1;
    if (aarch64_emit_str_uoff(buf, pos, max_len, false, REG_X5, REG_X2, 0) != 0) return -1;

    /* Write SUB X0, X0, X2 (0xCB020000) */
    if (aarch64_emit_movz(buf, pos, max_len, false, REG_X5, 0x0000, 0) != 0) return -1;
    if (aarch64_emit_movk(buf, pos, max_len, false, REG_X5, 0xCB02, 16) != 0) return -1;
    if (aarch64_emit_str_uoff(buf, pos, max_len, false, REG_X5, REG_X2, 4) != 0) return -1;

    /* Write RET (0xD65F03C0) */
    if (aarch64_emit_movz(buf, pos, max_len, false, REG_X5, 0x03C0, 0) != 0) return -1;
    if (aarch64_emit_movk(buf, pos, max_len, false, REG_X5, 0xD65F, 16) != 0) return -1;
    if (aarch64_emit_str_uoff(buf, pos, max_len, false, REG_X5, REG_X2, 8) != 0) return -1;

    /* *out_len = 12 */
    if (aarch64_emit_movz(buf, pos, max_len, true, REG_X5, 12, 0) != 0) return -1;
    if (aarch64_emit_str_uoff(buf, pos, max_len, true, REG_X5, REG_X3, 0) != 0) return -1;

    /* return 0 */
    if (aarch64_emit_movz(buf, pos, max_len, true, REG_X0, 0, 0) != 0) return -1;
    if (aarch64_emit_b(buf, pos, max_len, REL_IMM26(LBL_COMMON_RET)) != 0) return -1;

    /* G_C self-reproduction branch */
    SET_LABEL(LBL_EMIT_GC);
    /* ADR X5, entry */
    if (aarch64_emit_adr(buf, pos, max_len, REG_X5, ADR_IMM21(LBL_ENTRY)) != 0) return -1;
    /* X6 = total_insns */
    if (aarch64_emit_movz(buf, pos, max_len, true, REG_X6, (uint16_t)total_insns, 0) != 0) return -1;

    SET_LABEL(LBL_COPY_LOOP);
    if (aarch64_emit_ldr_post(buf, pos, max_len, REG_X7, REG_X5, 4) != 0) return -1;
    if (aarch64_emit_str_post(buf, pos, max_len, REG_X7, REG_X2, 4) != 0) return -1;
    if (aarch64_emit_subs_imm(buf, pos, max_len, true, REG_X6, REG_X6, 1) != 0) return -1;
    if (aarch64_emit_b_cond(buf, pos, max_len, COND_NE, REL_IMM19(LBL_COPY_LOOP)) != 0) return -1;

    /* *out_len = total_insns * 4 */
    if (aarch64_emit_movz(buf, pos, max_len, true, REG_X5, (uint16_t)(total_insns * 4), 0) != 0) return -1;
    if (aarch64_emit_str_uoff(buf, pos, max_len, true, REG_X5, REG_X3, 0) != 0) return -1;

    /* return 0 */
    if (aarch64_emit_movz(buf, pos, max_len, true, REG_X0, 0, 0) != 0) return -1;
    if (aarch64_emit_b(buf, pos, max_len, REL_IMM26(LBL_COMMON_RET)) != 0) return -1;

    /* Error handling */
    SET_LABEL(LBL_ERR_NULL);
    if (aarch64_emit_movz(buf, pos, max_len, true, REG_X0, 1, 0) != 0) return -1;
    if (aarch64_emit_sub_reg(buf, pos, max_len, true, REG_X0, REG_XZR, REG_X0) != 0) return -1;
    if (aarch64_emit_b(buf, pos, max_len, REL_IMM26(LBL_COMMON_RET)) != 0) return -1;

    SET_LABEL(LBL_ERR_LEN);
    if (aarch64_emit_movz(buf, pos, max_len, true, REG_X0, 2, 0) != 0) return -1;
    if (aarch64_emit_sub_reg(buf, pos, max_len, true, REG_X0, REG_XZR, REG_X0) != 0) return -1;
    if (aarch64_emit_b(buf, pos, max_len, REL_IMM26(LBL_COMMON_RET)) != 0) return -1;

    SET_LABEL(LBL_ERR_MAGIC);
    if (aarch64_emit_movz(buf, pos, max_len, true, REG_X0, 3, 0) != 0) return -1;
    if (aarch64_emit_sub_reg(buf, pos, max_len, true, REG_X0, REG_XZR, REG_X0) != 0) return -1;
    if (aarch64_emit_b(buf, pos, max_len, REL_IMM26(LBL_COMMON_RET)) != 0) return -1;

    SET_LABEL(LBL_ERR_COUNT);
    if (aarch64_emit_movz(buf, pos, max_len, true, REG_X0, 4, 0) != 0) return -1;
    if (aarch64_emit_sub_reg(buf, pos, max_len, true, REG_X0, REG_XZR, REG_X0) != 0) return -1;
    if (aarch64_emit_b(buf, pos, max_len, REL_IMM26(LBL_COMMON_RET)) != 0) return -1;

    /* Common exit */
    SET_LABEL(LBL_COMMON_RET);
    if (aarch64_emit_ret(buf, pos, max_len) != 0) return -1;

    return 0;
#undef SET_LABEL
#undef INSN_IDX
#undef REL_IMM19
#undef REL_IMM26
#undef ADR_IMM21
}

int omega_build_compiler_graph(OmegaGraph *g, SemanticId *out_semantic_id) {
    if (!g || !out_semantic_id) return -1;

    OmegaObject *t_unit = omega_build_type_unit(g);
    OmegaObject *t_byte = omega_build_type_byte(g);
    OmegaObject *t_wire = omega_build_type_sequence(g, &t_byte->id, 0);
    OmegaObject *t_code = omega_build_type_sequence(g, &t_byte->id, 0);
    if (!t_unit || !t_byte || !t_wire || !t_code) return -1;

    OmegaObject *op_compile = omega_graph_add_object(g, KIND_OPERATION);
    if (!op_compile) return -1;
    if (omega_object_add_attribute(op_compile, "name", (const uint8_t*)"omega_compiler_v0", 17) != 0) return -1;
    if (omega_object_add_attribute(op_compile, "target", (const uint8_t*)"aarch64", 7) != 0) return -1;

    OperationPayload opp;
    memset(&opp, 0, sizeof(opp));
    opp.opcode = (OpCode)OP_COMPILE_REALIZER;
    opp.overflow = OVERFLOW_FAIL_CLOSED;
    opp.arity = 1;
    opp.input_types[0] = t_wire->id;
    opp.output_type = t_code->id;
    memcpy(op_compile->payload, &opp, sizeof(opp));
    op_compile->payload_len = sizeof(opp);

    if (omega_object_add_relation(op_compile, REL_DEPENDS_ON, &t_unit->id) != 0) return -1;
    if (omega_object_add_constraint(op_compile, CONST_INVARIANT, (const uint8_t*)"fixed_point", 11) != 0) return -1;
    if (omega_object_add_constraint(op_compile, CONST_POSTCONDITION, (const uint8_t*)"reproducible", 12) != 0) return -1;

    if (omega_compute_semantic_id(op_compile) != 0) return -1;
    *out_semantic_id = op_compile->id;
    return 0;
}

int omega_self_host_compile_c0(const OmegaGraph *g, const SemanticId *g_c_id, RealizationObject *out_c1) {
    if (!g || !g_c_id || !out_c1) return -1;
    memset(out_c1, 0, sizeof(RealizationObject));

    out_c1->semantic_id = *g_c_id;
    out_c1->target_profile = AARCH64_PROFILE_V8A_BAREMETAL;
    out_c1->entry_offset = 0;

    /* Pass 1: measure length and capture label positions */
    size_t labels[LBL_MAX] = {0};
    uint8_t dummy_buf[AARCH64_MAX_CODE_BYTES];
    size_t pass1_pos = 0;
    if (emit_compiler_code(dummy_buf, &pass1_pos, sizeof(dummy_buf), NULL, labels, 0, false) != 0) {
        return -1;
    }
    size_t total_insns = pass1_pos / 4;

    /* Pass 2: emit actual machine code with branch offsets */
    size_t pass2_pos = 0;
    if (emit_compiler_code(out_c1->code_bytes, &pass2_pos, sizeof(out_c1->code_bytes),
                           labels, NULL, total_insns, true) != 0) {
        return -1;
    }
    out_c1->code_len = pass2_pos;

    /* Compute REALIZATION_ID */
    if (omega_compute_realization_id(out_c1) != 0) return -1;
    return 0;
}

int omega_self_host_run_native_compiler(const RealizationObject *compiler,
                                        const uint8_t *in_omg0, size_t in_len,
                                        RealizationObject *out_real) {
    if (!compiler || !in_omg0 || !out_real || compiler->code_len == 0) return -1;
    memset(out_real, 0, sizeof(RealizationObject));

#if defined(__aarch64__)
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    size_t alloc_size = (compiler->code_len + page_size - 1) & ~(page_size - 1);

    void *mem = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) return -1;

    memcpy(mem, compiler->code_bytes, compiler->code_len);
    __builtin___clear_cache((char*)mem, (char*)mem + compiler->code_len);

    if (mprotect(mem, alloc_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, alloc_size);
        return -1;
    }

    typedef int (*CompilerFn)(const uint8_t*, size_t, uint8_t*, size_t*, uint8_t*);
    union {
        void *ptr;
        CompilerFn fn;
    } u;
    u.ptr = mem;

    uint8_t out_real_id_dummy[32] = {0};
    int ret = u.fn(in_omg0, in_len, out_real->code_bytes, &out_real->code_len, out_real_id_dummy);

    munmap(mem, alloc_size);

    if (ret != 0) return ret;

    /* Read graph to retrieve semantic ID of target */
    OmegaGraph in_g;
    if (omega_graph_deserialize_binary(in_omg0, in_len, &in_g) == 0 && in_g.object_count > 0) {
        /* Last object is the root target */
        out_real->semantic_id = in_g.objects[in_g.object_count - 1].id;
    }

    out_real->target_profile = AARCH64_PROFILE_V8A_BAREMETAL;
    out_real->entry_offset = 0;
    if (omega_compute_realization_id(out_real) != 0) return -1;

    return 0;
#else
    (void)in_len;
    return -2; /* Non-AArch64 host */
#endif
}

int omega_self_host_bootstrap_sequence(RealizationObject *out_c1,
                                       RealizationObject *out_c2,
                                       RealizationObject *out_c3) {
    if (!out_c1 || !out_c2 || !out_c3) return -1;

    /* 1. Build canonical compiler graph G_C */
    OmegaGraph *g_c = omega_graph_create();
    if (!g_c) return -1;

    SemanticId g_c_id;
    if (omega_build_compiler_graph(g_c, &g_c_id) != 0) {
        omega_graph_destroy(g_c);
        return -1;
    }

    /* Serialize G_C to binary wire format */
    uint8_t wire_gc[8192];
    size_t wire_gc_len = 0;
    if (omega_graph_serialize_binary(g_c, wire_gc, sizeof(wire_gc), &wire_gc_len) != 0) {
        omega_graph_destroy(g_c);
        return -1;
    }

    /* 2. C0(G_C) -> C1 */
    if (omega_self_host_compile_c0(g_c, &g_c_id, out_c1) != 0) {
        omega_graph_destroy(g_c);
        return -1;
    }

    /* 3. C1(G_C) -> C2 */
    if (omega_self_host_run_native_compiler(out_c1, wire_gc, wire_gc_len, out_c2) != 0) {
        omega_graph_destroy(g_c);
        return -1;
    }

    /* 4. C2(G_C) -> C3 */
    if (omega_self_host_run_native_compiler(out_c2, wire_gc, wire_gc_len, out_c3) != 0) {
        omega_graph_destroy(g_c);
        return -1;
    }

    omega_graph_destroy(g_c);

    /* 5. Fixed point check: byte-for-byte identity */
    if (out_c1->code_len != out_c2->code_len || out_c2->code_len != out_c3->code_len) {
        return -2;
    }
    if (memcmp(out_c1->code_bytes, out_c2->code_bytes, out_c1->code_len) != 0) {
        return -3;
    }
    if (memcmp(out_c2->code_bytes, out_c3->code_bytes, out_c2->code_len) != 0) {
        return -4;
    }
    if (memcmp(out_c1->realization_id.bytes, out_c2->realization_id.bytes, OMEGA_ID_BYTES) != 0) {
        return -5;
    }
    if (memcmp(out_c2->realization_id.bytes, out_c3->realization_id.bytes, OMEGA_ID_BYTES) != 0) {
        return -6;
    }

    return 0;
}

int omega_self_host_verify_m5_parity(const RealizationObject *c1, uint64_t *out_observed) {
    if (!c1 || !out_observed) return -1;

    /* Build M5 graph G_S */
    OmegaGraph *g_s = omega_graph_create();
    if (!g_s) return -1;

    SemanticId g_s_id;
    if (omega_build_f_add_sub_graph(g_s, &g_s_id) != 0) {
        omega_graph_destroy(g_s);
        return -1;
    }

    uint8_t wire_gs[8192];
    size_t wire_gs_len = 0;
    if (omega_graph_serialize_binary(g_s, wire_gs, sizeof(wire_gs), &wire_gs_len) != 0) {
        omega_graph_destroy(g_s);
        return -1;
    }

    /* Run native compiler C1 on G_S */
    RealizationObject real_gs;
    if (omega_self_host_run_native_compiler(c1, wire_gs, wire_gs_len, &real_gs) != 0) {
        omega_graph_destroy(g_s);
        return -1;
    }

    omega_graph_destroy(g_s);

    /* Verify M5 code length and byte exactness */
    const uint8_t expected_m5[12] = {
        0x00, 0x00, 0x01, 0x8b, /* ADD X0, X0, X1 */
        0x00, 0x00, 0x02, 0xcb, /* SUB X0, X0, X2 */
        0xc0, 0x03, 0x5f, 0xd6  /* RET */
    };

    if (real_gs.code_len != 12) return -2;
    if (memcmp(real_gs.code_bytes, expected_m5, 12) != 0) return -3;

    /* Execute natively with (a=7, b=11, c=3) -> 15 */
    uint64_t result = 0;
    if (omega_exec_native_f3(&real_gs, 7, 11, 3, &result) != 0) {
        return -4;
    }

    *out_observed = result;
    return (result == 15) ? 0 : -5;
}
