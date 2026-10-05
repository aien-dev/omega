/* AIEN Prime Drag Race: implementation-side protocol. See prime_race_impl.h and bench/prime_race/README.md. */
#include "prime_race_impl.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef PR_SOURCE_COMMIT
#define PR_SOURCE_COMMIT "unknown"
#endif
#ifndef PR_CC
#define PR_CC "unknown"
#endif
#ifndef PR_CFLAGS
#define PR_CFLAGS "unknown"
#endif

static void set_err(char *err, size_t n, const char *fmt, const char *a) {
    if (err && n) snprintf(err, n, fmt, a ? a : "");
}

static int parse_u64(const char *s, uint64_t *out) {
    char *end = NULL;
    if (!s || s[0] < '0' || s[0] > '9') return -1; /* rejects empty, sign, space */
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return -1;
    *out = (uint64_t)v;
    return 0;
}

static int parse_seconds(const char *s, double *out) {
    char *end = NULL;
    if (!s || !((s[0] >= '0' && s[0] <= '9') || s[0] == '.')) return -1; /* rejects nan/inf/sign */
    errno = 0;
    double v = strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0' || !isfinite(v) || v < 0.0) return -1;
    *out = v;
    return 0;
}

int pr_parse_args(int argc, char **argv, pr_args *a, char *err, size_t errn) {
    a->limit = 1000000;
    a->min_seconds = 5.0;
    a->bitmap_out = NULL;
    a->report_out = NULL;
    a->audit_passes = 0;
    int audit_given = 0;
    for (int i = 1; i < argc; i++) {
        const char *opt = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(opt, "--limit") == 0 || strcmp(opt, "--min-seconds") == 0 || strcmp(opt, "--bitmap-out") == 0 ||
            strcmp(opt, "--report-out") == 0 || strcmp(opt, "--audit-passes") == 0) {
            if (!val) { set_err(err, errn, "missing value for %s", opt); return -1; }
            i++;
            if (strcmp(opt, "--limit") == 0) {
                if (parse_u64(val, &a->limit) != 0) { set_err(err, errn, "invalid --limit '%s'", val); return -1; }
                if (a->limit > PR_MAX_LIMIT) { set_err(err, errn, "--limit '%s' exceeds 2^40", val); return -1; }
            } else if (strcmp(opt, "--min-seconds") == 0) {
                if (parse_seconds(val, &a->min_seconds) != 0) { set_err(err, errn, "invalid --min-seconds '%s'", val); return -1; }
            } else if (strcmp(opt, "--audit-passes") == 0) {
                if (parse_u64(val, &a->audit_passes) != 0 || a->audit_passes == 0) {
                    set_err(err, errn, "invalid --audit-passes '%s' (need integer >= 1)", val);
                    return -1;
                }
                audit_given = 1;
            } else if (strcmp(opt, "--bitmap-out") == 0) {
                if (val[0] == '\0') { set_err(err, errn, "empty %s", opt); return -1; }
                a->bitmap_out = val;
            } else {
                if (val[0] == '\0') { set_err(err, errn, "empty %s", opt); return -1; }
                a->report_out = val;
            }
        } else {
            set_err(err, errn, "unknown argument '%s'", opt);
            return -1;
        }
    }
    if (audit_given && !a->bitmap_out) {
        set_err(err, errn, "%s", "--audit-passes requires --bitmap-out");
        return -1;
    }
    return 0;
}

int pr_format_line(char *buf, size_t n, const pr_impl *impl, uint64_t passes, double elapsed_s, uint64_t threads) {
    int w = snprintf(buf, n, "aien-%s;%llu;%.6f;%llu;algorithm=%s,faithful=%s,bits=%d", impl->name,
                     (unsigned long long)passes, elapsed_s, (unsigned long long)threads, impl->algorithm,
                     impl->faithful, impl->bits);
    if (w < 0 || (size_t)w >= n) return -1;
    return w;
}

int pr_line_allowed(int failed, uint64_t passes, double elapsed_s, double min_seconds) {
    if (failed) return 0;
    if (passes == 0) return 0;
    if (!isfinite(elapsed_s) || elapsed_s < min_seconds) return 0;
    return 1;
}

void pr_json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (*p < 0x20) fprintf(f, "\\u%04x", (unsigned)*p);
            else fputc(*p, f);
        }
    }
    fputc('"', f);
}

int pr_write_report(FILE *f, const pr_report *r) {
    const pr_impl *im = r->impl;
    char label[256];
    snprintf(label, sizeof label, "aien-%s", im->name);
    fputs("{\n  \"schema\": \"aien-prime-race/impl-report/v1\",\n  \"impl\": ", f);
    pr_json_string(f, im->name);
    fputs(",\n  \"label\": ", f);
    pr_json_string(f, label);
    fputs(",\n  \"algorithm\": ", f);
    pr_json_string(f, im->algorithm);
    fputs(",\n  \"faithful\": ", f);
    pr_json_string(f, im->faithful);
    fprintf(f, ",\n  \"bits\": %d,\n  \"threads\": %llu,\n  \"environment\": ", im->bits, (unsigned long long)r->threads);
    pr_json_string(f, im->environment);
    fprintf(f, ",\n  \"limit\": %llu,\n  \"min_seconds\": %.6f,\n  \"mode\": ", (unsigned long long)r->limit, r->min_seconds);
    pr_json_string(f, r->mode);
    fprintf(f, ",\n  \"passes\": %llu,\n  \"elapsed_s\": %.6f,\n  \"status\": ", (unsigned long long)r->passes, r->elapsed_s);
    pr_json_string(f, r->status);
    fputs(",\n  \"error\": ", f);
    if (r->error) pr_json_string(f, r->error); else fputs("null", f);
    fputs(",\n  \"build\": {\"source_commit\": ", f);
    pr_json_string(f, PR_SOURCE_COMMIT);
    fputs(", \"cc\": ", f);
    pr_json_string(f, PR_CC);
    fputs(", \"cflags\": ", f);
    pr_json_string(f, PR_CFLAGS);
    fputs("},\n  \"detail\": ", f);
    if (im->detail_json && r->state) {
        if (im->detail_json(r->state, f) != 0) fputs("null", f);
    } else {
        fputs("{}", f);
    }
    fputs("\n}\n", f);
    fflush(f);
    return ferror(f) ? -1 : 0;
}

static int64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int write_words(FILE *f, const uint64_t *w, size_t n) {
    return (n == 0 || fwrite(w, sizeof(uint64_t), n, f) == n) ? 0 : -1;
}

int pr_main(int argc, char **argv, const pr_impl *impl) {
    pr_args a;
    char err[256] = "";
    if (pr_parse_args(argc, argv, &a, err, sizeof err) != 0) {
        fprintf(stderr, "usage error: %s\n", err);
        fprintf(stderr, "usage: %s [--limit N=1000000] [--min-seconds S=5] [--bitmap-out PATH] "
                        "[--report-out PATH] [--audit-passes K]\n", argc > 0 ? argv[0] : "impl");
        return 64;
    }

    const int audit = a.audit_passes > 0;
    char error[256] = "";
    int failed = 0;
    uint64_t passes = 0;
    double elapsed = 0.0;
    void *st = NULL;
    int have_state = 0;
    uint64_t *words = NULL;
    FILE *bf = NULL;
    const size_t nw = pr_words64(a.limit);

    words = (uint64_t *)calloc(nw ? nw : 1, sizeof(uint64_t));
    if (!words) { failed = 1; snprintf(error, sizeof error, "cannot allocate export buffer"); goto report; }
    if (impl->setup(&st, a.limit) != 0) { failed = 1; snprintf(error, sizeof error, "setup failed"); goto report; }
    have_state = 1;
    if (a.bitmap_out && audit) {
        bf = fopen(a.bitmap_out, "wb");
        if (!bf) { failed = 1; snprintf(error, sizeof error, "cannot open bitmap-out"); goto report; }
    }

    if (audit) {
        for (uint64_t k = 0; k < a.audit_passes; k++) {
            if (impl->pass(st, a.limit) != 0) { failed = 1; snprintf(error, sizeof error, "pass %llu failed", (unsigned long long)k); break; }
            passes++;
            if (impl->export_bitmap(st, a.limit, words) != 0) { failed = 1; snprintf(error, sizeof error, "export failed on pass %llu", (unsigned long long)k); break; }
            if (write_words(bf, words, nw) != 0 || fflush(bf) != 0) { failed = 1; snprintf(error, sizeof error, "bitmap write failed"); break; }
        }
    } else {
        int64_t t0 = now_ns(), t1 = t0;
        if (t0 < 0) { failed = 1; snprintf(error, sizeof error, "clock failure"); goto report; }
        for (;;) {
            if (impl->pass(st, a.limit) != 0) { failed = 1; snprintf(error, sizeof error, "pass %llu failed", (unsigned long long)passes); break; }
            passes++;
            t1 = now_ns();
            if (t1 < 0) { failed = 1; snprintf(error, sizeof error, "clock failure"); break; }
            if ((double)(t1 - t0) / 1e9 >= a.min_seconds) break;
        }
        elapsed = (double)(t1 - t0) / 1e9;
        if (!failed && a.bitmap_out) {
            /* untimed export of the final pass */
            FILE *of = fopen(a.bitmap_out, "wb");
            if (!of) { failed = 1; snprintf(error, sizeof error, "cannot open bitmap-out"); }
            else {
                if (impl->export_bitmap(st, a.limit, words) != 0) { failed = 1; snprintf(error, sizeof error, "export failed"); }
                else if (write_words(of, words, nw) != 0) { failed = 1; snprintf(error, sizeof error, "bitmap write failed"); }
                if (fclose(of) != 0 && !failed) { failed = 1; snprintf(error, sizeof error, "bitmap close failed"); }
            }
        }
    }
    if (bf) {
        if (fclose(bf) != 0 && !failed) { failed = 1; snprintf(error, sizeof error, "bitmap close failed"); }
        bf = NULL;
    }
    if (!failed && !audit && !pr_line_allowed(0, passes, elapsed, a.min_seconds)) {
        failed = 1;
        snprintf(error, sizeof error, "timed run did not satisfy passes>0 and elapsed>=min_seconds");
    }
    if (!failed && audit && passes != a.audit_passes) { failed = 1; snprintf(error, sizeof error, "audit pass count mismatch"); }

report:;
    if (bf) fclose(bf);
    pr_report rep;
    memset(&rep, 0, sizeof rep);
    rep.impl = impl;
    rep.state = have_state ? st : NULL;
    rep.threads = have_state && impl->threads ? impl->threads(st) : 0;
    rep.limit = a.limit;
    rep.min_seconds = a.min_seconds;
    rep.mode = audit ? "audit" : "timed";
    rep.passes = passes;
    rep.elapsed_s = elapsed;
    rep.status = failed ? "EXEC_FAILED" : "OK";
    rep.error = failed ? error : NULL;

    if (a.report_out) {
        FILE *rf = fopen(a.report_out, "w");
        if (!rf || pr_write_report(rf, &rep) != 0 || fclose(rf) != 0) {
            fprintf(stderr, "cannot write report to %s\n", a.report_out);
            if (!failed) { failed = 1; snprintf(error, sizeof error, "report write failed"); }
        }
    }
    uint64_t threads = rep.threads;
    if (have_state && impl->teardown) impl->teardown(st);
    free(words);

    if (failed) {
        fprintf(stderr, "EXEC_FAILED: %s\n", error);
        return 2;
    }
    if (!audit) { /* audit mode makes no timing claims: no upstream line */
        char line[512];
        if (pr_format_line(line, sizeof line, impl, passes, elapsed, threads) < 0) {
            fprintf(stderr, "EXEC_FAILED: line too long\n");
            return 2;
        }
        printf("%s\n", line);
        fflush(stdout);
    }
    return 0;
}
