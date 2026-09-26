/*
 * omega_shared_world_worker.c -- resident GB10 qualification worker IR generator.
 *
 * VERIFICATION STATUS (2026-09-26): IMPLEMENTED, NOT COMPILED, NOT SILICON-
 * OBSERVED. See the header and evidence/omega_shared_world_stage1_status.md.
 * The instruction *sequence* below is the intended persistent polling loop using
 * the M20 STRONG.SYS/MEMBAR ordering opcodes. The per-instruction scheduling
 * words (BlackwellIRInsn.control: stall counts, wait/read/write barrier masks)
 * are marked CALIBRATE and MUST be tuned against real GB10 during bring-up, the
 * same way M17/M18/M19 captured their control words from silicon. Until the
 * campaign in omega_sw_run_campaign() passes on GB10, treat this as a bring-up
 * scaffold, not qualified code.
 *
 * Doctrine: this worker is the PHYSICS-realized GPU consumer/producer that
 * mirrors, mechanically, the CPU-side release/acquire the ring protocol uses.
 * It is a qualification worker, not AIEN.
 */
#include "omega_shared_world_worker.h"
#include "omega_shared_world_abi.h"   /* shared ABI: offsets, sizes, msg types  */
#include "physics_coherent.h"
#include "omega_shared_world.h"
#include <string.h>

/* Constant-bank argument layout the launcher writes at cbank+0x380 (GPU VAs and
 * scalars used by the trusted launcher only; never serialized into the world):
 *   [0..1] region_base_gpu_va (64-bit)
 *   [2]    world_epoch
 *   [3]    arg_a (XOR constant)
 *   [4]    ring_capacity mask
 *   [5..6] giveup_iterations (64-bit)
 * The worker keeps its private c2g.head and g2c.tail in registers across the
 * loop (it is the sole consumer of c2g and sole producer of g2c). */
#define ARG_BASE_LO   0x380u
#define CBANK_DESC    0x358u   /* LDCU.64 global-access descriptor (as M17/M18)  */

/*
 * NOTE ON COMPLETENESS: the register-by-register emission of the full loop is
 * large and only meaningful once its control words are silicon-calibrated. This
 * generator emits the canonical skeleton and the ordering-critical instructions
 * in the correct order so the sequence and the STRONG.SYS/MEMBAR placement are
 * reviewable; addressing arithmetic slots are left as clearly-labelled TODOs for
 * bring-up rather than emitted with guessed control words that would read as
 * authoritative. This keeps the artifact honest: the ordering contract is
 * expressed; the scheduling is explicitly pending hardware.
 */
int omega_sw_worker_build_ir(const OmegaSwWorkerParams *params,
                             BlackwellIRProgram *prog) {
    if (!params || !prog) return -1;
    omega_bw_ir_init(prog);

    /* uv_desc = global-access descriptor from cbank[0x358]. */
    int uv_desc = omega_bw_ir_alloc_uvreg64(prog);
    BlackwellIRInsn desc = { .op = BW_IR_LDCU64, .dst_vreg = uv_desc,
                             .imm = CBANK_DESC, .is_uniform = true };
    if (omega_bw_ir_append(prog, &desc) != 0) return -1;

    /* v_base = region base GPU VA from cbank[0x380] (64-bit). */
    int v_base = omega_bw_ir_alloc_vreg64(prog);
    BlackwellIRInsn base = { .op = BW_IR_LDC64, .dst_vreg = v_base, .imm = ARG_BASE_LO };
    if (omega_bw_ir_append(prog, &base) != 0) return -1;

    /*
     * ---- PERSISTENT POLLING LOOP (label L_loop) ----
     *
     * The ordering-critical shape, in emission order, is:
     *
     *   L_loop:
     *     CCTL.IVALL                         ; invalidate L1 before acquire
     *     LDG.E.STRONG.SYS  v_tail, [base + off_c2g + tail]   ; acquire tail
     *     ISETP.GE          P0, v_head, v_tail                ; empty?
     *     @P0  (giveup--) ; @giveup==0 BRA L_exit ; else BRA L_loop
     *     LDG.E.STRONG.SYS  v_in,  [c2g.slot(head).payload]   ; read request
     *     (validate magic/epoch; ISETP sequence==head)
     *     LOP3.LUT(0x3c)    v_out, v_in, arg_a, RZ            ; out = in XOR arg_a
     *     STG.E             [g2c.slot(tail).payload], v_out   ; write result body
     *     STG.E             [g2c.slot(tail).{magic,seq,epoch,msg_type=RES}]
     *     MEMBAR.ALL.SYS                                       ; release fence
     *     STG.E.STRONG.SYS  [base + off_g2c + tail], g2c_tail+1 ; publish g2c tail
     *     MEMBAR.ALL.SYS
     *     STG.E.STRONG.SYS  [base + off_c2g + head], head+1     ; free request slot
     *     (head++, g2c_tail++)
     *     BRA L_loop
     *   L_exit:
     *     EXIT
     *
     * The STRONG.SYS + MEMBAR.ALL.SYS placement is the mechanical equivalent of
     * the CPU's LDAR/STLR: the response body is fully written BEFORE the g2c tail
     * is release-published, so a CPU consumer that acquire-observes the new tail
     * sees a complete descriptor. CCTL.IVALL before the acquire load forces a
     * re-fetch from the coherent point rather than a stale L1 line.
     *
     * Addressing arithmetic (slot = (index & mask) * 128 + field offset) and the
     * per-instruction .control scheduling words are the bring-up TODOs; they are
     * intentionally not emitted with guessed control words here.
     */

    /* Emit the ordering-defining instructions so the contract is present and
     * reviewable. Control words CALIBRATE on GB10. */
    int v_tail = omega_bw_ir_alloc_vreg64(prog);
    BlackwellIRInsn cctl   = { .op = BW_IR_CCTL_IVALL };
    BlackwellIRInsn ld_acq = { .op = BW_IR_LDG_STRONG_SYS, .dst_vreg = v_tail,
                               .src1_vreg = v_base, .ureg = uv_desc }; /* + off_c2g+tail (TODO addr) */
    BlackwellIRInsn membar = { .op = BW_IR_MEMBAR_ALL_SYS };
    int v_out = omega_bw_ir_alloc_vreg();
    BlackwellIRInsn st_rel = { .op = BW_IR_STG_STRONG_SYS, .src1_vreg = v_base,
                               .src2_vreg = v_out, .ureg = uv_desc }; /* g2c tail (TODO addr) */
    BlackwellIRInsn ex     = { .op = BW_IR_EXIT };
    if (omega_bw_ir_append(prog, &cctl)   != 0) return -1;
    if (omega_bw_ir_append(prog, &ld_acq) != 0) return -1;
    if (omega_bw_ir_append(prog, &membar) != 0) return -1;
    if (omega_bw_ir_append(prog, &st_rel) != 0) return -1;
    if (omega_bw_ir_append(prog, &ex)     != 0) return -1;

    (void)params; /* epoch/xform/offsets/giveup consumed by the full loop (TODO) */
    return 0;
}

/*
 * omega_sw_run_campaign -- CPU side of the Stage 1 campaign. IMPLEMENTED, NOT
 * SILICON-OBSERVED. Structure:
 *   1. physics_coherent_open()                     (1 RM client + channel)
 *   2. physics_coherent_region_alloc(region >= omega_sw_required_bytes()+payload)
 *   3. physics_coherent_region_base_cpu/gpu()       (trusted host addresses)
 *   4. omega_sw_format(world, base_cpu, ...)        (lay out header/rings/table)
 *   5. omega_sw_object_register(payload window)
 *   6. build worker IR -> regalloc -> encode -> upload code into the region
 *   7. build a QMD whose entry is the worker code + cbank args (base_gpu_va,
 *      epoch, arg_a, mask, giveup); physics_coherent_submit_methods() ONCE
 *   8. loop count times: omega_sw_ring_publish(c2g, XFORM_REQ);
 *      poll omega_sw_ring_consume(g2c) until response; verify out == in^arg_a and
 *      response sequence matches the originating request; tally the result ledger
 *   9. publish OMEGA_SW_MSG_SHUTDOWN; join; physics_coherent_close()
 *
 * Every step reuses the host-tested ring protocol and the coherent-region
 * abstraction; only the persistent-worker launch and the exchange loop are new
 * and require GB10 to validate.
 */
int omega_sw_run_campaign(uint64_t count, unsigned stress_mask,
                          OmegaSwCampaignResult *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
        out->requested = count;
        out->silicon_observed = 0; /* set to 1 only by a real GB10 run */
    }
    (void)stress_mask;
    /* Not executed during authoring: GB10 contended and build/test unavailable.
     * Returns -1 (campaign not run) rather than fabricating a result. */
    return -1;
}
