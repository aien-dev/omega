/*
 * crumbline_learner.c -- Omega as a Crumbline learner.
 *
 * A standalone process that speaks the crumbs learner protocol v1 on
 * stdin/stdout (see crumbs/src/protocol.rs). It links only learner-side code:
 * the Crumb v1 visible reader, Omega's program/realization/verification
 * stack, the synthesis vocabulary, OmegaLibrary and the Crumbline search. It
 * contains no generator, sealed record, held-out set or label of any kind,
 * and everything it can know arrives as protocol frames.
 *
 * Modes:
 *   crumbline-learner                 serve the protocol on stdin/stdout
 *   crumbline-learner --decode FILE   decode a visible crumb file and print its shape
 */
#include "crumbline/cl_crumb.h"
#include "crumbline/cl_program.h"
#include "crumbline/cl_search.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    FT_HELLO = 0x01,
    FT_CRUMB = 0x02,
    FT_VERDICT = 0x03,
    FT_ADMIT = 0x04,
    FT_SHUTDOWN = 0x05,
    FT_READY = 0x81,
    FT_BANK = 0x82,
    FT_SUBMIT = 0x83,
    FT_EVENTS = 0x84,
    FT_DONE = 0x85,
};

#define MAX_FRAME (1u << 24)
#define EVENT_BYTES 20
#define EVENT_BATCH 4096

static int read_all(int fd, uint8_t *p, size_t n) {
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int send_frame(uint8_t type, const uint8_t *payload, size_t len) {
    uint8_t hdr[5] = {type, (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24)};
    if (write_all(1, hdr, 5) != 0) return -1;
    return len ? write_all(1, payload, len) : 0;
}

/* Returns frame type, or -1. *buf is malloc'd (caller frees). */
static int recv_frame(uint8_t **buf, size_t *len) {
    uint8_t hdr[5];
    if (read_all(0, hdr, 5) != 0) return -1;
    uint32_t n = (uint32_t)hdr[1] | (uint32_t)hdr[2] << 8 | (uint32_t)hdr[3] << 16 | (uint32_t)hdr[4] << 24;
    if (n > MAX_FRAME) return -1;
    *buf = malloc(n ? n : 1);
    if (!*buf) return -1;
    if (n && read_all(0, *buf, n) != 0) {
        free(*buf);
        return -1;
    }
    *len = n;
    return hdr[0];
}

/* ---- event batching ---------------------------------------------------- */

typedef struct {
    uint8_t buf[4 + EVENT_BATCH * EVENT_BYTES];
    uint32_t count;
    int failed;
} EventBatch;

static void flush_events(EventBatch *b) {
    if (b->count == 0) return;
    b->buf[0] = (uint8_t)b->count;
    b->buf[1] = (uint8_t)(b->count >> 8);
    b->buf[2] = (uint8_t)(b->count >> 16);
    b->buf[3] = (uint8_t)(b->count >> 24);
    if (send_frame(FT_EVENTS, b->buf, 4 + (size_t)b->count * EVENT_BYTES) != 0) b->failed = 1;
    b->count = 0;
}

static void emit_event(void *ctx, const ClEvent *ev) {
    EventBatch *b = ctx;
    ClWriter w;
    cl_w_init(&w, b->buf + 4 + (size_t)b->count * EVENT_BYTES, EVENT_BYTES);
    cl_w_u8(&w, ev->kind);
    cl_w_u8(&w, ev->prune);
    cl_w_u8(&w, ev->verify);
    cl_w_u8(&w, ev->fit);
    cl_w_u16(&w, ev->op_index);
    cl_w_u32(&w, ev->parent);
    cl_w_u32(&w, ev->child);
    cl_w_u32(&w, ev->exec_cost);
    cl_w_u16(&w, ev->oracle_index);
    b->count++;
    if (b->count == EVENT_BATCH) flush_events(b);
}

/* ---- oracle over the protocol ----------------------------------------- */

static int oracle_submit(void *ctx, uint32_t node, const ClSteps *cand, uint8_t flags, uint8_t hyp, uint8_t *verdict) {
    EventBatch *b = ctx;
    flush_events(b);
    uint8_t buf[16 + 7 + CL_MAX_STEPS * 9];
    ClWriter w;
    cl_w_init(&w, buf, sizeof(buf));
    cl_w_u32(&w, node);
    cl_w_u8(&w, flags);
    cl_w_u8(&w, hyp);
    if (cl_steps_encode_cpg1(cand, &w) != 0) return -1;
    if (send_frame(FT_SUBMIT, buf, w.len) != 0) return -1;
    uint8_t *p = NULL;
    size_t n = 0;
    int t = recv_frame(&p, &n);
    if (t != FT_VERDICT || n != 1) {
        free(p);
        return -1;
    }
    *verdict = p[0];
    free(p);
    return 0;
}

static int send_bank(const ClBank *bank) {
    size_t cap = 4 + bank->count * (5 + 7 + CL_MAX_STEPS * 9);
    uint8_t *buf = malloc(cap);
    if (!buf) return -1;
    ClWriter w;
    cl_w_init(&w, buf, cap);
    cl_w_u32(&w, (uint32_t)bank->count);
    for (size_t i = 0; i < bank->count; ++i) {
        cl_w_u8(&w, bank->ops[i].origin);
        cl_w_u32(&w, bank->ops[i].op_ref);
        cl_steps_encode_cpg1(&bank->ops[i].steps, &w);
    }
    int rc = w.overflow ? -1 : send_frame(FT_BANK, buf, w.len);
    free(buf);
    return rc;
}

static int send_done(const ClSearchResult *r) {
    uint8_t buf[1 + 4 + 12 * 4 + 16];
    ClWriter w;
    cl_w_init(&w, buf, sizeof(buf));
    cl_w_u8(&w, r->outcome);
    cl_w_u32(&w, r->solution_node);
    uint32_t f[12] = {r->generated,      r->evaluated,       r->expansions,     r->pruned_equiv,
                      r->pruned_cost,    r->pruned_cap,      r->visible_fits,   r->verify_failures,
                      r->oracle_queries, r->oracle_rejections, r->candidates_to_solution, r->hypotheses_at_first_submit};
    for (int i = 0; i < 12; ++i) cl_w_u32(&w, f[i]);
    cl_w_u64(&w, r->exec_count);
    cl_w_u64(&w, r->wall_ns);
    return send_frame(FT_DONE, buf, w.len);
}

static int serve(void) {
    ClSearchConfig cfg;
    cl_search_default_config(&cfg);
    bool library_enabled = false;
    ClLearnerLibrary *lib = calloc(1, sizeof(*lib));
    ClCrumb *crumb = calloc(1, sizeof(*crumb));
    ClBank *bank = calloc(1, sizeof(*bank));
    EventBatch *batch = calloc(1, sizeof(*batch));
    if (!lib || !crumb || !bank || !batch || cl_library_init(lib) != 0) return 3;

    for (;;) {
        uint8_t *p = NULL;
        size_t n = 0;
        int t = recv_frame(&p, &n);
        if (t < 0) return 2;
        ClReader r;
        cl_r_init(&r, p, n);
        int rc = 0;
        switch (t) {
            case FT_HELLO: {
                if (cl_r_u16(&r) != 1) {
                    rc = -1;
                    break;
                }
                library_enabled = cl_r_u8(&r) != 0;
                cfg.max_candidates = cl_r_u32(&r);
                cfg.max_depth = cl_r_u32(&r);
                cfg.frontier_cap = cl_r_u32(&r);
                cfg.max_oracle_queries = cl_r_u32(&r);
                cfg.ambiguity_scan = cl_r_u32(&r);
                cfg.robust_min_pct = cl_r_u32(&r);
                if (!cl_r_done(&r)) {
                    rc = -1;
                    break;
                }
                uint8_t ready[2] = {1, 0};
                rc = send_frame(FT_READY, ready, 2);
                break;
            }
            case FT_ADMIT: {
                uint32_t op_ref = cl_r_u32(&r);
                uint8_t scope = cl_r_u8(&r);
                ClSteps steps;
                if (cl_steps_decode_cpg1(&r, &steps) != 0 || !cl_r_done(&r)) {
                    rc = -1;
                    break;
                }
                /* A learner without reuse ignores admissions (control condition). */
                if (library_enabled) cl_library_admit(lib, &steps, scope, op_ref);
                break;
            }
            case FT_CRUMB: {
                ClSearchResult res;
                memset(&res, 0, sizeof(res));
                if (cl_crumb_decode(p, n, crumb) != CL_CRUMB_OK) {
                    res.outcome = CL_OUTCOME_UNSUPPORTED;
                    res.solution_node = UINT32_MAX;
                    rc = send_bank(bank) != 0 || send_done(&res) != 0 ? -1 : 0;
                    break;
                }
                cl_bank_init_base(bank);
                if (library_enabled) cl_library_export(lib, cl_crumb_input_bits(crumb), bank);
                if (send_bank(bank) != 0) {
                    rc = -1;
                    break;
                }
                ClOracle oracle = {oracle_submit, batch};
                ClEventSink sink = {emit_event, batch};
                batch->count = 0;
                if (cl_search_run(crumb, bank, &cfg, &oracle, &sink, &res) != 0) res.outcome = CL_OUTCOME_UNSOLVED;
                flush_events(batch);
                rc = (batch->failed || send_done(&res) != 0) ? -1 : 0;
                break;
            }
            case FT_SHUTDOWN:
                free(p);
                cl_library_destroy(lib);
                return 0;
            default:
                rc = -1;
        }
        free(p);
        if (rc != 0) return 2;
    }
}

static int decode_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    ClCrumb *c = calloc(1, sizeof(*c));
    int rc = cl_crumb_decode(buf, n, c);
    if (rc != CL_CRUMB_OK) {
        printf("decode-error %d\n", rc);
        free(c);
        return 1;
    }
    printf("ok enc=%u in=%ux%u out=%ux%u n=%u", c->encoding, c->in_arity, c->in_lane_bytes, c->out_arity,
           c->out_lane_bytes, c->n);
    if (c->has_budget)
        printf(" budget=%u,%u,%u,%u", c->budget_max_candidates, c->budget_max_depth, c->budget_max_oracle_queries,
               c->budget_max_program_ops);
    for (uint32_t i = 0; i < c->n; ++i) {
        printf(" |");
        for (uint8_t l = 0; l < c->in_arity; ++l) printf(" %llu", (unsigned long long)c->in[i][l]);
        printf(" ->");
        for (uint8_t l = 0; l < c->out_arity; ++l) printf(" %llu", (unsigned long long)c->out[i][l]);
    }
    printf("\n");
    free(c);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--decode") == 0) return decode_file(argv[2]);
    if (argc != 1) return 64;
    return serve();
}
