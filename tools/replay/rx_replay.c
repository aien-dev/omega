/* rx_replay.c -- independent verifier and replay comparator for World crumb
 * logs (RXCLOG01, tools/replay/rxlog.h) and M22 dispatch logs
 * (src/train/tg_store.h dispatch.log). Links libc + src/sha256.c only.
 *
 *   rx_replay verify LOG               one log against its own rules
 *   rx_replay compare EXPECTED ACTUAL  recorded run vs replayed run
 *   rx_replay compare-causal EXP ACT   the same, committed history as a causal
 *                                      partial order (concurrent commits; see
 *                                      the section below and CAUSAL_ORDER.md)
 *   rx_replay verify-dispatch DIR      dispatch.log chain + committed head
 *   rx_replay compare-dispatch A B     two dispatch logs, record by record
 *
 * Output is exactly one verdict line:
 *   MATCH through event N
 *   DIVERGENCE event=N expected=<hash|value> actual=<hash|value> subsystem=<name>
 * optionally followed by one "detail:" line and, for a world compare, the two
 * first differing records decoded ("expected-record:", "actual-record:";
 * diagnostic only, never parsed). Events are numbered from 1 in
 * log order (crumbs, inputs and checkpoints alike; dispatch: record seq + 1).
 * Exit 0 = MATCH, 1 = DIVERGENCE, 2 = cannot run (usage, or the reference
 * log of a compare does not verify by itself).
 *
 * World rules (rx_world_verify_crumbs, plus stricter ones marked *):
 *   ids dense from 1; parents in [1, id); digest recomputes; episode follows
 *   wake_cause; * kind known; * parents strictly ascending (the runtime sorts
 *   and de-duplicates them); * wake_cause < id; * with RXL_FLAG_INPUTS every
 *   EXTERNAL crumb is directly preceded by an INPUT record that names it and
 *   whose objects and fields it writes; * checkpoints name the crumb count;
 *   * the END record counts every record and matches the head chain. */
#include "replay/rxlog.h"
#include "replay/trn1.h"
#include "sha256.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int diverged;
    uint64_t event;
    char expected[80], actual[80], subsystem[32], detail[200];
    char rec_exp[640], rec_act[640];   /* first differing records, decoded (diagnostic only) */
} verdict;

static void hexs(char *out, const uint8_t d[32]) { rxl_hex(d, 32, out); }

static int diverge(verdict *v, uint64_t ev, const char *sub, const char *exp, const char *act,
                   const char *fmt, ...) __attribute__((format(printf, 6, 7)));
static int diverge(verdict *v, uint64_t ev, const char *sub, const char *exp, const char *act,
                   const char *fmt, ...) {
    v->diverged = 1;
    v->event = ev;
    snprintf(v->subsystem, sizeof v->subsystem, "%s", sub);
    snprintf(v->expected, sizeof v->expected, "%s", exp);
    snprintf(v->actual, sizeof v->actual, "%s", act);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v->detail, sizeof v->detail, fmt, ap);
    va_end(ap);
    return 1;
}

static int report(const verdict *v) {
    if (!v->diverged) {
        printf("MATCH through event %llu\n", (unsigned long long)v->event);
        return 0;
    }
    printf("DIVERGENCE event=%llu expected=%s actual=%s subsystem=%s\n",
           (unsigned long long)v->event, v->expected, v->actual, v->subsystem);
    if (v->detail[0]) printf("detail: %s\n", v->detail);
    if (v->rec_exp[0]) printf("expected-record: %s\n", v->rec_exp);
    if (v->rec_act[0]) printf("actual-record:   %s\n", v->rec_act);
    return 1;
}

/* ---- World crumb logs --------------------------------------------------- */

static int verify_log(const rxl_log *l, verdict *v) {
    memset(v, 0, sizeof *v);
    uint8_t (*dig)[32] = calloc(l->n + 1, 32);
    uint64_t *ep = calloc(l->n + 1, sizeof *ep);
    if (!dig || !ep) {
        free(dig); free(ep);
        return diverge(v, 0, "verifier", "memory", "none", "out of memory");
    }
    uint64_t nk = 0, events = 0;
    uint8_t head[32] = { 0 };
    char a[80], b[80];
    int done = 0, pending_input = 0;
    const rxl_input *last_in = NULL;
    for (size_t i = 0; i < l->n && !done; i++) {
        const rxl_rec *r = &l->recs[i];
        uint64_t ev = i + 1;
        if (r->type == RXL_END) {
            if (i != l->n - 1) { done = diverge(v, ev, "world.log", "end-last", "end-early", "END record is not the last record"); break; }
            if (pending_input) { done = diverge(v, ev, "world.input", "external", "end", "INPUT record not followed by its EXTERNAL crumb"); break; }
            if (r->u.end.n_records != events) {
                snprintf(a, sizeof a, "records:%llu", (unsigned long long)events);
                snprintf(b, sizeof b, "records:%llu", (unsigned long long)r->u.end.n_records);
                done = diverge(v, ev, "world.log", a, b, "END record count does not match the log");
                break;
            }
            if (memcmp(head, r->u.end.head, 32)) {
                hexs(a, head); hexs(b, r->u.end.head);
                done = diverge(v, ev, "world.log", a, b, "END head does not chain the records before it");
                break;
            }
            done = 2;
            break;
        }
        if (r->type == RXL_CRUMB) {
            const rxl_crumb *k = &r->u.c;
            uint64_t want = nk + 1;
            if (k->id != want) {
                snprintf(a, sizeof a, "id:%llu", (unsigned long long)want);
                snprintf(b, sizeof b, "id:%llu", (unsigned long long)k->id);
                done = diverge(v, ev, "world.order", a, b, "crumb ids must be dense and in order (omitted or reordered crumb)");
                break;
            }
            if (k->kind < 1 || k->kind > RXL_K_MAX) {
                snprintf(b, sizeof b, "kind:%u", k->kind);
                done = diverge(v, ev, "world.crumb", "kind:1..10", b, "unknown crumb kind (crumb %llu)", (unsigned long long)k->id);
                break;
            }
            for (uint32_t p = 0; p < k->n_parents && !done; p++) {
                if (k->parents[p] == 0 || k->parents[p] >= k->id ||
                    (p && k->parents[p] <= k->parents[p - 1])) {
                    snprintf(a, sizeof a, "parent<%llu,ascending", (unsigned long long)k->id);
                    snprintf(b, sizeof b, "parent[%u]:%llu", p, (unsigned long long)k->parents[p]);
                    done = diverge(v, ev, "world.parents", a, b, "parent link out of range or out of canonical order (crumb %llu)", (unsigned long long)k->id);
                }
            }
            if (done) break;
            if (k->wake_cause >= k->id) {
                snprintf(a, sizeof a, "cause<%llu", (unsigned long long)k->id);
                snprintf(b, sizeof b, "cause:%llu", (unsigned long long)k->wake_cause);
                done = diverge(v, ev, "world.parents", a, b, "wake cause is not an earlier crumb (crumb %llu)", (unsigned long long)k->id);
                break;
            }
            uint8_t d[32];
            rxl_crumb_digest(k, (const uint8_t (*)[32])dig, nk, d);
            if (memcmp(d, k->digest, 32)) {
                hexs(a, d); hexs(b, k->digest);
                done = diverge(v, ev, "world.crumb", a, b, "crumb %llu: recorded digest does not match its fields", (unsigned long long)k->id);
                break;
            }
            uint64_t e = k->kind == RXL_K_EXTERNAL ? k->id
                       : k->wake_cause >= 1 ? ep[k->wake_cause - 1] : 0;
            if (k->episode != e) {
                snprintf(a, sizeof a, "episode:%llu", (unsigned long long)e);
                snprintf(b, sizeof b, "episode:%llu", (unsigned long long)k->episode);
                done = diverge(v, ev, "world.episode", a, b, "crumb %llu: episode does not follow its wake cause", (unsigned long long)k->id);
                break;
            }
            if (l->flags & RXL_FLAG_INPUTS) {
                if (k->kind == RXL_K_EXTERNAL) {
                    int ok = pending_input && last_in->after_crumb + 1 == k->id && k->n_outputs >= 1;
                    for (uint32_t m = 0; ok && m < last_in->n; m++) {
                        int hit = 0;
                        for (uint32_t o = 0; o < k->n_outputs; o++)
                            if (k->outputs[o].id == last_in->m[m].id && k->outputs[o].gen == last_in->m[m].gen &&
                                last_in->m[m].field < 64 && ((k->outputs[o].mask >> last_in->m[m].field) & 1)) hit = 1;
                        ok = hit;
                    }
                    if (!ok) {
                        done = diverge(v, ev, "world.input", "input-record", pending_input ? "mismatched-input" : "none",
                                       "EXTERNAL crumb %llu is not bound to the INPUT record before it", (unsigned long long)k->id);
                        break;
                    }
                    pending_input = 0;
                } else if (pending_input) {
                    done = diverge(v, ev, "world.input", "external", "other-crumb", "INPUT record not followed by its EXTERNAL crumb");
                    break;
                }
            }
            memcpy(dig[nk], k->digest, 32);
            ep[nk] = k->episode;
            nk++;
        } else if (r->type == RXL_INPUT) {
            if (pending_input || r->u.in.after_crumb != nk || r->u.in.n == 0) {
                snprintf(a, sizeof a, "after:%llu", (unsigned long long)nk);
                snprintf(b, sizeof b, "after:%llu", (unsigned long long)r->u.in.after_crumb);
                done = diverge(v, ev, "world.input", a, b, "INPUT record out of place");
                break;
            }
            pending_input = (l->flags & RXL_FLAG_INPUTS) != 0;
            last_in = &r->u.in;
        } else if (r->type == RXL_CHECKPOINT) {
            if (r->u.ck.through_crumb != nk || pending_input) {
                snprintf(a, sizeof a, "through:%llu", (unsigned long long)nk);
                snprintf(b, sizeof b, "through:%llu", (unsigned long long)r->u.ck.through_crumb);
                done = diverge(v, ev, "world.state", a, b, "CHECKPOINT does not name the crumbs before it");
                break;
            }
            if (r->u.ck.n_obj) {
                uint8_t sh[32];
                rxl_state_hash(&r->u.ck, sh);
                if (memcmp(sh, r->u.ck.hash, 32)) {
                    hexs(a, sh); hexs(b, r->u.ck.hash);
                    done = diverge(v, ev, "world.state", a, b, "CHECKPOINT state table does not hash to its state hash");
                    break;
                }
                for (uint32_t o = 0; o < r->u.ck.n_obj && !done; o++)
                    for (uint32_t f = 0; f < RXL_MAX_FIELDS && !done; f++)
                        if (r->u.ck.obj[o].writer[f] > nk) {
                            snprintf(a, sizeof a, "writer<=%llu", (unsigned long long)nk);
                            snprintf(b, sizeof b, "writer:%llu", (unsigned long long)r->u.ck.obj[o].writer[f]);
                            done = diverge(v, ev, "world.state", a, b, "CHECKPOINT names a field writer that is not an earlier crumb");
                        }
                if (done) break;
            }
        }
        rxl_head_step(head, r);
        events++;
    }
    free(dig); free(ep);
    if (done == 1) return 1;
    if (l->malformed)
        return diverge(v, l->n + 1, "world.log", "record", "malformed", "%s", l->why);
    if (done != 2)
        return diverge(v, events + 1, "world.log", "end", "none", "log ends without its END record (truncated)");
    v->event = events;
    return 0;
}

static const char *rec_sub(uint32_t t) {
    return t == RXL_CRUMB ? "world.crumb" : t == RXL_INPUT ? "world.input"
         : t == RXL_CHECKPOINT ? "world.state" : "world.log";
}

/* First field that differs between two crumbs, for the detail line. */
static const char *crumb_field_diff(const rxl_crumb *x, const rxl_crumb *y) {
    if (x->id != y->id) return "id";
    if (x->kind != y->kind) return "kind";
    if (x->reaction != y->reaction) return "reaction";
    if (x->faculty != y->faculty) return "faculty";
    if (x->wake_cause != y->wake_cause) return "wake_cause";
    if (x->coalesced != y->coalesced) return "coalesced_wakes";
    if (x->n_inputs != y->n_inputs) return "inputs";
    for (uint32_t i = 0; i < x->n_inputs; i++)
        if (x->inputs[i].id != y->inputs[i].id || x->inputs[i].gen != y->inputs[i].gen ||
            x->inputs[i].version != y->inputs[i].version || x->inputs[i].mask != y->inputs[i].mask) return "inputs";
    if (x->n_caps != y->n_caps) return "caps";
    for (uint32_t i = 0; i < x->n_caps; i++)
        if (x->caps[i].cap_id != y->caps[i].cap_id || x->caps[i].gen != y->caps[i].gen ||
            x->caps[i].issuer != y->caps[i].issuer) return "caps";
    if (x->n_outputs != y->n_outputs) return "outputs";
    for (uint32_t i = 0; i < x->n_outputs; i++)
        if (x->outputs[i].id != y->outputs[i].id || x->outputs[i].gen != y->outputs[i].gen ||
            x->outputs[i].version != y->outputs[i].version || x->outputs[i].mask != y->outputs[i].mask) return "outputs";
    if (x->reason != y->reason) return "reason";
    if (x->n_parents != y->n_parents) return "parents";
    for (uint32_t i = 0; i < x->n_parents; i++)
        if (x->parents[i] != y->parents[i]) return "parents";
    return "parent digests";
}

/* Diagnostic: one record decoded on one line, so a failing compare shows
 * both sides of the first difference (worker and times included, though
 * they are not compared). Appends stop at the buffer end. */
static void cat_f(char *o, size_t n, size_t *at, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void cat_f(char *o, size_t n, size_t *at, const char *fmt, ...) {
    if (*at >= n) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(o + *at, n - *at, fmt, ap);
    va_end(ap);
    *at = k < 0 ? n : *at + (size_t)k;
}

static void rec_line(const rxl_rec *r, char *o, size_t n) {
    size_t at = 0;
    o[0] = 0;
    if (r->type == RXL_CRUMB) {
        const rxl_crumb *k = &r->u.c;
        cat_f(o, n, &at, "crumb id=%llu kind=%u reaction=%u faculty=%u worker=%u wake_cause=%llu coalesced=%llu reason=%d in=",
              (unsigned long long)k->id, k->kind, k->reaction, k->faculty, k->worker,
              (unsigned long long)k->wake_cause, (unsigned long long)k->coalesced, (int)k->reason);
        for (uint32_t i = 0; i < k->n_inputs && i < RXL_MAX_DEPS; i++)
            cat_f(o, n, &at, "%s%u/%u@v%llu:m%llx", i ? "," : "", k->inputs[i].id, k->inputs[i].gen,
                  (unsigned long long)k->inputs[i].version, (unsigned long long)k->inputs[i].mask);
        cat_f(o, n, &at, " out=");
        for (uint32_t i = 0; i < k->n_outputs && i < RXL_MAX_WRITES; i++)
            cat_f(o, n, &at, "%s%u/%u@v%llu:m%llx", i ? "," : "", k->outputs[i].id, k->outputs[i].gen,
                  (unsigned long long)k->outputs[i].version, (unsigned long long)k->outputs[i].mask);
        cat_f(o, n, &at, " parents=");
        for (uint32_t i = 0; i < k->n_parents && i < RXL_MAX_PARENTS; i++)
            cat_f(o, n, &at, "%s%llu", i ? "," : "", (unsigned long long)k->parents[i]);
    } else if (r->type == RXL_INPUT) {
        const rxl_input *in = &r->u.in;
        cat_f(o, n, &at, "input after_crumb=%llu muts=", (unsigned long long)in->after_crumb);
        for (uint32_t i = 0; i < in->n && i < RXL_MAX_MUTS; i++)
            cat_f(o, n, &at, "%s%u.f%u=%llu", i ? "," : "", in->m[i].id, in->m[i].field,
                  (unsigned long long)in->m[i].value);
    } else if (r->type == RXL_CHECKPOINT) {
        cat_f(o, n, &at, "checkpoint through_crumb=%llu", (unsigned long long)r->u.ck.through_crumb);
    } else {
        cat_f(o, n, &at, "record type=%u", r->type);
    }
}

static int compare_logs(const char *pa, const char *pb) {
    rxl_log A, B;
    char err[160];
    verdict v;
    if (rxl_read(pa, &A, err, sizeof err)) { fprintf(stderr, "rx_replay: expected log: %s\n", err); return 2; }
    if (verify_log(&A, &v)) {
        printf("REFUSED expected log does not verify: event=%llu subsystem=%s (%s)\n",
               (unsigned long long)v.event, v.subsystem, v.detail);
        rxl_free(&A);
        return 2;
    }
    if (rxl_read(pb, &B, err, sizeof err)) {
        printf("DIVERGENCE event=1 expected=RXCLOG01 actual=unreadable subsystem=world.log\ndetail: %s\n", err);
        rxl_free(&A);
        return 1;
    }
    /* Content first, in log order, so the first differing event is named
     * even when the replayed log is also internally broken there. */
    size_t na = A.n - 1;   /* reference verified: its last record is END */
    size_t nb = B.n && B.recs[B.n - 1].type == RXL_END ? B.n - 1 : B.n;
    memset(&v, 0, sizeof v);
    char a[80], b[80];
    for (size_t i = 0; i < na || i < nb; i++) {
        uint64_t ev = i + 1;
        uint8_t dx[32], dy[32];
        if (i >= nb) {
            rxl_event_digest(&A.recs[i], dx); hexs(a, dx);
            diverge(&v, ev, rec_sub(A.recs[i].type), a, "none", "replayed log stops early (missing event)");
            break;
        }
        if (i >= na) {
            rxl_event_digest(&B.recs[i], dy); hexs(b, dy);
            diverge(&v, ev, rec_sub(B.recs[i].type), "none", b, "replayed log has an extra event");
            break;
        }
        const rxl_rec *x = &A.recs[i], *y = &B.recs[i];
        rxl_event_digest(x, dx); rxl_event_digest(y, dy);
        if (x->type != y->type || memcmp(dx, dy, 32)) {
            hexs(a, dx); hexs(b, dy);
            if (x->type == RXL_CRUMB && y->type == RXL_CRUMB)
                diverge(&v, ev, "world.crumb", a, b, "crumb %llu differs first in: %s",
                        (unsigned long long)x->u.c.id, crumb_field_diff(&x->u.c, &y->u.c));
            else
                diverge(&v, ev, rec_sub(x->type), a, b, "record type %u vs %u", x->type, y->type);
            rec_line(x, v.rec_exp, sizeof v.rec_exp);
            rec_line(y, v.rec_act, sizeof v.rec_act);
            break;
        }
    }
    if (!v.diverged) {
        /* Same compared content: the replayed log must also hold its own
         * rules (stored digests that were copied, not recomputed, and a
         * missing END are caught here). */
        verdict w;
        if (verify_log(&B, &w)) v = w;
        else v.event = na;
    }
    rxl_free(&A); rxl_free(&B);
    return report(&v);
}

/* ---- Causal-order compare (concurrent commits) -------------------------
 *
 * rx_replay compare-causal EXPECTED ACTUAL
 *
 * Committed history as a causal partial order, not a sequence. With more
 * than one worker the World commits causally unrelated reactions in either
 * order and may supersede a run that read a stale belief (INVALIDATED crumb,
 * then a rerun); crumb ids, and every id that refers to them, then depend on
 * the schedule (runtime finding I11; tools/replay/CAUSAL_ORDER.md). This
 * compare is the property that holds anyway, and is stricter than a
 * sequence compare in no place and looser only where stated here:
 *
 *   identity  an entry's causal identity is a digest of its content (kind,
 *             reaction, faculty, inputs with versions and masks, caps,
 *             outputs with versions and masks, reason) and of the causal
 *             identities of its wake cause and of every parent (as a sorted
 *             set). Crumb ids are not part of it; cause links are, by
 *             identity, recursively back to the first crumb.
 *   excluded  coalesced_wakes (merged-wake count: how many wakes the
 *             scheduler folded into one run is a schedule fact, 1 with one
 *             worker and 0 with four in the decoded runs); it stays in the
 *             log and in the sequence compare. worker and clocks, as in the
 *             sequence compare. Crumb ids and the INPUT after_crumb /
 *             CHECKPOINT through_crumb counts (positions; each log must still
 *             verify by its own rules, which check them).
 *   segments  the log is cut at INPUT records (the replayer waits for
 *             quiescence after each), so entries are matched only within
 *             the same stimulus. INPUT contents (capability, mutations,
 *             values) must be equal, in order.
 *   entries   within a segment, the multiset of causal identities of all
 *             crumbs except superseded ones must be equal.
 *   superseded  an INVALIDATED crumb (in either log) must be flagged as such:
 *             kind INVALIDATED, a nonzero reason, no outputs. It must be
 *             paired with its rerun: a later crumb of the same reaction in the
 *             same episode and segment that is not itself INVALIDATED. A run
 *             that was superseded but is logged as committed is not
 *             superseded: it is an extra entry and fails (named "unflagged
 *             superseded entry" when the same reaction has another entry in
 *             the same episode).
 *   state     every CHECKPOINT state table (required in both logs) must
 *             match object for object: values, versions and field versions
 *             exactly, field writers by causal identity, not crumb id.
 *
 * Output and exit codes as compare. MATCH lines also report how many
 * superseded entries each log had. */

typedef struct {
    uint8_t (*cid)[32];      /* causal identity of crumb id, at [id - 1] */
    uint8_t (*content)[32];  /* content digest (no links), at [id - 1] */
    int *seg;                /* segment of crumb id */
    size_t *rec;             /* record index of crumb id */
    uint64_t n;              /* crumbs */
    int n_seg;               /* segments (INPUT records + 1) */
    uint64_t superseded;
} causal_ix;

static void causal_free(causal_ix *x) {
    free(x->cid); free(x->content); free(x->seg); free(x->rec);
    memset(x, 0, sizeof *x);
}

static void ch32(sha256_ctx *c, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    sha256_update(c, b, 4);
}
static void ch64(sha256_ctx *c, uint64_t v) { ch32(c, (uint32_t)v); ch32(c, (uint32_t)(v >> 32)); }

static int cmp32(const void *a, const void *b) { return memcmp(a, b, 32); }

/* Index a log that verify_log accepted (ids dense, links point back). */
static int causal_index(const rxl_log *l, causal_ix *x) {
    memset(x, 0, sizeof *x);
    size_t cap = l->n + 1;
    x->cid = calloc(cap, 32); x->content = calloc(cap, 32);
    x->seg = calloc(cap, sizeof *x->seg); x->rec = calloc(cap, sizeof *x->rec);
    uint8_t (*ps)[32] = calloc(RXL_MAX_PARENTS, 32);
    if (!x->cid || !x->content || !x->seg || !x->rec || !ps) { free(ps); causal_free(x); return -1; }
    int seg = 0;
    for (size_t i = 0; i < l->n; i++) {
        const rxl_rec *r = &l->recs[i];
        if (r->type == RXL_INPUT) { seg++; continue; }
        if (r->type != RXL_CRUMB) continue;
        const rxl_crumb *k = &r->u.c;
        uint64_t j = x->n++;
        sha256_ctx c;
        sha256_init(&c);
        sha256_update(&c, (const uint8_t *)"RXCLOG01-CAUSAL-CONTENT", 23);
        ch32(&c, k->kind); ch32(&c, k->reaction); ch32(&c, k->faculty);
        ch32(&c, k->n_inputs);
        for (uint32_t q = 0; q < k->n_inputs; q++) {
            ch32(&c, k->inputs[q].id); ch32(&c, k->inputs[q].gen);
            ch64(&c, k->inputs[q].version); ch64(&c, k->inputs[q].mask);
        }
        ch32(&c, k->n_caps);
        for (uint32_t q = 0; q < k->n_caps; q++) {
            ch32(&c, k->caps[q].cap_id); ch64(&c, k->caps[q].gen); ch32(&c, k->caps[q].issuer);
        }
        ch32(&c, k->n_outputs);
        for (uint32_t q = 0; q < k->n_outputs; q++) {
            ch32(&c, k->outputs[q].id); ch32(&c, k->outputs[q].gen);
            ch64(&c, k->outputs[q].version); ch64(&c, k->outputs[q].mask);
        }
        ch32(&c, (uint32_t)k->reason);
        sha256_final(&c, x->content[j]);
        /* links by identity: wake cause, then the sorted set of parents */
        sha256_init(&c);
        sha256_update(&c, (const uint8_t *)"RXCLOG01-CAUSAL-ID", 18);
        sha256_update(&c, x->content[j], 32);
        uint8_t z[32] = { 0 };
        ch32(&c, k->wake_cause ? 1u : 0u);
        sha256_update(&c, k->wake_cause ? x->cid[k->wake_cause - 1] : z, 32);
        for (uint32_t q = 0; q < k->n_parents; q++) memcpy(ps[q], x->cid[k->parents[q] - 1], 32);
        qsort(ps, k->n_parents, 32, cmp32);
        ch32(&c, k->n_parents);
        for (uint32_t q = 0; q < k->n_parents; q++) sha256_update(&c, ps[q], 32);
        sha256_final(&c, x->cid[j]);
        x->seg[j] = seg;
        x->rec[j] = i;
        if (k->kind == RXL_K_INVALIDATED) x->superseded++;
    }
    x->n_seg = seg + 1;
    free(ps);
    return 0;
}

/* Every superseded entry flagged and paired with its rerun. */
static int causal_superseded(const rxl_log *l, const causal_ix *x, const char *side, verdict *v) {
    char a[80], b[80];
    for (uint64_t j = 0; j < x->n; j++) {
        const rxl_crumb *k = &l->recs[x->rec[j]].u.c;
        if (k->kind != RXL_K_INVALIDATED) continue;
        if (k->reason == 0 || k->n_outputs) {
            snprintf(a, sizeof a, "flagged:reason!=0,outputs=0");
            snprintf(b, sizeof b, "reason:%d,outputs:%u", (int)k->reason, k->n_outputs);
            return diverge(v, x->rec[j] + 1, "world.superseded", a, b,
                           "%s crumb %llu: superseded entry is not flagged as superseded", side, (unsigned long long)k->id);
        }
        int paired = 0;
        for (uint64_t q = j + 1; q < x->n && !paired; q++) {
            const rxl_crumb *y = &l->recs[x->rec[q]].u.c;
            if (x->seg[q] == x->seg[j] && y->reaction == k->reaction && y->episode == k->episode &&
                y->kind != RXL_K_INVALIDATED) paired = 1;
        }
        if (!paired) {
            snprintf(b, sizeof b, "crumb:%llu", (unsigned long long)k->id);
            return diverge(v, x->rec[j] + 1, "world.superseded", "rerun", b,
                           "%s crumb %llu: superseded entry has no rerun of reaction %u in episode %llu",
                           side, (unsigned long long)k->id, k->reaction, (unsigned long long)k->episode);
        }
    }
    return 0;
}

static int same_input(const rxl_input *p, const rxl_input *q) {
    if (p->cap_id != q->cap_id || p->cap_gen != q->cap_gen || p->n != q->n) return 0;
    for (uint32_t i = 0; i < p->n; i++)
        if (p->m[i].id != q->m[i].id || p->m[i].gen != q->m[i].gen || p->m[i].field != q->m[i].field ||
            p->m[i].value != q->m[i].value) return 0;
    return 1;
}

/* Records of one segment: [*lo, *hi). */
static void seg_bounds(const rxl_log *l, int seg, size_t *lo, size_t *hi) {
    int s = 0;
    size_t i = 0;
    while (i < l->n && s < seg) { if (l->recs[i].type == RXL_INPUT) s++; i++; }
    *lo = i;
    while (i < l->n && l->recs[i].type != RXL_INPUT && l->recs[i].type != RXL_END) i++;
    *hi = i;
}

static int causal_state(const rxl_checkpoint *p, const causal_ix *xa, const rxl_checkpoint *q,
                        const causal_ix *xb, uint64_t ev, verdict *v) {
    char a[80], b[80];
    if (p->n_obj != q->n_obj) {
        snprintf(a, sizeof a, "objects:%u", p->n_obj);
        snprintf(b, sizeof b, "objects:%u", q->n_obj);
        return diverge(v, ev, "world.state", a, b, "state tables have different object counts");
    }
    for (uint32_t o = 0; o < p->n_obj; o++) {
        const rxl_obj *s = &p->obj[o], *t = &q->obj[o];
        if (s->id != t->id || s->gen != t->gen || s->type != t->type || s->version != t->version) {
            snprintf(a, sizeof a, "obj:%u/%u:t%u@v%llu", s->id, s->gen, s->type, (unsigned long long)s->version);
            snprintf(b, sizeof b, "obj:%u/%u:t%u@v%llu", t->id, t->gen, t->type, (unsigned long long)t->version);
            return diverge(v, ev, "world.state", a, b, "object %u identity or version differs", o);
        }
        for (uint32_t f = 0; f < RXL_MAX_FIELDS; f++) {
            if (s->value[f] != t->value[f] || s->fversion[f] != t->fversion[f]) {
                snprintf(a, sizeof a, "value:%llu@v%llu", (unsigned long long)s->value[f], (unsigned long long)s->fversion[f]);
                snprintf(b, sizeof b, "value:%llu@v%llu", (unsigned long long)t->value[f], (unsigned long long)t->fversion[f]);
                return diverge(v, ev, "world.state", a, b, "object %u field %u value or field version differs", s->id, f);
            }
            uint64_t wa = s->writer[f], wb = t->writer[f];
            int same = (!wa && !wb) ||
                       (wa && wb && wa <= xa->n && wb <= xb->n && !memcmp(xa->cid[wa - 1], xb->cid[wb - 1], 32));
            if (!same) {
                snprintf(a, sizeof a, "writer:crumb%llu", (unsigned long long)wa);
                snprintf(b, sizeof b, "writer:crumb%llu", (unsigned long long)wb);
                return diverge(v, ev, "world.state", a, b,
                               "object %u field %u: snapshot writer differs by causal identity", s->id, f);
            }
        }
    }
    return 0;
}

static int causal_segment(const rxl_log *A, const causal_ix *xa, const rxl_log *B, const causal_ix *xb,
                          int seg, verdict *v) {
    char a[80], b[80];
    size_t la, ha, lb, hb;
    seg_bounds(A, seg, &la, &ha);
    seg_bounds(B, seg, &lb, &hb);
    if (seg > 0 && !same_input(&A->recs[la - 1].u.in, &B->recs[lb - 1].u.in)) {
        uint8_t dx[32], dy[32];
        rxl_event_digest(&A->recs[la - 1], dx); rxl_event_digest(&B->recs[lb - 1], dy);
        hexs(a, dx); hexs(b, dy);
        diverge(v, la, "world.input", a, b, "INPUT %d differs (capability, mutations or values)", seg);
        rec_line(&A->recs[la - 1], v->rec_exp, sizeof v->rec_exp);
        rec_line(&B->recs[lb - 1], v->rec_act, sizeof v->rec_act);
        return 1;
    }
    /* entries of this segment, superseded ones left out */
    uint64_t *ea = calloc(xa->n + 1, sizeof *ea), *eb = calloc(xb->n + 1, sizeof *eb);
    char *used = calloc(xb->n + 1, 1);
    if (!ea || !eb || !used) { free(ea); free(eb); free(used); return diverge(v, 0, "verifier", "memory", "none", "out of memory"); }
    size_t na = 0, nb = 0;
    for (uint64_t j = 0; j < xa->n; j++)
        if (xa->seg[j] == seg && A->recs[xa->rec[j]].u.c.kind != RXL_K_INVALIDATED) ea[na++] = j;
    for (uint64_t j = 0; j < xb->n; j++)
        if (xb->seg[j] == seg && B->recs[xb->rec[j]].u.c.kind != RXL_K_INVALIDATED) eb[nb++] = j;
    int bad = 0;
    /* A reaction with more entries in the replayed stimulus than in the
     * expected one ran again after a run that was logged as committed: a
     * superseded run without its flag. Named before the entry match so the
     * report says what happened, not only which entry differs. */
    for (size_t q = 0; q < nb && !bad; q++) {
        const rxl_crumb *y = &B->recs[xb->rec[eb[q]]].u.c;
        if (y->kind == RXL_K_EXTERNAL || y->kind == RXL_K_CREATE) continue;
        size_t ca = 0, cb = 0;
        for (size_t p = 0; p < nb; p++) {
            const rxl_crumb *z = &B->recs[xb->rec[eb[p]]].u.c;
            if (z->reaction == y->reaction && z->kind != RXL_K_EXTERNAL && z->kind != RXL_K_CREATE) cb++;
        }
        for (size_t p = 0; p < na; p++) {
            const rxl_crumb *z = &A->recs[xa->rec[ea[p]]].u.c;
            if (z->reaction == y->reaction && z->kind != RXL_K_EXTERNAL && z->kind != RXL_K_CREATE) ca++;
        }
        if (cb > ca) {
            snprintf(a, sizeof a, "reaction%u:entries=%zu", y->reaction, ca);
            snprintf(b, sizeof b, "reaction%u:entries=%zu", y->reaction, cb);
            diverge(v, xb->rec[eb[q]] + 1, "world.superseded", a, b,
                    "stimulus %d: unflagged superseded entry (reaction %u logged as committed %zu times, expected %zu; first at replay crumb %llu)",
                    seg, y->reaction, cb, ca, (unsigned long long)y->id);
            rec_line(&B->recs[xb->rec[eb[q]]], v->rec_act, sizeof v->rec_act);
            bad = 1;
        }
    }
    for (size_t i = 0; i < na && !bad; i++) {
        uint64_t ja = ea[i];
        size_t hit = nb;
        for (size_t q = 0; q < nb && hit == nb; q++)
            if (!used[q] && !memcmp(xa->cid[ja], xb->cid[eb[q]], 32)) hit = q;
        if (hit < nb) { used[hit] = 1; continue; }
        const rxl_rec *ra = &A->recs[xa->rec[ja]];
        const rxl_crumb *k = &ra->u.c;
        hexs(a, xa->cid[ja]);
        size_t near = nb;
        for (size_t q = 0; q < nb && near == nb; q++)
            if (!used[q] && !memcmp(xa->content[ja], xb->content[eb[q]], 32)) near = q;
        if (near < nb) {
            hexs(b, xb->cid[eb[near]]);
            diverge(v, xa->rec[ja] + 1, "world.cause", a, b,
                    "crumb %llu: same entry, different cause links (wake cause or parents, by identity; replay crumb %llu)",
                    (unsigned long long)k->id, (unsigned long long)eb[near] + 1);
        } else {
            for (size_t q = 0; q < nb && near == nb; q++) {
                const rxl_crumb *y = &B->recs[xb->rec[eb[q]]].u.c;
                if (!used[q] && y->kind == k->kind && y->reaction == k->reaction) near = q;
            }
            if (near < nb) {
                hexs(b, xb->cid[eb[near]]);
                diverge(v, xa->rec[ja] + 1, "world.causal", a, b,
                        "crumb %llu: committed entry differs (replay crumb %llu)", (unsigned long long)k->id,
                        (unsigned long long)eb[near] + 1);
            } else {
                diverge(v, xa->rec[ja] + 1, "world.causal", a, "none",
                        "crumb %llu: entry missing from the replayed run", (unsigned long long)k->id);
            }
        }
        rec_line(ra, v->rec_exp, sizeof v->rec_exp);
        if (near < nb) rec_line(&B->recs[xb->rec[eb[near]]], v->rec_act, sizeof v->rec_act);
        bad = 1;
    }
    for (size_t q = 0; q < nb && !bad; q++) {
        if (used[q]) continue;
        const rxl_rec *rb = &B->recs[xb->rec[eb[q]]];
        const rxl_crumb *y = &rb->u.c;
        int twice = 0;
        for (size_t p = 0; p < nb && !twice; p++) {
            const rxl_crumb *z = &B->recs[xb->rec[eb[p]]].u.c;
            if (p != q && z->reaction == y->reaction && z->episode == y->episode && y->kind != RXL_K_EXTERNAL) twice = 1;
        }
        hexs(b, xb->cid[eb[q]]);
        if (twice)
            diverge(v, xb->rec[eb[q]] + 1, "world.superseded", "none", b,
                    "replay crumb %llu: unflagged superseded entry (reaction %u has another entry in episode %llu)",
                    (unsigned long long)y->id, y->reaction, (unsigned long long)y->episode);
        else
            diverge(v, xb->rec[eb[q]] + 1, "world.causal", "none", b, "replay crumb %llu: extra entry",
                    (unsigned long long)y->id);
        rec_line(rb, v->rec_act, sizeof v->rec_act);
        bad = 1;
    }
    free(ea); free(eb); free(used);
    if (bad) return 1;
    /* checkpoints of this segment, pairwise in order */
    size_t ia = la, ib = lb;
    for (;;) {
        while (ia < ha && A->recs[ia].type != RXL_CHECKPOINT) ia++;
        while (ib < hb && B->recs[ib].type != RXL_CHECKPOINT) ib++;
        if (ia >= ha && ib >= hb) break;
        if (ia >= ha || ib >= hb) {
            snprintf(a, sizeof a, "%s", ia < ha ? "checkpoint" : "none");
            snprintf(b, sizeof b, "%s", ib < hb ? "checkpoint" : "none");
            return diverge(v, ia < ha ? ia + 1 : ha, "world.state", a, b, "segment %d: checkpoint counts differ", seg);
        }
        const rxl_checkpoint *p = &A->recs[ia].u.ck, *q = &B->recs[ib].u.ck;
        if (!q->n_obj)
            return diverge(v, ia + 1, "world.state", "state-table", "hash-only",
                           "replayed CHECKPOINT has no state table: writers cannot be matched by identity");
        if (p->subsystem != q->subsystem || causal_state(p, xa, q, xb, ia + 1, v)) {
            if (!v->diverged) {
                snprintf(a, sizeof a, "subsystem:%u", p->subsystem);
                snprintf(b, sizeof b, "subsystem:%u", q->subsystem);
                diverge(v, ia + 1, "world.state", a, b, "checkpoint subsystems differ");
            }
            return 1;
        }
        ia++; ib++;
    }
    return 0;
}

static int compare_causal(const char *pa, const char *pb) {
    rxl_log A, B;
    char err[160];
    verdict v;
    if (rxl_read(pa, &A, err, sizeof err)) { fprintf(stderr, "rx_replay: expected log: %s\n", err); return 2; }
    if (verify_log(&A, &v)) {
        printf("REFUSED expected log does not verify: event=%llu subsystem=%s (%s)\n",
               (unsigned long long)v.event, v.subsystem, v.detail);
        rxl_free(&A);
        return 2;
    }
    for (size_t i = 0; i < A.n; i++)
        if (A.recs[i].type == RXL_CHECKPOINT && !A.recs[i].u.ck.n_obj) {
            printf("REFUSED expected log has a CHECKPOINT without a state table (event %zu)\n", i + 1);
            rxl_free(&A);
            return 2;
        }
    if (rxl_read(pb, &B, err, sizeof err)) {
        printf("DIVERGENCE event=1 expected=RXCLOG01 actual=unreadable subsystem=world.log\ndetail: %s\n", err);
        rxl_free(&A);
        return 1;
    }
    /* The replayed log must hold its own rules first: dense ids, links that
     * point back, recomputed digests, END chain, state tables that hash. */
    if (verify_log(&B, &v)) { rxl_free(&A); rxl_free(&B); return report(&v); }
    causal_ix xa, xb;
    if (causal_index(&A, &xa) || causal_index(&B, &xb)) {
        causal_free(&xa); rxl_free(&A); rxl_free(&B);
        fprintf(stderr, "rx_replay: out of memory\n");
        return 2;
    }
    memset(&v, 0, sizeof v);
    char a[80], b[80];
    if (xa.n_seg != xb.n_seg) {
        snprintf(a, sizeof a, "inputs:%d", xa.n_seg - 1);
        snprintf(b, sizeof b, "inputs:%d", xb.n_seg - 1);
        diverge(&v, 1, "world.input", a, b, "different number of INPUT records");
    }
    if (!v.diverged) causal_superseded(&A, &xa, "expected", &v);
    if (!v.diverged) causal_superseded(&B, &xb, "replayed", &v);
    for (int s = 0; s < xa.n_seg && !v.diverged; s++) causal_segment(&A, &xa, &B, &xb, s, &v);
    int rc;
    if (v.diverged) rc = report(&v);
    else {
        printf("MATCH through event %llu (causal order: %d segments, %llu crumbs expected, %llu replayed; "
               "superseded expected=%llu replayed=%llu; merged-wake counts excluded)\n",
               (unsigned long long)(A.n - 1), xa.n_seg, (unsigned long long)xa.n, (unsigned long long)xb.n,
               (unsigned long long)xa.superseded, (unsigned long long)xb.superseded);
        rc = 0;
    }
    causal_free(&xa); causal_free(&xb);
    rxl_free(&A); rxl_free(&B);
    return rc;
}

/* ---- M22 dispatch.log --------------------------------------------------- */

#define D_SIZE 192u
#define D_HASHED 160u
#define D_MAGIC 0x31444754u
#define G_HDR 152u

static uint32_t g32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t g64(const uint8_t *p) { return (uint64_t)g32(p) | (uint64_t)g32(p + 4) << 32; }

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    uint8_t *b = malloc(cap);
    if (!b) { fclose(f); return NULL; }
    for (;;) {
        if (n == cap) {
            uint8_t *nb = realloc(b, cap *= 2);
            if (!nb) { free(b); fclose(f); return NULL; }
            b = nb;
        }
        size_t g = fread(b + n, 1, cap - n, f);
        if (!g) break;
        n += g;
    }
    fclose(f);
    *len = n;
    return b;
}

/* Every file path the verifier builds fits in RX_PATH_CAP bytes or is
 * refused. A silently truncated path can name a different file (or the
 * store directory itself), so truncation is a hard error, never a guess. */
#define RX_PATH_CAP 4096

/* Build DIR/NAME into out[cap]. 0 = built; -1 = would not fit (out is
 * emptied and the reason goes to stderr). */
static int join_path(char *out, size_t cap, const char *dir, const char *name) {
    int w = snprintf(out, cap, "%s/%s", dir, name);
    if (w < 0 || (size_t)w >= cap) {
        fprintf(stderr, "rx_replay: path too long (%d bytes, limit %zu): %s/%s\n", w, cap - 1, dir, name);
        if (cap) out[0] = 0;
        return -1;
    }
    return 0;
}

static void chain_step(const uint8_t prev[32], const uint8_t *rec, uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, prev, 32);
    sha256_update(&c, rec, D_HASHED);
    sha256_final(&c, out);
}

/* Structure of one record beyond the chain: magic, seq, ref count, and the
 * bytes the writer leaves zero (tg_dispatch memsets the record). */
static const char *dispatch_shape(const uint8_t *r, uint64_t seq) {
    if (g32(r) != D_MAGIC) return "magic";
    if (g64(r + 8) != seq) return "seq";
    uint32_t nr = g32(r + 48);
    if (nr > 3) return "n_refs";
    for (int i = 52; i < 56; i++) if (r[i]) return "padding";
    for (uint32_t i = 0; i < 3; i++) {
        const uint8_t *q = r + 56 + 24 * i;
        if (i < nr) { for (int j = 4; j < 8; j++) if (q[j]) return "padding"; }
        else { for (int j = 0; j < 24; j++) if (q[j]) return "unused ref"; }
    }
    return NULL;
}

static const char *dispatch_field_diff(const uint8_t *x, const uint8_t *y) {
    static const struct { int off, len; const char *name; } f[] = {
        { 0, 4, "magic" }, { 4, 4, "op_id" }, { 8, 8, "seq" }, { 16, 8, "step" }, { 24, 8, "base_gen" },
        { 32, 8, "arg0" }, { 40, 8, "arg1" }, { 48, 8, "n_refs" }, { 56, 72, "refs" },
        { 128, 32, "base_digest" }, { 160, 32, "chain" } };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++)
        if (memcmp(x + f[i].off, y + f[i].off, (size_t)f[i].len)) return f[i].name;
    return "none";
}

/* Read CURRENT and the generation it names: committed dispatch length and
 * head. 0 = read, 1 = store has no CURRENT, -1 = malformed. */
static int committed_head(const char *dir, uint64_t *len, uint8_t head[32], char *why, size_t wn) {
    char p[RX_PATH_CAP];
    if (join_path(p, sizeof p, dir, "CURRENT")) {
        snprintf(why, wn, "store path too long");
        return -1;
    }
    size_t n;
    errno = 0;
    uint8_t *cur = slurp(p, &n);
    if (!cur) {
        if (errno == ENOENT) return 1;
        snprintf(why, wn, "CURRENT unreadable");
        return -1;
    }
    if (n != 95 || memcmp(cur, "OMTRCUR1 ", 9) || cur[29] != ' ' || cur[94] != '\n') {
        free(cur);
        snprintf(why, wn, "CURRENT malformed");
        return -1;
    }
    char gen[21], hexd[65];
    memcpy(gen, cur + 9, 20); gen[20] = 0;
    memcpy(hexd, cur + 30, 64); hexd[64] = 0;
    free(cur);
    char gname[32]; /* "gen-" + 20 + ".bin" + NUL = 29 */
    snprintf(gname, sizeof gname, "gen-%s.bin", gen);
    if (join_path(p, sizeof p, dir, gname)) {
        snprintf(why, wn, "store path too long");
        return -1;
    }
    uint8_t *g = slurp(p, &n);
    if (!g || n < G_HDR) {
        free(g);
        snprintf(why, wn, "generation file gen-%s.bin missing or short", gen);
        return -1;
    }
    /* The generation digest covers header bytes [0,120) + parameters + optimizer. */
    uint8_t d[32];
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, g, 120);
    sha256_update(&c, g + G_HDR, n - G_HDR);
    sha256_final(&c, d);
    char dh[65];
    rxl_hex(d, 32, dh);
    if (memcmp(d, g + 120, 32) || strcmp(dh, hexd)) {
        free(g);
        snprintf(why, wn, "generation gen-%s.bin does not verify against CURRENT", gen);
        return -1;
    }
    *len = g64(g + 48);
    memcpy(head, g + 56, 32);
    free(g);
    return 0;
}

static int verify_dispatch(const char *dir, verdict *v, uint8_t **out_log, size_t *out_n) {
    memset(v, 0, sizeof *v);
    char p[RX_PATH_CAP], a[80], b[80], why[160];
    if (join_path(p, sizeof p, dir, "dispatch.log"))
        return diverge(v, 1, "m22.commit", "store-path", "too-long", "store path too long");
    size_t n = 0;
    uint8_t *log = slurp(p, &n);
    if (!log) { log = calloc(1, 1); n = 0; }
    uint64_t clen = 0;
    uint8_t chead[32];
    int ch = committed_head(dir, &clen, chead, why, sizeof why);
    if (ch < 0) { free(log); return diverge(v, 1, "m22.commit", "committed-store", "malformed", "%s", why); }
    if (ch == 0 && n < clen) {
        snprintf(a, sizeof a, "bytes:%llu", (unsigned long long)clen);
        snprintf(b, sizeof b, "bytes:%llu", (unsigned long long)n);
        free(log);
        return diverge(v, n / D_SIZE + 1, "m22.dispatch", a, b, "dispatch.log shorter than the committed length (records omitted)");
    }
    uint64_t upto = ch == 0 ? clen : n - n % D_SIZE;
    if (upto % D_SIZE) {
        free(log);
        return diverge(v, upto / D_SIZE + 1, "m22.commit", "whole-records", "partial", "committed length is not whole records");
    }
    uint8_t head[32] = { 0 };
    for (uint64_t i = 0; i < upto / D_SIZE; i++) {
        const uint8_t *r = log + i * D_SIZE;
        const char *bad = dispatch_shape(r, i);
        if (bad) {
            snprintf(a, sizeof a, "seq:%llu", (unsigned long long)i);
            if (!strcmp(bad, "seq")) snprintf(b, sizeof b, "seq:%llu", (unsigned long long)g64(r + 8));
            else snprintf(b, sizeof b, "bad-%s", bad);
            free(log);
            return diverge(v, i + 1, "m22.dispatch", a, b, "record %llu malformed: %s", (unsigned long long)i, bad);
        }
        uint8_t nh[32];
        chain_step(head, r, nh);
        if (memcmp(nh, r + D_HASHED, 32)) {
            hexs(a, nh); hexs(b, r + D_HASHED);
            free(log);
            return diverge(v, i + 1, "m22.dispatch", a, b, "record %llu: chain does not recompute", (unsigned long long)i);
        }
        memcpy(head, nh, 32);
    }
    if (ch == 0 && memcmp(head, chead, 32)) {
        hexs(a, chead); hexs(b, head);
        free(log);
        return diverge(v, upto / D_SIZE, "m22.commit", a, b, "chain head does not match the committed generation");
    }
    if (ch == 1 && n % D_SIZE) {
        free(log);
        return diverge(v, n / D_SIZE + 1, "m22.dispatch", "record", "truncated", "dispatch.log ends inside a record");
    }
    if (n > upto)
        snprintf(v->detail, sizeof v->detail, "%llu uncommitted bytes after the committed head (recovery drops them)",
                 (unsigned long long)(n - upto));
    v->event = upto / D_SIZE;
    if (out_log) { *out_log = log; *out_n = upto; } else free(log);
    return 0;
}

static int compare_dispatch(const char *da, const char *db) {
    verdict v;
    uint8_t *A, *B;
    size_t na, nb;
    if (verify_dispatch(da, &v, &A, &na)) {
        printf("REFUSED expected store does not verify: event=%llu subsystem=%s (%s)\n",
               (unsigned long long)v.event, v.subsystem, v.detail);
        return 2;
    }
    if (verify_dispatch(db, &v, &B, &nb)) { free(A); return report(&v); }
    memset(&v, 0, sizeof v);
    char a[80], b[80];
    for (size_t i = 0; i * D_SIZE < na || i * D_SIZE < nb; i++) {
        if (i * D_SIZE >= nb) {
            hexs(a, A + i * D_SIZE + D_HASHED);
            diverge(&v, i + 1, "m22.dispatch", a, "none", "replayed log stops early");
            break;
        }
        if (i * D_SIZE >= na) {
            hexs(b, B + i * D_SIZE + D_HASHED);
            diverge(&v, i + 1, "m22.dispatch", "none", b, "replayed log has an extra record");
            break;
        }
        const uint8_t *x = A + i * D_SIZE, *y = B + i * D_SIZE;
        if (memcmp(x + D_HASHED, y + D_HASHED, 32)) {
            hexs(a, x + D_HASHED); hexs(b, y + D_HASHED);
            diverge(&v, i + 1, "m22.dispatch", a, b, "record %zu differs first in: %s", i, dispatch_field_diff(x, y));
            break;
        }
    }
    if (!v.diverged) v.event = na / D_SIZE;
    free(A); free(B);
    return report(&v);
}

/* ---- TRN1 ---------------------------------------------------------------- */

static int trn1_verify_cmd(const char *path) {
    size_t n = 0;
    uint8_t *b = slurp(path, &n);
    if (!b) { fprintf(stderr, "rx_replay: cannot read %s\n", path); return 2; }
    trn1_result r;
    trn1_verify(b, n, &r);
    char line[256];
    trn1_verify_line(&r, line, sizeof line);
    trn1_result_free(&r);
    free(b);
    puts(line);
    return r.code ? 1 : 0;
}

static int trn1_compare_cmd(const char *pa, const char *pb) {
    size_t na = 0, nb = 0;
    uint8_t *a = slurp(pa, &na), *b = slurp(pb, &nb);
    if (!a || !b) { free(a); free(b); fprintf(stderr, "rx_replay: cannot read input\n"); return 2; }
    char line[256];
    trn1_compare_line(a, na, b, nb, line, sizeof line);
    free(a); free(b);
    puts(line);
    return !strncmp(line, "MATCH", 5) ? 0 : !strncmp(line, "refuse expected", 15) ? 2 : 1;
}

/* Run a TRN1 corpus: every expected.txt and compare.txt line must be
 * reproduced exactly. Prints each mismatch and the conformance line. */
static int trn1_corpus(const char *dir) {
    char p[RX_PATH_CAP], l[1024], line[256];
    int pass = 0, fail = 0;
    for (int which = 0; which < 2; which++) {
        if (join_path(p, sizeof p, dir, which ? "compare.txt" : "expected.txt")) return 2;
        FILE *f = fopen(p, "r");
        if (!f) { fprintf(stderr, "rx_replay: cannot open %s\n", p); return 2; }
        while (fgets(l, sizeof l, f)) {
            size_t ll = strlen(l);
            if (ll && l[ll - 1] != '\n' && !feof(f)) {
                /* fgets split the line: refuse it instead of reading two halves. */
                printf("MISMATCH %s: line longer than %zu bytes\n", p, sizeof l - 2);
                fail++;
                int c;
                while ((c = fgetc(f)) != EOF && c != '\n') {}
                continue;
            }
            l[strcspn(l, "\r\n")] = 0;
            if (!l[0] || l[0] == '#') continue;
            char a[RX_PATH_CAP], b[RX_PATH_CAP];
            const char *want;
            char *s1 = strchr(l, ' ');
            if (!s1) { fail++; continue; }
            *s1 = 0;
            if (join_path(a, sizeof a, dir, l)) { printf("MISMATCH %s: path too long\n", l); fail++; continue; }
            size_t na = 0, nb = 0;
            uint8_t *A = slurp(a, &na), *B = NULL;
            if (!A) { A = calloc(1, 1); na = 0; }
            if (which) {
                char *s2 = strchr(s1 + 1, ' ');
                if (!s2) { free(A); fail++; continue; }
                *s2 = 0;
                if (join_path(b, sizeof b, dir, s1 + 1)) {
                    printf("MISMATCH %s: path too long\n", s1 + 1);
                    free(A); fail++; continue;
                }
                B = slurp(b, &nb);
                if (!B) { B = calloc(1, 1); nb = 0; }
                want = s2 + 1;
                trn1_compare_line(A, na, B, nb, line, sizeof line);
            } else {
                want = s1 + 1;
                trn1_result r;
                trn1_verify(A, na, &r);
                trn1_verify_line(&r, line, sizeof line);
                trn1_result_free(&r);
            }
            if (!strcmp(line, want)) pass++;
            else { fail++; printf("MISMATCH %s%s%s\n  want %s\n  got  %s\n", l, which ? " " : "", which ? s1 + 1 : "", want, line); }
            free(A); free(B);
        }
        fclose(f);
    }
    printf("TRN1_CONFORMANCE impl=omega-c pass=%d fail=%d\n", pass, fail);
    return fail ? 1 : 0;
}

static int trn1_export(const char *in, const char *out) {
    rxl_log l;
    char err[160];
    if (rxl_read(in, &l, err, sizeof err)) { fprintf(stderr, "rx_replay: %s\n", err); return 2; }
    if (l.malformed) { fprintf(stderr, "rx_replay: %s: %s\n", in, l.why); rxl_free(&l); return 2; }
    uint8_t *b;
    size_t n;
    int rc = trn1_from_rxlog(&l, &b, &n);
    rxl_free(&l);
    if (rc) { fprintf(stderr, "rx_replay: export failed\n"); return 2; }
    FILE *f = fopen(out, "wb");
    rc = !f || fwrite(b, 1, n, f) != n;
    if (f && fclose(f)) rc = 1;
    free(b);
    return rc ? 2 : 0;
}

/* Flip every bit of a valid transcript, one at a time: each must be refused.
 * Prints the number of accepted flips (survivors), which must be 0. */
static int trn1_flipall(const char *path) {
    size_t n = 0;
    uint8_t *b = slurp(path, &n);
    if (!b) return 2;
    trn1_result r;
    if (trn1_verify(b, n, &r)) { trn1_result_free(&r); free(b); printf("REFUSED input does not verify\n"); return 2; }
    trn1_result_free(&r);
    uint64_t tried = 0, survived = 0;
    for (size_t i = 0; i < n; i++)
        for (int bit = 0; bit < 8; bit++) {
            b[i] ^= (uint8_t)(1u << bit);
            if (trn1_verify(b, n, &r) == TRN1_OK) {
                if (survived < 5) printf("survivor: byte %zu bit %d\n", i, bit);
                survived++;
            }
            trn1_result_free(&r);
            b[i] ^= (uint8_t)(1u << bit);
            tried++;
        }
    free(b);
    printf("TRN1_BITFLIP tried=%llu survived=%llu\n", (unsigned long long)tried, (unsigned long long)survived);
    return survived ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "verify-trn1")) return trn1_verify_cmd(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "compare-trn1")) return trn1_compare_cmd(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "trn1-corpus")) return trn1_corpus(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "export-trn1")) return trn1_export(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "trn1-flipall")) return trn1_flipall(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "verify")) {
        rxl_log l;
        char err[160];
        verdict v;
        if (rxl_read(argv[2], &l, err, sizeof err)) {
            printf("DIVERGENCE event=1 expected=RXCLOG01 actual=unreadable subsystem=world.log\ndetail: %s\n", err);
            return 1;
        }
        verify_log(&l, &v);
        rxl_free(&l);
        return report(&v);
    }
    if (argc == 4 && !strcmp(argv[1], "compare")) return compare_logs(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "compare-causal")) return compare_causal(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "verify-dispatch")) {
        verdict v;
        verify_dispatch(argv[2], &v, NULL, NULL);
        int rc = report(&v);
        if (!rc && v.detail[0]) printf("detail: %s\n", v.detail);
        return rc;
    }
    if (argc == 4 && !strcmp(argv[1], "compare-dispatch")) return compare_dispatch(argv[2], argv[3]);
    fprintf(stderr, "usage: rx_replay verify LOG | compare EXPECTED ACTUAL | compare-causal EXPECTED ACTUAL |\n"
                    "       verify-dispatch DIR | compare-dispatch DIR_A DIR_B |\n"
                    "       export-trn1 RXLOG OUT.trn | verify-trn1 FILE | compare-trn1 EXPECTED ACTUAL |\n"
                    "       trn1-corpus VECTORS_DIR | trn1-flipall FILE\n");
    return 2;
}
