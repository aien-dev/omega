/* Turing Yield: read-only CTR1 search-trace reader (TY-2 data).
 *
 * Format source (read-only reference, Rust): aien-sovereign-core
 * crates/crumbs/src/trace.rs:26 (TRACE_RECORD_BYTES = 247), :406-436
 * (TraceRecord::encode_body field order), crates/crumbs/src/canon.rs:23-30
 * (little-endian integers), trace.rs:460-475 (body || 32-byte BLAKE3 chain
 * digest; files are headerless record streams, session.rs:196-203).
 * Byte offsets in the 247-byte record:
 *   0 "CTR1", 4 version u16 (=1), 6 kind u8, 7 result_class u8,
 *   8 event_index u32, 12 parent u32, 16 child u32, 20 op_index u16,
 *   22 op_origin u8, 23 prune u8, 24 verify u8, 25 fit u8, 26 hidden u8,
 *   27 improved u8, 28 contributed u8, 29 depth u16, 31 op_id[32],
 *   63 state_digest[32], 95 child_state_digest[32], 127 program_digest[32],
 *   159 mismatch_before u16, 161 mismatch_after u16, 163 hamming_before u32,
 *   167 hamming_after u32, 171 exec_cost u32, 175 search_cost u32,
 *   179 program_steps u16, 181 oracle_index u16, 183 residual u64[4],
 *   215 chain digest[32].
 * The BLAKE3 chain is NOT verified here (no C BLAKE3 in the tree); integrity
 * of the corpus is pinned by per-file SHA-256 instead (ty_record.h).
 *
 * The reader refuses (TY_E_FORMAT) any record outside the pinned value sets,
 * so a corpus that drifted from the declared alphabet is never silently scored.
 */
#ifndef TURING_TY_CTR1_H
#define TURING_TY_CTR1_H

#include <stddef.h>
#include <stdint.h>

#define TY_CTR1_BYTES 247
#define TY_CTR1_NOPS 15 /* base bank: src/omega_synthesis.c:11-27 via cl_program.c:149 */
#define TY_OP_SUBMIT 15 /* op feature value used for SUBMIT events */
#define TY_DEPTH_CLIP 7

/* x_t: one outcome symbol per search event (profile section 3). */
enum {
    TY_O_P_EQUIV = 0,    /* EXPAND, prune = CL_PRUNE_EQUIV */
    TY_O_P_COST = 1,     /* EXPAND, prune = CL_PRUNE_COST */
    TY_O_P_FRONTIER = 2, /* EXPAND, prune = CL_PRUNE_FRONTIER_CAP */
    TY_O_P_STEPCAP = 3,  /* EXPAND, prune = CL_PRUNE_STEP_CAP */
    TY_O_FAILED = 4,     /* EXPAND, not pruned, result_class Failed */
    TY_O_PARTIAL = 5,    /* EXPAND, not pruned, result_class Partial */
    TY_O_IMPROVED = 6,   /* EXPAND, not pruned, result_class Improved */
    TY_O_ACCEPT = 7,     /* SUBMIT, result_class Verified */
    TY_O_REJECT = 8,     /* SUBMIT, result_class RejectedHidden or RejectedRobust */
    TY_OUT_K = 9
};

/* One event: the coded symbol and the side information a model may condition on. */
typedef struct {
    uint8_t sym;   /* x_t, 0..K-1 */
    uint8_t op;    /* op_index for EXPAND (0..14), TY_OP_SUBMIT for SUBMIT */
    uint8_t depth; /* min(depth, TY_DEPTH_CLIP) */
    uint8_t first; /* 1 = first event of a crumb (context resets) */
    uint16_t crumb; /* crumb ordinal within its file (saturates at 65535) */
    uint32_t idx;  /* event_index within the crumb */
} ty_ev;

typedef struct {
    ty_ev *ev;
    size_t n, cap;
    uint32_t ncrumb;
    uint64_t gaps; /* event_index jumps inside a crumb (derive() skipped events) */
} ty_stream;

void ty_stream_init(ty_stream *s);
void ty_stream_free(ty_stream *s);
int ty_stream_push(ty_stream *s, const ty_ev *e);

/* Map one raw 247-byte record to an event, refusing out-of-set values.
 * prev_idx: event_index of the previous record in the same file, or -1 at the
 * start of the file. On success *starts_crumb says whether it opens a crumb. */
int ty_ctr1_decode(const uint8_t rec[TY_CTR1_BYTES], int64_t prev_idx, ty_ev *out, char *why, size_t whylen);

/* Read a whole CTR1 file, appending to s. Checks: size % 247 == 0, every
 * record passes ty_ctr1_decode, first record opens a crumb. Returns the number
 * of records read, or a negative TY_E_* code with a reason in why. */
int64_t ty_ctr1_read(const char *path, ty_stream *s, char *why, size_t whylen);

#endif /* TURING_TY_CTR1_H */
