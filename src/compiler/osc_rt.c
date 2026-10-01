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
_Static_assert(sizeof(void (*)(void)) == 8, "64-bit function pointers");

#define FNV_OFF 1469598103934665603ULL
#define FNV_PRIME 1099511628211ULL

void osc_rt_init(OscRt *rt) {
    memset(rt, 0, sizeof *rt);
    rt->alloc = osc_rt_alloc;
    rt->release = osc_rt_release;
    rt->trap = osc_rt_trap;
    rt->ev_hash = FNV_OFF;
}

void osc_rt_reset(OscRt *rt) {
    for (unsigned s = 0; s < OSC_RT_SLOTS; s++)
        if (rt->slot_serial[s] || rt->live[s]) memset(rt->cells[s], 0, sizeof rt->cells[s]);
    memset(rt->live, 0, sizeof rt->live);
    memset(rt->slot_len, 0, sizeof rt->slot_len);
    memset(rt->slot_serial, 0, sizeof rt->slot_serial);
    rt->next_serial = 0;
    rt->live_count = 0;
    rt->nev = 0;
    rt->trap_code = 0;
    rt->ev_hash = FNV_OFF;
    rt->alloc = osc_rt_alloc;
    rt->release = osc_rt_release;
    rt->trap = osc_rt_trap;
}

static void log_event(OscRt *rt, uint8_t kind, unsigned slot, uint16_t len, uint32_t serial) {
    OscRtEvent e = {kind, (uint8_t)slot, len, serial};
    if (rt->nev < OSC_RT_EVENTS) rt->ev[rt->nev] = e;
    if (rt->nev != UINT32_MAX) rt->nev++;
    uint8_t b[8] = {kind, (uint8_t)slot, (uint8_t)len, (uint8_t)(len >> 8),
                    (uint8_t)serial, (uint8_t)(serial >> 8), (uint8_t)(serial >> 16), (uint8_t)(serial >> 24)};
    for (unsigned i = 0; i < 8; i++) { rt->ev_hash ^= b[i]; rt->ev_hash *= FNV_PRIME; }
}

void osc_rt_trap(OscRt *rt, uint64_t code) {
    rt->trap_code = (code >= 1 && code <= OSC_TRAP_RUNTIME) ? (uint32_t)code : OSC_TRAP_RUNTIME;
    longjmp(rt->jb, 1);
}

uint64_t osc_rt_alloc(OscRt *rt, uint64_t len, uint64_t init) {
    if (len < 1 || len > OSC_MAX_ARRAY_LEN) osc_rt_trap(rt, OSC_TRAP_RUNTIME);
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
    if (s < 0) osc_rt_trap(rt, OSC_TRAP_RUNTIME);
    memset(rt->cells[s], 0, sizeof rt->cells[s]);
    rt->live[s] = 0;
    rt->live_count--;
    log_event(rt, OSC_RT_EV_RELEASE, (unsigned)s, (uint16_t)len, rt->slot_serial[s]);
    /* slot_len / slot_serial are kept: they describe the last occupant and let
     * osc_rt_reset find every slot that was ever used. */
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
    uint32_t n = a->nev < OSC_RT_EVENTS ? a->nev : OSC_RT_EVENTS;
    for (uint32_t i = 0; i < n; i++)
        if (a->ev[i].kind != b->ev[i].kind || a->ev[i].slot != b->ev[i].slot ||
            a->ev[i].len != b->ev[i].len || a->ev[i].serial != b->ev[i].serial) return 0;
    for (unsigned s = 0; s < OSC_RT_SLOTS; s++)
        if ((a->slot_serial[s] || a->live[s]) && memcmp(a->cells[s], b->cells[s], sizeof a->cells[s])) return 0;
    return 1;
}
