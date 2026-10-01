/*
 * osc_rt.c -- OSC-1 bootstrap runtime (OSC-0 II.10), shared by the reference
 * interpreter and natively compiled code. See osc_rt.h.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#include "osc_rt.h"

#include <stddef.h>
#include <string.h>

/* Native code reads these at fixed offsets (docs/osc/OSC-1-DESIGN.md s.7). */
_Static_assert(offsetof(OscRt, alloc) == 0, "OscRt.alloc must be at offset 0");
_Static_assert(offsetof(OscRt, release) == 8, "OscRt.release must be at offset 8");
_Static_assert(offsetof(OscRt, trap) == 16, "OscRt.trap must be at offset 16");
_Static_assert(offsetof(OscRt, arena_open) == 24, "OscRt.arena_open must be at offset 24");
_Static_assert(offsetof(OscRt, arena_alloc) == 32, "OscRt.arena_alloc must be at offset 32");
_Static_assert(offsetof(OscRt, arena_destroy) == 40, "OscRt.arena_destroy must be at offset 40");
_Static_assert(offsetof(OscRt, pool_open) == 48, "OscRt.pool_open must be at offset 48");
_Static_assert(offsetof(OscRt, pool_close) == 56, "OscRt.pool_close must be at offset 56");
_Static_assert(offsetof(OscRt, h_alloc) == 64, "OscRt.h_alloc must be at offset 64");
_Static_assert(offsetof(OscRt, h_gen) == 72, "OscRt.h_gen must be at offset 72");
_Static_assert(offsetof(OscRt, h_free) == 80, "OscRt.h_free must be at offset 80");
_Static_assert(offsetof(OscRt, h_load) == 88, "OscRt.h_load must be at offset 88");
_Static_assert(offsetof(OscRt, h_store) == 96, "OscRt.h_store must be at offset 96");
_Static_assert(sizeof(void (*)(void)) == 8, "64-bit function pointers");

#define FNV_OFF 1469598103934665603ULL
#define FNV_PRIME 1099511628211ULL

static void set_pool_fns(OscRt *rt) {
    rt->pool_open = osc_rt_pool_open;
    rt->pool_close = osc_rt_pool_close;
    rt->h_alloc = osc_rt_h_alloc;
    rt->h_gen = osc_rt_h_gen;
    rt->h_free = osc_rt_h_free;
    rt->h_load = osc_rt_h_load;
    rt->h_store = osc_rt_h_store;
}

void osc_rt_init(OscRt *rt) {
    memset(rt, 0, sizeof *rt);
    rt->alloc = osc_rt_alloc;
    rt->release = osc_rt_release;
    rt->trap = osc_rt_trap;
    rt->arena_open = osc_rt_arena_open;
    rt->arena_alloc = osc_rt_arena_alloc;
    rt->arena_destroy = osc_rt_arena_destroy;
    rt->ev_hash = FNV_OFF;
    set_pool_fns(rt);
}

void osc_rt_reset(OscRt *rt) {
    for (unsigned s = 0; s < OSC_RT_SLOTS; s++)
        if (rt->slot_serial[s] || rt->live[s]) memset(rt->cells[s], 0, sizeof rt->cells[s]);
    memset(rt->live, 0, sizeof rt->live);
    memset(rt->slot_len, 0, sizeof rt->slot_len);
    memset(rt->slot_serial, 0, sizeof rt->slot_serial);
    memset(rt->slot_arena, 0, sizeof rt->slot_arena);
    memset(rt->arena_top, 0, sizeof rt->arena_top);
    rt->next_serial = 0;
    rt->live_count = 0;
    rt->nev = 0;
    rt->trap_code = 0;
    rt->ev_hash = FNV_OFF;
    rt->alloc = osc_rt_alloc;
    rt->release = osc_rt_release;
    rt->trap = osc_rt_trap;
    rt->arena_open = osc_rt_arena_open;
    rt->arena_alloc = osc_rt_arena_alloc;
    rt->arena_destroy = osc_rt_arena_destroy;
    memset(rt->pools, 0, sizeof rt->pools);
    rt->pool_serial = 0;
    rt->trap_op = rt->trap_pool = rt->trap_slot = rt->trap_rights = 0;
    rt->trap_gen = 0;
    set_pool_fns(rt);
}

static void log_event_g(OscRt *rt, uint8_t kind, unsigned slot, uint16_t len, uint32_t serial, uint64_t gen) {
    OscRtEvent e = {kind, (uint8_t)slot, len, serial};
    if (rt->nev < OSC_RT_EVENTS) { rt->ev[rt->nev] = e; rt->ev_gen[rt->nev] = gen; }
    if (rt->nev != UINT32_MAX) rt->nev++;
    uint8_t b[8] = {kind, (uint8_t)slot, (uint8_t)len, (uint8_t)(len >> 8),
                    (uint8_t)serial, (uint8_t)(serial >> 8), (uint8_t)(serial >> 16), (uint8_t)(serial >> 24)};
    for (unsigned i = 0; i < 8; i++) { rt->ev_hash ^= b[i]; rt->ev_hash *= FNV_PRIME; }
    /* OSC-3 item 2: pool events also hash their u64 generation (older event
     * kinds hash exactly as before) */
    if (kind >= OSC_RT_EV_POOL_OPEN)
        for (unsigned i = 0; i < 8; i++) { rt->ev_hash ^= (uint8_t)(gen >> (8 * i)); rt->ev_hash *= FNV_PRIME; }
}

static void log_event(OscRt *rt, uint8_t kind, unsigned slot, uint16_t len, uint32_t serial) {
    log_event_g(rt, kind, slot, len, serial, 0);
}

void osc_rt_trap(OscRt *rt, uint64_t code) {
    rt->trap_code = (code >= 1 && code <= OSC_TRAP_MAX) ? (uint32_t)code : OSC_TRAP_RUNTIME;
    longjmp(rt->jb, 1);
}

uint64_t osc_rt_alloc(OscRt *rt, uint64_t len, uint64_t init) {
    if (len < 1 || len > OSC_MAX_ARRAY_LEN) osc_rt_trap(rt, OSC_TRAP_RUNTIME);
    if (rt->next_serial == UINT32_MAX) osc_rt_trap(rt, OSC_TRAP_RUNTIME); /* serial exhausted: never wrap */
    for (unsigned s = 0; s < OSC_RT_SLOTS; s++) {
        if (rt->live[s]) continue;
        for (unsigned i = 0; i < len; i++) rt->cells[s][i] = init;
        rt->live[s] = 1;
        rt->slot_len[s] = (uint16_t)len;
        rt->slot_serial[s] = ++rt->next_serial;
        rt->live_count++;
        log_event(rt, OSC_RT_EV_ALLOC, s, (uint16_t)len, rt->slot_serial[s]);
        return (uint64_t)(uintptr_t)&rt->cells[s][0];
    }
    osc_rt_trap(rt, OSC_TRAP_OOM);
    return 0; /* not reached */
}

/* slot index of a live slot start, or -1 */
static int slot_of(const OscRt *rt, uint64_t addr, uint64_t len) {
    uintptr_t base = (uintptr_t)&rt->cells[0][0];
    uintptr_t a = (uintptr_t)addr;
    size_t stride = sizeof rt->cells[0];
    if (a < base || a >= base + sizeof rt->cells) return -1;
    if ((a - base) % stride) return -1;
    unsigned s = (unsigned)((a - base) / stride);
    if (!rt->live[s] || rt->slot_len[s] != len) return -1;
    return (int)s;
}

int osc_rt_is_live_ref(const OscRt *rt, uint64_t addr, uint64_t len) {
    return slot_of(rt, addr, len) >= 0;
}

void osc_rt_release(OscRt *rt, uint64_t addr, uint64_t len) {
    int s = slot_of(rt, addr, len);
    if (s < 0 || rt->slot_arena[s]) osc_rt_trap(rt, OSC_TRAP_RUNTIME);
    memset(rt->cells[s], 0, sizeof rt->cells[s]);
    rt->live[s] = 0;
    rt->live_count--;
    log_event(rt, OSC_RT_EV_RELEASE, (unsigned)s, (uint16_t)len, rt->slot_serial[s]);
    /* slot_len / slot_serial are kept: they describe the last occupant and let
     * osc_rt_reset find every slot that was ever used. */
}

/* ---- OSC-2 arenas ----------------------------------------------------- */

uint64_t osc_rt_arena_open(OscRt *rt, uint64_t cap) {
    if (cap < 1 || cap > OSC_MAX_ARRAY_LEN) osc_rt_trap(rt, OSC_TRAP_RUNTIME);
    if (rt->next_serial == UINT32_MAX) osc_rt_trap(rt, OSC_TRAP_RUNTIME); /* serial exhausted: never wrap */
    for (unsigned s = 0; s < OSC_RT_SLOTS; s++) {
        if (rt->live[s]) continue;
        memset(rt->cells[s], 0, sizeof rt->cells[s]);
        rt->live[s] = 1;
        rt->slot_arena[s] = 1;
        rt->arena_top[s] = 0;
        rt->slot_len[s] = (uint16_t)cap;
        rt->slot_serial[s] = ++rt->next_serial;
        rt->live_count++;
        log_event(rt, OSC_RT_EV_REGION_OPEN, s, (uint16_t)cap, rt->slot_serial[s]);
        return (uint64_t)(uintptr_t)&rt->cells[s][0];
    }
    osc_rt_trap(rt, OSC_TRAP_OOM);
    return 0; /* not reached */
}

/* slot index of an open arena whose handle is h, or -1 */
static int arena_of(const OscRt *rt, uint64_t h) {
    uintptr_t base = (uintptr_t)&rt->cells[0][0];
    uintptr_t a = (uintptr_t)h;
    size_t stride = sizeof rt->cells[0];
    if (a < base || a >= base + sizeof rt->cells || (a - base) % stride) return -1;
    unsigned s = (unsigned)((a - base) / stride);
    return rt->live[s] && rt->slot_arena[s] ? (int)s : -1;
}

uint64_t osc_rt_arena_alloc(OscRt *rt, uint64_t h, uint64_t len, uint64_t init) {
    int s = arena_of(rt, h);
    if (s < 0 || len < 1 || len > OSC_MAX_ARRAY_LEN) osc_rt_trap(rt, OSC_TRAP_RUNTIME);
    unsigned top = rt->arena_top[s];
    if (len > (uint64_t)(rt->slot_len[s] - top)) osc_rt_trap(rt, OSC_TRAP_ARENA_FULL);
    if (rt->next_serial == UINT32_MAX) osc_rt_trap(rt, OSC_TRAP_RUNTIME); /* serial exhausted: never wrap */
    for (unsigned i = 0; i < len; i++) rt->cells[s][top + i] = init;
    rt->arena_top[s] = (uint16_t)(top + len);
    log_event(rt, OSC_RT_EV_ARENA_ALLOC, (unsigned)s, (uint16_t)len, ++rt->next_serial);
    return (uint64_t)(uintptr_t)&rt->cells[s][top];
}

void osc_rt_arena_destroy(OscRt *rt, uint64_t h) {
    int s = arena_of(rt, h);
    if (s < 0) osc_rt_trap(rt, OSC_TRAP_RUNTIME);
    memset(rt->cells[s], 0, sizeof rt->cells[s]);
    rt->live[s] = 0;
    rt->slot_arena[s] = 0;
    rt->arena_top[s] = 0;
    rt->live_count--;
    log_event(rt, OSC_RT_EV_REGION_DESTROY, (unsigned)s, rt->slot_len[s], rt->slot_serial[s]);
}

/* ---- OSC-3 item 2 versioned handles ----------------------------------- */

/* the open pool with id p (index + 1), else trap RUNTIME */
static OscRtPool *pool_of(OscRt *rt, uint64_t p) {
    if (p < 1 || p > OSC_RT_POOLS || !rt->pools[p - 1].open) osc_rt_trap(rt, OSC_TRAP_RUNTIME);
    return &rt->pools[p - 1];
}

static void pool_trap(OscRt *rt, uint64_t p, int code, int op, uint64_t slot, uint64_t gen, int rights) {
    rt->trap_op = (uint8_t)op;
    rt->trap_pool = (uint8_t)(p - 1);
    rt->trap_slot = (uint8_t)slot;
    rt->trap_gen = gen;
    rt->trap_rights = (uint8_t)rights;
    osc_rt_trap(rt, (uint64_t)code);
}

uint64_t osc_rt_pool_open(OscRt *rt, uint64_t k, uint64_t base) {
    if (k < 1 || k > OSC_POOL_SLOTS) osc_rt_trap(rt, OSC_TRAP_RUNTIME);
    if (rt->pool_serial == UINT32_MAX) osc_rt_trap(rt, OSC_TRAP_RUNTIME); /* serial exhausted: never wrap */
    for (unsigned i = 0; i < OSC_RT_POOLS; i++) {
        OscRtPool *q = &rt->pools[i];
        if (q->open) continue;
        memset(q, 0, sizeof *q);
        q->open = 1;
        q->k = (uint8_t)k;
        q->base = base;
        q->serial = ++rt->pool_serial;
        for (unsigned s = 0; s < k; s++) q->gen[s] = base;
        log_event_g(rt, OSC_RT_EV_POOL_OPEN, i, (uint16_t)k, q->serial, base);
        return i + 1;
    }
    osc_rt_trap(rt, OSC_TRAP_OOM);
    return 0; /* not reached */
}

void osc_rt_pool_close(OscRt *rt, uint64_t p) {
    OscRtPool *q = pool_of(rt, p);
    log_event_g(rt, OSC_RT_EV_POOL_CLOSE, (unsigned)(p - 1), q->k, q->serial, q->base);
    q->open = 0;
    memset(q->state, 0, sizeof q->state);
    memset(q->val, 0, sizeof q->val);
}

uint64_t osc_rt_h_alloc(OscRt *rt, uint64_t p, uint64_t init) {
    OscRtPool *q = pool_of(rt, p);
    int retired = -1;
    for (unsigned s = 0; s < q->k; s++) {
        if (q->state[s] == OSC_RT_SLOT_RETIRED && retired < 0) retired = (int)s;
        if (q->state[s] != OSC_RT_SLOT_FREE) continue;
        q->state[s] = OSC_RT_SLOT_LIVE;
        q->val[s] = init;
        log_event_g(rt, OSC_RT_EV_SLOT_ALLOC, (unsigned)(p - 1), (uint16_t)s, q->serial, q->gen[s]);
        return s;
    }
    if (retired >= 0) pool_trap(rt, p, OSC_TRAP_RETIRED, 3, (uint64_t)retired, q->gen[retired], 0);
    pool_trap(rt, p, OSC_TRAP_POOL_FULL, 3, 0, 0, 0);
    return 0; /* not reached */
}

uint64_t osc_rt_h_gen(OscRt *rt, uint64_t p, uint64_t slot) {
    OscRtPool *q = pool_of(rt, p);
    if (slot >= q->k) osc_rt_trap(rt, OSC_TRAP_RUNTIME);
    return q->gen[slot];
}

/* the pool of a handle access; traps STALE unless (slot, gen) is live */
static OscRtPool *checked(OscRt *rt, uint64_t p, uint64_t slot, uint64_t gen, int op, int rights) {
    OscRtPool *q = pool_of(rt, p);
    if (slot >= q->k) osc_rt_trap(rt, OSC_TRAP_RUNTIME);
    if (q->state[slot] != OSC_RT_SLOT_LIVE || q->gen[slot] != gen)
        pool_trap(rt, p, OSC_TRAP_STALE, op, slot, gen, rights);
    return q;
}

void osc_rt_h_free(OscRt *rt, uint64_t p, uint64_t slot, uint64_t gen) {
    OscRtPool *q = checked(rt, p, slot, gen, 2, 0);
    log_event_g(rt, OSC_RT_EV_SLOT_FREE, (unsigned)(p - 1), (uint16_t)slot, q->serial, gen);
    q->val[slot] = 0;
    if (gen == UINT64_MAX) {
        q->state[slot] = OSC_RT_SLOT_RETIRED; /* never wrapped */
    } else {
        q->state[slot] = OSC_RT_SLOT_FREE;
        q->gen[slot] = gen + 1;
    }
}

uint64_t osc_rt_h_load(OscRt *rt, uint64_t p, uint64_t slot, uint64_t gen) {
    OscRtPool *q = checked(rt, p, slot, gen, 1, 1);
    log_event_g(rt, OSC_RT_EV_HANDLE_USE, (unsigned)(p - 1), (uint16_t)(slot | 1u << 8), q->serial, gen);
    return q->val[slot];
}

void osc_rt_h_store(OscRt *rt, uint64_t p, uint64_t slot, uint64_t gen, uint64_t v) {
    OscRtPool *q = checked(rt, p, slot, gen, 1, 2);
    log_event_g(rt, OSC_RT_EV_HANDLE_USE, (unsigned)(p - 1), (uint16_t)(slot | 2u << 8), q->serial, gen);
    q->val[slot] = v;
}

typedef uint64_t (*OscNativeFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t, OscRt *);

int osc_rt_call_native(OscRt *rt, void *entry, const uint64_t *args, unsigned nargs, uint64_t *ret) {
    if (!rt || !entry || !ret || nargs > OSC_MAX_PARAMS || (nargs && !args)) return -1;
    uint64_t a[OSC_MAX_PARAMS] = {0};
    for (unsigned i = 0; i < nargs; i++) a[i] = args[i];
    union { void *p; OscNativeFn f; } u;
    u.p = entry;
    rt->trap_code = 0;
    if (setjmp(rt->jb) != 0) return (int)rt->trap_code;
    uint64_t r = u.f(a[0], a[1], a[2], a[3], a[4], a[5], 0, rt);
    *ret = r;
    return 0;
}

int osc_rt_same_outcome(const OscRt *a, const OscRt *b) {
    if (a->trap_code != b->trap_code || a->nev != b->nev || a->ev_hash != b->ev_hash) return 0;
    if (a->next_serial != b->next_serial || a->live_count != b->live_count) return 0;
    if (memcmp(a->live, b->live, sizeof a->live) || memcmp(a->slot_len, b->slot_len, sizeof a->slot_len) ||
        memcmp(a->slot_serial, b->slot_serial, sizeof a->slot_serial)) return 0;
    if (memcmp(a->slot_arena, b->slot_arena, sizeof a->slot_arena) ||
        memcmp(a->arena_top, b->arena_top, sizeof a->arena_top)) return 0;
    if (a->pool_serial != b->pool_serial || a->trap_op != b->trap_op || a->trap_pool != b->trap_pool ||
        a->trap_slot != b->trap_slot || a->trap_rights != b->trap_rights || a->trap_gen != b->trap_gen) return 0;
    for (unsigned i = 0; i < OSC_RT_POOLS; i++) {
        const OscRtPool *p = &a->pools[i], *q = &b->pools[i];
        if (p->open != q->open || p->k != q->k || p->serial != q->serial || p->base != q->base ||
            memcmp(p->state, q->state, sizeof p->state) || memcmp(p->gen, q->gen, sizeof p->gen) ||
            memcmp(p->val, q->val, sizeof p->val)) return 0;
    }
    uint32_t n = a->nev < OSC_RT_EVENTS ? a->nev : OSC_RT_EVENTS;
    for (uint32_t i = 0; i < n; i++)
        if (a->ev[i].kind != b->ev[i].kind || a->ev[i].slot != b->ev[i].slot ||
            a->ev[i].len != b->ev[i].len || a->ev[i].serial != b->ev[i].serial ||
            (a->ev[i].kind >= OSC_RT_EV_POOL_OPEN && a->ev_gen[i] != b->ev_gen[i])) return 0;
    for (unsigned s = 0; s < OSC_RT_SLOTS; s++)
        if ((a->slot_serial[s] || a->live[s]) && memcmp(a->cells[s], b->cells[s], sizeof a->cells[s])) return 0;
    return 1;
}
