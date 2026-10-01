/*
 * osc_model_gen.c -- seeded generator of valid and single-fault sequences for
 * the II.11 model. The shadow state here is written independently of
 * osc_model.c: it only ever has to apply VALID events, and it decides
 * validity with its own predicates. See osc_model_gen.h.
 */
#include "osc_model_gen.h"

#include <string.h>

#define NO OSC_MODEL_MAX_OBJECTS
#define NB OSC_MODEL_MAX_BORROWS
#define NR OSC_MODEL_MAX_REGIONS
#define NS OSC_MODEL_MAX_SLOTS
#define NH OSC_MODEL_MAX_HANDLES
#define NC OSC_MODEL_MAX_CELLS
#define NT OSC_MODEL_MAX_TOKENS
#define NM OSC_MODEL_MAX_SEMIDS

#define RESERVE 6   /* fresh ids kept back from valid steps for injection setup */

/* shadow states */
enum { OB_NONE, OB_OWN, OB_MOVED, OB_REL };
enum { BR_NONE, BR_LIVE, BR_END };
enum { RG_NONE, RG_OPEN, RG_DEAD };
enum { SL_FREE, SL_LIVE, SL_RET };
enum { CE_NONE, CE_WR, CE_PUB, CE_OBS, CE_GONE };
enum { TK_NONE, TK_LIVE, TK_USED };

typedef struct {
    OscGenSeq *q;
    uint64_t rng, gmax;
    uint8_t ost[NO + 1]; uint32_t oreg[NO + 1]; int osh[NO + 1], omu[NO + 1]; uint32_t nobj;
    uint8_t bst[NB + 1], bmut[NB + 1]; uint32_t bobj[NB + 1], bpar[NB + 1];
    int bksh[NB + 1], bkmu[NB + 1]; uint32_t nbor;
    uint8_t rst[NR + 1]; uint32_t nreg;
    uint8_t sst[NS + 1]; uint64_t sgen[NS + 1];
    uint8_t hused[NH + 1]; uint32_t hslot[NH + 1]; uint64_t hgen[NH + 1], hrig[NH + 1]; uint32_t nhnd;
    uint8_t cst[NC + 1]; uint32_t ncell;
    uint8_t tst[NT + 1]; uint32_t tcell[NT + 1]; uint32_t ntok;
    uint32_t nsem;
} Sh;

uint64_t osc_gen_splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static uint32_t rnd(Sh *s, uint32_t n) { return n ? (uint32_t)(osc_gen_splitmix64(&s->rng) % n) : 0; }

/* fresh id = count + 1 when within cap - reserve, else 0 */
static uint32_t fresh(uint32_t used, uint32_t cap, uint32_t reserve)
{
    return used + 1 + reserve <= cap ? used + 1 : 0;
}

/* ---- shadow predicates (independent of osc_model.c) ---- */
static int obj_free_of_borrows(const Sh *s, uint32_t x) { return s->ost[x] == OB_OWN && !s->osh[x] && !s->omu[x]; }
static int obj_readable(const Sh *s, uint32_t x)        { return s->ost[x] == OB_OWN && !s->omu[x]; }
static int bor_live(const Sh *s, uint32_t b)            { return s->bst[b] == BR_LIVE; }
static int bor_leaf(const Sh *s, uint32_t b)            { return bor_live(s, b) && !s->bksh[b] && !s->bkmu[b]; }
static int hnd_valid(const Sh *s, uint32_t h)
{
    return s->hused[h] && s->sst[s->hslot[h]] == SL_LIVE && s->sgen[s->hslot[h]] == s->hgen[h];
}
static int region_destroyable(const Sh *s, uint32_t r)
{
    if (s->rst[r] != RG_OPEN) return 0;
    for (uint32_t b = 1; b <= s->nbor; b++)
        if (bor_live(s, b) && s->oreg[s->bobj[b]] == r) return 0;
    return 1;
}

/* pick a random id in 1..n satisfying pred, 0 if none */
#define PICK(s, n, cond) ({ uint32_t c_[256]; uint32_t k_ = 0; \
    for (uint32_t i_ = 1; i_ <= (n); i_++) { uint32_t id = i_; if (cond) c_[k_++] = id; } \
    k_ ? c_[rnd((s), k_)] : 0u; })

/* ---- shadow apply: valid events only ---- */
static void sh_apply(Sh *s, const OscModelEvent *e)
{
    switch (e->kind) {
    case OSC_EV_ALLOC:
        s->ost[e->obj] = OB_OWN; s->oreg[e->obj] = e->region;
        if (e->obj > s->nobj) s->nobj = e->obj;
        break;
    case OSC_EV_MOVE:
        s->ost[e->obj] = OB_MOVED;
        if (e->obj2) {
            s->ost[e->obj2] = OB_OWN; s->oreg[e->obj2] = s->oreg[e->obj];
            if (e->obj2 > s->nobj) s->nobj = e->obj2;
        }
        break;
    case OSC_EV_BORROW_SHARED:
    case OSC_EV_BORROW_MUT: {
        int mut = e->kind == OSC_EV_BORROW_MUT;
        s->bst[e->borrow] = BR_LIVE; s->bmut[e->borrow] = (uint8_t)mut;
        s->bobj[e->borrow] = e->obj; s->bpar[e->borrow] = e->via;
        s->bksh[e->borrow] = s->bkmu[e->borrow] = 0;
        if (e->via) { if (mut) s->bkmu[e->via]++; else s->bksh[e->via]++; }
        else        { if (mut) s->omu[e->obj]++;  else s->osh[e->obj]++; }
        if (e->borrow > s->nbor) s->nbor = e->borrow;
        break;
    }
    case OSC_EV_END_BORROW: {
        uint32_t b = e->borrow, p = s->bpar[b];
        s->bst[b] = BR_END;
        if (p) { if (s->bmut[b]) s->bkmu[p]--; else s->bksh[p]--; }
        else   { if (s->bmut[b]) s->omu[s->bobj[b]]--; else s->osh[s->bobj[b]]--; }
        break;
    }
    case OSC_EV_RELEASE: s->ost[e->obj] = OB_REL; break;
    case OSC_EV_REGION_OPEN:
        s->rst[e->region] = RG_OPEN;
        if (e->region > s->nreg) s->nreg = e->region;
        break;
    case OSC_EV_REGION_DESTROY:
        for (uint32_t x = 1; x <= s->nobj; x++)
            if (s->ost[x] == OB_OWN && s->oreg[x] == e->region) s->ost[x] = OB_REL;
        s->rst[e->region] = RG_DEAD;
        break;
    case OSC_EV_SLOT_ALLOC:
        s->sst[e->slot] = SL_LIVE;
        if (e->handle) {
            s->hused[e->handle] = 1; s->hslot[e->handle] = e->slot;
            s->hgen[e->handle] = s->sgen[e->slot]; s->hrig[e->handle] = e->rights;
            if (e->handle > s->nhnd) s->nhnd = e->handle;
        }
        break;
    case OSC_EV_SLOT_FREE: {
        uint32_t sl = s->hslot[e->handle];
        if (s->sgen[sl] == s->gmax) s->sst[sl] = SL_RET;
        else { s->sst[sl] = SL_FREE; s->sgen[sl] += 1; }
        break;
    }
    case OSC_EV_HANDLE_DERIVE:
        s->hused[e->handle] = 1; s->hslot[e->handle] = s->hslot[e->handle2];
        s->hgen[e->handle] = s->hgen[e->handle2]; s->hrig[e->handle] = e->rights;
        if (e->handle > s->nhnd) s->nhnd = e->handle;
        break;
    case OSC_EV_PERSIST: if (e->semid > s->nsem) s->nsem = e->semid; break;
    case OSC_EV_CELL_WRITE:
        s->cst[e->cell] = CE_WR;
        if (e->cell > s->ncell) s->ncell = e->cell;
        break;
    case OSC_EV_CELL_PUBLISH: s->cst[e->cell] = CE_PUB; break;
    case OSC_EV_CELL_OBSERVE: s->cst[e->cell] = CE_OBS; break;
    case OSC_EV_QUIESCE:
        s->tst[e->token] = TK_LIVE; s->tcell[e->token] = e->cell;
        if (e->token > s->ntok) s->ntok = e->token;
        break;
    case OSC_EV_RECLAIM: s->cst[e->cell] = CE_GONE; s->tst[e->token] = TK_USED; break;
    default: break; /* reads/uses/durable writes do not change shadow state */
    }
}

static int emit(Sh *s, const OscModelEvent *e)
{
    if (s->q->n >= OSC_GEN_MAX_EVENTS - 1) return -1;
    s->q->ev[s->q->n++] = *e;
    sh_apply(s, e);
    return 0;
}

static OscModelEvent ev0(uint32_t kind) { OscModelEvent e; memset(&e, 0, sizeof e); e.kind = kind; return e; }

/* ---- valid step construction ---- */
#define N_ACTIONS 26

static int build_valid(Sh *s, int a, uint32_t reserve, OscModelEvent *e)
{
    uint32_t x, b, r, h, c, t, f;
    switch (a) {
    case 0:
        if (!(f = fresh(s->nobj, NO, reserve))) return 0;
        *e = ev0(OSC_EV_ALLOC); e->obj = f;
        if (rnd(s, 2)) e->region = PICK(s, s->nreg, s->rst[id] == RG_OPEN);
        return 1;
    case 1:
        if (!(f = fresh(s->nreg, NR, reserve))) return 0;
        *e = ev0(OSC_EV_REGION_OPEN); e->region = f; return 1;
    case 2:
        if (!(x = PICK(s, s->nobj, obj_free_of_borrows(s, id)))) return 0;
        *e = ev0(OSC_EV_MOVE); e->obj = x;
        if (rnd(s, 4)) e->obj2 = fresh(s->nobj, NO, reserve);
        return 1;
    case 3:
        if (!(x = PICK(s, s->nobj, obj_readable(s, id))) || !(f = fresh(s->nbor, NB, reserve))) return 0;
        *e = ev0(OSC_EV_BORROW_SHARED); e->obj = x; e->borrow = f; return 1;
    case 4:
        if (!(b = PICK(s, s->nbor, bor_live(s, id) && !s->bkmu[id])) || !(f = fresh(s->nbor, NB, reserve))) return 0;
        *e = ev0(OSC_EV_BORROW_SHARED); e->obj = s->bobj[b]; e->borrow = f; e->via = b; return 1;
    case 5:
        if (!(x = PICK(s, s->nobj, obj_free_of_borrows(s, id))) || !(f = fresh(s->nbor, NB, reserve))) return 0;
        *e = ev0(OSC_EV_BORROW_MUT); e->obj = x; e->borrow = f; return 1;
    case 6:
        if (!(b = PICK(s, s->nbor, bor_leaf(s, id) && s->bmut[id])) || !(f = fresh(s->nbor, NB, reserve))) return 0;
        *e = ev0(OSC_EV_BORROW_MUT); e->obj = s->bobj[b]; e->borrow = f; e->via = b; return 1;
    case 7:
        if (!(b = PICK(s, s->nbor, bor_leaf(s, id)))) return 0;
        *e = ev0(OSC_EV_END_BORROW); e->borrow = b; return 1;
    case 8:
        if (!(x = PICK(s, s->nobj, obj_readable(s, id)))) return 0;
        *e = ev0(OSC_EV_USE_READ); e->obj = x; return 1;
    case 9:
        if (!(b = PICK(s, s->nbor, bor_live(s, id) && !s->bkmu[id]))) return 0;
        *e = ev0(OSC_EV_USE_READ); e->obj = s->bobj[b]; e->via = b; return 1;
    case 10:
        if (!(x = PICK(s, s->nobj, obj_free_of_borrows(s, id)))) return 0;
        *e = ev0(OSC_EV_USE_WRITE); e->obj = x; return 1;
    case 11:
        if (!(b = PICK(s, s->nbor, bor_leaf(s, id) && s->bmut[id]))) return 0;
        *e = ev0(OSC_EV_USE_WRITE); e->obj = s->bobj[b]; e->via = b; return 1;
    case 12:
        if (!(x = PICK(s, s->nobj, obj_free_of_borrows(s, id)))) return 0;
        *e = ev0(OSC_EV_RELEASE); e->obj = x; return 1;
    case 13:
        if (!(r = PICK(s, s->nreg, region_destroyable(s, id)))) return 0;
        *e = ev0(OSC_EV_REGION_DESTROY); e->region = r; return 1;
    case 14:
        if (!(r = PICK(s, NS, s->sst[id] == SL_FREE)) || !(f = fresh(s->nhnd, NH, reserve))) return 0;
        *e = ev0(OSC_EV_SLOT_ALLOC); e->slot = r; e->handle = f; e->rights = rnd(s, 256); return 1;
    case 15:
        if (!(h = PICK(s, s->nhnd, hnd_valid(s, id)))) return 0;
        *e = ev0(OSC_EV_SLOT_FREE); e->handle = h; return 1;
    case 16:
        if (!(h = PICK(s, s->nhnd, hnd_valid(s, id))) || !(f = fresh(s->nhnd, NH, reserve))) return 0;
        *e = ev0(OSC_EV_HANDLE_DERIVE); e->handle = f; e->handle2 = h; e->rights = s->hrig[h] & rnd(s, 256); return 1;
    case 17:
        if (!(h = PICK(s, s->nhnd, hnd_valid(s, id)))) return 0;
        *e = ev0(OSC_EV_HANDLE_USE); e->handle = h; e->rights = s->hrig[h] & rnd(s, 256); return 1;
    case 18:
        if (!(h = PICK(s, s->nhnd, hnd_valid(s, id))) || !(f = fresh(s->nsem, NM, reserve))) return 0;
        *e = ev0(OSC_EV_PERSIST); e->handle = h; e->semid = f; return 1;
    case 19:
        *e = ev0(OSC_EV_DURABLE_WRITE);
        if (rnd(s, 2) && s->nsem) { e->vkind = OSC_VK_SEMID; e->id = 1 + rnd(s, s->nsem); return 1; }
        if (!(x = PICK(s, s->nobj, obj_readable(s, id)))) return 0;
        e->vkind = OSC_VK_OBJECT; e->id = x; return 1;
    case 20:
        c = rnd(s, 3) ? PICK(s, s->ncell, s->cst[id] != CE_GONE) : 0;
        if (!c && !(c = fresh(s->ncell, NC, reserve))) return 0;
        *e = ev0(OSC_EV_CELL_WRITE); e->cell = c; return 1;
    case 21:
        if (!(c = PICK(s, s->ncell, s->cst[id] == CE_WR))) return 0;
        *e = ev0(OSC_EV_CELL_PUBLISH); e->cell = c; e->order = OSC_ORD_RELEASE; return 1;
    case 22:
        if (!(c = PICK(s, s->ncell, s->cst[id] == CE_PUB))) return 0;
        *e = ev0(OSC_EV_CELL_OBSERVE); e->cell = c; e->order = OSC_ORD_ACQUIRE; return 1;
    case 23:
        if (!(c = PICK(s, s->ncell, s->cst[id] == CE_OBS))) return 0;
        *e = ev0(OSC_EV_CELL_READ); e->cell = c; return 1;
    case 24:
        if (!(c = PICK(s, s->ncell, s->cst[id] == CE_OBS)) || !(f = fresh(s->ntok, NT, reserve))) return 0;
        *e = ev0(OSC_EV_QUIESCE); e->cell = c; e->token = f; return 1;
    case 25:
        if (!(t = PICK(s, s->ntok, s->tst[id] == TK_LIVE && s->cst[s->tcell[id]] != CE_GONE))) return 0;
        *e = ev0(OSC_EV_RECLAIM); e->cell = s->tcell[t]; e->token = t; return 1;
    }
    return 0;
}

/* one random valid step; 0 = emitted, 1 = nothing possible, -1 = full */
static int valid_step(Sh *s)
{
    OscModelEvent e;
    for (int tries = 0; tries < 16; tries++)
        if (build_valid(s, (int)rnd(s, N_ACTIONS), RESERVE, &e)) return emit(s, &e);
    for (int a = 0; a < N_ACTIONS; a++)
        if (build_valid(s, a, RESERVE, &e)) return emit(s, &e);
    return 1;
}

/* Valid teardown: end borrows leaf first, release / consume owners, destroy
 * regions, free live slots, drain and reclaim observed cells. */
static int teardown(Sh *s)
{
    OscModelEvent e;
    for (;;) {
        uint32_t b = PICK(s, s->nbor, bor_leaf(s, id));
        if (!b) break;
        e = ev0(OSC_EV_END_BORROW); e.borrow = b;
        if (emit(s, &e)) return -1;
    }
    for (uint32_t x = s->nobj; x >= 1; x--) {
        if (s->ost[x] != OB_OWN || (s->oreg[x] && rnd(s, 2))) continue;
        e = ev0(rnd(s, 5) ? OSC_EV_RELEASE : OSC_EV_MOVE); e.obj = x;
        if (emit(s, &e)) return -1;
    }
    for (uint32_t r = 1; r <= s->nreg; r++) {
        if (s->rst[r] != RG_OPEN) continue;
        e = ev0(OSC_EV_REGION_DESTROY); e.region = r;
        if (emit(s, &e)) return -1;
    }
    for (uint32_t h = 1; h <= s->nhnd; h++) {
        if (!hnd_valid(s, h)) continue;
        e = ev0(OSC_EV_SLOT_FREE); e.handle = h;
        if (emit(s, &e)) return -1;
    }
    for (uint32_t c = 1; c <= s->ncell; c++) {
        if (s->cst[c] != CE_OBS) continue;
        uint32_t t = fresh(s->ntok, NT, 0);
        if (!t) break;
        e = ev0(OSC_EV_QUIESCE); e.cell = c; e.token = t;
        if (emit(s, &e)) return -1;
        e = ev0(OSC_EV_RECLAIM); e.cell = c; e.token = t;
        if (emit(s, &e)) return -1;
    }
    return 0;
}

/* ---- injection setup helpers (valid events, may use the reserve) ---- */
#define EMIT(ev) do { if (emit(s, &(ev))) return -1; } while (0)

static uint32_t new_obj(Sh *s, uint32_t region)
{
    uint32_t x = fresh(s->nobj, NO, 0);
    if (!x) return 0;
    OscModelEvent e = ev0(OSC_EV_ALLOC); e.obj = x; e.region = region;
    return emit(s, &e) ? 0 : x;
}

static uint32_t new_borrow(Sh *s, uint32_t x, uint32_t via, int mut)
{
    uint32_t b = fresh(s->nbor, NB, 0);
    if (!b) return 0;
    OscModelEvent e = ev0(mut ? OSC_EV_BORROW_MUT : OSC_EV_BORROW_SHARED);
    e.obj = x; e.borrow = b; e.via = via;
    return emit(s, &e) ? 0 : b;
}

/* an owned object that has a live root borrow of the given kind
 * (want: 0 = shared, 1 = mut, 2 = any); creates one if none exists */
static uint32_t obj_with_root_borrow(Sh *s, int want)
{
    uint32_t x = PICK(s, s->nobj, s->ost[id] == OB_OWN &&
                      (want == 0 ? s->osh[id] > 0 : want == 1 ? s->omu[id] > 0 : (s->osh[id] + s->omu[id]) > 0));
    if (x && rnd(s, 4)) return x;
    int mut = want == 2 ? (int)rnd(s, 2) : want;
    x = mut ? PICK(s, s->nobj, obj_free_of_borrows(s, id)) : PICK(s, s->nobj, obj_readable(s, id));
    if (!x || !rnd(s, 2)) x = new_obj(s, 0);
    if (!x || !new_borrow(s, x, 0, mut)) return 0;
    return x;
}

/* a live mut borrow with a live child of the given kind (mut_kid) */
static uint32_t mut_borrow_with_kid(Sh *s, int mut_kid)
{
    uint32_t p = PICK(s, s->nbor, bor_live(s, id) && s->bmut[id] && (mut_kid ? s->bkmu[id] > 0 : s->bksh[id] > 0));
    if (p && rnd(s, 4)) return p;
    p = PICK(s, s->nbor, bor_leaf(s, id) && s->bmut[id]);
    if (!p || !rnd(s, 2)) {
        uint32_t x = new_obj(s, 0);
        if (!x || !(p = new_borrow(s, x, 0, 1))) return 0;
    }
    if (!new_borrow(s, s->bobj[p], p, mut_kid)) return 0;
    return p;
}

static uint32_t valid_handle(Sh *s)
{
    uint32_t h = PICK(s, s->nhnd, hnd_valid(s, id));
    if (h) return h;
    uint32_t sl = PICK(s, NS, s->sst[id] == SL_FREE), f = fresh(s->nhnd, NH, 0);
    if (!sl || !f) return 0;
    OscModelEvent e = ev0(OSC_EV_SLOT_ALLOC); e.slot = sl; e.handle = f; e.rights = rnd(s, 256);
    return emit(s, &e) ? 0 : f;
}

static uint32_t stale_handle(Sh *s)
{
    uint32_t h = PICK(s, s->nhnd, s->hused[id] && !hnd_valid(s, id));
    if (h && rnd(s, 4)) return h;
    h = valid_handle(s);
    if (!h) return PICK(s, s->nhnd, s->hused[id] && !hnd_valid(s, id));
    OscModelEvent e = ev0(OSC_EV_SLOT_FREE); e.handle = h;
    if (emit(s, &e)) return 0;
    return h;
}

/* an existing, not reclaimed cell in the requested state set (bitmask over
 * CE_*), created and advanced if none */
static uint32_t cell_in(Sh *s, unsigned states)
{
    uint32_t c = PICK(s, s->ncell, (states >> s->cst[id]) & 1u);
    if (c && rnd(s, 4)) return c;
    if (!(c = fresh(s->ncell, NC, 0))) return PICK(s, s->ncell, (states >> s->cst[id]) & 1u);
    OscModelEvent e = ev0(OSC_EV_CELL_WRITE); e.cell = c;
    if (emit(s, &e)) return 0;
    /* advance to a random allowed state */
    int target;
    do target = CE_WR + (int)rnd(s, 3); while (!((states >> target) & 1u));
    if (target >= CE_PUB) { e = ev0(OSC_EV_CELL_PUBLISH); e.cell = c; e.order = OSC_ORD_RELEASE; if (emit(s, &e)) return 0; }
    if (target >= CE_OBS) { e = ev0(OSC_EV_CELL_OBSERVE); e.cell = c; e.order = OSC_ORD_ACQUIRE; if (emit(s, &e)) return 0; }
    return c;
}

/* a cell that has been reclaimed (creates + drains + reclaims a fresh one) */
static uint32_t reclaimed_cell(Sh *s)
{
    uint32_t c = PICK(s, s->ncell, s->cst[id] == CE_GONE);
    if (c && rnd(s, 2)) return c;
    uint32_t t = fresh(s->ntok, NT, 0);
    if (!t || !(c = cell_in(s, 1u << CE_OBS))) return PICK(s, s->ncell, s->cst[id] == CE_GONE);
    OscModelEvent e = ev0(OSC_EV_QUIESCE); e.cell = c; e.token = t;
    if (emit(s, &e)) return 0;
    e = ev0(OSC_EV_RECLAIM); e.cell = c; e.token = t;
    if (emit(s, &e)) return 0;
    return c;
}

/* Build the single invalid event of class k. Setup events are emitted as
 * valid steps. Returns 0 and fills *bad, or -1. */
static int build_bad(Sh *s, OscModelReject k, OscModelEvent *bad)
{
    OscModelEvent e;
    uint32_t x, b, r, h, c, t, v;
    switch (k) {
    case OSC_REJ_USE_AFTER_MOVE:
        x = PICK(s, s->nobj, s->ost[id] == OB_MOVED);
        if (!x || !rnd(s, 3)) {
            x = PICK(s, s->nobj, obj_free_of_borrows(s, id));
            if (!x) x = new_obj(s, 0);
            if (!x) return -1;
            e = ev0(OSC_EV_MOVE); e.obj = x; e.obj2 = rnd(s, 2) ? fresh(s->nobj, NO, 0) : 0; EMIT(e);
        }
        v = rnd(s, 7); s->q->variant = (int)v;
        static const uint32_t uam_k[7] = { OSC_EV_USE_READ, OSC_EV_USE_WRITE, OSC_EV_BORROW_SHARED,
                                           OSC_EV_BORROW_MUT, OSC_EV_MOVE, OSC_EV_RELEASE, OSC_EV_DURABLE_WRITE };
        *bad = ev0(uam_k[v]); bad->obj = x;
        if (v == 2 || v == 3) bad->borrow = fresh(s->nbor, NB, 0);
        if (v == 4) bad->obj2 = rnd(s, 2) ? fresh(s->nobj, NO, 0) : 0;
        if (v == 6) { bad->obj = 0; bad->vkind = OSC_VK_OBJECT; bad->id = x; }
        return 0;

    case OSC_REJ_USE_AFTER_RELEASE:
        v = rnd(s, 4); s->q->variant = (int)v;
        if (v == 0) {          /* released object */
            x = PICK(s, s->nobj, s->ost[id] == OB_REL);
            if (!x || !rnd(s, 3)) {
                if (!(x = new_obj(s, 0))) return -1;
                e = ev0(OSC_EV_RELEASE); e.obj = x; EMIT(e);
            }
            static const uint32_t uar_k[6] = { OSC_EV_USE_READ, OSC_EV_USE_WRITE, OSC_EV_BORROW_SHARED,
                                               OSC_EV_BORROW_MUT, OSC_EV_MOVE, OSC_EV_DURABLE_WRITE };
            uint32_t w = rnd(s, 6);
            *bad = ev0(uar_k[w]); bad->obj = x;
            if (w == 2 || w == 3) bad->borrow = fresh(s->nbor, NB, 0);
            if (w == 5) { bad->obj = 0; bad->vkind = OSC_VK_OBJECT; bad->id = x; }
        } else if (v == 1) {   /* use through an ended borrow */
            b = PICK(s, s->nbor, s->bst[id] == BR_END);
            if (!b || !rnd(s, 3)) {
                x = PICK(s, s->nobj, obj_readable(s, id));
                if (!x && !(x = new_obj(s, 0))) return -1;
                if (!(b = new_borrow(s, x, 0, 0))) return -1;
                e = ev0(OSC_EV_END_BORROW); e.borrow = b; EMIT(e);
            }
            static const uint32_t ub_k[4] = { OSC_EV_USE_READ, OSC_EV_USE_WRITE, OSC_EV_BORROW_SHARED, OSC_EV_BORROW_MUT };
            uint32_t w = rnd(s, 4);
            *bad = ev0(ub_k[w]); bad->obj = s->bobj[b]; bad->via = b;
            if (w >= 2) bad->borrow = fresh(s->nbor, NB, 0);
        } else if (v == 2) {   /* alloc into a destroyed region */
            r = PICK(s, s->nreg, s->rst[id] == RG_DEAD);
            if (!r) {
                if (!(r = fresh(s->nreg, NR, 0))) return -1;
                e = ev0(OSC_EV_REGION_OPEN); e.region = r; EMIT(e);
                if (rnd(s, 2) && !new_obj(s, r)) return -1;
                e = ev0(OSC_EV_REGION_DESTROY); e.region = r; EMIT(e);
            }
            *bad = ev0(OSC_EV_ALLOC); bad->obj = fresh(s->nobj, NO, 0); bad->region = r;
        } else {               /* reclaimed cell */
            if (!(c = reclaimed_cell(s))) return -1;
            static const uint32_t rc_k[5] = { OSC_EV_CELL_WRITE, OSC_EV_CELL_PUBLISH, OSC_EV_CELL_OBSERVE,
                                              OSC_EV_CELL_READ, OSC_EV_QUIESCE };
            uint32_t w = rnd(s, 5);
            *bad = ev0(rc_k[w]); bad->cell = c;
            bad->order = w == 1 ? OSC_ORD_RELEASE : OSC_ORD_ACQUIRE;
            if (w == 4) bad->token = fresh(s->ntok, NT, 0);
        }
        return 0;

    case OSC_REJ_DOUBLE_RELEASE:
        v = rnd(s, 4); s->q->variant = (int)v;
        if (v == 0) {
            x = PICK(s, s->nobj, s->ost[id] == OB_REL);
            if (!x || !rnd(s, 3)) {
                if (!(x = new_obj(s, 0))) return -1;
                e = ev0(OSC_EV_RELEASE); e.obj = x; EMIT(e);
            }
            *bad = ev0(OSC_EV_RELEASE); bad->obj = x;
        } else if (v == 1) {
            b = PICK(s, s->nbor, s->bst[id] == BR_END);
            if (!b || !rnd(s, 3)) {
                x = PICK(s, s->nobj, obj_readable(s, id));
                if (!x && !(x = new_obj(s, 0))) return -1;
                if (!(b = new_borrow(s, x, 0, 0))) return -1;
                e = ev0(OSC_EV_END_BORROW); e.borrow = b; EMIT(e);
            }
            *bad = ev0(OSC_EV_END_BORROW); bad->borrow = b;
        } else if (v == 2) {
            r = PICK(s, s->nreg, s->rst[id] == RG_DEAD);
            if (!r) {
                if (!(r = fresh(s->nreg, NR, 0))) return -1;
                e = ev0(OSC_EV_REGION_OPEN); e.region = r; EMIT(e);
                e = ev0(OSC_EV_REGION_DESTROY); e.region = r; EMIT(e);
            }
            *bad = ev0(OSC_EV_REGION_DESTROY); bad->region = r;
        } else {
            if (!(c = reclaimed_cell(s))) return -1;
            *bad = ev0(OSC_EV_RECLAIM); bad->cell = c;
            bad->token = rnd(s, 2) ? PICK(s, s->ntok, s->tst[id] != TK_NONE) : 0;
        }
        return 0;

    case OSC_REJ_STALE_GENERATION:
        if (!(h = stale_handle(s))) return -1;
        v = rnd(s, 5); s->q->variant = (int)v;
        if (v == 0) { *bad = ev0(OSC_EV_HANDLE_USE); bad->handle = h; bad->rights = s->hrig[h] & rnd(s, 256); }
        else if (v == 1) { *bad = ev0(OSC_EV_HANDLE_DERIVE); bad->handle = fresh(s->nhnd, NH, 0); bad->handle2 = h;
                           bad->rights = s->hrig[h] & rnd(s, 256); }
        else if (v == 2) { *bad = ev0(OSC_EV_SLOT_FREE); bad->handle = h; }
        else if (v == 3) { *bad = ev0(OSC_EV_PERSIST); bad->handle = h; bad->semid = fresh(s->nsem, NM, 0); }
        else { *bad = ev0(OSC_EV_SLOT_FREE); bad->slot = s->hslot[h]; bad->gen = s->hgen[h]; }
        return 0;

    case OSC_REJ_GENERATION_WRAP:
        s->q->variant = 0;
        r = PICK(s, NS, s->sst[id] == SL_RET);
        if (!r) {
            /* drive one slot to retirement: free it if live, then alloc/free */
            h = PICK(s, s->nhnd, hnd_valid(s, id));
            r = h ? s->hslot[h] : PICK(s, NS, s->sst[id] == SL_FREE);
            if (!r) return -1;
            if (s->sst[r] == SL_LIVE) {
                h = PICK(s, s->nhnd, hnd_valid(s, id) && s->hslot[id] == r);
                if (!h) return -1;
                e = ev0(OSC_EV_SLOT_FREE); e.handle = h; EMIT(e);
            }
            for (int i = 0; i < 8 && s->sst[r] != SL_RET; i++) {
                if (!(h = fresh(s->nhnd, NH, 0))) return -1;
                e = ev0(OSC_EV_SLOT_ALLOC); e.slot = r; e.handle = h; e.rights = rnd(s, 256); EMIT(e);
                e = ev0(OSC_EV_SLOT_FREE); e.handle = h; EMIT(e);
            }
            if (s->sst[r] != SL_RET) return -1;
            s->q->variant = 1;
        }
        *bad = ev0(OSC_EV_SLOT_ALLOC); bad->slot = r;
        bad->handle = rnd(s, 2) ? fresh(s->nhnd, NH, 0) : 0; bad->rights = rnd(s, 256);
        return 0;

    case OSC_REJ_MUTABLE_ALIAS:
        v = rnd(s, 10); s->q->variant = (int)v;
        switch (v) {
        case 0: if (!(x = obj_with_root_borrow(s, 0))) return -1;
                *bad = ev0(OSC_EV_BORROW_MUT); bad->obj = x; bad->borrow = fresh(s->nbor, NB, 0); return 0;
        case 1: if (!(x = obj_with_root_borrow(s, 1))) return -1;
                *bad = ev0(OSC_EV_BORROW_SHARED); bad->obj = x; bad->borrow = fresh(s->nbor, NB, 0); return 0;
        case 2: if (!(x = obj_with_root_borrow(s, 1))) return -1;
                *bad = ev0(OSC_EV_BORROW_MUT); bad->obj = x; bad->borrow = fresh(s->nbor, NB, 0); return 0;
        case 3: if (!(x = obj_with_root_borrow(s, 2))) return -1;
                *bad = ev0(OSC_EV_USE_WRITE); bad->obj = x; return 0;
        case 4: if (!(x = obj_with_root_borrow(s, 1))) return -1;
                if (rnd(s, 2)) { *bad = ev0(OSC_EV_USE_READ); bad->obj = x; }
                else { *bad = ev0(OSC_EV_DURABLE_WRITE); bad->vkind = OSC_VK_OBJECT; bad->id = x; }
                return 0;
        case 5: if (!(x = obj_with_root_borrow(s, 2))) return -1;
                *bad = ev0(OSC_EV_MOVE); bad->obj = x; bad->obj2 = rnd(s, 2) ? fresh(s->nobj, NO, 0) : 0; return 0;
        case 6: case 7: {  /* parent &mut with a live &mut child */
            if (!(b = mut_borrow_with_kid(s, 1))) return -1;
            static const uint32_t pk[4] = { OSC_EV_USE_READ, OSC_EV_USE_WRITE, OSC_EV_BORROW_SHARED, OSC_EV_BORROW_MUT };
            uint32_t w = rnd(s, 4);
            *bad = ev0(pk[w]); bad->obj = s->bobj[b]; bad->via = b;
            if (w >= 2) bad->borrow = fresh(s->nbor, NB, 0);
            return 0;
        }
        default: {         /* parent &mut with a live & child */
            if (!(b = mut_borrow_with_kid(s, 0))) return -1;
            *bad = ev0(rnd(s, 2) ? OSC_EV_USE_WRITE : OSC_EV_BORROW_MUT); bad->obj = s->bobj[b]; bad->via = b;
            if (bad->kind == OSC_EV_BORROW_MUT) bad->borrow = fresh(s->nbor, NB, 0);
            return 0;
        }
        }

    case OSC_REJ_BORROW_OUTLIVES_OWNER:
        v = rnd(s, 2); s->q->variant = (int)v;
        if (v == 0) {
            if (!(x = obj_with_root_borrow(s, 2))) return -1;
            *bad = ev0(OSC_EV_RELEASE); bad->obj = x;
        } else {
            b = PICK(s, s->nbor, bor_live(s, id) && (s->bksh[id] + s->bkmu[id]) > 0);
            if (!b || !rnd(s, 3)) {
                int pm = (int)rnd(s, 2);
                uint32_t p = PICK(s, s->nbor, pm ? (bor_leaf(s, id) && s->bmut[id]) : (bor_live(s, id) && !s->bmut[id]));
                if (!p) {
                    if (!(x = new_obj(s, 0)) || !(p = new_borrow(s, x, 0, pm))) return -1;
                }
                if (!new_borrow(s, s->bobj[p], p, s->bmut[p] ? (int)rnd(s, 2) : 0)) return -1;
                b = p;
            }
            *bad = ev0(OSC_EV_END_BORROW); bad->borrow = b;
        }
        return 0;

    case OSC_REJ_ARENA_ESCAPE:
        s->q->variant = 0;
        r = PICK(s, s->nreg, s->rst[id] == RG_OPEN && !region_destroyable(s, id));
        if (!r || !rnd(s, 3)) {
            r = PICK(s, s->nreg, s->rst[id] == RG_OPEN);
            if (!r || !rnd(s, 2)) {
                if (!(r = fresh(s->nreg, NR, 0))) return -1;
                e = ev0(OSC_EV_REGION_OPEN); e.region = r; EMIT(e);
            }
            uint32_t via = 0;
            int mut = (int)rnd(s, 2);
            if (!(x = new_obj(s, r))) return -1;
            if (rnd(s, 3) == 0) {   /* reborrow chain into the region */
                if (!(via = new_borrow(s, x, 0, 1))) return -1;
                s->q->variant = 2;
            } else s->q->variant = 1;
            if (!new_borrow(s, x, via, mut)) return -1;
        }
        *bad = ev0(OSC_EV_REGION_DESTROY); bad->region = r;
        return 0;

    case OSC_REJ_RECLAIM_WITHOUT_QUIESCED:
        v = rnd(s, 3); s->q->variant = (int)v;
        if (v == 2 && !PICK(s, s->ntok, s->tst[id] == TK_USED) && !reclaimed_cell(s)) return -1;
        if (!(c = cell_in(s, (1u << CE_WR) | (1u << CE_PUB) | (1u << CE_OBS)))) return -1;
        *bad = ev0(OSC_EV_RECLAIM); bad->cell = c;
        if (v == 0) { bad->token = 0; return 0; }
        if (v == 1) {   /* a live token for another cell */
            t = PICK(s, s->ntok, s->tst[id] == TK_LIVE && s->tcell[id] != c);
            if (!t) {
                uint32_t c2 = fresh(s->ncell, NC, 0);
                if (!c2 || !(t = fresh(s->ntok, NT, 0))) return -1;
                e = ev0(OSC_EV_CELL_WRITE); e.cell = c2; EMIT(e);
                e = ev0(OSC_EV_CELL_PUBLISH); e.cell = c2; e.order = OSC_ORD_RELEASE; EMIT(e);
                e = ev0(OSC_EV_CELL_OBSERVE); e.cell = c2; e.order = OSC_ORD_ACQUIRE; EMIT(e);
                e = ev0(OSC_EV_QUIESCE); e.cell = c2; e.token = t; EMIT(e);
            }
            bad->token = t; return 0;
        }
        /* a consumed token */
        t = PICK(s, s->ntok, s->tst[id] == TK_USED);
        if (!t) {
            uint32_t c2 = reclaimed_cell(s);
            if (!c2) return -1;
            t = PICK(s, s->ntok, s->tst[id] == TK_USED);
            if (!t) return -1;
        }
        bad->token = t; return 0;

    case OSC_REJ_READ_BEFORE_OBSERVE:
        v = rnd(s, 2); s->q->variant = (int)v;
        if (v == 0) {
            if (!(c = cell_in(s, (1u << CE_WR) | (1u << CE_PUB)))) return -1;
            *bad = ev0(OSC_EV_CELL_READ); bad->cell = c;
        } else {
            if (!(c = cell_in(s, (1u << CE_WR) | (1u << CE_PUB) | (1u << CE_OBS)))) return -1;
            *bad = ev0(OSC_EV_CELL_OBSERVE); bad->cell = c;
            bad->order = rnd(s, 2) ? OSC_ORD_RELAXED : OSC_ORD_RELEASE;
        }
        return 0;

    case OSC_REJ_PUBLISH_WITHOUT_RELEASE:
        s->q->variant = 0;
        if (!(c = cell_in(s, (1u << CE_WR) | (1u << CE_PUB) | (1u << CE_OBS)))) return -1;
        *bad = ev0(OSC_EV_CELL_PUBLISH); bad->cell = c;
        bad->order = rnd(s, 2) ? OSC_ORD_RELAXED : OSC_ORD_ACQUIRE;
        return 0;

    case OSC_REJ_FORGED_RIGHTS:
        v = rnd(s, 4); s->q->variant = (int)v;
        if (v <= 1) {
            if (!(h = valid_handle(s))) v = 2 + rnd(s, 2), s->q->variant = (int)v;
            else {
                uint64_t missing = ~s->hrig[h] & 0x1ffull;   /* bit 8 is never granted */
                uint64_t extra = 0;
                while (!extra) extra = missing & ((uint64_t)rnd(s, 512) | (1ull << 8));
                uint64_t want = (s->hrig[h] & rnd(s, 256)) | extra;
                if (v == 0) { *bad = ev0(OSC_EV_HANDLE_DERIVE); bad->handle = fresh(s->nhnd, NH, 0); bad->handle2 = h; }
                else        { *bad = ev0(OSC_EV_HANDLE_USE); bad->handle = h; }
                bad->rights = want;
                return 0;
            }
        }
        /* write or &mut through a live shared borrow */
        b = PICK(s, s->nbor, bor_live(s, id) && !s->bmut[id]);
        if (!b || !rnd(s, 3)) {
            x = PICK(s, s->nobj, obj_readable(s, id));
            if (!x && !(x = new_obj(s, 0))) return -1;
            if (!(b = new_borrow(s, x, 0, 0))) return -1;
        }
        *bad = ev0(v == 2 ? OSC_EV_USE_WRITE : OSC_EV_BORROW_MUT); bad->obj = s->bobj[b]; bad->via = b;
        if (v == 3) bad->borrow = fresh(s->nbor, NB, 0);
        return 0;

    case OSC_REJ_LIVE_HANDLE_DURABLE:
        v = rnd(s, 2); s->q->variant = (int)v;
        *bad = ev0(OSC_EV_DURABLE_WRITE);
        if (v == 0) {
            h = PICK(s, s->nhnd, s->hused[id]);
            if (!h || !rnd(s, 3)) h = valid_handle(s);
            if (!h) h = PICK(s, s->nhnd, s->hused[id]);
            if (!h) return -1;
            bad->vkind = OSC_VK_HANDLE; bad->id = h;
        } else {
            b = PICK(s, s->nbor, s->bst[id] != BR_NONE);
            if (!b || !rnd(s, 3)) {
                x = PICK(s, s->nobj, obj_readable(s, id));
                if (!x && !(x = new_obj(s, 0))) return -1;
                if (!(b = new_borrow(s, x, 0, 0))) return -1;
            }
            bad->vkind = OSC_VK_BORROW; bad->id = b;
        }
        return 0;

    default:
        return -1;
    }
}

int osc_gen_sequence(uint64_t seed, uint64_t index, OscModelReject inject_class, OscGenSeq *out)
{
    static Sh zero;
    Sh sh = zero;
    Sh *s = &sh;
    s->q = out;
    out->n = 0; out->inject_at = -1; out->expect = OSC_REJ_NONE; out->variant = -1;
    s->rng = seed ^ (0x9E3779B97F4A7C15ull * (index + 1));
    (void)osc_gen_splitmix64(&s->rng);

    /* generation configuration */
    uint32_t cfg = rnd(s, 4);
    if (inject_class == OSC_REJ_GENERATION_WRAP && cfg == 0) cfg = 1 + rnd(s, 3);
    switch (cfg) {
    case 0:  out->gen_base = 0; out->gen_max = UINT64_MAX; break;
    case 1:  out->gen_base = 0; out->gen_max = 1 + rnd(s, 3); break;
    case 2:  out->gen_base = UINT64_MAX - 1 - rnd(s, 2); out->gen_max = UINT64_MAX; break;
    default: out->gen_base = 5; out->gen_max = 5 + rnd(s, 2); break;
    }
    s->gmax = out->gen_max;
    for (uint32_t i = 1; i <= NS; i++) { s->sst[i] = SL_FREE; s->sgen[i] = out->gen_base; }

    if (inject_class == OSC_REJ_NONE) {
        uint32_t len = 4 + rnd(s, 40);
        for (uint32_t i = 0; i < len; i++) {
            int r = valid_step(s);
            if (r < 0) return -1;
            if (r > 0) break;
        }
        if (rnd(s, 2) && teardown(s)) return -1;
        return 0;
    }

    uint32_t prefix = rnd(s, 31);
    for (uint32_t i = 0; i < prefix; i++) {
        int r = valid_step(s);
        if (r < 0) return -1;
        if (r > 0) break;
    }
    OscModelEvent bad;
    if (build_bad(s, inject_class, &bad)) return -1;
    if (out->n >= OSC_GEN_MAX_EVENTS - 1) return -1;
    out->inject_at = out->n;
    out->expect = inject_class;
    out->ev[out->n++] = bad;          /* not applied to the shadow */
    uint32_t suffix = rnd(s, 6);
    for (uint32_t i = 0; i < suffix; i++) {
        int r = valid_step(s);
        if (r < 0) return -1;
        if (r > 0) break;
    }
    return 0;
}
