#include "cl_program.h"
#include "omega_core.h"
#include "omega_synthesis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint64_t PROBES[CL_PROBE_COUNT] = {
    0ULL, 1ULL, 2ULL, 3ULL, 5ULL, 7ULL, 13ULL, 42ULL,
    100ULL, 127ULL, 128ULL, 255ULL, 256ULL, 1000ULL, 4095ULL, 65535ULL,
    65536ULL, 1000003ULL, 0x7FFFFFFFULL, 0x80000000ULL, 0xFFFFFFFFULL, 0x100000000ULL,
    0x123456789ABCDEFULL, 0x0F0F0F0F0F0F0F0FULL, 0x5555555555555555ULL, 0xAAAAAAAAAAAAAAAAULL,
    0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL,
    0x9E3779B97F4A7C15ULL, 0xD1B54A32D192ED03ULL,
};

const uint64_t *cl_probe_inputs(void) { return PROBES; }

bool cl_step_op_valid(uint8_t op) {
    return op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_AND || op == OP_OR;
}

static uint64_t eval_step(const ClStep *s, uint64_t x) {
    uint64_t r = 0;
    if (omega_eval_pure_binary_uint((OpCode)s->op, OVERFLOW_WRAP, 64, x, s->imm, &r) != 0) return 0;
    return r;
}

uint64_t cl_steps_eval(const ClSteps *p, uint64_t x) {
    for (uint8_t i = 0; i < p->n; ++i) x = eval_step(&p->s[i], x);
    return x;
}

uint32_t cl_steps_insns(const ClSteps *p) {
    uint32_t n = 1; /* RET */
    for (uint8_t i = 0; i < p->n; ++i) n += 2u + ((p->s[i].imm >> 16) != 0 ? 1u : 0u);
    return n;
}

int cl_steps_concat(const ClSteps *a, const ClSteps *b, ClSteps *out) {
    if ((unsigned)a->n + b->n > CL_MAX_STEPS) return -1;
    ClSteps t;
    memset(&t, 0, sizeof(t));
    t.n = (uint8_t)(a->n + b->n);
    memcpy(t.s, a->s, a->n * sizeof(ClStep));
    memcpy(t.s + a->n, b->s, b->n * sizeof(ClStep));
    *out = t;
    return 0;
}

int cl_steps_encode(const ClSteps *p, ClWriter *w) {
    if (p->n > CL_MAX_STEPS) return -1;
    cl_w_u8(w, p->n);
    for (uint8_t i = 0; i < p->n; ++i) {
        cl_w_u8(w, p->s[i].op);
        cl_w_u64(w, p->s[i].imm);
    }
    return w->overflow ? -1 : 0;
}

int cl_steps_decode(ClReader *r, ClSteps *out) {
    memset(out, 0, sizeof(*out));
    out->n = cl_r_u8(r);
    if (r->error || out->n > CL_MAX_STEPS) {
        r->error = true;
        return -1;
    }
    for (uint8_t i = 0; i < out->n; ++i) {
        out->s[i].op = cl_r_u8(r);
        out->s[i].imm = cl_r_u64(r);
        if (!cl_step_op_valid(out->s[i].op)) r->error = true;
    }
    return r->error ? -1 : 0;
}

void cl_steps_digest(const ClSteps *p, uint8_t out[CL_DIGEST_BYTES]) {
    uint8_t buf[1 + CL_MAX_STEPS * 9];
    ClWriter w;
    cl_w_init(&w, buf, sizeof(buf));
    cl_steps_encode(p, &w);
    cl_digest("CRUMBLINE-V1-PROGRAM", buf, w.len, out);
}

void cl_steps_behavior(const ClSteps *p, uint8_t out[CL_DIGEST_BYTES]) {
    uint8_t buf[CL_PROBE_COUNT * 8];
    ClWriter w;
    cl_w_init(&w, buf, sizeof(buf));
    for (size_t i = 0; i < CL_PROBE_COUNT; ++i) cl_w_u64(&w, cl_steps_eval(p, PROBES[i]));
    cl_digest("CRUMBLINE-V1-BEHAVIOR", buf, w.len, out);
}

int cl_steps_build_program(const ClSteps *p, OmegaProgram *out) {
    if (!p || !out || p->n == 0) return -1;
    OmegaProgram acc;
    if (omega_program_build_unary_op(&acc, "s", (OpCode)p->s[0].op, p->s[0].imm) != 0) return -1;
    for (uint8_t i = 1; i < p->n; ++i) {
        OmegaProgram step, next;
        if (omega_program_build_unary_op(&step, "s", (OpCode)p->s[i].op, p->s[i].imm) != 0) {
            omega_program_destroy(&acc);
            return -1;
        }
        char err[160];
        int rc = omega_program_compose(&acc, &step, &next, err, sizeof(err));
        omega_program_destroy(&step);
        omega_program_destroy(&acc);
        if (rc != 0) return -1;
        acc = next;
    }
    uint8_t d[32];
    char hex[65];
    cl_steps_digest(p, d);
    cl_hex(d, 8, hex);
    snprintf(acc.name, sizeof(acc.name), "op_%.16s", hex);
    omega_program_compute_id(&acc);
    *out = acc;
    return 0;
}

int cl_steps_differential(const ClSteps *p, const OmegaProgram *prog, const uint64_t *inputs, size_t n,
                          uint32_t *exec_count) {
    int bad = 0;
    for (size_t k = 0; k < n + CL_PROBE_COUNT; ++k) {
        uint64_t x = k < n ? inputs[k] : PROBES[k - n];
        uint64_t native = 0;
        if (omega_program_exec(prog, x, &native) != 0) return -1;
        if (exec_count) (*exec_count)++;
        if (native != cl_steps_eval(p, x)) bad++;
    }
    return bad;
}

/* ---- bank ----------------------------------------------------------- */

int cl_bank_add(ClBank *bank, const ClSteps *steps, ClOpOrigin origin, uint32_t op_ref) {
    if (bank->count >= CL_BANK_MAX || steps->n == 0) return -1;
    uint8_t d[32];
    cl_steps_digest(steps, d);
    for (size_t i = 0; i < bank->count; ++i)
        if (memcmp(bank->ops[i].digest, d, 32) == 0) return 1; /* already present */
    ClOp *op = &bank->ops[bank->count++];
    op->steps = *steps;
    memcpy(op->digest, d, 32);
    op->origin = (uint8_t)origin;
    op->op_ref = op_ref;
    return 0;
}

int cl_bank_init_base(ClBank *bank) {
    memset(bank, 0, sizeof(*bank));
    const OmegaSynthPrimDef *defs = NULL;
    size_t n = omega_synth_base_prim_defs(&defs);
    for (size_t i = 0; i < n; ++i) {
        ClSteps s;
        memset(&s, 0, sizeof(s));
        s.n = 1;
        s.s[0].op = (uint8_t)defs[i].op;
        s.s[0].imm = defs[i].imm;
        if (cl_bank_add(bank, &s, CL_OP_BASE, 0) < 0) return -1;
    }
    return 0;
}

void cl_bank_digest(const ClBank *bank, uint8_t out[CL_DIGEST_BYTES]) {
    uint8_t buf[CL_BANK_MAX * 33 + 4];
    ClWriter w;
    cl_w_init(&w, buf, sizeof(buf));
    cl_w_u32(&w, (uint32_t)bank->count);
    for (size_t i = 0; i < bank->count; ++i) {
        cl_w_bytes(&w, bank->ops[i].digest, 32);
        cl_w_u8(&w, bank->ops[i].origin);
    }
    cl_digest("CRUMBLINE-V1-BANK", buf, w.len, out);
}

/* ---- learner library ------------------------------------------------ */

int cl_library_init(ClLearnerLibrary *l) {
    memset(l, 0, sizeof(*l));
    return omega_library_init(&l->lib);
}

void cl_library_destroy(ClLearnerLibrary *l) { omega_library_destroy(&l->lib); }

int cl_library_admit(ClLearnerLibrary *l, const ClSteps *steps, uint8_t scope_bits, uint32_t op_ref) {
    if (!l || !steps || steps->n == 0 || l->lib.count >= OMEGA_LIB_MAX_PROGRAMS) return -1;
    OmegaProgram prog;
    if (cl_steps_build_program(steps, &prog) != 0) return -1;
    VerifyReport rep;
    int rc = -1;
    if (omega_program_verify(&prog, &rep) == 0 && cl_steps_differential(steps, &prog, NULL, 0, NULL) == 0) {
        uint8_t pre[4 + CL_DIGEST_BYTES], receipt[CL_DIGEST_BYTES];
        for (int k = 0; k < 4; ++k) pre[k] = (uint8_t)(op_ref >> (8 * k));
        cl_steps_digest(steps, pre + 4);
        cl_digest("CRUMBLINE-OMEGA-ADMISSION", pre, sizeof(pre), receipt);
        size_t slot = l->lib.count;
        if (omega_library_insert(&l->lib, &prog, NULL, 0, receipt) == 0) {
            l->steps[slot] = *steps;
            l->scope_bits[slot] = scope_bits;
            l->op_ref[slot] = op_ref;
            rc = 0;
        }
    }
    if (rc != 0) omega_program_destroy(&prog);
    return rc;
}

int cl_library_export(const ClLearnerLibrary *l, uint8_t input_bits, ClBank *bank) {
    int added = 0;
    for (size_t i = 0; i < l->lib.count; ++i) {
        if (l->scope_bits[i] < input_bits) continue; /* scope enforcement */
        if (cl_bank_add(bank, &l->steps[i], CL_OP_LIBRARY, l->op_ref[i]) == 0) added++;
    }
    return added;
}

/* ---- CPG1 ----------------------------------------------------------- */

uint8_t cl_cpg1_op(uint8_t omega_op) {
    switch (omega_op) {
        case OP_ADD: return 1;
        case OP_SUB: return 2;
        case OP_MUL: return 3;
        case OP_AND: return 4;
        case OP_OR: return 5;
        default: return 0;
    }
}

uint8_t cl_omega_op(uint8_t cpg1_op) {
    switch (cpg1_op) {
        case 1: return OP_ADD;
        case 2: return OP_SUB;
        case 3: return OP_MUL;
        case 4: return OP_AND;
        case 5: return OP_OR;
        default: return 0;
    }
}

int cl_steps_encode_cpg1(const ClSteps *p, ClWriter *w) {
    cl_w_bytes(w, "CPG1", 4);
    cl_w_u16(w, 1);
    cl_w_u8(w, p->n);
    for (uint8_t i = 0; i < p->n; ++i) {
        uint8_t op = cl_cpg1_op(p->s[i].op);
        if (op == 0) return -1;
        cl_w_u8(w, op);
        cl_w_u64(w, p->s[i].imm);
    }
    return w->overflow ? -1 : 0;
}

int cl_steps_decode_cpg1(ClReader *r, ClSteps *out) {
    memset(out, 0, sizeof(*out));
    uint8_t magic[4];
    cl_r_bytes(r, magic, 4);
    if (r->error || memcmp(magic, "CPG1", 4) != 0 || cl_r_u16(r) != 1) return -1;
    uint8_t n = cl_r_u8(r);
    if (r->error || n > CL_MAX_STEPS) return -1;
    out->n = n;
    for (uint8_t i = 0; i < n; ++i) {
        out->s[i].op = cl_omega_op(cl_r_u8(r));
        out->s[i].imm = cl_r_u64(r);
        if (out->s[i].op == 0) return -1;
    }
    return r->error ? -1 : 0;
}
