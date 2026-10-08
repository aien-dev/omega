/*
 * osc_check.c -- OSC-1 checker: names, types, literals, overflow-unsafe
 * constants, loop/return rules, ownership + borrows (lexical lifetimes).
 * See osc_check.h and docs/osc/OSC-1-DESIGN.md section 3.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#include "osc_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SK_SCALAR = 1, SK_OWNER, SK_BORROW, SK_ARENA /* OSC-2 arenas */,
       SK_POOL, SK_HANDLE /* OSC-3 item 2: versioned handles */,
       SK_SLICE /* external byte slice parameter (docs/osc/OSC-EXT-BYTES-DESIGN.md) */ };

/* OSC-3 item 2: static provenances (one per checked `P.alloc` evaluation) */
#define MAX_PROV 1024

typedef struct {
    char name[64];
    uint32_t line;
    uint8_t kind, mut, is_param;
    OscType ty;
    int loop_depth;
    int obj;   /* owner: object index */
    int bor;   /* borrow binding: current borrow index (-1 none) */
    /* SK_ARENA (OSC-2 section 3): model region id (per function, 1-based),
     * bound K in cells, cells taken by definite allocations so far, and the
     * conditional depth at the declaration (with loop_depth: an allocation is
     * definite when both depths equal the arena's) */
    uint32_t region;
    uint16_t cap, used;
    int cond_depth;
    /* OSC-3 item 2. SK_POOL: cap = K slots, gbase = declared generation base,
     * dallocs = definite allocations so far, dlive = definite live slots (a
     * lower bound; valid while cap_off == 0), cap_off = 1 once a free that is
     * not definite was seen. SK_HANDLE: pool = its pool symbol, prov = the
     * static provenance it holds (-1 unknown). */
    uint64_t gbase, dallocs;
    uint16_t dlive;
    uint8_t cap_off;
    int pool, prov;
    int decl;  /* OSC-3 item 3: declaring AST node (owners: carries the drop-flag mark) */
} Sym;

/* OSC-3 item 2: handle state saved around branches and loop bodies */
typedef struct {
    uint8_t pfreed[MAX_PROV];
    int hprov[OSC_CHECK_MAX_SYMS];
    int nprov, nsym;
} HSave;

/* Obj.moved. OSC-3 item 3 (drop flags) adds OM_MAYBE: moved on some but not
 * all paths that reach the current point. Such an owner cannot be used,
 * borrowed or moved; at scope end its release is guarded by a drop flag. */
enum { OM_LIVE = 0, OM_MOVED = 1, OM_GONE = 2 /* released (scope gone) */, OM_MAYBE = 3 };

typedef struct {
    uint8_t moved;   /* OM_* */
    uint16_t n_shared, n_mut;
    uint32_t move_line;
    int sym;   /* owning symbol or -1 (stand-in / dummy) */
    int arena1; /* OSC-2: arena symbol + 1 for an arena object, 0 for a unique owner */
} Obj;

typedef struct {
    uint8_t live, mut;
    int obj, parent;
    uint16_t kids_shared, kids_mut;
    uint32_t line;
    int sym;   /* binding symbol or -1 (call argument) */
} Bor;

typedef struct {
    Obj obj[OSC_CHECK_MAX_OBJS];
    Bor bor[OSC_CHECK_MAX_BORS];
    int symbor[OSC_CHECK_MAX_SYMS];
    int nobj, nbor, nsym;
} Snap;

#define MAX_SCOPES (OSC_AST_MAX_DEPTH * 2 + 8)

typedef struct {
    OscAst *ast;
    OscDiag *d;
    OscTrace *tr;
    int fi;               /* current function index */
    int fnode;
    OscType fret;
    Sym sym[OSC_CHECK_MAX_SYMS];
    int nsym;
    Obj obj[OSC_CHECK_MAX_OBJS];
    int nobj;
    Bor bor[OSC_CHECK_MAX_BORS];
    int nbor;
    int vis[OSC_CHECK_MAX_SYMS];
    int nvis;
    int scope_start[MAX_SCOPES];
    int depth;
    int loop_depth;
    int term;             /* current path has returned */
    char sobj[64];        /* statement object (diagnostics) */
    int contract;         /* 0 body; 1 checking a requires clause; 2 an ensures clause (OSC-2) */
    int cond_depth;       /* OSC-2 arenas: nesting of if/else branches */
    uint32_t nregion;     /* OSC-2 arenas: model region ids handed out in this function */
    /* OSC-3 item 2: provenances of this function. pfreed = freed on every path
     * reaching the current point; pmslot / pmhnd = model slot and handle ids
     * (fresh per provenance); pfree_sym = handle symbol named by the free. */
    int nprov;
    uint8_t pfreed[MAX_PROV];
    uint32_t pfree_line[MAX_PROV], pmslot[MAX_PROV], pmhnd[MAX_PROV];
    int pfree_sym[MAX_PROV];
    uint32_t nmslot, nmhnd;
} C;

#define NODE(i) (&c->ast->nodes[(i)])

/* ------------------------------------------------------------ trace */
static void tr_push(C *c, uint8_t op, const OscModelEvent *ev, uint32_t line, int refused)
{
    OscTrace *t = c->tr;
    if (!t) return;
    if (t->n >= OSC_TRACE_MAX) { t->overflow = 1; return; }
    OscTraceEntry *e = &t->e[t->n++];
    memset(e, 0, sizeof *e);
    e->op = op;
    e->refused = (uint8_t)refused;
    e->func = (uint16_t)c->fi;
    e->line = line;
    if (ev) {
        e->ev = *ev;
        if (ev->obj > OSC_MODEL_MAX_OBJECTS || ev->obj2 > OSC_MODEL_MAX_OBJECTS ||
            ev->borrow > OSC_MODEL_MAX_BORROWS || ev->via > OSC_MODEL_MAX_BORROWS ||
            ev->region > OSC_MODEL_MAX_REGIONS || ev->slot > OSC_MODEL_MAX_SLOTS ||
            ev->handle > OSC_MODEL_MAX_HANDLES)
            t->overflow = 1;
    }
    if (refused) t->refused = 1;
}
static void tr_ev(C *c, uint32_t kind, int obj, int obj2, int borrow, int via, uint32_t line, int refused)
{
    OscModelEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    ev.obj = obj >= 0 ? (uint32_t)obj + 1 : 0;
    ev.obj2 = obj2 >= 0 ? (uint32_t)obj2 + 1 : 0;
    ev.borrow = borrow >= 0 ? (uint32_t)borrow + 1 : 0;
    ev.via = via >= 0 ? (uint32_t)via + 1 : 0;
    tr_push(c, OSC_TR_EVENT, &ev, line, refused);
}
/* OSC-2 arenas: REGION_OPEN / REGION_DESTROY of region, ALLOC of obj in region */
static void tr_region(C *c, uint32_t kind, int obj, uint32_t region, uint32_t line, int refused)
{
    OscModelEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    ev.obj = obj >= 0 ? (uint32_t)obj + 1 : 0;
    ev.region = region;
    tr_push(c, OSC_TR_EVENT, &ev, line, refused);
}
/* OSC-3 item 2: SLOT_ALLOC (slot, handle, rights) / SLOT_FREE (handle) /
 * HANDLE_USE (handle, rights) */
static void tr_slot(C *c, uint32_t kind, uint32_t slot, uint32_t handle, uint64_t rights, uint32_t line, int refused)
{
    OscModelEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    ev.slot = slot;
    ev.handle = handle;
    ev.rights = rights;
    tr_push(c, OSC_TR_EVENT, &ev, line, refused);
}
static void tr_save(C *c, uint32_t line) { tr_push(c, OSC_TR_SAVE, NULL, line, 0); }
static void tr_restore(C *c, uint32_t line) { tr_push(c, OSC_TR_RESTORE, NULL, line, 0); }

/* ------------------------------------------------------------ diag helpers */
static int cap_fail(C *c, uint32_t line, const char *what)
{
    char fname[64];
    osc_node_name(c->ast, NODE(c->fnode), fname, sizeof fname);
    osc_diag_set(c->d, OSC_DIAG_CAPACITY, line, 0, fname, NODE(c->fnode)->line, NULL, what,
                 "function '%s' exceeds the %s", fname, what);
    return -1;
}

static int tmismatch(C *c, const OscNode *n, const char *other, const char *fmt_what)
{
    osc_diag_set(c->d, OSC_DIAG_TYPE_MISMATCH, n->line, n->col, c->sobj, 0, other, "type mismatch", "%s", fmt_what);
    return -1;
}

static const char *tname(OscScalar s) { return osc_scalar_name(s); }

/* ------------------------------------------------------------ objects & borrows */
static int new_obj(C *c, int sym, uint32_t line)
{
    if (c->nobj >= OSC_CHECK_MAX_OBJS) return cap_fail(c, line, "owner capacity");
    Obj *o = &c->obj[c->nobj];
    memset(o, 0, sizeof *o);
    o->sym = sym;
    return c->nobj++;
}

static int new_bor(C *c, int obj, int parent, int mut, uint32_t line)
{
    if (c->nbor >= OSC_CHECK_MAX_BORS) return cap_fail(c, line, "borrow capacity");
    Bor *b = &c->bor[c->nbor];
    memset(b, 0, sizeof *b);
    b->live = 1;
    b->mut = (uint8_t)mut;
    b->obj = obj;
    b->parent = parent;
    b->line = line;
    b->sym = -1;
    if (parent >= 0) {
        if (mut) c->bor[parent].kids_mut++; else c->bor[parent].kids_shared++;
    } else {
        if (mut) c->obj[obj].n_mut++; else c->obj[obj].n_shared++;
    }
    return c->nbor++;
}

static void end_bor(C *c, int b, uint32_t line)
{
    Bor *x = &c->bor[b];
    tr_ev(c, OSC_EV_END_BORROW, -1, -1, b, -1, line, 0);
    x->live = 0;
    if (x->parent >= 0) {
        if (x->mut) c->bor[x->parent].kids_mut--; else c->bor[x->parent].kids_shared--;
    } else {
        if (x->mut) c->obj[x->obj].n_mut--; else c->obj[x->obj].n_shared--;
    }
}

/* describe a live borrow conflicting on object o (direct) or as a child of borrow p */
static void conflict_name(C *c, int o, int p, int want_mut_only, char *buf, size_t cap)
{
    for (int i = c->nbor - 1; i >= 0; i--) {
        const Bor *b = &c->bor[i];
        if (!b->live) continue;
        if (p >= 0 ? b->parent != p : (b->parent >= 0 || b->obj != o)) continue;
        if (want_mut_only && !b->mut) continue;
        if (b->sym >= 0) snprintf(buf, cap, "%s%s", b->mut ? "&mut " : "&", c->sym[b->sym].name);
        else snprintf(buf, cap, "%s argument (line %u)", b->mut ? "&mut" : "&", b->line);
        return;
    }
    snprintf(buf, cap, "?");
}

static int diag_own(C *c, OscDiagKind k, uint32_t line, uint32_t col, int s, const char *other,
                    const char *transition, const char *msg)
{
    osc_diag_set(c->d, k, line, col, c->sym[s].name, c->sym[s].line, other, transition, "%s", msg);
    return -1;
}

/* OSC-3 item 3: a maybe-moved owner (OM_MAYBE) is refused exactly like a
 * moved one. Its trace records the path on which it was moved (MOVE obj -> 0,
 * accepted) before the refused event, so the refusal replays through the
 * model as use-after-move. Returns 1 for a maybe-moved owner. */
static int maybe_path(C *c, const Sym *y)
{
    if (c->obj[y->obj].moved != OM_MAYBE) return 0;
    tr_ev(c, OSC_EV_MOVE, y->obj, -1, -1, -1, c->obj[y->obj].move_line, 0);
    return 1;
}

/* direct or via-borrow access of symbol s (owner or borrow binding). wr: write. */
static int access(C *c, int s, int wr, uint32_t line, uint32_t col)
{
    Sym *y = &c->sym[s];
    char other[64], msg[160];
    if (y->kind == SK_OWNER) {
        Obj *o = &c->obj[y->obj];
        uint32_t k = wr ? OSC_EV_USE_WRITE : OSC_EV_USE_READ;
        if (o->moved) {
            int mb = maybe_path(c, y);
            tr_ev(c, k, y->obj, -1, -1, -1, line, 1);
            snprintf(msg, sizeof msg, mb ? "'%s' used after it may have been moved at line %u (moved on some path of an 'if')" : "'%s' used after it was moved at line %u", y->name, o->move_line);
            return diag_own(c, OSC_DIAG_USE_AFTER_MOVE, line, col, s, NULL, mb ? "use after maybe-move" : "use after move", msg);
        }
        if (o->n_mut || (wr && o->n_shared)) {
            conflict_name(c, y->obj, -1, !wr, other, sizeof other);
            tr_ev(c, k, y->obj, -1, -1, -1, line, 1);
            snprintf(msg, sizeof msg, "direct %s of '%s' while %s is live", wr ? "store" : "read", y->name, other);
            return diag_own(c, OSC_DIAG_MUTABLE_ALIAS, line, col, s, other,
                            wr ? "write while borrowed" : "read while borrow_mut live", msg);
        }
        tr_ev(c, k, y->obj, -1, -1, -1, line, 0);
        return 0;
    }
    /* borrow binding / borrow parameter */
    int b = y->bor;
    Bor *x = &c->bor[b];
    uint32_t k = wr ? OSC_EV_USE_WRITE : OSC_EV_USE_READ;
    if (wr && !x->mut) {
        tr_ev(c, k, x->obj, -1, -1, b, line, 1);
        snprintf(msg, sizeof msg, "store through shared borrow '%s' (read only)", y->name);
        return diag_own(c, OSC_DIAG_READ_ONLY_BORROW, line, col, s, NULL, "write through shared borrow", msg);
    }
    if (x->kids_mut || (wr && x->kids_shared)) {
        conflict_name(c, -1, b, !wr, other, sizeof other);
        tr_ev(c, k, x->obj, -1, -1, b, line, 1);
        snprintf(msg, sizeof msg, "use of '%s' while its reborrow %s is live", y->name, other);
        return diag_own(c, OSC_DIAG_MUTABLE_ALIAS, line, col, s, other,
                        wr ? "write while reborrowed" : "read while reborrow_mut live", msg);
    }
    tr_ev(c, k, x->obj, -1, -1, b, line, 0);
    return 0;
}

/* take a borrow of symbol s; returns borrow index or -1 */
static int take_borrow(C *c, int s, int mut, uint32_t line, uint32_t col)
{
    Sym *y = &c->sym[s];
    char other[64], msg[160];
    uint32_t k = mut ? OSC_EV_BORROW_MUT : OSC_EV_BORROW_SHARED;
    if (y->kind == SK_OWNER) {
        Obj *o = &c->obj[y->obj];
        if (o->moved) {
            int mb = maybe_path(c, y);
            tr_ev(c, k, y->obj, -1, c->nbor, -1, line, 1);
            snprintf(msg, sizeof msg, mb ? "borrow of '%s' after it may have been moved at line %u (moved on some path of an 'if')" : "borrow of '%s' after it was moved at line %u", y->name, o->move_line);
            return diag_own(c, OSC_DIAG_USE_AFTER_MOVE, line, col, s, NULL, mb ? "use after maybe-move" : "use after move", msg);
        }
        if (o->n_mut || (mut && o->n_shared)) {
            conflict_name(c, y->obj, -1, !mut, other, sizeof other);
            tr_ev(c, k, y->obj, -1, c->nbor, -1, line, 1);
            snprintf(msg, sizeof msg, "%s of '%s' while %s is live", mut ? "&mut" : "&", y->name, other);
            return diag_own(c, OSC_DIAG_MUTABLE_ALIAS, line, col, s, other,
                            mut ? (o->n_mut ? "borrow_mut while borrow_mut live" : "borrow_mut while borrow_shared live")
                                : "borrow_shared while borrow_mut live", msg);
        }
        int b = new_bor(c, y->obj, -1, mut, line);
        if (b < 0) return -1;
        tr_ev(c, k, y->obj, -1, b, -1, line, 0);
        return b;
    }
    int p = y->bor;
    Bor *x = &c->bor[p];
    if (mut && !x->mut) {
        tr_ev(c, k, x->obj, -1, c->nbor, p, line, 1);
        snprintf(msg, sizeof msg, "&mut reborrow through shared borrow '%s' (read only)", y->name);
        return diag_own(c, OSC_DIAG_READ_ONLY_BORROW, line, col, s, NULL, "borrow_mut through shared borrow", msg);
    }
    if (x->kids_mut || (mut && x->kids_shared)) {
        conflict_name(c, -1, p, !mut, other, sizeof other);
        tr_ev(c, k, x->obj, -1, c->nbor, p, line, 1);
        snprintf(msg, sizeof msg, "reborrow of '%s' while %s is live", y->name, other);
        return diag_own(c, OSC_DIAG_MUTABLE_ALIAS, line, col, s, other,
                        mut ? "reborrow_mut while reborrow live" : "reborrow_shared while reborrow_mut live", msg);
    }
    int b = new_bor(c, x->obj, p, mut, line);
    if (b < 0) return -1;
    tr_ev(c, k, x->obj, -1, b, p, line, 0);
    return b;
}

/* move owner symbol s; dest object index or -1 (consumed by an own parameter) */
static int do_move(C *c, int s, int dest, uint32_t line, uint32_t col)
{
    Sym *y = &c->sym[s];
    Obj *o = &c->obj[y->obj];
    char other[64], msg[160];
    if (o->moved) {
        int mb = maybe_path(c, y);
        tr_ev(c, OSC_EV_MOVE, y->obj, dest, -1, -1, line, 1);
        snprintf(msg, sizeof msg, mb ? "'%s' moved again after it may have been moved at line %u (moved on some path of an 'if')" : "'%s' moved again after it was moved at line %u", y->name, o->move_line);
        return diag_own(c, OSC_DIAG_USE_AFTER_MOVE, line, col, s, NULL, mb ? "use after maybe-move" : "use after move", msg);
    }
    if (o->n_shared || o->n_mut) {
        conflict_name(c, y->obj, -1, 0, other, sizeof other);
        tr_ev(c, OSC_EV_MOVE, y->obj, dest, -1, -1, line, 1);
        snprintf(msg, sizeof msg, "move of '%s' while %s is live", y->name, other);
        return diag_own(c, OSC_DIAG_MUTABLE_ALIAS, line, col, s, other, "move while borrowed", msg);
    }
    if (y->loop_depth < c->loop_depth) {
        /* first iteration moves, the next iteration would use a moved owner */
        tr_ev(c, OSC_EV_MOVE, y->obj, dest, -1, -1, line, 0);
        int d2 = -1;
        if (dest >= 0) { d2 = new_obj(c, -1, line); if (d2 < 0) return -1; }
        tr_ev(c, OSC_EV_MOVE, y->obj, d2, -1, -1, line, 1);
        snprintf(msg, sizeof msg, "'%s' (declared outside the loop) is moved inside a loop: the next iteration "
                                  "would use it after the move", y->name);
        return diag_own(c, OSC_DIAG_USE_AFTER_MOVE, line, col, s, NULL, "use after move (move inside loop)", msg);
    }
    tr_ev(c, OSC_EV_MOVE, y->obj, dest, -1, -1, line, 0);
    o->moved = 1;
    o->move_line = line;
    return 0;
}

/* ------------------------------------------------------------ scopes */
static int lookup(C *c, const char *name)
{
    for (int i = c->nvis - 1; i >= 0; i--)
        if (strcmp(c->sym[c->vis[i]].name, name) == 0) return c->vis[i];
    return -1;
}

static int declare(C *c, const OscNode *n, uint8_t kind, const OscType *ty, int mut)
{
    char name[64];
    osc_node_name(c->ast, n, name, sizeof name);
    int prev = lookup(c, name);
    if (prev >= 0) {
        osc_diag_set(c->d, OSC_DIAG_REDEFINED_NAME, n->line, n->col, name, c->sym[prev].line, NULL,
                     "redefine visible name", "'%s' is already defined at line %u (no shadowing)", name,
                     c->sym[prev].line);
        return -1;
    }
    if (c->nsym >= OSC_CHECK_MAX_SYMS) return cap_fail(c, n->line, "symbol capacity");
    int s = c->nsym++;
    Sym *y = &c->sym[s];
    memset(y, 0, sizeof *y);
    memcpy(y->name, name, sizeof name);
    y->line = n->line;
    y->kind = kind;
    y->mut = (uint8_t)mut;
    y->ty = *ty;
    y->loop_depth = c->loop_depth;
    y->obj = y->bor = -1;
    y->decl = (int)(n - c->ast->nodes);
    c->vis[c->nvis++] = s;
    return s;
}

static int push_scope(C *c, uint32_t line)
{
    if (c->depth >= MAX_SCOPES) return cap_fail(c, line, "scope depth");
    c->scope_start[c->depth++] = c->nvis;
    return 0;
}

/* OSC-2 arenas: a borrow of an object of arena symbol as would outlive the
 * arena. The refused event is the arena's REGION_DESTROY (the model rejects it
 * with arena-escape while that borrow is live). */
static int arena_escape(C *c, const OscNode *at, int as, const char *who, const char *transition)
{
    Sym *y = &c->sym[as];
    tr_region(c, OSC_EV_REGION_DESTROY, -1, y->region, at->line, 1);
    osc_diag_set(c->d, OSC_DIAG_ARENA_ESCAPE, at->line, at->col, y->name, y->line, who, transition,
                 "a borrow of an object in arena '%s' would outlive the arena (%s)", y->name, transition);
    return -1;
}

/* scope-end destruction of the innermost `levels` scopes (without popping).
 * Records released owner symbols into the AST release list. OSC-2 arenas:
 * arena objects are never released one by one; the arena symbol (declared in
 * the scope just outside its body) emits REGION_DESTROY, which releases them
 * all, and is recorded in the release list (the lowerer emits ADESTROY). Since
 * scopes are walked innermost first and symbols newest first, the owners and
 * borrows declared inside an arena body end before the arena is destroyed,
 * and an inner arena before an outer one. */
static int scope_exit_events(C *c, int levels, uint32_t line, OscNode *rec)
{
    OscAst *a = c->ast;
    rec->rel_start = a->nrel;
    rec->rel_count = 0;
    for (int lv = 0; lv < levels; lv++) {
        int d = c->depth - 1 - lv;
        /* each level visits only its own symbols (inner levels were done) */
        int hi = lv == 0 ? c->nvis : c->scope_start[d + 1];
        for (int i = hi - 1; i >= c->scope_start[d]; i--) {
            int s = c->vis[i];
            Sym *y = &c->sym[s];
            if (y->kind == SK_BORROW) {
                if (y->bor >= 0 && c->bor[y->bor].live) end_bor(c, y->bor, line);
            } else if (y->kind == SK_OWNER) {
                Obj *o = &c->obj[y->obj];
                /* OSC-3 item 3: a maybe-moved owner is listed too; the lowerer
                 * guards its release with the owner's drop flag */
                if ((o->moved == OM_LIVE || o->moved == OM_MAYBE) && !o->arena1) {
                    tr_ev(c, OSC_EV_RELEASE, y->obj, -1, -1, -1, line, 0);
                    o->moved = 2; /* released (scope gone) */
                    if (a->nrel < OSC_AST_MAX_REL) { a->rel[a->nrel++] = (int16_t)s; rec->rel_count++; }
                }
            } else if (y->kind == SK_ARENA) {
                for (int b = 0; b < c->nbor; b++)
                    if (c->bor[b].live && c->obj[c->bor[b].obj].arena1 == s + 1)
                        return arena_escape(c, rec, s, "scope end", "borrow live at arena end");
                tr_region(c, OSC_EV_REGION_DESTROY, -1, y->region, line, 0);
                for (int o = 0; o < c->nobj; o++)
                    if (c->obj[o].arena1 == s + 1 && !c->obj[o].moved) c->obj[o].moved = 2;
                if (a->nrel < OSC_AST_MAX_REL) { a->rel[a->nrel++] = (int16_t)s; rec->rel_count++; }
            } else if (y->kind == SK_POOL) {
                /* OSC-3 item 2: the pool closes (PCLOSE); its handles are
                 * values declared inside its scope, so none outlives it */
                if (a->nrel < OSC_AST_MAX_REL) { a->rel[a->nrel++] = (int16_t)s; rec->rel_count++; }
            }
        }
    }
    return 0;
}

static void pop_scope(C *c)
{
    c->depth--;
    c->nvis = c->scope_start[c->depth];
}

/* ------------------------------------------------------------ state snapshots */
static Snap *snap_take(C *c)
{
    Snap *s = malloc(sizeof *s);
    if (!s) return NULL;
    memcpy(s->obj, c->obj, sizeof(Obj) * (size_t)c->nobj);
    memcpy(s->bor, c->bor, sizeof(Bor) * (size_t)c->nbor);
    for (int i = 0; i < c->nsym; i++) s->symbor[i] = c->sym[i].bor;
    s->nobj = c->nobj;
    s->nbor = c->nbor;
    s->nsym = c->nsym;
    return s;
}
/* OSC-3 item 2: handle state (freed provenances, provenance per handle) */
static HSave *hs_take(C *c)
{
    HSave *s = malloc(sizeof *s);
    if (!s) return NULL;
    memcpy(s->pfreed, c->pfreed, (size_t)c->nprov);
    for (int i = 0; i < c->nsym; i++) s->hprov[i] = c->sym[i].prov;
    s->nprov = c->nprov;
    s->nsym = c->nsym;
    return s;
}
static void hs_restore(C *c, const HSave *s)
{
    memcpy(c->pfreed, s->pfreed, (size_t)s->nprov);
    for (int i = 0; i < s->nsym; i++) c->sym[i].prov = s->hprov[i];
}
/* after a branch or loop: a handle whose provenance differs from the one it
 * had before (assigned on some path) becomes unknown; the model trace was
 * restored at OSC_TR_RESTORE, so provenances freed on every surviving path
 * since `before` are re-emitted as SLOT_FREE */
static void hs_settle(C *c, const HSave *before, const HSave *other, uint32_t line)
{
    for (int i = 0; i < before->nsym; i++) {
        if (c->sym[i].kind != SK_HANDLE) continue;
        if (c->sym[i].prov != before->hprov[i] || (other && other->hprov[i] != before->hprov[i])) c->sym[i].prov = -1;
    }
    for (int p = 0; p < before->nprov; p++)
        if (c->pfreed[p] && !before->pfreed[p] && c->pmhnd[p])
            tr_slot(c, OSC_EV_SLOT_FREE, 0, c->pmhnd[p], 0, line, 0);
}

/* restore state of the entities that existed at the snapshot; later ones stay
 * allocated (ids are single assignment) but are dead */
static void snap_restore(C *c, const Snap *s)
{
    memcpy(c->obj, s->obj, sizeof(Obj) * (size_t)s->nobj);
    memcpy(c->bor, s->bor, sizeof(Bor) * (size_t)s->nbor);
    for (int i = s->nbor; i < c->nbor; i++) c->bor[i].live = 0;
    for (int i = 0; i < s->nsym; i++) c->sym[i].bor = s->symbor[i];
}

/* ------------------------------------------------------------ integer helpers */
typedef __int128 i128;

static int fits(OscScalar t, i128 v)
{
    unsigned w = osc_scalar_width(t);
    if (osc_scalar_signed(t)) {
        i128 lo = -((i128)1 << (w - 1)), hi = ((i128)1 << (w - 1)) - 1;
        return v >= lo && v <= hi;
    }
    return v >= 0 && v <= (((i128)1 << w) - 1);
}
static i128 wrapu(OscScalar t, i128 v)
{
    unsigned w = osc_scalar_width(t);
    unsigned __int128 m = (((unsigned __int128)1) << w) - 1;
    return (i128)((unsigned __int128)v & m);
}
static uint64_t canon(OscScalar t, i128 v)
{
    if (t == OSC_T_BOOL) return v ? 1 : 0;
    if (osc_scalar_signed(t)) return (uint64_t)(int64_t)v;
    return (uint64_t)wrapu(t, v);
}

static int is_shift(int op) { return op == OT_SHL || op == OT_SHR; }
static int is_arith(int op)
{
    return op == OT_PLUS || op == OT_MINUS || op == OT_STAR || op == OT_SLASH || op == OT_PERCENT || op == OT_AMP ||
           op == OT_PIPE || op == OT_CARET;
}
static int is_cmpop(int op) { return op == OT_EQ || op == OT_NE || op == OT_LT || op == OT_LE || op == OT_GT || op == OT_GE; }

/* type comes only from context */
static int ctxfree(C *c, int i)
{
    const OscNode *n = NODE(i);
    switch (n->kind) {
    case ON_INT: return 1;
    case ON_UN: return n->op != OT_BANG && ctxfree(c, n->a);
    case ON_BIN:
        if (is_shift(n->op)) return ctxfree(c, n->a);
        if (is_arith(n->op)) return ctxfree(c, n->a) && ctxfree(c, n->b);
        return 0;
    default: return 0;
    }
}
/* all leaves literal */
static int constexpr_(C *c, int i)
{
    const OscNode *n = NODE(i);
    switch (n->kind) {
    case ON_INT: return 1;
    case ON_UN: return n->op != OT_BANG && constexpr_(c, n->a);
    case ON_BIN: return (is_shift(n->op) || is_arith(n->op)) && constexpr_(c, n->a) && constexpr_(c, n->b);
    default: return 0;
    }
}

static int ovf(C *c, const OscNode *n, const char *transition, const char *msg)
{
    osc_diag_set(c->d, OSC_DIAG_OVERFLOW_UNSAFE, n->line, n->col, c->sobj, 0, NULL, transition, "%s", msg);
    return -1;
}

/* fold constant expression i at type t; value as mathematical integer in range of t */
static int fold(C *c, int i, OscScalar t, i128 *out)
{
    OscNode *n = NODE(i);
    char msg[320];
    if (t == OSC_T_BOOL) return tmismatch(c, n, "bool", "integer literal where bool is required");
    int sg = osc_scalar_signed(t);
    unsigned w = osc_scalar_width(t);
    i128 v, a, b;
    switch (n->kind) {
    case ON_INT:
        v = (i128)n->ival;
        if (!fits(t, v)) {
            snprintf(msg, sizeof msg, "literal %llu does not fit %s", (unsigned long long)n->ival, tname(t));
            return ovf(c, n, "literal does not fit type", msg);
        }
        break;
    case ON_UN:
        if (n->op == OT_MINUS && NODE(n->a)->kind == ON_INT) {
            const OscNode *l = NODE(n->a);
            v = -(i128)l->ival;
            if (!sg && l->ival != 0) {
                snprintf(msg, sizeof msg, "negative literal -%llu for unsigned %s", (unsigned long long)l->ival, tname(t));
                return ovf(c, n, "negative literal for unsigned type", msg);
            }
            if (!fits(t, v)) {
                snprintf(msg, sizeof msg, "literal -%llu does not fit %s", (unsigned long long)l->ival, tname(t));
                return ovf(c, n, "literal does not fit type", msg);
            }
            NODE(n->a)->ty.s = t;
            NODE(n->a)->is_const = 1;
            NODE(n->a)->cval = canon(t, (i128)l->ival);
            break;
        }
        if (fold(c, n->a, t, &a)) return -1;
        if (n->op == OT_MINUS) {
            if (!sg) {
                if (a != 0) return ovf(c, n, "negative constant for unsigned type", "negation of a non-zero unsigned constant");
                v = 0;
            } else {
                v = -a;
                if (!fits(t, v)) return ovf(c, n, "signed constant overflow", "constant negation overflows");
            }
        } else { /* ~ */
            v = sg ? (i128)(~(int64_t)a) : wrapu(t, ~a);
        }
        break;
    case ON_BIN:
        if (fold(c, n->a, t, &a) || fold(c, n->b, t, &b)) return -1;
        switch (n->op) {
        case OT_PLUS: v = a + b; break;
        case OT_MINUS: v = a - b; break;
        case OT_STAR:
            if (sg) v = a * b;
            else v = (i128)(((unsigned __int128)a * (unsigned __int128)b) & ((((unsigned __int128)1) << w) - 1));
            break;
        case OT_SLASH: case OT_PERCENT:
            if (b == 0) return ovf(c, NODE(n->b), "literal zero divisor", "division by a constant zero");
            v = n->op == OT_SLASH ? a / b : a % b;
            break;
        case OT_AMP: v = sg ? (i128)((int64_t)a & (int64_t)b) : (a & b); break;
        case OT_PIPE: v = sg ? (i128)((int64_t)a | (int64_t)b) : (a | b); break;
        case OT_CARET: v = sg ? (i128)((int64_t)a ^ (int64_t)b) : (a ^ b); break;
        case OT_SHL: case OT_SHR:
            if (b < 0 || b >= (i128)w) {
                snprintf(msg, sizeof msg, "constant shift amount %lld outside 0..%u", (long long)b, w - 1);
                return ovf(c, NODE(n->b), "literal shift >= width", msg);
            }
            if (n->op == OT_SHL) {
                if (sg) {
                    v = a * ((i128)1 << (int)b);
                    if (!fits(t, v)) return ovf(c, n, "signed constant overflow", "constant left shift overflows");
                } else {
                    v = wrapu(t, a << (int)b);
                }
            } else {
                v = sg ? (a >> (int)b) : (a >> (int)b);
            }
            break;
        default: return tmismatch(c, n, NULL, "operator not allowed in a constant");
        }
        if (sg) {
            if (!fits(t, v)) {
                snprintf(msg, sizeof msg, "constant expression overflows %s", tname(t));
                return ovf(c, n, "signed constant overflow", msg);
            }
        } else {
            v = wrapu(t, v);
        }
        break;
    default:
        return tmismatch(c, n, NULL, "not a constant");
    }
    n->ty.s = t;
    n->is_const = 1;
    n->cval = canon(t, v);
    *out = v;
    return 0;
}


/* ------------------------------------------------------------ contracts (OSC-2) */
/* Static evaluation of a checked contract clause (docs/osc/OSC-2-DESIGN.md
 * section 1.4). Constant folding only: a leaf is known if it is a literal /
 * folded constant, a `true` / `false`, or a name whose value the caller
 * supplies (call-site literal arguments, a constant return value). Operators
 * follow section 5 of OSC-1 exactly; any operation that would trap at run time
 * (overflow, divide by zero, shift range, cast range) makes the result
 * unknown, as do array reads and calls. `&&` / `||` are decided left to
 * right: the right operand is consulted only when the left one is known. */
typedef struct {
    int nk;                         /* symbols 0..nk-1 may be known (parameters) */
    uint64_t v[OSC_MAX_PARAMS];
    uint8_t k[OSC_MAX_PARAMS];
    int res_sym;                    /* `result` symbol, or -1 */
    uint8_t res_known;
    uint64_t res_val;
} CEnv;

static uint64_t ccanon(OscScalar t, uint64_t x)
{
    unsigned w = osc_scalar_width(t);
    if (t == OSC_T_BOOL) return x & 1;
    if (w >= 64) return x;
    if (osc_scalar_signed(t)) {
        uint64_t m = 1ULL << (w - 1);
        x &= (1ULL << w) - 1;
        return (x ^ m) - m;
    }
    return x & ((1ULL << w) - 1);
}
static int sfits(unsigned w, i128 r) { return r >= -((i128)1 << (w - 1)) && r <= ((i128)1 << (w - 1)) - 1; }

/* 1 known (*out canonical for the node's type), 0 unknown */
static int ceval(C *c, int i, const CEnv *env, uint64_t *out)
{
    const OscNode *n = NODE(i);
    if (n->is_const) { *out = n->cval; return 1; }
    uint64_t a, b;
    switch (n->kind) {
    case ON_BOOL: *out = n->ival ? 1 : 0; return 1;
    case ON_NAME:
        if (n->sym >= 0 && n->sym < env->nk && env->k[n->sym]) { *out = env->v[n->sym]; return 1; }
        if (n->sym >= 0 && n->sym == env->res_sym && env->res_known) { *out = env->res_val; return 1; }
        return 0;
    case ON_UN: {
        if (!ceval(c, n->a, env, &a)) return 0;
        OscScalar t = n->ty.s;
        if (n->op == OT_BANG) { *out = a ^ 1; return 1; }
        if (n->op == OT_TILDE) { *out = ccanon(t, ~a); return 1; }
        if (osc_scalar_signed(t)) {
            i128 r = -(i128)(int64_t)a;
            if (!sfits(osc_scalar_width(t), r)) return 0;
            *out = (uint64_t)(int64_t)r;
            return 1;
        }
        *out = ccanon(t, 0 - a);
        return 1;
    }
    case ON_CAST: {
        if (!ceval(c, n->a, env, &a)) return 0;
        OscScalar from = NODE(n->a)->ty.s, to = n->ty.s;
        i128 v = osc_scalar_signed(from) ? (i128)(int64_t)a : (i128)a;
        unsigned w = osc_scalar_width(to);
        i128 lo = osc_scalar_signed(to) ? -((i128)1 << (w - 1)) : 0;
        i128 hi = osc_scalar_signed(to) ? ((i128)1 << (w - 1)) - 1 : (((i128)1 << w) - 1);
        if (v < lo || v > hi) return 0;
        *out = (uint64_t)v;
        return 1;
    }
    case ON_BIN: {
        int op = n->op;
        if (op == OT_ANDAND || op == OT_OROR) {
            if (!ceval(c, n->a, env, &a)) return 0;
            if (op == OT_ANDAND && !a) { *out = 0; return 1; }
            if (op == OT_OROR && a) { *out = 1; return 1; }
            if (!ceval(c, n->b, env, &b)) return 0;
            *out = b;
            return 1;
        }
        if (!ceval(c, n->a, env, &a) || !ceval(c, n->b, env, &b)) return 0;
        if (is_cmpop(op)) {
            OscScalar T = (OscScalar)n->flag;
            int lt = osc_scalar_signed(T) ? (int64_t)a < (int64_t)b : a < b, eq = a == b;
            switch (op) {
            case OT_EQ: *out = eq; break;
            case OT_NE: *out = !eq; break;
            case OT_LT: *out = lt; break;
            case OT_LE: *out = lt || eq; break;
            case OT_GT: *out = !lt && !eq; break;
            default: *out = !lt; break;
            }
            return 1;
        }
        OscScalar t = n->ty.s, tb = NODE(n->b)->ty.s;
        unsigned w = osc_scalar_width(t);
        int s = osc_scalar_signed(t);
        i128 A = s ? (i128)(int64_t)a : (i128)a, B = osc_scalar_signed(tb) ? (i128)(int64_t)b : (i128)b, r;
        switch (op) {
        case OT_PLUS: if (!s) { *out = ccanon(t, a + b); return 1; } r = A + B; break;
        case OT_MINUS: if (!s) { *out = ccanon(t, a - b); return 1; } r = A - B; break;
        case OT_STAR: if (!s) { *out = ccanon(t, a * b); return 1; } r = A * B; break;
        case OT_SLASH:
            if (b == 0) return 0;
            if (!s) { *out = a / b; return 1; }
            r = A / B;
            break;
        case OT_PERCENT:
            if (b == 0) return 0;
            *out = s ? (uint64_t)(int64_t)(A % B) : a % b;
            return 1;
        case OT_AMP: *out = a & b; return 1;
        case OT_PIPE: *out = a | b; return 1;
        case OT_CARET: *out = a ^ b; return 1;
        case OT_SHL: case OT_SHR:
            if (B < 0 || B >= (i128)w) return 0;
            if (op == OT_SHR) { *out = s ? (uint64_t)((int64_t)a >> (unsigned)B) : a >> (unsigned)B; return 1; }
            if (!s) { *out = ccanon(t, a << (unsigned)B); return 1; }
            r = A * ((i128)1 << (unsigned)B);
            break;
        default: return 0;
        }
        if (!sfits(w, r)) return 0;
        *out = (uint64_t)(int64_t)r;
        return 1;
    }
    default:
        return 0; /* ON_INDEX (memory), ON_CALL (refused in clauses anyway) */
    }
}

/* name check inside a clause: `result` is reserved there */
static int contract_name(C *c, const OscNode *n)
{
    if (!c->contract) return 0;
    char name[64];
    osc_node_name(c->ast, n, name, sizeof name);
    if (strcmp(name, "result") != 0) return 0;
    char fname[64];
    osc_node_name(c->ast, NODE(c->fnode), fname, sizeof fname);
    if (c->contract == 1) {
        osc_diag_set(c->d, OSC_DIAG_CONTRACT_INVALID, n->line, n->col, "result", NODE(c->fnode)->line, fname,
                     "result in requires", "'result' (the return value) is not available in a requires clause");
        return -1;
    }
    if (c->fret.s == OSC_T_VOID) {
        osc_diag_set(c->d, OSC_DIAG_CONTRACT_INVALID, n->line, n->col, "result", NODE(c->fnode)->line, fname,
                     "result in void function", "'%s' returns nothing, so its ensures clause has no 'result'", fname);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------ expressions */
static int chk(C *c, int i, OscScalar want);
static int chk_call(C *c, int i, OscScalar want, int as_stmt);
static int arena_move_refused(C *c, const OscNode *at, int s, const char *transition);

/* external byte slices: a slice parameter may only be indexed, measured with
 * .len, or passed whole to a slice parameter of the same type */
static int slice_diag(C *c, const OscNode *at, OscDiagKind k, const Sym *y, const char *tr, const char *msg)
{
    osc_diag_set(c->d, k, at->line, at->col, y->name, y->line, NULL, tr, "%s", msg);
    return -1;
}

/* node i is the bare name of a slice parameter: its symbol, else -1 (silent) */
static int slice_name(C *c, int i)
{
    const OscNode *n = NODE(i);
    char name[64];
    if (n->kind != ON_NAME) return -1;
    osc_node_name(c->ast, n, name, sizeof name);
    int s = lookup(c, name);
    return s >= 0 && c->sym[s].kind == SK_SLICE ? s : -1;
}

/* a value stored anywhere (let, assignment, element, field, return) must not be a slice */
static int slice_noescape(C *c, int i)
{
    int s = i >= 0 ? slice_name(c, i) : -1;
    if (s < 0) return 0;
    char msg[160];
    snprintf(msg, sizeof msg, "'%s' is a borrowed slice: it is never returned or stored (not even in a local)", c->sym[s].name);
    return slice_diag(c, NODE(i), OSC_DIAG_SLICE_ESCAPE, &c->sym[s], "store or return a slice", msg);
}

static int want_ok(C *c, const OscNode *n, OscScalar got, OscScalar want)
{
    if (want && got != want) {
        char msg[320];
        snprintf(msg, sizeof msg, "expression of type %s where %s is required", tname(got), tname(want));
        return tmismatch(c, n, tname(got), msg);
    }
    return 0;
}

static int resolve(C *c, const OscNode *n)
{
    char name[64];
    osc_node_name(c->ast, n, name, sizeof name);
    int s = lookup(c, name);
    if (s < 0)
        osc_diag_set(c->d, OSC_DIAG_UNDEFINED_NAME, n->line, n->col, name, 0, NULL, "use of undefined name",
                     "'%s' is not defined in an enclosing scope", name);
    if (s >= 0 && c->sym[s].kind == SK_ARENA) {
        osc_diag_set(c->d, OSC_DIAG_TYPE_MISMATCH, n->line, n->col, name, c->sym[s].line, NULL, "arena used as a value",
                     "'%s' is an arena; it is only named by 'in %s' in an owner declaration", name, name);
        return -1;
    }
    if (s >= 0 && c->sym[s].kind == SK_POOL) { /* OSC-3 item 2 */
        osc_diag_set(c->d, OSC_DIAG_TYPE_MISMATCH, n->line, n->col, name, c->sym[s].line, NULL, "pool used as a value",
                     "'%s' is a pool; it is only used as '%s.alloc(e)', '%s.free(h)', '%s[h]'", name, name, name, name);
        return -1;
    }
    if (s >= 0 && c->sym[s].kind == SK_HANDLE) {
        osc_diag_set(c->d, OSC_DIAG_TYPE_MISMATCH, n->line, n->col, name, c->sym[s].line, NULL, "handle used as a value",
                     "'%s' is a handle; it is only used as a pool index, freed, or copied into another handle "
                     "binding (no arithmetic, casts, arguments, returns or storage)", name);
        return -1;
    }
    return s;
}

/* ------------------------------------------------------------ OSC-3 item 2: versioned handles */
/* pool symbol named by n (no diagnostic), or -1 */
static int pool_named(C *c, const OscNode *n)
{
    char name[64];
    osc_node_name(c->ast, n, name, sizeof name);
    int s = lookup(c, name);
    return s >= 0 && c->sym[s].kind == SK_POOL ? s : -1;
}

/* the pool for `NAME.alloc` / `NAME.free` / a handle type: must be a pool */
static int pool_sym(C *c, const OscNode *n)
{
    char name[64];
    osc_node_name(c->ast, n, name, sizeof name);
    int s = lookup(c, name);
    if (s < 0) {
        osc_diag_set(c->d, OSC_DIAG_UNDEFINED_NAME, n->line, n->col, name, 0, NULL, "use of undefined name",
                     "'%s' is not defined in an enclosing scope", name);
        return -1;
    }
    if (c->sym[s].kind != SK_POOL) {
        osc_diag_set(c->d, OSC_DIAG_TYPE_MISMATCH, n->line, n->col, name, c->sym[s].line, NULL, "not a pool",
                     "'%s' is not a pool", name);
        return -1;
    }
    return s;
}

/* handle operand: an ON_NAME naming a handle of pool ps. Returns its symbol. */
static int handle_operand(C *c, const OscNode *hn, int ps, const char *what)
{
    Sym *p = &c->sym[ps];
    char name[64];
    if (hn->kind != ON_NAME) {
        osc_diag_set(c->d, OSC_DIAG_TYPE_MISMATCH, hn->line, hn->col, p->name, p->line, NULL, "pool index not a handle",
                     "%s of pool '%s' takes a handle binding of that pool", what, p->name);
        return -1;
    }
    osc_node_name(c->ast, hn, name, sizeof name);
    int s = lookup(c, name);
    if (s < 0) {
        osc_diag_set(c->d, OSC_DIAG_UNDEFINED_NAME, hn->line, hn->col, name, 0, NULL, "use of undefined name",
                     "'%s' is not defined in an enclosing scope", name);
        return -1;
    }
    if (c->sym[s].kind != SK_HANDLE) {
        osc_diag_set(c->d, OSC_DIAG_TYPE_MISMATCH, hn->line, hn->col, name, c->sym[s].line, p->name,
                     "pool index not a handle", "%s of pool '%s' takes a handle of that pool; '%s' is not a handle",
                     what, p->name, name);
        return -1;
    }
    if (c->sym[s].pool != ps) {
        osc_diag_set(c->d, OSC_DIAG_WRONG_POOL, hn->line, hn->col, name, c->sym[s].line, p->name, "handle of another pool",
                     "'%s' is a handle of pool '%s', used with pool '%s'", name, c->sym[c->sym[s].pool].name, p->name);
        return -1;
    }
    return s;
}

/* use of handle hs (read wr = 0 / write wr = 1): refused when its provenance
 * was freed on every path here. The refused event is the model's HANDLE_USE
 * (stale-generation). */
static int handle_use(C *c, const OscNode *at, int hs, int wr)
{
    Sym *h = &c->sym[hs];
    int pv = h->prov;
    if (pv < 0) return 0;
    if (c->pfreed[pv]) {
        int copy = c->pfree_sym[pv] != hs;
        char tr[64];
        snprintf(tr, sizeof tr, "%s after free%s", wr ? "write" : "read", copy ? " via a copy" : "");
        tr_slot(c, OSC_EV_HANDLE_USE, 0, c->pmhnd[pv], wr ? 2 : 1, at->line, 1);
        osc_diag_set(c->d, OSC_DIAG_STALE_HANDLE, at->line, at->col, h->name, c->pfree_line[pv],
                     copy ? c->sym[c->pfree_sym[pv]].name : NULL, tr,
                     "'%s' is stale: its slot was freed at line %u%s%s (generation mismatch)", h->name,
                     c->pfree_line[pv], copy ? " through " : "", copy ? c->sym[c->pfree_sym[pv]].name : "");
        return -1;
    }
    tr_slot(c, OSC_EV_HANDLE_USE, 0, c->pmhnd[pv], wr ? 2 : 1, at->line, 0);
    return 0;
}

/* `P.alloc(e)` (ON_PALLOC an) for a handle of pool ps: returns the new
 * provenance. Definite allocations (same loop and branch depth as the pool)
 * are counted against K live slots and the generation budget. */
static int chk(C *c, int i, OscScalar want);
static int handle_alloc(C *c, OscNode *an, int ps)
{
    int as = pool_sym(c, an);
    if (as < 0) return -1;
    Sym *p = &c->sym[ps];
    if (as != ps) {
        osc_diag_set(c->d, OSC_DIAG_WRONG_POOL, an->line, an->col, c->sym[as].name, c->sym[as].line, p->name,
                     "handle of another pool", "'%s.alloc' makes a handle of pool '%s', bound as a handle of pool '%s'",
                     c->sym[as].name, c->sym[as].name, p->name);
        return -1;
    }
    an->sym = ps;
    if (slice_noescape(c, an->a) || chk(c, an->a, p->ty.s) < 0) return -1;
    if (c->loop_depth == p->loop_depth && c->cond_depth == p->cond_depth) {
        if (!p->cap_off && p->dlive + 1u > p->cap) {
            osc_diag_set(c->d, OSC_DIAG_POOL_CAPACITY, an->line, an->col, p->name, p->line, NULL, "pool full",
                         "allocation needs %u live slots, pool '%s' has %u", p->dlive + 1u, p->name, p->cap);
            return -1;
        }
        /* each slot serves generations gbase..UINT64_MAX: K * (2^64 - gbase) allocations in all */
        uint64_t per = UINT64_MAX - p->gbase; /* + 1 */
        if (per < (1ull << 32) && p->dallocs + 1 > (uint64_t)p->cap * (per + 1)) {
            osc_diag_set(c->d, OSC_DIAG_GENERATION_EXHAUSTED, an->line, an->col, p->name, p->line, NULL,
                         "retired slot reused",
                         "allocation %llu from pool '%s' exceeds its generation budget (%u slots x %llu generations "
                         "from base %llu); every slot is live or retired",
                         (unsigned long long)(p->dallocs + 1), p->name, p->cap, (unsigned long long)(per + 1),
                         (unsigned long long)p->gbase);
            return -1;
        }
        p->dallocs++;
        if (!p->cap_off) p->dlive++;
    }
    if (c->nprov >= MAX_PROV) return cap_fail(c, an->line, "handle provenance capacity");
    int pv = c->nprov++;
    c->pfreed[pv] = 0;
    c->pfree_line[pv] = 0;
    c->pfree_sym[pv] = -1;
    c->pmslot[pv] = ++c->nmslot;
    c->pmhnd[pv] = ++c->nmhnd;
    tr_slot(c, OSC_EV_SLOT_ALLOC, c->pmslot[pv], c->pmhnd[pv], 3, an->line, 0);
    return pv;
}

/* value of a handle binding (let or assign): P.alloc(e) or a handle of P.
 * Returns the provenance (-1 unknown) or -2 on error. */
static int handle_value(C *c, OscNode *v, int ps, const char *target)
{
    if (v->kind == ON_PALLOC) {
        int pv = handle_alloc(c, v, ps);
        return pv < 0 ? -2 : pv;
    }
    if (v->kind == ON_NAME) {
        int s = handle_operand(c, v, ps, "a handle binding");
        if (s < 0) return -2;
        v->sym = s;
        return c->sym[s].prov;
    }
    char msg[200];
    snprintf(msg, sizeof msg, "handle '%s' takes '%s.alloc(e)' or another handle of pool '%s'", target,
             c->sym[ps].name, c->sym[ps].name);
    return tmismatch(c, v, target, msg) ? -2 : -2;
}

/* "[u8; 4]" for an array ref type, the struct name for a struct ref type */
static const char *rty(C *c, const OscType *t, char *buf, size_t cap)
{
    if (t->sid) snprintf(buf, cap, "%s", c->ast->structs[t->sid - 1].name);
    else snprintf(buf, cap, "[%s; %u]", tname(t->elem), t->len);
    return buf;
}

static int chk_index_len(C *c, const char *name, uint32_t dline, unsigned len, int ix, const OscNode *at);

/* index expression + static bounds; arr symbol s */
static int chk_index(C *c, int s, int ix, const OscNode *at)
{
    return chk_index_len(c, c->sym[s].name, c->sym[s].line, c->sym[s].ty.len, ix, at);
}

static int chk_index_len(C *c, const char *name, uint32_t dline, unsigned len, int ix, const OscNode *at)
{
    OscScalar it;
    if (ctxfree(c, ix)) it = (OscScalar)chk(c, ix, OSC_T_I64);
    else it = (OscScalar)chk(c, ix, 0);
    if ((int)it < 0) return -1;
    if (!osc_scalar_is_int(it)) return tmismatch(c, NODE(ix), tname(it), "array index must be an integer");
    const OscNode *x = NODE(ix);
    if (x->is_const) {
        int64_t v = osc_scalar_signed(it) ? (int64_t)x->cval : (int64_t)(x->cval > (uint64_t)INT64_MAX ? -1 : (int64_t)x->cval);
        int oob = osc_scalar_signed(it) ? (v < 0 || v >= (int64_t)len)
                                        : (x->cval >= (uint64_t)len);
        if (oob) {
            char other[64];
            if (osc_scalar_signed(it)) snprintf(other, sizeof other, "index %lld", (long long)v);
            else snprintf(other, sizeof other, "index %llu", (unsigned long long)x->cval);
            osc_diag_set(c->d, OSC_DIAG_STATIC_OUT_OF_BOUNDS, at->line, at->col, name, dline, other,
                         "index outside 0..N-1", "constant %s outside 0..%u of '%s'", other,
                         len - 1u, name);
            return -1;
        }
    }
    return 0;
}

/* OSC-2 structs: resolve the field of ON_FIELD / ON_FSTORE node n (base symbol
 * s, already resolved), check its index, set n->hi. Returns the field or NULL. */
static const OscField *chk_field(C *c, OscNode *n, int s)
{
    char msg[320], fname[64];
    Sym *y = &c->sym[s];
    osc_tok_text(c->ast->src, &c->ast->toks[n->lo], fname, sizeof fname);
    if (y->kind == SK_SCALAR || !y->ty.sid) {
        snprintf(msg, sizeof msg, "'%s' is %s, not a struct ('.%s')", y->name,
                 y->kind == SK_SCALAR ? "a scalar" : "an array", fname);
        tmismatch(c, n, y->name, msg);
        return NULL;
    }
    const OscStruct *st = &c->ast->structs[y->ty.sid - 1];
    int f = -1;
    for (int k = 0; k < st->nfields; k++)
        if (!strcmp(st->fields[k].name, fname)) f = k;
    if (f < 0) {
        const OscToken *ft = &c->ast->toks[n->lo];
        osc_diag_set(c->d, OSC_DIAG_UNKNOWN_FIELD, ft->line, ft->col, fname, c->ast->struct_line[y->ty.sid - 1],
                     st->name, "access unknown field", "struct '%s' (of '%s') has no field '%s'", st->name, y->name,
                     fname);
        return NULL;
    }
    n->hi = f;
    const OscField *fd = &st->fields[f];
    if (fd->alen && n->a < 0) {
        snprintf(msg, sizeof msg, "array field '%s.%s' is used element by element ('%s.%s[i]')", y->name, fname,
                 y->name, fname);
        tmismatch(c, n, y->name, msg);
        return NULL;
    }
    if (!fd->alen && n->a >= 0) {
        snprintf(msg, sizeof msg, "field '%s.%s' is a scalar, not an array", y->name, fname);
        tmismatch(c, n, y->name, msg);
        return NULL;
    }
    if (fd->alen) {
        char qn[64];
        snprintf(qn, sizeof qn, "%.30s.%.30s", y->name, fname);
        if (chk_index_len(c, qn, y->line, fd->alen, n->a, n)) return NULL;
    }
    return fd;
}

static int chk(C *c, int i, OscScalar want)
{
    OscNode *n = NODE(i);
    char msg[320];
    if (ctxfree(c, i) && !want) {
        osc_diag_set(c->d, OSC_DIAG_AMBIGUOUS_WIDTH, n->line, n->col, c->sobj, 0, NULL, "literal without type context",
                     "integer literal has no type from context (ambiguous width)");
        return -1;
    }
    if (want && constexpr_(c, i)) {
        i128 v;
        if (fold(c, i, want, &v)) return -1;
        return (int)want;
    }
    OscScalar t;
    switch (n->kind) {
    case ON_INT: { i128 v; if (fold(c, i, want, &v)) return -1; return (int)want; } /* unreachable: constexpr */
    case ON_BOOL:
        t = OSC_T_BOOL;
        break;
    case ON_NAME: {
        if (contract_name(c, n)) return -1;
        int s = resolve(c, n);
        if (s < 0) return -1;
        n->sym = s;
        if (c->sym[s].kind == SK_SLICE)
            return slice_diag(c, n, OSC_DIAG_SLICE_VALUE, &c->sym[s], "slice used as a value",
                              "a slice is used only as 'name[i]', 'name.len' or whole as a slice argument");
        if (c->sym[s].kind != SK_SCALAR) {
            snprintf(msg, sizeof msg, "'%s' is %s %s, not a scalar value", c->sym[s].name,
                     c->sym[s].ty.sid ? "a struct" : "an array", c->sym[s].kind == SK_OWNER ? "owner" : "borrow");
            return tmismatch(c, n, c->sym[s].name, msg);
        }
        t = c->sym[s].ty.s;
        break;
    }
    case ON_HLOAD:
    case ON_INDEX: {
        if (contract_name(c, n)) return -1;
        int ps = pool_named(c, n);
        if (ps >= 0) { /* OSC-3 item 2: P[h] reads the slot's value */
            int hs = handle_operand(c, NODE(n->a), ps, "a read");
            if (hs < 0) return -1;
            n->kind = ON_HLOAD;
            n->sym = ps;
            n->sym2 = hs;
            NODE(n->a)->sym = hs;
            if (handle_use(c, n, hs, 0)) return -1;
            t = c->sym[ps].ty.s;
            break;
        }
        int s = resolve(c, n);
        if (s < 0) return -1;
        n->sym = s;
        if (c->sym[s].kind == SK_SLICE) { /* checked at run time against the length register; result is u64 */
            if (c->contract == 2 && c->sym[s].ty.s == OSC_T_CELLS) {
                osc_diag_set(c->d, OSC_DIAG_CONTRACT_INVALID, n->line, n->col, c->sym[s].name, c->sym[s].line, NULL,
                             "cells read in ensures", "an ensures clause may not read the written slice '%s'", c->sym[s].name);
                return -1;
            }
            int r = chk(c, n->a, ctxfree(c, n->a) ? OSC_T_I64 : 0);
            if (r < 0) return -1;
            if (!osc_scalar_is_int((OscScalar)r)) return tmismatch(c, NODE(n->a), tname((OscScalar)r), "slice index must be an integer");
            t = OSC_T_U64;
            break;
        }
        if (c->contract == 2 && c->sym[s].kind != SK_SCALAR && !(c->sym[s].kind == SK_BORROW && c->sym[s].ty.ref == OSC_REF_SHARED)) {
            osc_diag_set(c->d, OSC_DIAG_CONTRACT_INVALID, n->line, n->col, c->sym[s].name, c->sym[s].line, NULL,
                         "array read in ensures",
                         "an ensures clause may read elements only through a shared '&' parameter ('%s' is not one)",
                         c->sym[s].name);
            return -1;
        }
        if (c->sym[s].kind == SK_SCALAR || c->sym[s].ty.sid) {
            snprintf(msg, sizeof msg, "'%s' is a %s, not an array", c->sym[s].name,
                     c->sym[s].ty.sid ? "struct" : "scalar");
            return tmismatch(c, n, c->sym[s].name, msg);
        }
        if (chk_index(c, s, n->a, n)) return -1;
        if (access(c, s, 0, n->line, n->col)) return -1;
        t = c->sym[s].ty.elem;
        break;
    }
    case ON_FIELD: {
        if (contract_name(c, n)) return -1;
        int s = resolve(c, n);
        if (s < 0) return -1;
        n->sym = s;
        if (c->contract == 2 && c->sym[s].kind != SK_SCALAR &&
            !(c->sym[s].kind == SK_BORROW && c->sym[s].ty.ref == OSC_REF_SHARED)) {
            osc_diag_set(c->d, OSC_DIAG_CONTRACT_INVALID, n->line, n->col, c->sym[s].name, c->sym[s].line, NULL,
                         "struct field read in ensures",
                         "an ensures clause may read fields only through a shared '&' parameter ('%s' is not one)",
                         c->sym[s].name);
            return -1;
        }
        if (c->sym[s].kind == SK_SLICE) { /* NAME.len */
            char fname[64];
            osc_tok_text(c->ast->src, &c->ast->toks[n->lo], fname, sizeof fname);
            if (strcmp(fname, "len") != 0 || n->a >= 0)
                return slice_diag(c, n, OSC_DIAG_SLICE_VALUE, &c->sym[s], "slice member", "the only member of a slice is '.len'");
            t = OSC_T_U64;
            break;
        }
        const OscField *fd = chk_field(c, n, s);
        if (!fd) return -1;
        if (access(c, s, 0, n->line, n->col)) return -1;
        t = fd->s;
        break;
    }
    case ON_CALL: {
        if (c->contract) {
            char cn[64];
            osc_node_name(c->ast, n, cn, sizeof cn);
            osc_diag_set(c->d, OSC_DIAG_CONTRACT_INVALID, n->line, n->col, cn, 0, c->contract == 1 ? "requires" : "ensures",
                         "call in contract", "a contract clause may not call a function ('%s')", cn);
            return -1;
        }
        int r = chk_call(c, i, want, 0);
        if (r < 0) return -1;
        t = (OscScalar)r;
        break;
    }
    case ON_BORROW:
        return tmismatch(c, n, NULL, "a borrow is not a value here");
    case ON_UN: {
        if (n->op == OT_BANG) {
            if (chk(c, n->a, OSC_T_BOOL) < 0) return -1;
            t = OSC_T_BOOL;
            break;
        }
        int r = chk(c, n->a, ctxfree(c, n->a) ? want : 0);
        if (r < 0) return -1;
        t = (OscScalar)r;
        if (!osc_scalar_is_int(t)) return tmismatch(c, n, tname(t), "'-' and '~' need an integer operand");
        break;
    }
    case ON_CAST: {
        if (ctxfree(c, n->a)) {
            osc_diag_set(c->d, OSC_DIAG_AMBIGUOUS_WIDTH, NODE(n->a)->line, NODE(n->a)->col, c->sobj, 0, NULL,
                         "literal as 'as' source", "the source of 'as' must not be a bare literal (ambiguous width)");
            return -1;
        }
        int r = chk(c, n->a, 0);
        if (r < 0) return -1;
        if (!osc_scalar_is_int((OscScalar)r) || !osc_scalar_is_int(n->ty.s)) {
            snprintf(msg, sizeof msg, "'as' converts between integer types only (%s as %s)", tname((OscScalar)r),
                     tname(n->ty.s));
            return tmismatch(c, n, tname((OscScalar)r), msg);
        }
        t = n->ty.s;
        break;
    }
    case ON_BIN: {
        int op = n->op;
        if (op == OT_ANDAND || op == OT_OROR) {
            if (chk(c, n->a, OSC_T_BOOL) < 0 || chk(c, n->b, OSC_T_BOOL) < 0) return -1;
            t = OSC_T_BOOL;
            break;
        }
        if (is_cmpop(op)) {
            OscScalar T;
            if (ctxfree(c, n->a) && ctxfree(c, n->b)) {
                osc_diag_set(c->d, OSC_DIAG_AMBIGUOUS_WIDTH, n->line, n->col, c->sobj, 0, NULL,
                             "literal without type context", "comparison of two literals (ambiguous width)");
                return -1;
            }
            if (ctxfree(c, n->a)) {
                int r = chk(c, n->b, 0);
                if (r < 0) return -1;
                T = (OscScalar)r;
                if (T == OSC_T_BOOL) return tmismatch(c, NODE(n->a), "bool", "integer literal compared with bool");
                if (chk(c, n->a, T) < 0) return -1;
            } else {
                int r = chk(c, n->a, 0);
                if (r < 0) return -1;
                T = (OscScalar)r;
                if (T == OSC_T_BOOL && ctxfree(c, n->b))
                    return tmismatch(c, NODE(n->b), "bool", "integer literal compared with bool");
                if (chk(c, n->b, T) < 0) return -1;
            }
            n->flag = (uint8_t)T; /* operand type for lowering */
            t = OSC_T_BOOL;
            break;
        }
        if (is_shift(op)) {
            int r = chk(c, n->a, ctxfree(c, n->a) ? want : 0);
            if (r < 0) return -1;
            OscScalar T = (OscScalar)r;
            if (!osc_scalar_is_int(T)) return tmismatch(c, n, tname(T), "shift of a non-integer");
            int ra;
            if (ctxfree(c, n->b)) ra = chk(c, n->b, T);
            else ra = chk(c, n->b, 0);
            if (ra < 0) return -1;
            if (!osc_scalar_is_int((OscScalar)ra)) return tmismatch(c, NODE(n->b), tname((OscScalar)ra), "shift amount must be an integer");
            const OscNode *bn = NODE(n->b);
            if (bn->is_const) {
                unsigned w = osc_scalar_width(T);
                int neg = osc_scalar_signed((OscScalar)ra) && (int64_t)bn->cval < 0;
                if (neg || bn->cval >= w) {
                    snprintf(msg, sizeof msg, "constant shift amount outside 0..%u for %s", w - 1, tname(T));
                    return ovf(c, bn, "literal shift >= width", msg);
                }
            }
            t = T;
            break;
        }
        /* arithmetic / bitwise */
        OscScalar T;
        if (ctxfree(c, n->a) && ctxfree(c, n->b)) {
            /* only reachable with want (else ambiguous above) and non-constant leaves */
            int r = chk(c, n->a, want);
            if (r < 0) return -1;
            T = (OscScalar)r;
            if (chk(c, n->b, T) < 0) return -1;
        } else if (ctxfree(c, n->a)) {
            int r = chk(c, n->b, 0);
            if (r < 0) return -1;
            T = (OscScalar)r;
            if (T == OSC_T_BOOL) return tmismatch(c, NODE(n->b), "bool", "arithmetic on bool");
            if (chk(c, n->a, T) < 0) return -1;
        } else {
            int r = chk(c, n->a, 0);
            if (r < 0) return -1;
            T = (OscScalar)r;
            if (T == OSC_T_BOOL) return tmismatch(c, NODE(n->a), "bool", "arithmetic on bool");
            if (chk(c, n->b, T) < 0) return -1;
        }
        if (!osc_scalar_is_int(T)) return tmismatch(c, n, tname(T), "arithmetic needs integer operands");
        if ((op == OT_SLASH || op == OT_PERCENT) && NODE(n->b)->is_const && NODE(n->b)->cval == 0)
            return ovf(c, NODE(n->b), "literal zero divisor", "division by a constant zero");
        t = T;
        break;
    }
    case ON_PALLOC: /* OSC-3 item 2 */
        return tmismatch(c, n, NULL, "'P.alloc(e)' makes a handle; bind it with 'let NAME: handle P = P.alloc(e);'");
    default:
        return tmismatch(c, n, NULL, "unexpected expression");
    }
    n->ty.s = t;
    if (want_ok(c, n, t, want)) return -1;
    return (int)t;
}

static int find_fn(C *c, const char *name, int *after)
{
    *after = 0;
    for (uint32_t k = 0; k < c->ast->nfns; k++) {
        char fn[64];
        osc_node_name(c->ast, NODE(c->ast->fns[k]), fn, sizeof fn);
        if (strcmp(fn, name) == 0) {
            if ((int)k < c->fi) return (int)k;
            *after = 1;
            return -1;
        }
    }
    return -1;
}

static int same_arr(const OscType *a, const OscType *b)
{
    return a->elem == b->elem && a->len == b->len && a->sid == b->sid;
}

/* OSC-2: check fn fi's requires / ensures clauses (after its parameters are
 * declared, before its body). See docs/osc/OSC-2-DESIGN.md section 1. */
static int chk_contracts(C *c, int fi)
{
    OscAst *a = c->ast;
    char fname[64];
    osc_node_name(a, NODE(c->fnode), fname, sizeof fname);
    CEnv none;
    memset(&none, 0, sizeof none);
    none.res_sym = -1;
    uint64_t v;
    if (a->reqn[fi] >= 0) {
        int r = a->reqn[fi];
        c->contract = 1;
        snprintf(c->sobj, sizeof c->sobj, "requires");
        /* element reads in a requires clause are uses at function entry (traced) */
        if (chk(c, r, OSC_T_BOOL) < 0) return -1;
        c->contract = 0;
        if (ceval(c, r, &none, &v)) {
            if (!v) {
                osc_diag_set(c->d, OSC_DIAG_CONTRACT_VIOLATION, NODE(r)->line, NODE(r)->col, fname, NODE(r)->line,
                             a->req[fi], "requires is constant false",
                             "the requires clause of '%s' is false for every call", fname);
                return -1;
            }
            a->req_elide[fi] = 1;
        }
    }
    if (a->ensn[fi] >= 0) {
        int e = a->ensn[fi];
        OscTrace *tr = c->tr;
        if (push_scope(c, NODE(e)->line)) return -1;
        if (lookup(c, "result") >= 0) { /* any fn with ensures: `result` is reserved there */
            int p = lookup(c, "result");
            osc_diag_set(c->d, OSC_DIAG_REDEFINED_NAME, NODE(e)->line, NODE(e)->col, "result", c->sym[p].line,
                         fname, "parameter named result with ensures",
                         "'result' names the return value in an ensures clause; parameter 'result' of '%s' "
                         "collides with it", fname);
            return -1;
        }
        if (c->fret.s != OSC_T_VOID) {
            /* the `result` pseudo binding, visible only inside the clause */
            if (c->nsym >= OSC_CHECK_MAX_SYMS) return cap_fail(c, NODE(e)->line, "symbol capacity");
            int s = c->nsym++;
            Sym *y = &c->sym[s];
            memset(y, 0, sizeof *y);
            snprintf(y->name, sizeof y->name, "result");
            y->line = NODE(e)->line;
            y->kind = SK_SCALAR;
            y->ty = c->fret;
            y->obj = y->bor = -1;
            c->vis[c->nvis++] = s;
            a->res_sym[fi] = s;
        }
        c->contract = 2;
        snprintf(c->sobj, sizeof c->sobj, "ensures");
        /* ensures reads happen at each return; only shared-borrow parameters may
         * be read, which no body operation can invalidate, so they are not traced */
        c->tr = NULL;
        int rc = chk(c, e, OSC_T_BOOL);
        c->tr = tr;
        c->contract = 0;
        if (rc < 0) return -1;
        pop_scope(c);
        if (ceval(c, e, &none, &v)) {
            if (!v) {
                osc_diag_set(c->d, OSC_DIAG_CONTRACT_VIOLATION, NODE(e)->line, NODE(e)->col, fname, NODE(e)->line,
                             a->ens[fi], "ensures is constant false",
                             "the ensures clause of '%s' is false for every return", fname);
                return -1;
            }
            a->ens_elide[fi] = 1;
        }
    }
    snprintf(c->sobj, sizeof c->sobj, "%s", fname);
    return 0;
}

/* OSC-2: refuse a call whose literal / constant arguments make the callee's
 * requires clause fold to false. */
static int chk_call_requires(C *c, int i, int g, const int *args, int na)
{
    OscAst *a = c->ast;
    if (a->reqn[g] < 0 || a->req_elide[g]) return 0;
    const OscNode *n = NODE(i);
    const OscNode *f = NODE(a->fns[g]);
    CEnv env, none;
    memset(&env, 0, sizeof env);
    memset(&none, 0, sizeof none);
    env.res_sym = none.res_sym = -1;
    env.nk = na;
    int k = 0;
    for (int p = f->a; p >= 0 && k < na; p = NODE(p)->next, k++)
        if (NODE(p)->ty.s != OSC_T_REF && NODE(p)->ty.s != OSC_T_BYTES && NODE(p)->ty.s != OSC_T_CELLS && ceval(c, args[k], &none, &env.v[k])) env.k[k] = 1;
    uint64_t v;
    if (ceval(c, a->reqn[g], &env, &v) && !v) {
        char name[64];
        osc_node_name(a, n, name, sizeof name);
        osc_diag_set(c->d, OSC_DIAG_CONTRACT_VIOLATION, n->line, n->col, name, NODE(a->reqn[g])->line, a->req[g],
                     "requires false at call", "these constant arguments make the requires clause of '%s' false",
                     name);
        return -1;
    }
    return 0;
}

static int chk_call(C *c, int i, OscScalar want, int as_stmt)
{
    OscNode *n = NODE(i);
    char name[64], msg[160];
    osc_node_name(c->ast, n, name, sizeof name);
    int after;
    int g = find_fn(c, name, &after);
    if (g < 0) {
        osc_diag_set(c->d, OSC_DIAG_UNDEFINED_NAME, n->line, n->col, name, 0, NULL,
                     after ? "call before definition" : "call of undefined function",
                     after ? "'%s' is called before its definition is complete (no recursion, no forward reference)"
                           : "function '%s' is not defined", name);
        return -1;
    }
    n->sym = g;
    const OscNode *f = NODE(c->ast->fns[g]);
    int params[OSC_MAX_PARAMS], np = 0;
    for (int p = f->a; p >= 0; p = NODE(p)->next) params[np++] = p;
    int args[OSC_MAX_PARAMS + 1], na = 0;
    for (int a = n->a; a >= 0; a = NODE(a)->next) {
        if (na > OSC_MAX_PARAMS) break;
        args[na++] = a;
    }
    if (na != np) {
        snprintf(msg, sizeof msg, "'%s' takes %d argument(s), %d given", name, np, na);
        osc_diag_set(c->d, OSC_DIAG_TYPE_MISMATCH, n->line, n->col, name, f->line, NULL, "arity mismatch", "%s", msg);
        return -1;
    }
    int cb[OSC_MAX_PARAMS], ncb = 0;
    for (int k = 0; k < na; k++) {
        OscNode *a = NODE(args[k]);
        const OscType *pt = &NODE(params[k])->ty;
        if (pt->s == OSC_T_BYTES || pt->s == OSC_T_CELLS) { /* a slice goes down whole, by name, to a slice of the same type */
            int ss = slice_name(c, args[k]);
            if (ss < 0) {
                if (a->kind == ON_BORROW) return tmismatch(c, a, name, "borrow passed to a slice parameter");
                if (a->kind == ON_NAME) { int ns = resolve(c, a); if (ns < 0) return -1; }
                return tmismatch(c, a, name, "a slice parameter takes the name of a slice of the same type");
            }
            a->sym = ss;
            if (c->sym[ss].ty.s != pt->s) {
                snprintf(msg, sizeof msg, "'%s' is a %s slice where '%s' takes a %s slice", c->sym[ss].name,
                         c->sym[ss].ty.s == OSC_T_BYTES ? "bytes" : "cells", name, pt->s == OSC_T_BYTES ? "bytes" : "cells");
                return tmismatch(c, a, c->sym[ss].name, msg);
            }
            continue;
        }
        if (pt->s != OSC_T_REF) {
            if (a->kind == ON_BORROW) return tmismatch(c, a, name, "borrow passed to a scalar parameter");
            if (chk(c, args[k], pt->s) < 0) return -1;
            continue;
        }
        if (pt->ref == OSC_REF_OWN) {
            if (a->kind != ON_NAME) return tmismatch(c, a, name, "an 'own' parameter takes an owner name (move)");
            int s = resolve(c, a);
            if (s < 0) return -1;
            a->sym = s;
            if (c->sym[s].kind != SK_OWNER || !same_arr(&c->sym[s].ty, pt)) {
                char tb[72];
                snprintf(msg, sizeof msg, "'%s' is not an owner of %s", c->sym[s].name, rty(c, pt, tb, sizeof tb));
                return tmismatch(c, a, c->sym[s].name, msg);
            }
            if (arena_move_refused(c, a, s, "move into own parameter")) return -1;
            if (do_move(c, s, -1, a->line, a->col)) return -1;
            continue;
        }
        if (a->kind != ON_BORROW || (a->mut != 0) != (pt->ref == OSC_REF_MUT)) {
            snprintf(msg, sizeof msg, "parameter %d of '%s' takes %s", k + 1, name,
                     pt->ref == OSC_REF_MUT ? "'&mut name'" : "'&name'");
            return tmismatch(c, a, name, msg);
        }
        int s = resolve(c, a);
        if (s < 0) return -1;
        a->sym = s;
        if (c->sym[s].kind == SK_SLICE)
            return slice_diag(c, a, OSC_DIAG_SLICE_VALUE, &c->sym[s], "borrow of a slice", "a slice is passed whole by name, never borrowed with '&'");
        if (c->sym[s].kind == SK_SCALAR || !same_arr(&c->sym[s].ty, pt)) {
            char tb[72];
            snprintf(msg, sizeof msg, "'%s' is not %s %s", c->sym[s].name, pt->sid ? "a struct" : "an array",
                     rty(c, pt, tb, sizeof tb));
            return tmismatch(c, a, c->sym[s].name, msg);
        }
        int b = take_borrow(c, s, a->mut, a->line, a->col);
        if (b < 0) return -1;
        cb[ncb++] = b;
    }
    if (chk_call_requires(c, i, g, args, na)) return -1;
    for (int k = ncb - 1; k >= 0; k--) end_bor(c, cb[k], n->line);
    if (as_stmt) return (int)f->ty.s;
    if (f->ty.s == OSC_T_VOID) {
        snprintf(msg, sizeof msg, "'%s' returns nothing and cannot be used as a value", name);
        return tmismatch(c, n, name, msg);
    }
    (void)want;
    return (int)f->ty.s;
}

/* ------------------------------------------------------------ statements */
/* OSC-2 arenas: the arena named by "in NAME" (node an) for an allocation of
 * len cells by owner node n; checks the static capacity rule. Returns the
 * arena symbol or -1. */
static int arena_alloc_site(C *c, const OscNode *n, OscNode *an)
{
    char aname[64], oname[64];
    osc_node_name(c->ast, an, aname, sizeof aname);
    osc_node_name(c->ast, n, oname, sizeof oname);
    int as = lookup(c, aname);
    if (as < 0) {
        osc_diag_set(c->d, OSC_DIAG_UNDEFINED_NAME, an->line, an->col, aname, 0, oname, "allocate in undefined arena",
                     "arena '%s' is not defined in an enclosing scope", aname);
        return -1;
    }
    Sym *y = &c->sym[as];
    if (y->kind != SK_ARENA) {
        osc_diag_set(c->d, OSC_DIAG_TYPE_MISMATCH, an->line, an->col, aname, y->line, oname, "in a non-arena",
                     "'%s' is not an arena", aname);
        return -1;
    }
    an->sym = as;
    unsigned len = n->ty.len;
    if (c->loop_depth == y->loop_depth && c->cond_depth == y->cond_depth) {
        /* definite allocation: runs exactly once per arena lifetime */
        if ((unsigned)y->used + len > y->cap) {
            osc_diag_set(c->d, OSC_DIAG_ARENA_CAPACITY, n->line, n->col, oname, y->line, y->name,
                         "arena capacity exceeded",
                         "'%s' needs %u cell(s) but arena '%s' (bound %u) has %u left", oname, len, y->name,
                         (unsigned)y->cap, (unsigned)(y->cap - y->used));
            return -1;
        }
        y->used = (uint16_t)(y->used + len);
    }
    return as;
}

/* OSC-2 arenas: refuse moving owner symbol s if it is an arena object (slice
 * restriction, stricter than the model, which would let the MOVE inherit the
 * region; no trace event is recorded for it). */
static int arena_move_refused(C *c, const OscNode *at, int s, const char *transition)
{
    Sym *y = &c->sym[s];
    if (y->kind != SK_OWNER || !c->obj[y->obj].arena1) return 0;
    const Sym *ar = &c->sym[c->obj[y->obj].arena1 - 1];
    osc_diag_set(c->d, OSC_DIAG_ARENA_MOVE, at->line, at->col, y->name, y->line, ar->name, transition,
                 "'%s' lives in arena '%s' and cannot be moved (arena objects are never moved in this slice)",
                 y->name, ar->name);
    return -1;
}

/* arena symbol + 1 of the object borrow b refers to (0 = not an arena object) */
static int bor_arena1(C *c, int b) { return c->obj[c->bor[b].obj].arena1; }

static int chk_block(C *c, int i, int new_scope);
static int chk_stmt(C *c, int i);

static void set_sobj_tok(C *c, const OscNode *n) { osc_node_name(c->ast, n, c->sobj, sizeof c->sobj); }

/* borrow node bn into a borrow of type ty (SHARED/MUT). Returns borrow index. */
static int chk_borrow_src(C *c, OscNode *bn, const OscType *ty, int *src)
{
    char msg[320];
    int s = resolve(c, bn);
    if (s < 0) return -1;
    bn->sym = s;
    *src = s;
    if (c->sym[s].kind == SK_SLICE) return slice_diag(c, bn, OSC_DIAG_SLICE_VALUE, &c->sym[s], "borrow of a slice", "a slice is passed whole by name, never borrowed with '&'");
    if (c->sym[s].kind == SK_SCALAR) {
        snprintf(msg, sizeof msg, "'%s' is a scalar; only arrays are borrowed", c->sym[s].name);
        return tmismatch(c, bn, c->sym[s].name, msg);
    }
    if (!same_arr(&c->sym[s].ty, ty) || (bn->mut != 0) != (ty->ref == OSC_REF_MUT)) {
        char tb[72];
        snprintf(msg, sizeof msg, "borrow does not match %s%s", ty->ref == OSC_REF_MUT ? "&mut " : "&",
                 rty(c, ty, tb, sizeof tb));
        return tmismatch(c, bn, c->sym[s].name, msg);
    }
    return 0;
}

/* `return &x;` or a borrow assigned to an outer binding from a later-declared source */
static int outlives(C *c, const OscNode *at, int s, int b, const char *who, const char *transition)
{
    Sym *y = &c->sym[s];
    if (y->kind == SK_OWNER) tr_ev(c, OSC_EV_RELEASE, y->obj, -1, -1, -1, at->line, 1);
    else tr_ev(c, OSC_EV_END_BORROW, -1, -1, y->bor, -1, at->line, 1);
    (void)b;
    char msg[320];
    snprintf(msg, sizeof msg, "borrow of '%s' would outlive it (%s)", y->name, transition);
    osc_diag_set(c->d, OSC_DIAG_BORROW_OUTLIVES_OWNER, at->line, at->col, y->name, y->line, who, transition, "%s", msg);
    return -1;
}

static int chk_if(C *c, int i)
{
    OscNode *n = NODE(i);
    snprintf(c->sobj, sizeof c->sobj, "if");
    if (chk(c, n->a, OSC_T_BOOL) < 0) return -1;
    Snap *s0 = snap_take(c);
    if (!s0) return cap_fail(c, n->line, "checker memory");
    int nobj0 = c->nobj;
    c->cond_depth++; /* OSC-2 arenas: allocations in a branch are not definite */
    int rc = -1;
    Snap *s1 = NULL;
    HSave *h0 = hs_take(c), *h1 = NULL; /* OSC-3 item 2 */
    if (!h0) { cap_fail(c, n->line, "checker memory"); goto out; }
    tr_save(c, n->line);
    if (chk_block(c, n->b, 1)) goto out;
    int t1 = c->term;
    s1 = snap_take(c);
    h1 = hs_take(c);
    if (!s1 || !h1) { cap_fail(c, n->line, "checker memory"); goto out; }
    tr_restore(c, n->line);
    snap_restore(c, s0);
    hs_restore(c, h0);
    c->term = 0;
    int t2 = 0;
    if (n->c >= 0) {
        tr_save(c, n->line);
        if (NODE(n->c)->kind == ON_IF) { if (chk_if(c, n->c)) goto out; }
        else if (chk_block(c, n->c, 1)) goto out;
        t2 = c->term;
        tr_restore(c, n->line);
    }
    /* state after else is in c; after then is s1 */
    if (t1 && t2) {
        c->term = 1;
        rc = 0;
        goto out;
    }
    if (t2) {
        snap_restore(c, s1);
        hs_restore(c, h1);
    } else if (!t1) {
        /* OSC-3 item 2: freed only if freed on both paths */
        for (int p = 0; p < h0->nprov; p++) c->pfreed[p] = (uint8_t)(c->pfreed[p] && h1->pfreed[p]);
        /* OSC-3 item 3 (drop flags): an owner moved on one path and not (or
         * only maybe) on the other is maybe-moved after the `if`. Its
         * declaration gets a drop flag: set at the declaration, cleared at
         * every move, tested by its scope-end release. Both paths moved:
         * definitely moved (no flag). Neither: unchanged. Pre-existing
         * objects are never OM_GONE on a path that falls through. */
        for (int o = 0; o < nobj0; o++) {
            int m1 = s1->obj[o].moved, m2 = c->obj[o].moved;
            if (m1 == m2) continue;
            if (m2 == OM_LIVE || (m2 == OM_MOVED && m1 == OM_MAYBE)) c->obj[o].move_line = s1->obj[o].move_line;
            c->obj[o].moved = OM_MAYBE;
            int sym = c->obj[o].sym;
            if (sym >= 0) NODE(c->sym[sym].decl)->dflag = 1;
        }
    }
    c->term = 0;
    /* re-emit moves that hold after the if */
    for (int o = 0; o < nobj0; o++)
        if (c->obj[o].moved == 1 && s0->obj[o].moved != 1)
            tr_ev(c, OSC_EV_MOVE, o, -1, -1, -1, c->obj[o].move_line, 0);
    hs_settle(c, h0, t1 || t2 ? NULL : h1, n->line);
    rc = 0;
out:
    c->cond_depth--;
    free(s0);
    free(s1);
    free(h0);
    free(h1);
    return rc;
}

static int chk_loop_body(C *c, OscNode *n, int is_for)
{
    Snap *s0 = snap_take(c);
    if (!s0) return cap_fail(c, n->line, "checker memory");
    HSave *h0 = hs_take(c); /* OSC-3 item 2 */
    if (!h0) { free(s0); return cap_fail(c, n->line, "checker memory"); }
    int rc = -1;
    tr_save(c, n->line);
    c->loop_depth++;
    if (is_for) {
        if (push_scope(c, n->line)) goto out;
        OscType ti = {OSC_T_I64, OSC_REF_NONE, OSC_T_VOID, 0, 0};
        int s = declare(c, n, SK_SCALAR, &ti, 0);
        if (s < 0) goto out;
        n->sym = s;
        if (chk_block(c, n->b, 1)) goto out;
        pop_scope(c);
    } else {
        snprintf(c->sobj, sizeof c->sobj, "while");
        if (chk(c, n->a, OSC_T_BOOL) < 0) goto out;
        if (chk_block(c, n->b, 1)) goto out;
    }
    c->loop_depth--;
    tr_restore(c, n->line);
    snap_restore(c, s0);
    {
        /* OSC-3 item 2: frees in the body are not definite after the loop;
         * handles assigned in the body become unknown */
        int keep[OSC_CHECK_MAX_SYMS];
        for (int k = 0; k < h0->nsym; k++) keep[k] = c->sym[k].prov;
        hs_restore(c, h0);
        for (int k = 0; k < h0->nsym; k++) c->sym[k].prov = keep[k];
        hs_settle(c, h0, NULL, n->line);
    }
    c->term = 0;
    rc = 0;
out:
    free(s0);
    free(h0);
    return rc;
}

/* OSC-2: `return E;` with E constant: fold the ensures clause with `result` = E.
 * False -> static postcondition violation; true -> no check at this return
 * (ON_RETURN flag = 1). */
static int chk_return_ensures(C *c, OscNode *n)
{
    OscAst *a = c->ast;
    int fi = c->fi;
    if (a->ensn[fi] < 0 || a->ens_elide[fi] || a->res_sym[fi] < 0) return 0;
    CEnv env;
    memset(&env, 0, sizeof env);
    env.res_sym = -1;
    uint64_t rv, v;
    if (!ceval(c, n->a, &env, &rv)) return 0;
    env.res_sym = a->res_sym[fi];
    env.res_known = 1;
    env.res_val = rv;
    if (!ceval(c, a->ensn[fi], &env, &v)) return 0;
    if (!v) {
        char fname[64];
        osc_node_name(a, NODE(c->fnode), fname, sizeof fname);
        osc_diag_set(c->d, OSC_DIAG_CONTRACT_VIOLATION, n->line, n->col, fname, NODE(a->ensn[fi])->line, a->ens[fi],
                     "ensures false at return", "this constant return value makes the ensures clause of '%s' false",
                     fname);
        return -1;
    }
    n->flag = 1;
    return 0;
}

static int chk_stmt(C *c, int i)
{
    OscNode *n = NODE(i);
    char msg[320];
    switch (n->kind) {
    case ON_LET: {
        set_sobj_tok(c, n);
        char name[64];
        osc_node_name(c->ast, n, name, sizeof name);
        int prev = lookup(c, name);
        if (prev >= 0) return declare(c, n, SK_SCALAR, &n->ty, n->mut) < 0 ? -1 : -1;
        if (slice_noescape(c, n->a)) return -1;
        if (chk(c, n->a, n->ty.s) < 0) return -1;
        int s = declare(c, n, SK_SCALAR, &n->ty, n->mut);
        if (s < 0) return -1;
        n->sym = s;
        return 0;
    }
    case ON_LET_ALLOC: {
        set_sobj_tok(c, n);
        char name[64];
        osc_node_name(c->ast, n, name, sizeof name);
        if (lookup(c, name) >= 0) return declare(c, n, SK_OWNER, &n->ty, 0) < 0 ? -1 : -1;
        if (n->ty.sid) {  /* struct literal: field values in source order */
            const OscStruct *st = &c->ast->structs[n->ty.sid - 1];
            for (int fi = n->a; fi >= 0; fi = NODE(fi)->next)
                if (slice_noescape(c, NODE(fi)->a) || chk(c, NODE(fi)->a, st->fields[NODE(fi)->hi].s) < 0) return -1;
        } else if (slice_noescape(c, n->a) || chk(c, n->a, n->ty.elem) < 0) return -1;
        int as = -1;
        if (n->c >= 0 && (as = arena_alloc_site(c, n, NODE(n->c))) < 0) return -1;
        int s = declare(c, n, SK_OWNER, &n->ty, 0);
        if (s < 0) return -1;
        int o = new_obj(c, s, n->line);
        if (o < 0) return -1;
        c->sym[s].obj = o;
        n->sym = s;
        if (as >= 0) {
            c->obj[o].arena1 = as + 1;
            tr_region(c, OSC_EV_ALLOC, o, c->sym[as].region, n->line, 0);
        } else {
            tr_ev(c, OSC_EV_ALLOC, o, -1, -1, -1, n->line, 0);
        }
        return 0;
    }
    case ON_LET_MOVE: {
        set_sobj_tok(c, n);
        char name[64];
        osc_node_name(c->ast, n, name, sizeof name);
        if (lookup(c, name) >= 0) return declare(c, n, SK_OWNER, &n->ty, 0) < 0 ? -1 : -1;
        if (n->c >= 0) {
            osc_diag_set(c->d, OSC_DIAG_ARENA_MOVE, n->line, n->col, name, 0, NULL, "move into arena",
                         "'%s': an owner cannot be moved into an arena (arena objects are created in place)", name);
            return -1;
        }
        OscNode *sn = NODE(n->a);
        int src = resolve(c, sn);
        if (src < 0) return -1;
        sn->sym = src;
        n->sym2 = src;
        if (c->sym[src].kind != SK_OWNER || !same_arr(&c->sym[src].ty, &n->ty)) {
            char tb[72];
            snprintf(msg, sizeof msg, "'%s' is not an owner of %s", c->sym[src].name, rty(c, &n->ty, tb, sizeof tb));
            return tmismatch(c, sn, c->sym[src].name, msg);
        }
        int o = new_obj(c, -1, n->line);
        if (o < 0) return -1;
        if (arena_move_refused(c, sn, src, "move out of arena")) return -1;
        if (do_move(c, src, o, sn->line, sn->col)) return -1;
        int s = declare(c, n, SK_OWNER, &n->ty, 0);
        if (s < 0) return -1;
        c->obj[o].sym = s;
        c->sym[s].obj = o;
        n->sym = s;
        return 0;
    }
    case ON_LET_BORROW: {
        set_sobj_tok(c, n);
        char name[64];
        osc_node_name(c->ast, n, name, sizeof name);
        if (lookup(c, name) >= 0) return declare(c, n, SK_BORROW, &n->ty, n->mut) < 0 ? -1 : -1;
        OscNode *bn = NODE(n->a);
        int src;
        if (chk_borrow_src(c, bn, &n->ty, &src)) return -1;
        n->sym2 = src;
        int b = take_borrow(c, src, bn->mut, bn->line, bn->col);
        if (b < 0) return -1;
        int s = declare(c, n, SK_BORROW, &n->ty, n->mut);
        if (s < 0) return -1;
        c->sym[s].bor = b;
        c->bor[b].sym = s;
        n->sym = s;
        return 0;
    }
    case ON_ASSIGN: {
        set_sobj_tok(c, n);
        {
            /* OSC-3 item 2: h = P.alloc(e) / h = h2 */
            char hn[64];
            osc_node_name(c->ast, n, hn, sizeof hn);
            int hs = lookup(c, hn);
            if (hs >= 0 && c->sym[hs].kind == SK_HANDLE) {
                Sym *y = &c->sym[hs];
                if (!y->mut) {
                    osc_diag_set(c->d, OSC_DIAG_IMMUTABLE_ASSIGN, n->line, n->col, y->name, y->line, NULL,
                                 "assign to immutable binding", "'%s' is not assignable (declare it with 'let mut')",
                                 y->name);
                    return -1;
                }
                if (NODE(n->a)->kind == ON_BORROW) return tmismatch(c, NODE(n->a), y->name, "a borrow cannot be assigned to a handle");
                int pv = handle_value(c, NODE(n->a), y->pool, y->name);
                if (pv == -2) return -1;
                n->sym = hs;
                y->prov = pv;
                return 0;
            }
        }
        int s = resolve(c, n);
        if (s < 0) return -1;
        n->sym = s;
        Sym *y = &c->sym[s];
        if (y->kind == SK_OWNER || !y->mut) {
            snprintf(msg, sizeof msg, "'%s' is not assignable (%s)", y->name,
                     y->kind == SK_OWNER ? "owners are never reassigned" : "declare it with 'let mut'");
            osc_diag_set(c->d, OSC_DIAG_IMMUTABLE_ASSIGN, n->line, n->col, y->name, y->line, NULL,
                         "assign to immutable binding", "%s", msg);
            return -1;
        }
        OscNode *v = NODE(n->a);
        if (y->kind == SK_SCALAR) {
            if (v->kind == ON_BORROW) return tmismatch(c, v, y->name, "a borrow cannot be assigned to a scalar");
            if (slice_noescape(c, n->a)) return -1;
            return chk(c, n->a, y->ty.s) < 0 ? -1 : 0;
        }
        /* borrow binding */
        if (v->kind != ON_BORROW) return tmismatch(c, v, y->name, "a borrow binding takes '&name' or '&mut name'");
        int src;
        if (chk_borrow_src(c, v, &y->ty, &src)) return -1;
        n->sym2 = src;
        if (src == s) {
            osc_diag_set(c->d, OSC_DIAG_UNSUPPORTED, v->line, v->col, y->name, y->line, NULL, "self reborrow",
                         "a borrow binding cannot be reassigned to a reborrow of itself");
            return -1;
        }
        Bor *old = &c->bor[y->bor];
        if (old->kids_shared || old->kids_mut) {
            char other[64];
            conflict_name(c, -1, y->bor, 0, other, sizeof other);
            tr_ev(c, OSC_EV_END_BORROW, -1, -1, y->bor, -1, n->line, 1);
            osc_diag_set(c->d, OSC_DIAG_BORROW_OUTLIVES_OWNER, n->line, n->col, y->name, y->line, other,
                         "end borrow while reborrowed", "'%s' is reassigned while its reborrow %s is live", y->name, other);
            return -1;
        }
        end_bor(c, y->bor, n->line);
        y->bor = -1;
        int b = take_borrow(c, src, v->mut, v->line, v->col);
        if (b < 0) return -1;
        y->bor = b;
        c->bor[b].sym = s;
        int ar1 = bor_arena1(c, b);
        if (ar1 && s < ar1 - 1) /* binding declared before the arena: it would outlive the arena */
            return arena_escape(c, v, ar1 - 1, y->name, "outer borrow assigned a borrow of an arena object");
        if (src > s) /* declared after the binding: destroyed while the binding is still in scope */
            return outlives(c, v, src, b, y->name, "outer borrow assigned a borrow of an inner owner");
        return 0;
    }
    case ON_HSTORE:
    case ON_STORE: {
        set_sobj_tok(c, n);
        int ps = pool_named(c, n);
        if (ps >= 0) { /* OSC-3 item 2: P[h] = e writes the slot's value */
            int hs = handle_operand(c, NODE(n->a), ps, "a write");
            if (hs < 0) return -1;
            n->kind = ON_HSTORE;
            n->sym = ps;
            n->sym2 = hs;
            NODE(n->a)->sym = hs;
            if (slice_noescape(c, n->b) || chk(c, n->b, c->sym[ps].ty.s) < 0) return -1;
            return handle_use(c, n, hs, 1);
        }
        int s = resolve(c, n);
        if (s < 0) return -1;
        n->sym = s;
        Sym *y = &c->sym[s];
        if (y->kind == SK_SLICE) { /* cells[i] = e (u64); bytes are read-only */
            if (y->ty.s == OSC_T_BYTES)
                return slice_diag(c, n, OSC_DIAG_SLICE_WRITE, y, "write to a bytes slice",
                                  "a 'bytes' slice is read-only: there is no way to write an element of it");
            if (slice_noescape(c, n->b)) return -1;
            int r = chk(c, n->a, ctxfree(c, n->a) ? OSC_T_I64 : 0);
            if (r < 0) return -1;
            if (!osc_scalar_is_int((OscScalar)r)) return tmismatch(c, NODE(n->a), tname((OscScalar)r), "slice index must be an integer");
            return chk(c, n->b, OSC_T_U64) < 0 ? -1 : 0;
        }
        if (y->kind == SK_SCALAR || y->ty.sid) {
            snprintf(msg, sizeof msg, "'%s' is a %s, not an array", y->name, y->ty.sid ? "struct" : "scalar");
            return tmismatch(c, n, y->name, msg);
        }
        if (chk_index(c, s, n->a, n)) return -1;
        if (slice_noescape(c, n->b)) return -1;
        if (chk(c, n->b, y->ty.elem) < 0) return -1;
        return access(c, s, 1, n->line, n->col);
    }
    case ON_FSTORE: {
        set_sobj_tok(c, n);
        int s = resolve(c, n);
        if (s < 0) return -1;
        n->sym = s;
        if (c->sym[s].kind == SK_SLICE)
            return slice_diag(c, n, OSC_DIAG_SLICE_WRITE, &c->sym[s], "write to a slice member", "the length of a slice is read-only");
        const OscField *fd = chk_field(c, n, s);
        if (!fd) return -1;
        if (slice_noescape(c, n->b)) return -1;
        if (chk(c, n->b, fd->s) < 0) return -1;
        return access(c, s, 1, n->line, n->col);
    }
    case ON_IF:
        return chk_if(c, i);
    case ON_WHILE:
        return chk_loop_body(c, n, 0);
    case ON_FOR:
        set_sobj_tok(c, n);
        return chk_loop_body(c, n, 1);
    case ON_RETURN: {
        snprintf(c->sobj, sizeof c->sobj, "return");
        if (n->a >= 0 && NODE(n->a)->kind == ON_BORROW) {
            OscNode *bn = NODE(n->a);
            int s = resolve(c, bn);
            if (s < 0) return -1;
            if (c->sym[s].kind == SK_SLICE) return slice_diag(c, bn, OSC_DIAG_SLICE_ESCAPE, &c->sym[s], "return a borrow of a slice", "a slice is never returned");
            if (c->sym[s].kind == SK_SCALAR) return tmismatch(c, bn, c->sym[s].name, "only arrays are borrowed");
            int b = take_borrow(c, s, bn->mut, bn->line, bn->col);
            if (b < 0) return -1;
            if (bor_arena1(c, b)) return arena_escape(c, bn, bor_arena1(c, b) - 1, "return", "borrow of an arena object returned");
            return outlives(c, bn, s, b, "return", "borrow returned from function");
        }
        if (c->fret.s == OSC_T_VOID) {
            if (n->a >= 0) return tmismatch(c, NODE(n->a), NULL, "a function without '->' returns no value");
        } else {
            if (n->a < 0) {
                osc_diag_set(c->d, OSC_DIAG_TYPE_MISMATCH, n->line, n->col, "return", 0, tname(c->fret.s),
                             "type mismatch", "'return;' in a function returning %s", tname(c->fret.s));
                return -1;
            }
            if (slice_noescape(c, n->a)) return -1;
            if (chk(c, n->a, c->fret.s) < 0) return -1;
            if (chk_return_ensures(c, n)) return -1;
        }
        if (scope_exit_events(c, c->depth, n->line, n)) return -1;
        c->term = 1;
        return 0;
    }
    case ON_ARENA: { /* OSC-2: arena NAME bound K { body } */
        set_sobj_tok(c, n);
        if (push_scope(c, n->line)) return -1;
        OscType ta = {OSC_T_U64, OSC_REF_NONE, OSC_T_VOID, 0, 0};
        int s = declare(c, n, SK_ARENA, &ta, 0);
        if (s < 0) return -1;
        Sym *y = &c->sym[s];
        y->region = ++c->nregion;
        y->cap = (uint16_t)n->ival;
        y->used = 0;
        y->cond_depth = c->cond_depth;
        n->sym = s;
        tr_region(c, OSC_EV_REGION_OPEN, -1, y->region, n->line, 0);
        if (chk_block(c, n->b, 1)) return -1;
        /* destroy after the body's own scope exit (its owners and borrows end first) */
        if (!c->term && scope_exit_events(c, 1, (uint32_t)NODE(n->b)->ival, n)) return -1;
        pop_scope(c);
        return 0;
    }
    case ON_POOL: { /* OSC-3 item 2: pool NAME: [T; K] [gen B] { body } */
        set_sobj_tok(c, n);
        if (push_scope(c, n->line)) return -1;
        OscType tp = {n->ty.s, OSC_REF_NONE, OSC_T_VOID, 0, 0};
        int s = declare(c, n, SK_POOL, &tp, 0);
        if (s < 0) return -1;
        Sym *y = &c->sym[s];
        y->cap = (uint16_t)n->ival;
        y->gbase = (uint64_t)n->lo;
        y->cond_depth = c->cond_depth;
        y->dallocs = 0;
        y->dlive = 0;
        y->cap_off = 0;
        n->sym = s;
        if (chk_block(c, n->b, 1)) return -1;
        if (!c->term && scope_exit_events(c, 1, (uint32_t)NODE(n->b)->ival, n)) return -1;
        pop_scope(c);
        return 0;
    }
    case ON_LET_HANDLE: { /* let [mut] h: handle P = P.alloc(e) | h2; */
        set_sobj_tok(c, n);
        char name[64];
        osc_node_name(c->ast, n, name, sizeof name);
        if (lookup(c, name) >= 0) return declare(c, n, SK_HANDLE, &n->ty, n->mut) < 0 ? -1 : -1;
        int ps = pool_sym(c, NODE(n->c));
        if (ps < 0) return -1;
        int pv = handle_value(c, NODE(n->a), ps, name);
        if (pv == -2) return -1;
        OscType th = {OSC_T_U64, OSC_REF_NONE, OSC_T_VOID, 0, 0};
        n->ty = th;
        int s = declare(c, n, SK_HANDLE, &th, n->mut);
        if (s < 0) return -1;
        c->sym[s].pool = ps;
        c->sym[s].prov = pv;
        n->sym = s;
        n->sym2 = ps;
        return 0;
    }
    case ON_PFREE: { /* P.free(h); */
        set_sobj_tok(c, n);
        int ps = pool_sym(c, n);
        if (ps < 0) return -1;
        int hs = handle_operand(c, NODE(n->a), ps, "a free");
        if (hs < 0) return -1;
        n->sym = ps;
        n->sym2 = hs;
        NODE(n->a)->sym = hs;
        Sym *p = &c->sym[ps], *h = &c->sym[hs];
        if (c->loop_depth == p->loop_depth && c->cond_depth == p->cond_depth) {
            if (p->dlive) p->dlive--;
        } else {
            p->cap_off = 1;
        }
        int pv = h->prov;
        if (pv < 0) return 0;
        if (c->pfreed[pv]) {
            int copy = c->pfree_sym[pv] != hs;
            tr_slot(c, OSC_EV_SLOT_FREE, 0, c->pmhnd[pv], 0, n->line, 1);
            osc_diag_set(c->d, OSC_DIAG_STALE_HANDLE, n->line, n->col, h->name, c->pfree_line[pv],
                         copy ? c->sym[c->pfree_sym[pv]].name : NULL, copy ? "free after free via a copy" : "free after free",
                         "'%s' is stale: its slot was already freed at line %u (double free)", h->name, c->pfree_line[pv]);
            return -1;
        }
        tr_slot(c, OSC_EV_SLOT_FREE, 0, c->pmhnd[pv], 0, n->line, 0);
        c->pfreed[pv] = 1;
        c->pfree_line[pv] = n->line;
        c->pfree_sym[pv] = hs;
        return 0;
    }
    case ON_CALLSTMT:
        set_sobj_tok(c, NODE(n->a));
        return chk_call(c, n->a, 0, 1) < 0 ? -1 : 0;
    case ON_BLOCK:
        return chk_block(c, i, 1);
    default:
        osc_diag_set(c->d, OSC_DIAG_SYNTAX, n->line, n->col, "?", 0, NULL, "statement", "unexpected statement");
        return -1;
    }
}

static int chk_block(C *c, int i, int new_scope)
{
    OscNode *n = NODE(i);
    if (new_scope && push_scope(c, n->line)) return -1;
    for (int s = n->a; s >= 0; s = NODE(s)->next) {
        if (c->term) {
            osc_diag_set(c->d, OSC_DIAG_UNSUPPORTED, NODE(s)->line, NODE(s)->col, "return", 0, NULL,
                         "unreachable statement", "statement after 'return' is unreachable");
            return -1;
        }
        if (chk_stmt(c, s)) return -1;
    }
    if (!c->term && new_scope && scope_exit_events(c, 1, (uint32_t)n->ival, n)) return -1;
    if (new_scope) pop_scope(c);
    return 0;
}

static int chk_fn(C *c, int fi)
{
    OscAst *a = c->ast;
    int f = a->fns[fi];
    OscNode *fn = NODE(f);
    c->fi = fi;
    c->fnode = f;
    c->fret = fn->ty;
    c->nsym = c->nobj = c->nbor = c->nvis = c->depth = c->loop_depth = c->term = 0;
    c->cond_depth = 0;
    c->nregion = 0;
    c->nprov = 0;          /* OSC-3 item 2 */
    c->nmslot = c->nmhnd = 0;
    char fname[64];
    osc_node_name(a, fn, fname, sizeof fname);
    snprintf(c->sobj, sizeof c->sobj, "%s", fname);
    for (int k = 0; k < fi; k++) {
        char g[64];
        osc_node_name(a, NODE(a->fns[k]), g, sizeof g);
        if (strcmp(g, fname) == 0) {
            osc_diag_set(c->d, OSC_DIAG_REDEFINED_NAME, fn->line, fn->col, fname, NODE(a->fns[k])->line, NULL,
                         "redefine function", "function '%s' is already defined at line %u", fname,
                         NODE(a->fns[k])->line);
            return -1;
        }
    }
    if (c->tr) c->tr->nfuncs = (uint16_t)(fi + 1);
    if (push_scope(c, fn->line)) return -1;
    for (int p = fn->a; p >= 0; p = NODE(p)->next) {
        OscNode *pn = NODE(p);
        uint8_t kind = pn->ty.s == OSC_T_BYTES || pn->ty.s == OSC_T_CELLS ? SK_SLICE :
                       pn->ty.s != OSC_T_REF ? SK_SCALAR : pn->ty.ref == OSC_REF_OWN ? SK_OWNER : SK_BORROW;
        int s = declare(c, pn, kind, &pn->ty, 0);
        if (s < 0) return -1;
        pn->sym = s;
        c->sym[s].is_param = 1;
        if (kind == SK_OWNER) {
            int o = new_obj(c, s, pn->line);
            if (o < 0) return -1;
            c->sym[s].obj = o;
            tr_ev(c, OSC_EV_ALLOC, o, -1, -1, -1, pn->line, 0);
        } else if (kind == SK_BORROW) {
            int o = new_obj(c, -1, pn->line);
            if (o < 0) return -1;
            tr_ev(c, OSC_EV_ALLOC, o, -1, -1, -1, pn->line, 0);
            int mut = pn->ty.ref == OSC_REF_MUT;
            int b = new_bor(c, o, -1, mut, pn->line);
            if (b < 0) return -1;
            tr_ev(c, mut ? OSC_EV_BORROW_MUT : OSC_EV_BORROW_SHARED, o, -1, b, -1, pn->line, 0);
            c->sym[s].bor = b;
            c->bor[b].sym = s;
        }
    }
    if (chk_contracts(c, fi)) return -1;
    if (chk_block(c, fn->b, 1)) return -1;
    if (!c->term) {
        if (fn->ty.s != OSC_T_VOID) {
            uint32_t end = (uint32_t)NODE(fn->b)->ival;
            osc_diag_set(c->d, OSC_DIAG_MISSING_RETURN, end, 0, fname, fn->line, NULL, "fall off end of non-void function",
                         "function '%s' returning %s can reach its end without 'return'", fname, tname(fn->ty.s));
            return -1;
        }
        if (scope_exit_events(c, 1, (uint32_t)NODE(fn->b)->ival, fn)) return -1;
    }
    pop_scope(c);
    return 0;
}

int osc_check(OscAst *ast, OscDiag *d, OscTrace *trace)
{
    C *c = calloc(1, sizeof *c);
    if (!c) {
        osc_diag_set(d, OSC_DIAG_CAPACITY, 0, 0, "unit", 0, NULL, "checker memory", "out of memory");
        return -1;
    }
    c->ast = ast;
    c->d = d;
    c->tr = trace;
    if (trace) { trace->n = 0; trace->overflow = 0; trace->refused = 0; trace->nfuncs = 0; }
    ast->nrel = 0;
    int rc = 0;
    for (uint32_t fi = 0; fi < ast->nfns && rc == 0; fi++) rc = chk_fn(c, (int)fi);
    if (rc == 0 && ast->nrel >= OSC_AST_MAX_REL) {
        osc_diag_set(d, OSC_DIAG_CAPACITY, 0, 0, "unit", 0, NULL, "release list capacity", "too many scope-exit releases");
        rc = -1;
    }
    free(c);
    return rc;
}
