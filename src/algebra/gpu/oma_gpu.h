/* Omega mixed algebra MA-6: GPU (Blackwell GB10, sm_121) realizations of
 * Omega-X, the exact ternary GEMV  y = W . x  (W in {-1,0,+1}^(m x n),
 * x int8, y int32, bit-exact vs oma_rz_oracle). spec/mixed-algebra-ma6-gpu.md
 *
 * Everything here goes through Omega's own GPU path: Omega's IR + sm_121
 * encoder (src/omega_blackwell_codegen.c), Omega's QMD/cbank builders
 * (src/omega_blackwell_qmd.c) and the physics M16 native channel. No CUDA
 * toolkit, no vendor compiler, no vendor runtime library.
 *
 * Realizations (kind):
 *   G1  int8 weights (offset binary, 4 per word), x widened to int32,
 *       IMAD multiply-add per element. Conventional INT32 SIMT.
 *   G2A 2-bit crumb codes c = w + 1 (16 per word), x widened to int32,
 *       IMAD c*x then subtract sum(x). Packed ternary, uses multiplies.
 *   G2B bit planes: P (w = +1) and M (w = -1), 32 weights per word; x sent
 *       as its 8 two's-complement bit planes; y = sum_j c_j (popc(P&X_j) -
 *       popc(M&X_j)), c_j = 2^j, c_7 = -128. No multiplies in the loop.
 *       Uses the ops added to Omega's encoder for MA-6 (BW_IR_LOP3_LUT,
 *       BW_IR_POPC); omg_selftest checks them on silicon before any result
 *       is trusted.
 *   G3  BF16 tensor core, HMMA.16816.F32.BF16, W as BF16 {-1,0,+1} in
 *       pre-packed fragment order, x as BF16 (every int8 is exact in BF16).
 *       FP32 accumulators start at 1.5*2^23, so while every partial sum
 *       stays inside (-2^22, 2^22) each accumulator holds an exact integer
 *       whose bit pattern minus 0x4B400000 is the int32 result. Exact domain
 *       declared: n <= OMG_G3_MAX_N (128 * 32768 = 2^22 bound).
 *
 * Split-K: each thread (or warp for G3) owns one row (tile) and one k slice;
 * slice results are summed with integer ATOMG.ADD into y (zeroed per call),
 * so the order of the adds cannot change the result.
 */
#ifndef OMA_GPU_H
#define OMA_GPU_H

#include <stddef.h>
#include <stdint.h>

#define OMG_G3_MAX_N 32768u

typedef enum {
    OMG_G1_I8 = 1,
    OMG_G2A_CRUMB = 2,
    OMG_G2B_PLANE = 3,
    OMG_G3_BF16 = 4
} omg_kind;

const char *omg_kind_name(omg_kind k);

typedef struct {
    omg_kind kind;
    uint32_t m, n;
    uint32_t m_pad;          /* rows in the y buffer (G3: multiple of 16) */
    uint32_t wpw;            /* weights per packed word (G3: 16 per chunk) */
    uint32_t units_per_row;  /* G1/G2: packed words per row; G3: 16-wide chunks */
    uint32_t tiles;          /* G3: 16-row tiles; else = m */
    uint32_t S;              /* k slices */
    uint32_t U;              /* units per loop trip */
    uint32_t threads, cta, grid, warps;
    size_t w0_bytes, w1_bytes, x_bytes, y_bytes, ts_bytes;
} omg_geom;

/* Chooses slices/unroll/launch shape. m, n powers of two, m <= 65536,
 * 1024 <= n <= 2^20 (G3: n <= OMG_G3_MAX_N). Returns 0 or -1 (refused). */
int omg_plan(omg_kind kind, uint32_t m, uint32_t n, omg_geom *g);

/* Pack once: canonical row-major int8 ternary W -> device weight form.
 * w1 is used only by G2B (the M plane); pass NULL otherwise. Fails closed on
 * any weight outside {-1,0,+1}. Buffers sized g->w0_bytes / g->w1_bytes. */
int omg_pack_weights(const omg_geom *g, const int8_t *w, uint32_t *w0, uint32_t *w1);

/* Per call: int8 x -> device activation form (g->x_bytes). */
int omg_pack_x(const omg_geom *g, const int8_t *x, uint32_t *xd);

/* CPU model of each kernel: computes y from the packed buffers with the same
 * index arithmetic and slice split the GPU kernel uses. Host-only check of
 * the layouts; it is not the oracle. */
int omg_model(const omg_geom *g, const uint32_t *w0, const uint32_t *w1,
              const uint32_t *xd, int32_t *y_pad);

typedef struct {
    uint8_t *code;
    size_t code_bytes;
    size_t insn_count;
    uint32_t gpr;
    uint8_t sha256[32];
} omg_kernel;

int omg_build(const omg_geom *g, omg_kernel *k);
void omg_kernel_free(omg_kernel *k);

/* ---- silicon session (one channel, buffers allocated once) ---- */
typedef struct omg_session omg_session;

int omg_open(omg_session **out, const omg_kernel *k, size_t w0, size_t w1, size_t x,
             size_t y, size_t ts);
void *omg_buf(omg_session *s, int which); /* 0 w0, 1 w1, 2 x, 3 y, 4 ts */
/* One launch: builds the push buffer, submits, waits (m16_native_wait_marker) on the completion
 * marker. host_ns = submit -> marker observed (CLOCK_MONOTONIC). gpu_ns =
 * max(end) - min(start) over per-warp %globaltimer stamps written by lane 0
 * of every warp into the ts buffer. Returns 0, or -1 on any failure. */
int omg_launch(omg_session *s, uint32_t cta, uint32_t grid, uint32_t warps,
               uint64_t *host_ns, uint64_t *gpu_ns);
int omg_close(omg_session *s);

/* Silicon checks of the encoder ops added for MA-6 and of what it relies on beyond the
 * existing gates: LOP3 register-LUT (8 LUTs), POPC, IADD3 third operand,
 * LDG immediate offset, ATOMG.ADD, %globaltimer. Writes a one-line result per
 * check to the report buffer. Returns number of failed checks (0 = pass), or
 * -1 if the launch path itself failed. */
int omg_selftest(char *report, size_t cap);



#endif
