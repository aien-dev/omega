/* omega_bw_dep.h: dependency flags between the launches of one command list (DEPFLAGS, 2026-10-16), the kernel side.
 * The protocol (one shared done counter per command list, bank words, host check) is described in omega_gpu_dep.h.
 *
 * Why: the decode graph serialised its ~436 dependent launches per token with WAIT_FOR_IDLE + INVALIDATE_SHADER_CACHES,
 * and the launch-cost study (evidence/OWN30B-L2D-BARRIER-20261014) measured 20-28 us per dependent launch for that drain
 * and refill against ~4 us without a barrier. With flags the list runs with barrier NONE and every kernel orders itself:
 *   prologue  warp 0 polls the shared done counter (LDG.E.STRONG.GPU) until done - target < 2^31 (wrap-safe), then
 *             BAR.SYNC and CCTL.IVALL (nvcc's lowering of ld.acquire.gpu, probe in investigations/2026-10-16-DEPFLAGS/sass);
 *             a poll that never succeeds (OMEGA_DEP_POLL_LIMIT iterations) adds OMEGA_DEP_TIMEOUT_SENTINEL once (lane 0) to the counter
 *             and continues, so the host sees the wrong count and nothing hangs the chip.
 *   epilogue  before every EXIT: MEMBAR.ALL.GPU; ERRBAR; CGAERRBAR (nvcc's lowering of fence + red.release.gpu), BAR.SYNC,
 *             thread 0 ATOMG.E.ADD.STRONG.GPU done, 1 (the CUDA Programming Guide reduction sample: a device-scope fence
 *             before the count increment, and a device-scope acquire load of the flag on the other side).
 *   camera    (OMEGA_DEP_CAMERA) CS2R SR_GLOBALTIMERLO at entry and before EXIT; thread 0 stores {t0, t1} (16 B) at
 *             cam_base + 16 * (slot & (OMEGA_DEP_CAM_SLOTS - 1)), slot from an atomic add on the counter line's word 1.
 * Every data buffer the kernels touch is GPU-uncached (omega_gpu_session.h), so only L1 can hold a stale line and
 * CCTL.IVALL drops it. Header-only, included by a generator AFTER its Em / emit / I0 / V / V64 / here / patch_bra
 * definitions and its K_ enum (all six generators share them). */
#ifndef OMEGA_BW_DEP_H
#define OMEGA_BW_DEP_H
#include "omega_gpu_dep.h" /* the constants and the protocol description */

typedef struct { int pdone, t0; uint32_t dep; } OmegaBwDep;

static void dep_insn(Em *e, BlackwellIROpcode op, int d, int a, int b, int u, int kind) {
    BlackwellIRInsn i = I0(op); i.dst_vreg = d; i.src1_vreg = a; i.src2_vreg = b; i.ureg = u; emit(e, i, kind);
}
static void dep_imadi(Em *e, int d, int a, uint32_t imm, int c) { BlackwellIRInsn i = I0(BW_IR_IMAD); i.dst_vreg = d; i.src1_vreg = a; i.imm = imm; i.src3_vreg = c; emit(e, i, K_FIXED); }
static void dep_movi(Em *e, int d, uint32_t imm) { BlackwellIRInsn i = I0(BW_IR_MOV_IMM); i.dst_vreg = d; i.imm = imm; emit(e, i, K_FIXED); }
static void dep_shr(Em *e, int d, int a, uint32_t imm) { BlackwellIRInsn i = I0(BW_IR_SHF_R); i.dst_vreg = d; i.src1_vreg = a; i.imm = imm; emit(e, i, K_FIXED); }
static void dep_isetp_ge(Em *e, int a, int b) { BlackwellIRInsn i = I0(BW_IR_ISETP_GE_U32); i.src1_vreg = a; i.src2_vreg = b; emit(e, i, K_PRED); }
static int dep_bra_p0_fwd(Em *e) { int at = here(e); BlackwellIRInsn i = I0(BW_IR_BRA); i.predicate_p0 = true; i.imm = 1; emit(e, i, K_BRA); return at; }
static void dep_bra_to(Em *e, int target) { BlackwellIRInsn i = I0(BW_IR_BRA); i.imm = (uint32_t)(target - here(e)); emit(e, i, K_BRA); }
static void dep_ldc64(Em *e, int d, uint32_t off) { BlackwellIRInsn i = I0(BW_IR_LDC64); i.dst_vreg = d; i.imm = off; emit(e, i, K_VAR); }
static void dep_ldc32(Em *e, int d, uint32_t off) { BlackwellIRInsn i = I0(BW_IR_LDC); i.dst_vreg = d; i.imm = off; emit(e, i, K_VAR); }
static void dep_bar(Em *e) { emit(e, I0(BW_IR_BAR_SYNC), K_SYNC); }
/* The linear-scan allocator knows no back edges (same rule as the generators' loop_end): a value defined before the poll
 * loop and read inside it must stay live until the back-branch, else the allocator hands its register to a loop temporary. */
static void dep_loop_extend(Em *e, int start, int end) {
    OmegaRegAlloc *ra = &e->p->regalloc;
    for (int v = 0; v < ra->num_vregs; v++) {
        OmegaLiveInterval *iv = &ra->intervals[v];
        if (iv->first_def >= 0 && iv->first_def < start && iv->last_use >= start && iv->last_use < end) iv->last_use = end;
    }
}

/* Prologue: call after the kernel read its thread id (tid, warp index `warp` = tid >> 5) and the uniform descriptor, BEFORE
 * its first global load. dep 0: emits nothing. */
static void dep_prologue(Em *e, uint32_t dep, int udesc, int tid, OmegaBwDep *D) {
    memset(D, 0, sizeof *D); D->dep = dep; D->pdone = -1; D->t0 = -1;
    if (!(dep & (OMEGA_DEP_FLAGS | OMEGA_DEP_CAMERA))) return;
    D->pdone = V64(e);
    if (dep & OMEGA_DEP_CAMERA) { D->t0 = V64(e); if (e->err) return; dep_insn(e, BW_IR_CS2R_GLOBALTIMER, D->t0, -1, -1, -1, K_VAR); }
    dep_ldc64(e, D->pdone, OMEGA_DEP_ARG_DONE);
    if (!(dep & OMEGA_DEP_FLAGS) || (dep & OMEGA_DEP_TEST_NO_PROLOGUE)) return; /* TEST bit: no wait (the sim test must catch the early read) */
    int pwait = V64(e), target = V(e), v = V(e), diff = V(e), cnt = V(e), one = V(e), zero = V(e), lim = V(e), sent = V(e), scratch = V(e), warp = V(e);
    if (e->err) return;
    dep_shr(e, warp, tid, 5);
    dep_ldc64(e, pwait, OMEGA_DEP_ARG_WAIT); dep_ldc32(e, target, OMEGA_DEP_ARG_TARGET);
    dep_movi(e, one, 1); dep_movi(e, zero, 0); dep_movi(e, lim, OMEGA_DEP_POLL_LIMIT); dep_movi(e, cnt, 0);
    /* Reconvergence region B0 around the poll (nvcc wraps every data-dependent loop so): lanes may leave the loop on different
     * iterations (the sim test makes the producer land mid-warp), and a split warp must not reach the epilogue BSSY. */
    const int at_bssy = here(e); { BlackwellIRInsn bs = I0(BW_IR_BSSY); bs.bar_reg = 0; bs.imm = 2; emit(e, bs, K_BSSY); }
    dep_isetp_ge(e, warp, one);                 /* P0: warp >= 1 -> straight to the join (warp 0 polls) */
    const int to_join = dep_bra_p0_fwd(e);
    const int L = here(e);
    dep_insn(e, BW_IR_LDG_STRONG_GPU, v, pwait, -1, udesc, K_VAR);
    dep_imadi(e, diff, target, 0xffffffffu, v); /* v - target */
    dep_shr(e, diff, diff, 31);                 /* 0 when 0 <= v - target < 2^31 (wrap-safe) */
    dep_isetp_ge(e, zero, diff);                /* P0: satisfied, i.e. diff == 0. (The WIP compared one >= diff, true for diff in {0, 1}: never waited.) */
    const int to_done = dep_bra_p0_fwd(e);
    dep_imadi(e, cnt, one, 1u, cnt);
    dep_isetp_ge(e, cnt, lim);                  /* P0: timed out */
    const int to_timeout = dep_bra_p0_fwd(e);
    dep_bra_to(e, L);
    dep_loop_extend(e, L, here(e) - 1);
    const int at_timeout = here(e);
    /* only lane 0 adds the sentinel (a warp-wide ATOMG adds once per active lane): sent = ((tid - 1) >> 31) * SENTINEL, tid < 32 here */
    dep_imadi(e, sent, one, 0xffffffffu, tid); dep_shr(e, sent, sent, 31); dep_imadi(e, sent, sent, OMEGA_DEP_TIMEOUT_SENTINEL, -1);
    dep_insn(e, BW_IR_ATOMG_ADD_STRONG_GPU, scratch, D->pdone, sent, udesc, K_VAR); /* names this launch to the host; no hang */
    const int at_join = here(e);
    patch_bra(e, to_join, at_join); patch_bra(e, to_done, at_join); patch_bra(e, to_timeout, at_timeout);
    { BlackwellIRInsn bsy = I0(BW_IR_BSYNC); bsy.bar_reg = 0; emit(e, bsy, K_BSYNC); } patch_bra(e, at_bssy, at_join + 1); /* BSYNC at at_join */
    dep_bar(e);
    emit(e, I0(BW_IR_CCTL_IVALL), K_FIXED);     /* acquire: no stale L1 line of a buffer the previous launch rewrote */
}

/* Epilogue: call before EVERY exit of the kernel, with all threads of the CTA converged (after the output stores). */
static void dep_epilogue(Em *e, int udesc, int tid, OmegaBwDep *D) {
    if (!D->dep) return;
    int one = V(e), scratch = V(e);
    if (e->err) return;
    dep_movi(e, one, 1);
    if (D->dep & OMEGA_DEP_FLAGS) {
        emit(e, I0(BW_IR_MEMBAR_ALL_GPU), K_FIXED); emit(e, I0(BW_IR_ERRBAR), K_FIXED); emit(e, I0(BW_IR_CGAERRBAR), K_FIXED);
    }
    dep_bar(e);
    dep_isetp_ge(e, tid, one);                  /* P0: not thread 0 -> skip */
    const int at_bssy = here(e); { BlackwellIRInsn i = I0(BW_IR_BSSY); i.bar_reg = 0; i.imm = 2; emit(e, i, K_BSSY); }
    const int skip = dep_bra_p0_fwd(e);
    if ((D->dep & OMEGA_DEP_FLAGS) && !(D->dep & OMEGA_DEP_TEST_NO_EPILOGUE)) dep_insn(e, BW_IR_ATOMG_ADD_STRONG_GPU, scratch, D->pdone, one, udesc, K_VAR); /* TEST bit: never reports */
    if (D->dep & OMEGA_DEP_CAMERA) {
        int t1 = V64(e), pslot = V64(e), slot = V(e), cam = V64(e), rec = V64(e);
        if (e->err) return;
        dep_insn(e, BW_IR_CS2R_GLOBALTIMER, t1, -1, -1, -1, K_VAR);
        { BlackwellIRInsn i = I0(BW_IR_IMAD_WIDE); i.dst_vreg = pslot; i.src1_vreg = one; i.imm = 4u; i.src3_vreg = D->pdone; emit(e, i, K_FIXED); }
        dep_insn(e, BW_IR_ATOMG_ADD_STRONG_GPU, slot, pslot, one, udesc, K_VAR);
        { BlackwellIRInsn i = I0(BW_IR_LDG_E_64); i.dst_vreg = cam; i.src1_vreg = D->pdone; i.imm = 8u; i.ureg = udesc; emit(e, i, K_VAR); }
        { BlackwellIRInsn i = I0(BW_IR_LOP3_AND); i.dst_vreg = slot; i.src1_vreg = slot; i.imm = OMEGA_DEP_CAM_SLOTS - 1u; emit(e, i, K_FIXED); }
        { BlackwellIRInsn i = I0(BW_IR_IMAD_WIDE); i.dst_vreg = rec; i.src1_vreg = slot; i.imm = 16u; i.src3_vreg = cam; emit(e, i, K_FIXED); }
        dep_isetp_ge(e, cam, one);              /* P0: camera base non-zero (low word; the host keeps bases above 4 GB-aligned pages) */
        const int at_bssy2 = here(e); { BlackwellIRInsn i = I0(BW_IR_BSSY); i.bar_reg = 1; i.imm = 2; emit(e, i, K_BSSY); }
        const int to_store = dep_bra_p0_fwd(e);
        const int to_end = here(e); dep_bra_to(e, to_end + 1);
        patch_bra(e, to_store, here(e));
        for (unsigned q = 0; q < 4; q++) { BlackwellIRInsn st = I0(BW_IR_STG_E); st.src1_vreg = rec; st.src2_vreg = q < 2 ? D->t0 : t1; st.src2_subreg = (uint8_t)(q & 1u); st.imm = 4u * q; st.ureg = udesc; emit(e, st, K_STORE); }
        patch_bra(e, to_end, here(e));
        { BlackwellIRInsn i = I0(BW_IR_BSYNC); i.bar_reg = 1; emit(e, i, K_BSYNC); }
        patch_bra(e, at_bssy2, here(e)); /* BSSY imm = distance to its BSYNC + 1 as bssy_fwd/patch_bra use (target = at_bsync + 1) */
    }
    const int at_bsync = here(e);
    { BlackwellIRInsn i = I0(BW_IR_BSYNC); i.bar_reg = 0; emit(e, i, K_BSYNC); }
    patch_bra(e, skip, at_bsync); patch_bra(e, at_bssy, at_bsync + 1);
}
#endif
