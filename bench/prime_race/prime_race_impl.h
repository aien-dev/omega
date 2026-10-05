/* AIEN Prime Drag Race: implementation-side protocol (one copy for every C/CUDA implementation).
 * Contract: canonical bitmap, bit k (u64 word k/64, bit k%64) set iff 2k+1 is prime, 0 <= k < (limit+1)/2,
 * tail bits zero; prime 2 is implicit when limit >= 2. */
#ifndef PRIME_RACE_IMPL_H
#define PRIME_RACE_IMPL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct pr_impl {
    const char *name;        /* e.g. "cpu-c-base"; label printed as "aien-<name>" */
    const char *algorithm;   /* base|wheel|other */
    const char *faithful;    /* yes|no */
    int bits;                /* storage bits per flag */
    const char *environment; /* linux-host-cpu | linux-hosted-gb10-native | linux-hosted-gb10-cuda */
    int  (*setup)(void **state, uint64_t limit);          /* untimed one-time setup, 0 = ok */
    int  (*pass)(void *state, uint64_t limit);            /* timed: fresh sieve from scratch, complete, host-readable; 0 = ok */
    int  (*export_bitmap)(void *state, uint64_t limit, uint64_t *out); /* canonical words of last pass, 0 = ok */
    uint64_t (*threads)(void *state);                     /* threads that ran the sieve in last pass */
    int  (*detail_json)(void *state, FILE *f);            /* optional (may be NULL): writes one JSON object */
    void (*teardown)(void *state);
} pr_impl;

static inline uint64_t pr_odd_count(uint64_t limit) { return (limit + 1) / 2; }
static inline size_t pr_words64(uint64_t limit) { return (size_t)((pr_odd_count(limit) + 63) / 64); }

#define PR_MAX_LIMIT (1ULL << 40)

/* Full CLI: <impl> [--limit N=1000000] [--min-seconds S=5] [--bitmap-out PATH] [--report-out PATH]
 * [--audit-passes K]. Exit 0 ok, 2 execution failure (no stdout line), 64 usage error. */
int pr_main(int argc, char **argv, const pr_impl *impl);

/* ---- hooks exposed for host tests (pure functions, no process state) ---- */
typedef struct pr_args {
    uint64_t limit;
    double min_seconds;
    const char *bitmap_out;
    const char *report_out;
    uint64_t audit_passes; /* 0 = timed mode */
} pr_args;

/* 0 = ok; -1 = usage error with a message in err. */
int pr_parse_args(int argc, char **argv, pr_args *a, char *err, size_t errn);

/* Upstream line "aien-<name>;<passes>;<elapsed %.6f>;<threads>;algorithm=..,faithful=..,bits=..".
 * Returns length written, or -1 if it does not fit. */
int pr_format_line(char *buf, size_t n, const pr_impl *impl, uint64_t passes, double elapsed_s, uint64_t threads);

/* 1 only if the stdout line may be printed: no failure, passes > 0, elapsed >= min_seconds. */
int pr_line_allowed(int failed, uint64_t passes, double elapsed_s, double min_seconds);

/* JSON string literal with hand-written escaping (quotes included). */
void pr_json_string(FILE *f, const char *s);

typedef struct pr_report {
    const pr_impl *impl;
    void *state;            /* for threads()/detail_json(); may be NULL */
    uint64_t threads;
    uint64_t limit;
    double min_seconds;
    const char *mode;       /* "timed" | "audit" */
    uint64_t passes;
    double elapsed_s;
    const char *status;     /* "OK" | "EXEC_FAILED" */
    const char *error;      /* NULL when ok */
} pr_report;

/* Writes the "aien-prime-race/impl-report/v1" JSON object. Returns 0 if the stream had no error. */
int pr_write_report(FILE *f, const pr_report *r);

#endif
