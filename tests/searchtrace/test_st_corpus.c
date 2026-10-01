/* M23 G1 corpus: determinism, strict refusal of malformed/partial corpora and
 * task sets, hook API rules, and hooks-off equivalence on a small task set. */
#include "searchtrace/st_corpus.h"
#include "searchtrace/st_hook.h"
#include "omega_synthesis.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, name) do { if (cond) { ++g_pass; } else { ++g_fail; printf("FAIL %s\n", name); } } while (0)

static const char TS[] =
    "OMEGA-SEARCHTRACE-TASKSET v1\n"
    "# small set for unit tests\n"
    "task add3 depth=1 max_cost=10 max_candidates=100 dedup=1 pairs=0:3,1:4,7:10\n"
    "task affine depth=2 max_cost=10 max_candidates=500 dedup=1 pairs=0:1,1:3,2:5,10:21\n"
    "task cut depth=3 max_cost=10 max_candidates=60 dedup=0 pairs=0:0,1:1,2:4,3:9\n"
    "end\n";

static int verify(const char *d, size_t n, const StTaskSet *ts) {
    char why[192];
    return st_corpus_verify(d, n, ts, NULL, NULL, why, sizeof why);
}

static int ts_refused(const char *s) {
    StTaskSet *ts = malloc(sizeof *ts);
    char why[192];
    int rc = st_taskset_parse(s, strlen(s), ts, why, sizeof why);
    free(ts);
    return rc != 0;
}

/* Copy with one line (0-based) replaced by repl (without newline) or deleted when repl is NULL. */
static char *edit_line(const char *d, size_t n, size_t which, const char *repl, size_t *out_n) {
    char *o = malloc(n + 4096);
    size_t on = 0, line = 0;
    const char *p = d, *e = d + n;
    while (p < e) {
        const char *nl = memchr(p, '\n', (size_t)(e - p));
        size_t ln = (size_t)(nl - p) + 1;
        if (line == which) {
            if (repl) { size_t rl = strlen(repl); memcpy(o + on, repl, rl); on += rl; o[on++] = '\n'; }
        } else { memcpy(o + on, p, ln); on += ln; }
        p = nl + 1; ++line;
    }
    *out_n = on;
    return o;
}

static const char *line_ptr(const char *d, size_t n, size_t which, size_t *len) {
    const char *p = d, *e = d + n;
    for (size_t i = 0; i < which && p < e; ++i) p = memchr(p, '\n', (size_t)(e - p)) + 1;
    const char *nl = memchr(p, '\n', (size_t)(e - p));
    *len = (size_t)(nl - p);
    return p;
}

static void noop_hook(void *ctx, const StEvent *ev) { (void)ctx; (void)ev; }

int main(void) {
    StTaskSet *ts = malloc(sizeof *ts);
    char why[192];
    CHECK(st_taskset_parse(TS, strlen(TS), ts, why, sizeof why) == 0, "taskset parses");

    /* determinism */
    StBuf a = {0}, b = {0};
    CHECK(st_corpus_generate(ts, &a, why, sizeof why) == 0, "generate run 1");
    CHECK(st_corpus_generate(ts, &b, why, sizeof why) == 0, "generate run 2");
    CHECK(a.n == b.n && memcmp(a.p, b.p, a.n) == 0, "two runs byte-identical");
    CHECK(verify(a.p, a.n, ts) == 0, "corpus verifies against its task set");
    CHECK(verify(a.p, a.n, NULL) == 0, "corpus verifies standalone");
    CHECK(strstr(a.p, "prune=budget") != NULL, "budget cutoff recorded");
    CHECK(strstr(a.p, "verdict=solved") != NULL, "solved step recorded");
    CHECK(strstr(a.p, "prune=equiv") != NULL, "equivalence prune recorded");
    CHECK(strstr(a.p, "rrc=0 ") != NULL, "realization cost recorded");

    /* partial: every proper prefix refused */
    int prefix_ok = 1;
    for (size_t k = 0; k < a.n; k += (k < 4096 ? 1 : 97))
        if (verify(a.p, k, ts) == 0) { prefix_ok = 0; printf("prefix %zu accepted\n", k); break; }
    CHECK(prefix_ok, "every truncated prefix refused");
    CHECK(verify(a.p, a.n - 1, ts) != 0, "missing final newline refused");

    /* trailing bytes, CR, NUL */
    {
        char *c = malloc(a.n + 8);
        memcpy(c, a.p, a.n); memcpy(c + a.n, "x\n", 2);
        CHECK(verify(c, a.n + 2, ts) != 0, "bytes after end refused");
        memcpy(c, a.p, a.n); c[10] = '\r';
        CHECK(verify(c, a.n, ts) != 0, "CR refused");
        memcpy(c, a.p, a.n); c[a.n / 2] = '\0';
        CHECK(verify(c, a.n, ts) != 0, "NUL refused");
        free(c);
    }

    /* every single-byte change in the body is refused (digest or grammar) */
    {
        char *c = malloc(a.n);
        int all = 1;
        for (size_t k = 0; k < a.n; k += 13) {
            memcpy(c, a.p, a.n);
            c[k] = (char)(c[k] == '0' ? '1' : (c[k] == '\n' ? ' ' : '0'));
            if (verify(c, a.n, ts) == 0) { all = 0; printf("mutation at %zu accepted\n", k); break; }
        }
        CHECK(all, "single-byte mutations refused");
        free(c);
    }

    /* line-level edits: drop, duplicate-swap, bad enum, reordered fields, uppercase hex */
    {
        size_t n2, ln;
        char *c;
        c = edit_line(a.p, a.n, 6, NULL, &n2);
        CHECK(verify(c, n2, ts) != 0, "dropped step line refused"); free(c);
        const char *l5 = line_ptr(a.p, a.n, 5, &ln);
        char tmp[2048];
        memcpy(tmp, l5, ln); tmp[ln] = '\0';
        char *pr = strstr(tmp, "prune=");
        if (pr) { memcpy(pr, "prune=wrong", 11); }
        c = edit_line(a.p, a.n, 5, tmp, &n2);
        CHECK(verify(c, n2, ts) != 0, "unknown prune value refused"); free(c);
        memcpy(tmp, l5, ln); tmp[ln] = '\0';
        char *ch = strstr(tmp, "child=");
        if (ch && ch[6] != '-') { for (int i = 6; i < 70; ++i) if (ch[i] >= 'a' && ch[i] <= 'f') { ch[i] = (char)(ch[i] - 32); break; } }
        c = edit_line(a.p, a.n, 5, tmp, &n2);
        CHECK(verify(c, n2, ts) != 0, "uppercase hex refused"); free(c);
        c = edit_line(a.p, a.n, 0, "OMEGA-SEARCHTRACE v2", &n2);
        CHECK(verify(c, n2, ts) != 0, "wrong version refused"); free(c);
    }

    /* corpus bound to a different task set */
    {
        StTaskSet *ts2 = malloc(sizeof *ts2);
        char alt[sizeof TS + 16];
        snprintf(alt, sizeof alt, "%s", TS);
        char *x = strstr(alt, "0:3,1:4"); x[2] = '4';
        CHECK(st_taskset_parse(alt, strlen(alt), ts2, why, sizeof why) == 0, "alt taskset parses");
        CHECK(verify(a.p, a.n, ts2) != 0, "corpus for another task set refused");
        free(ts2);
    }

    /* task-set refusals */
    CHECK(ts_refused("OMEGA-SEARCHTRACE-TASKSET v2\ntask a depth=1 max_cost=1 max_candidates=1 dedup=0 pairs=0:0\nend\n"), "taskset wrong version");
    CHECK(ts_refused("OMEGA-SEARCHTRACE-TASKSET v1\ntask a depth=1 max_cost=1 max_candidates=1 dedup=0 pairs=0:0\n"), "taskset without end");
    CHECK(ts_refused("OMEGA-SEARCHTRACE-TASKSET v1\nend\n"), "taskset with no tasks");
    CHECK(ts_refused("OMEGA-SEARCHTRACE-TASKSET v1\ntask a depth=4 max_cost=1 max_candidates=1 dedup=0 pairs=0:0\nend\n"), "taskset depth 4");
    CHECK(ts_refused("OMEGA-SEARCHTRACE-TASKSET v1\ntask a depth=1 max_cost=1 max_candidates=1 dedup=0 pairs=00:0\nend\n"), "taskset leading zero");
    CHECK(ts_refused("OMEGA-SEARCHTRACE-TASKSET v1\ntask a max_cost=1 depth=1 max_candidates=1 dedup=0 pairs=0:0\nend\n"), "taskset reordered fields");
    CHECK(ts_refused("OMEGA-SEARCHTRACE-TASKSET v1\ntask a depth=1 max_cost=1 max_candidates=1 dedup=0 pairs=0:0,\nend\n"), "taskset trailing comma");
    CHECK(ts_refused("OMEGA-SEARCHTRACE-TASKSET v1\ntask a depth=1 max_cost=1 max_candidates=1 dedup=0 pairs=0:0\ntask a depth=1 max_cost=1 max_candidates=1 dedup=0 pairs=0:0\nend\n"), "taskset duplicate name");
    CHECK(ts_refused("OMEGA-SEARCHTRACE-TASKSET v1\ntask a depth=1 max_cost=1 max_candidates=1 dedup=0 pairs=0:0\nend\nextra\n"), "taskset bytes after end");
    CHECK(ts_refused("OMEGA-SEARCHTRACE-TASKSET v1\r\ntask a depth=1 max_cost=1 max_candidates=1 dedup=0 pairs=0:0\nend\n"), "taskset CR");
    CHECK(ts_refused("OMEGA-SEARCHTRACE-TASKSET v1\ntask a depth=1 max_cost=1 max_candidates=1 dedup=0 pairs=0:18446744073709551616\nend\n"), "taskset u64 overflow");

    /* hook API: at most one per site, clear needs the installing ctx */
    int k1, k2;
    CHECK(omega_synth_set_trace_hook(noop_hook, &k1) == 0, "synth hook install");
    CHECK(omega_synth_set_trace_hook(noop_hook, &k2) != 0, "second synth hook refused");
    CHECK(omega_synth_clear_trace_hook(&k2) != 0, "clear with wrong ctx refused");
    StBuf c0 = {0};
    CHECK(st_corpus_generate(ts, &c0, why, sizeof why) != 0, "generate refused while a hook is installed");
    st_buf_free(&c0);
    CHECK(omega_synth_clear_trace_hook(&k1) == 0, "synth hook clear");
    CHECK(omega_realize_set_trace_hook(noop_hook, &k1) == 0, "realize hook install");
    CHECK(st_corpus_generate(ts, &c0, why, sizeof why) != 0, "generate refused while realize hook is installed");
    CHECK(omega_synth_set_trace_hook(noop_hook, &k2) == 0, "synth hook free after refused generate");
    CHECK(omega_synth_clear_trace_hook(&k2) == 0, "synth hook clear 2");
    st_buf_free(&c0);
    CHECK(omega_realize_clear_trace_hook(&k1) == 0, "realize hook clear");
    CHECK(omega_synth_set_trace_hook(NULL, &k1) != 0, "null hook refused");

    /* hooks off == hooks on for search results */
    CHECK(st_hook_equivalence(ts, why, sizeof why) == 0, "hook equivalence on unit set");

    st_buf_free(&a);
    st_buf_free(&b);
    free(ts);
    printf("st_corpus: %d/%d PASS\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
