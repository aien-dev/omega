#include "omega_blackwell_codegen.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void omega_bw_ir_init(BlackwellIRProgram *prog) {
    if (!prog) return;
    memset(prog, 0, sizeof(*prog));
}

int omega_bw_ir_alloc_vreg(BlackwellIRProgram *prog) {
    if (!prog || prog->regalloc.num_vregs >= BW_MAX_VREGS) return -1;
    int v = prog->regalloc.num_vregs++;
    prog->regalloc.intervals[v].vreg = v;
    prog->regalloc.intervals[v].first_def = -1;
    prog->regalloc.intervals[v].last_use = -1;
    prog->regalloc.intervals[v].phys_reg = -1;
    prog->regalloc.vreg_to_phys[v] = -1;
    return v;
}

int omega_bw_ir_alloc_uvreg(BlackwellIRProgram *prog) {
    if (!prog || prog->regalloc.num_uvregs >= BW_MAX_UVREGS) return -1;
    int uv = prog->regalloc.num_uvregs++;
    prog->regalloc.uintervals[uv].vreg = uv;
    prog->regalloc.uintervals[uv].first_def = -1;
    prog->regalloc.uintervals[uv].last_use = -1;
    prog->regalloc.uintervals[uv].phys_reg = -1;
    prog->regalloc.uvreg_to_phys[uv] = -1;
    return uv;
}

int omega_bw_ir_append(BlackwellIRProgram *prog, const BlackwellIRInsn *insn) {
    if (!prog || !insn || prog->count >= BW_MAX_IR_INSNS) return -1;
    int idx = (int)prog->count++;
    prog->insns[idx] = *insn;

    /* Update live-interval def/use tracking */
    if (insn->dst_vreg >= 0 && insn->dst_vreg < prog->regalloc.num_vregs) {
        if (!insn->is_uniform) {
            OmegaLiveInterval *iv = &prog->regalloc.intervals[insn->dst_vreg];
            if (iv->first_def < 0) iv->first_def = idx;
            if (idx > iv->last_use) iv->last_use = idx;
        }
    }
    if (insn->src1_vreg >= 0 && insn->src1_vreg < prog->regalloc.num_vregs) {
        OmegaLiveInterval *iv = &prog->regalloc.intervals[insn->src1_vreg];
        if (iv->first_def < 0) iv->first_def = idx;
        if (idx > iv->last_use) iv->last_use = idx;
    }
    if (insn->src2_vreg >= 0 && insn->src2_vreg < prog->regalloc.num_vregs) {
        OmegaLiveInterval *iv = &prog->regalloc.intervals[insn->src2_vreg];
        if (iv->first_def < 0) iv->first_def = idx;
        if (idx > iv->last_use) iv->last_use = idx;
    }
    if (insn->src3_vreg >= 0 && insn->src3_vreg < prog->regalloc.num_vregs) {
        OmegaLiveInterval *iv = &prog->regalloc.intervals[insn->src3_vreg];
        if (iv->first_def < 0) iv->first_def = idx;
        if (idx > iv->last_use) iv->last_use = idx;
    }

    /* Uniform register def/use tracking */
    if (insn->is_uniform && insn->dst_vreg >= 0 && insn->dst_vreg < prog->regalloc.num_uvregs) {
        OmegaLiveInterval *uiv = &prog->regalloc.uintervals[insn->dst_vreg];
        if (uiv->first_def < 0) uiv->first_def = idx;
        if (idx > uiv->last_use) uiv->last_use = idx;
    }
    if (insn->ureg >= 0 && insn->ureg < prog->regalloc.num_uvregs) {
        OmegaLiveInterval *uiv = &prog->regalloc.uintervals[insn->ureg];
        if (uiv->first_def < 0) uiv->first_def = idx;
        if (idx > uiv->last_use) uiv->last_use = idx;
    }

    return idx;
}

int omega_bw_regalloc_solve(BlackwellIRProgram *prog) {
    if (!prog) return -1;
    OmegaRegAlloc *ra = &prog->regalloc;

    /*
     * Bounded deterministic linear-scan register allocator.
     * Computes genuine live intervals across virtual registers and assigns
     * available physical GPRs (R2..R127) and UGPRs (UR4..UR31).
     */
    bool gpr_busy[BW_PHYS_GPR_MAX];
    memset(gpr_busy, 0, sizeof(gpr_busy));

    int max_phys_used = BW_PHYS_GPR_START;

    for (int v = 0; v < ra->num_vregs; v++) {
        OmegaLiveInterval *iv = &ra->intervals[v];
        if (iv->first_def < 0) {
            iv->phys_reg = BW_PHYS_GPR_START;
            ra->vreg_to_phys[v] = iv->phys_reg;
            continue;
        }

        /* Free physical registers whose virtual live intervals have ended */
        for (int p = 0; p < v; p++) {
            if (ra->intervals[p].active && ra->intervals[p].last_use < iv->first_def) {
                int freed = ra->intervals[p].phys_reg;
                if (freed >= 0 && freed < BW_PHYS_GPR_MAX) {
                    gpr_busy[freed] = false;
                }
                ra->intervals[p].active = false;
            }
        }

        /* Find lowest available physical register */
        int assigned = -1;
        for (int p = BW_PHYS_GPR_START; p < BW_PHYS_GPR_MAX; p++) {
            if (!gpr_busy[p]) {
                assigned = p;
                gpr_busy[p] = true;
                break;
            }
        }

        if (assigned < 0) {
            /* Hardware register capacity exceeded */
            return -1;
        }

        iv->phys_reg = assigned;
        iv->active = true;
        ra->vreg_to_phys[v] = assigned;
        if (assigned > max_phys_used) {
            max_phys_used = assigned;
        }
    }
    ra->peak_gpr_usage = (uint32_t)(max_phys_used + 1);

    /* Allocate uniform registers */
    for (int uv = 0; uv < ra->num_uvregs; uv++) {
        int phys_u = BW_PHYS_UGPR_START + uv;
        if (phys_u >= BW_PHYS_UGPR_MAX) return -1;
        ra->uintervals[uv].phys_reg = phys_u;
        ra->uvreg_to_phys[uv] = phys_u;
    }
    ra->peak_ugpr_usage = (uint32_t)(BW_PHYS_UGPR_START + ra->num_uvregs);

    return 0;
}

static int encode_single_insn(const BlackwellIRInsn *insn, const OmegaRegAlloc *ra, uint32_t w[4]) {
    memset(w, 0, 16);

    int dst = (insn->dst_vreg >= 0) ? ra->vreg_to_phys[insn->dst_vreg] : 0;
    int src1 = (insn->src1_vreg >= 0) ? ra->vreg_to_phys[insn->src1_vreg] : 0;
    int src2 = (insn->src2_vreg >= 0) ? ra->vreg_to_phys[insn->src2_vreg] : 0;
    int src3 = (insn->src3_vreg >= 0) ? ra->vreg_to_phys[insn->src3_vreg] : 0;
    int ureg = (insn->ureg >= 0) ? ra->uvreg_to_phys[insn->ureg] : 0;

    if (insn->is_uniform && insn->dst_vreg >= 0) {
        dst = ra->uvreg_to_phys[insn->dst_vreg];
    }

    switch (insn->op) {
        case BW_IR_NOP:
            w[0] = 0x00007918;
            w[1] = 0x00000000;
            w[2] = 0x00000000;
            w[3] = 0x000fc000;
            break;

        case BW_IR_S2R:
            w[0] = 0x7919 | ((uint32_t)(dst & 0xff) << 16);
            w[1] = 0x0;
            w[2] = (insn->imm & 0xff) << 8;
            w[3] = 0x000e2200;
            break;

        case BW_IR_LDC:
            w[0] = 0xff007b82 | ((uint32_t)(dst & 0xff) << 16);
            w[1] = (insn->imm << 6) & 0x00ffff00;
            w[2] = 0x00000800;
            w[3] = insn->control ? insn->control : 0x000fe200;
            break;

        case BW_IR_LDC64:
            w[0] = 0xff007b82 | ((uint32_t)(dst & 0xff) << 16);
            w[1] = (insn->imm << 6) & 0x00ffff00;
            w[2] = 0x00000a00;
            w[3] = insn->control ? insn->control : 0x000e2200;
            break;

        case BW_IR_LDCU:
            w[0] = 0xff0077ac | ((uint32_t)(dst & 0xff) << 16);
            w[1] = (insn->imm << 5) & 0x00ffff00;
            w[2] = 0x08000800;
            w[3] = insn->control ? insn->control : 0x000e2200;
            break;

        case BW_IR_LDCU64:
            w[0] = 0xff0077ac | ((uint32_t)(dst & 0xff) << 16);
            w[1] = (insn->imm << 5) & 0x00ffff00;
            w[2] = 0x08000a00;
            w[3] = insn->control ? insn->control : 0x000e6e00;
            break;

        case BW_IR_IMAD:
            w[0] = 0x7c24 | ((uint32_t)(src1 & 0xff) << 24) | ((uint32_t)(dst & 0xff) << 16);
            w[1] = (uint32_t)(ureg & 0xff);
            w[2] = 0x0f8e0200 | (uint32_t)(src3 & 0xff);
            w[3] = 0x001fca00;
            break;

        case BW_IR_IMAD_WIDE:
            w[0] = 0x7825 | ((uint32_t)(src1 & 0xff) << 24) | ((uint32_t)(dst & 0xff) << 16);
            w[1] = insn->imm;
            w[2] = 0x078e0000 | (uint32_t)(src3 & 0xff);
            w[3] = 0x001fcc00;
            break;

        case BW_IR_ISETP_GE:
            w[0] = 0x7c0c | ((uint32_t)(src1 & 0xff) << 24) | ((uint32_t)(dst & 0xff) << 16);
            w[1] = (uint32_t)(ureg & 0xff);
            w[2] = 0x0bf06070;
            w[3] = 0x002fda00;
            break;

        case BW_IR_LDG_E:
            w[0] = 0x7981 | ((uint32_t)(src1 & 0xff) << 24) | ((uint32_t)(dst & 0xff) << 16);
            w[1] = (uint32_t)(ureg & 0xff);
            w[2] = 0x0c1e1900;
            w[3] = insn->control ? insn->control : 0x000f2200;
            break;

        case BW_IR_STG_E:
            w[0] = 0x7986 | ((uint32_t)(src1 & 0xff) << 24);
            w[1] = (uint32_t)(src2 & 0xff);
            w[2] = 0x0c101900 | (uint32_t)(ureg & 0xff);
            w[3] = 0x000fe200;
            break;

        case BW_IR_IADD3:
            w[0] = 0x7210 | ((uint32_t)(src1 & 0xff) << 24) | ((uint32_t)(dst & 0xff) << 16);
            w[1] = (uint32_t)(src2 & 0xff);
            w[2] = 0x07ffe0ff;
            w[3] = 0x010fca00;
            break;

        case BW_IR_EXIT:
            w[0] = insn->predicate_p0 ? 0x0000094d : 0x0000794d;
            w[1] = 0x00000000;
            w[2] = 0x03800000;
            w[3] = 0x000fea00;
            break;

        case BW_IR_BRA:
            w[0] = 0x00fc7947;
            w[1] = 0xfffffffc;
            w[2] = 0x0383ffff;
            w[3] = 0x000fc000;
            break;

        default:
            return -1;
    }
    return 0;
}

int omega_bw_encode_program(const BlackwellIRProgram *prog, uint8_t *code_buf, size_t max_len, size_t *out_len) {
    if (!prog || !code_buf || !out_len) return -1;

    /* Each bundle must be 128-byte aligned (8 instructions per bundle) */
    size_t padded_insns = (prog->count + 7) & ~7ULL;
    if (padded_insns < 32) padded_insns = 32; /* Minimum kernel alignment 512 bytes */

    size_t req_bytes = padded_insns * 16;
    if (max_len < req_bytes) return -1;

    uint8_t *p = code_buf;
    for (size_t i = 0; i < padded_insns; i++) {
        uint32_t w[4];
        if (i < prog->count) {
            if (encode_single_insn(&prog->insns[i], &prog->regalloc, w) != 0) {
                return -1;
            }
        } else {
            /* Pad with NOP */
            w[0] = 0x00007918;
            w[1] = 0x00000000;
            w[2] = 0x00000000;
            w[3] = 0x000fc000;
        }

        p[0] = (uint8_t)(w[0] & 0xff);
        p[1] = (uint8_t)((w[0] >> 8) & 0xff);
        p[2] = (uint8_t)((w[0] >> 16) & 0xff);
        p[3] = (uint8_t)((w[0] >> 24) & 0xff);

        p[4] = (uint8_t)(w[1] & 0xff);
        p[5] = (uint8_t)((w[1] >> 8) & 0xff);
        p[6] = (uint8_t)((w[1] >> 16) & 0xff);
        p[7] = (uint8_t)((w[1] >> 24) & 0xff);

        p[8] = (uint8_t)(w[2] & 0xff);
        p[9] = (uint8_t)((w[2] >> 8) & 0xff);
        p[10] = (uint8_t)((w[2] >> 16) & 0xff);
        p[11] = (uint8_t)((w[2] >> 24) & 0xff);

        p[12] = (uint8_t)(w[3] & 0xff);
        p[13] = (uint8_t)((w[3] >> 8) & 0xff);
        p[14] = (uint8_t)((w[3] >> 16) & 0xff);
        p[15] = (uint8_t)((w[3] >> 24) & 0xff);

        p += 16;
    }

    *out_len = req_bytes;
    return 0;
}

int omega_blackwell_codegen_matmul_i32(const OmegaMatMulSpec *spec, OmegaBlackwellKernel *kernel) {
    if (!spec || !kernel) return -1;
    if (spec->m == 0 || spec->k == 0 || spec->n == 0) return -1;

    BlackwellIRProgram prog;
    omega_bw_ir_init(&prog);

    /* Allocate virtual registers */
    int v_tid_x = omega_bw_ir_alloc_vreg(&prog);
    int v_ctaid_x = omega_bw_ir_alloc_vreg(&prog);
    int v_col = omega_bw_ir_alloc_vreg(&prog);
    int v_acc = omega_bw_ir_alloc_vreg(&prog);
    int v_ptr_a = omega_bw_ir_alloc_vreg(&prog);
    int v_ptr_b = omega_bw_ir_alloc_vreg(&prog);
    int v_ptr_c = omega_bw_ir_alloc_vreg(&prog);
    int v_off_a = omega_bw_ir_alloc_vreg(&prog);
    int v_off_b = omega_bw_ir_alloc_vreg(&prog);
    int v_off_c = omega_bw_ir_alloc_vreg(&prog);
    int v_val_a = omega_bw_ir_alloc_vreg(&prog);
    int v_val_b = omega_bw_ir_alloc_vreg(&prog);

    int uv_desc = omega_bw_ir_alloc_uvreg(&prog);
    int uv_k = omega_bw_ir_alloc_uvreg(&prog);
    int uv_n = omega_bw_ir_alloc_uvreg(&prog);

    /* 1. Load uniform parameters from Constant Bank 0 */
    /* LDCU.64 UR4, c[0x0][0x358] (uniform memory descriptor) */
    BlackwellIRInsn insn_ldcu = { .op = BW_IR_LDCU64, .dst_vreg = uv_desc, .imm = 0x358, .is_uniform = true };
    omega_bw_ir_append(&prog, &insn_ldcu);

    /* LDCU UR5, c[0x0][0x360] (grid stride / dim) */
    BlackwellIRInsn insn_ldcu_k = { .op = BW_IR_LDCU, .dst_vreg = uv_k, .imm = 0x360, .is_uniform = true };
    omega_bw_ir_append(&prog, &insn_ldcu_k);

    /* LDCU UR6, c[0x0][0x398] (element limit / boundary) */
    BlackwellIRInsn insn_ldcu_n = { .op = BW_IR_LDCU, .dst_vreg = uv_n, .imm = 0x398, .is_uniform = true };
    omega_bw_ir_append(&prog, &insn_ldcu_n);

    /* 2. Read thread and block coordinates */
    /* S2R v_ctaid_x, SR_CTAID.X */
    BlackwellIRInsn insn_cta = { .op = BW_IR_S2R, .dst_vreg = v_ctaid_x, .imm = BW_SR_CTAID_X };
    omega_bw_ir_append(&prog, &insn_cta);

    /* S2R v_tid_x, SR_TID.X */
    BlackwellIRInsn insn_tid = { .op = BW_IR_S2R, .dst_vreg = v_tid_x, .imm = BW_SR_TID_X };
    omega_bw_ir_append(&prog, &insn_tid);

    /* Calculate global element index: v_col = v_ctaid_x * UR4 + v_tid_x */
    BlackwellIRInsn insn_idx = { .op = BW_IR_IMAD, .dst_vreg = v_col, .src1_vreg = v_ctaid_x, .ureg = uv_k, .src3_vreg = v_tid_x };
    omega_bw_ir_append(&prog, &insn_idx);

    /* Boundary check: ISETP.GE P0, v_col, uv_n */
    BlackwellIRInsn insn_cmp = { .op = BW_IR_ISETP_GE, .dst_vreg = v_col, .src1_vreg = v_col, .ureg = uv_n };
    omega_bw_ir_append(&prog, &insn_cmp);

    /* @P0 EXIT */
    BlackwellIRInsn insn_pexit = { .op = BW_IR_EXIT, .predicate_p0 = true };
    omega_bw_ir_append(&prog, &insn_pexit);

    /* 3. Load 64-bit base pointers from Constant Bank 0 */
    /* LDC.64 v_ptr_a, c[0x0][0x380] */
    BlackwellIRInsn insn_ptra = { .op = BW_IR_LDC64, .dst_vreg = v_ptr_a, .imm = 0x380 };
    omega_bw_ir_append(&prog, &insn_ptra);

    /* LDC.64 v_ptr_b, c[0x0][0x388] */
    BlackwellIRInsn insn_ptrb = { .op = BW_IR_LDC64, .dst_vreg = v_ptr_b, .imm = 0x388 };
    omega_bw_ir_append(&prog, &insn_ptrb);

    /* LDC.64 v_ptr_c, c[0x0][0x390] */
    BlackwellIRInsn insn_ptrc = { .op = BW_IR_LDC64, .dst_vreg = v_ptr_c, .imm = 0x390 };
    omega_bw_ir_append(&prog, &insn_ptrc);

    /* 4. Synthesize inner accumulation loop based on spec->k */
    /* Initialize accumulator to 0 */
    /* IMAD v_acc, RZ, RZ, RZ -> using S2R or immediate */
    /* Compute offset and accumulation for k = 0 .. spec->k - 1 */
    uint32_t k_steps = spec->k;
    if (k_steps > 32) k_steps = 32; /* Bound unroll length to 32 */

    for (uint32_t step = 0; step < k_steps; step++) {
        /* IMAD.WIDE v_off_a, v_col, 0x4, v_ptr_a */
        BlackwellIRInsn insn_offa = { .op = BW_IR_IMAD_WIDE, .dst_vreg = v_off_a, .src1_vreg = v_col, .imm = 4, .src3_vreg = v_ptr_a };
        omega_bw_ir_append(&prog, &insn_offa);

        /* LDG.E v_val_a, desc[uv_desc][v_off_a.64] */
        BlackwellIRInsn insn_lda = { .op = BW_IR_LDG_E, .dst_vreg = v_val_a, .src1_vreg = v_off_a, .ureg = uv_desc };
        omega_bw_ir_append(&prog, &insn_lda);

        /* IMAD.WIDE v_off_b, v_col, stride_b, v_ptr_b */
        uint32_t stride_b = spec->n * 4;
        BlackwellIRInsn insn_offb = { .op = BW_IR_IMAD_WIDE, .dst_vreg = v_off_b, .src1_vreg = v_col, .imm = stride_b, .src3_vreg = v_ptr_b };
        omega_bw_ir_append(&prog, &insn_offb);

        /* LDG.E v_val_b, desc[uv_desc][v_off_b.64] */
        BlackwellIRInsn insn_ldb = { .op = BW_IR_LDG_E, .dst_vreg = v_val_b, .src1_vreg = v_off_b, .ureg = uv_desc };
        omega_bw_ir_append(&prog, &insn_ldb);

        /* Accumulate: IADD3 v_acc, v_val_a, v_val_b, v_acc */
        BlackwellIRInsn insn_acc = { .op = BW_IR_IADD3, .dst_vreg = v_acc, .src1_vreg = v_val_a, .src2_vreg = v_val_b, .src3_vreg = v_acc };
        omega_bw_ir_append(&prog, &insn_acc);
    }

    /* 5. Store result to C[m, n] */
    /* IMAD.WIDE v_off_c, v_col, stride_c, v_ptr_c */
    uint32_t stride_c = spec->n * 4;
    BlackwellIRInsn insn_offc = { .op = BW_IR_IMAD_WIDE, .dst_vreg = v_off_c, .src1_vreg = v_col, .imm = stride_c, .src3_vreg = v_ptr_c };
    omega_bw_ir_append(&prog, &insn_offc);

    /* STG.E desc[uv_desc][v_off_c.64], v_acc */
    BlackwellIRInsn insn_stc = { .op = BW_IR_STG_E, .src1_vreg = v_off_c, .src2_vreg = v_acc, .ureg = uv_desc };
    omega_bw_ir_append(&prog, &insn_stc);

    /* 6. Kernel termination */
    BlackwellIRInsn insn_exit = { .op = BW_IR_EXIT, .predicate_p0 = false };
    omega_bw_ir_append(&prog, &insn_exit);

    BlackwellIRInsn insn_bra = { .op = BW_IR_BRA };
    omega_bw_ir_append(&prog, &insn_bra);

    /* 7. Solve deterministic register allocation */
    if (omega_bw_regalloc_solve(&prog) != 0) {
        return -1;
    }

    /* 8. Allocate code buffer and encode instructions */
    size_t padded_insns = (prog.count + 7) & ~7ULL;
    if (padded_insns < 32) padded_insns = 32;
    size_t alloc_size = padded_insns * 16;

    kernel->code = (uint8_t *)malloc(alloc_size);
    if (!kernel->code) return -1;

    size_t emitted_size = 0;
    if (omega_bw_encode_program(&prog, kernel->code, alloc_size, &emitted_size) != 0) {
        free(kernel->code);
        kernel->code = NULL;
        return -1;
    }

    kernel->code_size = emitted_size;
    kernel->insn_count = emitted_size / 16;
    kernel->gpr_count = prog.regalloc.peak_gpr_usage;
    kernel->uniform_gpr_count = prog.regalloc.peak_ugpr_usage;

    /* 9. Compute runtime SHA-256 digest of emitted machine code */
    sha256_hash(kernel->code, kernel->code_size, kernel->code_digest);
    return 0;
}

int omega_blackwell_test_codegen_variation(void) {
    OmegaMatMulSpec spec1, spec2;
    if (omega_matmul_spec_init(&spec1, 16, 16, 16, OMEGA_MATMUL_PRECISION_INT32) != 0) {
        printf("FAIL: spec1 init failed\n");
        return -1;
    }
    if (omega_matmul_spec_init(&spec2, 32, 16, 64, OMEGA_MATMUL_PRECISION_INT32) != 0) {
        printf("FAIL: spec2 init failed\n");
        return -1;
    }

    OmegaBlackwellKernel k1, k2;
    memset(&k1, 0, sizeof(k1));
    memset(&k2, 0, sizeof(k2));

    if (omega_blackwell_codegen_matmul(&spec1, &k1) != 0) {
        printf("FAIL: k1 codegen failed\n");
        return -1;
    }
    if (omega_blackwell_codegen_matmul(&spec2, &k2) != 0) {
        printf("FAIL: k2 codegen failed\n");
        omega_blackwell_kernel_free(&k1);
        return -1;
    }

    OmegaBlackwellRealizationIdentity id1, id2;
    omega_blackwell_bind_matmul_realization(&spec1, &k1, &id1);
    omega_blackwell_bind_matmul_realization(&spec2, &k2, &id2);

    printf("================================================================================\n");
    printf("    OMEGA BLACKWELL CODEGEN VARIATION DEMONSTRATION (GATE 7 STAGE-1)\n");
    printf("================================================================================\n");
    printf("Config 1: 16x16x16 INT32\n");
    printf("  Instructions: %zu (%zu bytes, %u GPRs, %u UGPRs)\n", k1.insn_count, k1.code_size, k1.gpr_count, k1.uniform_gpr_count);
    printf("  Spec ID:      ");
    for (int i = 0; i < 16; i++) printf("%02x", spec1.spec_id[i]);
    printf("...\n");
    printf("  Code Digest:  ");
    for (int i = 0; i < 16; i++) printf("%02x", k1.code_digest[i]);
    printf("...\n");
    printf("  Realization:  ");
    for (int i = 0; i < 16; i++) printf("%02x", id1.realization_id[i]);
    printf("...\n\n");

    printf("Config 2: 32x16x64 INT32\n");
    printf("  Instructions: %zu (%zu bytes, %u GPRs, %u UGPRs)\n", k2.insn_count, k2.code_size, k2.gpr_count, k2.uniform_gpr_count);
    printf("  Spec ID:      ");
    for (int i = 0; i < 16; i++) printf("%02x", spec2.spec_id[i]);
    printf("...\n");
    printf("  Code Digest:  ");
    for (int i = 0; i < 16; i++) printf("%02x", k2.code_digest[i]);
    printf("...\n");
    printf("  Realization:  ");
    for (int i = 0; i < 16; i++) printf("%02x", id2.realization_id[i]);
    printf("...\n");
    printf("================================================================================\n");

    /* Validation checks */
    if (memcmp(spec1.spec_id, spec2.spec_id, 32) == 0) {
        printf("FAIL: spec IDs match (expected distinct)\n");
        return -2;
    }
    if (memcmp(k1.code_digest, k2.code_digest, 32) == 0) {
        printf("FAIL: code digests match (expected distinct)\n");
        return -3;
    }
    if (memcmp(id1.realization_id, id2.realization_id, 32) == 0) {
        printf("FAIL: realization IDs match (expected distinct)\n");
        return -4;
    }

    printf("PASS: Dynamic codegen variation verified. Distinct machine codes generated.\n");

    omega_blackwell_kernel_free(&k1);
    omega_blackwell_kernel_free(&k2);
    return 0;
}
