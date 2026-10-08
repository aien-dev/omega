/*
 * test_osc_compiler.c -- OSC-1 front-end gate (Lane 22 worker C).
 *
 *  1. Golden programs (tests/compiler/progs/NAME.osc, header `// entry: f g ...`):
 *     compile; run every entry natively (osc_cg + osc_native +
 *     osc_rt_call_native) and in the reference interpreter over N fuzzed
 *     argument vectors (seeded splitmix64, uniform + edge values per param
 *     type); require osc_rt_same_outcome and pool live_count 0 after every
 *     non-trapping run; every runtime trap kind 1..7 must be observed.
 *  2. Determinism: compile twice in-process, same IR digest and code bytes.
 *  3. Model replay: each golden ownership trace replays through the OSC-0
 *     model (osc_model_init(&m, 0, UINT64_MAX)) and is accepted; each
 *     non-trapping native run's pool event log replays as ALLOC/RELEASE and
 *     is accepted.
 *  4. Negative programs (tests/compiler/neg/NAME.osc, header
 *     `// expect: KIND object=NAME line=N` and optional
 *     `// transition: TEXT`): kind, object, line (and transition substring)
 *     match. For USE_AFTER_MOVE, MUTABLE_ALIAS, BORROW_OUTLIVES_OWNER and
 *     READ_ONLY_BORROW the refusal trace must replay with every event
 *     accepted except the last, which the model rejects with the mapped name.
 *
 * Usage: test_osc_compiler [fuzz_count (default 1000)] [tests dir (default tests/compiler)]
 * Final line: OSC1_COMPILER_PASS or OSC1_COMPILER_FAIL with counts.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "osc_cg.h"
#include "osc_front.h"
#include "osc_interp.h"
#include "osc_native.h"
#include "osc_rt.h"
#include "model/osc_model.h"

static unsigned long checks, failures;
static unsigned long trap_seen[OSC_TRAP_MAX + 1];
static unsigned long diff_runs, rt_replays, trace_replays;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        checks++;                                         \
        if (!(cond)) {                                    \
            failures++;                                   \
            if (failures <= 40) {                         \
                printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                      \
                printf("\n");                             \
            }                                             \
        }                                                 \
    } while (0)

/* ------------------------------------------------------------ files */
static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    char *b = malloc(cap + 1);
    while (b) {
        size_t r = fread(b + n, 1, cap - n, f);
        n += r;
        if (n < cap) break;
        cap *= 2;
        char *nb = realloc(b, cap + 1);
        if (!nb) { free(b); b = NULL; break; }
        b = nb;
    }
    fclose(f);
    if (b) { b[n] = 0; *len = n; }
    return b;
}

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int list_osc(const char *dir, char ***out)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0, cap = 64;
    char **v = malloc(sizeof(char *) * (size_t)cap);
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".osc")) continue;
        if (n == cap) { cap *= 2; v = realloc(v, sizeof(char *) * (size_t)cap); }
        v[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(v, (size_t)n, sizeof(char *), cmpstr);
    *out = v;
    return n;
}

/* value of `key` in a `// key ...` header line; copies the rest of the line */
static int header(const char *src, const char *key, char *buf, size_t cap)
{
    const char *p = src;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t l = eol ? (size_t)(eol - p) : strlen(p);
        size_t kl = strlen(key);
        if (l > 3 + kl && strncmp(p, "// ", 3) == 0 && strncmp(p + 3, key, kl) == 0) {
            const char *v = p + 3 + kl;
            size_t vl = l - 3 - kl;
            while (vl && (*v == ' ')) { v++; vl--; }
            while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\r')) vl--;
            if (vl >= cap) vl = cap - 1;
            memcpy(buf, v, vl);
            buf[vl] = 0;
            return 0;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return -1;
}

/* ------------------------------------------------------------ fuzz */
static uint64_t sm_state;
static uint64_t sm(void)
{
    uint64_t z = (sm_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint64_t canon(OscScalar t, uint64_t v)
{
    if (t == OSC_T_BOOL) return v & 1;
    unsigned w = osc_scalar_width(t);
    if (w == 64) return v;
    uint64_t m = (1ull << w) - 1;
    v &= m;
    if (osc_scalar_signed(t) && (v >> (w - 1))) v |= ~m;
    return v;
}
static uint64_t gen_arg(OscScalar t)
{
    unsigned w = osc_scalar_width(t);
    int sg = osc_scalar_signed(t);
    uint64_t r = sm();
    switch (r % 6) {
    case 0: {
        uint64_t mx = sg ? ((1ull << (w - 1)) - 1) : (w == 64 ? UINT64_MAX : (1ull << w) - 1);
        uint64_t mn = sg ? (uint64_t)(-(int64_t)(mx) - 1) : 0;
        uint64_t e[] = {0, 1, (uint64_t)-1, mx, mn, mx - 1, mn + 1, 2, (uint64_t)-2};
        return canon(t, e[(r >> 8) % 9]);
    }
    case 1: case 2: return canon(t, (uint64_t)((int64_t)((r >> 8) % 41) - 20));
    case 3: return canon(t, (r >> 8) % 300);
    case 4: return canon(t, (r >> 8) % 70);
    default: return canon(t, sm());
    }
}

/* ------------------------------------------------------------ model replays */
/* Replay one function's trace slice [i0,i1). Returns index of the first
 * rejected event (or -1 if all accepted); *rej gets the reason. */
#define SNAP_MAX 128
static int replay_trace(const OscTrace *t, uint32_t i0, uint32_t i1, OscModelReject *rej)
{
    static OscModel m, stack[SNAP_MAX];
    int sp = 0;
    osc_model_init(&m, 0, UINT64_MAX);
    *rej = OSC_REJ_NONE;
    for (uint32_t i = i0; i < i1; i++) {
        const OscTraceEntry *e = &t->e[i];
        if (e->op == OSC_TR_SAVE) {
            if (sp >= SNAP_MAX) { *rej = OSC_REJ_CAPACITY; return (int)i; }
            stack[sp++] = m;
        } else if (e->op == OSC_TR_RESTORE) {
            if (sp <= 0) { *rej = OSC_REJ_PROTOCOL; return (int)i; }
            m = stack[--sp];
        } else {
            OscModelReject r;
            if (osc_model_step(&m, &e->ev, &r) != OSC_MODEL_ACCEPT) { *rej = r; return (int)i; }
        }
    }
    return -1;
}

/* runtime pool log -> model events, windowed ids:
 *   ALLOC -> ALLOC(obj, region 0); RELEASE -> RELEASE(obj);
 *   REGION_OPEN -> REGION_OPEN(region); ARENA_ALLOC -> ALLOC(obj, region of
 *   that arena slot); REGION_DESTROY -> REGION_DESTROY(region), which releases
 *   the region's objects in the model.
 * When the next object id would exceed 64 (or region id 16), a new window
 * starts: a fresh model re-opens the open regions and re-allocates the live
 * objects (with their regions) under fresh ids. Every event of the log is
 * replayed; a trapping run's log is a prefix and replays the same way.
 * Returns 0 (all accepted) or -1 (why says which event and reason). */
/* OSC-3 item 2: runtime pool events -> OSC-0B slot/handle model events. One
 * model per open pool (osc_model_init(gen_base = the declared base,
 * gen_max = UINT64_MAX)); model slot = pool slot + 1. Each SLOT_ALLOC mints a
 * fresh model handle (rights 3); HANDLE_USE and SLOT_FREE name the live
 * handle of the slot. Every logged generation must equal the model slot's
 * generation. Past 64 handles a fresh model is advanced to the same slot
 * generations (ALLOC with no handle + explicit SLOT_FREE per generation step)
 * and the live slots re-minted. A STALE / RETIRED / POOL_FULL trap is then
 * confirmed: the refused operation replays as a model rejection
 * (STALE_GENERATION / GENERATION_WRAP / every slot live). */
static unsigned long rp_events, rp_stale_confirmed, rp_wrap_confirmed, rp_full_confirmed, rp_windows;
typedef struct {
    OscModel m;
    uint64_t base;
    uint8_t k, open;
    uint32_t next;                             /* next model handle id */
    uint32_t hid[OSC_POOL_SLOTS];              /* live handle of each slot (0 = none) */
    uint32_t mslot[OSC_MODEL_MAX_HANDLES + 1]; /* minted handle -> pool slot + 1 */
    uint64_t mgen[OSC_MODEL_MAX_HANDLES + 1];  /* minted handle -> generation */
} RpPool;
static RpPool rp[OSC_RT_POOLS];

static int rp_step(OscModel *m, uint32_t kind, uint32_t slot, uint32_t handle, uint64_t gen, uint64_t rights,
                   OscModelReject *r)
{
    OscModelEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    ev.slot = slot;
    ev.handle = handle;
    ev.gen = gen;
    ev.rights = rights;
    return osc_model_step(m, &ev, r) == OSC_MODEL_ACCEPT ? 0 : -1;
}

/* fresh model whose slots match q's: free/live at the same generation, or retired */
static int rp_rewindow(RpPool *q, char *why, size_t n)
{
    OscModel old = q->m;
    rp_windows++;
    osc_model_init(&q->m, q->base, UINT64_MAX);
    q->next = 1;
    memset(q->mslot, 0, sizeof q->mslot);
    for (uint32_t s = 1; s <= q->k; s++) {
        const OscMSlot *o = &old.slot[s];
        for (uint64_t g = q->base; g != o->gen || o->state == 2 /* retired: also free gen_max */; g++) {
            if (rp_step(&q->m, OSC_EV_SLOT_ALLOC, s, 0, 0, 0, NULL) ||
                rp_step(&q->m, OSC_EV_SLOT_FREE, s, 0, g, 0, NULL)) { snprintf(why, n, "re-window advance"); return -1; }
            if (g == UINT64_MAX) break;
        }
        q->hid[s - 1] = 0;
        if (o->state == 1) { /* live: re-mint */
            uint32_t h = q->next++;
            if (rp_step(&q->m, OSC_EV_SLOT_ALLOC, s, h, 0, 3, NULL)) { snprintf(why, n, "re-window mint"); return -1; }
            q->hid[s - 1] = h;
            q->mslot[h] = s;
            q->mgen[h] = q->m.slot[s].gen;
        }
        if (q->m.slot[s].state != o->state || q->m.slot[s].gen != o->gen) { snprintf(why, n, "re-window state"); return -1; }
    }
    return 0;
}

static int rp_event(const OscRt *rt, uint32_t i, char *why, size_t n)
{
    const OscRtEvent *e = &rt->ev[i];
    uint64_t gen = rt->ev_gen[i];
    if (e->slot >= OSC_RT_POOLS) { snprintf(why, n, "pool event on bad pool %u", e->slot); return -1; }
    RpPool *q = &rp[e->slot];
    unsigned ps = e->len & 0xff, rights = e->len >> 8;
    OscModelReject r = OSC_REJ_NONE;
    rp_events++;
    switch (e->kind) {
    case OSC_RT_EV_POOL_OPEN:
        if (q->open || e->len < 1 || e->len > OSC_POOL_SLOTS) { snprintf(why, n, "bad pool open"); return -1; }
        memset(q, 0, sizeof *q);
        q->open = 1;
        q->k = (uint8_t)e->len;
        q->base = gen;
        q->next = 1;
        osc_model_init(&q->m, gen, UINT64_MAX);
        return 0;
    case OSC_RT_EV_POOL_CLOSE:
        if (!q->open) { snprintf(why, n, "close of a closed pool"); return -1; }
        q->open = 0;
        return 0;
    case OSC_RT_EV_SLOT_ALLOC:
        if (!q->open || ps >= q->k) { snprintf(why, n, "bad slot alloc"); return -1; }
        if (q->next > OSC_MODEL_MAX_HANDLES && rp_rewindow(q, why, n)) return -1;
        if (q->m.slot[ps + 1].gen != gen) { snprintf(why, n, "alloc generation %llu != model %llu", (unsigned long long)gen, (unsigned long long)q->m.slot[ps + 1].gen); return -1; }
        if (rp_step(&q->m, OSC_EV_SLOT_ALLOC, ps + 1, q->next, 0, 3, &r)) break;
        q->hid[ps] = q->next;
        q->mslot[q->next] = ps + 1;
        q->mgen[q->next] = gen;
        q->next++;
        return 0;
    case OSC_RT_EV_SLOT_FREE:
    case OSC_RT_EV_HANDLE_USE:
        if (!q->open || ps >= q->k || !q->hid[ps]) { snprintf(why, n, "pool event on a slot with no handle"); return -1; }
        if (q->mgen[q->hid[ps]] != gen) { snprintf(why, n, "handle generation %llu != minted %llu", (unsigned long long)gen, (unsigned long long)q->mgen[q->hid[ps]]); return -1; }
        if (e->kind == OSC_RT_EV_SLOT_FREE) {
            if (rp_step(&q->m, OSC_EV_SLOT_FREE, 0, q->hid[ps], 0, 0, &r)) break;
            q->hid[ps] = 0;
        } else {
            if (rights < 1 || rights > 2) { snprintf(why, n, "bad use rights"); return -1; }
            if (rp_step(&q->m, OSC_EV_HANDLE_USE, 0, q->hid[ps], 0, rights, &r)) break;
        }
        return 0;
    default:
        snprintf(why, n, "unknown pool event %u", e->kind);
        return -1;
    }
    snprintf(why, n, "pool event %u rejected: %s", i, osc_model_reject_name(r));
    return -1;
}

/* the refused operation of a pool trap is rejected by the model */
static int rp_trap(const OscRt *rt, char *why, size_t n)
{
    unsigned code = rt->trap_code;
    if (code != OSC_TRAP_STALE && code != OSC_TRAP_RETIRED && code != OSC_TRAP_POOL_FULL) return 0;
    if (rt->nev > OSC_RT_EVENTS) return 0; /* log prefix not complete: nothing to confirm */
    if (rt->trap_pool >= OSC_RT_POOLS || !rp[rt->trap_pool].open) { snprintf(why, n, "pool trap on unopened pool"); return -1; }
    RpPool *q = &rp[rt->trap_pool];
    OscModel m = q->m;
    OscModelReject r = OSC_REJ_NONE;
    if (code == OSC_TRAP_STALE) {
        uint32_t s = rt->trap_slot + 1u, h = 0;
        if (s > q->k) { snprintf(why, n, "stale trap on bad slot"); return -1; }
        for (uint32_t k = 1; k < q->next && k <= OSC_MODEL_MAX_HANDLES; k++)
            if (q->mslot[k] == s && q->mgen[k] == rt->trap_gen) h = k;
        int rc;
        if (h && rt->trap_op == 1) rc = rp_step(&m, OSC_EV_HANDLE_USE, 0, h, 0, rt->trap_rights, &r);
        else if (h) rc = rp_step(&m, OSC_EV_SLOT_FREE, 0, h, 0, 0, &r);
        else rc = rp_step(&m, OSC_EV_SLOT_FREE, s, 0, rt->trap_gen, 0, &r);
        if (rc == 0 || r != OSC_REJ_STALE_GENERATION) { snprintf(why, n, "stale trap not a model STALE_GENERATION (%s)", osc_model_reject_name(r)); return -1; }
        rp_stale_confirmed++;
        return 0;
    }
    /* RETIRED / POOL_FULL: no slot of the pool can be allocated */
    unsigned wrap = 0;
    for (uint32_t s = 1; s <= q->k; s++) {
        OscModel t = m;
        if (rp_step(&t, OSC_EV_SLOT_ALLOC, s, 0, 0, 0, &r) == 0) { snprintf(why, n, "pool trap but model slot %u is free", s); return -1; }
        if (r == OSC_REJ_GENERATION_WRAP) wrap++;
        else if (r != OSC_REJ_PROTOCOL) { snprintf(why, n, "pool trap: unexpected %s", osc_model_reject_name(r)); return -1; }
    }
    if ((code == OSC_TRAP_RETIRED) != (wrap > 0)) { snprintf(why, n, "pool trap kind disagrees with the model"); return -1; }
    if (code == OSC_TRAP_RETIRED) rp_wrap_confirmed++;
    else rp_full_confirmed++;
    return 0;
}

static unsigned long rr_runs, rr_events, rr_accepted, rr_rejected;
static int replay_rt(const OscRt *rt, char *why, size_t n)
{
    static OscModel m;
    static uint32_t id_of_serial[1 << 16];
    static uint32_t serial_of_id[OSC_MODEL_MAX_OBJECTS + 1];
    static uint8_t live_id[OSC_MODEL_MAX_OBJECTS + 1];
    static int16_t aslot_of_id[OSC_MODEL_MAX_OBJECTS + 1]; /* arena slot, -1 = unique */
    static uint32_t rid_of_slot[OSC_RT_SLOTS];               /* 0 = no open arena */
    uint32_t next = 1, nextr = 1;
    OscModelEvent ev;
    osc_model_init(&m, 0, UINT64_MAX);
    memset(live_id, 0, sizeof live_id);
    memset(rid_of_slot, 0, sizeof rid_of_slot);
    memset(rp, 0, sizeof rp);
    uint32_t nev = rt->nev < OSC_RT_EVENTS ? rt->nev : OSC_RT_EVENTS;
    for (uint32_t i = 0; i < nev; i++) {
        const OscRtEvent *e = &rt->ev[i];
        if (e->kind >= OSC_RT_EV_POOL_OPEN) { /* OSC-3 item 2 */
            if (rp_event(rt, i, why, n)) return -1;
            rr_events++;
            continue;
        }
        if (e->serial >= (1u << 16)) { snprintf(why, n, "serial too large"); return -1; }
        int is_alloc = e->kind == OSC_RT_EV_ALLOC || e->kind == OSC_RT_EV_ARENA_ALLOC;
        if ((is_alloc && next > OSC_MODEL_MAX_OBJECTS) ||
            (e->kind == OSC_RT_EV_REGION_OPEN && nextr > OSC_MODEL_MAX_REGIONS)) {
            /* new window: re-open open regions, re-create live objects */
            uint32_t keep[OSC_MODEL_MAX_OBJECTS], nk = 0;
            int16_t keep_slot[OSC_MODEL_MAX_OBJECTS];
            for (uint32_t k = 1; k <= OSC_MODEL_MAX_OBJECTS; k++)
                if (live_id[k]) { keep_slot[nk] = aslot_of_id[k]; keep[nk++] = serial_of_id[k]; }
            osc_model_init(&m, 0, UINT64_MAX);
            memset(live_id, 0, sizeof live_id);
            next = 1;
            nextr = 1;
            for (uint32_t s = 0; s < OSC_RT_SLOTS; s++) {
                if (!rid_of_slot[s]) continue;
                if (nextr > OSC_MODEL_MAX_REGIONS) { snprintf(why, n, "16 open regions"); return -1; }
                memset(&ev, 0, sizeof ev);
                ev.kind = OSC_EV_REGION_OPEN;
                ev.region = nextr;
                if (osc_model_step(&m, &ev, NULL) != OSC_MODEL_ACCEPT) { snprintf(why, n, "re-open"); return -1; }
                rid_of_slot[s] = nextr++;
            }
            for (uint32_t k = 0; k < nk; k++) {
                memset(&ev, 0, sizeof ev);
                ev.kind = OSC_EV_ALLOC;
                ev.obj = next;
                ev.region = keep_slot[k] >= 0 ? rid_of_slot[keep_slot[k]] : 0;
                if (osc_model_step(&m, &ev, NULL) != OSC_MODEL_ACCEPT) { snprintf(why, n, "re-alloc"); return -1; }
                id_of_serial[keep[k]] = next;
                serial_of_id[next] = keep[k];
                aslot_of_id[next] = keep_slot[k];
                live_id[next] = 1;
                next++;
            }
            if (is_alloc && next > OSC_MODEL_MAX_OBJECTS) { snprintf(why, n, "64 live objects"); return -1; }
        }
        memset(&ev, 0, sizeof ev);
        switch (e->kind) {
        case OSC_RT_EV_ALLOC:
        case OSC_RT_EV_ARENA_ALLOC:
            ev.kind = OSC_EV_ALLOC;
            ev.obj = next;
            if (e->kind == OSC_RT_EV_ARENA_ALLOC) {
                ev.region = rid_of_slot[e->slot];
                if (!ev.region) { snprintf(why, n, "arena alloc into unopened slot %u", e->slot); return -1; }
            }
            id_of_serial[e->serial] = next;
            serial_of_id[next] = e->serial;
            aslot_of_id[next] = e->kind == OSC_RT_EV_ARENA_ALLOC ? (int16_t)e->slot : -1;
            live_id[next] = 1;
            next++;
            break;
        case OSC_RT_EV_RELEASE:
            ev.kind = OSC_EV_RELEASE;
            ev.obj = id_of_serial[e->serial];
            if (!ev.obj || !live_id[ev.obj]) { snprintf(why, n, "release of unknown serial %u", e->serial); return -1; }
            live_id[ev.obj] = 0;
            break;
        case OSC_RT_EV_REGION_OPEN:
            if (rid_of_slot[e->slot]) { snprintf(why, n, "region open on an open arena slot %u", e->slot); return -1; }
            ev.kind = OSC_EV_REGION_OPEN;
            ev.region = nextr;
            rid_of_slot[e->slot] = nextr++;
            break;
        case OSC_RT_EV_REGION_DESTROY:
            ev.kind = OSC_EV_REGION_DESTROY;
            ev.region = rid_of_slot[e->slot];
            if (!ev.region) { snprintf(why, n, "destroy of unopened arena slot %u", e->slot); return -1; }
            rid_of_slot[e->slot] = 0;
            for (uint32_t k = 1; k <= OSC_MODEL_MAX_OBJECTS; k++)
                if (live_id[k] && aslot_of_id[k] == (int16_t)e->slot) live_id[k] = 0;
            break;
        default:
            snprintf(why, n, "unknown runtime event kind %u", e->kind);
            return -1;
        }
        OscModelReject r;
        if (osc_model_step(&m, &ev, &r) != OSC_MODEL_ACCEPT) {
            snprintf(why, n, "event %u %s rejected: %s", i, osc_model_event_name(ev.kind), osc_model_reject_name(r));
            return -1;
        }
        rr_events++;
    }
    if (rp_trap(rt, why, n)) return -1;
    return 0;
}

/* Replay the native run's runtime event log (every native run in this test:
 * golden fuzz, expect-run lines, contract / struct / arena fuzz, destruction
 * order tests). Non-trapping runs replay fully; trapping runs replay the
 * prefix logged before the trap. This checks executed runs only. */
static int rt_replay_run(const OscRt *rt, const char *ctx)
{
    char why[160];
    rr_runs++;
    if (replay_rt(rt, why, sizeof why) == 0) { rr_accepted++; return 0; }
    rr_rejected++;
    CHECK(0, "%s: runtime event log replay rejected: %s", ctx, why);
    return -1;
}

/* ------------------------------------------------------------ golden */
static OscUnit *U1, *U2;
static unsigned long expect_total;
static const char *trap_names[OSC_TRAP_MAX + 1] = {"none", "OVERFLOW", "DIV0", "BOUNDS", "LOOP_BOUND", "CAST", "OOM", "SHIFT", "RUNTIME", "REQUIRES", "ENSURES", "ARENA_FULL", "STALE", "POOL_FULL", "RETIRED"};

static uint64_t parse_val(const char *s)
{
    if (*s == '-') return (uint64_t)strtoll(s, NULL, 10);
    return strtoull(s, NULL, 10);
}

/* `// expect-run: ENTRY ARG... -> VALUE` or `-> trap=NAME`: hand-computed
 * results; both engines must produce them (catches lowering errors that a
 * differential run of the same IR cannot see). */
static void expect_runs(const char *name, const char *src, const OscNative *nm, const OscCode *code);
static OscTrace *TR;
static OscRt *RI, *RN;
static unsigned long golden_ok, golden_entries, golden_funcs, traces_skipped_overflow;

static void golden(const char *dir, const char *name, unsigned fuzz)
{
    char path[1024], ent[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t len;
    char *src = read_file(path, &len);
    CHECK(src != NULL, "%s: unreadable", name);
    if (!src) return;
    unsigned long f0 = failures;
    OscDiag d;
    int rc = osc_compile(src, len, U1, &d, TR);
    CHECK(rc == 0, "%s: refused: %s line %u object=%s: %s", name, osc_diag_kind_name(d.kind), d.line, d.object, d.message);
    if (rc) { free(src); return; }
    golden_funcs += U1->nfuncs;

    /* determinism: second in-process compile */
    OscDiag d2;
    CHECK(osc_compile(src, len, U2, &d2, NULL) == 0, "%s: second compile refused", name);
    uint8_t g1[32], g2[32];
    CHECK(osc_ir_digest(U1, g1) == 0 && osc_ir_digest(U2, g2) == 0 && memcmp(g1, g2, 32) == 0, "%s: IR digest differs", name);
    OscCode c1, c2;
    char err[160];
    memset(&c1, 0, sizeof c1);
    memset(&c2, 0, sizeof c2);
    int cg1 = osc_cg_compile(U1, &c1, err, sizeof err);
    CHECK(cg1 == 0, "%s: codegen refused: %s", name, err);
    int cg2 = osc_cg_compile(U2, &c2, err, sizeof err);
    CHECK(cg1 == 0 && cg2 == 0 && c1.len == c2.len && memcmp(c1.code, c2.code, c1.len) == 0 &&
          memcmp(c1.entry, c2.entry, sizeof c1.entry) == 0, "%s: code bytes differ", name);
    if (cg2 == 0) osc_cg_free(&c2);
    if (cg1) { free(src); return; }

    /* ownership trace replay, per function */
    CHECK(!TR->refused, "%s: golden trace has a refused event", name);
    if (TR->overflow) traces_skipped_overflow++;
    else {
        uint32_t i = 0;
        while (i < TR->n) {
            uint32_t j = i;
            while (j < TR->n && TR->e[j].func == TR->e[i].func) j++;
            OscModelReject r;
            int bad = replay_trace(TR, i, j, &r);
            CHECK(bad < 0, "%s: trace of %s rejected at event %d (line %u): %s", name, U1->funcs[TR->e[i].func].name,
                  bad, bad >= 0 ? TR->e[bad].line : 0, osc_model_reject_name(r));
            trace_replays++;
            i = j;
        }
    }

    OscNative nm;
    int mr = osc_native_map(&nm, c1.code, c1.len);
    CHECK(mr == 0, "%s: native map failed (%d)", name, mr);
    if (mr) { osc_cg_free(&c1); free(src); return; }

    CHECK(header(src, "entry:", ent, sizeof ent) == 0, "%s: missing // entry: header", name);
    char *save = NULL;
    for (char *tok = strtok_r(ent, " ,", &save); tok; tok = strtok_r(NULL, " ,", &save)) {
        int fi = -1;
        for (int k = 0; k < U1->nfuncs; k++)
            if (strcmp(U1->funcs[k].name, tok) == 0) fi = k;
        CHECK(fi >= 0, "%s: entry %s not found", name, tok);
        if (fi < 0) continue;
        const OscFunc *f = &U1->funcs[fi];
        int scalar = 1;
        for (int p = 0; p < f->nparams; p++)
            if (f->vtype[p].s == OSC_T_REF) scalar = 0;
        CHECK(scalar, "%s: entry %s takes an array parameter", name, tok);
        if (!scalar) continue;
        golden_entries++;
        void *entry = osc_native_at(&nm, c1.entry[fi]);
        sm_state = 0x05C1C0DE00000000ull ^ (uint64_t)(fi * 7919) ^ (uint64_t)strlen(name) * 104729u;
        for (const char *q = name; *q; q++) sm_state = sm_state * 131 + (uint8_t)*q;
        unsigned long pertrap[OSC_TRAP_MAX + 1] = {0};
        for (unsigned it = 0; it < fuzz; it++) {
            uint64_t args[OSC_MAX_PARAMS] = {0}, ri = 0, rn = 0;
            for (int p = 0; p < f->nparams; p++) args[p] = gen_arg(f->vtype[p].s);
            osc_rt_reset(RI);
            osc_rt_reset(RN);
            int ti = osc_interp_run_prevalidated(U1, fi, args, f->nparams, RI, &ri);
            int tn = osc_rt_call_native(RN, entry, args, f->nparams, &rn);
            diff_runs++;
            int same = ti == tn && ti >= 0 && (ti != 0 || ri == rn) && osc_rt_same_outcome(RI, RN);
            CHECK(same, "%s:%s args[0]=%llu: interp trap %d ret %llu vs native trap %d ret %llu", name, tok,
                  (unsigned long long)args[0], ti, (unsigned long long)ri, tn, (unsigned long long)rn);
            if (!same) {
                char bp[256];
                snprintf(bp, sizeof bp, "/tmp/l22c/backend-bug-%s-%s.txt", name, tok);
                FILE *bf = fopen(bp, "w");
                if (bf) {
                    fprintf(bf, "program %s entry %s\nargs:", name, tok);
                    for (int p = 0; p < f->nparams; p++) fprintf(bf, " %llu", (unsigned long long)args[p]);
                    fprintf(bf, "\ninterp trap %d ret %llu; native trap %d ret %llu\n", ti, (unsigned long long)ri, tn,
                            (unsigned long long)rn);
                    fclose(bf);
                }
                break;
            }
            if (ti >= 0 && ti <= OSC_TRAP_MAX) { trap_seen[ti]++; pertrap[ti]++; }
            CHECK(ti != OSC_TRAP_RUNTIME, "%s:%s RUNTIME trap (must never happen for checked code)", name, tok);
            if (ti == 0) {
                CHECK(RI->live_count == 0 && RN->live_count == 0, "%s:%s leak: live_count interp %u native %u", name,
                      tok, RI->live_count, RN->live_count);
            }
            if (rt_replay_run(RN, name) == 0 && ti == 0) rt_replays++;
        }
        printf("  %-18s %-12s ok=%-5lu ovf=%-4lu div0=%-4lu bnd=%-4lu loop=%-4lu cast=%-4lu oom=%-4lu shift=%-4lu rq=%-4lu en=%-4lu af=%-4lu\n",
               name, tok, pertrap[0], pertrap[1], pertrap[2], pertrap[3], pertrap[4], pertrap[5], pertrap[6], pertrap[7], pertrap[9], pertrap[10],
               pertrap[11]);
    }
    expect_runs(name, src, &nm, &c1);
    osc_native_unmap(&nm);
    osc_cg_free(&c1);
    if (failures == f0) golden_ok++;
    free(src);
}

/* ------------------------------------------------------------ front-end mutation fuzz */
/* Mutated golden sources must either compile (unit validated, codegen ok) or
 * be refused with a well-formed diagnostic; never an internal error or crash. */
static unsigned long mut_total, mut_accepted, mut_refused;

static void mutate_fuzz(const char *dir, const char *name, unsigned n)
{
    static const char pool[] = " \n;{}()[]&*+-<>=!~^|%/,:.0123456789abxyzimlet mut own fn if else while bound for in return as u8 i64 bool";
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t len;
    char *src = read_file(path, &len);
    if (!src || len == 0) { free(src); return; }
    char *buf = malloc(len * 2 + 64);
    sm_state = 0xF022u ^ (uint64_t)len;
    for (unsigned it = 0; it < n; it++) {
        size_t bl = len;
        memcpy(buf, src, len);
        unsigned edits = 1 + (unsigned)(sm() % 3);
        for (unsigned e = 0; e < edits && bl > 0; e++) {
            size_t p = (size_t)(sm() % bl);
            switch (sm() % 4) {
            case 0: buf[p] = pool[sm() % (sizeof pool - 1)]; break;
            case 1: { size_t k = 1 + (size_t)(sm() % 8); if (p + k > bl) k = bl - p; memmove(buf + p, buf + p + k, bl - p - k); bl -= k; break; }
            case 2: if (bl + 1 < len * 2 + 64) { memmove(buf + p + 1, buf + p, bl - p); buf[p] = pool[sm() % (sizeof pool - 1)]; bl++; } break;
            default: bl = p; break;
            }
        }
        OscDiag d;
        mut_total++;
        int rc = osc_compile(buf, bl, U1, &d, TR);
        if (rc == 0) {
            OscCode c;
            char err[160];
            memset(&c, 0, sizeof c);
            int cg = osc_cg_compile(U1, &c, err, sizeof err);
            CHECK(cg == 0, "%s mutant %u: accepted but codegen refused: %s", name, it, err);
            if (cg == 0) osc_cg_free(&c);
            mut_accepted++;
        } else {
            CHECK(d.kind > OSC_DIAG_NONE && d.kind < OSC_DIAG__COUNT && d.message[0] && d.object[0] &&
                  strcmp(d.object, "internal") != 0,
                  "%s mutant %u: bad diagnostic kind=%d object=%s: %s", name, it, (int)d.kind, d.object, d.message);
            mut_refused++;
        }
    }
    free(buf);
    free(src);
}

/* ------------------------------------------------------------ negative */
static unsigned long neg_ok, neg_total, neg_traced;

static const char *mapped(int kind)
{
    switch (kind) {
    case OSC_DIAG_USE_AFTER_MOVE: return "use-after-move";
    case OSC_DIAG_MUTABLE_ALIAS: return "mutable-alias";
    case OSC_DIAG_BORROW_OUTLIVES_OWNER: return "borrow-outlives-owner";
    case OSC_DIAG_READ_ONLY_BORROW: return "forged-rights";
    case OSC_DIAG_ARENA_ESCAPE: return "arena-escape";
    case OSC_DIAG_STALE_HANDLE: return "stale-generation"; /* OSC-3 item 2 */
    default: return NULL;
    }
}

static void negative(const char *dir, const char *name)
{
    char path[1024], exp[256], tr[160];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t len;
    char *src = read_file(path, &len);
    CHECK(src != NULL, "%s: unreadable", name);
    if (!src) return;
    neg_total++;
    unsigned long f0 = failures;
    CHECK(header(src, "expect:", exp, sizeof exp) == 0, "%s: missing // expect: header", name);
    char kind[64] = "", obj[64] = "";
    unsigned line = 0;
    char *sp = strchr(exp, ' ');
    if (sp) {
        memcpy(kind, exp, (size_t)(sp - exp) < 63 ? (size_t)(sp - exp) : 63);
        char *o = strstr(sp, "object=");
        char *l = strstr(sp, "line=");
        if (o) sscanf(o + 7, "%63s", obj);
        if (l) line = (unsigned)strtoul(l + 5, NULL, 10);
    }
    int have_tr = header(src, "transition:", tr, sizeof tr) == 0;
    OscDiag d;
    int rc = osc_compile(src, len, U1, &d, TR);
    CHECK(rc == -1, "%s: accepted, expected %s", name, kind);
    if (rc == -1) {
        CHECK(strcmp(osc_diag_kind_name(d.kind), kind) == 0, "%s: kind %s, expected %s (%s)", name,
              osc_diag_kind_name(d.kind), kind, d.message);
        CHECK(strcmp(d.object, obj) == 0, "%s: object '%s', expected '%s'", name, d.object, obj);
        CHECK(d.line == line, "%s: line %u, expected %u", name, d.line, line);
        if (have_tr) CHECK(strstr(d.transition, tr) != NULL, "%s: transition '%s' lacks '%s'", name, d.transition, tr);
        const char *mn = mapped(d.kind);
        if (mn) {
            CHECK(TR->refused && !TR->overflow && TR->n > 0 && TR->e[TR->n - 1].refused,
                  "%s: trace does not end in the refused event", name);
            if (TR->n > 0 && TR->e[TR->n - 1].refused) {
                uint32_t j = TR->n, i = j - 1;
                while (i > 0 && TR->e[i - 1].func == TR->e[j - 1].func) i--;
                OscModelReject r;
                int bad = replay_trace(TR, i, j, &r);
                CHECK(bad == (int)(j - 1), "%s: model rejected at event %d, expected the last (%u): %s", name, bad,
                      j - 1, osc_model_reject_name(r));
                CHECK(bad < 0 || strcmp(osc_model_reject_name(r), mn) == 0, "%s: model says %s, expected %s", name,
                      osc_model_reject_name(r), mn);
                neg_traced++;
            }
        }
    }
    if (failures == f0) neg_ok++;
    free(src);
}

/* ------------------------------------------------------------ OSC-2 contract fuzz */
/* Generated units: 2..4 helper functions with random `requires` / `ensures`
 * clauses over their parameters (and `result`), plus an entry that chains
 * calls to them. Each unit either compiles, or is refused statically with
 * CONTRACT_VIOLATION (a clause or call that folds false); any other refusal
 * is a failure. Compiled units run in the interpreter and natively on random
 * arguments: identical trap / value / pool outcome (0 mismatches), and both
 * TRAP REQUIRES and TRAP ENSURES must be observed. */
static unsigned long cf_mismatch, cf_units, cf_compiled, cf_static_refused, cf_runs, cf_trap[OSC_TRAP_MAX + 1];

static void cf_cat(char *b, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void cf_cat(char *b, size_t cap, const char *fmt, ...)
{
    size_t l = strlen(b);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b + l, cap - l, fmt, ap);
    va_end(ap);
}

/* an atom of type T over the names in scope */
static void cf_atom(char *o, size_t cap, int with_result)
{
    unsigned r = (unsigned)(sm() % (with_result ? 6 : 4));
    switch (r) {
    case 0: snprintf(o, cap, "a"); break;
    case 1: snprintf(o, cap, "b"); break;
    case 2: snprintf(o, cap, "%u", (unsigned)(sm() % 60)); break;
    case 3: snprintf(o, cap, "%s %s %u", sm() & 1 ? "a" : "b", sm() & 1 ? "+" : "-", (unsigned)(sm() % 5)); break;
    default: snprintf(o, cap, "result"); break;
    }
}

static void cf_clause(char *o, size_t cap, int with_result)
{
    static const char *cmp[] = {"<", "<=", ">", ">=", "==", "!="};
    char x[48], y[48], x2[48], y2[48];
    do cf_atom(x, sizeof x, with_result); while (x[0] >= '0' && x[0] <= '9');
    if (with_result && strcmp(x, "result") && sm() % 2) snprintf(x, sizeof x, "result");
    cf_atom(y, sizeof y, 0);
    unsigned shape = (unsigned)(sm() % 8);
    if (shape < 4) { snprintf(o, cap, "%s %s %s", x, cmp[sm() % 6], y); return; }
    do cf_atom(x2, sizeof x2, with_result); while (x2[0] >= '0' && x2[0] <= '9');
    cf_atom(y2, sizeof y2, 0);
    if (shape < 6)
        snprintf(o, cap, "%s %s %s %s %s %s %s", x, cmp[sm() % 6], y, shape == 4 ? "&&" : "||", x2, cmp[sm() % 6], y2);
    else if (shape == 6)
        snprintf(o, cap, "!(%s %s %s)", x, cmp[sm() % 6], y);
    else
        snprintf(o, cap, "%s", sm() % 3 ? "true" : "a >= a");
}

static void contract_fuzz(unsigned n)
{
    static const char *tys[] = {"u8", "u16", "u32", "i8", "i16", "i32", "i64"};
    static const OscScalar tsc[] = {OSC_T_U8, OSC_T_U16, OSC_T_U32, OSC_T_I8, OSC_T_I16, OSC_T_I32, OSC_T_I64};
    static const char *ops[] = {"+", "-", "*", "&", "|", "^"};
    static char src[8192];
    sm_state = 0x05C2C0DEull;
    for (unsigned u = 0; u < n; u++) {
        unsigned ti = (unsigned)(sm() % 7);
        const char *T = tys[ti];
        unsigned nh = 2 + (unsigned)(sm() % 3);
        src[0] = 0;
        for (unsigned h = 0; h < nh; h++) {
            char rq[200], en[200];
            cf_clause(rq, sizeof rq, 0);
            cf_clause(en, sizeof en, 1);
            cf_cat(src, sizeof src, "fn g%u(a: %s, b: %s) -> %s", h, T, T, T);
            if (sm() % 5) cf_cat(src, sizeof src, " requires %s", rq);
            if (sm() % 5) cf_cat(src, sizeof src, " ensures %s", en);
            unsigned body = (unsigned)(sm() % 4);
            if (body == 0)
                cf_cat(src, sizeof src, " {\n    return a %s b;\n}\n", ops[sm() % 6]);
            else if (body == 1)
                cf_cat(src, sizeof src, " {\n    if a > b { return a - b; }\n    return b %s a;\n}\n", ops[sm() % 6]);
            else if (body == 2) /* a constant return: checked statically against ensures */
                cf_cat(src, sizeof src, " {\n    if a == %u { return %u; }\n    return b;\n}\n",
                       (unsigned)(sm() % 40), (unsigned)(sm() % 40));
            else
                cf_cat(src, sizeof src, " {\n    let t: %s = a %s %u;\n    return t;\n}\n", T, ops[sm() % 3],
                       (unsigned)(sm() % 9));
        }
        cf_cat(src, sizeof src, "fn entry(a: %s, b: %s) -> %s {\n    let x0: %s = g0(a, b);\n", T, T, T, T);
        for (unsigned h = 1; h < nh; h++) {
            if (sm() % 6 == 0) /* literal arguments: decided at compile time when the clause folds */
                cf_cat(src, sizeof src, "    let x%u: %s = g%u(%u, %u);\n", h, T, h, (unsigned)(sm() % 50),
                       (unsigned)(sm() % 50));
            else
                cf_cat(src, sizeof src, "    let x%u: %s = g%u(x%u, %s);\n", h, T, h, h - 1, sm() & 1 ? "a" : "b");
        }
        cf_cat(src, sizeof src, "    return x%u;\n}\n", nh - 1);
        cf_units++;

        OscDiag d;
        int rc = osc_compile(src, strlen(src), U1, &d, NULL);
        if (rc) {
            CHECK(d.kind == OSC_DIAG_CONTRACT_VIOLATION, "contract fuzz unit %u refused %s line %u object=%s: %s\n%s", u,
                  osc_diag_kind_name(d.kind), d.line, d.object, d.message, src);
            cf_static_refused++;
            continue;
        }
        cf_compiled++;
        OscCode c;
        char err[160];
        memset(&c, 0, sizeof c);
        int cg = osc_cg_compile(U1, &c, err, sizeof err);
        CHECK(cg == 0, "contract fuzz unit %u codegen refused: %s", u, err);
        if (cg) continue;
        OscNative nm;
        int mr = osc_native_map(&nm, c.code, c.len);
        CHECK(mr == 0, "contract fuzz unit %u native map failed (%d)", u, mr);
        if (mr) { osc_cg_free(&c); continue; }
        int fi = U1->nfuncs - 1;
        void *entry = osc_native_at(&nm, c.entry[fi]);
        for (unsigned it = 0; it < 64; it++) {
            uint64_t args[OSC_MAX_PARAMS] = {0}, ri = 0, rn = 0;
            args[0] = gen_arg(tsc[ti]);
            args[1] = gen_arg(tsc[ti]);
            osc_rt_reset(RI);
            osc_rt_reset(RN);
            int t1 = osc_interp_run_prevalidated(U1, fi, args, 2, RI, &ri);
            int t2 = osc_rt_call_native(RN, entry, args, 2, &rn);
            rt_replay_run(RN, "contract fuzz");
            cf_runs++;
            diff_runs++;
            int same = t1 == t2 && t1 >= 0 && (t1 != 0 || ri == rn) && osc_rt_same_outcome(RI, RN);
            CHECK(same, "contract fuzz unit %u args %llu %llu: interp trap %d ret %llu vs native trap %d ret %llu\n%s", u,
                  (unsigned long long)args[0], (unsigned long long)args[1], t1, (unsigned long long)ri, t2,
                  (unsigned long long)rn, src);
            if (!same) { cf_mismatch++; break; }
            if (t1 >= 0 && t1 <= OSC_TRAP_MAX) cf_trap[t1]++;
            CHECK(t1 != OSC_TRAP_RUNTIME, "contract fuzz unit %u RUNTIME trap", u);
        }
        osc_native_unmap(&nm);
        osc_cg_free(&c);
    }
    printf("contract fuzz: units=%lu compiled=%lu static_refused=%lu runs=%lu ok=%lu requires=%lu ensures=%lu "
           "overflow=%lu other_traps=%lu mismatches=%lu\n",
           cf_units, cf_compiled, cf_static_refused, cf_runs, cf_trap[0], cf_trap[OSC_TRAP_REQUIRES],
           cf_trap[OSC_TRAP_ENSURES], cf_trap[OSC_TRAP_OVERFLOW],
           cf_runs - cf_trap[0] - cf_trap[OSC_TRAP_REQUIRES] - cf_trap[OSC_TRAP_ENSURES] - cf_trap[OSC_TRAP_OVERFLOW] - cf_mismatch, cf_mismatch);
    CHECK(cf_trap[OSC_TRAP_REQUIRES] > 0, "contract fuzz never hit TRAP REQUIRES");
    CHECK(cf_trap[OSC_TRAP_ENSURES] > 0, "contract fuzz never hit TRAP ENSURES");
    CHECK(cf_trap[0] > 0, "contract fuzz never returned normally");
    CHECK(cf_static_refused > 0, "contract fuzz never produced a static CONTRACT_VIOLATION");
    CHECK(cf_compiled > n / 2, "contract fuzz compiled only %lu of %u units", cf_compiled, n);
}


/* ------------------------------------------------------------ OSC-2 structs */
/* Layout: fixed 8-byte cells in declaration order, scalar = 1 cell, [T; N] =
 * N cells, offsets are prefix sums (no padding). The layout is part of the IR
 * digest: reordering two fields changes it; a unit without structs keeps the
 * OSC-1 encoding (format version byte 1). */
static void struct_layout(void)
{
    static const char *s1 = "struct L { a: u8, b: [i16; 3], c: bool, d: [u64; 2], e: i64 }\n"
                            "struct M { z: [u8; 64] }\nfn f() { }\n";
    static const char *s2 = "struct L { b: [i16; 3], a: u8, c: bool, d: [u64; 2], e: i64 }\n"
                            "struct M { z: [u8; 64] }\nfn f() { }\n";
    static const char *s3 = "fn f() { }\n";
    static const uint16_t off[5] = {0, 1, 4, 5, 7}, alen[5] = {0, 3, 0, 2, 0};
    static const OscScalar sc[5] = {OSC_T_U8, OSC_T_I16, OSC_T_BOOL, OSC_T_U64, OSC_T_I64};
    OscDiag d;
    int rc = osc_compile(s1, strlen(s1), U1, &d, NULL);
    CHECK(rc == 0, "struct layout: refused: %s %s", osc_diag_kind_name(d.kind), d.message);
    if (rc) return;
    CHECK(U1->nstructs == 2, "struct layout: nstructs %u", U1->nstructs);
    const OscStruct *L = &U1->structs[0];
    CHECK(strcmp(L->name, "L") == 0 && L->nfields == 5 && L->ncells == 8, "struct layout: L has %u fields, %u cells",
          L->nfields, L->ncells);
    for (int k = 0; k < 5 && k < L->nfields; k++)
        CHECK(L->fields[k].off == off[k] && L->fields[k].alen == alen[k] && L->fields[k].s == sc[k],
              "struct layout: field %s off %u alen %u", L->fields[k].name, L->fields[k].off, L->fields[k].alen);
    CHECK(U1->structs[1].ncells == 64 && U1->structs[1].fields[0].off == 0, "struct layout: M has %u cells",
          U1->structs[1].ncells);
    uint8_t buf[4096], g1[32], g2[32];
    size_t n = 0;
    CHECK(osc_ir_encode(U1, buf, sizeof buf, &n) == 0 && n > 8 && buf[7] == 2, "struct layout: encoding version");
    CHECK(osc_ir_digest(U1, g1) == 0, "struct layout: digest");
    rc = osc_compile(s2, strlen(s2), U2, &d, NULL);
    CHECK(rc == 0 && U2->structs[0].fields[0].off == 0 && U2->structs[0].fields[1].off == 3,
          "struct layout: reordered L offsets");
    CHECK(rc == 0 && osc_ir_digest(U2, g2) == 0 && memcmp(g1, g2, 32) != 0, "struct layout: digest ignores layout");
    rc = osc_compile(s3, strlen(s3), U2, &d, NULL);
    CHECK(rc == 0 && osc_ir_encode(U2, buf, sizeof buf, &n) == 0 && n > 8 && buf[7] == 1,
          "struct layout: struct-free unit is not OSC-1 encoded");
    printf("struct layout: L offsets 0 1 4 5 7 cells 8; M cells 64; digest binds layout\n");
}

/* Destruction order of progs/structs_dtor.osc: owners released at scope end in
 * reverse declaration order, the moved-from struct is not released, in the
 * interpreter and natively. */
/* Index of the function named `name` that takes exactly `nargs` parameters,
 * all scalar, or -1. The harnesses below pass raw integers as arguments; a
 * ref parameter (own / borrow) would receive an integer where native code
 * expects an object address and dereference it (segfault), so such a
 * function is never a valid harness entry. */
static int scalar_entry(const OscUnit *u, const char *name, unsigned nargs)
{
    for (int k = 0; k < u->nfuncs; k++) {
        const OscFunc *f = &u->funcs[k];
        if (strcmp(f->name, name) != 0) continue;
        if ((unsigned)f->nparams != nargs) return -1;
        for (unsigned p = 0; p < nargs; p++)
            if (f->vtype[p].s == OSC_T_REF || f->vtype[p].s == OSC_T_VOID) return -1;
        return k;
    }
    return -1;
}

/* Run function `entry` of <dir>/<file> with argument arg in the interpreter and
 * natively; both pool logs must match the expected (kind, serial) sequence
 * exactly. The entry is looked up by name: a program may define helpers before
 * it (no forward references), so function 0 is not the entry in general. */
static void dtor_order_k(const char *dir, const char *file, const char *entry, uint64_t arg, uint64_t want_ret,
                         unsigned n, const uint8_t *want_kind, const uint32_t *want_ser)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    size_t len;
    char *src = read_file(path, &len);
    CHECK(src != NULL, "%s unreadable", file);
    if (!src) return;
    OscDiag d;
    OscCode c;
    char err[160];
    memset(&c, 0, sizeof c);
    int rc = osc_compile(src, len, U1, &d, NULL);
    free(src);
    CHECK(rc == 0, "%s: compile failed", file);
    if (rc) return;
    int fi = scalar_entry(U1, entry, 1);
    CHECK(fi >= 0, "%s: no function '%s' taking one scalar argument", file, entry);
    if (fi < 0) return;
    int cg = osc_cg_compile(U1, &c, err, sizeof err);
    CHECK(cg == 0, "%s: codegen refused: %s", file, err);
    if (cg) return;
    OscNative nm;
    if (osc_native_map(&nm, c.code, c.len) != 0) { CHECK(0, "%s: map failed", file); osc_cg_free(&c); return; }
    uint64_t args[1] = {arg}, ri = 0, rn = 0;
    osc_rt_reset(RI);
    osc_rt_reset(RN);
    int ti = osc_interp_run(U1, fi, args, 1, RI, &ri);
    int tn = osc_rt_call_native(RN, osc_native_at(&nm, c.entry[fi]), args, 1, &rn);
    rt_replay_run(RN, file);
    CHECK(ti == 0 && tn == 0 && ri == want_ret && rn == want_ret, "%s: run %d/%d ret %llu/%llu", file, ti, tn,
          (unsigned long long)ri, (unsigned long long)rn);
    const OscRt *rts[2] = {RI, RN};
    for (int r = 0; r < 2; r++) {
        CHECK(rts[r]->nev == n, "%s: %u pool events, expected %u", file, rts[r]->nev, n);
        for (unsigned k = 0; k < n && k < rts[r]->nev; k++)
            CHECK(rts[r]->ev[k].kind == want_kind[k] && rts[r]->ev[k].serial == want_ser[k],
                  "%s: event %u is kind %u serial %u, expected kind %u serial %u", file, k, rts[r]->ev[k].kind,
                  rts[r]->ev[k].serial, want_kind[k], want_ser[k]);
    }
    osc_native_unmap(&nm);
    osc_cg_free(&c);
}

static void dtor_order(const char *dir, const char *file, const char *entry, uint64_t want_ret, unsigned n,
                       const uint8_t *want_kind, const uint32_t *want_ser)
{
    dtor_order_k(dir, file, entry, 1, want_ret, n, want_kind, want_ser);
}

/* OSC-3 item 3 (drop flags): progs/df_order.osc release order for k = 0, 1, 3
 * (see the program header). Each engine's pool log is printed as A<serial>
 * (alloc) / R<serial> (release) after dtor_order_k checked it exactly. */
static void df_fmt_log(const OscRt *rt, char *out, size_t cap)
{
    out[0] = 0;
    for (unsigned k = 0; k < rt->nev; k++) {
        size_t l = strlen(out);
        snprintf(out + l, cap - l, "%s%c%u", k ? " " : "", rt->ev[k].kind == 1 ? 'A' : rt->ev[k].kind == 2 ? 'R' : '?',
                 rt->ev[k].serial);
    }
}

static void df_dtor_order(const char *dir)
{
    static const uint8_t kind0[10] = {1, 1, 1, 2, 1, 1, 2, 2, 2, 2}, kind3[10] = {1, 1, 1, 2, 1, 2, 1, 2, 2, 2};
    static const uint32_t ser0[10] = {1, 2, 3, 2, 4, 5, 5, 4, 3, 1}, ser1[10] = {1, 2, 3, 1, 4, 5, 5, 4, 3, 2},
                          ser3[10] = {1, 2, 3, 1, 4, 4, 5, 5, 3, 2};
    static const uint64_t ks[3] = {0, 1, 3}, rets[3] = {8, 8, 26};
    const uint32_t *sers[3] = {ser0, ser1, ser3};
    const uint8_t *kinds[3] = {kind0, kind0, kind3};
    for (int t = 0; t < 3; t++) {
        unsigned long f0 = failures;
        dtor_order_k(dir, "df_order.osc", "order", ks[t], rets[t], 10, kinds[t], sers[t]);
        char li[128], ln[128];
        df_fmt_log(RI, li, sizeof li);
        df_fmt_log(RN, ln, sizeof ln);
        printf("osc3 dropflags: order k=%llu interp=[%s] native=[%s] %s\n", (unsigned long long)ks[t], li, ln,
               failures == f0 && strcmp(li, ln) == 0 ? "match" : "MISMATCH");
        CHECK(strcmp(li, ln) == 0, "df_order k=%llu: engines disagree", (unsigned long long)ks[t]);
    }
    /* Regression (forge run lane30-dropflags-172347, test_osc_compiler segfault):
     * df_order.osc defines its helper eata(a: own [u32; 2]) before the entry
     * order(k: u32). The harness used to run function 0, so natively eata
     * received k (0, 1, 3) as an array address and loaded through it. The
     * entry is now resolved by name and must take only scalars. Counterexamples:
     * running function 0 again crashes the run above; dropping the ref-param
     * test in scalar_entry makes the eata check below fail. */
    int fo = scalar_entry(U1, "order", 1), fe = scalar_entry(U1, "eata", 1);
    CHECK(fo > 0, "df_order.osc: entry 'order' must not be function 0 (got %d); the regression would be vacuous", fo);
    CHECK(fe < 0, "scalar_entry accepted 'eata' (own parameter) as a raw-argument entry (got %d)", fe);
    printf("osc3 dropflags: harness entry by name: order=%d (not function 0), own-param helper eata refused=%s\n", fo,
           fe < 0 ? "yes" : "NO");
}

static void struct_dtor_order(const char *dir)
{
    static const uint8_t want_kind[8] = {1, 1, 1, 2, 2, 1, 2, 2};
    static const uint32_t want_ser[8] = {1, 2, 3, 3, 2, 4, 4, 1};
    dtor_order(dir, "structs_dtor.osc", "dtor", 14, 8, want_kind, want_ser);
    printf("struct destruction order: alloc 1 2 3, release 3 2, alloc 4, release 4 1 (interp == native)\n");
}

/* OSC-2 arenas (arena_dtor.osc): u = ALLOC 1; outer = REGION_OPEN 2;
 * a = ARENA_ALLOC 3; inner = REGION_OPEN 4; b = ARENA_ALLOC 5; v = ALLOC 6;
 * inner block end: RELEASE v 6, REGION_DESTROY inner 4; w = ALLOC 7;
 * outer block end: RELEASE w 7, REGION_DESTROY outer 2; return: RELEASE u 1.
 * Unique owners inside an arena block are released before the arena is
 * destroyed; the inner arena dies before the outer one. */
static void arena_dtor_order(const char *dir)
{
    static const uint8_t want_kind[12] = {1, 3, 4, 3, 4, 1, 2, 5, 1, 2, 5, 2};
    static const uint32_t want_ser[12] = {1, 2, 3, 4, 5, 6, 6, 4, 7, 7, 2, 1};
    dtor_order(dir, "arena_dtor.osc", "adtor", 8, 12, want_kind, want_ser);
    printf("arena destruction order: alloc 1, open 2, aalloc 3, open 4, aalloc 5, alloc 6, release 6, destroy 4, "
           "alloc 7, release 7, destroy 2, release 1 (interp == native)\n");
}

/* Struct fuzz: generated units with 1..2 structs of random integer, bool and
 * [T; N] fields, an entry that builds a struct literal, writes fields
 * (dynamic array-field indexes reach BOUNDS), moves it, passes it as &mut,
 * & (with a requires on a field) and own, and sums the fields. Every unit
 * must compile; interpreter and native outcomes must be identical. */
static unsigned long sf_units, sf_runs, sf_mismatch, sf_trap[OSC_TRAP_MAX + 1];

static void struct_fuzz(unsigned n)
{
    static const char *tys[] = {"u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "bool"};
    static char src[8192];
    sm_state = 0x05C2517Cull;
    for (unsigned u = 0; u < n; u++) {
        unsigned ns = 1 + (unsigned)(sm() % 2), nf[2], ft[2][6], fl[2][6];
        src[0] = 0;
        for (unsigned s = 0; s < ns; s++) {
            nf[s] = 1 + (unsigned)(sm() % 6);
            cf_cat(src, sizeof src, "struct S%u {", s);
            for (unsigned f = 0; f < nf[s]; f++) {
                ft[s][f] = (unsigned)(sm() % 9);
                fl[s][f] = ft[s][f] != 8 && sm() % 3 == 0 ? 1 + (unsigned)(sm() % 4) : 0;
                if (fl[s][f]) cf_cat(src, sizeof src, "%s f%u: [%s; %u]", f ? "," : "", f, tys[ft[s][f]], fl[s][f]);
                else cf_cat(src, sizeof src, "%s f%u: %s", f ? "," : "", f, tys[ft[s][f]]);
            }
            cf_cat(src, sizeof src, " }\n");
        }
        /* reader: requires on a field, sums every field through & */
        for (unsigned s = 0; s < ns; s++) {
            cf_cat(src, sizeof src, "fn rd%u(p: &S%u, k: i64) -> i64", s, s);
            unsigned rf = (unsigned)(sm() % nf[s]);
            if (sm() % 3 == 0) {
                if (ft[s][rf] == 8) cf_cat(src, sizeof src, " requires p.f%u%s || k != 3", rf, fl[s][rf] ? "[0]" : "");
                else cf_cat(src, sizeof src, " requires p.f%u%s != %u", rf, fl[s][rf] ? "[0]" : "", (unsigned)(sm() % 6));
            }
            cf_cat(src, sizeof src, " {\n    let mut acc: i64 = k;\n");
            for (unsigned f = 0; f < nf[s]; f++) {
                char ix[8];
                snprintf(ix, sizeof ix, "%s", fl[s][f] ? "[0]" : "");
                if (ft[s][f] == 8) cf_cat(src, sizeof src, "    if p.f%u%s { acc = acc + 1; }\n", f, ix);
                else if (ft[s][f] == 3) cf_cat(src, sizeof src, "    acc = acc + ((p.f%u%s & 255) as i64);\n", f, ix);
                else cf_cat(src, sizeof src, "    acc = acc + (p.f%u%s as i64);\n", f, ix);
            }
            cf_cat(src, sizeof src, "    return acc;\n}\n");
            /* writer through &mut, dynamic index */
            cf_cat(src, sizeof src, "fn wr%u(p: &mut S%u, i: u8, b: i64) {\n", s, s);
            for (unsigned f = 0; f < nf[s]; f++) {
                if (sm() % 2) continue;
                char ix[16];
                snprintf(ix, sizeof ix, "%s", fl[s][f] ? (sm() % 2 ? "[i & 3]" : "[i % 2]") : "");
                if (ft[s][f] == 8) cf_cat(src, sizeof src, "    p.f%u%s = b > 0;\n", f, ix);
                else cf_cat(src, sizeof src, "    p.f%u%s = p.f%u%s %s ((b & 7) as %s);\n", f, ix, f, ix,
                            sm() % 2 ? "+" : "*", tys[ft[s][f]]);
            }
            cf_cat(src, sizeof src, "}\n");
            cf_cat(src, sizeof src, "fn own%u(p: own S%u, i: u8, b: i64) -> i64 {\n    wr%u(&mut p, i, b);\n"
                                    "    return rd%u(&p, b);\n}\n", s, s, s, s);
        }
        cf_cat(src, sizeof src, "fn entry(a: i64, b: i64, i: u8) -> i64 {\n    let mut acc: i64 = 0;\n");
        for (unsigned s = 0; s < ns; s++) {
            cf_cat(src, sizeof src, "    let v%u: own S%u = S%u {", s, s, s);
            for (unsigned f = 0; f < nf[s]; f++) {
                char val[64];
                if (ft[s][f] == 8) snprintf(val, sizeof val, "a > b");
                else if (sm() % 8 == 0) snprintf(val, sizeof val, "a as %s", tys[ft[s][f]]);
                else snprintf(val, sizeof val, "(a & 15) as %s", tys[ft[s][f]]);
                if (fl[s][f]) cf_cat(src, sizeof src, "%s f%u: [%s; %u]", f ? "," : "", f, val, fl[s][f]);
                else cf_cat(src, sizeof src, "%s f%u: %s", f ? "," : "", f, val);
            }
            cf_cat(src, sizeof src, " };\n");
            const char *cur = "v";
            unsigned ops = 1 + (unsigned)(sm() % 4);
            for (unsigned o = 0; o < ops; o++) {
                unsigned f = (unsigned)(sm() % nf[s]);
                switch (sm() % 4) {
                case 0:
                    if (ft[s][f] == 8) break;
                    if (fl[s][f]) cf_cat(src, sizeof src, "    %s%u.f%u[i] = %s%u.f%u[0] + ((b & 3) as %s);\n", cur, s,
                                         f, cur, s, f, tys[ft[s][f]]);
                    else cf_cat(src, sizeof src, "    %s%u.f%u = %s%u.f%u - ((b & 3) as %s);\n", cur, s, f, cur, s,
                                f, tys[ft[s][f]]);
                    break;
                case 1: cf_cat(src, sizeof src, "    wr%u(&mut %s%u, i, b);\n", s, cur, s); break;
                case 2: cf_cat(src, sizeof src, "    acc = acc + rd%u(&%s%u, a & 7);\n", s, cur, s); break;
                default:
                    if (cur[0] == 'v') {
                        cf_cat(src, sizeof src, "    let m%u: own S%u = v%u;\n", s, s, s);
                        cur = "m";
                    }
                }
            }
            if (sm() % 2) cf_cat(src, sizeof src, "    acc = acc + own%u(%s%u, i, b);\n", s, cur, s);
            else cf_cat(src, sizeof src, "    acc = acc + rd%u(&%s%u, 0);\n", s, cur, s);
        }
        cf_cat(src, sizeof src, "    return acc;\n}\n");
        sf_units++;

        OscDiag d;
        int rc = osc_compile(src, strlen(src), U1, &d, NULL);
        CHECK(rc == 0, "struct fuzz unit %u refused %s line %u object=%s: %s\n%s", u, osc_diag_kind_name(d.kind),
              d.line, d.object, d.message, src);
        if (rc) continue;
        OscCode c;
        char err[160];
        memset(&c, 0, sizeof c);
        int cg = osc_cg_compile(U1, &c, err, sizeof err);
        CHECK(cg == 0, "struct fuzz unit %u codegen refused: %s", u, err);
        if (cg) continue;
        OscNative nm;
        int mr = osc_native_map(&nm, c.code, c.len);
        CHECK(mr == 0, "struct fuzz unit %u native map failed (%d)", u, mr);
        if (mr) { osc_cg_free(&c); continue; }
        int fi = U1->nfuncs - 1;
        void *entry = osc_native_at(&nm, c.entry[fi]);
        for (unsigned it = 0; it < 48; it++) {
            uint64_t args[OSC_MAX_PARAMS] = {0}, ri = 0, rn = 0;
            args[0] = gen_arg(OSC_T_I64);
            args[1] = gen_arg(OSC_T_I64);
            args[2] = it & 1 ? (uint64_t)(it % 5) : gen_arg(OSC_T_U8);
            osc_rt_reset(RI);
            osc_rt_reset(RN);
            int t1 = osc_interp_run_prevalidated(U1, fi, args, 3, RI, &ri);
            int t2 = osc_rt_call_native(RN, entry, args, 3, &rn);
            rt_replay_run(RN, "struct fuzz");
            sf_runs++;
            diff_runs++;
            int same = t1 == t2 && t1 >= 0 && (t1 != 0 || ri == rn) && osc_rt_same_outcome(RI, RN);
            CHECK(same, "struct fuzz unit %u args %llu %llu %llu: interp trap %d ret %llu vs native trap %d ret %llu\n%s",
                  u, (unsigned long long)args[0], (unsigned long long)args[1], (unsigned long long)args[2], t1,
                  (unsigned long long)ri, t2, (unsigned long long)rn, src);
            if (!same) { sf_mismatch++; break; }
            if (t1 >= 0 && t1 <= OSC_TRAP_MAX) sf_trap[t1]++;
            CHECK(t1 != OSC_TRAP_RUNTIME, "struct fuzz unit %u RUNTIME trap", u);
            if (t1 == 0) CHECK(RI->live_count == 0 && RN->live_count == 0, "struct fuzz unit %u leak", u);
        }
        osc_native_unmap(&nm);
        osc_cg_free(&c);
    }
    printf("struct fuzz: units=%lu runs=%lu ok=%lu bounds=%lu requires=%lu overflow=%lu cast=%lu mismatches=%lu\n",
           sf_units, sf_runs, sf_trap[0], sf_trap[OSC_TRAP_BOUNDS], sf_trap[OSC_TRAP_REQUIRES],
           sf_trap[OSC_TRAP_OVERFLOW], sf_trap[OSC_TRAP_CAST], sf_mismatch);
    CHECK(sf_trap[0] > 0, "struct fuzz never returned normally");
    CHECK(sf_trap[OSC_TRAP_BOUNDS] > 0, "struct fuzz never hit TRAP BOUNDS");
    CHECK(sf_trap[OSC_TRAP_REQUIRES] > 0, "struct fuzz never hit TRAP REQUIRES");
}

/* OSC-2 arena fuzz: generated units with 1..3 levels of nested arenas of
 * random bound (4..31 cells). Statements allocate arrays and struct literals
 * into any visible arena, unique owners inside arena blocks, borrows of
 * arena objects passed to & / &mut parameters (dynamic indexes reach BOUNDS),
 * and allocations inside loops and branches whose capacity is only known at
 * run time (TRAP ARENA_FULL). Definite allocations are kept within the bound
 * (the generator tracks the static budget), so every unit must compile.
 * Interpreter and native outcomes must be identical (0 mismatches); every
 * native run's event log replays through the model (rt_replay_run). */
static unsigned long af_units, af_runs, af_mismatch, af_trap[OSC_TRAP_MAX + 1], af_allocs;
typedef struct { unsigned id, cap, used; } AfArena;

static void af_alloc(char *src, size_t cap, AfArena *r, int definite, unsigned *nm)
{
    unsigned v = (*nm)++;
    int st = sm() % 4 == 0;
    unsigned len = st ? 3 : 1 + (unsigned)(sm() % 8);
    if (sm() % 3 == 0) len = 4;
    int wrap = definite && r->used + len > r->cap; /* would be refused statically: make it dynamic */
    if (wrap) cf_cat(src, cap, "    if (a & 3) != 0 {\n");
    if (definite && !wrap) r->used += len;
    af_allocs++;
    if (st) {
        cf_cat(src, cap, "    let x%u: own P in r%u = P { x: %s, y: [b & 7; 2] };\n", v, r->id,
               sm() % 6 ? "a & 1023" : "a");
        cf_cat(src, cap, "    acc = acc + x%u.x + x%u.y[%s];\n", v, v, sm() % 3 ? "1" : "i % 2");
    } else {
        cf_cat(src, cap, "    let x%u: own [i64; %u] in r%u = alloc(%s);\n", v, len, r->id, sm() % 6 ? "a & 1023" : "a");
        if (len == 4 && sm() % 2) cf_cat(src, cap, "    wr4(&mut x%u, i, b);\n    acc = acc + rd4(&x%u);\n", v, v);
        else cf_cat(src, cap, "    acc = acc + x%u[%s];\n", v, sm() % 4 ? "0" : "i & 7");
    }
    if (wrap) cf_cat(src, cap, "    }\n");
}

static void af_body(char *src, size_t cap, AfArena *ar, unsigned nar, unsigned depth, unsigned *nm, unsigned *nmade)
{
    unsigned ns = 2 + (unsigned)(sm() % 4);
    for (unsigned k = 0; k < ns; k++) {
        AfArena *r = &ar[sm() % nar];
        switch (sm() % 6) {
        case 0:
        case 1: af_alloc(src, cap, r, 1, nm); break;
        case 2: {
            unsigned j = (*nm)++;
            cf_cat(src, cap, "    let mut j%u: u8 = 0;\n    while j%u < (i %% 6) bound 6 {\n", j, j);
            af_alloc(src, cap, r, 0, nm);
            cf_cat(src, cap, "    j%u = j%u + 1;\n    }\n", j, j);
            break;
        }
        case 3:
            cf_cat(src, cap, "    if a > b {\n");
            af_alloc(src, cap, r, 0, nm);
            cf_cat(src, cap, "    }\n");
            break;
        case 4: {
            unsigned v = (*nm)++;
            cf_cat(src, cap, "    let w%u: own [i64; 2] = alloc(b & 7);\n    acc = acc + w%u[1];\n", v, v);
            break;
        }
        default:
            if (depth < 3 && *nmade < 6) {
                AfArena *in = &ar[nar];
                in->id = (*nmade)++;
                in->cap = 4 + (unsigned)(sm() % 28);
                in->used = 0;
                cf_cat(src, cap, "    arena r%u bound %u {\n", in->id, in->cap);
                af_body(src, cap, ar, nar + 1, depth + 1, nm, nmade);
                cf_cat(src, cap, "    }\n");
            } else {
                af_alloc(src, cap, r, 1, nm);
            }
        }
    }
}

static void arena_fuzz(unsigned n)
{
    static char src[16384];
    sm_state = 0x05C2A3E4Aull;
    for (unsigned u = 0; u < n; u++) {
        src[0] = 0;
        cf_cat(src, sizeof src,
               "struct P { x: i64, y: [i64; 2] }\n"
               "fn rd4(p: &[i64; 4]) -> i64 { return p[0] + p[1] + p[2] + p[3]; }\n"
               "fn wr4(p: &mut [i64; 4], i: u8, b: i64) { p[i & 3] = p[i %% 4] + (b & 15); p[i %% 5] = 1; }\n"
               "fn entry(a: i64, b: i64, i: u8) -> i64 {\n    let mut acc: i64 = 0;\n");
        AfArena ar[8];
        unsigned nm = 0, nmade = 1;
        ar[0].id = 0;
        ar[0].cap = 4 + (unsigned)(sm() % 28);
        ar[0].used = 0;
        cf_cat(src, sizeof src, "    arena r0 bound %u {\n", ar[0].cap);
        af_body(src, sizeof src, ar, 1, 1, &nm, &nmade);
        cf_cat(src, sizeof src, "    }\n    return acc;\n}\n");
        af_units++;

        OscDiag d;
        int rc = osc_compile(src, strlen(src), U1, &d, NULL);
        CHECK(rc == 0, "arena fuzz unit %u refused %s line %u object=%s: %s\n%s", u, osc_diag_kind_name(d.kind),
              d.line, d.object, d.message, src);
        if (rc) continue;
        OscCode c;
        char err[160];
        memset(&c, 0, sizeof c);
        int cg = osc_cg_compile(U1, &c, err, sizeof err);
        CHECK(cg == 0, "arena fuzz unit %u codegen refused: %s", u, err);
        if (cg) continue;
        OscNative nm2;
        int mr = osc_native_map(&nm2, c.code, c.len);
        CHECK(mr == 0, "arena fuzz unit %u native map failed (%d)", u, mr);
        if (mr) { osc_cg_free(&c); continue; }
        int fi = U1->nfuncs - 1;
        void *entry = osc_native_at(&nm2, c.entry[fi]);
        for (unsigned it = 0; it < 48; it++) {
            uint64_t args[OSC_MAX_PARAMS] = {0}, ri = 0, rn = 0;
            args[0] = gen_arg(OSC_T_I64);
            args[1] = gen_arg(OSC_T_I64);
            args[2] = it & 1 ? (uint64_t)(it % 6) : gen_arg(OSC_T_U8);
            osc_rt_reset(RI);
            osc_rt_reset(RN);
            int t1 = osc_interp_run_prevalidated(U1, fi, args, 3, RI, &ri);
            int t2 = osc_rt_call_native(RN, entry, args, 3, &rn);
            rt_replay_run(RN, "arena fuzz");
            af_runs++;
            diff_runs++;
            int same = t1 == t2 && t1 >= 0 && (t1 != 0 || ri == rn) && osc_rt_same_outcome(RI, RN);
            CHECK(same, "arena fuzz unit %u args %llu %llu %llu: interp trap %d ret %llu vs native trap %d ret %llu\n%s",
                  u, (unsigned long long)args[0], (unsigned long long)args[1], (unsigned long long)args[2], t1,
                  (unsigned long long)ri, t2, (unsigned long long)rn, src);
            if (!same) { af_mismatch++; break; }
            if (t1 >= 0 && t1 <= OSC_TRAP_MAX) { af_trap[t1]++; trap_seen[t1]++; }
            CHECK(t1 != OSC_TRAP_RUNTIME, "arena fuzz unit %u RUNTIME trap", u);
            if (t1 == 0) CHECK(RI->live_count == 0 && RN->live_count == 0, "arena fuzz unit %u leak", u);
        }
        osc_native_unmap(&nm2);
        osc_cg_free(&c);
    }
    printf("arena fuzz: units=%lu allocs=%lu runs=%lu ok=%lu arena_full=%lu bounds=%lu overflow=%lu mismatches=%lu\n",
           af_units, af_allocs, af_runs, af_trap[0], af_trap[OSC_TRAP_ARENA_FULL], af_trap[OSC_TRAP_BOUNDS],
           af_trap[OSC_TRAP_OVERFLOW], af_mismatch);
    CHECK(af_trap[0] > 0, "arena fuzz never returned normally");
    CHECK(af_trap[OSC_TRAP_ARENA_FULL] > 0, "arena fuzz never hit TRAP ARENA_FULL");
}

/* ------------------------------------------------------------ OSC-3 item 2 handle fuzz */
/* Generated units: one pool (K = 1..4 slots of i64, generation base 0, small,
 * or within 3 of 2^64 - 1) with 1..3 mutable handles (the first allocated,
 * the others copies), then random reads, writes, conditional frees,
 * re-allocations (leaking the old slot), free + re-allocate pairs, copies,
 * bounded loops of alloc/free cycles and a nested second pool. Frees behind a
 * dynamic condition make later uses stale at run time only (TRAP STALE);
 * leaks fill the pool (TRAP POOL_FULL); near-maximum bases retire slots
 * (TRAP RETIRED). The generator keeps definite allocations within the static
 * K and generation budgets, so a unit is either compiled or refused with
 * STALE_HANDLE (a copy made stale by a definite free, a static use after
 * free); any other refusal fails. Interpreter and native outcomes must be
 * identical; every native run replays through the OSC-0B model. */
static unsigned long hf_units, hf_compiled, hf_refused, hf_runs, hf_mismatch, hf_trap[OSC_TRAP_MAX + 1];
typedef struct { unsigned k, nh, used, off; uint64_t base, dallocs; } HfPool;

static int hf_budget(const HfPool *p)
{
    uint64_t per = UINT64_MAX - p->base;
    if (!p->off && p->used + 1 > p->k) return 0;
    if (per < 8 && p->dallocs + 1 > (uint64_t)p->k * (per + 1)) return 0;
    return 1;
}

/* one statement; definite = at the pool's own depth */
static void hf_stmt(char *src, size_t cap, HfPool *p, int definite, unsigned depth, unsigned *nm)
{
    unsigned h = (unsigned)(sm() % p->nh), h2 = (unsigned)(sm() % p->nh);
    switch (sm() % 9) {
    case 0: case 1:
        cf_cat(src, cap, "        acc = acc + p0[h%u];\n", h);
        break;
    case 2:
        cf_cat(src, cap, "        p0[h%u] = (acc & 1023) + (b & 7);\n", h);
        break;
    case 3:
        cf_cat(src, cap, "        if ((a >> %u) & 3) == 0 {\n            p0.free(h%u);\n        }\n", (unsigned)(sm() % 40), h);
        p->off = 1;
        break;
    case 4: /* re-allocate, leaking the old slot */
        if (definite && !hf_budget(p)) {
            cf_cat(src, cap, "        if (b & %u) == 1 {\n            h%u = p0.alloc(a & 255);\n        }\n", 1u + (unsigned)(sm() % 7), h);
        } else {
            cf_cat(src, cap, "        h%u = p0.alloc(a & 255);\n", h);
            if (definite) { p->used++; p->dallocs++; }
        }
        break;
    case 5: /* free + re-allocate */
        if (definite && !(p->dallocs + 1 <= (uint64_t)p->k * ((UINT64_MAX - p->base) < 8 ? (UINT64_MAX - p->base) + 1 : 64))) {
            cf_cat(src, cap, "        acc = acc + p0[h%u];\n", h);
        } else {
            cf_cat(src, cap, "        p0.free(h%u);\n        h%u = p0.alloc(i as i64);\n", h, h);
            if (definite) p->dallocs++;
            else p->off = 1;
        }
        break;
    case 6:
        if (h != h2) cf_cat(src, cap, "        h%u = h%u;\n", h2, h);
        else cf_cat(src, cap, "        acc = acc + p0[h%u];\n", h);
        break;
    case 7:
        if (depth < 2) {
            unsigned j = (*nm)++;
            cf_cat(src, cap, "        let mut j%u: u8 = 0;\n        while j%u < (i %% 6) bound 6 {\n", j, j);
            unsigned ns = 1 + (unsigned)(sm() % 3);
            for (unsigned s = 0; s < ns; s++) hf_stmt(src, cap, p, 0, depth + 1, nm);
            cf_cat(src, cap, "        j%u = j%u + 1;\n        }\n", j, j);
        } else {
            cf_cat(src, cap, "        acc = acc + p0[h%u];\n", h);
        }
        break;
    default:
        if (depth == 0 && sm() % 2) {
            unsigned q = (*nm)++;
            cf_cat(src, cap, "        pool q%u: [u8; %u] {\n            let g%u: handle q%u = q%u.alloc((a & 7) as u8);\n"
                             "            q%u[g%u] = q%u[g%u] + 1;\n            acc = acc + (q%u[g%u] as i64);\n        }\n",
                   q, 1u + (unsigned)(sm() % 3), q, q, q, q, q, q, q, q, q);
        } else {
            cf_cat(src, cap, "        acc = acc + p0[h%u];\n", h);
        }
    }
}

static void handle_fuzz(unsigned n)
{
    static char src[16384];
    const unsigned long ev0 = rp_events, st0 = rp_stale_confirmed, wr0 = rp_wrap_confirmed, fu0 = rp_full_confirmed, wi0 = rp_windows;
    sm_state = 0x05C3A2B4ull;
    for (unsigned u = 0; u < n; u++) {
        HfPool p;
        memset(&p, 0, sizeof p);
        p.k = 1 + (unsigned)(sm() % 4);
        p.nh = 1 + (unsigned)(sm() % 3);
        switch (sm() % 3) {
        case 0: p.base = 0; break;
        case 1: p.base = sm() % 1000; break;
        default: p.base = UINT64_MAX - sm() % 4;
        }
        src[0] = 0;
        cf_cat(src, sizeof src, "fn entry(a: i64, b: i64, i: u8) -> i64 {\n    let mut acc: i64 = 0;\n"
                                "    pool p0: [i64; %u] gen %llu {\n        let mut h0: handle p0 = p0.alloc(a & 1023);\n",
               p.k, (unsigned long long)p.base);
        p.used = 1;
        p.dallocs = 1;
        for (unsigned h = 1; h < p.nh; h++) cf_cat(src, sizeof src, "        let mut h%u: handle p0 = h0;\n", h);
        unsigned nm = 0, ns = 3 + (unsigned)(sm() % 6);
        for (unsigned s = 0; s < ns; s++) hf_stmt(src, sizeof src, &p, 1, 0, &nm);
        cf_cat(src, sizeof src, "        acc = acc + p0[h0];\n    }\n    return acc;\n}\n");
        hf_units++;

        OscDiag d;
        int rc = osc_compile(src, strlen(src), U1, &d, NULL);
        if (rc) {
            CHECK(d.kind == OSC_DIAG_STALE_HANDLE, "handle fuzz unit %u refused %s line %u object=%s: %s\n%s", u,
                  osc_diag_kind_name(d.kind), d.line, d.object, d.message, src);
            hf_refused++;
            continue;
        }
        hf_compiled++;
        OscCode c;
        char err[160];
        memset(&c, 0, sizeof c);
        int cg = osc_cg_compile(U1, &c, err, sizeof err);
        CHECK(cg == 0, "handle fuzz unit %u codegen refused: %s", u, err);
        if (cg) continue;
        OscNative nm2;
        int mr = osc_native_map(&nm2, c.code, c.len);
        CHECK(mr == 0, "handle fuzz unit %u native map failed (%d)", u, mr);
        if (mr) { osc_cg_free(&c); continue; }
        int fi = U1->nfuncs - 1;
        void *entry = osc_native_at(&nm2, c.entry[fi]);
        for (unsigned it = 0; it < 48; it++) {
            uint64_t args[OSC_MAX_PARAMS] = {0}, ri = 0, rn = 0;
            args[0] = gen_arg(OSC_T_I64);
            args[1] = gen_arg(OSC_T_I64);
            args[2] = it & 1 ? (uint64_t)(it % 6) : gen_arg(OSC_T_U8);
            osc_rt_reset(RI);
            osc_rt_reset(RN);
            int t1 = osc_interp_run_prevalidated(U1, fi, args, 3, RI, &ri);
            int t2 = osc_rt_call_native(RN, entry, args, 3, &rn);
            rt_replay_run(RN, "handle fuzz");
            hf_runs++;
            diff_runs++;
            int same = t1 == t2 && t1 >= 0 && (t1 != 0 || ri == rn) && osc_rt_same_outcome(RI, RN);
            CHECK(same, "handle fuzz unit %u args %llu %llu %llu: interp trap %d ret %llu vs native trap %d ret %llu\n%s",
                  u, (unsigned long long)args[0], (unsigned long long)args[1], (unsigned long long)args[2], t1,
                  (unsigned long long)ri, t2, (unsigned long long)rn, src);
            if (!same) { hf_mismatch++; break; }
            if (t1 >= 0 && t1 <= OSC_TRAP_MAX) { hf_trap[t1]++; trap_seen[t1]++; }
            CHECK(t1 != OSC_TRAP_RUNTIME, "handle fuzz unit %u RUNTIME trap\n%s", u, src);
        }
        osc_native_unmap(&nm2);
        osc_cg_free(&c);
    }
    printf("osc3 handles: units=%lu compiled=%lu static_refused=%lu runs=%lu ok=%lu stale_traps=%lu pool_full=%lu "
           "retired=%lu overflow=%lu mismatches=%lu\n",
           hf_units, hf_compiled, hf_refused, hf_runs, hf_trap[0], hf_trap[OSC_TRAP_STALE], hf_trap[OSC_TRAP_POOL_FULL],
           hf_trap[OSC_TRAP_RETIRED], hf_trap[OSC_TRAP_OVERFLOW], hf_mismatch);
    printf("osc3 handles: model replay pool_events=%lu stale_confirmed=%lu wrap_confirmed=%lu full_confirmed=%lu "
           "windows=%lu (handle fuzz runs only); all runs re-windows=%lu\n", rp_events - ev0, rp_stale_confirmed - st0, rp_wrap_confirmed - wr0,
           rp_full_confirmed - fu0, rp_windows - wi0, rp_windows);
    CHECK(hf_trap[0] > 0, "handle fuzz never returned normally");
    CHECK(hf_trap[OSC_TRAP_STALE] > 0, "handle fuzz never hit TRAP STALE");
    CHECK(hf_trap[OSC_TRAP_POOL_FULL] > 0, "handle fuzz never hit TRAP POOL_FULL");
    CHECK(hf_trap[OSC_TRAP_RETIRED] > 0, "handle fuzz never hit TRAP RETIRED");
    CHECK(rp_stale_confirmed - st0 == hf_trap[OSC_TRAP_STALE], "stale traps %lu, model-confirmed %lu",
          hf_trap[OSC_TRAP_STALE], rp_stale_confirmed - st0);
    CHECK(rp_wrap_confirmed - wr0 == hf_trap[OSC_TRAP_RETIRED], "retired traps %lu, model-confirmed %lu",
          hf_trap[OSC_TRAP_RETIRED], rp_wrap_confirmed - wr0);
    CHECK(rp_full_confirmed - fu0 == hf_trap[OSC_TRAP_POOL_FULL], "pool_full traps %lu, model-confirmed %lu",
          hf_trap[OSC_TRAP_POOL_FULL], rp_full_confirmed - fu0);
    CHECK(hf_compiled > hf_refused, "handle fuzz mostly refused (%lu of %lu)", hf_refused, hf_units);
    /* serial counters never wrap: at UINT32_MAX the next allocation traps RUNTIME, before any state changes */
    unsigned long sw_traps = 0;
    for (int w = 0; w < 4; w++) {
        osc_rt_reset(RI);
        volatile uint64_t ah = 0;
        if (w == 2 && setjmp(RI->jb) == 0) ah = osc_rt_arena_open(RI, 4);
        if (w == 3) RI->pool_serial = UINT32_MAX;
        else RI->next_serial = UINT32_MAX;
        volatile unsigned live = RI->live_count;
        if (setjmp(RI->jb) == 0) {
            if (w == 0) osc_rt_alloc(RI, 1, 0);
            else if (w == 1) osc_rt_arena_open(RI, 4);
            else if (w == 2) osc_rt_arena_alloc(RI, ah, 1, 0);
            else osc_rt_pool_open(RI, 2, 0);
            CHECK(0, "serial counter %d wrapped without a trap", w);
        } else {
            CHECK(RI->trap_code == OSC_TRAP_RUNTIME && RI->live_count == live, "serial wrap %d trapped %u", w,
                  RI->trap_code);
            sw_traps++;
        }
    }
    printf("osc3 handles: serial counters at UINT32_MAX trap RUNTIME, never wrap: %lu of 4\n", sw_traps);
}

/* OSC-3 item 3 (drop flags) fuzz: generated units whose entry declares
 * array ([u32; 3]) and struct (S) owners and runs a random statement tree
 * of depth <= 3: if / else / else-if with conditions on the arguments,
 * moves by own call and by let-move, reads and writes, branch-local owners,
 * early returns, loop-local owners moved on one path, and calls of an own
 * parameter helper that moves its parameter on some paths. The generator
 * mirrors the checker's merge rule (live / moved / maybe) and only touches
 * owners that are live on the current path, so every unit must compile.
 * Each unit: static trace replay per function (no refused event, model
 * accepts), then interpreter vs native on 24 argument triples with the
 * runtime pool log replayed through the model, no RUNTIME trap, no leak. */
static unsigned long df_units, df_runs, df_mismatch, df_maybe_units, df_rej, df_trace_skip, df_traced,
    df_trap[OSC_TRAP_MAX + 1];
typedef struct { char name[8]; uint8_t st, isst; } DfOwn; /* st: 0 live, 1 moved, 3 maybe */
static DfOwn df_o[64];
static unsigned df_no, df_id, df_maybe;
static char df_src[16384];
#define DFC(...) cf_cat(df_src, sizeof df_src, __VA_ARGS__)

static void df_ind(unsigned d) { for (unsigned k = 0; k <= d; k++) DFC("    "); }
static void df_cond(void)
{
    static const char *v[] = {"a", "b", "c"};
    const char *x = v[sm() % 3];
    switch (sm() % 3) {
    case 0: DFC("%s > %u", x, (unsigned)(sm() % 16)); break;
    case 1: DFC("(%s & %u) == %u", x, 1 + (unsigned)(sm() % 7), (unsigned)(sm() % 4)); break;
    default: DFC("%s < %u", x, (unsigned)(sm() % 16)); break;
    }
}
/* a random owner with state 0 (live) at index >= lo, or -1 */
static int df_pick(unsigned lo)
{
    int c[64], k = 0;
    for (unsigned i = lo; i < df_no; i++)
        if (df_o[i].st == 0) c[k++] = (int)i;
    return k ? c[sm() % (unsigned)k] : -1;
}
static unsigned df_new(const char *pfx, int isst)
{
    unsigned i = df_no++;
    snprintf(df_o[i].name, sizeof df_o[i].name, "%s%u", pfx, df_id++);
    df_o[i].st = 0;
    df_o[i].isst = (uint8_t)isst;
    return i;
}
static void df_decl(unsigned d, int isst, const char *pfx)
{
    unsigned i = df_new(pfx, isst);
    df_ind(d);
    if (isst) DFC("let %s: own S = S { x: (b & 31) as u32, v: [(c & 3) as u32; 2] };\n", df_o[i].name);
    else DFC("let %s: own [u32; 3] = alloc((a & 15) as u32);\n", df_o[i].name);
}

/* one block body at depth d; lo = first owner that may be moved (owners
 * declared outside an enclosing loop stay unmoved); returns 1 if the block
 * ends in a return. Branch-local owners are dropped from df_o at the end. */
static int df_block(unsigned d, unsigned lo, int inloop)
{
    unsigned base = df_no, ns = 1 + (unsigned)(sm() % 3);
    for (unsigned s = 0; s < ns; s++) {
        unsigned k = (unsigned)(sm() % 16);
        int o;
        if (k < 4 && (o = df_pick(lo)) >= 0) { /* move by own call */
            df_ind(d);
            if (df_o[o].isst && sm() % 2) DFC("acc = acc + pm(%s, a);\n", df_o[o].name);
            else DFC("acc = acc + %s(%s);\n", df_o[o].isst ? "es" : "ea", df_o[o].name);
            df_o[o].st = 1;
        } else if (k < 6 && (o = df_pick(lo)) >= 0) { /* let-move */
            int isst = df_o[o].isst;
            df_o[o].st = 1;
            unsigned m = df_new("m", isst);
            df_ind(d);
            DFC("let %s: own %s = %s;\n", df_o[m].name, isst ? "S" : "[u32; 3]", df_o[o].name);
            df_ind(d);
            DFC("acc = acc + (%s%s as u64);\n", df_o[m].name, isst ? ".x" : "[1]");
        } else if (k < 8 && (o = df_pick(0)) >= 0) { /* read and write */
            df_ind(d);
            if (df_o[o].isst) DFC("%s.v[1] = %s.x + ((b & 7) as u32);\n", df_o[o].name, df_o[o].name);
            else DFC("%s[2] = %s[0] + ((b & 7) as u32);\n", df_o[o].name, df_o[o].name);
            df_ind(d);
            DFC("acc = acc + (%s%s as u64);\n", df_o[o].name, df_o[o].isst ? ".v[1]" : "[2]");
        } else if (k < 12 && d < 3) { /* if / else */
            uint8_t s0[64], s1[64];
            for (unsigned i = 0; i < df_no; i++) s0[i] = df_o[i].st;
            df_ind(d);
            DFC("if ");
            df_cond();
            DFC(" {\n");
            int t1 = df_block(d + 1, lo, inloop);
            df_ind(d);
            DFC("}");
            for (unsigned i = 0; i < df_no; i++) { s1[i] = df_o[i].st; df_o[i].st = s0[i]; }
            int t2 = 0;
            if (sm() % 2) {
                DFC(" else {\n");
                t2 = df_block(d + 1, lo, inloop);
                df_ind(d);
                DFC("}");
            }
            DFC("\n");
            if (t1 && t2) { df_no = base; return 1; }
            if (t2) for (unsigned i = 0; i < df_no; i++) df_o[i].st = s1[i];
            else if (!t1)
                for (unsigned i = 0; i < df_no; i++)
                    if (s1[i] != df_o[i].st) { df_o[i].st = 3; df_maybe = 1; }
        } else if (k < 13) { /* branch-local or block owner */
            df_decl(d, (int)(sm() % 2), "t");
        } else if (k < 14 && d < 2 && !inloop) { /* loop-local owner moved on one path */
            unsigned iv = df_id++, lb = df_no;
            df_ind(d);
            DFC("for v%u in 0 .. 3 {\n", iv);
            unsigned w = df_new("w", 0);
            df_ind(d + 1);
            DFC("let %s: own [u32; 3] = alloc((v%u as u32) + ((c & 3) as u32));\n", df_o[w].name, iv);
            df_block(d + 1, lb, 1);
            df_no = lb;
            df_ind(d);
            DFC("}\n");
        } else if (k < 15 && !inloop && sm() % 2) { /* early return */
            df_ind(d);
            DFC("return acc + %u;\n", (unsigned)(sm() % 50));
            df_no = base;
            return 1;
        }
    }
    df_no = base;
    return 0;
}

static void dropflags_fuzz(unsigned n)
{
    sm_state = 0xD50F1A65ull;
    unsigned long rej0 = rr_rejected;
    for (unsigned u = 0; u < n; u++) {
        df_src[0] = 0;
        df_no = df_id = df_maybe = 0;
        DFC("struct S { x: u32, v: [u32; 2] }\n");
        DFC("fn es(p: own S) -> u64 { return (p.x as u64) + (p.v[1] as u64); }\n");
        DFC("fn ea(x: own [u32; 3]) -> u64 { return (x[0] as u64) + (x[2] as u64); }\n");
        DFC("fn pm(p: own S, k: u64) -> u64 {\n    if k > %u { return es(p) + 1; }\n"
            "    let mut r: u64 = p.x as u64;\n    if (k & 1) == 1 { r = r + es(p); }",
            (unsigned)(sm() % 16));
        if (sm() % 2) DFC(" else { let q: own S = p; r = r + (q.v[0] as u64); }\n");
        else DFC("\n");
        DFC("    return r;\n}\n");
        DFC("fn entry(a: u64, b: u64, c: u64) -> u64 {\n    let mut acc: u64 = 0;\n");
        unsigned no = 2 + (unsigned)(sm() % 4);
        for (unsigned i = 0; i < no; i++) df_decl(0, (int)(sm() % 2), "o");
        unsigned keep = df_no;
        if (!df_block(0, 0, 0)) DFC("    return acc;\n");
        df_no = keep;
        DFC("}\n");
        df_units++;
        if (df_maybe) df_maybe_units++;

        OscDiag d;
        int rc = osc_compile(df_src, strlen(df_src), U1, &d, TR);
        CHECK(rc == 0, "dropflags fuzz unit %u refused %s line %u object=%s: %s\n%s", u, osc_diag_kind_name(d.kind),
              d.line, d.object, d.message, df_src);
        if (rc) continue;
        CHECK(!TR->refused, "dropflags fuzz unit %u: trace has a refused event", u);
        if (!TR->overflow) {
            uint32_t i = 0;
            while (i < TR->n) {
                uint32_t j = i;
                while (j < TR->n && TR->e[j].func == TR->e[i].func) j++;
                OscModelReject r;
                int bad = replay_trace(TR, i, j, &r);
                CHECK(bad < 0, "dropflags fuzz unit %u: trace rejected at event %d: %s\n%s", u, bad,
                      osc_model_reject_name(r), df_src);
                trace_replays++;
                df_traced++;
                i = j;
            }
        } else df_trace_skip++;
        OscCode c;
        char err[160];
        memset(&c, 0, sizeof c);
        int cg = osc_cg_compile(U1, &c, err, sizeof err);
        CHECK(cg == 0, "dropflags fuzz unit %u codegen refused: %s", u, err);
        if (cg) continue;
        OscNative nm;
        int mr = osc_native_map(&nm, c.code, c.len);
        CHECK(mr == 0, "dropflags fuzz unit %u native map failed (%d)", u, mr);
        if (mr) { osc_cg_free(&c); continue; }
        int fi = U1->nfuncs - 1;
        void *entry = osc_native_at(&nm, c.entry[fi]);
        for (unsigned it = 0; it < 24; it++) {
            uint64_t args[OSC_MAX_PARAMS] = {0}, ri = 0, rn = 0;
            for (int k = 0; k < 3; k++) args[k] = it < 16 ? sm() % 20 : gen_arg(OSC_T_U64);
            osc_rt_reset(RI);
            osc_rt_reset(RN);
            int t1 = osc_interp_run_prevalidated(U1, fi, args, 3, RI, &ri);
            int t2 = osc_rt_call_native(RN, entry, args, 3, &rn);
            if (rt_replay_run(RN, "dropflags fuzz") != 0) df_rej++;
            df_runs++;
            diff_runs++;
            int same = t1 == t2 && t1 >= 0 && (t1 != 0 || ri == rn) && osc_rt_same_outcome(RI, RN);
            CHECK(same, "dropflags fuzz unit %u args %llu %llu %llu: interp trap %d ret %llu vs native trap %d ret "
                        "%llu\n%s", u, (unsigned long long)args[0], (unsigned long long)args[1],
                  (unsigned long long)args[2], t1, (unsigned long long)ri, t2, (unsigned long long)rn, df_src);
            if (!same) { df_mismatch++; break; }
            if (t1 >= 0 && t1 <= OSC_TRAP_MAX) df_trap[t1]++;
            CHECK(t1 != OSC_TRAP_RUNTIME, "dropflags fuzz unit %u RUNTIME trap (double release)\n%s", u, df_src);
            if (t1 == 0) CHECK(RI->live_count == 0 && RN->live_count == 0, "dropflags fuzz unit %u leak\n%s", u, df_src);
        }
        osc_native_unmap(&nm);
        osc_cg_free(&c);
    }
    printf("osc3 dropflags: fuzz units=%lu runs=%lu mismatches=%lu model_rejected=%lu maybe_units=%lu ok=%lu "
           "traces=%lu trace_skipped=%lu\n", df_units, df_runs, df_mismatch, rr_rejected - rej0, df_maybe_units, df_trap[0],
           df_traced, df_trace_skip);
    CHECK(df_rej == 0 && rr_rejected == rej0, "dropflags fuzz: %lu runs rejected by the model", df_rej);
    CHECK(df_maybe_units > 0, "dropflags fuzz never generated a maybe-moved owner");
    CHECK(df_trap[0] > 0, "dropflags fuzz never returned normally");
}
#undef DFC

/* ------------------------------------------------------------ external byte slices */
/* tests/compiler/slices/NAME.osc (docs/osc/OSC-EXT-BYTES-DESIGN.md): functions with `bytes` / `cells`
 * parameters. Headers: `// slice-entry: f g` (fuzzed entries) and
 * `// slice-run: ENTRY ARG... -> RESULT [cells=a,b,..]`. ARG: an integer; `bytes:HEX` (HEX may be empty:
 * length 0, non-NULL pointer); `bytes:NULL`; `cells:N` (N zero cells) or `cells:N=a,b,..`; `cells:NULL`.
 * RESULT: a value or `trap=NAME`; `cells=` is the final content of the first cells argument. Every buffer is
 * a separate heap block of exactly its length (ASan redzones catch any access past the end), one set for the
 * interpreter and one for the native code; trap, value and cells must agree between them. */
typedef struct {
    uint8_t *bp[OSC_MAX_PARAMS];   /* buffer per argument register pair (NULL for scalars / NULL slices) */
    uint64_t n[OSC_MAX_PARAMS];    /* length in elements */
    int kind[OSC_MAX_PARAMS];      /* 0 scalar, 1 bytes, 2 cells */
    uint64_t sc[OSC_MAX_PARAMS];   /* scalar value */
    int is_null[OSC_MAX_PARAMS];
    void *gbase[OSC_MAX_PARAMS];   /* guard-page mapping behind bp[i], NULL if heap */
    size_t gsize[OSC_MAX_PARAMS];
    unsigned nargs;                /* source arguments */
} SliceCase;

static unsigned long slice_ok, slice_runs, slice_expect, slice_fuzz_runs, slice_neg_overlap;

static void slice_free(SliceCase *s)
{
    for (unsigned i = 0; i < s->nargs; i++) {
        if (s->gbase[i]) munmap(s->gbase[i], s->gsize[i]);
        else free(s->bp[i]);
    }
}

/* a buffer that ends exactly at the end of a mapped page; the next page is PROT_NONE, so the native code
 * (which ASan cannot see) faults on any access past the last element */
static uint8_t *guard_alloc(SliceCase *c, unsigned i, size_t bytes)
{
    size_t pg = 4096, body = (bytes + pg - 1) / pg * pg + pg;
    uint8_t *m = mmap(NULL, body + pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { CHECK(0, "mmap failed"); exit(1); }
    mprotect(m + body, pg, PROT_NONE);
    c->gbase[i] = m;
    c->gsize[i] = body + pg;
    return m + body - bytes;
}

/* heap copy of case s into regs[] (pointer, length per slice); buffers own the copies in *c */
static unsigned slice_regs(const SliceCase *s, SliceCase *c, uint64_t *regs, int guard)
{
    unsigned nr = 0;
    *c = *s;
    for (unsigned i = 0; i < s->nargs; i++) {
        c->bp[i] = NULL;
        c->gbase[i] = NULL;
        if (!s->kind[i]) { regs[nr++] = s->sc[i]; continue; }
        size_t bytes = s->n[i] * (s->kind[i] == 2 ? 8 : 1);
        if (!s->is_null[i]) {
            c->bp[i] = guard ? guard_alloc(c, i, bytes) : malloc(bytes ? bytes : 1);
            if (bytes) memcpy(c->bp[i], s->bp[i], bytes);
        }
        regs[nr++] = (uint64_t)(uintptr_t)c->bp[i];
        regs[nr++] = s->n[i];
    }
    return nr;
}

static int slice_parse_arg(SliceCase *s, char *t)
{
    unsigned i = s->nargs++;
    s->kind[i] = 0;
    if (strncmp(t, "bytes:", 6) == 0 || strncmp(t, "cells:", 6) == 0) {
        int cells = t[0] == 'c';
        char *v = t + 6;
        s->kind[i] = cells ? 2 : 1;
        if (strcmp(v, "NULL") == 0) { s->is_null[i] = 1; s->n[i] = 0; return 0; }
        if (cells) {
            char *eq = strchr(v, '=');
            s->n[i] = strtoull(v, NULL, 10);
            s->bp[i] = calloc(s->n[i] ? s->n[i] : 1, 8);
            if (eq) {
                uint64_t *w = (uint64_t *)s->bp[i];
                char *save = NULL;
                unsigned k = 0;
                for (char *x = strtok_r(eq + 1, ",", &save); x && k < s->n[i]; x = strtok_r(NULL, ",", &save)) w[k++] = parse_val(x);
            }
        } else {
            size_t hl = strlen(v);
            s->n[i] = hl / 2;
            s->bp[i] = malloc(s->n[i] ? s->n[i] : 1);
            for (size_t k = 0; k < s->n[i]; k++) {
                char h[3] = {v[2 * k], v[2 * k + 1], 0};
                s->bp[i][k] = (uint8_t)strtoul(h, NULL, 16);
            }
        }
        return 0;
    }
    s->sc[i] = parse_val(t);
    return 0;
}

/* run one case on both engines; returns 0 and fills the outcome, -1 on disagreement */
static int slice_run(const char *name, const char *what, int fi, const SliceCase *s, void *entry, int *trap_out,
                     uint64_t *ret_out, const SliceCase *keep_cells)
{
    const OscFunc *f = &U1->funcs[fi];
    SliceCase ci, cn;
    uint64_t ai[OSC_MAX_PARAMS] = {0}, an[OSC_MAX_PARAMS] = {0}, ri = 0, rn = 0;
    unsigned nri = slice_regs(s, &ci, ai, 0), nrn = slice_regs(s, &cn, an, 1);
    CHECK(nri == f->nparams && nrn == f->nparams, "%s: %s: %u registers for %u parameters", name, what, nri, f->nparams);
    CHECK(osc_ir_slice_args_ok(f, ai, nri) == 0, "%s: %s: host entry refused valid buffers", name, what);
    osc_rt_reset(RI);
    osc_rt_reset(RN);
    int ti = osc_interp_run_prevalidated(U1, fi, ai, nri, RI, &ri);
    int tn = osc_rt_call_native(RN, entry, an, nrn, &rn);
    slice_runs++;
    int same = ti == tn && ti >= 0 && (ti != 0 || ri == rn) && osc_rt_same_outcome(RI, RN);
    for (unsigned i = 0; i < s->nargs && same; i++)
        if (s->kind[i] == 2 && !s->is_null[i]) same = memcmp(ci.bp[i], cn.bp[i], s->n[i] * 8) == 0;
    CHECK(same, "%s: %s: interp trap %d ret %llu vs native trap %d ret %llu", name, what, ti, (unsigned long long)ri, tn,
          (unsigned long long)rn);
    if (ti >= 0 && ti <= OSC_TRAP_MAX) trap_seen[ti]++;
    *trap_out = ti;
    *ret_out = ri;
    if (keep_cells) /* hand the interpreter's cells back for the expect check */
        for (unsigned i = 0; i < s->nargs; i++)
            if (s->kind[i] == 2 && !s->is_null[i]) memcpy(((SliceCase *)keep_cells)->bp[i], ci.bp[i], s->n[i] * 8);
    slice_free(&ci);
    slice_free(&cn);
    return same ? 0 : -1;
}

static void slice_prog(const char *dir, const char *name, unsigned fuzz)
{
    char path[1024], ent[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t len;
    char *src = read_file(path, &len);
    CHECK(src != NULL, "%s: unreadable", name);
    if (!src) return;
    unsigned long f0 = failures;
    OscDiag d;
    int rc = osc_compile(src, len, U1, &d, TR);
    CHECK(rc == 0, "%s: refused: %s line %u object=%s: %s", name, osc_diag_kind_name(d.kind), d.line, d.object, d.message);
    if (rc) { free(src); return; }
    /* slices make a version 5 unit; determinism of the IR digest and of the code bytes */
    uint8_t enc[65536];
    size_t elen = 0;
    CHECK(osc_ir_encode(U1, enc, sizeof enc, &elen) == 0 && osc_ir_encoding_version(enc, elen) == 5,
          "%s: slice unit is not encoding version 5", name);
    OscDiag d2;
    CHECK(osc_compile(src, len, U2, &d2, NULL) == 0, "%s: second compile refused", name);
    uint8_t g1[32], g2[32];
    CHECK(osc_ir_digest(U1, g1) == 0 && osc_ir_digest(U2, g2) == 0 && memcmp(g1, g2, 32) == 0, "%s: IR digest differs", name);
    OscCode c1;
    char err[160];
    memset(&c1, 0, sizeof c1);
    int cg1 = osc_cg_compile(U1, &c1, err, sizeof err);
    CHECK(cg1 == 0, "%s: codegen refused: %s", name, err);
    OscNative nm;
    if (cg1 || osc_native_map(&nm, c1.code, c1.len)) { if (!cg1) osc_cg_free(&c1); free(src); CHECK(0, "%s: no native code", name); return; }

    /* hand-written runs */
    const char *key = "// slice-run: ";
    size_t kl = strlen(key);
    for (const char *p = src; p && *p;) {
        const char *eol = strchr(p, '\n');
        size_t l = eol ? (size_t)(eol - p) : strlen(p);
        if (l > kl && strncmp(p, key, kl) == 0 && l - kl < 400) {
            char line[512], *save = NULL, *t, *res = NULL, *cellsx = NULL;
            memcpy(line, p + kl, l - kl);
            line[l - kl] = 0;
            char *en = strtok_r(line, " ", &save);
            SliceCase s;
            memset(&s, 0, sizeof s);
            while ((t = strtok_r(NULL, " ", &save))) {
                if (strcmp(t, "->") == 0) { res = strtok_r(NULL, " ", &save); cellsx = strtok_r(NULL, " ", &save); break; }
                if (s.nargs < OSC_MAX_PARAMS) slice_parse_arg(&s, t);
            }
            int fi = -1;
            for (int k = 0; en && k < U1->nfuncs; k++)
                if (strcmp(U1->funcs[k].name, en) == 0) fi = k;
            CHECK(fi >= 0 && res, "%s: bad slice-run line '%.*s'", name, (int)l, p);
            if (fi >= 0 && res) {
                int want_trap = 0, ti = 0;
                uint64_t want = 0, ri = 0;
                if (strncmp(res, "trap=", 5) == 0) {
                    want_trap = -1;
                    for (int k = 1; k <= OSC_TRAP_MAX; k++)
                        if (strcmp(res + 5, trap_names[k]) == 0) want_trap = k;
                } else {
                    want = parse_val(res);
                }
                SliceCase out = s; /* receives the interpreter's final cells */
                for (unsigned i = 0; i < s.nargs; i++)
                    if (s.kind[i] == 2 && !s.is_null[i]) { out.bp[i] = malloc(s.n[i] ? s.n[i] * 8 : 1); }
                int rr = slice_run(name, line, fi, &s, osc_native_at(&nm, c1.entry[fi]), &ti, &ri, &out);
                int ok = rr == 0 && ti == want_trap && (want_trap || ri == want);
                if (ok && cellsx && strncmp(cellsx, "cells=", 6) == 0) {
                    for (unsigned i = 0; i < s.nargs; i++) {
                        if (s.kind[i] != 2 || s.is_null[i]) continue;
                        char *sv = NULL, *w = cellsx + 6;
                        unsigned k = 0;
                        for (char *x = strtok_r(w, ",", &sv); x; x = strtok_r(NULL, ",", &sv), k++)
                            ok = ok && k < s.n[i] && ((uint64_t *)out.bp[i])[k] == parse_val(x);
                        break;
                    }
                }
                CHECK(ok, "%s: slice-run '%.*s': got trap %d ret %llu", name, (int)l, p, ti, (unsigned long long)ri);
                for (unsigned i = 0; i < s.nargs; i++) if (s.kind[i] == 2 && !s.is_null[i]) free(out.bp[i]);
                slice_free(&s);
                slice_expect++;
            }
        }
        p = eol ? eol + 1 : NULL;
    }

    /* fuzzed entries: random lengths 0..40 (0 = NULL), random contents, edge-biased scalar arguments */
    if (header(src, "slice-entry:", ent, sizeof ent) == 0) {
        char *save = NULL;
        for (char *tok = strtok_r(ent, " ,", &save); tok; tok = strtok_r(NULL, " ,", &save)) {
            int fi = -1;
            for (int k = 0; k < U1->nfuncs; k++)
                if (strcmp(U1->funcs[k].name, tok) == 0) fi = k;
            CHECK(fi >= 0, "%s: slice-entry %s not found", name, tok);
            if (fi < 0) continue;
            const OscFunc *f = &U1->funcs[fi];
            void *entry = osc_native_at(&nm, c1.entry[fi]);
            sm_state = 0x5111CE0000000000ull ^ (uint64_t)(fi * 7919);
            for (const char *q = name; *q; q++) sm_state = sm_state * 131 + (uint8_t)*q;
            for (unsigned it = 0; it < fuzz; it++) {
                SliceCase s;
                memset(&s, 0, sizeof s);
                for (int r = 0; r < f->nparams; r++) {
                    unsigned i = s.nargs++;
                    OscScalar t = f->vtype[r].s;
                    if (t == OSC_T_BYTES || t == OSC_T_CELLS) {
                        int cells = t == OSC_T_CELLS;
                        s.kind[i] = cells ? 2 : 1;
                        s.n[i] = sm() % 41;
                        if (s.n[i] == 0 && sm() % 2) s.is_null[i] = 1;
                        size_t bytes = s.n[i] * (cells ? 8 : 1);
                        if (!s.is_null[i]) {
                            s.bp[i] = malloc(bytes ? bytes : 1);
                            for (size_t k = 0; k < bytes; k++) s.bp[i][k] = (uint8_t)(sm() % 7 == 0 ? 0xFF : sm() % 4 == 0 ? 0 : sm());
                        }
                        r++; /* the length register */
                    } else {
                        s.sc[i] = gen_arg(t);
                        /* indices near the slice length are the interesting ones */
                        if (osc_scalar_is_int(t) && sm() % 3 == 0) s.sc[i] = canon(t, sm() % 45);
                    }
                }
                int ti, rr;
                uint64_t ri;
                rr = slice_run(name, tok, fi, &s, entry, &ti, &ri, NULL);
                slice_fuzz_runs++;
                slice_free(&s);
                if (rr) break;
            }
        }
    }
    osc_native_unmap(&nm);
    osc_cg_free(&c1);
    if (failures == f0) slice_ok++;
    free(src);
}

/* host entry check: overlapping bytes / cells, NULL with length, wrapping ranges are refused before any call */
static void slice_host_checks(void)
{
    OscUnit *u = U1;
    OscDiag d;
    const char *src = "fn f(b: bytes, c: cells, d: bytes) -> u64 { return b.len + c.len + d.len; }\n";
    CHECK(osc_compile(src, strlen(src), u, &d, NULL) == 0, "host-check unit refused: %s", d.message);
    const OscFunc *f = &u->funcs[0];
    CHECK(f->nparams == 6, "three slices should take six registers, got %u", f->nparams);
    uint8_t *buf = malloc(64);
    uint64_t v[6];
    #define SA(bp, bn, cp, cn, dp, dn) (v[0] = (uint64_t)(uintptr_t)(bp), v[1] = (bn), v[2] = (uint64_t)(uintptr_t)(cp), \
                                        v[3] = (cn), v[4] = (uint64_t)(uintptr_t)(dp), v[5] = (dn), osc_ir_slice_args_ok(f, v, 6))
    CHECK(SA(buf, 16, buf + 16, 2, buf + 32, 8) == 0, "disjoint buffers refused");
    CHECK(SA(buf, 16, buf + 8, 2, buf + 32, 8) == -1, "overlap bytes/cells accepted");
    slice_neg_overlap++;
    CHECK(SA(buf, 16, buf + 16, 2, buf + 24, 8) == -1, "overlap cells/bytes(d) accepted");
    slice_neg_overlap++;
    CHECK(SA(buf, 16, buf + 16, 2, buf + 16, 0) == 0, "empty slice inside a buffer refused");
    CHECK(SA(buf, 16, buf, 0, buf, 16) == 0, "bytes over bytes with an empty cells refused");
    CHECK(SA(NULL, 0, NULL, 0, NULL, 0) == 0, "all-empty NULL slices refused");
    CHECK(SA(NULL, 1, buf + 16, 1, buf + 32, 1) == -1, "NULL with a length accepted");
    slice_neg_overlap++;
    CHECK(SA(buf, UINT64_MAX, buf + 16, 1, buf + 32, 1) == -1, "wrapping bytes range accepted");
    slice_neg_overlap++;
    CHECK(SA(buf, 1, buf + 16, UINT64_MAX / 4, buf + 32, 1) == -1, "wrapping cells range accepted");
    slice_neg_overlap++;
    #undef SA
    /* the interpreter entry applies the same check and returns -1 without running */
    uint64_t ov[6] = {(uint64_t)(uintptr_t)buf, 16, (uint64_t)(uintptr_t)(buf + 8), 2, (uint64_t)(uintptr_t)(buf + 40), 4}, r = 77;
    osc_rt_reset(RI);
    CHECK(osc_interp_run(u, 0, ov, 6, RI, &r) == -1 && r == 77, "interpreter ran overlapping buffers");
    /* encoding versions: plain 1, slices 5, unknown refused, bad magic refused */
    uint8_t enc[4096];
    size_t el = 0;
    CHECK(osc_ir_encode(u, enc, sizeof enc, &el) == 0 && osc_ir_encoding_version(enc, el) == 5, "slice unit not version 5");
    const char *plain = "fn g(a: u64) -> u64 { return a + 1; }\n";
    CHECK(osc_compile(plain, strlen(plain), u, &d, NULL) == 0, "plain unit refused");
    CHECK(osc_ir_encode(u, enc, sizeof enc, &el) == 0 && osc_ir_encoding_version(enc, el) == 1, "plain unit not version 1");
    enc[7] = 6;
    CHECK(osc_ir_encoding_version(enc, el) == -1, "unknown version 6 accepted");
    enc[7] = 0;
    CHECK(osc_ir_encoding_version(enc, el) == -1, "version 0 accepted");
    enc[7] = 1;
    enc[3] = 'X';
    CHECK(osc_ir_encoding_version(enc, el) == -1, "bad magic accepted");
    CHECK(osc_ir_encoding_version(enc, 7) == -1 && osc_ir_encoding_version(NULL, 0) == -1, "short buffer accepted");
    free(buf);
}

int main(int argc, char **argv)
{
    unsigned fuzz = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1000;
    const char *root = argc > 2 ? argv[2] : "tests/compiler";
    char pdir[512], ndir[512];
    snprintf(pdir, sizeof pdir, "%s/progs", root);
    snprintf(ndir, sizeof ndir, "%s/neg", root);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    U1 = malloc(sizeof *U1);
    U2 = malloc(sizeof *U2);
    TR = malloc(sizeof *TR);
    RI = malloc(sizeof *RI);
    RN = malloc(sizeof *RN);
    if (!U1 || !U2 || !TR || !RI || !RN) { printf("OSC1_COMPILER_FAIL out of memory\n"); return 1; }
    osc_rt_init(RI);
    osc_rt_init(RN);

    char **pv, **nv;
    int np = list_osc(pdir, &pv), nn = list_osc(ndir, &nv);
    CHECK(np >= 20, "need >= 20 golden programs, found %d", np);
    CHECK(nn >= 30, "need >= 30 negative programs, found %d", nn);
    printf("golden programs (%d), fuzz=%u per entry:\n", np, fuzz);
    for (int i = 0; i < np; i++) golden(pdir, pv[i], fuzz);
    for (int i = 0; i < nn; i++) negative(ndir, nv[i]);
    unsigned muts = fuzz / 4 ? fuzz / 4 : 1;
    for (int i = 0; i < np; i++) mutate_fuzz(pdir, pv[i], muts);
    printf("front-end mutants: %lu (accepted %lu, refused %lu)\n", mut_total, mut_accepted, mut_refused);
    contract_fuzz(fuzz / 5 ? fuzz / 5 : 20);
    struct_layout();
    struct_dtor_order(pdir);
    arena_dtor_order(pdir);
    struct_fuzz(fuzz / 8 ? fuzz / 8 : 16);
    arena_fuzz(fuzz / 8 ? fuzz / 8 : 16);
    handle_fuzz(fuzz / 8 ? fuzz / 8 : 16);
    df_dtor_order(pdir);
    dropflags_fuzz(fuzz / 8 ? fuzz / 8 : 16);
    {
        char sdir[512];
        char **sv;
        snprintf(sdir, sizeof sdir, "%s/slices", root);
        int ns = list_osc(sdir, &sv);
        CHECK(ns >= 4, "need >= 4 slice programs, found %d", ns);
        for (int i = 0; i < ns; i++) slice_prog(sdir, sv[i], fuzz);
        slice_host_checks();
        printf("external byte slices: programs=%lu/%d expect_runs=%lu fuzz_runs=%lu diff_runs=%lu host_refusals=%lu\n", slice_ok, ns,
               slice_expect, slice_fuzz_runs, slice_runs, slice_neg_overlap);
        for (int i = 0; i < ns; i++) free(sv[i]);
        free(sv);
    }

    const char *const *tn = trap_names;
    printf("trap coverage (runs, interpreter == native):\n");
    for (int k = 0; k <= OSC_TRAP_MAX; k++) printf("  %-10s %lu\n", tn[k], trap_seen[k]);
    for (int k = 1; k <= 7; k++) CHECK(trap_seen[k] > 0, "trap %s never observed", tn[k]);
    CHECK(trap_seen[8] == 0, "RUNTIME trap observed");
    CHECK(trap_seen[OSC_TRAP_REQUIRES] > 0 && trap_seen[OSC_TRAP_ENSURES] > 0, "golden fuzz never hit REQUIRES/ENSURES");
    CHECK(traces_skipped_overflow == 0, "%lu golden traces exceeded the model's 64 ids", traces_skipped_overflow);
    printf("runtime model replay: runs=%lu events=%lu accepted=%lu rejected=%lu\n", rr_runs, rr_events, rr_accepted,
           rr_rejected);
    CHECK(rr_runs > 0 && rr_rejected == 0 && rr_accepted == rr_runs, "runtime model replay: %lu of %lu runs rejected",
          rr_rejected, rr_runs);

    for (int i = 0; i < np; i++) free(pv[i]);
    for (int i = 0; i < nn; i++) free(nv[i]);
    free(pv);
    free(nv);
    free(U1); free(U2); free(TR); free(RI); free(RN);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("golden=%lu/%d funcs=%lu entries=%lu expect_runs=%lu diff_runs=%lu trace_replays=%lu rt_replays=%lu rt_events=%lu "
           "neg=%lu/%lu neg_traced=%lu checks=%lu failed=%lu time=%.2fs\n",
           golden_ok, np, golden_funcs, golden_entries, expect_total, diff_runs, trace_replays, rt_replays, rr_events,
           neg_ok, neg_total, neg_traced, checks, failures, secs);
    if (failures == 0) {
        printf("OSC1_COMPILER_PASS golden=%lu neg=%lu diff_runs=%lu checks=%lu\n", golden_ok, neg_ok, diff_runs, checks);
        return 0;
    }
    printf("OSC1_COMPILER_FAIL golden=%lu/%d neg=%lu/%lu failed=%lu\n", golden_ok, np, neg_ok, neg_total, failures);
    return 1;
}

static void expect_runs(const char *name, const char *src, const OscNative *nm, const OscCode *code)
{
    const char *key = "// expect-run: ";
    size_t kl = strlen(key);
    for (const char *p = src; p && *p;) {
        const char *eol = strchr(p, '\n');
        size_t l = eol ? (size_t)(eol - p) : strlen(p);
        if (l > kl && strncmp(p, key, kl) == 0 && l - kl < 400) {
            char line[512], *save = NULL;
            memcpy(line, p + kl, l - kl);
            line[l - kl] = 0;
            char *ent = strtok_r(line, " ", &save);
            uint64_t args[OSC_MAX_PARAMS + 1] = {0};
            unsigned na = 0;
            char *t, *res = NULL;
            while ((t = strtok_r(NULL, " ", &save))) {
                if (strcmp(t, "->") == 0) { res = strtok_r(NULL, " ", &save); break; }
                if (na <= OSC_MAX_PARAMS) args[na++] = parse_val(t);
            }
            int fi = -1;
            for (int k = 0; ent && k < U1->nfuncs; k++)
                if (strcmp(U1->funcs[k].name, ent) == 0) fi = k;
            CHECK(fi >= 0 && res && na == U1->funcs[fi].nparams, "%s: bad expect-run line '%.*s'", name, (int)l, p);
            if (fi >= 0 && res && na == U1->funcs[fi].nparams) {
                const OscFunc *f = &U1->funcs[fi];
                for (unsigned k = 0; k < na; k++) args[k] = canon(f->vtype[k].s, args[k]);
                int want_trap = 0;
                uint64_t want = 0;
                if (strncmp(res, "trap=", 5) == 0) {
                    want_trap = -1;
                    for (int k = 1; k <= OSC_TRAP_MAX; k++)
                        if (strcmp(res + 5, trap_names[k]) == 0) want_trap = k;
                } else {
                    want = canon(f->ret.s, parse_val(res));
                }
                uint64_t ri = 0, rn = 0;
                osc_rt_reset(RI);
                osc_rt_reset(RN);
                int ti = osc_interp_run(U1, fi, args, na, RI, &ri);
                int tn = osc_rt_call_native(RN, osc_native_at(nm, code->entry[fi]), args, na, &rn);
                rt_replay_run(RN, name);
                int ok = ti == want_trap && tn == want_trap && (want_trap || (ri == want && rn == want &&
                                                                              RI->live_count == 0 && RN->live_count == 0));
                CHECK(ok, "%s: expect-run '%.*s': interp trap %d ret %llu, native trap %d ret %llu", name, (int)l, p,
                      ti, (unsigned long long)ri, tn, (unsigned long long)rn);
                expect_total++;
            }
        }
        p = eol ? eol + 1 : NULL;
    }
}
