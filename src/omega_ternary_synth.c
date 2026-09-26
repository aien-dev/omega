#include "omega_ternary_synth.h"
#include "omega_ternary_measure.h"
#include "sha256.h"
#include <linux/seccomp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* =========================================================================
 * Bank: mirrors the binary bank's structure (add/sub/mul with the same
 * constants, two AND-like and two OR-like constants), plus the operations
 * the ternary profile has natively (negation, shifts by trits, sign).
 * ========================================================================= */

void omega_t_bank_init(TBank *bank) {
    static const struct { const char *name; TernaryOp op; int64_t k; } defs[] = {
        { "tadd_1", TOP_TADD, 1 }, { "tadd_2", TOP_TADD, 2 }, { "tadd_3", TOP_TADD, 3 }, { "tadd_5", TOP_TADD, 5 },
        { "tsub_1", TOP_TSUB, 1 }, { "tsub_2", TOP_TSUB, 2 }, { "tsub_3", TOP_TSUB, 3 }, { "tsub_5", TOP_TSUB, 5 },
        { "tmul_2", TOP_TMUL, 2 }, { "tmul_3", TOP_TMUL, 3 }, { "tmul_4", TOP_TMUL, 4 },
        { "tand_40", TOP_TAND, 40 }, { "tand_13", TOP_TAND, 13 },
        { "tor_1", TOP_TOR, 1 }, { "tor_4", TOP_TOR, 4 },
        { "tneg", TOP_TNEG, 0 }, { "tshl_1", TOP_TSHL, 1 }, { "tshl_2", TOP_TSHL, 2 },
        { "tshr_1", TOP_TSHR, 1 }, { "tshr_2", TOP_TSHR, 2 }, { "tsign", TOP_TSIGN, 0 }
    };
    memset(bank, 0, sizeof(*bank));
    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]) && i < TSYN_MAX_BANK; ++i) {
        snprintf(bank->prims[i].name, sizeof(bank->prims[i].name), "%s", defs[i].name);
        bank->prims[i].step.op = defs[i].op;
        bank->prims[i].step.k = defs[i].k;
        bank->count++;
    }
}

/* =========================================================================
 * Task set: arithmetic that favors neither radix a priori, plus tasks native
 * to each side (bit masks and halving for binary, thirds and sign for ternary).
 * ========================================================================= */

static int64_t f_2x_plus_1(int64_t x) { return 2 * x + 1; }
static int64_t f_3x_minus_2(int64_t x) { return 3 * x - 2; }
static int64_t f_9x_plus_4(int64_t x) { return 9 * x + 4; }
static int64_t f_neg(int64_t x) { return -x; }
static int64_t f_neg3x_plus_1(int64_t x) { return -3 * x + 1; }
static int64_t f_round_div3(int64_t x) { int64_t q = 0; omega_t_eval(TOP_TSHR, x, 1, &q, NULL); return q; }
static int64_t f_round_div9(int64_t x) { int64_t q = 0; omega_t_eval(TOP_TSHR, x, 2, &q, NULL); return q; }
static int64_t f_4x(int64_t x) { return 4 * x; }
static int64_t f_6x_minus_5(int64_t x) { return 6 * x - 5; }
static int64_t f_sign(int64_t x) { return (x > 0) - (x < 0); }
static int64_t f_mask_low8(int64_t x) { return (int64_t)((uint64_t)x & 0xFF); }
static int64_t f_floor_half(int64_t x) { return x < 0 ? -((-x + 1) / 2) : x / 2; }
static int64_t f_x_plus_10(int64_t x) { return x + 10; }
static int64_t f_27x(int64_t x) { return 27 * x; }
static int64_t f_8x(int64_t x) { return 8 * x; }
static int64_t f_3x_plus_15(int64_t x) { return 3 * (x + 5); }

size_t omega_t_tasks_init(TTask *tasks, size_t max) {
    static const struct { const char *name; const char *formula; int64_t (*fn)(int64_t); } defs[] = {
        { "affine_2x_plus_1", "2x + 1", f_2x_plus_1 },
        { "affine_3x_minus_2", "3x - 2", f_3x_minus_2 },
        { "affine_9x_plus_4", "9x + 4", f_9x_plus_4 },
        { "negate", "-x", f_neg },
        { "affine_neg3x_plus_1", "-3x + 1", f_neg3x_plus_1 },
        { "round_div3", "round(x / 3)", f_round_div3 },
        { "round_div9", "round(x / 9)", f_round_div9 },
        { "scale_4x", "4x", f_4x },
        { "affine_6x_minus_5", "6x - 5", f_6x_minus_5 },
        { "sign", "sign(x)", f_sign },
        { "mask_low8", "x & 0xFF", f_mask_low8 },
        { "floor_half", "floor(x / 2)", f_floor_half },
        { "add_10", "x + 10", f_x_plus_10 },
        { "scale_27x", "27x", f_27x },
        { "scale_8x", "8x", f_8x },
        { "affine_3x_plus_15", "3(x + 5)", f_3x_plus_15 },
    };
    static const int64_t xs[TSYN_MAX_EXAMPLES] = {
        -100000, -9841, -500, -37, -5, -1, 0, 1, 2, 7, 13, 42, 500, 9841, 100000, TSYN_DOMAIN_BOUND
    };
    size_t n = sizeof(defs) / sizeof(defs[0]);
    if (n > max) n = max;
    for (size_t t = 0; t < n; ++t) {
        memset(&tasks[t], 0, sizeof(TTask));
        snprintf(tasks[t].name, sizeof(tasks[t].name), "%s", defs[t].name);
        snprintf(tasks[t].formula, sizeof(tasks[t].formula), "%s", defs[t].formula);
        tasks[t].fn = defs[t].fn;
        tasks[t].n = TSYN_MAX_EXAMPLES;
        tasks[t].domain_bound = TSYN_DOMAIN_BOUND;
        for (size_t i = 0; i < TSYN_MAX_EXAMPLES; ++i) {
            tasks[t].inputs[i] = xs[i];
            tasks[t].outputs[i] = defs[t].fn(xs[i]);
        }
    }
    return n;
}

/* =========================================================================
 * Lowering-aware cost model
 * ========================================================================= */

double omega_t_best_realization(const TChain *chain, const int64_t *xs, size_t n, int64_t bound,
                                TernaryRep *reps, TStepFlags *flags) {
    if (omega_t_chain_intervals(chain, bound, flags) != 0) return -1.0;
    double best = -1.0;
    TernaryRep trial[TCHAIN_MAX_STEPS];
    for (uint32_t mask = 0; mask < (1u << chain->n); ++mask) {
        bool ok = true;
        for (size_t i = 0; i < chain->n; ++i) {
            trial[i] = (mask >> i) & 1 ? TREP_PLANES : TREP_INT;
            if (!omega_t_a64_supported(chain->steps[i].op, trial[i])) ok = false;
        }
        if (!ok) continue;
        double sum = 0;
        for (size_t j = 0; j < n; ++j) sum += (double)omega_t_a64_chain_dyn(chain, trial, flags, xs[j]);
        double mean = sum / (double)n;
        if (best < 0 || mean < best) {
            best = mean;
            memcpy(reps, trial, sizeof(TernaryRep) * chain->n);
        }
    }
    return best;
}

/* =========================================================================
 * Enumerative synthesis (same shape as the binary V0 synthesizer: bottom-up
 * by depth, observational-equivalence pruning on probe outputs, cost-aware).
 * ========================================================================= */

static const int64_t PROBES[TSYN_PROBES] = { 0, 1, 3, 7, 13, 42, -1, -13 };

#define TSYN_MAX_SIGS 16384
#define TSYN_MAX_LEVEL 1024

typedef struct {
    uint8_t hash[32];
    double cost;
} TSig;

static bool sig_seen_or_add(TSig *tbl, size_t *count, const uint8_t hash[32], double cost) {
    for (size_t i = 0; i < *count; ++i) {
        if (memcmp(tbl[i].hash, hash, 32) == 0) {
            if (tbl[i].cost <= cost) return true;
            tbl[i].cost = cost;
            return false;
        }
    }
    if (*count < TSYN_MAX_SIGS) {
        memcpy(tbl[*count].hash, hash, 32);
        tbl[*count].cost = cost;
        (*count)++;
    }
    return false;
}

static bool chain_signature(const TChain *c, uint8_t out[32]) {
    int64_t outs[TSYN_PROBES];
    for (size_t i = 0; i < TSYN_PROBES; ++i) {
        if (omega_t_chain_eval(c, PROBES[i], &outs[i]) != 0) return false;
    }
    sha256_hash((const uint8_t *)outs, sizeof(outs), out);
    return true;
}

static bool chain_solves(const TChain *c, const TTask *task) {
    for (size_t i = 0; i < task->n; ++i) {
        int64_t y = 0;
        if (omega_t_chain_eval(c, task->inputs[i], &y) != 0 || y != task->outputs[i]) return false;
    }
    return true;
}

static TSig g_sigs[TSYN_MAX_SIGS];
static TChain g_level[2][TSYN_MAX_LEVEL];

int omega_t_synthesize(const TTask *task, const TBank *bank, uint32_t max_depth, TSynthResult *res) {
    if (!task || !bank || !res || max_depth < 1 || max_depth > 3) return -1;
    memset(res, 0, sizeof(*res));
    size_t nsig = 0;
    size_t cur_n = 1;
    g_level[0][0].n = 0; /* the empty chain seeds depth 1 */
    int cur = 0;
    TChain best_chain;
    double best_cost = -1.0;

    for (uint32_t depth = 1; depth <= max_depth; ++depth) {
        int nxt = cur ^ 1;
        size_t nxt_n = 0;
        for (size_t i = 0; i < cur_n; ++i) {
            for (size_t j = 0; j < bank->count; ++j) {
                TChain c = g_level[cur][i];
                c.steps[c.n++] = bank->prims[j].step;
                res->stats.candidates++;
                TernaryRep reps[TCHAIN_MAX_STEPS];
                TStepFlags fl[TCHAIN_MAX_STEPS];
                double cost = omega_t_best_realization(&c, task->inputs, task->n, task->domain_bound, reps, fl);
                if (cost < 0) continue;
                /* Solutions are recorded before pruning so no example-distinct
                 * program is lost to a probe collision. */
                if (chain_solves(&c, task)) {
                    res->stats.solutions++;
                    if (best_cost < 0 || cost < best_cost || (cost == best_cost && c.n < best_chain.n)) {
                        best_cost = cost;
                        best_chain = c;
                    }
                }
                uint8_t sig[32];
                if (!chain_signature(&c, sig)) continue;
                if (sig_seen_or_add(g_sigs, &nsig, sig, cost)) {
                    res->stats.pruned_equiv++;
                    continue;
                }
                if (depth < max_depth && nxt_n < TSYN_MAX_LEVEL) g_level[nxt][nxt_n++] = c;
            }
        }
        cur = nxt;
        cur_n = nxt_n;
    }

    if (best_cost < 0) return 0;
    TProgram *p = &res->prog;
    memset(p, 0, sizeof(*p));
    p->chain = best_chain;
    p->domain_bound = task->domain_bound;
    res->predicted_cost = omega_t_best_realization(&p->chain, task->inputs, task->n, task->domain_bound,
                                                   p->reps, p->flags);
    if (omega_t_program_realize(p) != 0 || omega_t_program_compute_id(p) != 0) return -1;
    VerifyReport rep;
    p->is_verified = omega_t_verify_pipeline(p, &rep) == 0;
    res->solved = true;
    return 0;
}

/* =========================================================================
 * Binary control
 * ========================================================================= */

static int build_binary_extra(OmegaProgram *prog, const char *name, TBinaryExtra kind, int imm, const char *post) {
    omega_program_init(prog, name);
    prog->contract.input_type = TYPE_UNSIGNED_INT;
    prog->contract.input_width = 64;
    prog->contract.output_type = TYPE_UNSIGNED_INT;
    prog->contract.output_width = 64;
    snprintf(prog->contract.precondition, sizeof(prog->contract.precondition), "x >= 0");
    omega_build_constraint_id(CONST_PRECONDITION, prog->contract.precondition, &prog->contract.precondition_id);
    snprintf(prog->contract.postcondition, sizeof(prog->contract.postcondition), "%s", post);
    omega_build_constraint_id(CONST_POSTCONDITION, prog->contract.postcondition, &prog->contract.postcondition_id);
    if (omega_t_a64_binary_extra(kind, imm, &prog->realization) != 0) return -1;
    prog->cost.insn_count = (uint32_t)(prog->realization.code_len / 4);
    prog->cost.reg_pressure = 1;
    prog->cost.latency_cycles = 1;
    omega_program_compute_id(prog);
    prog->is_realized = true;
    return 0;
}

int omega_t_binary_bank_augmented(SynthPrimitiveBank *bank) {
    if (omega_synth_bank_init(bank) != 0) return -1;
    if (bank->count + 3 > SYNTH_MAX_PRIMITIVES) return -1;
    if (build_binary_extra(&bank->programs[bank->count], "neg", TBIN_NEG, 0, "-x") != 0) return -1;
    bank->count++;
    if (build_binary_extra(&bank->programs[bank->count], "lsl_1", TBIN_LSL, 1, "x << 1") != 0) return -1;
    bank->count++;
    if (build_binary_extra(&bank->programs[bank->count], "asr_1", TBIN_ASR, 1, "x >> 1") != 0) return -1;
    bank->count++;
    return 0;
}

static int binary_solve(const TTask *task, const SynthPrimitiveBank *bank, SynthesisResult *out) {
    SynthesisTask st;
    uint64_t in[TSYN_MAX_EXAMPLES], outp[TSYN_MAX_EXAMPLES];
    for (size_t i = 0; i < task->n; ++i) {
        in[i] = (uint64_t)task->inputs[i];
        outp[i] = (uint64_t)task->outputs[i];
    }
    if (omega_task_init(&st, task->formula, TYPE_UNSIGNED_INT, 64, TYPE_UNSIGNED_INT, 64, in, outp, task->n) != 0) return -1;
    /* Depth-3 compositions of three 3-insn primitives need more than the default budget of 10. */
    st.cost_budget.insn_count = 16;
    SynthesisConfig cfg = { .max_depth = 3, .max_cost = 16, .max_candidates = 20000, .deduplicate_equiv = true };
    return omega_synthesize(&st, bank, &cfg, out);
}

/* =========================================================================
 * Measurement helpers
 * ========================================================================= */

static double mean_traced_dyn(const RealizationObject *real, const TTask *task, bool *all_ok) {
    double sum = 0;
    size_t n = 0;
    for (size_t i = 0; i < task->n; ++i) {
        uint64_t res = 0, insns = 0;
        if (omega_t_measure_dyn(real, (uint64_t)task->inputs[i], 0, &res, &insns) != 0) { *all_ok = false; continue; }
        if ((int64_t)res != task->outputs[i]) *all_ok = false;
        sum += (double)insns;
        n++;
    }
    return n ? sum / (double)n : -1.0;
}

static double time_on_task(const RealizationObject *real, const TTask *task) {
    uint64_t a[TSYN_MAX_EXAMPLES], b[TSYN_MAX_EXAMPLES];
    for (size_t i = 0; i < task->n; ++i) { a[i] = (uint64_t)task->inputs[i]; b[i] = 0; }
    return omega_t_measure_ns(real, a, b, task->n, 400000, 15);
}

static void describe_chain(const TProgram *p, const TBank *bank, char *prog_out, size_t plen, char *rep_out, size_t rlen) {
    prog_out[0] = '\0';
    rep_out[0] = '\0';
    for (size_t i = 0; i < p->chain.n; ++i) {
        const char *nm = "?";
        for (size_t j = 0; j < bank->count; ++j) {
            if (bank->prims[j].step.op == p->chain.steps[i].op && bank->prims[j].step.k == p->chain.steps[i].k) {
                nm = bank->prims[j].name;
                break;
            }
        }
        size_t used = strlen(prog_out);
        snprintf(prog_out + used, plen - used, "%s%s", i ? " ; " : "", nm);
        used = strlen(rep_out);
        snprintf(rep_out + used, rlen - used, "%s%c", i ? "," : "", p->reps[i] == TREP_PLANES ? 'P' : 'I');
    }
}

static void fill_binary(TPathResult *r, const SynthesisResult *sr, const TTask *task, bool measure_time) {
    memset(r, 0, sizeof(*r));
    if (!sr->solved) return;
    const OmegaProgram *p = &sr->solution;
    r->solved = true;
    r->steps = 1;
    for (const char *c = p->name; *c; ++c) if (*c == 'o' && c[1] == '(') r->steps++;
    r->static_insns = (uint32_t)(p->realization.code_len / 4);
    r->predicted_dyn = (double)r->static_insns; /* straight-line code */
    bool ok = sr->verify_report.passed;
    r->measured_dyn = mean_traced_dyn(&p->realization, task, &ok);
    r->verified = ok;
    r->ns_per_call = measure_time ? time_on_task(&p->realization, task) : -1.0;
    snprintf(r->program, sizeof(r->program), "%.95s", p->name);
    snprintf(r->realization, sizeof(r->realization), "a64");
}

size_t omega_t_run_comparison(TCompareRow *rows, size_t max_rows, bool measure_time) {
    static TTask tasks[TSYN_MAX_TASKS];
    size_t nt = omega_t_tasks_init(tasks, TSYN_MAX_TASKS);
    if (nt > max_rows) nt = max_rows;
    TBank tbank;
    omega_t_bank_init(&tbank);
    static SynthPrimitiveBank bin, aug;
    omega_synth_bank_init(&bin);
    omega_t_binary_bank_augmented(&aug);

    for (size_t t = 0; t < nt; ++t) {
        TCompareRow *row = &rows[t];
        memset(row, 0, sizeof(*row));
        snprintf(row->task, sizeof(row->task), "%.31s", tasks[t].name);
        snprintf(row->formula, sizeof(row->formula), "%.47s", tasks[t].formula);

        static TSynthResult tr;
        if (omega_t_synthesize(&tasks[t], &tbank, 3, &tr) == 0 && tr.solved) {
            TPathResult *r = &row->ternary;
            r->solved = true;
            r->steps = (uint32_t)tr.prog.chain.n;
            r->static_insns = (uint32_t)(tr.prog.real.code_len / 4);
            r->predicted_dyn = tr.predicted_cost;
            bool ok = tr.prog.is_verified;
            r->measured_dyn = mean_traced_dyn(&tr.prog.real, &tasks[t], &ok);
            r->verified = ok;
            r->ns_per_call = measure_time ? time_on_task(&tr.prog.real, &tasks[t]) : -1.0;
            describe_chain(&tr.prog, &tbank, r->program, sizeof(r->program), r->realization, sizeof(r->realization));
        }

        static SynthesisResult sr;
        memset(&sr, 0, sizeof(sr));
        if (binary_solve(&tasks[t], &bin, &sr) == 0) fill_binary(&row->binary, &sr, &tasks[t], measure_time);
        memset(&sr, 0, sizeof(sr));
        if (binary_solve(&tasks[t], &aug, &sr) == 0) fill_binary(&row->binary_aug, &sr, &tasks[t], measure_time);
    }
    omega_synth_bank_destroy(&bin);
    omega_synth_bank_destroy(&aug);
    return nt;
}

/* =========================================================================
 * AEGIS parity: sandboxed mutation campaign
 * ========================================================================= */

typedef struct {
    volatile int stage;      /* 1 = verdict written, 2 = truth written */
    volatile int accepted;
    volatile int equivalent;
} TVerdict;

static void enter_sandbox(void) {
    alarm(5);
    prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT);
}

static void sandbox_exit(void) {
    syscall(SYS_exit, 0);
}

/* Truth: the mutant must match the reference on every point of the domain. */
static int ternary_truth(const TProgram *orig, const TJit *mut) {
    for (int64_t x = -orig->domain_bound; x <= orig->domain_bound; ++x) {
        int64_t want = 0;
        omega_t_chain_eval(&orig->chain, x, &want);
        if ((int64_t)omega_t_jit_call(mut, (uint64_t)x, 0) != want) return 0;
    }
    return 1;
}

static int binary_truth(const TJit *orig, const TJit *mut, int64_t bound) {
    for (int64_t x = -bound; x <= bound; ++x) {
        if (omega_t_jit_call(orig, (uint64_t)x, 0) != omega_t_jit_call(mut, (uint64_t)x, 0)) return 0;
    }
    return 1;
}

static void tally(TParity *p, const TVerdict *v, int status) {
    p->mutants++;
    bool finished = WIFEXITED(status);
    if (!finished) p->crashed++;
    bool accepted = v->stage >= 1 && v->accepted;
    bool equivalent = v->stage >= 2 && v->equivalent;
    if (accepted) p->accepted++; else p->rejected++;
    if (accepted && !equivalent) p->false_accepts++;
    if (!accepted && equivalent) p->false_rejects++;
}

static uint64_t plcg(uint64_t *s) {
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return *s;
}

static void flip(RealizationObject *r, size_t insn, int bit) {
    r->code_bytes[4 * insn + (size_t)(bit / 8)] ^= (uint8_t)(1u << (bit % 8));
}

static void run_ternary_mutant(const TProgram *orig, size_t insn, int bit, TParity *p, TVerdict *v) {
    TProgram mut = *orig;
    flip(&mut.real, insn, bit);
    memset((void *)v, 0, sizeof(*v));
    pid_t pid = fork();
    if (pid == 0) {
        TJit jit;
        if (omega_t_jit_open(&mut.real, &jit) != 0) sandbox_exit();
        enter_sandbox();
        VerifyReport rep;
        v->accepted = omega_t_verify_pipeline_mapped(&mut, &jit, &rep) == 0;
        v->stage = 1;
        v->equivalent = ternary_truth(orig, &jit);
        v->stage = 2;
        sandbox_exit();
    }
    int status = 0;
    waitpid(pid, &status, 0);
    tally(p, v, status);
}

static void run_binary_mutant(const OmegaProgram *orig, size_t insn, int bit, TParity *p, TVerdict *v) {
    OmegaProgram mut = *orig;
    flip(&mut.realization, insn, bit);
    mut.graph = NULL;
    memset((void *)v, 0, sizeof(*v));
    pid_t pid = fork();
    if (pid == 0) {
        TJit jo, jm;
        if (omega_t_jit_open(&orig->realization, &jo) != 0 || omega_t_jit_open(&mut.realization, &jm) != 0) sandbox_exit();
        /* The binary verifier (V0 + V2) does not execute the realization. */
        VerifyReport rep;
        v->accepted = omega_program_verify(&mut, &rep) == 0;
        v->stage = 1;
        enter_sandbox();
        v->equivalent = binary_truth(&jo, &jm, TSYN_DOMAIN_BOUND);
        v->stage = 2;
        sandbox_exit();
    }
    int status = 0;
    waitpid(pid, &status, 0);
    tally(p, v, status);
}

int omega_t_run_parity(TParity *ternary, TParity *binary, size_t flips_per_insn) {
    memset(ternary, 0, sizeof(*ternary));
    memset(binary, 0, sizeof(*binary));
    TVerdict *v = mmap(NULL, sizeof(TVerdict), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if (v == MAP_FAILED) return -1;
    static TTask tasks[TSYN_MAX_TASKS];
    size_t nt = omega_t_tasks_init(tasks, TSYN_MAX_TASKS);
    TBank tbank;
    omega_t_bank_init(&tbank);
    static SynthPrimitiveBank aug;
    omega_t_binary_bank_augmented(&aug);
    uint64_t seed = 0x9A4170ULL;
    for (size_t t = 0; t < nt; ++t) {
        static TSynthResult tr;
        if (omega_t_synthesize(&tasks[t], &tbank, 3, &tr) == 0 && tr.solved && tr.prog.is_verified) {
            size_t n = tr.prog.real.code_len / 4;
            for (size_t i = 0; i + 1 < n; ++i)
                for (size_t f = 0; f < flips_per_insn; ++f)
                    run_ternary_mutant(&tr.prog, i, (int)(plcg(&seed) >> 59), ternary, v);
        }
        static SynthesisResult sr;
        memset(&sr, 0, sizeof(sr));
        if (binary_solve(&tasks[t], &aug, &sr) == 0 && sr.solved && sr.verify_report.passed) {
            size_t n = sr.solution.realization.code_len / 4;
            for (size_t i = 0; i + 1 < n; ++i)
                for (size_t f = 0; f < flips_per_insn; ++f)
                    run_binary_mutant(&sr.solution, i, (int)(plcg(&seed) >> 59), binary, v);
        }
    }
    omega_synth_bank_destroy(&aug);
    munmap(v, sizeof(TVerdict));
    return 0;
}
