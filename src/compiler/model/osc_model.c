/*
 * osc_model.c -- II.11 executable reference model (see osc_model.h).
 * Every handler validates completely before it mutates, so a rejected event
 * leaves the state unchanged.
 */
#include "osc_model.h"

#include <string.h>

enum { O_UNALLOC = 0, O_OWNED, O_MOVED, O_RELEASED };
enum { B_UNUSED = 0, B_LIVE, B_ENDED };
enum { R_UNUSED = 0, R_OPEN, R_DESTROYED };
enum { S_FREE = 0, S_LIVE, S_RETIRED };
enum { C_UNUSED = 0, C_WRITTEN, C_PUBLISHED, C_OBSERVED, C_RECLAIMED };
enum { T_UNUSED = 0, T_LIVE, T_CONSUMED };

static const char *const k_reject_names[OSC_REJ__COUNT] = {
    "accept",
    "use-after-move",
    "use-after-release",
    "double-release",
    "stale-generation",
    "generation-wrap",
    "mutable-alias",
    "borrow-outlives-owner",
    "arena-escape",
    "reclaim-without-quiesced",
    "read-before-observe",
    "publish-without-release",
    "forged-rights",
    "live-handle-durable",
    "malformed",
    "capacity",
    "protocol",
};

static const char *const k_event_names[OSC_EV__COUNT] = {
    "?", "alloc", "move", "borrow_shared", "borrow_mut", "end_borrow",
    "use_read", "use_write", "release", "region_open", "region_destroy",
    "slot_alloc", "slot_free", "handle_derive", "handle_use", "persist",
    "durable_write", "cell_write", "cell_publish", "cell_observe", "cell_read",
    "quiesce", "reclaim",
};

const char *osc_model_reject_name(OscModelReject r)
{
    if ((unsigned)r >= OSC_REJ__COUNT) return "?";
    return k_reject_names[r];
}

const char *osc_model_event_name(uint32_t kind)
{
    if (kind == 0 || kind >= OSC_EV__COUNT) return "?";
    return k_event_names[kind];
}

void osc_model_init(OscModel *m, uint64_t gen_base, uint64_t gen_max)
{
    memset(m, 0, sizeof *m);
    if (gen_base > gen_max) gen_base = gen_max;
    m->gen_base = gen_base;
    m->gen_max = gen_max;
    for (uint32_t s = 1; s <= OSC_MODEL_MAX_SLOTS; s++) {
        m->slot[s].state = S_FREE;
        m->slot[s].gen = gen_base;
    }
}

/* Id checks. "ref": the id must already exist. "fresh": it must be unused. */
#define IDCHK(id, cap) do { if ((id) == 0) return OSC_REJ_MALFORMED; \
                            if ((id) > (cap)) return OSC_REJ_CAPACITY; } while (0)

static OscModelReject obj_ref(const OscModel *m, uint32_t id)
{
    IDCHK(id, OSC_MODEL_MAX_OBJECTS);
    return m->obj[id].state == O_UNALLOC ? OSC_REJ_MALFORMED : OSC_REJ_NONE;
}
static OscModelReject obj_fresh(const OscModel *m, uint32_t id)
{
    IDCHK(id, OSC_MODEL_MAX_OBJECTS);
    return m->obj[id].state != O_UNALLOC ? OSC_REJ_MALFORMED : OSC_REJ_NONE;
}
static OscModelReject bor_ref(const OscModel *m, uint32_t id)
{
    IDCHK(id, OSC_MODEL_MAX_BORROWS);
    return m->bor[id].state == B_UNUSED ? OSC_REJ_MALFORMED : OSC_REJ_NONE;
}
static OscModelReject bor_fresh(const OscModel *m, uint32_t id)
{
    IDCHK(id, OSC_MODEL_MAX_BORROWS);
    return m->bor[id].state != B_UNUSED ? OSC_REJ_MALFORMED : OSC_REJ_NONE;
}
static OscModelReject reg_ref(const OscModel *m, uint32_t id)
{
    IDCHK(id, OSC_MODEL_MAX_REGIONS);
    return m->region[id] == R_UNUSED ? OSC_REJ_MALFORMED : OSC_REJ_NONE;
}
static OscModelReject reg_fresh(const OscModel *m, uint32_t id)
{
    IDCHK(id, OSC_MODEL_MAX_REGIONS);
    return m->region[id] != R_UNUSED ? OSC_REJ_MALFORMED : OSC_REJ_NONE;
}
static OscModelReject slot_ref(uint32_t id)
{
    IDCHK(id, OSC_MODEL_MAX_SLOTS);
    return OSC_REJ_NONE;
}
static OscModelReject hnd_ref(const OscModel *m, uint32_t id)
{
    IDCHK(id, OSC_MODEL_MAX_HANDLES);
    return m->hnd[id].used ? OSC_REJ_NONE : OSC_REJ_MALFORMED;
}
static OscModelReject hnd_fresh(const OscModel *m, uint32_t id)
{
    IDCHK(id, OSC_MODEL_MAX_HANDLES);
    return m->hnd[id].used ? OSC_REJ_MALFORMED : OSC_REJ_NONE;
}
static OscModelReject cell_ref(const OscModel *m, uint32_t id)
{
    IDCHK(id, OSC_MODEL_MAX_CELLS);
    return m->cell[id].state == C_UNUSED ? OSC_REJ_MALFORMED : OSC_REJ_NONE;
}
static OscModelReject tok_ref(const OscModel *m, uint32_t id)
{
    IDCHK(id, OSC_MODEL_MAX_TOKENS);
    return m->tok[id].state == T_UNUSED ? OSC_REJ_MALFORMED : OSC_REJ_NONE;
}
static OscModelReject tok_fresh(const OscModel *m, uint32_t id)
{
    IDCHK(id, OSC_MODEL_MAX_TOKENS);
    return m->tok[id].state != T_UNUSED ? OSC_REJ_MALFORMED : OSC_REJ_NONE;
}
static OscModelReject sem_ref(const OscModel *m, uint32_t id)
{
    IDCHK(id, OSC_MODEL_MAX_SEMIDS);
    return m->semid[id] ? OSC_REJ_NONE : OSC_REJ_MALFORMED;
}
static OscModelReject sem_fresh(const OscModel *m, uint32_t id)
{
    IDCHK(id, OSC_MODEL_MAX_SEMIDS);
    return m->semid[id] ? OSC_REJ_MALFORMED : OSC_REJ_NONE;
}

#define TRY(expr) do { OscModelReject r_ = (expr); if (r_ != OSC_REJ_NONE) return r_; } while (0)

/* The owner itself must still be owned (direct use). */
static OscModelReject owner_usable(const OscModel *m, uint32_t obj)
{
    if (m->obj[obj].state == O_MOVED) return OSC_REJ_USE_AFTER_MOVE;
    if (m->obj[obj].state == O_RELEASED) return OSC_REJ_USE_AFTER_RELEASE;
    return OSC_REJ_NONE;
}

/* Optional `via` borrow: exists and refers to obj. */
static OscModelReject via_ref(const OscModel *m, uint32_t via, uint32_t obj)
{
    if (via == 0) return OSC_REJ_NONE;
    TRY(bor_ref(m, via));
    return m->bor[via].obj == obj ? OSC_REJ_NONE : OSC_REJ_MALFORMED;
}

static int hnd_stale(const OscModel *m, uint32_t h)
{
    const OscMSlot *s = &m->slot[m->hnd[h].slot];
    return s->state != S_LIVE || s->gen != m->hnd[h].gen;
}

/* Direct read of an owner (USE_READ via 0, DURABLE_WRITE of an object). */
static OscModelReject direct_read(const OscModel *m, uint32_t obj)
{
    TRY(owner_usable(m, obj));
    if (m->obj[obj].n_mut) return OSC_REJ_MUTABLE_ALIAS;
    return OSC_REJ_NONE;
}

static OscModelReject step(OscModel *m, const OscModelEvent *e)
{
    switch (e->kind) {
    case OSC_EV_ALLOC:
        TRY(obj_fresh(m, e->obj));
        if (e->region) {
            TRY(reg_ref(m, e->region));
            if (m->region[e->region] == R_DESTROYED) return OSC_REJ_USE_AFTER_RELEASE;
        }
        m->obj[e->obj].state = O_OWNED;
        m->obj[e->obj].region = e->region;
        return OSC_REJ_NONE;

    case OSC_EV_MOVE:
        TRY(obj_ref(m, e->obj));
        if (e->obj2) TRY(obj_fresh(m, e->obj2));
        TRY(owner_usable(m, e->obj));
        if (m->obj[e->obj].n_shared || m->obj[e->obj].n_mut) return OSC_REJ_MUTABLE_ALIAS;
        m->obj[e->obj].state = O_MOVED;
        if (e->obj2) {
            m->obj[e->obj2].state = O_OWNED;
            m->obj[e->obj2].region = m->obj[e->obj].region;
        }
        return OSC_REJ_NONE;

    case OSC_EV_BORROW_SHARED:
    case OSC_EV_BORROW_MUT: {
        int mut = e->kind == OSC_EV_BORROW_MUT;
        TRY(obj_ref(m, e->obj));
        TRY(bor_fresh(m, e->borrow));
        TRY(via_ref(m, e->via, e->obj));
        if (e->via == 0) {
            const OscMObj *o = &m->obj[e->obj];
            TRY(owner_usable(m, e->obj));
            if (o->n_mut) return OSC_REJ_MUTABLE_ALIAS;
            if (mut && o->n_shared) return OSC_REJ_MUTABLE_ALIAS;
        } else {
            const OscMBorrow *p = &m->bor[e->via];
            if (p->state == B_ENDED) return OSC_REJ_USE_AFTER_RELEASE;
            if (mut && !p->mut) return OSC_REJ_FORGED_RIGHTS;
            if (p->live_kids_mut) return OSC_REJ_MUTABLE_ALIAS;
            if (mut && p->live_kids_shared) return OSC_REJ_MUTABLE_ALIAS;
        }
        OscMBorrow *b = &m->bor[e->borrow];
        b->state = B_LIVE;
        b->mut = (uint8_t)mut;
        b->obj = e->obj;
        b->parent = e->via;
        b->live_kids_shared = b->live_kids_mut = 0;
        if (e->via) {
            if (mut) m->bor[e->via].live_kids_mut++; else m->bor[e->via].live_kids_shared++;
        } else {
            if (mut) m->obj[e->obj].n_mut++; else m->obj[e->obj].n_shared++;
        }
        return OSC_REJ_NONE;
    }

    case OSC_EV_END_BORROW: {
        TRY(bor_ref(m, e->borrow));
        OscMBorrow *b = &m->bor[e->borrow];
        if (b->state == B_ENDED) return OSC_REJ_DOUBLE_RELEASE;
        if (b->live_kids_shared || b->live_kids_mut) return OSC_REJ_BORROW_OUTLIVES_OWNER;
        b->state = B_ENDED;
        if (b->parent) {
            if (b->mut) m->bor[b->parent].live_kids_mut--; else m->bor[b->parent].live_kids_shared--;
        } else {
            if (b->mut) m->obj[b->obj].n_mut--; else m->obj[b->obj].n_shared--;
        }
        return OSC_REJ_NONE;
    }

    case OSC_EV_USE_READ:
    case OSC_EV_USE_WRITE: {
        int wr = e->kind == OSC_EV_USE_WRITE;
        TRY(obj_ref(m, e->obj));
        TRY(via_ref(m, e->via, e->obj));
        if (e->via == 0) {
            if (!wr) return direct_read(m, e->obj);
            TRY(owner_usable(m, e->obj));
            if (m->obj[e->obj].n_shared || m->obj[e->obj].n_mut) return OSC_REJ_MUTABLE_ALIAS;
            return OSC_REJ_NONE;
        }
        const OscMBorrow *b = &m->bor[e->via];
        if (b->state == B_ENDED) return OSC_REJ_USE_AFTER_RELEASE;
        if (!wr) return b->live_kids_mut ? OSC_REJ_MUTABLE_ALIAS : OSC_REJ_NONE;
        if (!b->mut) return OSC_REJ_FORGED_RIGHTS;
        if (b->live_kids_shared || b->live_kids_mut) return OSC_REJ_MUTABLE_ALIAS;
        return OSC_REJ_NONE;
    }

    case OSC_EV_RELEASE: {
        TRY(obj_ref(m, e->obj));
        OscMObj *o = &m->obj[e->obj];
        if (o->state == O_MOVED) return OSC_REJ_USE_AFTER_MOVE;
        if (o->state == O_RELEASED) return OSC_REJ_DOUBLE_RELEASE;
        if (o->n_shared || o->n_mut) return OSC_REJ_BORROW_OUTLIVES_OWNER;
        o->state = O_RELEASED;
        return OSC_REJ_NONE;
    }

    case OSC_EV_REGION_OPEN:
        TRY(reg_fresh(m, e->region));
        m->region[e->region] = R_OPEN;
        return OSC_REJ_NONE;

    case OSC_EV_REGION_DESTROY:
        TRY(reg_ref(m, e->region));
        if (m->region[e->region] == R_DESTROYED) return OSC_REJ_DOUBLE_RELEASE;
        for (uint32_t b = 1; b <= OSC_MODEL_MAX_BORROWS; b++)
            if (m->bor[b].state == B_LIVE && m->obj[m->bor[b].obj].region == e->region)
                return OSC_REJ_ARENA_ESCAPE;
        for (uint32_t o = 1; o <= OSC_MODEL_MAX_OBJECTS; o++)
            if (m->obj[o].state == O_OWNED && m->obj[o].region == e->region)
                m->obj[o].state = O_RELEASED;
        m->region[e->region] = R_DESTROYED;
        return OSC_REJ_NONE;

    case OSC_EV_SLOT_ALLOC: {
        TRY(slot_ref(e->slot));
        if (e->handle) TRY(hnd_fresh(m, e->handle));
        OscMSlot *s = &m->slot[e->slot];
        if (s->state == S_RETIRED) return OSC_REJ_GENERATION_WRAP;
        if (s->state == S_LIVE) return OSC_REJ_PROTOCOL;
        s->state = S_LIVE;
        if (e->handle) {
            OscMHandle *h = &m->hnd[e->handle];
            h->used = 1; h->slot = e->slot; h->gen = s->gen; h->rights = e->rights;
        }
        return OSC_REJ_NONE;
    }

    case OSC_EV_SLOT_FREE: {
        uint32_t sid; uint64_t g;
        if (e->handle) {
            TRY(hnd_ref(m, e->handle));
            sid = m->hnd[e->handle].slot; g = m->hnd[e->handle].gen;
        } else {
            TRY(slot_ref(e->slot));
            sid = e->slot; g = e->gen;
        }
        OscMSlot *s = &m->slot[sid];
        if (s->state != S_LIVE || s->gen != g) return OSC_REJ_STALE_GENERATION;
        if (s->gen == m->gen_max) s->state = S_RETIRED;
        else { s->state = S_FREE; s->gen++; }
        return OSC_REJ_NONE;
    }

    case OSC_EV_HANDLE_DERIVE: {
        TRY(hnd_fresh(m, e->handle));
        TRY(hnd_ref(m, e->handle2));
        if (hnd_stale(m, e->handle2)) return OSC_REJ_STALE_GENERATION;
        const OscMHandle *p = &m->hnd[e->handle2];
        if (e->rights & ~p->rights) return OSC_REJ_FORGED_RIGHTS;
        OscMHandle *h = &m->hnd[e->handle];
        h->used = 1; h->slot = p->slot; h->gen = p->gen; h->rights = e->rights;
        return OSC_REJ_NONE;
    }

    case OSC_EV_HANDLE_USE:
        TRY(hnd_ref(m, e->handle));
        if (hnd_stale(m, e->handle)) return OSC_REJ_STALE_GENERATION;
        if (e->rights & ~m->hnd[e->handle].rights) return OSC_REJ_FORGED_RIGHTS;
        return OSC_REJ_NONE;

    case OSC_EV_PERSIST:
        TRY(hnd_ref(m, e->handle));
        TRY(sem_fresh(m, e->semid));
        if (hnd_stale(m, e->handle)) return OSC_REJ_STALE_GENERATION;
        m->semid[e->semid] = 1;
        return OSC_REJ_NONE;

    case OSC_EV_DURABLE_WRITE:
        switch (e->vkind) {
        case OSC_VK_OBJECT: TRY(obj_ref(m, e->id)); return direct_read(m, e->id);
        case OSC_VK_SEMID:  return sem_ref(m, e->id);
        case OSC_VK_HANDLE: TRY(hnd_ref(m, e->id)); return OSC_REJ_LIVE_HANDLE_DURABLE;
        case OSC_VK_BORROW: TRY(bor_ref(m, e->id)); return OSC_REJ_LIVE_HANDLE_DURABLE;
        default:            return OSC_REJ_MALFORMED;
        }

    case OSC_EV_CELL_WRITE:
        IDCHK(e->cell, OSC_MODEL_MAX_CELLS);
        if (m->cell[e->cell].state == C_RECLAIMED) return OSC_REJ_USE_AFTER_RELEASE;
        m->cell[e->cell].state = C_WRITTEN;
        return OSC_REJ_NONE;

    case OSC_EV_CELL_PUBLISH:
        TRY(cell_ref(m, e->cell));
        if (m->cell[e->cell].state == C_RECLAIMED) return OSC_REJ_USE_AFTER_RELEASE;
        if (e->order != OSC_ORD_RELEASE) return OSC_REJ_PUBLISH_WITHOUT_RELEASE;
        if (m->cell[e->cell].state != C_WRITTEN) return OSC_REJ_PROTOCOL;
        m->cell[e->cell].state = C_PUBLISHED;
        return OSC_REJ_NONE;

    case OSC_EV_CELL_OBSERVE:
        TRY(cell_ref(m, e->cell));
        if (m->cell[e->cell].state == C_RECLAIMED) return OSC_REJ_USE_AFTER_RELEASE;
        if (e->order != OSC_ORD_ACQUIRE) return OSC_REJ_READ_BEFORE_OBSERVE;
        if (m->cell[e->cell].state != C_PUBLISHED) return OSC_REJ_PROTOCOL;
        m->cell[e->cell].state = C_OBSERVED;
        return OSC_REJ_NONE;

    case OSC_EV_CELL_READ:
        TRY(cell_ref(m, e->cell));
        if (m->cell[e->cell].state == C_RECLAIMED) return OSC_REJ_USE_AFTER_RELEASE;
        if (m->cell[e->cell].state != C_OBSERVED) return OSC_REJ_READ_BEFORE_OBSERVE;
        return OSC_REJ_NONE;

    case OSC_EV_QUIESCE:
        TRY(tok_fresh(m, e->token));
        TRY(cell_ref(m, e->cell));
        if (m->cell[e->cell].state == C_RECLAIMED) return OSC_REJ_USE_AFTER_RELEASE;
        if (m->cell[e->cell].state != C_OBSERVED) return OSC_REJ_PROTOCOL;
        m->tok[e->token].state = T_LIVE;
        m->tok[e->token].cell = e->cell;
        return OSC_REJ_NONE;

    case OSC_EV_RECLAIM:
        TRY(cell_ref(m, e->cell));
        if (e->token) TRY(tok_ref(m, e->token));
        if (m->cell[e->cell].state == C_RECLAIMED) return OSC_REJ_DOUBLE_RELEASE;
        if (e->token == 0 || m->tok[e->token].state != T_LIVE || m->tok[e->token].cell != e->cell)
            return OSC_REJ_RECLAIM_WITHOUT_QUIESCED;
        m->cell[e->cell].state = C_RECLAIMED;
        m->tok[e->token].state = T_CONSUMED;
        return OSC_REJ_NONE;

    default:
        return OSC_REJ_PROTOCOL;
    }
}

OscModelVerdict osc_model_step(OscModel *m, const OscModelEvent *ev, OscModelReject *out_reject)
{
    OscModelReject r = step(m, ev);
    if (out_reject) *out_reject = r;
    if (r == OSC_REJ_NONE) { m->accepted++; return OSC_MODEL_ACCEPT; }
    m->rejected++;
    return OSC_MODEL_REJECT;
}
