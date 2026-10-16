/* omega_blackwell_codegen_tgemm.c: device-resident bf16 training GEMM (trainer lane E1, cut 1: NN, F32 overwrite). See the
 * header. The K loop, staging and scheduling follow omega_blackwell_codegen_tc2.c (chip gate OWN30B-L1G-GEMM-1e76e728). */
#include "omega_blackwell_codegen_tgemm.h"
#include "omega_bw_em.h"

int omega_tgemm_spec_ok(const OmegaTgemmSpec *s) {
    if (!s) return 0;
    if (s->k < 128u || s->k > 65536u || s->k % 128u) return 0;
    if (s->rt != 2 && s->rt != 4 && s->rt != 8) return 0;
    if (s->ct != 2) return 0; /* the paired column tiles share one B word per lane */
    if (s->warps != 8 && s->warps != 16) return 0;
    if (s->kc != 32) return 0;
    if (s->aqn != 1 && s->aqn != 2) return 0;
    if (s->layout != OMEGA_TGEMM_LAYOUT_NN || s->epi != OMEGA_TGEMM_EPI_F32) return 0;
    const uint32_t span = s->warps * 8u * s->ct;
    if (s->n == 0 || s->n % span || s->n >= (1u << 17)) return 0;
    if ((16u * s->rt * (s->kc / 4u)) % (32u * s->warps)) return 0; /* whole LDG.64 staging pieces per thread */
    {   /* the pieces of one thread are RJ rows apart: constant load offsets that must stay under the encoder's 2^23 */
        const uint32_t RJ = 32u * s->warps / (s->kc / 4u), nj = 16u * s->rt * (s->kc / 4u) / (32u * s->warps);
        if (RJ % 16u) return 0;
        if ((uint64_t)(nj - 1u) * RJ * s->k * 2u + (128u / s->kc - 1u) * s->kc * 2u >= (1u << 23)) return 0;
    }
    if (2u * 16u * s->rt * s->kc * 2u > 16384u) return 0;           /* QMD shared cap */
    if (32u * span > 2u * 16u * s->rt * s->kc * 2u) return 0;         /* an 8-row output pass must fit the staging buffers */
    if ((8u * span) % (32u * s->warps)) return 0;
    if (s->mutant < 0 || s->mutant > 5) return 0;
    return 1;
}
uint32_t omega_tgemm_shared_bytes(const OmegaTgemmSpec *s) { return omega_tgemm_spec_ok(s) ? 2u * 16u * s->rt * s->kc * 2u : 0u; }
uint32_t omega_tgemm_span(const OmegaTgemmSpec *s) { return omega_tgemm_spec_ok(s) ? s->warps * 8u * s->ct : 0u; }
uint32_t omega_tgemm_bm(const OmegaTgemmSpec *s) { return omega_tgemm_spec_ok(s) ? 16u * s->rt : 0u; }

/* d = lut(a[asub], b[bsub], c) */
static void lop3s(Em *e, int d, int a, unsigned asub, int b, unsigned bsub, int c, uint32_t lut) {
    BlackwellIRInsn i = I0(BW_IR_LOP3_LUT); i.dst_vreg = d; i.src1_vreg = a; i.src1_subreg = (uint8_t)asub; i.src2_vreg = b; i.src2_subreg = (uint8_t)bsub; i.src3_vreg = c; i.imm = lut; emit(e, i, K_FIXED);
}
/* d[dsub] = a * imm + c */
static void imadi_d(Em *e, int d, unsigned dsub, int a, uint32_t imm, int c) {
    BlackwellIRInsn i = I0(BW_IR_IMAD); i.dst_vreg = d; i.dst_subreg = (uint8_t)dsub; i.src1_vreg = a; i.imm = imm; i.src3_vreg = c; emit(e, i, K_FIXED);
}
/* d[dsub] = a >> imm (SHF.R.U32.HI) */
static void shr_d(Em *e, int d, unsigned dsub, int a, uint32_t imm) {
    BlackwellIRInsn i = I0(BW_IR_SHF_R); i.dst_vreg = d; i.dst_subreg = (uint8_t)dsub; i.src1_vreg = a; i.imm = imm; emit(e, i, K_FIXED);
}
/* d[dsub] = a[asub] + b */
static void iadd_d(Em *e, int d, unsigned dsub, int a, unsigned asub, int b) {
    BlackwellIRInsn i = I0(BW_IR_IADD3); i.dst_vreg = d; i.dst_subreg = (uint8_t)dsub; i.src1_vreg = a; i.src1_subreg = (uint8_t)asub; i.src2_vreg = b; emit(e, i, K_FIXED);
}
static void sts32_sub(Em *e, int addr, int val, unsigned sub) {
    BlackwellIRInsn st = I0(BW_IR_STS32); st.src1_vreg = addr; st.src2_vreg = val; st.src2_subreg = (uint8_t)sub; emit(e, st, K_STORE);
}

int omega_blackwell_codegen_tgemm_prog(const OmegaTgemmSpec *spec, BlackwellIRProgram *prog) {
    if (!omega_tgemm_spec_ok(spec) || !prog) return -1;
    const uint32_t K = spec->k, N = spec->n, RT = spec->rt, CT = spec->ct, W = spec->warps, T = 32u * W, KC = spec->kc, AQN = spec->aqn;
    const uint32_t BM = 16u * RT, SPAN = W * 8u * CT, NS = KC / 16u /* HMMA steps per chunk */, NCH = 128u / KC /* chunks per block */;
    const uint32_t PPR = KC / 4u /* LDG.64 pieces per row of a chunk */, NJ = BM * PPR / T, BUF = BM * KC * 2u;
    omega_bw_ir_init(prog);
    Em em; memset(&em, 0, sizeof em); em.p = prog;
    Em *e = &em;

    /* ---- prologue: arguments, ids, tile entry ---- */
    int udesc = UV64(e), ptab = V64(e), pa = V64(e), pbb = V64(e);
    int tid = V(e), cx = V(e), cy = V(e), warp = V(e), lane = V(e), g = V(e), tq = V(e), one = V(e), tent = V64(e);
    int rk = V64(e) /* row0, kb0 */, nkb = V64(e) /* nkb, pad */;
    int n0w = V(e), tmp = V(e), tmp2 = V(e), pb = V64(e), la = V(e), msel = V(e);
    if (e->err) return -1;
    ldcu64(e, udesc, 0x358);
    ldc64(e, ptab, 0x380); ldc64(e, pa, 0x388); ldc64(e, pbb, 0x390);
    s2r(e, tid, BW_SR_TID_X); s2r(e, cx, BW_SR_CTAID_X); s2r(e, cy, BW_SR_CTAID_Y);
    movi(e, one, 1);
    shr(e, warp, tid, 0, 5); andi(e, lane, tid, 0, 31); shr(e, g, lane, 0, 2); andi(e, tq, lane, 0, 3);
    imadw(e, tent, cy, OMEGA_TGEMM_TILE_BYTES, ptab);
    ldg_wide(e, BW_IR_LDG_E_64, rk, tent, 8, udesc, 1);
    ldg_wide(e, BW_IR_LDG_E_64, nkb, tent, 16, udesc, 0);
    /* this warp's first output column n0w; B fragment pointer: row kb0 * 128 + 8 t, column n0w + 4 (g / 2) */
    imadi(e, n0w, cx, 0, SPAN, NOREG); imadi(e, tmp, warp, 0, 8u * CT, NOREG); iadd(e, n0w, n0w, tmp);
    imadi(e, tmp, rk, 1, 128u, NOREG); imadi(e, tmp, tq, 0, 8u, tmp); imadi(e, tmp, tmp, 0, N, n0w);
    shr(e, tmp2, g, 0, 1); imadi(e, tmp, tmp2, 0, 4u, tmp); imadw(e, pb, tmp, 2u, pbb);
    /* half-word select mask: odd g takes the high word of its 8 B row bytes (mutant 4 swaps the parity) */
    andi(e, tmp, g, 0, 1); if (spec->mutant == 4) imadi(e, tmp, tmp, 0, 0xffffffffu, one);
    imadi(e, msel, tmp, 0, 0xffffffffu, NOREG);
    imadi(e, la, lane, 0, 16u, NOREG);

    /* ---- staging assignment: piece = tid + jj * T -> row r = piece / PPR, pp = piece % PPR (4 bf16 at k 4 pp) ---- */
    /* piece jj of a thread is piece 0 moved RJ = T / PPR rows down: the same pp, row + jj RJ, so one address register and one
     * shared offset serve all pieces (constant byte offsets jj * RJ * K * 2 on the load, jj * (RJ / 16) * NS * 512 on the store) */
    const uint32_t RJ = T / PPR, GXJ = RJ * K * 2u, SAJ = (RJ / 16u) * NS * 512u;
    int gx = V64(e), sa = V(e), r = V(e), pp = V(e), hh = V(e);
    if (e->err) return -1;
    {
        shr(e, r, tid, 0, (uint32_t)log2u(PPR)); andi(e, pp, tid, 0, PPR - 1u);
        /* global: a + ((row0 + r) * K + kb0 * 128 + 4 pp) * 2 */
        iadd(e, tmp, rk, r); imadw(e, gx, tmp, K * 2u, pa);
        imadi(e, tmp, rk, 1, 256u, NOREG); imadi(e, tmp, pp, 0, 8u, tmp); imadw(e, gx, tmp, 1, gx);
        /* shared: (r / 16) * NS * 512 + (pp & 1) * 512 + (4 (r % 8) + pp / 2) * 16 + 4 ((r / 8) & 1) */
        shr(e, tmp, r, 0, 4); imadi(e, tmp, tmp, 0, NS * 512u, NOREG);
        andi(e, tmp2, pp, 0, 1); imadi(e, tmp, tmp2, 0, 512u, tmp);
        andi(e, tmp2, r, 0, 7); imadi(e, tmp2, tmp2, 0, 4u, NOREG); shr(e, hh, pp, 0, 1); iadd(e, tmp2, tmp2, hh); imadi(e, tmp, tmp2, 0, 16u, tmp);
        shr(e, hh, r, 0, 3); andi(e, hh, hh, 0, 1); imadi(e, sa, hh, 0, 4u, tmp);
    }

    /* ---- loop-wide registers ---- */
    int acc[16], bq[2][8], stgr[8], aq[2], wsel[2], bE = V64(e), bO = V64(e), tw = V(e), cntk = V(e);
    for (uint32_t x = 0; x < RT * CT; x++) acc[x] = V128(e);
    for (unsigned set = 0; set < 2; set++) for (uint32_t j = 0; j < 4u * NS; j++) bq[set][j] = V64(e);
    for (uint32_t jj = 0; jj < NJ; jj++) stgr[jj] = V64(e);
    for (uint32_t q = 0; q < AQN; q++) aq[q] = V128(e);
    for (unsigned j = 0; j < 2; j++) wsel[j] = V(e);
    if (e->err) return -1;

    /* A pieces of chunk c (immediate c * KC * 2) and the B rows of the chunk (pb points at its row kb0 * 128 + chunk * KC + 8 t) */
    #define ISSUE(set, sb, chunk) do { \
        for (uint32_t jj = 0; jj < NJ; jj++) ldw_pf(e, BW_IR_LDG_E_64, stgr[jj], gx, jj * GXJ + (chunk) * KC * 2u, udesc, (sb)); \
        for (uint32_t s = 0; s < NS; s++) for (uint32_t j = 0; j < 4; j++) ldw_pf(e, BW_IR_LDG_E_64, bq[set][s * 4u + j], pb, (4u * s + j) * N * 2u, udesc, (sb)); } while (0)
    #define STAGE(buf) do { for (uint32_t jj = 0; jj < NJ; jj++) { \
        imadi(e, tmp, one, 0, (buf) + jj * SAJ, sa); sts32_sub(e, tmp, stgr[jj], 0); \
        imadi(e, tmp, one, 0, (buf) + jj * SAJ + 8u, sa); sts32_sub(e, tmp, stgr[jj], 1); } bar_sync(e); } while (0)
    /* B fragments of step s: W_j = half word (g % 2) of row word j (LOP3 select); E: (W0.lo, W1.lo), (W2.lo, W3.lo) by
     * IMAD W1 * 2^16 + (W0 & 0xffff); O: (W0.hi, W1.hi), (W2.hi, W3.hi) by (W0 >> 16) + (W1 & 0xffff0000). (IMAD.HI.U32 would do
     * the O half in one instruction, but its Rc is a 64-bit pair on the chip: odd columns came back wrong, first chip run.) */
    #define BFRAG(set, s) do { for (unsigned h = 0; h < 2; h++) { \
        for (unsigned j = 0; j < 2; j++) lop3s(e, wsel[j], bq[set][(s) * 4u + 2 * h + j], 0, bq[set][(s) * 4u + 2 * h + j], 1, msel, 0xd8u); \
        andi(e, tw, wsel[0], 0, 0xffffu); imadi_d(e, bE, h, wsel[1], 0x10000u, tw); \
        andi(e, tw, wsel[1], 0, 0xffff0000u); shr_d(e, bO, h, wsel[0], 16u); iadd_d(e, bO, h, bO, h, tw); } } while (0)
    #define COMPUTE(buf, set, last) do { \
        for (uint32_t s = 0; s < NS; s++) { \
            if (spec->mutant == 1 && (last) && s == NS - 1u) break; \
            BFRAG(set, s); \
            for (uint32_t i0 = 0; i0 < RT; i0 += AQN) { \
                const uint32_t nb = RT - i0 < AQN ? RT - i0 : AQN; \
                for (uint32_t q = 0; q < nb; q++) { uint32_t i = i0 + q, ii = spec->mutant == 3 ? (i ^ 1u) : i; \
                    lds128(e, aq[q], la, (buf) + (ii * NS + s) * 512u, q == 0); } \
                for (uint32_t q = 0; q < nb; q++) { const uint32_t x = (i0 + q) * CT; \
                    hmma(e, acc[x], aq[q], bE, acc[x], 8); hmma(e, acc[x + 1], aq[q], bO, acc[x + 1], 8); } \
            } \
        } } while (0)

    for (uint32_t x = 0; x < RT * CT; x++) for (unsigned q = 0; q < 4; q++) { BlackwellIRInsn mz = I0(BW_IR_MOV_RZ); mz.dst_vreg = acc[x]; mz.dst_subreg = (uint8_t)q; emit(e, mz, K_FIXED); }
    ISSUE(0, 3u, 0u);
    consume(e, 3);
    STAGE(0u);
    imadi(e, cntk, nkb, 0, 1, NOREG);

    /* ---- K loop: one 128-wide block (NCH chunks) per iteration; sets and buffers alternate per chunk (NCH even) ---- */
    const int L_k = here(e);
    loop_begin(e);
    for (uint32_t c = 0; c < NCH; c++) {
        const unsigned cur = c & 1u, nxt = cur ^ 1u;        /* set / buffer holding chunk c; the other receives chunk c + 1 */
        const uint32_t cbuf = cur ? BUF : 0u, nbuf = nxt ? BUF : 0u;
        if (c + 1u < NCH) {
            imadw(e, pb, one, KC * N * 2u, pb);             /* B rows of chunk c + 1 */
            ISSUE(nxt, nxt ? 4u : 3u, c + 1u);
        } else {
            /* the next block's chunk 0: advance only when a block follows (nf = cntk != 1); the last iteration re-reads its own */
            int nf = V(e); if (e->err) return -1;
            imadi(e, nf, one, 0, 0xfffffffeu, cntk); shr(e, nf, nf, 0, 31); imadi(e, nf, nf, 0, 0xffffffffu, one);
            imadw(e, gx, nf, spec->mutant == 2 ? 0u : 256u, gx);
            imadw(e, pb, nf, KC * N * 2u, pb);
            ISSUE(nxt, nxt ? 4u : 3u, 0u);
        }
        COMPUTE(cbuf, cur, c + 1u == NCH);
        consume(e, nxt ? 4u : 3u);
        STAGE(nbuf);
    }
    imadi(e, cntk, one, 0, 0xffffffffu, cntk);
    isetp_ge_u32(e, cntk, one);
    bra_if_p0(e, L_k);
    loop_end(e);
    #undef ISSUE
    #undef STAGE
    #undef BFRAG
    #undef COMPUTE

    /* ---- epilogue: the HMMA results must have settled. The tile goes out through shared memory in 8-row passes so that every
     * warp store writes 128 contiguous bytes: storing straight from the fragment layout (a lane owns 16 bytes per row, one 4-byte
     * STG per word) touched 16 half-filled sectors per instruction and cost more than half of the kernel time at K 768
     * (chip probe: the no-store mutant 5 ran 8192x768x768 in 0.150 ms against 0.334 ms). Addresses in fresh registers. ---- */
    nop(e, 15); nop(e, 14);
    {
        const uint32_t RJ2 = T / SPAN /* rows per read step */, NJ2 = 8u * SPAN / T /* words per thread per pass */;
        int tid2 = V(e), cx2 = V(e), cy2 = V(e), lane2 = V(e), g2 = V(e), tq2 = V(e), warp2 = V(e), tab2 = V64(e), ten2 = V64(e), cva2 = V64(e), rk2 = V64(e);
        int onee = V(e), p0 = V64(e), pp2 = V64(e), sw[4], sr[4], rd[4];
        for (unsigned w = 0; w < 4; w++) sw[w] = V(e);
        for (uint32_t j = 0; j < NJ2; j++) { sr[j] = V(e); rd[j] = V(e); }
        if (e->err) return -1;
        ldc64(e, tab2, 0x380);
        s2r(e, tid2, BW_SR_TID_X); s2r(e, cx2, BW_SR_CTAID_X); s2r(e, cy2, BW_SR_CTAID_Y);
        imadw(e, ten2, cy2, OMEGA_TGEMM_TILE_BYTES, tab2);
        ldg_wide(e, BW_IR_LDG_E_64, cva2, ten2, 0, udesc, 1);
        ldg_wide(e, BW_IR_LDG_E_64, rk2, ten2, 8, udesc, 0);
        andi(e, lane2, tid2, 0, 31); shr(e, g2, lane2, 0, 2); andi(e, tq2, lane2, 0, 3); shr(e, warp2, tid2, 0, 5); movi(e, onee, 1);
        /* the lane's four slots of a pass: (g * SPAN + warp * 16 + 4 t + w) * 4 (row g of the pass, the warp's columns) */
        imadi(e, tmp, g2, 0, SPAN, NOREG); imadi(e, tmp, warp2, 0, 16u, tmp); imadi(e, tmp, tq2, 0, 4u, tmp); imadi(e, tmp2, tmp, 0, 4u, NOREG);
        for (unsigned w = 0; w < 4; w++) imadi(e, sw[w], onee, 0, 4u * w, tmp2);
        /* the thread's read slots: word q = tid + T j (consecutive lanes, consecutive words): row q / SPAN = tid / SPAN + j RJ2 */
        for (uint32_t j = 0; j < NJ2; j++) { imadi(e, tmp, onee, 0, j * T, tid2); imadi(e, sr[j], tmp, 0, 4u, NOREG); }
        /* p0 = c + ((row0 + tid / SPAN) * N + cx * SPAN + tid % SPAN) * 4; pass (i, h) adds (16 i + 8 h) rows, read step j adds j RJ2 rows */
        shr(e, tmp, tid2, 0, (uint32_t)log2u(SPAN)); iadd(e, tmp, rk2, tmp); imadi(e, tmp, tmp, 0, N, NOREG);
        imadi(e, tmp2, cx2, 0, SPAN, tmp); andi(e, tmp, tid2, 0, SPAN - 1u); iadd(e, tmp, tmp, tmp2); imadw(e, p0, tmp, 4u, cva2);
        for (uint32_t i = 0; i < RT; i++) for (unsigned h = 0; h < 2; h++) {
            if (spec->mutant == 5) continue; /* timing probe only: no output */
            bar_sync(e); /* the previous pass's reads (or the K loop's last LDS) are complete before the slots are rewritten */
            for (unsigned w = 0; w < 4; w++) sts32_sub(e, sw[w], acc[i * CT + (w & 1u)], 2u * h + (w >> 1));
            bar_sync(e);
            for (uint32_t j = 0; j < NJ2; j++) { BlackwellIRInsn ld = I0(BW_IR_LDS32); ld.dst_vreg = rd[j]; ld.src1_vreg = sr[j]; emit(e, ld, j == 0 ? K_VAR : K_VAR_MORE); }
            imadw(e, pp2, onee, (16u * i + 8u * h) * N * 4u, p0);
            for (uint32_t j = 0; j < NJ2; j++) stg(e, pp2, j * RJ2 * N * 4u, rd[j], udesc);
        }
    }
    tail(e);
    if (e->err) return -1;
    if (prog->count > OMEGA_TGEMM_MAX_INSNS) return -1;
    if (omega_bw_regalloc_solve(prog) != 0) return -1;
    if (!omega_bw_gpr_declared_ok(omega_bw_gpr_declared(prog->regalloc.peak_gpr_usage), prog->regalloc.peak_gpr_usage) || omega_bw_gpr_declared(prog->regalloc.peak_gpr_usage) > 128u) return -1;
    return 0;
}
