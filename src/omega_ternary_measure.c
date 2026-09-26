#include "omega_ternary_measure.h"
#include <elf.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__aarch64__)
#include <asm/ptrace.h>
#endif

typedef uint64_t (*TFn2)(uint64_t, uint64_t);

/* Maps the realization read+exec; caller unmaps with jit_unmap. */
static void *jit_map(const RealizationObject *real, size_t *out_size) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t size = (real->code_len + page - 1) & ~(page - 1);
    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) return NULL;
    memcpy(mem, real->code_bytes, real->code_len);
    __builtin___clear_cache((char *)mem, (char *)mem + real->code_len);
    if (mprotect(mem, size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, size);
        return NULL;
    }
    *out_size = size;
    return mem;
}

static TFn2 as_fn(void *mem) {
    union { void *p; TFn2 f; } u;
    u.p = mem;
    return u.f;
}

int omega_t_exec2(const RealizationObject *real, uint64_t a, uint64_t b, uint64_t *out_result) {
#if defined(__aarch64__)
    if (!real || !out_result || real->code_len == 0) return -1;
    size_t size = 0;
    void *mem = jit_map(real, &size);
    if (!mem) return -1;
    *out_result = as_fn(mem)(a, b);
    munmap(mem, size);
    return 0;
#else
    (void)real; (void)a; (void)b; (void)out_result;
    return -2;
#endif
}

int omega_t_jit_open(const RealizationObject *real, TJit *jit) {
#if defined(__aarch64__)
    if (!real || !jit || real->code_len == 0) return -1;
    jit->mem = jit_map(real, &jit->size);
    return jit->mem ? 0 : -1;
#else
    (void)real; (void)jit;
    return -2;
#endif
}

uint64_t omega_t_jit_call(const TJit *jit, uint64_t a, uint64_t b) {
    return as_fn(jit->mem)(a, b);
}

void omega_t_jit_close(TJit *jit) {
    if (jit && jit->mem) munmap(jit->mem, jit->size);
    if (jit) jit->mem = NULL;
}

int omega_t_measure_dyn(const RealizationObject *real, uint64_t a, uint64_t b,
                        uint64_t *out_result, uint64_t *out_insns) {
#if defined(__aarch64__)
    if (!real || !out_insns || real->code_len == 0) return -1;
    size_t size = 0;
    void *mem = jit_map(real, &size);
    if (!mem) return -1;
    volatile uint64_t *shared = mmap(NULL, sizeof(uint64_t), PROT_READ | PROT_WRITE,
                                     MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if (shared == MAP_FAILED) { munmap(mem, size); return -1; }

    pid_t pid = fork();
    if (pid < 0) { munmap(mem, size); munmap((void *)shared, sizeof(uint64_t)); return -1; }
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        *shared = as_fn(mem)(a, b);
        _exit(0);
    }

    int status = 0;
    int rc = -1;
    uint64_t count = 0;
    uint64_t lo = (uint64_t)(uintptr_t)mem, hi = lo + real->code_len;
    if (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status)) goto done;
    for (;;) {
        if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) != 0) goto done;
        if (waitpid(pid, &status, 0) != pid) goto done;
        if (WIFEXITED(status)) { rc = WEXITSTATUS(status) == 0 ? 0 : -1; break; }
        if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            goto done;
        }
        struct user_pt_regs regs;
        struct iovec iov = { &regs, sizeof(regs) };
        if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) != 0) goto done;
        if (regs.pc >= lo && regs.pc < hi) count++;
    }
    /* The step that lands on the entry was counted as a stop at the first
     * instruction; every executed instruction in the region is one stop. */
    *out_insns = count;
    if (out_result) *out_result = *shared;
done:
    munmap(mem, size);
    munmap((void *)shared, sizeof(uint64_t));
    return rc;
#else
    (void)real; (void)a; (void)b; (void)out_result; (void)out_insns;
    return -2;
#endif
}

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static int cmp_double(const void *x, const void *y) {
    double a = *(const double *)x, b = *(const double *)y;
    return (a > b) - (a < b);
}

static double time_fn(TFn2 fn, const uint64_t *ia, const uint64_t *ib, size_t n,
                      size_t calls, size_t rounds) {
    double samples[64];
    if (rounds > 64) rounds = 64;
    volatile uint64_t sink = 0;
    for (size_t r = 0; r < rounds; ++r) {
        double t0 = now_ns();
        for (size_t c = 0; c < calls; ++c) {
            size_t i = c % n;
            sink += fn(ia[i], ib[i]);
        }
        samples[r] = (now_ns() - t0) / (double)calls;
    }
    (void)sink;
    qsort(samples, rounds, sizeof(double), cmp_double);
    return samples[rounds / 2];
}

double omega_t_measure_ns(const RealizationObject *real, const uint64_t *inputs_a,
                          const uint64_t *inputs_b, size_t n_inputs, size_t calls_per_round,
                          size_t rounds) {
#if defined(__aarch64__)
    if (!real || !inputs_a || !inputs_b || n_inputs == 0) return -1.0;
    RealizationObject bare;
    memset(&bare, 0, sizeof(bare));
    bare.code_bytes[0] = 0xC0; bare.code_bytes[1] = 0x03;
    bare.code_bytes[2] = 0x5F; bare.code_bytes[3] = 0xD6; /* RET */
    bare.code_len = 4;
    size_t s1 = 0, s2 = 0;
    void *m1 = jit_map(real, &s1);
    void *m2 = jit_map(&bare, &s2);
    if (!m1 || !m2) {
        if (m1) munmap(m1, s1);
        if (m2) munmap(m2, s2);
        return -1.0;
    }
    time_fn(as_fn(m1), inputs_a, inputs_b, n_inputs, calls_per_round / 4 + 1, 3); /* warm */
    double t = time_fn(as_fn(m1), inputs_a, inputs_b, n_inputs, calls_per_round, rounds);
    double base = time_fn(as_fn(m2), inputs_a, inputs_b, n_inputs, calls_per_round, rounds);
    munmap(m1, s1);
    munmap(m2, s2);
    double d = t - base;
    return d < 0 ? 0.0 : d;
#else
    (void)real; (void)inputs_a; (void)inputs_b; (void)n_inputs; (void)calls_per_round; (void)rounds;
    return -1.0;
#endif
}
