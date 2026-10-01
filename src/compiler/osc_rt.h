/*
 * osc_rt.h -- OSC-1 bootstrap runtime (OSC-0 II.10: "built in C first as the
 * bootstrap runtime"). Shared by the reference interpreter and natively
 * compiled code so both observe the same allocation semantics.
 *
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 *
 * Pool: OSC_RT_SLOTS fixed slots of OSC_MAX_ARRAY_LEN 8-byte cells. Allocation
 * takes the lowest-numbered free slot (deterministic). Release zeroes the
 * cells it owned and frees the slot. Double release or releasing an address
 * that is not a live slot start is a runtime invariant violation
 * (OSC_TRAP_RUNTIME). An OSC-2 arena occupies one slot of K <= 64 cells and
 * bump-allocates inside it (TRAP ARENA_FULL past K); its objects are freed
 * together by arena_destroy. Every alloc/release and arena open/alloc/destroy
 * is appended to an event log that
 * tests replay through the II.11 model (src/compiler/model/).
 *
 * Native ABI (docs/osc/OSC-1-DESIGN.md "Native ABI"): every compiled function
 * receives the OscRt* in x7. The entry points below (alloc, release, trap and,
 * for OSC-2 arenas, arena_open, arena_alloc, arena_destroy) are reached with
 * BLR through the function-pointer fields of OscRt, at the fixed offsets
 * asserted in osc_rt.c. osc_rt_trap never returns: it records the code and
 * longjmps back to osc_rt_call_native.
 */
#ifndef OSC_RT_H
#define OSC_RT_H

#include <setjmp.h>
#include <stdint.h>
#include "osc_ir.h"

#define OSC_RT_SLOTS     64
#define OSC_RT_EVENTS  4096

typedef enum {
    OSC_RT_EV_ALLOC = 1,
    OSC_RT_EV_RELEASE = 2,
    /* OSC-2 arenas (docs/osc/OSC-2-DESIGN.md section 3). For the three arena
     * events, slot is the arena's pool slot; REGION_OPEN / REGION_DESTROY carry
     * len = capacity K and serial = the region serial; ARENA_ALLOC carries the
     * object's len and its own serial. Region and object serials share one
     * counter (next_serial). */
    OSC_RT_EV_REGION_OPEN = 3,
    OSC_RT_EV_ARENA_ALLOC = 4,
    OSC_RT_EV_REGION_DESTROY = 5
} OscRtEventKind;

typedef struct {
    uint8_t kind;      /* OscRtEventKind */
    uint8_t slot;
    uint16_t len;      /* elements */
    uint32_t serial;   /* allocation serial number (object identity, 1-based) */
} OscRtEvent;

typedef struct OscRt OscRt;
struct OscRt {
    /* Fixed-offset fields read by native code. Do not reorder. */
    uint64_t (*alloc)(OscRt *rt, uint64_t len, uint64_t init);  /* offset 0  */
    void     (*release)(OscRt *rt, uint64_t addr, uint64_t len); /* offset 8  */
    void     (*trap)(OscRt *rt, uint64_t code);                  /* offset 16 */
    /* OSC-2 arenas (appended; native code reaches them at these offsets). */
    uint64_t (*arena_open)(OscRt *rt, uint64_t cap);                          /* offset 24 */
    uint64_t (*arena_alloc)(OscRt *rt, uint64_t h, uint64_t len, uint64_t init); /* offset 32 */
    void     (*arena_destroy)(OscRt *rt, uint64_t h);                         /* offset 40 */
    /* Host-side state. */
    uint64_t cells[OSC_RT_SLOTS][OSC_MAX_ARRAY_LEN];
    uint8_t  live[OSC_RT_SLOTS];
    uint16_t slot_len[OSC_RT_SLOTS];
    uint32_t slot_serial[OSC_RT_SLOTS];
    uint32_t next_serial;
    uint32_t live_count;
    OscRtEvent ev[OSC_RT_EVENTS];
    uint32_t nev;
    uint32_t trap_code;      /* OscTrap of the last run */
    jmp_buf jb;
    /* Appended (Lane 22 worker B): FNV-1a hash over ALL events, including
     * those beyond OSC_RT_EVENTS that ev[] cannot hold, so long runs still
     * compare their complete event history. */
    uint64_t ev_hash;
    /* OSC-2 arenas: slot_arena[s] = 1 while slot s holds an open arena (its
     * slot_len is the capacity K, slot_serial the region serial); arena_top[s]
     * is the bump pointer (cells handed out so far). */
    uint8_t  slot_arena[OSC_RT_SLOTS];
    uint16_t arena_top[OSC_RT_SLOTS];
};

void osc_rt_init(OscRt *rt);
/* Same semantics as the native entry points, callable from the interpreter. */
uint64_t osc_rt_alloc(OscRt *rt, uint64_t len, uint64_t init);
void osc_rt_release(OscRt *rt, uint64_t addr, uint64_t len);
void osc_rt_trap(OscRt *rt, uint64_t code);

/* ---- additions (Lane 22 worker B) ------------------------------------- */

/* Cheap reset between runs: clears every slot ever used, the metadata, the
 * event log and the trap code. Equivalent to osc_rt_init in effect. */
void osc_rt_reset(OscRt *rt);

/* Run natively compiled code: args in x0..x5 (missing ones 0), x6 = 0,
 * rt in x7. setjmps on rt->jb so osc_rt_trap returns here. Returns the trap
 * code (0 = normal return, *ret set), or -1 on bad arguments. */
int osc_rt_call_native(OscRt *rt, void *entry, const uint64_t *args, unsigned nargs, uint64_t *ret);

/* 1 if the observable pool state of a and b is identical: trap code, live
 * slots, lengths, serials, cell contents, full event log (count, recorded
 * events, hash). Addresses are not compared. */
int osc_rt_same_outcome(const OscRt *a, const OscRt *b);

/* 1 if addr is the start of a live slot (used to check REF arguments). */
int osc_rt_is_live_ref(const OscRt *rt, uint64_t addr, uint64_t len);

/* ---- OSC-2 arenas (docs/osc/OSC-2-DESIGN.md section 3) ----------------
 * An arena is one pool slot with capacity cap (1..OSC_MAX_ARRAY_LEN cells),
 * opened by osc_rt_arena_open, which returns its handle (the slot's address)
 * and logs REGION_OPEN. osc_rt_arena_alloc bump-allocates len cells (all set
 * to init) and logs ARENA_ALLOC; if the arena has fewer than len cells left it
 * traps OSC_TRAP_ARENA_FULL. Objects are never released one by one:
 * osc_rt_arena_destroy zeroes the slot, frees it and logs REGION_DESTROY. A bad
 * handle or cap traps OSC_TRAP_RUNTIME (never happens for checked code); no
 * free slot for a new arena traps OSC_TRAP_OOM. */
uint64_t osc_rt_arena_open(OscRt *rt, uint64_t cap);
uint64_t osc_rt_arena_alloc(OscRt *rt, uint64_t h, uint64_t len, uint64_t init);
void osc_rt_arena_destroy(OscRt *rt, uint64_t h);

#endif /* OSC_RT_H */
