/* M23 G1 search-trace corpus: generator (recorder on the two search hooks) and
 * strict verifier. Format: spec/searchtrace/M23_SEARCH_TRACE_CORPUS_V1.md */
#include "searchtrace/st_corpus.h"
#include "searchtrace/st_hook.h"
#include "omega_synthesis.h"
#include "omega_realize_synth.h"
#include "omega_machine.h"
#include "sha256.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_why(char *why, size_t n, const char *fmt, ...) {
    if (!why || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, n, fmt, ap);
    va_end(ap);
}

void st_hex(const uint8_t *b, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) { out[2 * i] = d[b[i] >> 4]; out[2 * i + 1] = d[b[i] & 15]; }
    out[2 * n] = '\0';
}

void st_buf_free(StBuf *b) {
    if (!b) return;
    free(b->p);
    memset(b, 0, sizeof *b);
}

static void buf_put(StBuf *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 65536;
        while (b->n + n + 1 > cap) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) { b->oom = 1; return; }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static void buf_printf(StBuf *b, const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= sizeof tmp) { b->oom = 1; return; }
    buf_put(b, tmp, (size_t)k);
}

/* ---------------------------------------------------------------- task set */

static bool name_ok(const char *s, size_t n) {
    if (n == 0 || n > 60) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '-')) return false;
    }
    return true;
}

/* Canonical unsigned decimal: no sign, no leading zero (except "0"), fits u64. */
static bool parse_u64(const char *s, size_t n, uint64_t *out) {
    if (n == 0 || n > 20 || (n > 1 && s[0] == '0')) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        uint64_t d = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

/* Canonical signed decimal (for return codes): "-" only before nonzero. */
static bool parse_i64(const char *s, size_t n, int64_t *out) {
    bool neg = n > 0 && s[0] == '-';
    uint64_t v;
    if (!parse_u64(s + neg, n - neg, &v) || v > (uint64_t)INT32_MAX + 1) return false;
    if (neg && v == 0) return false;
    *out = neg ? -(int64_t)v : (int64_t)v;
    return true;
}

static bool expect_kv(const char **pp, const char *end, const char *key, const char **v, size_t *vn) {
    const char *p = *pp;
    size_t kl = strlen(key);
    if ((size_t)(end - p) < kl + 1 || memcmp(p, key, kl) != 0 || p[kl] != '=') return false;
    p += kl + 1;
    const char *q = p;
    while (q < end && *q != ' ') ++q;
    *v = p;
    *vn = (size_t)(q - p);
    if (q < end) ++q;            /* single space separator */
    *pp = q;
    return true;
}

int st_taskset_parse(const char *data, size_t len, StTaskSet *ts, char *why, size_t why_len) {
    if (!data || !ts) { set_why(why, why_len, "bad arguments"); return -1; }
    memset(ts, 0, sizeof *ts);
    if (memchr(data, '\0', len) || memchr(data, '\r', len)) { set_why(why, why_len, "task set contains NUL or CR"); return -1; }
    if (len == 0 || data[len - 1] != '\n') { set_why(why, why_len, "task set does not end with a newline"); return -1; }
    const char *p = data, *end = data + len;
    size_t lineno = 0;
    bool got_end = false;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl;
        ++lineno;
        if (got_end) { set_why(why, why_len, "line %zu: bytes after end", lineno); return -1; }
        if (le > p && le[-1] == ' ') { set_why(why, why_len, "line %zu: trailing space", lineno); return -1; }
        size_t ln = (size_t)(le - p);
        if (lineno == 1) {
            if (ln != strlen(ST_TASKSET_MAGIC) || memcmp(p, ST_TASKSET_MAGIC, ln) != 0) {
                set_why(why, why_len, "line 1: not an %s task set", ST_TASKSET_MAGIC); return -1;
            }
        } else if (ln > 0 && p[0] == '#') {
            /* comment line: part of the digest, ignored by the parser */
        } else if (ln == 3 && memcmp(p, "end", 3) == 0) {
            got_end = true;
        } else if (ln > 5 && memcmp(p, "task ", 5) == 0) {
            if (ts->n_tasks >= ST_MAX_TASKS) { set_why(why, why_len, "line %zu: more than %d tasks", lineno, ST_MAX_TASKS); return -1; }
            StTask *t = &ts->tasks[ts->n_tasks];
            const char *q = p + 5, *ne = q;
            while (ne < le && *ne != ' ') ++ne;
            if (!name_ok(q, (size_t)(ne - q)) || ne >= le) { set_why(why, why_len, "line %zu: bad task name", lineno); return -1; }
            memcpy(t->name, q, (size_t)(ne - q));
            for (uint32_t k = 0; k < ts->n_tasks; ++k)
                if (strcmp(ts->tasks[k].name, t->name) == 0) { set_why(why, why_len, "line %zu: duplicate task name", lineno); return -1; }
            q = ne + 1;
            const char *v; size_t vn; uint64_t u;
            if (!expect_kv(&q, le, "depth", &v, &vn) || !parse_u64(v, vn, &u) || u < 1 || u > 3) { set_why(why, why_len, "line %zu: bad depth", lineno); return -1; }
            t->depth = (uint32_t)u;
            if (!expect_kv(&q, le, "max_cost", &v, &vn) || !parse_u64(v, vn, &u) || u < 1 || u > 64) { set_why(why, why_len, "line %zu: bad max_cost", lineno); return -1; }
            t->max_cost = (uint32_t)u;
            if (!expect_kv(&q, le, "max_candidates", &v, &vn) || !parse_u64(v, vn, &u) || u < 1 || u > 100000) { set_why(why, why_len, "line %zu: bad max_candidates", lineno); return -1; }
            t->max_candidates = (uint32_t)u;
            if (!expect_kv(&q, le, "dedup", &v, &vn) || !parse_u64(v, vn, &u) || u > 1) { set_why(why, why_len, "line %zu: bad dedup", lineno); return -1; }
            t->dedup = (uint32_t)u;
            if (!expect_kv(&q, le, "pairs", &v, &vn) || q != le) { set_why(why, why_len, "line %zu: bad pairs field or trailing fields", lineno); return -1; }
            const char *a = v, *ve = v + vn;
            while (a < ve) {
                const char *c = memchr(a, ',', (size_t)(ve - a));
                const char *pe = c ? c : ve;
                const char *colon = memchr(a, ':', (size_t)(pe - a));
                if (!colon || t->n_pairs >= ST_MAX_PAIRS ||
                    !parse_u64(a, (size_t)(colon - a), &t->in[t->n_pairs]) ||
                    !parse_u64(colon + 1, (size_t)(pe - colon - 1), &t->out[t->n_pairs])) {
                    set_why(why, why_len, "line %zu: bad pair list", lineno); return -1;
                }
                t->n_pairs++;
                if (c && c + 1 == ve) { set_why(why, why_len, "line %zu: trailing comma", lineno); return -1; }
                a = c ? c + 1 : ve;
            }
            if (t->n_pairs == 0) { set_why(why, why_len, "line %zu: no pairs", lineno); return -1; }
            ts->n_tasks++;
        } else {
            set_why(why, why_len, "line %zu: unknown line", lineno); return -1;
        }
        p = nl + 1;
    }
    if (!got_end) { set_why(why, why_len, "task set has no end line (truncated)"); return -1; }
    if (ts->n_tasks == 0) { set_why(why, why_len, "task set has no tasks"); return -1; }
    sha256_hash((const uint8_t *)data, len, ts->digest);
    return 0;
}

/* ---------------------------------------------------------------- recorder */

static const char *PRUNE_NAMES[] = { "none", "type", "cost", "equiv", "budget" };
static const char *VERDICT_NAMES[] = { "none", "reject", "solved", "verify_fail" };

typedef struct {
    StBuf *out;
    uint32_t t;
    uint64_t seq;
    const OmegaMachineGraph *mg;
    SemanticId ids1[SYNTH_MAX_PRIMITIVES];
    uint64_t seq1[SYNTH_MAX_PRIMITIVES];
    size_t n1;
    SemanticId ids2[SYNTH_MAX_CANDIDATES];
    uint64_t seq2[SYNTH_MAX_CANDIDATES];
    size_t n2;
    bool budget_seen;
    bool realize_seen;
    int32_t r_rc;
    uint32_t r_bytes, r_cycles, r_checked;
    int err;
    char why[192];
} StRec;

static void rec_fail(StRec *r, const char *msg) {
    if (!r->err) { r->err = 1; snprintf(r->why, sizeof r->why, "task %u seq %" PRIu64 ": %s", r->t, r->seq, msg); }
}

static void rec_realize(void *ctx, const StEvent *ev) {
    StRec *r = ctx;
    if (ev->kind != ST_EV_REALIZATION) { rec_fail(r, "wrong event kind at realize hook"); return; }
    const RealizationSynthesisResult *res = ev->u.realize.result;
    r->realize_seen = true;
    r->r_rc = ev->u.realize.rc;
    r->r_bytes = res->code_bytes_len;
    r->r_cycles = res->estimated_cycles;
    r->r_checked = res->inputs_checked;
}

static void put_body(StBuf *b, const OmegaProgram *pr) {
    if (!pr->body.has_body || pr->body.step_count == 0) { buf_put(b, "-", 1); return; }
    for (uint16_t k = 0; k < pr->body.step_count && k < OMEGA_PROGRAM_MAX_STEPS; ++k)
        buf_printf(b, "%s%02x:%" PRIx64, k ? "/" : "", pr->body.steps[k].op, pr->body.steps[k].imm);
}

static void rec_synth(void *ctx, const StEvent *ev) {
    StRec *r = ctx;
    if (r->err) return;
    if (ev->kind != ST_EV_SYNTH_CANDIDATE) { rec_fail(r, "wrong event kind at synth hook"); return; }
    const StSynthEvent *e = &ev->u.synth;
    if (r->budget_seen) { rec_fail(r, "event after budget cutoff"); return; }
    if (e->prune > ST_PRUNE_BUDGET || e->verdict > ST_VERDICT_VERIFY_FAIL || e->depth < 1 || e->depth > 3) {
        rec_fail(r, "event field out of range"); return;
    }
    const OmegaProgram *parent = e->parent, *prim = e->prim, *child = e->child;
    char hx[65];

    buf_printf(r->out, "step %u %" PRIu64 " d=%u parent=", r->t, r->seq, e->depth);
    if (e->depth == 1) {
        if (parent) { rec_fail(r, "depth-1 event with a parent"); return; }
        buf_put(r->out, "-", 1);
    } else {
        const SemanticId *ids = e->depth == 2 ? r->ids1 : r->ids2;
        const uint64_t *seqs = e->depth == 2 ? r->seq1 : r->seq2;
        size_t n = e->depth == 2 ? r->n1 : r->n2;
        if (!parent || e->parent_index < 0 || (size_t)e->parent_index >= n ||
            memcmp(ids[e->parent_index].bytes, parent->program_id.bytes, OMEGA_ID_BYTES) != 0) {
            rec_fail(r, "parent does not match a recorded kept candidate"); return;
        }
        buf_printf(r->out, "%" PRIu64, seqs[e->parent_index]);
    }
    if (!prim || !prim->body.has_body || prim->body.step_count != 1) { rec_fail(r, "primitive is not a one-step program"); return; }
    buf_printf(r->out, " prim=%d pname=%s transform=%02x:%" PRIx64 " child=", e->prim_index,
               prim->name[0] ? prim->name : "-", prim->body.steps[0].op, prim->body.steps[0].imm);
    if (child) {
        st_hex(child->program_id.bytes, OMEGA_ID_BYTES, hx);
        buf_printf(r->out, "%s nsteps=%u insn=%u body=", hx, child->body.step_count, child->cost.insn_count);
        put_body(r->out, child);
    } else {
        buf_put(r->out, "- nsteps=- insn=- body=-", 24);
    }
    if (e->has_signature) { st_hex(e->signature, 32, hx); buf_printf(r->out, " sig=%s", hx); }
    else buf_put(r->out, " sig=-", 6);
    buf_printf(r->out, " prune=%s verdict=%s erc=%d vrc=%d", PRUNE_NAMES[e->prune], VERDICT_NAMES[e->verdict],
               e->eval_rc, e->verify_rc);

    /* realization cost: every evaluated candidate is realized for the fixed machine graph */
    if (e->prune == ST_PRUNE_NONE && child) {
        RealizationSynthesisTask rt;
        RealizationSynthesisResult *res = malloc(sizeof *res);
        if (!res) { rec_fail(r, "out of memory"); return; }
        r->realize_seen = false;
        omega_realize_task_init(&rt, child, r->mg);
        int rc = omega_synthesize_realization(&rt, res);
        free(res);
        if (!r->realize_seen || r->r_rc != rc) { rec_fail(r, "realize hook did not report the realization"); return; }
        buf_printf(r->out, " rrc=%d rbytes=%u rcycles=%u rchecked=%u\n", r->r_rc, r->r_bytes, r->r_cycles, r->r_checked);
    } else {
        buf_put(r->out, " rrc=- rbytes=- rcycles=- rchecked=-\n", 37);
    }

    /* mirror the search's kept lists so later parents resolve to step numbers */
    if (e->prune == ST_PRUNE_NONE && e->verdict != ST_VERDICT_SOLVED && child) {
        if (e->depth == 1 && r->n1 < SYNTH_MAX_PRIMITIVES) {
            r->ids1[r->n1] = child->program_id; r->seq1[r->n1++] = r->seq;
        } else if (e->depth == 2 && r->n2 < SYNTH_MAX_CANDIDATES) {
            r->ids2[r->n2] = child->program_id; r->seq2[r->n2++] = r->seq;
        }
    }
    if (e->prune == ST_PRUNE_BUDGET) r->budget_seen = true;
    r->seq++;
}

static int build_task(const StTask *st, SynthesisTask *task, SynthesisConfig *cfg) {
    if (omega_task_init(task, st->name, TYPE_UNSIGNED_INT, 64, TYPE_UNSIGNED_INT, 64,
                        st->in, st->out, st->n_pairs) != 0) return -1;
    memset(cfg, 0, sizeof *cfg);
    cfg->max_depth = st->depth;
    cfg->max_cost = st->max_cost;
    cfg->max_candidates = st->max_candidates;
    cfg->deduplicate_equiv = st->dedup != 0;
    return 0;
}

int st_corpus_generate(const StTaskSet *ts, StBuf *out, char *why, size_t why_len) {
    if (!ts || !out || out->n != 0) { set_why(why, why_len, "bad arguments"); return -1; }
    OmegaMachineGraph mg;
    if (omega_machine_build_dgx_spark(&mg) != 0) { set_why(why, why_len, "machine graph build failed"); return -1; }
    SynthPrimitiveBank bank;
    if (omega_synth_bank_init(&bank) != 0) { set_why(why, why_len, "primitive bank init failed"); return -1; }
    StRec *r = calloc(1, sizeof *r);
    if (!r) { omega_synth_bank_destroy(&bank); set_why(why, why_len, "out of memory"); return -1; }
    r->out = out;
    r->mg = &mg;
    char hx[65];
    int ret = -1;
    uint64_t total = 0;

    buf_printf(out, "%s\n", ST_CORPUS_MAGIC);
    st_hex(ts->digest, 32, hx);
    buf_printf(out, "taskset %s\n", hx);
    st_hex(mg.machine_id.bytes, OMEGA_ID_BYTES, hx);
    buf_printf(out, "machine %s bank=%zu\n", hx, bank.count);

    if (omega_synth_set_trace_hook(rec_synth, r) != 0 || omega_realize_set_trace_hook(rec_realize, r) != 0) {
        omega_synth_clear_trace_hook(r);
        set_why(why, why_len, "a trace hook is already installed on this thread");
        goto done;
    }
    for (uint32_t t = 0; t < ts->n_tasks; ++t) {
        const StTask *st = &ts->tasks[t];
        SynthesisTask task;
        SynthesisConfig cfg;
        if (build_task(st, &task, &cfg) != 0) { set_why(why, why_len, "task %u: init failed", t); goto unhook; }
        st_hex(task.task_id.bytes, OMEGA_ID_BYTES, hx);
        buf_printf(out, "task %u name=%s task_id=%s depth=%u max_cost=%u max_candidates=%u dedup=%u examples=%u\n",
                   t, st->name, hx, st->depth, st->max_cost, st->max_candidates, st->dedup, st->n_pairs);
        r->t = t; r->seq = 0; r->n1 = 0; r->n2 = 0; r->budget_seen = false;
        SynthesisResult res;
        int rc = omega_synthesize(&task, &bank, &cfg, &res);
        if (r->err) { set_why(why, why_len, "%s", r->why); goto unhook; }
        if (rc != 0) { set_why(why, why_len, "task %u: synthesis returned %d", t, rc); goto unhook; }
        if (res.solved) st_hex(res.solution.program_id.bytes, OMEGA_ID_BYTES, hx);
        buf_printf(out, "result %u solved=%d solution=%s generated=%zu pruned_type=%zu pruned_equiv=%zu failed=%zu solutions=%zu steps=%" PRIu64 "\n",
                   t, res.solved ? 1 : 0, res.solved ? hx : "-", res.stats.candidates_generated,
                   res.stats.candidates_pruned_type, res.stats.candidates_pruned_equiv,
                   res.stats.candidates_failed_v1, res.stats.solutions_found, r->seq);
        total += r->seq;
    }
    if (out->oom) { set_why(why, why_len, "out of memory"); goto unhook; }
    {
        uint8_t dg[32];
        sha256_hash((const uint8_t *)out->p, out->n, dg);
        st_hex(dg, 32, hx);
        buf_printf(out, "end tasks=%u steps=%" PRIu64 " digest=%s\n", ts->n_tasks, total, hx);
    }
    ret = out->oom ? -1 : 0;
    if (out->oom) set_why(why, why_len, "out of memory");
unhook:
    omega_synth_clear_trace_hook(r);
    omega_realize_clear_trace_hook(r);
done:
    omega_synth_bank_destroy(&bank);
    free(r);
    return ret;
}

/* ---------------------------------------------------------------- verifier */

static bool hex_ok(const char *v, size_t n, size_t want) {
    if (n != want) return false;
    for (size_t i = 0; i < n; ++i)
        if (!((v[i] >= '0' && v[i] <= '9') || (v[i] >= 'a' && v[i] <= 'f'))) return false;
    return true;
}

static bool is_dash(const char *v, size_t n) { return n == 1 && v[0] == '-'; }

static int enum_idx(const char *v, size_t n, const char *const *names, int count) {
    for (int i = 0; i < count; ++i)
        if (strlen(names[i]) == n && memcmp(names[i], v, n) == 0) return i;
    return -1;
}

/* <2 hex>:<hex imm, no leading zeros> */
static bool opimm_ok(const char *v, size_t n) {
    if (n < 4 || !hex_ok(v, 2, 2) || v[2] != ':') return false;
    size_t m = n - 3;
    if (m > 16 || (m > 1 && v[3] == '0')) return false;
    return hex_ok(v + 3, m, m);
}

static bool body_ok(const char *v, size_t n, uint64_t *steps) {
    *steps = 0;
    const char *a = v, *e = v + n;
    while (a < e) {
        const char *s = memchr(a, '/', (size_t)(e - a));
        const char *pe = s ? s : e;
        if (!opimm_ok(a, (size_t)(pe - a))) return false;
        (*steps)++;
        if (s && s + 1 == e) return false;
        a = s ? s + 1 : e;
    }
    return *steps > 0;
}

#define VFAIL(...) do { set_why(why, why_len, __VA_ARGS__); return -1; } while (0)

int st_corpus_verify(const char *data, size_t len, const StTaskSet *ts,
                     uint8_t digest_out[32], uint64_t *steps_out, char *why, size_t why_len) {
    if (!data) VFAIL("bad arguments");
    if (memchr(data, '\0', len) || memchr(data, '\r', len)) VFAIL("corpus contains NUL or CR");
    if (len == 0 || data[len - 1] != '\n') VFAIL("corpus does not end with a newline (partial)");
    const char *p = data, *end = data + len;
    size_t lineno = 0;
    enum { S_HDR, S_TASKSET, S_MACHINE, S_TASK, S_STEP, S_DONE } st = S_HDR;
    uint32_t cur_task = 0, n_tasks = 0;
    uint64_t seq = 0, total = 0;
    bool budget_seen = false;
    const char *v; size_t vn; uint64_t u; int64_t sv;
    char key[32];

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl;
        size_t ln = (size_t)(le - p);
        ++lineno;
        if (ln == 0) VFAIL("line %zu: empty line", lineno);
        if (le[-1] == ' ') VFAIL("line %zu: trailing space", lineno);
        const char *q;
        switch (st) {
        case S_HDR:
            if (ln != strlen(ST_CORPUS_MAGIC) || memcmp(p, ST_CORPUS_MAGIC, ln) != 0) VFAIL("line 1: not an %s corpus", ST_CORPUS_MAGIC);
            st = S_TASKSET;
            break;
        case S_TASKSET:
            if (ln != 8 + 64 || memcmp(p, "taskset ", 8) != 0 || !hex_ok(p + 8, 64, 64)) VFAIL("line %zu: bad taskset line", lineno);
            if (ts) {
                char hx[65];
                st_hex(ts->digest, 32, hx);
                if (memcmp(p + 8, hx, 64) != 0) VFAIL("corpus belongs to a different task set");
            }
            st = S_MACHINE;
            break;
        case S_MACHINE:
            q = p + 8;
            if (ln < 8 + 64 + 1 || memcmp(p, "machine ", 8) != 0 || !hex_ok(q, 64, 64) || q[64] != ' ') VFAIL("line %zu: bad machine line", lineno);
            q += 65;
            if (!expect_kv(&q, le, "bank", &v, &vn) || !parse_u64(v, vn, &u) || u == 0 || u > SYNTH_MAX_PRIMITIVES || q != le)
                VFAIL("line %zu: bad machine line", lineno);
            st = S_TASK;
            break;
        case S_TASK:
            if (ln >= 4 && memcmp(p, "end ", 4) == 0) {
                q = p + 4;
                if (!expect_kv(&q, le, "tasks", &v, &vn) || !parse_u64(v, vn, &u) || u != n_tasks) VFAIL("line %zu: end task count mismatch", lineno);
                if (!expect_kv(&q, le, "steps", &v, &vn) || !parse_u64(v, vn, &u) || u != total) VFAIL("line %zu: end step count mismatch", lineno);
                if (!expect_kv(&q, le, "digest", &v, &vn) || !hex_ok(v, vn, 64) || q != le) VFAIL("line %zu: bad end digest", lineno);
                uint8_t dg[32]; char hx[65];
                sha256_hash((const uint8_t *)data, (size_t)(p - data), dg);
                st_hex(dg, 32, hx);
                if (memcmp(v, hx, 64) != 0) VFAIL("corpus digest mismatch (content altered)");
                if (ts && n_tasks != ts->n_tasks) VFAIL("corpus has %u tasks, task set has %u", n_tasks, ts->n_tasks);
                if (n_tasks == 0) VFAIL("corpus has no tasks");
                if (digest_out) memcpy(digest_out, dg, 32);
                if (steps_out) *steps_out = total;
                st = S_DONE;
                break;
            }
            if (ln < 5 || memcmp(p, "task ", 5) != 0) VFAIL("line %zu: expected task or end line", lineno);
            q = p + 5;
            {
                const char *ne = memchr(q, ' ', (size_t)(le - q));
                if (!ne || !parse_u64(q, (size_t)(ne - q), &u) || u != n_tasks) VFAIL("line %zu: task index out of order", lineno);
                q = ne + 1;
            }
            if (!expect_kv(&q, le, "name", &v, &vn) || !name_ok(v, vn)) VFAIL("line %zu: bad task name", lineno);
            if (ts && (n_tasks >= ts->n_tasks || strlen(ts->tasks[n_tasks].name) != vn ||
                       memcmp(ts->tasks[n_tasks].name, v, vn) != 0)) VFAIL("line %zu: task name differs from task set", lineno);
            if (!expect_kv(&q, le, "task_id", &v, &vn) || !hex_ok(v, vn, 64)) VFAIL("line %zu: bad task_id", lineno);
            {
                static const char *const keys[] = { "depth", "max_cost", "max_candidates", "dedup", "examples" };
                for (int k = 0; k < 5; ++k)
                    if (!expect_kv(&q, le, keys[k], &v, &vn) || !parse_u64(v, vn, &u)) VFAIL("line %zu: bad %s", lineno, keys[k]);
            }
            if (q != le) VFAIL("line %zu: trailing fields", lineno);
            cur_task = n_tasks++;
            seq = 0;
            budget_seen = false;
            st = S_STEP;
            break;
        case S_STEP:
            if (ln >= 7 && memcmp(p, "result ", 7) == 0) {
                q = p + 7;
                const char *ne = memchr(q, ' ', (size_t)(le - q));
                if (!ne || !parse_u64(q, (size_t)(ne - q), &u) || u != cur_task) VFAIL("line %zu: result for wrong task", lineno);
                q = ne + 1;
                if (!expect_kv(&q, le, "solved", &v, &vn) || !parse_u64(v, vn, &u) || u > 1) VFAIL("line %zu: bad solved", lineno);
                uint64_t solved = u;
                if (!expect_kv(&q, le, "solution", &v, &vn) || !(solved ? hex_ok(v, vn, 64) : is_dash(v, vn))) VFAIL("line %zu: bad solution", lineno);
                static const char *const keys[] = { "generated", "pruned_type", "pruned_equiv", "failed", "solutions" };
                for (int k = 0; k < 5; ++k)
                    if (!expect_kv(&q, le, keys[k], &v, &vn) || !parse_u64(v, vn, &u)) VFAIL("line %zu: bad %s", lineno, keys[k]);
                if (!expect_kv(&q, le, "steps", &v, &vn) || !parse_u64(v, vn, &u) || u != seq || q != le) VFAIL("line %zu: result step count mismatch (partial task)", lineno);
                total += seq;
                st = S_TASK;
                break;
            }
            if (ln < 5 || memcmp(p, "step ", 5) != 0) VFAIL("line %zu: expected step or result line", lineno);
            if (budget_seen) VFAIL("line %zu: step after budget cutoff", lineno);
            q = p + 5;
            {
                const char *ne = memchr(q, ' ', (size_t)(le - q));
                if (!ne || !parse_u64(q, (size_t)(ne - q), &u) || u != cur_task) VFAIL("line %zu: step for wrong task", lineno);
                q = ne + 1;
                ne = memchr(q, ' ', (size_t)(le - q));
                if (!ne || !parse_u64(q, (size_t)(ne - q), &u) || u != seq) VFAIL("line %zu: step sequence gap", lineno);
                q = ne + 1;
            }
            {
                uint64_t depth, nsteps = 0, bsteps = 0;
                if (!expect_kv(&q, le, "d", &v, &vn) || !parse_u64(v, vn, &depth) || depth < 1 || depth > 3) VFAIL("line %zu: bad d", lineno);
                if (!expect_kv(&q, le, "parent", &v, &vn)) VFAIL("line %zu: missing parent", lineno);
                if (depth == 1 ? !is_dash(v, vn) : (!parse_u64(v, vn, &u) || u >= seq)) VFAIL("line %zu: bad parent", lineno);
                if (!expect_kv(&q, le, "prim", &v, &vn) || !parse_u64(v, vn, &u) || u >= SYNTH_MAX_PRIMITIVES) VFAIL("line %zu: bad prim", lineno);
                if (!expect_kv(&q, le, "pname", &v, &vn) || !(name_ok(v, vn) || is_dash(v, vn))) VFAIL("line %zu: bad pname", lineno);
                if (!expect_kv(&q, le, "transform", &v, &vn) || !opimm_ok(v, vn)) VFAIL("line %zu: bad transform", lineno);
                if (!expect_kv(&q, le, "child", &v, &vn)) VFAIL("line %zu: missing child", lineno);
                bool has_child = !is_dash(v, vn);
                if (has_child && !hex_ok(v, vn, 64)) VFAIL("line %zu: bad child id", lineno);
                if (!expect_kv(&q, le, "nsteps", &v, &vn) || !(has_child ? parse_u64(v, vn, &nsteps) : is_dash(v, vn))) VFAIL("line %zu: bad nsteps", lineno);
                if (!expect_kv(&q, le, "insn", &v, &vn) || !(has_child ? parse_u64(v, vn, &u) : is_dash(v, vn))) VFAIL("line %zu: bad insn", lineno);
                if (!expect_kv(&q, le, "body", &v, &vn) || !(has_child ? body_ok(v, vn, &bsteps) && bsteps == nsteps : is_dash(v, vn)))
                    VFAIL("line %zu: bad body", lineno);
                if (!expect_kv(&q, le, "sig", &v, &vn) || !(is_dash(v, vn) || hex_ok(v, vn, 64))) VFAIL("line %zu: bad sig", lineno);
                bool has_sig = !is_dash(v, vn);
                if (!expect_kv(&q, le, "prune", &v, &vn)) VFAIL("line %zu: missing prune", lineno);
                int prune = enum_idx(v, vn, PRUNE_NAMES, 5);
                if (!expect_kv(&q, le, "verdict", &v, &vn)) VFAIL("line %zu: missing verdict", lineno);
                int verdict = enum_idx(v, vn, VERDICT_NAMES, 4);
                if (prune < 0 || verdict < 0) VFAIL("line %zu: unknown prune or verdict", lineno);
                if ((prune == ST_PRUNE_NONE) != (verdict != ST_VERDICT_NONE)) VFAIL("line %zu: verdict inconsistent with prune", lineno);
                if ((prune == ST_PRUNE_TYPE || prune == ST_PRUNE_BUDGET) == has_child) VFAIL("line %zu: child presence inconsistent with prune", lineno);
                if (prune == ST_PRUNE_EQUIV && !has_sig) VFAIL("line %zu: equivalence prune without signature", lineno);
                if (depth == 1 && (prune == ST_PRUNE_TYPE || prune == ST_PRUNE_COST || prune == ST_PRUNE_BUDGET)) VFAIL("line %zu: impossible depth-1 prune", lineno);
                if (!expect_kv(&q, le, "erc", &v, &vn) || !parse_i64(v, vn, &sv)) VFAIL("line %zu: bad erc", lineno);
                if (!expect_kv(&q, le, "vrc", &v, &vn) || !parse_i64(v, vn, &sv)) VFAIL("line %zu: bad vrc", lineno);
                bool realized = prune == ST_PRUNE_NONE;
                const char *rk[] = { "rrc", "rbytes", "rcycles", "rchecked" };
                for (int k = 0; k < 4; ++k) {
                    snprintf(key, sizeof key, "%s", rk[k]);
                    if (!expect_kv(&q, le, key, &v, &vn)) VFAIL("line %zu: missing %s", lineno, key);
                    bool okv = realized ? (k == 0 ? parse_i64(v, vn, &sv) : parse_u64(v, vn, &u)) : is_dash(v, vn);
                    if (!okv) VFAIL("line %zu: bad %s", lineno, key);
                }
                if (q != le) VFAIL("line %zu: trailing fields", lineno);
                if (prune == ST_PRUNE_BUDGET) budget_seen = true;
            }
            seq++;
            break;
        case S_DONE:
            VFAIL("line %zu: bytes after end line", lineno);
        }
        p = nl + 1;
    }
    if (st != S_DONE) VFAIL("corpus has no end line (partial corpus)");
    return 0;
}

/* ------------------------------------------------------ hooks-off equivalence */

static void count_synth(void *ctx, const StEvent *ev) { (void)ev; ++*(uint64_t *)ctx; }

int st_hook_equivalence(const StTaskSet *ts, char *why, size_t why_len) {
    if (!ts) VFAIL("bad arguments");
    SynthPrimitiveBank bank;
    if (omega_synth_bank_init(&bank) != 0) VFAIL("bank init failed");
    int ret = 0;
    for (uint32_t t = 0; t < ts->n_tasks && ret == 0; ++t) {
        SynthesisTask task; SynthesisConfig cfg;
        SynthesisResult off, on;
        uint64_t events = 0;
        if (build_task(&ts->tasks[t], &task, &cfg) != 0) { set_why(why, why_len, "task %u init failed", t); ret = -1; break; }
        int rc_off = omega_synthesize(&task, &bank, &cfg, &off);
        if (omega_synth_set_trace_hook(count_synth, &events) != 0) { set_why(why, why_len, "hook busy"); ret = -1; break; }
        int rc_on = omega_synthesize(&task, &bank, &cfg, &on);
        omega_synth_clear_trace_hook(&events);
        if (rc_off != rc_on || off.solved != on.solved ||
            memcmp(&off.stats, &on.stats, sizeof off.stats) != 0 ||
            (off.solved && (memcmp(off.solution.program_id.bytes, on.solution.program_id.bytes, OMEGA_ID_BYTES) != 0 ||
                            off.verify_report.passed != on.verify_report.passed))) {
            set_why(why, why_len, "task %u (%s): result differs with the hook installed", t, ts->tasks[t].name);
            ret = -1;
        } else if (events == 0) {
            set_why(why, why_len, "task %u (%s): hook installed but no events", t, ts->tasks[t].name);
            ret = -1;
        }
    }
    omega_synth_bank_destroy(&bank);
    return ret;
}
