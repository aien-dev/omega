/*
 * rx_omega.c -- Omega as a resident realization faculty (ADR 0016 §43, R10).
 *
 * See rx_omega.h for the object and reaction map. Every function named fn_*
 * is a reaction body: it reads its versioned snapshot and proposes
 * mutations. None of them calls another faculty or waits for one.
 *
 * Side effects outside the world are limited to the realization store, which
 * is content-addressed and append-only. Writing the same identity twice with
 * the same bytes is a no-op; the same identity with different bytes is
 * refused. So a reaction that is invalidated and runs again cannot leave the
 * store in a different state.
 */
#include "rx_omega.h"

#include "aarch64_encoder.h"
#include "aarch64_target.h"
#include "omega_realize.h"
#include "omega_verify.h"
#include "sha256.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef void (*MatVecFn)(const uint64_t *A, const uint64_t *x, uint64_t *y, uint64_t M, uint64_t N);

#define KIND_DEFECT (OMEGA_MATVEC_KIND_QUAD4 + 1u)
#define PAGE        4096u

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t id_word(const SemanticId *id, uint32_t i) {
    uint64_t v = 0;
    for (uint32_t b = 0; b < 8; b++) v |= (uint64_t)id->bytes[i * 8 + b] << (8 * b);
    return v;
}

static void id_from_words(SemanticId *id, const uint64_t w[4]) {
    for (uint32_t i = 0; i < 4; i++)
        for (uint32_t b = 0; b < 8; b++) id->bytes[i * 8 + b] = (uint8_t)(w[i] >> (8 * b));
}

static const RxSnapshotDep *in_of(const RxCtx *c, RxObjRef r) {
    for (uint32_t i = 0; i < c->n_in; i++)
        if (c->in[i].obj.id == r.id) return &c->in[i];
    return NULL;
}

static void put(RxCtx *c, RxObjRef o, uint32_t field, uint64_t v) {
    c->out[c->n_out++] = (RxMutation){ o, field, v };
}

/* ---- data shared by production, verification and measurement ---- */

static uint64_t mix(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

void rx_omega_fill(uint64_t seed, uint64_t *A, uint64_t *x, uint32_t M, uint32_t N) {
    uint64_t s = seed;
    for (size_t i = 0; i < (size_t)M * N; i++) A[i] = mix(&s);
    for (uint32_t j = 0; j < N; j++) x[j] = mix(&s);
}

uint64_t rx_omega_digest(const uint64_t *y, uint32_t M) {
    sha256_ctx c;
    sha256_init(&c);
    uint8_t b[8];
    for (uint32_t i = 0; i < M; i++) {
        for (int k = 0; k < 8; k++) b[k] = (uint8_t)(y[i] >> (8 * k));
        sha256_update(&c, b, 8);
    }
    uint8_t d[32];
    sha256_final(&c, d);
    uint64_t v = 0;
    for (int k = 0; k < 8; k++) v |= (uint64_t)d[k] << (8 * k);
    return v;
}

static int shape_ok(uint64_t M, uint64_t N) {
    return M >= 1 && N >= 1 && M <= RX_OMEGA_MAX_ELEMS && N <= RX_OMEGA_MAX_ELEMS &&
           M * N <= RX_OMEGA_MAX_ELEMS;
}

/* ---- realization store ---- */

static void real_object(const RxOmegaFaculty *f, const uint8_t *code, size_t len,
                        RealizationObject *r) {
    memset(r, 0, sizeof(*r));
    r->target_profile = f->machine.target_profile;
    r->entry_offset = 0;
    r->semantic_id = f->spec.spec_id;
    r->machine_id = f->machine.machine_id;
    r->has_machine_id = true;
    r->code_len = len;
    memcpy(r->code_bytes, code, len);
}

static int ids_equal(const SemanticId *a, const SemanticId *b) {
    return memcmp(a->bytes, b->bytes, sizeof(a->bytes)) == 0;
}

/* Caller holds f->mu. */
static RxOmegaRealization *store_find_locked(RxOmegaFaculty *f, const SemanticId *id) {
    for (uint32_t i = 0; i < f->n_store; i++)
        if (ids_equal(&f->store[i].id, id)) return &f->store[i];
    return NULL;
}

/* Record bytes under an identity. The identity is what the synthesizer
 * claims; only the verifier decides whether the bytes live up to it. */
static int store_put(RxOmegaFaculty *f, const SemanticId *id, uint32_t kind,
                     const uint8_t *code, size_t len) {
    if (len == 0 || len > AARCH64_MAX_CODE_BYTES || len > PAGE) return -1;
    pthread_mutex_lock(&f->mu);
    RxOmegaRealization *e = store_find_locked(f, id);
    int rc = 0;
    if (e) {
        if (e->code_len != len || memcmp(e->code, code, len) != 0) rc = -1;
        goto out;
    }
    if (f->n_store >= RX_OMEGA_STORE) { rc = -1; goto out; }
    void *page = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (page == MAP_FAILED) { rc = -1; goto out; }
    memcpy(page, code, len);
    __builtin___clear_cache((char *)page, (char *)page + len);
    if (mprotect(page, PAGE, PROT_READ | PROT_EXEC) != 0) {
        munmap(page, PAGE);
        rc = -1;
        goto out;
    }
    e = &f->store[f->n_store++];
    memset(e, 0, sizeof(*e));
    e->id = *id;
    e->kind = kind;
    e->code_len = len;
    memcpy(e->code, code, len);
    e->page = page;
out:
    pthread_mutex_unlock(&f->mu);
    return rc;
}

int rx_omega_store_find(RxOmegaFaculty *f, uint64_t w0, RxOmegaRealization *out) {
    pthread_mutex_lock(&f->mu);
    int rc = -1;
    for (uint32_t i = 0; i < f->n_store; i++)
        if (id_word(&f->store[i].id, 0) == w0) {
            *out = f->store[i];
            rc = 0;
            break;
        }
    pthread_mutex_unlock(&f->mu);
    return rc;
}

/* A realization the parent may execute: verified, and named by all 32 bytes. */
static MatVecFn verified_fn(RxOmegaFaculty *f, const SemanticId *id) {
    pthread_mutex_lock(&f->mu);
    RxOmegaRealization *e = store_find_locked(f, id);
    MatVecFn fn = NULL;
    if (e && e->verified) {
        union { void *p; MatVecFn fn; } u;
        u.p = e->page;
        fn = u.fn;
        e->parent_runs++;
    }
    pthread_mutex_unlock(&f->mu);
    return fn;
}

/* ---- workload.serve: the production path ---- */

static int fn_serve(RxCtx *c) {
    RxOmegaFaculty *f = c->user;
    const RxSnapshotDep *req = in_of(c, f->o.request);
    const RxSnapshotDep *sel = in_of(c, f->serve_from_record ? f->serve_record : f->o.selection);
    const RxSnapshotDep *dem = in_of(c, f->o.demand);
    if (!req || !sel || !dem) return -1;
    uint64_t seq = req->field[0], M = req->field[1], N = req->field[2];
    if (seq == 0) return 0;
    if (!shape_ok(M, N)) return -1;

    uint64_t *A = malloc((size_t)M * N * sizeof(uint64_t));
    uint64_t *x = malloc((size_t)N * sizeof(uint64_t));
    uint64_t *y = malloc((size_t)M * sizeof(uint64_t));
    if (!A || !x || !y) { free(A); free(x); free(y); return -1; }
    rx_omega_fill(req->field[3], A, x, (uint32_t)M, (uint32_t)N);

    /* Whatever the world currently records for this regime. Never waits. */
    MatVecFn fn = NULL;
    uint64_t used = 0;
    if (sel->field[0] != 0 && sel->field[6] == rx_omega_regime(M, N) && sel->field[1] != 0) {
        SemanticId id;
        id_from_words(&id, &sel->field[1]);
        fn = verified_fn(f, &id);
        if (fn) used = sel->field[1];
    }
    uint64_t t0 = now_ns();
    if (fn) fn(A, x, y, M, N);
    else omega_matvec_reference(A, x, y, (uint32_t)M, (uint32_t)N);
    uint64_t ns = now_ns() - t0;
    uint64_t digest = rx_omega_digest(y, (uint32_t)M);
    free(A); free(x); free(y);

    pthread_mutex_lock(&f->mu);
    if (fn) f->served_realized++; else f->served_reference++;
    pthread_mutex_unlock(&f->mu);

    put(c, f->o.result, 0, seq);
    put(c, f->o.result, 1, digest);
    put(c, f->o.result, 2, used);
    put(c, f->o.result, 3, ns);

    /* Cost evidence. A new regime starts its own count. The window field
     * moves only every RX_OMEGA_WINDOW calls; that is what wakes omega.watch. */
    int same = dem->field[4] == M && dem->field[5] == N;
    uint64_t calls = same ? dem->field[0] + 1 : 1;
    uint64_t spent = same ? dem->field[1] + ns : ns;
    put(c, f->o.demand, 0, calls);
    put(c, f->o.demand, 1, spent);
    if (calls % RX_OMEGA_WINDOW == 0) {
        put(c, f->o.demand, 2, dem->field[2] + 1);
        put(c, f->o.demand, 3, spent / calls);
    }
    if (!same) {
        put(c, f->o.demand, 4, M);
        put(c, f->o.demand, 5, N);
    }
    put(c, f->o.demand, 6, used);
    put(c, f->o.demand, 7, dem->field[7] + 1);
    return 0;
}

/* ---- omega.watch: cost evidence crosses the threshold ---- */

static int fn_watch(RxCtx *c) {
    RxOmegaFaculty *f = c->user;
    const RxSnapshotDep *dem = in_of(c, f->o.demand);
    const RxSnapshotDep *s = in_of(c, f->o.search);
    if (!dem || !s) return -1;
    uint64_t M = dem->field[4], N = dem->field[5];
    if (M == 0 || N == 0) return 0;
    if (s->field[0] != 0 && s->field[1] == M && s->field[2] == N) return 0; /* already searched */
    if (dem->field[0] < f->cfg.hot_calls || dem->field[1] < f->cfg.hot_ns) return 0; /* not hot */
    put(c, f->o.search, 0, s->field[0] + 1);
    put(c, f->o.search, 1, M);
    put(c, f->o.search, 2, N);
    put(c, f->o.search, 3, dem->field[3]);
    put(c, f->o.search, 4, dem->field[0]);
    put(c, f->o.search, 5, dem->field[1]);
    return 0;
}

/* ---- omega.synthesize.k: Omega emits one candidate ---- */

static int patch_word(uint8_t *code, size_t len, uint32_t word, const uint8_t insn[4]) {
    if ((size_t)(word + 1) * 4u > len) return -1;
    memcpy(code + (size_t)word * 4u, insn, 4);
    return 0;
}

static int encode_one(uint8_t out[4], int which, int32_t imm) {
    size_t pos = 0;
    switch (which) {
    case 0: return aarch64_emit_cbz(out, &pos, 4, true, REG_XZR, imm);           /* always taken */
    case 1: return aarch64_emit_movz(out, &pos, 4, true, REG_X16, 0, 0);          /* x16 = 0 */
    case 2: return aarch64_emit_ldr_x_post(out, &pos, 4, REG_X10, REG_X16, 8);    /* load [0] */
    }
    return -1;
}

/* Omega's unroll4_dual with one test fault applied. Word numbers are from
 * the emitter's own layout in omega_matvec.c. */
static int defect_bytes(RxOmegaFaculty *f, RxOmegaDefect d, MatVecRealization *base,
                        SemanticId *claimed, uint8_t *code, size_t *len) {
    memcpy(code, base->realization.code_bytes, base->realization.code_len);
    *len = base->realization.code_len;
    uint8_t insn[4];
    /* Word 31 is "cbz x15, row_end": skipping it always drops N % 4. */
    if (encode_one(insn, 0, 8) != 0 || patch_word(code, *len, 31, insn) != 0) return -1;
    RealizationObject r;
    real_object(f, code, *len, &r);
    if (omega_realize_compute_triple_id(&f->spec.spec_id, &f->machine.machine_id, &r, claimed) != 0)
        return -1;
    if (d == RX_OMEGA_DEFECT_SKIP_REMAINDER) return 0;
    if (d == RX_OMEGA_DEFECT_TAMPER) {
        /* Keep the identity of the bytes above; store different bytes. */
        uint8_t mov[4];
        if (encode_one(mov, 1, 0) != 0 || patch_word(code, *len, 2, mov) != 0) return -1;
        return 0;
    }
    if (d == RX_OMEGA_DEFECT_CRASH) {
        uint8_t mov[4], ld[4];
        if (encode_one(mov, 1, 0) != 0 || encode_one(ld, 2, 0) != 0 ||
            patch_word(code, *len, 0, mov) != 0 || patch_word(code, *len, 1, ld) != 0)
            return -1;
        real_object(f, code, *len, &r);
        return omega_realize_compute_triple_id(&f->spec.spec_id, &f->machine.machine_id, &r,
                                               claimed);
    }
    return -1;
}

static int fn_synthesize(RxCtx *c) {
    struct { RxOmegaFaculty *f; uint32_t k; } *sl = c->user;
    RxOmegaFaculty *f = sl->f;
    uint32_t k = sl->k;
    const RxSnapshotDep *s = in_of(c, f->o.search);
    const RxSnapshotDep *cand = in_of(c, f->o.candidate[k]);
    if (!s || !cand) return -1;
    uint64_t epoch = s->field[0];
    if (epoch == 0 || cand->field[0] == epoch) return 0;

    MatVecLivingKernel kernel;
    memset(&kernel, 0, sizeof(kernel));
    kernel.spec = f->spec;
    kernel.machine = &f->machine;
    MatVecRealization mv;
    uint32_t kind = k;
    int rc;
    if (k < MATVEC_REALIZATION_COUNT)
        rc = omega_matvec_synthesize(&kernel, (MatVecRealizationKind)k, &mv);
    else if (k == OMEGA_MATVEC_KIND_QUAD4)
        rc = omega_matvec_synthesize_quad4(&f->spec, &f->machine, &mv);
    else
        rc = omega_matvec_synthesize(&kernel, MATVEC_REALIZATION_UNROLL4_DUAL, &mv);
    if (rc != 0) return -1;

    SemanticId id = mv.realization_id;
    uint8_t code[AARCH64_MAX_CODE_BYTES];
    size_t len = mv.realization.code_len;
    memcpy(code, mv.realization.code_bytes, len);
    if (kind == KIND_DEFECT && defect_bytes(f, f->cfg.defect, &mv, &id, code, &len) != 0)
        return -1;
    if (store_put(f, &id, kind, code, len) != 0) return -1;

    put(c, f->o.candidate[k], 0, epoch);
    put(c, f->o.candidate[k], 1, kind);
    for (uint32_t i = 0; i < 4; i++) put(c, f->o.candidate[k], 2 + i, id_word(&id, i));
    put(c, f->o.candidate[k], 6, len);
    put(c, f->o.candidate[k], 7, RX_OMEGA_SYNTHESIZED);
    return 0;
}

/* ---- omega.verify.k: V0 structure, then differential in a sandbox ---- */

typedef struct {
    uint32_t checks;
    uint32_t fails;
    uint32_t bounds;
    uint32_t done;
} SandboxReport;

#define CANARY      0xC0FFEE0DDBA11AD5ull
#define GUARD_WORDS 8u
#define VMAX_ELEMS  (64u * 257u)

typedef struct {
    uint64_t *A, *A0, *x, *x0, *y, *yref;
} Buffers;

static int buffers_alloc(Buffers *b) {
    memset(b, 0, sizeof(*b));
    b->A = malloc(VMAX_ELEMS * sizeof(uint64_t));
    b->A0 = malloc(VMAX_ELEMS * sizeof(uint64_t));
    b->x = malloc(512 * sizeof(uint64_t));
    b->x0 = malloc(512 * sizeof(uint64_t));
    b->y = malloc((512 + GUARD_WORDS) * sizeof(uint64_t));
    b->yref = malloc(512 * sizeof(uint64_t));
    return b->A && b->A0 && b->x && b->x0 && b->y && b->yref ? 0 : -1;
}

static void buffers_free(Buffers *b) {
    free(b->A); free(b->A0); free(b->x); free(b->x0); free(b->y); free(b->yref);
}

/* Edge values that stress wrap-around and the zero/one identities. */
static void fill_edge(uint64_t seed, uint64_t *A, uint64_t *x, uint32_t M, uint32_t N) {
    static const uint64_t edge[] = { 0, 1, 2, UINT64_MAX, UINT64_MAX - 1, 1ull << 63,
                                     (1ull << 32) - 1, 1ull << 32 };
    uint64_t s = seed;
    for (size_t i = 0; i < (size_t)M * N; i++) A[i] = edge[mix(&s) % 8];
    for (uint32_t j = 0; j < N; j++) x[j] = edge[mix(&s) % 8];
}

/* Runs in the child only. Never returns to the caller's stack frame. */
static void sandbox_body(MatVecFn fn, Buffers *b, uint64_t regime_m, uint64_t regime_n,
                         uint64_t seed, int wfd) {
    static const uint32_t Ms[] = { 1, 2, 3, 4, 5, 7, 16 };
    static const uint32_t Ns[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 13, 31, 64 };
    SandboxReport rep = { 0, 0, 0, 0 };
    uint32_t shapes[128][2];
    uint32_t n = 0;
    for (uint32_t i = 0; i < sizeof Ms / sizeof Ms[0]; i++)
        for (uint32_t j = 0; j < sizeof Ns / sizeof Ns[0]; j++) {
            shapes[n][0] = Ms[i];
            shapes[n][1] = Ns[j];
            n++;
        }
    shapes[n][0] = 64; shapes[n][1] = 256; n++;
    shapes[n][0] = 33; shapes[n][1] = 257; n++;
    if (regime_m && regime_n && regime_m * regime_n <= VMAX_ELEMS && regime_m <= 512 &&
        regime_n <= 512) {
        shapes[n][0] = (uint32_t)regime_m;
        shapes[n][1] = (uint32_t)regime_n;
        n++;
    }
    uint64_t s = seed;
    for (uint32_t t = 0; t < n; t++) {
        uint32_t M = shapes[t][0], N = shapes[t][1];
        for (int variant = 0; variant < 3; variant++) {
            uint64_t cs = mix(&s);
            if (variant == 1) fill_edge(cs, b->A, b->x, M, N);
            else rx_omega_fill(cs, b->A, b->x, M, N);
            memcpy(b->A0, b->A, (size_t)M * N * sizeof(uint64_t));
            memcpy(b->x0, b->x, (size_t)N * sizeof(uint64_t));
            for (uint32_t i = 0; i < M + GUARD_WORDS; i++) b->y[i] = CANARY;
            omega_matvec_reference(b->A0, b->x0, b->yref, M, N);
            fn(b->A, b->x, b->y, M, N);
            rep.checks++;
            if (memcmp(b->y, b->yref, (size_t)M * sizeof(uint64_t)) != 0) rep.fails++;
            int oob = 0;
            for (uint32_t i = M; i < M + GUARD_WORDS; i++) oob |= b->y[i] != CANARY;
            oob |= memcmp(b->A, b->A0, (size_t)M * N * sizeof(uint64_t)) != 0;
            oob |= memcmp(b->x, b->x0, (size_t)N * sizeof(uint64_t)) != 0;
            rep.checks++;
            if (oob) { rep.fails++; rep.bounds++; }
        }
    }
    rep.done = 1;
    ssize_t wr = write(wfd, &rep, sizeof rep);
    _exit(wr == (ssize_t)sizeof rep ? 0 : 3);
}

static int sandbox_run(RxOmegaFaculty *f, void *page, uint64_t M, uint64_t N, uint64_t seed,
                       SandboxReport *rep, uint32_t *why) {
    Buffers b;
    if (buffers_alloc(&b) != 0) { buffers_free(&b); return -1; }
    int fds[2];
    if (pipe(fds) != 0) { buffers_free(&b); return -1; }
    union { void *p; MatVecFn fn; } u;
    u.p = page;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        buffers_free(&b);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        signal(SIGPIPE, SIG_DFL);
        sandbox_body(u.fn, &b, M, N, seed, fds[1]);
    }
    close(fds[1]);
    pthread_mutex_lock(&f->mu);
    f->sandbox_runs++;
    pthread_mutex_unlock(&f->mu);

    memset(rep, 0, sizeof(*rep));
    size_t got = 0;
    uint64_t deadline = now_ns() + (uint64_t)f->cfg.sandbox_ms * 1000000ull;
    int hung = 0;
    while (got < sizeof(*rep)) {
        uint64_t t = now_ns();
        if (t >= deadline) { hung = 1; break; }
        struct pollfd p = { fds[0], POLLIN, 0 };
        int pr = poll(&p, 1, (int)((deadline - t) / 1000000ull) + 1);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) continue;
        ssize_t r = read(fds[0], (uint8_t *)rep + got, sizeof(*rep) - got);
        if (r <= 0) break;  /* child closed: crashed or exited early */
        got += (size_t)r;
    }
    close(fds[0]);
    if (hung) kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    buffers_free(&b);

    if (hung) { *why = RX_OMEGA_WHY_HUNG; return 1; }
    if (WIFSIGNALED(status) || got != sizeof(*rep) || !rep->done ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        pthread_mutex_lock(&f->mu);
        f->sandbox_crashes++;
        pthread_mutex_unlock(&f->mu);
        *why = RX_OMEGA_WHY_CRASHED;
        return 1;
    }
    if (rep->bounds) { *why = RX_OMEGA_WHY_BOUNDS; return 1; }
    if (rep->fails) { *why = RX_OMEGA_WHY_DIFFERENTIAL; return 1; }
    *why = RX_OMEGA_WHY_NONE;
    return 0;
}

/* Identity, V0 structure, then the sandboxed differential on shape M x N.
 * On a pass the store entry is marked verified for exactly the bytes that
 * were checked. Returns the reason (RX_OMEGA_WHY_NONE on a pass), or -1 when
 * the check itself could not run. */
static int verify_stored(RxOmegaFaculty *f, const SemanticId *id, uint64_t M, uint64_t N,
                         uint32_t *out_checks, uint32_t *out_fails) {
    RxOmegaRealization e;
    int have = 0;
    pthread_mutex_lock(&f->mu);
    RxOmegaRealization *p = store_find_locked(f, id);
    if (p) { e = *p; have = 1; }
    pthread_mutex_unlock(&f->mu);

    uint32_t why = RX_OMEGA_WHY_NONE, checks = 0, fails = 0;
    if (!have) why = RX_OMEGA_WHY_UNKNOWN;

    RealizationObject r;
    if (why == RX_OMEGA_WHY_NONE) {
        /* The bytes must be the ones the candidate names. */
        SemanticId again;
        real_object(f, e.code, e.code_len, &r);
        checks++;
        if (omega_realize_compute_triple_id(&f->spec.spec_id, &f->machine.machine_id, &r,
                                            &again) != 0 || !ids_equal(&again, id)) {
            fails++;
            why = RX_OMEGA_WHY_IDENTITY;
        }
    }
    if (why == RX_OMEGA_WHY_NONE) {
        VerifyReport rep;
        memset(&rep, 0, sizeof(rep));
        r.realization_id = *id;
        r.has_id = true;
        int v0 = omega_verify_v0_structural(NULL, &r, &rep);
        checks += rep.check_count;
        fails += rep.fail_count;
        if (v0 != 0 || !rep.passed) why = RX_OMEGA_WHY_STRUCTURE;
    }
    if (why == RX_OMEGA_WHY_NONE) {
        SandboxReport sb;
        int rc = sandbox_run(f, e.page, M, N, id_word(id, 0), &sb, &why);
        if (rc < 0) return -1;
        checks += sb.checks;
        fails += sb.fails;
        if (rc > 0 && why == RX_OMEGA_WHY_NONE) why = RX_OMEGA_WHY_CRASHED;
    }
    if (why == RX_OMEGA_WHY_NONE) {
        pthread_mutex_lock(&f->mu);
        RxOmegaRealization *q = store_find_locked(f, id);
        if (q && q->code_len == e.code_len && memcmp(q->code, e.code, e.code_len) == 0)
            q->verified = 1;
        else why = RX_OMEGA_WHY_IDENTITY;
        pthread_mutex_unlock(&f->mu);
    }
    *out_checks = checks;
    *out_fails = fails;
    return (int)why;
}

static int fn_verify(RxCtx *c) {
    struct { RxOmegaFaculty *f; uint32_t k; } *sl = c->user;
    RxOmegaFaculty *f = sl->f;
    uint32_t k = sl->k;
    const RxSnapshotDep *cand = in_of(c, f->o.candidate[k]);
    const RxSnapshotDep *v = in_of(c, f->o.verdict[k]);
    const RxSnapshotDep *s = in_of(c, f->o.search);
    if (!cand || !v || !s) return -1;
    uint64_t epoch = cand->field[0];
    if (cand->field[7] != RX_OMEGA_SYNTHESIZED || epoch == 0 || v->field[0] == epoch) return 0;

    SemanticId id;
    id_from_words(&id, &cand->field[2]);
    uint32_t checks = 0, fails = 0;
    int why = verify_stored(f, &id, s->field[1], s->field[2], &checks, &fails);
    if (why < 0) return -1;

    put(c, f->o.verdict[k], 0, epoch);
    put(c, f->o.verdict[k], 1, why == RX_OMEGA_WHY_NONE ? RX_OMEGA_PASSED : RX_OMEGA_REFUSED);
    put(c, f->o.verdict[k], 2, checks);
    put(c, f->o.verdict[k], 3, fails);
    put(c, f->o.verdict[k], 4, id_word(&id, 0));
    put(c, f->o.verdict[k], 5, (uint64_t)why);
    return 0;
}

/* ---- omega.measure.k: benchmark a verified candidate against the incumbent ---- */

#define ROUNDS 7u

static uint64_t time_calls(MatVecFn fn, const uint64_t *A, const uint64_t *x, uint64_t *y,
                           uint32_t M, uint32_t N, uint32_t iters) {
    uint64_t t0 = now_ns();
    for (uint32_t i = 0; i < iters; i++) {
        if (fn) fn(A, x, y, M, N);
        else omega_matvec_reference(A, x, y, M, N);
    }
    return now_ns() - t0;
}

static int fn_measure(RxCtx *c) {
    struct { RxOmegaFaculty *f; uint32_t k; } *sl = c->user;
    RxOmegaFaculty *f = sl->f;
    uint32_t k = sl->k;
    const RxSnapshotDep *v = in_of(c, f->o.verdict[k]);
    const RxSnapshotDep *cand = in_of(c, f->o.candidate[k]);
    const RxSnapshotDep *s = in_of(c, f->o.search);
    const RxSnapshotDep *m = in_of(c, f->o.measure[k]);
    if (!v || !cand || !s || !m) return -1;
    uint64_t epoch = v->field[0];
    if (epoch == 0 || epoch != cand->field[0] || epoch != s->field[0] || m->field[0] == epoch)
        return 0;

    SemanticId id;
    id_from_words(&id, &cand->field[2]);
    uint64_t cps = 0, rps = 0, state = RX_OMEGA_DECLINED;
    MatVecFn fn = v->field[1] == RX_OMEGA_PASSED ? verified_fn(f, &id) : NULL;
    uint32_t M = (uint32_t)s->field[1], N = (uint32_t)s->field[2];
    if (fn && shape_ok(M, N)) {
        uint64_t *A = malloc((size_t)M * N * sizeof(uint64_t));
        uint64_t *x = malloc((size_t)N * sizeof(uint64_t));
        uint64_t *y = malloc((size_t)M * sizeof(uint64_t));
        uint64_t *yr = malloc((size_t)M * sizeof(uint64_t));
        if (!A || !x || !y || !yr) { free(A); free(x); free(y); free(yr); return -1; }
        rx_omega_fill(epoch * 0x9e37u + k, A, x, M, N);
        fn(A, x, y, M, N);
        omega_matvec_reference(A, x, yr, M, N);
        if (memcmp(y, yr, (size_t)M * sizeof(uint64_t)) == 0) {
            /* Size one round to about 200 microseconds of the reference. */
            uint64_t one = time_calls(NULL, A, x, yr, M, N, 1);
            uint32_t iters = one ? (uint32_t)(200000u / one) : 1000u;
            if (iters < 4) iters = 4;
            if (iters > 20000) iters = 20000;
            uint64_t best_c = UINT64_MAX, best_r = UINT64_MAX;
            for (uint32_t r = 0; r < ROUNDS; r++) {
                uint64_t tc = time_calls(fn, A, x, y, M, N, iters);
                uint64_t tr = time_calls(NULL, A, x, yr, M, N, iters);
                if (tc < best_c) best_c = tc;
                if (tr < best_r) best_r = tr;
            }
            cps = best_c * 1000u / iters;
            rps = best_r * 1000u / iters;
            state = RX_OMEGA_MEASURED;
            pthread_mutex_lock(&f->mu);
            RxOmegaRealization *e = store_find_locked(f, &id);
            if (e) e->parent_runs += (uint64_t)iters * ROUNDS;
            pthread_mutex_unlock(&f->mu);
        }
        free(A); free(x); free(y); free(yr);
    }
    put(c, f->o.measure[k], 0, epoch);
    put(c, f->o.measure[k], 1, state);
    put(c, f->o.measure[k], 2, cps);
    put(c, f->o.measure[k], 3, rps);
    for (uint32_t i = 0; i < 4; i++) put(c, f->o.measure[k], 4 + i, id_word(&id, i));
    return 0;
}

/* ---- omega.select: record the fastest verified realization ---- */

static int fn_select(RxCtx *c) {
    RxOmegaFaculty *f = c->user;
    const RxSnapshotDep *s = in_of(c, f->o.search);
    const RxSnapshotDep *sel = in_of(c, f->o.selection);
    if (!s || !sel) return -1;
    uint64_t epoch = s->field[0];
    if (epoch == 0 || sel->field[0] == epoch) return 0;
    if (f->require_experiment_evidence && epoch > 1) {
        const RxSnapshotDep *e = in_of(c, f->experiment_evidence);
        if (!e || e->field[0] != epoch || e->field[2] != 1) return 0;
    }
    const RxSnapshotDep *best = NULL;
    uint64_t ref_ps = 0;
    for (uint32_t k = 0; k < f->cfg.n_slots; k++) {
        const RxSnapshotDep *m = in_of(c, f->o.measure[k]);
        if (!m) return -1;
        if (m->field[0] != epoch) return 0;  /* not every candidate has reported */
        if (m->field[1] != RX_OMEGA_MEASURED) continue;
        if (!ref_ps || m->field[3] < ref_ps) ref_ps = m->field[3];
        /* Beat the incumbent measured in the same rounds by the margin. */
        if (m->field[2] * 100u > m->field[3] * (100u - f->cfg.margin_pct)) continue;
        if (!best || m->field[2] < best->field[2]) best = m;
    }
    put(c, f->o.selection, 0, epoch);
    for (uint32_t i = 0; i < 4; i++) put(c, f->o.selection, 1 + i, best ? best->field[4 + i] : 0);
    put(c, f->o.selection, 5, best ? best->field[2] : ref_ps);
    put(c, f->o.selection, 6, rx_omega_regime(s->field[1], s->field[2]));
    put(c, f->o.selection, 7, best ? best->field[3] : ref_ps);
    return 0;
}

/* ---- omega.reconsider: a published plan reopens a regime (R11) ----
 *
 * Omega is not asked. It observes a plan object and decides for itself:
 * a regime it can realize, no search already in flight. The new epoch runs
 * the ordinary synthesize -> verify -> measure -> select chain, measured on
 * the cores the world runs on now. */
static int fn_reconsider(RxCtx *c) {
    RxOmegaFaculty *f = c->user;
    const RxSnapshotDep *p = in_of(c, f->inquiry);
    const RxSnapshotDep *s = in_of(c, f->o.search);
    const RxSnapshotDep *sel = in_of(c, f->o.selection);
    if (!p || !s || !sel) return -1;
    uint64_t seq = p->field[0];
    if (seq == 0 || seq == s->field[6]) return 0;
    if (s->field[0] != sel->field[0]) return 0;    /* a search is in flight; its selection re-wakes us */
    uint64_t M = p->field[2] >> 32, N = p->field[2] & 0xffffffffu;
    if (p->field[1] != RX_OMEGA_INQ_RESEARCH || !shape_ok(M, N)) {
        put(c, f->o.search, 6, seq);               /* declined, and recorded as seen */
        return 0;
    }
    put(c, f->o.search, 0, s->field[0] + 1);
    put(c, f->o.search, 1, M);
    put(c, f->o.search, 2, N);
    put(c, f->o.search, 6, seq);
    return 0;
}

/* ---- setup ---- */

void rx_omega_default_config(RxOmegaConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->n_slots = OMEGA_MATVEC_KIND_QUAD4 + 1u;
    cfg->defect = RX_OMEGA_DEFECT_NONE;
    cfg->hot_calls = 64;
    cfg->hot_ns = 1000000;
    cfg->margin_pct = 5;
    cfg->sandbox_ms = 5000;
}

int rx_omega_create_objects(RxOmegaFaculty *f, RxWorld *w, const RxOmegaConfig *cfg) {
    memset(f, 0, sizeof(*f));
    f->w = w;
    f->cfg = *cfg;
    if (cfg->defect != RX_OMEGA_DEFECT_NONE) f->cfg.n_slots = RX_OMEGA_SLOTS;
    if (f->cfg.n_slots < 1 || f->cfg.n_slots > RX_OMEGA_SLOTS) return RX_ERR_ARG;
    if (pthread_mutex_init(&f->mu, NULL) != 0) return RX_ERR_ARG;
    if (omega_machine_build_dgx_spark(&f->machine) != 0) return RX_ERR_ARG;
    if (omega_matvec_spec_init(&f->spec, "rx_omega_matvec", RX_OMEGA_MAX_ELEMS,
                               RX_OMEGA_MAX_ELEMS) != 0)
        return RX_ERR_ARG;
    uint64_t z[RX_MAX_FIELDS] = { 0 };
#define MK(ref, type, res) do {                                                        \
        int rc_ = rx_world_create(w, (type), RX_PERSIST_RESIDENT,                       \
                                  RX_OMEGA_RES_BASE + (res), z, &(ref));                \
        if (rc_ != RX_OK) return rc_;                                                   \
    } while (0)
    MK(f->o.request, RX_OT_REQUEST, RX_OMEGA_RES_REQUEST);
    MK(f->o.demand, RX_OT_DEMAND, RX_OMEGA_RES_DEMAND);
    MK(f->o.result, RX_OT_RESULT, RX_OMEGA_RES_RESULT);
    MK(f->o.search, RX_OT_SEARCH, RX_OMEGA_RES_SEARCH);
    MK(f->o.selection, RX_OT_SELECTION, RX_OMEGA_RES_SELECTION);
    for (uint32_t k = 0; k < f->cfg.n_slots; k++) {
        MK(f->o.candidate[k], RX_OT_CANDIDATE, RX_OMEGA_RES_CANDIDATE0 + k);
        MK(f->o.verdict[k], RX_OT_VERDICT, RX_OMEGA_RES_VERDICT0 + k);
        MK(f->o.measure[k], RX_OT_MEASURE, RX_OMEGA_RES_MEASURE0 + k);
    }
#undef MK
    return RX_OK;
}

static uint32_t res_index(const RxWorld *w, RxObjRef o) {
    return (uint32_t)(w->objects[o.id].resource - RX_OMEGA_RES_BASE);
}

typedef struct {
    RxReactionDesc d;
    const RxCapRef *table;
} Builder;

static void need(Builder *b, RxWorld *w, RxObjRef o, uint32_t rights) {
    uint64_t res = w->objects[o.id].resource;
    for (uint32_t i = 0; i < b->d.n_caps; i++)
        if (b->d.caps[i].resource == res) { b->d.caps[i].rights |= rights; return; }
    b->d.caps[b->d.n_caps++] = (RxCapNeed){ b->table[res_index(w, o)], res, rights };
}

static void begin(Builder *b, const char *name, uint32_t faculty, uint32_t subject,
                  uint32_t prio, RxFn fn, void *user, const RxCapRef *table) {
    memset(b, 0, sizeof(*b));
    b->d.name = name;
    b->d.faculty = faculty;
    b->d.subject = subject;
    b->d.priority = prio;
    b->d.fn = fn;
    b->d.user = user;
    b->table = table;
}

static void trig(Builder *b, RxWorld *w, RxObjRef o, uint64_t mask) {
    b->d.triggers[b->d.n_triggers++] = (RxDep){ o, mask };
    need(b, w, o, RX_RIGHT_READ);
}

static void rd(Builder *b, RxWorld *w, RxObjRef o) {
    b->d.reads[b->d.n_reads++] = (RxDep){ o, RX_ALL_FIELDS };
    need(b, w, o, RX_RIGHT_READ);
}

static void wr(Builder *b, RxWorld *w, RxObjRef o) {
    b->d.writes[b->d.n_writes++] = (RxDep){ o, RX_ALL_FIELDS };
    need(b, w, o, RX_RIGHT_READ | RX_RIGHT_WRITE);
}

static const char *const synth_names[RX_OMEGA_SLOTS] = {
    "omega.synthesize.0", "omega.synthesize.1", "omega.synthesize.2", "omega.synthesize.3",
    "omega.synthesize.4" };
static const char *const verify_names[RX_OMEGA_SLOTS] = {
    "omega.verify.0", "omega.verify.1", "omega.verify.2", "omega.verify.3", "omega.verify.4" };
static const char *const measure_names[RX_OMEGA_SLOTS] = {
    "omega.measure.0", "omega.measure.1", "omega.measure.2", "omega.measure.3", "omega.measure.4" };

int rx_omega_register(RxOmegaFaculty *f, const RxOmegaCaps *caps) {
    RxWorld *w = f->w;
    Builder b;
    int rc;

    /* The production path. It stands for any consumer of the operation;
     * it is not labelled as AIEN. */
    begin(&b, "workload.matvec.serve", RX_FACULTY_EXTERNAL, RX_OMEGA_SUBJ_SERVE,
          RX_PRIO_INTERACTIVE, fn_serve, f, caps->serve);
    trig(&b, w, f->o.request, RX_ALL_FIELDS);
    if (f->serve_from_record) {
        b.d.reads[b.d.n_reads++] = (RxDep){ f->serve_record, RX_ALL_FIELDS };
        b.d.caps[b.d.n_caps++] = (RxCapNeed){ f->serve_record_cap,
            w->objects[f->serve_record.id].resource, RX_RIGHT_READ };
    } else {
        rd(&b, w, f->o.selection);
    }
    rd(&b, w, f->o.demand);
    wr(&b, w, f->o.demand);
    wr(&b, w, f->o.result);
    if ((rc = rx_world_add_reaction(w, &b.d, &f->r_serve)) != RX_OK) return rc;

    begin(&b, "omega.watch", RX_FACULTY_OMEGA, RX_OMEGA_SUBJ_OMEGA, RX_PRIO_FOREGROUND,
          fn_watch, f, caps->omega);
    trig(&b, w, f->o.demand, RX_FIELD(2));
    rd(&b, w, f->o.search);
    wr(&b, w, f->o.search);
    if ((rc = rx_world_add_reaction(w, &b.d, &f->r_watch)) != RX_OK) return rc;

    for (uint32_t k = 0; k < f->cfg.n_slots; k++) {
        f->slot[k].f = f;
        f->slot[k].k = k;

        begin(&b, synth_names[k], RX_FACULTY_OMEGA, RX_OMEGA_SUBJ_OMEGA, RX_PRIO_LEARNING,
              fn_synthesize, &f->slot[k], caps->omega);
        trig(&b, w, f->o.search, RX_FIELD(0));
        rd(&b, w, f->o.candidate[k]);
        wr(&b, w, f->o.candidate[k]);
        if ((rc = rx_world_add_reaction(w, &b.d, &f->r_synth[k])) != RX_OK) return rc;

        begin(&b, verify_names[k], RX_FACULTY_OMEGA, RX_OMEGA_SUBJ_OMEGA, RX_PRIO_LEARNING,
              fn_verify, &f->slot[k], caps->omega);
        trig(&b, w, f->o.candidate[k], RX_ALL_FIELDS);
        rd(&b, w, f->o.search);
        rd(&b, w, f->o.verdict[k]);
        wr(&b, w, f->o.verdict[k]);
        if ((rc = rx_world_add_reaction(w, &b.d, &f->r_verify[k])) != RX_OK) return rc;

        begin(&b, measure_names[k], RX_FACULTY_OMEGA, RX_OMEGA_SUBJ_OMEGA, RX_PRIO_LEARNING,
              fn_measure, &f->slot[k], caps->omega);
        trig(&b, w, f->o.verdict[k], RX_ALL_FIELDS);
        rd(&b, w, f->o.candidate[k]);
        rd(&b, w, f->o.search);
        rd(&b, w, f->o.measure[k]);
        wr(&b, w, f->o.measure[k]);
        if ((rc = rx_world_add_reaction(w, &b.d, &f->r_measure[k])) != RX_OK) return rc;
    }

    begin(&b, "omega.select", RX_FACULTY_OMEGA, RX_OMEGA_SUBJ_OMEGA, RX_PRIO_LEARNING,
          fn_select, f, caps->omega);
    for (uint32_t k = 0; k < f->cfg.n_slots; k++) trig(&b, w, f->o.measure[k], RX_ALL_FIELDS);
    if (f->require_experiment_evidence) {
        b.d.triggers[b.d.n_triggers++] = (RxDep){ f->experiment_evidence, RX_FIELD(0) };
        b.d.caps[b.d.n_caps++] = (RxCapNeed){ f->experiment_evidence_cap,
            w->objects[f->experiment_evidence.id].resource, RX_RIGHT_READ };
    }
    rd(&b, w, f->o.search);
    rd(&b, w, f->o.selection);
    wr(&b, w, f->o.selection);
    if ((rc = rx_world_add_reaction(w, &b.d, &f->r_select)) != RX_OK) return rc;
    return RX_OK;
}

int rx_omega_require_evidence(RxOmegaFaculty *f, RxObjRef evidence, RxCapRef read_cap) {
    if (!f || !f->w || evidence.id >= RX_MAX_OBJECTS ||
        !f->w->objects[evidence.id].live ||
        f->w->objects[evidence.id].generation != evidence.generation ||
        read_cap.cap_id == UINT32_MAX) return RX_ERR_ARG;
    f->experiment_evidence = evidence;
    f->experiment_evidence_cap = read_cap;
    f->require_experiment_evidence = 1;
    return RX_OK;
}

int rx_omega_register_reconsider(RxOmegaFaculty *f, RxObjRef plan, RxCapRef read_cap,
                                 RxCapRef search_cap, RxCapRef selection_cap) {
    RxWorld *w = f->w;
    f->inquiry = plan;
    RxReactionDesc d;
    memset(&d, 0, sizeof(d));
    d.name = "omega.reconsider";
    d.faculty = RX_FACULTY_OMEGA;
    d.subject = RX_OMEGA_SUBJ_OMEGA;
    d.priority = RX_PRIO_LEARNING;
    d.fn = fn_reconsider;
    d.user = f;
    d.n_triggers = 2;
    d.triggers[0] = (RxDep){ plan, RX_FIELD(0) };
    d.triggers[1] = (RxDep){ f->o.selection, RX_FIELD(0) };
    d.n_reads = 1;
    d.reads[0] = (RxDep){ f->o.search, RX_ALL_FIELDS };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ f->o.search, RX_ALL_FIELDS };
    d.n_caps = 3;
    d.caps[0] = (RxCapNeed){ read_cap, w->objects[plan.id].resource, RX_RIGHT_READ };
    d.caps[1] = (RxCapNeed){ search_cap, w->objects[f->o.search.id].resource,
                             RX_RIGHT_READ | RX_RIGHT_WRITE };
    d.caps[2] = (RxCapNeed){ selection_cap, w->objects[f->o.selection.id].resource, RX_RIGHT_READ };
    return rx_world_add_reaction(w, &d, &f->r_reconsider);
}

void rx_omega_destroy(RxOmegaFaculty *f) {
    for (uint32_t i = 0; i < f->n_store; i++)
        if (f->store[i].page) munmap(f->store[i].page, PAGE);
    f->n_store = 0;
    pthread_mutex_destroy(&f->mu);
}

int rx_omega_serve_from(RxOmegaFaculty *f, RxObjRef record, RxCapRef read_cap) {
    if (!f || !f->w || record.id >= RX_MAX_OBJECTS || !f->w->objects[record.id].live ||
        f->w->objects[record.id].generation != record.generation ||
        read_cap.cap_id == UINT32_MAX) return RX_ERR_ARG;
    f->serve_record = record;
    f->serve_record_cap = read_cap;
    f->serve_from_record = 1;
    return RX_OK;
}

int rx_omega_identity_of(const RxOmegaFaculty *f, const uint8_t *code, size_t len,
                         SemanticId *out) {
    if (!f || !code || !out || len == 0 || len > AARCH64_MAX_CODE_BYTES) return -1;
    RealizationObject r;
    real_object(f, code, len, &r);
    return omega_realize_compute_triple_id(&f->spec.spec_id, &f->machine.machine_id, &r, out);
}

int rx_omega_readmit(RxOmegaFaculty *f, const uint8_t *code, size_t len, uint32_t kind,
                     uint64_t regime, SemanticId *out_id, uint32_t *out_why) {
    if (!f || !code || !out_id || !out_why || kind >= f->cfg.n_slots) return -1;
    uint64_t M = regime >> 32, N = regime & 0xffffffffu;
    if (!shape_ok(M, N) || rx_omega_identity_of(f, code, len, out_id) != 0) return -1;
    /* The store holds one set of bytes per identity. Bytes that hash to
     * this identity can only be these bytes. */
    if (store_put(f, out_id, kind, code, len) != 0) {
        *out_why = RX_OMEGA_WHY_IDENTITY;
        return 1;
    }
    uint32_t checks = 0, fails = 0;
    int why = verify_stored(f, out_id, M, N, &checks, &fails);
    if (why < 0) return -1;
    *out_why = (uint32_t)why;
    return why == RX_OMEGA_WHY_NONE ? 0 : 1;
}
