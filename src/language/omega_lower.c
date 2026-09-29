/* omega_lower.c -- Omega surface language V0 lowering. See omega_lower.h.
 *
 * Builds objects only through the existing omega_build_* / omega_program_*
 * builders. No evaluator lives here. */
#include "omega_lower.h"
#include "omega_core.h"
#include "omega_canonical.h"

#include <stdio.h>
#include <string.h>

#define V0_OVERFLOW OVERFLOW_WRAP   /* matches src/omega_realize.c for u64 ops */
#define V0_MAX_PROGRAM_STEPS 32
#define V0_MAX_PROGRAM_IMM 0xFFFFFFFFull  /* build_unary_op loads MOVZ + one MOVK */

typedef struct {
    OmegaGraph *g;
    const VisorBindings *b;
    const VisorBinding *last;
    const OmegaAst *ast;
    char *err;
    size_t n;
} Lower;

static int lfail(char *err, size_t n, uint32_t col, int rc, const char *msg) {
    if (err && n) snprintf(err, n, "column %u: %s", (unsigned)col, msg);
    return rc;
}

static bool same_type(const OmegaLangType *a, const OmegaLangType *b) {
    return a->tag == b->tag && a->width == b->width;
}

/* If the object just appended equals an earlier one, drop it and reuse the earlier. */
static OmegaObject *dedupe(OmegaGraph *g, OmegaObject *o) {
    if (!o || !g->object_count) return o;
    for (uint16_t i = 0; i + 1 < g->object_count; i++) {
        if (g->objects[i].has_id && omega_compare_semantic_id(&g->objects[i].id, &o->id) == 0) {
            g->object_count--;
            return &g->objects[i];
        }
    }
    return o;
}

static int capacity_fail(Lower *L, uint32_t col) {
    return lfail(L->err, L->n, col, -3, "the session graph is full (256 objects); use `clear` to start over");
}

static int build_type(Lower *L, const OmegaLangType *t, uint32_t col, SemanticId *out) {
    OmegaObject *o = (t->tag == TYPE_BOOL) ? omega_build_type_bool(L->g)
                                           : omega_build_type_uint(L->g, t->width);
    o = dedupe(L->g, o);
    if (!o || !o->has_id) return capacity_fail(L, col);
    *out = o->id;
    return 0;
}

/* Read the V0 type of an existing object: VALUE -> its type, APPLY -> its op's output type. */
static int type_of_object(const OmegaGraph *g, const SemanticId *id, OmegaLangType *out, SemanticId *type_id) {
    const OmegaObject *o = omega_graph_find_object_const(g, id);
    if (!o) return -1;
    SemanticId tid;
    if (o->kind == KIND_VALUE && o->payload_len == sizeof(ValuePayload)) {
        ValuePayload vp;
        memcpy(&vp, o->payload, sizeof vp);
        tid = vp.type_id;
    } else if (o->kind == KIND_OPERATION && o->payload_len == sizeof(ApplyPayload)) {
        ApplyPayload ap;
        memcpy(&ap, o->payload, sizeof ap);
        const OmegaObject *op = omega_graph_find_object_const(g, &ap.op_id);
        if (!op || op->kind != KIND_OPERATION || op->payload_len != sizeof(OperationPayload)) return -1;
        OperationPayload opp;
        memcpy(&opp, op->payload, sizeof opp);
        tid = opp.output_type;
    } else {
        return -1;
    }
    const OmegaObject *t = omega_graph_find_object_const(g, &tid);
    if (!t || t->kind != KIND_TYPE || t->payload_len != sizeof(TypePayload)) return -1;
    TypePayload tp;
    memcpy(&tp, t->payload, sizeof tp);
    if (tp.tag == TYPE_BOOL) { out->tag = TYPE_BOOL; out->width = 1; }
    else if (tp.tag == TYPE_UNSIGNED_INT &&
             (tp.width == 8 || tp.width == 16 || tp.width == 32 || tp.width == 64)) {
        out->tag = TYPE_UNSIGNED_INT; out->width = tp.width;
    } else {
        return -1;
    }
    if (type_id) *type_id = tid;
    return 0;
}

static int resolve_name(Lower *L, const OmegaAstNode *nd, SemanticId *id, OmegaLangType *ty) {
    const VisorBinding *vb = NULL;
    char msg[200];
    if (strcmp(nd->name, "_") == 0) {
        vb = L->last;
    } else if (L->b) {
        for (size_t i = 0; i < L->b->count && i < VISOR_MAX_BINDINGS; i++) {
            if (strcmp(L->b->items[i].name, nd->name) == 0) { vb = &L->b->items[i]; break; }
        }
    }
    if (!vb) {
        snprintf(msg, sizeof msg, "unknown name '%s' (define it first, e.g. let %s: u64 = 7)", nd->name, nd->name);
        return lfail(L->err, L->n, nd->col, -2, msg);
    }
    if (vb->kind == VISOR_BIND_PROGRAM) {
        snprintf(msg, sizeof msg, "'%s' is a program; using programs inside expressions is not supported in V0", nd->name);
        return lfail(L->err, L->n, nd->col, -2, msg);
    }
    if (vb->kind != VISOR_BIND_OBJECT) {
        snprintf(msg, sizeof msg, "'%s' is not a value (only integer and bool values can be used in V0)", nd->name);
        return lfail(L->err, L->n, nd->col, -2, msg);
    }
    if (type_of_object(L->g, &vb->id, ty, NULL) != 0) {
        snprintf(msg, sizeof msg, "'%s' does not name a V0 integer or bool value in this graph", nd->name);
        return lfail(L->err, L->n, nd->col, -2, msg);
    }
    *id = vb->id;
    return 0;
}

static bool is_comparison(OmegaTokKind k) {
    return k == OTOK_EQEQ || k == OTOK_NE || k == OTOK_LT || k == OTOK_LE || k == OTOK_GT || k == OTOK_GE;
}

static int comparison_fail(Lower *L, const OmegaAstNode *nd) {
    return lfail(L->err, L->n, nd->col, -2,
                 "comparisons are not supported in V0 expressions (the existing builder would type the result "
                 "as the operand type, not bool)");
}

/* 1 = type known, 0 = only untyped literals, <0 = error. */
static int infer(Lower *L, int idx, OmegaLangType *out) {
    const OmegaAstNode *nd = &L->ast->nodes[idx];
    switch (nd->kind) {
    case OAST_INT: return 0;
    case OAST_BOOL: out->tag = TYPE_BOOL; out->width = 1; return 1;
    case OAST_ASCRIBE: *out = nd->type; return 1;
    case OAST_NAME: {
        SemanticId id;
        int rc = resolve_name(L, nd, &id, out);
        return rc ? rc : 1;
    }
    case OAST_BINARY: {
        if (is_comparison(nd->op)) return comparison_fail(L, nd);
        int r = infer(L, nd->lhs, out);
        if (r != 0) return r;
        return infer(L, nd->rhs, out);
    }
    }
    return lfail(L->err, L->n, nd->col, -1, "internal: unknown syntax node");
}

static OpCode opcode_of(OmegaTokKind k) {
    switch (k) {
    case OTOK_PLUS: return OP_ADD;
    case OTOK_MINUS: return OP_SUB;
    case OTOK_STAR: return OP_MUL;
    case OTOK_SLASH: return OP_DIV;
    case OTOK_AMP: return OP_AND;
    case OTOK_PIPE: return OP_OR;
    default: return OP_INVALID;
    }
}

static int mismatch(Lower *L, uint32_t col, const char *what, const OmegaLangType *have, const OmegaLangType *want) {
    char msg[200];
    snprintf(msg, sizeof msg, "%s is %s but %s is required here (V0 never converts between types)",
             what, omega_language_type_text(have), omega_language_type_text(want));
    return lfail(L->err, L->n, col, -2, msg);
}

/* Is node idx a literal (possibly ascribed)? Returns its value. */
static bool literal_value(const OmegaAst *a, int idx, uint64_t *v) {
    const OmegaAstNode *nd = &a->nodes[idx];
    if (nd->kind == OAST_ASCRIBE) nd = &a->nodes[nd->lhs];
    if (nd->kind != OAST_INT) return false;
    *v = nd->value;
    return true;
}

static int lower_expr(Lower *L, int idx, const OmegaLangType *expected,
                      SemanticId *out_id, OmegaLangType *out_ty, SemanticId *out_tid) {
    const OmegaAstNode *nd = &L->ast->nodes[idx];
    int rc;
    char msg[200];
    switch (nd->kind) {
    case OAST_INT: {
        if (!expected)
            return lfail(L->err, L->n, nd->col, -2,
                         "ambiguous width: a bare number has no type; write it as `7: u64` or `(7: u64)`");
        if (expected->tag != TYPE_UNSIGNED_INT)
            return lfail(L->err, L->n, nd->col, -2, "a number is used where a bool is required");
        if (expected->width < 64 && (nd->value >> expected->width) != 0) {
            snprintf(msg, sizeof msg, "literal %llu does not fit in %s",
                     (unsigned long long)nd->value, omega_language_type_text(expected));
            return lfail(L->err, L->n, nd->col, -2, msg);
        }
        SemanticId tid;
        if ((rc = build_type(L, expected, nd->col, &tid))) return rc;
        OmegaObject *v = dedupe(L->g, omega_build_val_uint(L->g, &tid, expected->width, nd->value));
        if (!v || !v->has_id) return capacity_fail(L, nd->col);
        *out_id = v->id; *out_ty = *expected; *out_tid = tid;
        return 0;
    }
    case OAST_BOOL: {
        OmegaLangType bt = { TYPE_BOOL, 1 };
        if (expected && !same_type(expected, &bt)) return mismatch(L, nd->col, "this value", &bt, expected);
        SemanticId tid;
        if ((rc = build_type(L, &bt, nd->col, &tid))) return rc;
        OmegaObject *v = dedupe(L->g, omega_build_val_bool(L->g, &tid, nd->bval));
        if (!v || !v->has_id) return capacity_fail(L, nd->col);
        *out_id = v->id; *out_ty = bt; *out_tid = tid;
        return 0;
    }
    case OAST_NAME: {
        SemanticId id;
        OmegaLangType ty;
        if ((rc = resolve_name(L, nd, &id, &ty))) return rc;
        if (expected && !same_type(expected, &ty)) {
            snprintf(msg, sizeof msg, "'%s'", nd->name);
            return mismatch(L, nd->col, msg, &ty, expected);
        }
        SemanticId tid;
        if (type_of_object(L->g, &id, &ty, &tid) != 0)
            return lfail(L->err, L->n, nd->col, -2, "internal: binding type vanished");
        *out_id = id; *out_ty = ty; *out_tid = tid;
        return 0;
    }
    case OAST_ASCRIBE:
        if (expected && !same_type(expected, &nd->type))
            return mismatch(L, nd->col, "this annotation", &nd->type, expected);
        return lower_expr(L, nd->lhs, &nd->type, out_id, out_ty, out_tid);
    case OAST_BINARY: {
        if (is_comparison(nd->op)) return comparison_fail(L, nd);
        OpCode opc = opcode_of(nd->op);
        if (opc == OP_INVALID) return lfail(L->err, L->n, nd->col, -1, "internal: unknown operator");
        OmegaLangType t;
        if (expected) {
            t = *expected;
        } else {
            int r = infer(L, idx, &t);
            if (r < 0) return r;
            if (r == 0)
                return lfail(L->err, L->n, nd->col, -2,
                             "ambiguous width: no operand has a type; write e.g. `(1: u64) + 2`");
        }
        if (t.tag != TYPE_UNSIGNED_INT) {
            snprintf(msg, sizeof msg, "'%s' on bool is not supported in V0 (integers only)",
                     omega_language_tok_text(nd->op));
            return lfail(L->err, L->n, nd->col, -2, msg);
        }
        uint64_t dv;
        if (opc == OP_DIV && literal_value(L->ast, nd->rhs, &dv) && dv == 0)
            return lfail(L->err, L->n, nd->col, -2, "division by the constant 0 is undefined");
        SemanticId lid, rid, ltid, rtid, tid;
        OmegaLangType lt, rt;
        if ((rc = lower_expr(L, nd->lhs, &t, &lid, &lt, &ltid))) return rc;
        if ((rc = lower_expr(L, nd->rhs, &t, &rid, &rt, &rtid))) return rc;
        if ((rc = build_type(L, &t, nd->col, &tid))) return rc;
        OmegaObject *op = dedupe(L->g, omega_build_op_binary(L->g, opc, V0_OVERFLOW, &tid));
        if (!op || !op->has_id) return capacity_fail(L, nd->col);
        SemanticId op_id = op->id;
        OmegaObject *ap = dedupe(L->g, omega_build_apply(L->g, &op_id, &lid, &rid));
        if (!ap || !ap->has_id) return capacity_fail(L, nd->col);
        *out_id = ap->id; *out_ty = t; *out_tid = tid;
        return 0;
    }
    }
    return lfail(L->err, L->n, nd->col, -1, "internal: unknown syntax node");
}

int omega_language_lower_to_graph(OmegaGraph *g, const VisorBindings *b, const VisorBinding *last,
                                  const OmegaAst *ast, SemanticId *out_id, OmegaLangResult *info,
                                  char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    if (!g || !ast || !out_id) return lfail(err, err_len, 0, -1, "internal: bad lowering arguments");
    if (ast->stmt == OSTMT_FN)
        return lfail(err, err_len, ast->stmt_col, -2, "fn lowers to a program, not a graph object");
    if ((ast->stmt != OSTMT_EXPR && ast->stmt != OSTMT_LET) || ast->root < 0 ||
        (size_t)ast->root >= ast->node_count)
        return lfail(err, err_len, ast->stmt_col, -1, "nothing to lower");
    Lower L = { g, b, last, ast, err, err_len };
    uint16_t saved = g->object_count;
    SemanticId id, tid;
    OmegaLangType ty;
    int rc = lower_expr(&L, ast->root, ast->stmt == OSTMT_LET ? &ast->let_type : NULL, &id, &ty, &tid);
    if (rc) {
        if (g->object_count > saved)
            memset(&g->objects[saved], 0, (size_t)(g->object_count - saved) * sizeof(OmegaObject));
        g->object_count = saved;
        return rc;
    }
    *out_id = id;
    if (info) {
        info->kind = ast->stmt == OSTMT_LET ? OMEGA_LANG_BINDING : OMEGA_LANG_EXPRESSION;
        info->id = id;
        info->type_id = tid;
        snprintf(info->type_text, sizeof info->type_text, "%s", omega_language_type_text(&ty));
    }
    return 0;
}

int omega_language_lower_program(const OmegaAst *ast, OmegaProgram *out, char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    if (!ast || !out) return lfail(err, err_len, 0, -1, "internal: bad lowering arguments");
    if (ast->stmt != OSTMT_FN || ast->root < 0)
        return lfail(err, err_len, ast->stmt_col, -1, "not a fn definition");
    const OmegaLangType u64 = { TYPE_UNSIGNED_INT, 64 };
    if (!same_type(&ast->param_type, &u64) || !same_type(&ast->ret_type, &u64))
        return lfail(err, err_len, ast->stmt_col, -2,
                     "V0 functions must be (x: u64) -> u64 (the existing program model is fixed at 64-bit)");

    struct { OpCode op; uint64_t imm; } steps[V0_MAX_PROGRAM_STEPS];
    size_t count = 0;
    int idx = ast->root;
    char msg[200];
    while (1) {
        const OmegaAstNode *nd = &ast->nodes[idx];
        if (nd->kind == OAST_ASCRIBE) {
            if (!same_type(&nd->type, &u64))
                return lfail(err, err_len, nd->col, -2, "inside a V0 function everything is u64");
            idx = nd->lhs;
            continue;
        }
        if (nd->kind != OAST_BINARY) break;
        if (is_comparison(nd->op))
            return lfail(err, err_len, nd->col, -2, "comparisons are not supported in V0 function bodies");
        if (nd->op == OTOK_SLASH)
            return lfail(err, err_len, nd->col, -2,
                         "'/' is not in the program model's operation set (+ - * & |)");
        OpCode opc = opcode_of(nd->op);
        bool commutative = (opc == OP_ADD || opc == OP_MUL || opc == OP_AND || opc == OP_OR);
        uint64_t imm;
        int next_idx;
        const OmegaAstNode *r = &ast->nodes[nd->rhs], *l = &ast->nodes[nd->lhs];
        if ((r->kind == OAST_ASCRIBE && !same_type(&r->type, &u64)) ||
            (l->kind == OAST_ASCRIBE && !same_type(&l->type, &u64)))
            return lfail(err, err_len, nd->col, -2, "inside a V0 function everything is u64");
        if (literal_value(ast, nd->rhs, &imm)) next_idx = nd->lhs;
        else if (commutative && literal_value(ast, nd->lhs, &imm)) next_idx = nd->rhs;
        else
            return lfail(err, err_len, nd->col, -2,
                         "function body must be a chain like (x * 2) + 1: each step applies + - * & | "
                         "to the running value and one constant");
        if (imm > V0_MAX_PROGRAM_IMM) {
            snprintf(msg, sizeof msg,
                     "constant %llu is wider than 32 bits; the program builder can only load 32-bit constants",
                     (unsigned long long)imm);
            return lfail(err, err_len, nd->col, -2, msg);
        }
        if (count >= V0_MAX_PROGRAM_STEPS)
            return lfail(err, err_len, nd->col, -3, "function body has more than 32 steps");
        steps[count].op = opc;
        steps[count].imm = imm;
        count++;
        idx = next_idx;
    }
    const OmegaAstNode *leaf = &ast->nodes[idx];
    if (leaf->kind == OAST_NAME && strcmp(leaf->name, ast->name) == 0)
        return lfail(err, err_len, leaf->col, -2, "recursion is not supported in V0");
    if (leaf->kind != OAST_NAME || strcmp(leaf->name, ast->param) != 0)
        return lfail(err, err_len, leaf->col, -2,
                     "function body must start from its parameter and use only constants "
                     "(no other names, no constant-only bodies)");
    if (count == 0)
        return lfail(err, err_len, leaf->col, -2,
                     "function body must apply at least one operation to the parameter");

    /* steps[] is outermost-first; build innermost-first. */
    OmegaProgram acc, step, tmp;
    if (omega_program_build_unary_op(&acc, ast->name, steps[count - 1].op, steps[count - 1].imm) != 0)
        return lfail(err, err_len, ast->stmt_col, -2, "program builder rejected the first step");
    for (size_t i = count - 1; i-- > 0;) {
        if (omega_program_build_unary_op(&step, ast->name, steps[i].op, steps[i].imm) != 0)
            return lfail(err, err_len, ast->stmt_col, -2, "program builder rejected a step");
        char cerr[160] = {0};
        if (omega_program_compose(&acc, &step, &tmp, cerr, sizeof cerr) != 0) {
            snprintf(msg, sizeof msg, "program composition failed: %.150s", cerr);
            return lfail(err, err_len, ast->stmt_col, strstr(cerr, "overflow") ? -3 : -2, msg);
        }
        acc = tmp;
    }
    snprintf(acc.name, sizeof acc.name, "%s", ast->name);
    snprintf(acc.contract.precondition, sizeof acc.contract.precondition, "%s", ast->requires_text);
    snprintf(acc.contract.postcondition, sizeof acc.contract.postcondition, "%s", ast->ensures_text);
    if (omega_build_constraint_id(CONST_PRECONDITION, acc.contract.precondition, &acc.contract.precondition_id) ||
        omega_build_constraint_id(CONST_POSTCONDITION, acc.contract.postcondition, &acc.contract.postcondition_id) ||
        omega_program_compute_id(&acc))
        return lfail(err, err_len, ast->stmt_col, -2, "could not compute the program's ids");
    *out = acc;
    return 0;
}

static bool binding_slot_available(const VisorSession *s, const char *name) {
    for (size_t i = 0; i < s->bindings.count; i++)
        if (strcmp(s->bindings.items[i].name, name) == 0) return true;
    return s->bindings.count < VISOR_MAX_BINDINGS;
}

int omega_language_eval_line(VisorSession *s, const char *line, OmegaLangResult *out,
                             char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    if (!out) return lfail(err, err_len, 0, -1, "internal: no result slot");
    memset(out, 0, sizeof(*out));
    out->kind = OMEGA_LANG_NONE;
    out->program_index = -1;
    if (!s || !s->graph || !line) return lfail(err, err_len, 0, -1, "internal: no session");

    OmegaAst ast;
    int rc = omega_language_parse_line(line, &ast, err, err_len);
    if (rc) return rc;

    switch (ast.stmt) {
    case OSTMT_EMPTY:
        return 0;
    case OSTMT_EXPR:
        return omega_language_lower_to_graph(s->graph, &s->bindings, s->has_last ? &s->last : NULL,
                                             &ast, &out->id, out, err, err_len);
    case OSTMT_LET: {
        if (!binding_slot_available(s, ast.name))
            return lfail(err, err_len, ast.stmt_col, -3, "binding table is full (64 names)");
        uint16_t saved = s->graph->object_count;
        OmegaLangResult r;
        memset(&r, 0, sizeof r);
        rc = omega_language_lower_to_graph(s->graph, &s->bindings, s->has_last ? &s->last : NULL,
                                           &ast, &r.id, &r, err, err_len);
        if (rc) return rc;
        if (visor_binding_set(s, ast.name, VISOR_BIND_OBJECT, &r.id, -1) != 0) {
            s->graph->object_count = saved;
            return lfail(err, err_len, ast.stmt_col, -2, "the session refused this binding name");
        }
        *out = r;
        out->kind = OMEGA_LANG_BINDING;
        out->program_index = -1;
        snprintf(out->name, sizeof out->name, "%s", ast.name);
        return 0;
    }
    case OSTMT_FN: {
        if (!binding_slot_available(s, ast.name))
            return lfail(err, err_len, ast.stmt_col, -3, "binding table is full (64 names)");
        OmegaProgram prog;
        rc = omega_language_lower_program(&ast, &prog, err, err_len);
        if (rc) return rc;
        /* The program id (v2, spec/program-identity.md) binds the canonical body
         * and the contract, not the name: an equal id is the same program and
         * is reused (same rule as graph dedupe), even under another name. A
         * different body gets a different id, so redefining a name simply adds
         * the new program and rebinds the name. Equal id with different code
         * would mean the builder emitted two realizations for one body: an
         * internal invariant violation, refused. */
        int pidx = -1;
        for (size_t i = 0; i < s->program_count; i++) {
            const OmegaProgram *q = &s->programs[i];
            if (omega_compare_semantic_id(&q->program_id, &prog.program_id) != 0) continue;
            if (q->realization.code_len != prog.realization.code_len ||
                memcmp(q->realization.code_bytes, prog.realization.code_bytes, prog.realization.code_len) != 0)
                return lfail(err, err_len, ast.stmt_col, -2,
                             "internal: program id matches an existing program but its realization differs "
                             "(builder invariant violated)");
            pidx = (int)i;
            break;
        }
        bool added = false;
        if (pidx < 0) {
            if (s->program_count >= VISOR_MAX_PROGRAMS)
                return lfail(err, err_len, ast.stmt_col, -3, "program table is full (16 programs)");
            pidx = visor_program_add(s, &prog);
            added = true;
        }
        if (pidx < 0) return lfail(err, err_len, ast.stmt_col, -3, "program table is full (16 programs)");
        if (visor_binding_set(s, ast.name, VISOR_BIND_PROGRAM, &prog.program_id, pidx) != 0) {
            if (added) {
                s->program_count--;
                memset(&s->programs[s->program_count], 0, sizeof(OmegaProgram));
            }
            return lfail(err, err_len, ast.stmt_col, -2, "the session refused this program name");
        }
        out->kind = OMEGA_LANG_PROGRAM;
        out->id = prog.program_id;
        out->program_index = pidx;
        snprintf(out->name, sizeof out->name, "%s", ast.name);
        snprintf(out->type_text, sizeof out->type_text, "u64 -> u64");
        return 0;
    }
    }
    return lfail(err, err_len, 0, -1, "internal: unknown statement");
}
