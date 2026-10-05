/* omega_program_ir.c -- see omega_program_ir.h. */
#include "omega_program_ir.h"

#include <stdlib.h>
#include <string.h>


typedef struct { uint8_t *p; size_t n, cap; int bad; } W;

static void wput(W *w, const void *d, size_t k)
{
    if (w->bad) return;
    if (w->n + k > w->cap) {
        size_t nc = (w->n + k) * 2 + 64;
        uint8_t *t = realloc(w->p, nc);
        if (!t) { w->bad = 1; return; }
        w->p = t;
        w->cap = nc;
    }
    memcpy(w->p + w->n, d, k);
    w->n += k;
}
static void wu8(W *w, uint8_t v) { wput(w, &v, 1); }
static void wu16(W *w, uint16_t v) { uint8_t t[2] = { (uint8_t)(v >> 8), (uint8_t)v }; wput(w, t, 2); }
static void wu64(W *w, uint64_t v)
{
    uint8_t t[8];
    for (int i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (56 - 8 * i));
    wput(w, t, 8);
}

int omega_program_ir_encode(const OmegaProgram *prog, uint8_t **out, size_t *len)
{
    if (!prog || !out || !len || !prog->body.has_body || prog->body.step_count > OMEGA_PROGRAM_MAX_STEPS) return -1;
    W w = { NULL, 0, 0, 0 };
    wput(&w, OMEGA_PROGRAM_IR_TAG, OMEGA_PROGRAM_IR_TAG_LEN);
    wu8(&w, (uint8_t)prog->contract.input_type);
    wu16(&w, prog->contract.input_width);
    wu8(&w, (uint8_t)prog->contract.output_type);
    wu16(&w, prog->contract.output_width);
    wput(&w, prog->contract.precondition_id.bytes, 32);
    wput(&w, prog->contract.postcondition_id.bytes, 32);
    wu16(&w, prog->body.step_count);
    for (uint16_t i = 0; i < prog->body.step_count; i++) {
        wu8(&w, prog->body.steps[i].op);
        wu64(&w, prog->body.steps[i].imm);
    }
    if (w.bad) { free(w.p); return -1; }
    *out = w.p;
    *len = w.n;
    return 0;
}

typedef struct { const uint8_t *b; size_t n, pos; int bad; } R;
static const uint8_t *rtake(R *r, size_t k)
{
    if (r->bad || r->n - r->pos < k) { r->bad = 1; return NULL; } /* VC1I:truncated */
    const uint8_t *p = r->b + r->pos;
    r->pos += k;
    return p;
}
static uint64_t rbe(R *r, size_t k)
{
    const uint8_t *p = rtake(r, k);
    uint64_t v = 0;
    if (!p) return 0;
    for (size_t i = 0; i < k; i++) v = v << 8 | p[i];
    return v;
}

int omega_program_ir_decode(const uint8_t *bytes, size_t len, OmegaProgram *out)
{
    if (!bytes || !out) return -1;
    memset(out, 0, sizeof *out);
    R r = { bytes, len, 0, 0 };
    const uint8_t *tag = rtake(&r, OMEGA_PROGRAM_IR_TAG_LEN);
    if (!tag || memcmp(tag, OMEGA_PROGRAM_IR_TAG, OMEGA_PROGRAM_IR_TAG_LEN) != 0) return -1; /* VC1I:tag */
    OmegaProgram *p = out;
    p->contract.input_type = (TypeTag)rbe(&r, 1);
    p->contract.input_width = (uint16_t)rbe(&r, 2);
    p->contract.output_type = (TypeTag)rbe(&r, 1);
    p->contract.output_width = (uint16_t)rbe(&r, 2);
    const uint8_t *pre = rtake(&r, 32), *post = rtake(&r, 32);
    uint64_t steps = rbe(&r, 2);
    if (r.bad || steps > OMEGA_PROGRAM_MAX_STEPS) { memset(out, 0, sizeof *out); return -1; } /* VC1I:steps */
    memcpy(p->contract.precondition_id.bytes, pre, 32);
    memcpy(p->contract.postcondition_id.bytes, post, 32);
    p->body.has_body = true;
    p->body.step_count = (uint16_t)steps;
    for (uint64_t i = 0; i < steps; i++) {
        p->body.steps[i].op = (uint8_t)rbe(&r, 1);
        p->body.steps[i].imm = rbe(&r, 8);
    }
    if (r.bad || r.pos != r.n) { memset(out, 0, sizeof *out); return -1; } /* VC1I:trailing */
    if (omega_program_compute_id(p) != 0) { memset(out, 0, sizeof *out); return -1; } /* VC1I:id */
    return 0;
}

int omega_program_ir_recompute_id(const uint8_t *bytes, size_t len, uint8_t id[32])
{
    OmegaProgram *p = malloc(sizeof *p);
    if (!p) return -1;
    int rc = omega_program_ir_decode(bytes, len, p);
    if (rc == 0) memcpy(id, p->program_id.bytes, 32);
    free(p);
    return rc;
}

