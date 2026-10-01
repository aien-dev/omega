/*
 * world_model.c -- the lifecycle model (see world_model.h).
 *
 * Every AS_BUILT rule cites the rx_world.c function it mirrors, so a change
 * there that the model does not follow shows up as a differential mismatch.
 */
#include "world_model.h"

#include <stdio.h>
#include <string.h>

const int      wm_parent[WM_N]   = { -1, WM_P, WM_P, WM_C1 };
const uint32_t wm_mem[WM_N]      = { 4, 4, 2, 2 };
const uint32_t wm_deadline[WM_N] = { 1, 2, 0, 1 };
const char    *wm_name[WM_N]     = { "P", "C1", "C2", "G" };

const WmProfile wm_spec       = { "SPEC",          CANCEL_TRANSITIVE,  1, 0, 0 };
const WmProfile wm_as_built   = { "AS_BUILT",      CANCEL_NONE,        0, 0, 0 };
const WmProfile wm_mut_leak   = { "MUT_LEAK_ON_FAIL",       CANCEL_TRANSITIVE, 1, 1, 0 };
const WmProfile wm_mut_noprop = { "MUT_CANCEL_NO_PROPAGATE", CANCEL_TARGET_ONLY, 1, 0, 0 };
const WmProfile wm_mut_double = { "MUT_COMMIT_KEEPS_ACTIVATION", CANCEL_TRANSITIVE, 1, 0, 1 };
const WmProfile wm_mut_deadline = { "MUT_DEADLINE_ADMIT_ONLY", CANCEL_TRANSITIVE, 0, 0, 0 };

int wm_is_descendant(uint32_t d, uint32_t r) {
    for (int a = wm_parent[d]; a >= 0; a = wm_parent[a])
        if ((uint32_t)a == r) return 1;
    return 0;
}

void wm_init(WmState *s) { memset(s, 0, sizeof *s); }

/* res_fits */
static int fits(const WmState *s, uint32_t r) {
    if (s->used_slots >= WM_SLOTS) return 0;
    if (s->used_mem > WM_MEM) return 0;
    if (wm_mem[r] > WM_MEM - s->used_mem) return 0;
    return 1;
}

/* charge + enqueue; the harness waits until the worker is inside fn, so
 * admitted == running here. */
static void admit(WmState *s, uint32_t r) {
    s->st[r] = WM_RUN;
    s->used_slots++;
    s->used_mem += wm_mem[r];
    s->in_flight++;
    s->stale[r] = 0;
    s->act_committed[r] = 0;
    s->surfaced[r] = 0;
    if (wm_deadline[r] && s->tick > wm_deadline[r]) {   /* charge(): counted */
        s->overdue++;
        s->surfaced[r] = 1;
    }
}

/* try_admit / pick_admit / best_waiting: one priority class, oldest
 * fitting waiter first, until nothing fits. */
static void try_admit(WmState *s) {
    for (;;) {
        int best = -1;
        for (uint32_t r = 0; r < WM_N; r++) {
            if (s->st[r] != WM_WAITING || !fits(s, r)) continue;
            if (best < 0 || s->wait_seq[r] < s->wait_seq[best]) best = (int)r;
        }
        if (best < 0) return;
        admit(s, (uint32_t)best);
    }
}

/* demand_inner (stability limits off, no fan-out limit). */
static void demand(WmState *s, uint32_t r, int external) {
    if (external) s->yield[r] = 0;
    s->parked[r] = 0;
    switch (s->st[r]) {
    case WM_IDLE:
        s->st[r] = WM_WAITING;
        s->wait_seq[r] = ++s->seq;
        try_admit(s);
        break;
    case WM_WAITING:
        break;                          /* coalesced */
    default:
        s->rearm[r] = 1;                /* woken while running */
        break;
    }
}

/* others_busy */
static int others_busy(const WmState *s, uint32_t self) {
    for (uint32_t i = 0; i < WM_N; i++)
        if (i != self && (s->st[i] == WM_WAITING || s->st[i] == WM_RUN)) return 1;
    return 0;
}

/* release_parked */
static void release_parked(WmState *s) {
    for (uint32_t i = 0; i < WM_N; i++) {
        if (!s->parked[i]) continue;
        if (s->yield[i] && others_busy(s, i)) continue;
        s->parked[i] = 0;
        s->yield[i] = 0;
        demand(s, i, i == WM_P);
    }
}

/* arm_backoff */
static uint8_t backoff(uint8_t y) {
    uint32_t n = y ? y * 2u : 1u;
    if (n > 8u) n = 8u;
    return (uint8_t)n;
}

static void uncharge(WmState *s, uint32_t r) {
    if (s->used_slots) s->used_slots--;
    s->used_mem = s->used_mem >= wm_mem[r] ? s->used_mem - wm_mem[r] : 0;
}

/* end_activation_inner */
static void end_activation(const WmProfile *p, WmState *s, uint32_t r, int failed) {
    for (uint32_t i = 0; i < WM_N; i++)             /* tick_parked */
        if (s->parked[i] && s->yield[i]) s->yield[i]--;
    release_parked(s);
    if (!(failed && p->mut_leak_on_fail)) uncharge(s, r);
    s->st[r] = WM_IDLE;
    if (s->in_flight) s->in_flight--;
    if (s->rearm[r] && s->yield[r] && others_busy(s, r)) {
        s->yield[r]--;
        s->parked[r] = 1;
        s->rearm[r] = 0;
        try_admit(s);
    } else if (s->rearm[r]) {
        s->rearm[r] = 0;
        demand(s, r, r == WM_P);
    } else {
        try_admit(s);
    }
    if (s->in_flight == 0) release_parked(s);
}

/* A write to the object r's subscribers trigger on (finish_writes ->
 * propagate): running subscribers now hold a stale snapshot. */
static void write_and_wake(WmState *s, int writer) {
    uint32_t kids[WM_N], nk = 0;
    for (uint32_t c = 0; c < WM_N; c++)
        if (wm_parent[c] == writer) kids[nk++] = c;
    for (uint32_t i = 0; i < nk; i++)
        if (s->st[kids[i]] == WM_RUN) s->stale[kids[i]] = 1;
    for (uint32_t i = 0; i < nk; i++) demand(s, kids[i], writer < 0);
}

static void cancel(const WmProfile *p, WmState *s, uint32_t r) {
    if (p->cancel_mode == CANCEL_NONE) return;       /* no entry point exists */
    for (uint32_t x = 0; x < WM_N; x++) {
        if (x != r && !(p->cancel_mode == CANCEL_TRANSITIVE && wm_is_descendant(x, r)))
            continue;
        if (s->st[x] == WM_RUN) {
            uncharge(s, x);
            if (s->in_flight) s->in_flight--;
        }
        s->st[x] = WM_IDLE;
        s->rearm[x] = s->parked[x] = s->yield[x] = s->stale[x] = 0;
        s->cancels++;
    }
    try_admit(s);
    if (s->in_flight == 0) release_parked(s);
}

int wm_enabled(const WmProfile *p, const WmState *s, WmOp op) {
    (void)p;
    switch (op.kind) {
    case OP_EXT:    return s->ext < WM_EXT_MAX;
    case OP_FIN:
    case OP_FAIL:   return op.r < WM_N && s->st[op.r] == WM_RUN;
    case OP_TICK:   return s->tick < WM_TICK_MAX;
    case OP_CANCEL: return op.r == WM_P || op.r == WM_C1;
    default:        return 0;
    }
}

static uint32_t check(const WmState *s) {
    uint32_t v = 0, n_run = 0, mem = 0;
    for (uint32_t r = 0; r < WM_N; r++)
        if (s->st[r] == WM_RUN) { n_run++; mem += wm_mem[r]; }
    if (s->used_slots != n_run || s->used_mem != mem ||
        s->used_slots > WM_SLOTS || s->used_mem > WM_MEM)
        v |= V_LEAK;
    for (uint32_t r = 0; r < WM_N; r++)
        if (s->st[r] == WM_RUN && wm_deadline[r] && s->tick > wm_deadline[r] && !s->surfaced[r])
            v |= V_DEADLINE;
    return v;
}

uint32_t wm_step(const WmProfile *p, WmState *s, WmOp op) {
    s->viol = 0;
    uint32_t r = op.r;
    switch (op.kind) {
    case OP_EXT:
        s->ext++;
        write_and_wake(s, -1);
        break;
    case OP_TICK:
        s->tick++;
        if (p->deadline_on_tick)
            for (uint32_t i = 0; i < WM_N; i++)
                if (s->st[i] == WM_RUN && wm_deadline[i] && s->tick > wm_deadline[i] &&
                    !s->surfaced[i]) {
                    s->surfaced[i] = 1;
                    s->overdue++;
                }
        try_admit(s);                   /* rx_world_set_resources */
        break;
    case OP_FAIL:                       /* fn < 0: FAILED, no back-off */
        s->failures++;
        s->st[r] = WM_ENDING;
        end_activation(p, s, r, 1);
        break;
    case OP_FIN:
        if (s->stale[r]) {              /* PUBLISHING -> INVALIDATED */
            s->invalidations++;
            s->yield[r] = backoff(s->yield[r]);
            s->rearm[r] = 1;
            s->st[r] = WM_ENDING;
            end_activation(p, s, r, 0);
            break;
        }
        if (s->act_committed[r]) s->viol |= V_DOUBLE;
        s->act_committed[r] = 1;
        s->commits[r]++;
        s->yield[r] = 0;                /* note_value: progress, no flip */
        s->st[r] = WM_ENDING;
        write_and_wake(s, (int)r);
        if (p->mut_commit_keeps_act && !s->rearm[r]) {
            s->st[r] = WM_RUN;          /* activation never retired */
            break;
        }
        end_activation(p, s, r, 0);
        break;
    case OP_CANCEL:
        cancel(p, s, r);
        if (s->st[r] != WM_IDLE || s->rearm[r] || s->parked[r]) s->viol |= V_CANCEL_SELF;
        for (uint32_t x = 0; x < WM_N; x++)
            if (wm_is_descendant(x, r) &&
                (s->st[x] != WM_IDLE || s->rearm[x] || s->parked[x]))
                s->viol |= V_CANCEL_CHILD;
        break;
    }
    s->viol |= check(s);
    return s->viol;
}

int wm_key(const WmState *s, uint8_t *out) {
    int n = 0;
    for (uint32_t r = 0; r < WM_N; r++) {
        uint32_t rank = 0;
        if (s->st[r] == WM_WAITING)
            for (uint32_t q = 0; q < WM_N; q++)
                if (s->st[q] == WM_WAITING && s->wait_seq[q] <= s->wait_seq[r]) rank++;
        out[n++] = s->st[r];
        out[n++] = s->rearm[r];
        out[n++] = s->stale[r];
        out[n++] = s->parked[r];
        out[n++] = s->yield[r];
        out[n++] = s->surfaced[r];
        out[n++] = s->act_committed[r];
        out[n++] = (uint8_t)rank;
    }
    out[n++] = s->ext;
    out[n++] = s->tick;
    out[n++] = s->used_slots;
    out[n++] = (uint8_t)s->used_mem;
    out[n++] = s->in_flight;
    return n;
}

void wm_op_str(WmOp op, char *buf, int n) {
    static const char *k[] = { "EXT", "FIN", "FAIL", "TICK", "CANCEL" };
    if (op.kind == OP_EXT || op.kind == OP_TICK) snprintf(buf, (size_t)n, "%s", k[op.kind]);
    else snprintf(buf, (size_t)n, "%s(%s)", k[op.kind], wm_name[op.r]);
}

int wm_all_ops(WmOp *out) {
    int n = 0;
    out[n++] = (WmOp){ OP_EXT, 0 };
    out[n++] = (WmOp){ OP_TICK, 0 };
    for (uint8_t r = 0; r < WM_N; r++) out[n++] = (WmOp){ OP_FIN, r };
    for (uint8_t r = 0; r < WM_N; r++) out[n++] = (WmOp){ OP_FAIL, r };
    out[n++] = (WmOp){ OP_CANCEL, WM_P };
    out[n++] = (WmOp){ OP_CANCEL, WM_C1 };
    return n;
}
