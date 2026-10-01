#ifndef OMEGA_SEARCHTRACE_CORPUS_H
#define OMEGA_SEARCHTRACE_CORPUS_H
/* M23 G1 search-trace corpus: frozen task set -> deterministic corpus of every
 * search step (parent/child candidate, transform, features, verifier result,
 * realization cost, prune reason). Format: spec/searchtrace/M23_SEARCH_TRACE_CORPUS_V1.md */
#include <stddef.h>
#include <stdint.h>

#define ST_TASKSET_MAGIC "OMEGA-SEARCHTRACE-TASKSET v1"
#define ST_CORPUS_MAGIC  "OMEGA-SEARCHTRACE v1"
#define ST_MAX_TASKS 64
#define ST_MAX_PAIRS 16

typedef struct {
    char name[61];
    uint32_t depth;              /* 1..3 */
    uint32_t max_cost;
    uint32_t max_candidates;     /* 1..100000 */
    uint32_t dedup;              /* 0 or 1 */
    uint32_t n_pairs;            /* 1..16 */
    uint64_t in[ST_MAX_PAIRS];
    uint64_t out[ST_MAX_PAIRS];
} StTask;

typedef struct {
    uint32_t n_tasks;
    StTask tasks[ST_MAX_TASKS];
    uint8_t digest[32];          /* sha256 of the task-set file bytes */
} StTaskSet;

typedef struct {
    char *p;
    size_t n, cap;
    int oom;
} StBuf;

void st_buf_free(StBuf *b);

/* Parse a task-set file image. 0 ok; -1 refused (why). */
int st_taskset_parse(const char *data, size_t len, StTaskSet *ts, char *why, size_t why_len);

/* Run every task of the set with the recorder hooks installed and write the
 * corpus into out (appended to an empty buffer). 0 ok; -1 refused (why). */
int st_corpus_generate(const StTaskSet *ts, StBuf *out, char *why, size_t why_len);

/* Strictly check a corpus image. When ts is non-null the corpus must belong to
 * it (task-set digest, task count, names). On success digest_out (may be null)
 * receives the corpus digest from the end line. 0 ok; -1 refused (why). */
int st_corpus_verify(const char *data, size_t len, const StTaskSet *ts,
                     uint8_t digest_out[32], uint64_t *steps_out, char *why, size_t why_len);

/* Hooks-off equivalence: every task searched without hooks and with the
 * recorder installed must give the same solved flag, solution id and stats.
 * 0 identical; -1 differs or refused (why). */
int st_hook_equivalence(const StTaskSet *ts, char *why, size_t why_len);

void st_hex(const uint8_t *b, size_t n, char *out);

#endif /* OMEGA_SEARCHTRACE_CORPUS_H */
