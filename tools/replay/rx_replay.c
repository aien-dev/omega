/* rx_replay.c -- independent verifier and replay comparator for World crumb
 * logs (RXCLOG01, tools/replay/rxlog.h) and M22 dispatch logs
 * (src/train/tg_store.h dispatch.log). Links libc + src/sha256.c only.
 *
 *   rx_replay verify LOG               one log against its own rules
 *   rx_replay compare EXPECTED ACTUAL  recorded run vs replayed run
 *   rx_replay verify-dispatch DIR      dispatch.log chain + committed head
 *   rx_replay compare-dispatch A B     two dispatch logs, record by record
 *
 * Output is exactly one verdict line:
 *   MATCH through event N
 *   DIVERGENCE event=N expected=<hash|value> actual=<hash|value> subsystem=<name>
 * optionally followed by one "detail:" line. Events are numbered from 1 in
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
    char p[4096];
    snprintf(p, sizeof p, "%s/CURRENT", dir);
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
    snprintf(p, sizeof p, "%s/gen-%s.bin", dir, gen);
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
    char p[4096], a[80], b[80], why[160];
    snprintf(p, sizeof p, "%s/dispatch.log", dir);
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
    char p[4096], l[1024], line[256];
    int pass = 0, fail = 0;
    for (int which = 0; which < 2; which++) {
        snprintf(p, sizeof p, "%s/%s", dir, which ? "compare.txt" : "expected.txt");
        FILE *f = fopen(p, "r");
        if (!f) { fprintf(stderr, "rx_replay: cannot open %s\n", p); return 2; }
        while (fgets(l, sizeof l, f)) {
            l[strcspn(l, "\r\n")] = 0;
            if (!l[0] || l[0] == '#') continue;
            char a[512], b[512];
            const char *want;
            char *s1 = strchr(l, ' ');
            if (!s1) { fail++; continue; }
            *s1 = 0;
            snprintf(a, sizeof a, "%s/%s", dir, l);
            size_t na = 0, nb = 0;
            uint8_t *A = slurp(a, &na), *B = NULL;
            if (!A) { A = calloc(1, 1); na = 0; }
            if (which) {
                char *s2 = strchr(s1 + 1, ' ');
                if (!s2) { free(A); fail++; continue; }
                *s2 = 0;
                snprintf(b, sizeof b, "%s/%s", dir, s1 + 1);
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
    if (argc == 3 && !strcmp(argv[1], "verify-dispatch")) {
        verdict v;
        verify_dispatch(argv[2], &v, NULL, NULL);
        int rc = report(&v);
        if (!rc && v.detail[0]) printf("detail: %s\n", v.detail);
        return rc;
    }
    if (argc == 4 && !strcmp(argv[1], "compare-dispatch")) return compare_dispatch(argv[2], argv[3]);
    fprintf(stderr, "usage: rx_replay verify LOG | compare EXPECTED ACTUAL |\n"
                    "       verify-dispatch DIR | compare-dispatch DIR_A DIR_B |\n"
                    "       export-trn1 RXLOG OUT.trn | verify-trn1 FILE | compare-trn1 EXPECTED ACTUAL |\n"
                    "       trn1-corpus VECTORS_DIR | trn1-flipall FILE\n");
    return 2;
}
