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
#include <time.h>

#include "osc_cg.h"
#include "osc_front.h"
#include "osc_interp.h"
#include "osc_native.h"
#include "osc_rt.h"
#include "model/osc_model.h"

static unsigned long checks, failures;
static unsigned long trap_seen[OSC_TRAP_MAX + 1];
static unsigned long diff_runs, rt_replays, trace_replays, rt_events_replayed;

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

/* runtime pool log -> model ALLOC/RELEASE; windowed ids when > 64 objects */
static int replay_rt(const OscRt *rt, char *why, size_t n)
{
    static OscModel m;
    static uint32_t id_of_serial[1 << 16];
    static uint32_t serial_of_id[OSC_MODEL_MAX_OBJECTS + 1];
    static uint8_t live_id[OSC_MODEL_MAX_OBJECTS + 1];
    uint32_t next = 1;
    osc_model_init(&m, 0, UINT64_MAX);
    memset(live_id, 0, sizeof live_id);
    uint32_t nev = rt->nev < OSC_RT_EVENTS ? rt->nev : OSC_RT_EVENTS;
    for (uint32_t i = 0; i < nev; i++) {
        const OscRtEvent *e = &rt->ev[i];
        if (e->serial >= (1u << 16)) { snprintf(why, n, "serial too large"); return -1; }
        OscModelEvent ev;
        memset(&ev, 0, sizeof ev);
        if (e->kind == OSC_RT_EV_ALLOC) {
            if (next > OSC_MODEL_MAX_OBJECTS) {
                /* new window: re-create the live objects with fresh ids */
                uint32_t keep[OSC_MODEL_MAX_OBJECTS], nk = 0;
                for (uint32_t k = 1; k <= OSC_MODEL_MAX_OBJECTS; k++)
                    if (live_id[k]) keep[nk++] = serial_of_id[k];
                osc_model_init(&m, 0, UINT64_MAX);
                memset(live_id, 0, sizeof live_id);
                next = 1;
                for (uint32_t k = 0; k < nk; k++) {
                    memset(&ev, 0, sizeof ev);
                    ev.kind = OSC_EV_ALLOC;
                    ev.obj = next;
                    if (osc_model_step(&m, &ev, NULL) != OSC_MODEL_ACCEPT) { snprintf(why, n, "re-alloc"); return -1; }
                    id_of_serial[keep[k]] = next;
                    serial_of_id[next] = keep[k];
                    live_id[next] = 1;
                    next++;
                }
                if (next > OSC_MODEL_MAX_OBJECTS) { snprintf(why, n, "64 live objects"); return -1; }
                memset(&ev, 0, sizeof ev);
            }
            ev.kind = OSC_EV_ALLOC;
            ev.obj = next;
            id_of_serial[e->serial] = next;
            serial_of_id[next] = e->serial;
            live_id[next] = 1;
            next++;
        } else {
            ev.kind = OSC_EV_RELEASE;
            ev.obj = id_of_serial[e->serial];
            if (!ev.obj || !live_id[ev.obj]) { snprintf(why, n, "release of unknown serial %u", e->serial); return -1; }
            live_id[ev.obj] = 0;
        }
        OscModelReject r;
        if (osc_model_step(&m, &ev, &r) != OSC_MODEL_ACCEPT) {
            snprintf(why, n, "event %u %s rejected: %s", i, osc_model_event_name(ev.kind), osc_model_reject_name(r));
            return -1;
        }
        rt_events_replayed++;
    }
    return 0;
}

/* ------------------------------------------------------------ golden */
static OscUnit *U1, *U2;
static unsigned long expect_total;
static const char *trap_names[OSC_TRAP_MAX + 1] = {"none", "OVERFLOW", "DIV0", "BOUNDS", "LOOP_BOUND", "CAST", "OOM", "SHIFT", "RUNTIME", "REQUIRES", "ENSURES"};

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
    char path[1024], ent[512], why[160];
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
                if (replay_rt(RN, why, sizeof why) == 0) rt_replays++;
                else CHECK(0, "%s:%s pool log replay rejected: %s", name, tok, why);
            }
        }
        printf("  %-18s %-12s ok=%-5lu ovf=%-4lu div0=%-4lu bnd=%-4lu loop=%-4lu cast=%-4lu oom=%-4lu shift=%-4lu rq=%-4lu en=%-4lu\n",
               name, tok, pertrap[0], pertrap[1], pertrap[2], pertrap[3], pertrap[4], pertrap[5], pertrap[6], pertrap[7], pertrap[9], pertrap[10]);
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

    const char *const *tn = trap_names;
    printf("trap coverage (runs, interpreter == native):\n");
    for (int k = 0; k <= OSC_TRAP_MAX; k++) printf("  %-10s %lu\n", tn[k], trap_seen[k]);
    for (int k = 1; k <= 7; k++) CHECK(trap_seen[k] > 0, "trap %s never observed", tn[k]);
    CHECK(trap_seen[8] == 0, "RUNTIME trap observed");
    CHECK(trap_seen[OSC_TRAP_REQUIRES] > 0 && trap_seen[OSC_TRAP_ENSURES] > 0, "golden fuzz never hit REQUIRES/ENSURES");
    CHECK(traces_skipped_overflow == 0, "%lu golden traces exceeded the model's 64 ids", traces_skipped_overflow);

    for (int i = 0; i < np; i++) free(pv[i]);
    for (int i = 0; i < nn; i++) free(nv[i]);
    free(pv);
    free(nv);
    free(U1); free(U2); free(TR); free(RI); free(RN);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("golden=%lu/%d funcs=%lu entries=%lu expect_runs=%lu diff_runs=%lu trace_replays=%lu rt_replays=%lu rt_events=%lu "
           "neg=%lu/%lu neg_traced=%lu checks=%lu failed=%lu time=%.2fs\n",
           golden_ok, np, golden_funcs, golden_entries, expect_total, diff_runs, trace_replays, rt_replays, rt_events_replayed,
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
