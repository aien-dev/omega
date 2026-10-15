/*
 * typediff.c -- differential test input for omega_emit (omega#445): for every op of the op-list schema
 * and every combination of operand types from a fixed list, builds a one-op unit in memory, asks the
 * OSC IR validator and decoder (src/compiler/osc_ir.c) whether they accept it, and writes the same unit
 * as an op-list for omega_emit:
 *   typediff <outdir>
 * writes <outdir>/<id>.json for every case and <outdir>/<id>.oscir (canonical bytes) for every case
 * the decoder accepts, and prints one line per case: "<id> ACCEPT|REFUSE".
 * test_omega_emit.sh then requires: omega_emit emits every ACCEPT case byte-identically (and lift ->
 * emit of its bytes is byte-identical), and refuses every REFUSE case. The unit: optional callee "g"
 * (for call), then "f" whose params are the read operands (so every read register is written) and
 * whose locals are the written operands; one block: the op, then ret.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osc_ir.h"

typedef struct { const char *name; OscType t; } Ty;
#define SC(s) {OSC_T_##s, OSC_REF_NONE, OSC_T_VOID, 0, 0}
#define RF(k, e, n) {OSC_T_REF, OSC_REF_##k, OSC_T_##e, n, 0}
static const Ty TY[] = {
    {"bool", SC(BOOL)}, {"u8", SC(U8)}, {"i32", SC(I32)}, {"u64", SC(U64)},
    {"own:u32:1", RF(OWN, U32, 1)}, {"shared:u32:1", RF(SHARED, U32, 1)}, {"mut:u32:1", RF(MUT, U32, 1)},
    {"own:u32:2", RF(OWN, U32, 2)}, {"shared:u8:1", RF(SHARED, U8, 1)}, {"u32", SC(U32)},
};
#define NT ((int)(sizeof TY / sizeof TY[0]))

static OscUnit U;
static const char *g_out;
static int g_n;

typedef struct {
    const char *op;            /* schema op name */
    uint8_t code, sub;
    int nread, write;          /* read operand registers (params), written register (local) */
    const char *rkey[3];       /* JSON keys of the read operands, in order */
} Shape;

static void put_type(FILE *f, const Ty *t) { fprintf(f, "\"%s\"", t->name); }

/* one case: types[0..nread-1] = read operands, wt = written type or -1; extra = op-specific */
static void emit_case(const Shape *s, const int *rt, int wt, int pt, int rett, int fret)
{
    memset(&U, 0, sizeof U);
    int fi = 0;
    char id[96];
    snprintf(id, sizeof id, "%s", s->op);
    for (int k = 0; k < s->nread; k++) snprintf(id + strlen(id), sizeof id - strlen(id), "_%d", rt[k]);
    if (wt >= 0) snprintf(id + strlen(id), sizeof id - strlen(id), "_w%d", wt);
    if (pt >= 0) snprintf(id + strlen(id), sizeof id - strlen(id), "_p%d", pt);
    if (rett >= 0) snprintf(id + strlen(id), sizeof id - strlen(id), "_r%d", rett);
    if (fret >= 0) snprintf(id + strlen(id), sizeof id - strlen(id), "_f%d", fret);
    char path[512];
    snprintf(path, sizeof path, "%s/%s.json", g_out, id);
    FILE *j = fopen(path, "w");
    if (!j) { perror(path); exit(2); }
    fprintf(j, "{\"functions\": [");
    if (!strcmp(s->op, "call")) {
        /* g(p: TY[pt]) -> TY[rett] or void: one block returning a fresh constant of its return type */
        OscFunc *g = &U.funcs[0];
        strcpy(g->name, "g");
        g->nparams = 1; g->vtype[0] = TY[pt].t; g->nvregs = 1;
        g->ret.s = OSC_T_VOID;
        fprintf(j, "{\"name\": \"g\", \"params\": ["); put_type(j, &TY[pt]); fprintf(j, "], \"ret\": ");
        if (rett >= 0) {
            g->ret = TY[rett].t; g->vtype[1] = TY[rett].t; g->nvregs = 2;
            g->insns[0] = (OscInsn){.op = OSC_I_CONST, .dst = 1, .a = -1, .b = -1, .c = -1, .blk_t = -1, .blk_f = -1, .callee = -1, .imm = 0};
            g->insns[1] = (OscInsn){.op = OSC_I_RET, .dst = -1, .a = 1, .b = -1, .c = -1, .blk_t = -1, .blk_f = -1, .callee = -1};
            g->ninsns = 2;
            put_type(j, &TY[rett]);
            fprintf(j, ", \"locals\": ["); put_type(j, &TY[rett]);
            fprintf(j, "], \"blocks\": [[{\"op\": \"const\", \"dst\": 1, \"value\": %s}, {\"op\": \"ret\", \"a\": 1}]]}, ",
                    TY[rett].t.s == OSC_T_BOOL ? "false" : "0");
        } else {
            g->insns[0] = (OscInsn){.op = OSC_I_RET, .dst = -1, .a = -1, .b = -1, .c = -1, .blk_t = -1, .blk_f = -1, .callee = -1};
            g->ninsns = 1;
            fprintf(j, "\"void\", \"locals\": [], \"blocks\": [[{\"op\": \"ret\"}]]}, ");
        }
        g->nblocks = 1; g->blocks[0] = (OscBlock){0, g->ninsns};
        fi = 1;
    }
    OscFunc *f = &U.funcs[fi];
    U.nfuncs = (uint16_t)(fi + 1);
    strcpy(f->name, "f");
    f->nparams = (uint8_t)s->nread;
    for (int k = 0; k < s->nread; k++) f->vtype[k] = TY[rt[k]].t;
    f->nvregs = (uint16_t)s->nread;
    int dst = -1;
    if (wt >= 0) { dst = f->nvregs; f->vtype[f->nvregs++] = TY[wt].t; }
    f->ret.s = OSC_T_VOID;
    if (fret >= 0) f->ret = TY[fret].t;
    OscInsn in = {.op = s->code, .sub = s->sub, .dst = (int16_t)dst, .a = -1, .b = -1, .c = -1, .blk_t = -1, .blk_f = -1, .callee = -1};
    int16_t *slot[3] = {&in.a, &in.b, &in.c};
    for (int k = 0; k < s->nread; k++) *slot[k] = (int16_t)k;
    if (s->code == OSC_I_CALL) { in.callee = 0; in.nargs = 1; in.args[0] = 0; in.a = -1; }
    if (s->code == OSC_I_CBR) { in.blk_t = 1; in.blk_f = 1; }
    f->insns[0] = in;
    int n = 1;
    if (s->code != OSC_I_RET && s->code != OSC_I_CBR) {
        f->insns[n++] = (OscInsn){.op = OSC_I_RET, .dst = -1, .a = -1, .b = -1, .c = -1, .blk_t = -1, .blk_f = -1, .callee = -1};
    }
    f->ninsns = (uint32_t)n;
    f->blocks[0] = (OscBlock){0, (uint32_t)n};
    f->nblocks = 1;
    if (s->code == OSC_I_CBR) {
        f->insns[1] = (OscInsn){.op = OSC_I_RET, .dst = -1, .a = -1, .b = -1, .c = -1, .blk_t = -1, .blk_f = -1, .callee = -1};
        f->ninsns = 2; f->blocks[0] = (OscBlock){0, 1}; f->blocks[1] = (OscBlock){1, 1}; f->nblocks = 2;
    }
    /* JSON of f */
    fprintf(j, "{\"name\": \"f\", \"params\": [");
    for (int k = 0; k < s->nread; k++) { if (k) fprintf(j, ", "); put_type(j, &TY[rt[k]]); }
    fprintf(j, "], \"ret\": ");
    if (fret >= 0) put_type(j, &TY[fret]); else fprintf(j, "\"void\"");
    fprintf(j, ", \"locals\": [");
    if (wt >= 0) put_type(j, &TY[wt]);
    fprintf(j, "], \"blocks\": [[{\"op\": \"%s\"", s->op);
    if (s->code == OSC_I_CONST) fprintf(j, ", \"dst\": %d, \"value\": 0", dst);
    else if (s->code == OSC_I_CALL) {
        fprintf(j, ", \"func\": \"g\", \"args\": [0]");
        if (wt >= 0) fprintf(j, ", \"dst\": %d", dst);
    } else {
        if (wt >= 0) fprintf(j, ", \"dst\": %d", dst);
        for (int k = 0; k < s->nread; k++) fprintf(j, ", \"%s\": %d", s->rkey[k], k);
    }
    if (s->code == OSC_I_CBR) fprintf(j, ", \"then\": 1, \"else\": 1}], [{\"op\": \"ret\"}]]}");
    else if (s->code == OSC_I_RET) fprintf(j, "}]]}");
    else fprintf(j, "}, {\"op\": \"ret\"}]]}");
    fprintf(j, "]}\n");
    fclose(j);

    static uint8_t buf[OSC_IR_ENCODED_MAX];
    static OscUnit back;
    size_t len = 0;
    int ok = !osc_ir_encode(&U, buf, sizeof buf, &len) && !osc_ir_decode(buf, len, &back);
    if (ok) {
        snprintf(path, sizeof path, "%s/%s.oscir", g_out, id);
        FILE *b = fopen(path, "wb");
        if (!b || fwrite(buf, 1, len, b) != len || fclose(b)) { perror(path); exit(2); }
    }
    printf("%s %s\n", id, ok ? "ACCEPT" : "REFUSE");
    g_n++;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: typediff <outdir>\n"); return 2; }
    g_out = argv[1];
    static const Shape S[] = {
        {"mov", OSC_I_MOV, 0, 1, 1, {"a"}},
        {"add", OSC_I_BIN, OSC_B_ADD, 2, 1, {"a", "b"}}, {"div", OSC_I_BIN, OSC_B_DIV, 2, 1, {"a", "b"}},
        {"and", OSC_I_BIN, OSC_B_AND, 2, 1, {"a", "b"}}, {"shl", OSC_I_BIN, OSC_B_SHL, 2, 1, {"a", "b"}},
        {"neg", OSC_I_UN, OSC_U_NEG, 1, 1, {"a"}}, {"bnot", OSC_I_UN, OSC_U_BNOT, 1, 1, {"a"}},
        {"not", OSC_I_UN, OSC_U_LNOT, 1, 1, {"a"}},
        {"eq", OSC_I_CMP, OSC_C_EQ, 2, 1, {"a", "b"}}, {"lt", OSC_I_CMP, OSC_C_LT, 2, 1, {"a", "b"}},
        {"cast", OSC_I_CAST, 0, 1, 1, {"a"}},
        {"alloc", OSC_I_ALLOC, 0, 1, 1, {"init"}},
        {"release", OSC_I_RELEASE, 0, 1, 0, {"arr"}},
        {"load", OSC_I_LOAD, 0, 2, 1, {"arr", "index"}},
        {"store", OSC_I_STORE, 0, 3, 0, {"arr", "index", "value"}},
        {"cbr", OSC_I_CBR, 0, 1, 0, {"cond"}},
        {"ret", OSC_I_RET, 0, 1, 0, {"a"}},
    };
    for (size_t si = 0; si < sizeof S / sizeof S[0]; si++) {
        const Shape *s = &S[si];
        int rt[3] = {0, 0, 0};
        int nw = s->write ? NT : 1;
        int total = 1;
        for (int k = 0; k < s->nread; k++) total *= NT;
        for (int c = 0; c < total; c++) {
            int x = c;
            for (int k = 0; k < s->nread; k++) { rt[k] = x % NT; x /= NT; }
            if (s->code == OSC_I_RET) {
                for (int fr = 0; fr < NT; fr++) if (TY[fr].t.s != OSC_T_REF) emit_case(s, rt, -1, -1, -1, fr);
                continue;
            }
            for (int w = 0; w < nw; w++) emit_case(s, rt, s->write ? w : -1, -1, -1, -1);
        }
    }
    /* const: every destination type */
    static const Shape K = {"const", OSC_I_CONST, 0, 0, 1, {0}};
    for (int w = 0; w < NT; w++) {
        /* JSON value 0 / false; refs must be refused by both */
        int rt[3] = {0};
        emit_case(&K, rt, w, -1, -1, -1);
    }
    /* call: parameter type x argument type, and return type x dst type (or void) */
    static const Shape C = {"call", OSC_I_CALL, 0, 1, 1, {0}};
    static const Shape CV = {"call", OSC_I_CALL, 0, 1, 0, {0}};
    for (int p = 0; p < NT; p++)
        for (int a = 0; a < NT; a++) {
            int rt[3] = {a};
            emit_case(&CV, rt, -1, p, -1, -1);
        }
    for (int r = 0; r < NT; r++) {
        if (TY[r].t.s == OSC_T_REF) continue;
        for (int w = 0; w < NT; w++) {
            int rt[3] = {9};  /* argument u32 to a u32 parameter */
            emit_case(&C, rt, w, 9, r, -1);
        }
    }
    fprintf(stderr, "typediff: %d cases\n", g_n);
    return 0;
}
