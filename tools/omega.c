/*
 * omega -- Omega Visor V1 terminal console.
 *
 *   omega [--json] [--command "<line>"]... [--script <file>|-] [--evidence-root <dir>]
 *
 * No arguments: interactive REPL on stdin. Exit code: 0 if every command was
 * ok, 1 if any command errored, 2 on usage error.
 *
 * The console (src/visor/visor_console.c) only parses, classifies and prints.
 * This file is the ONE place where the console's hooks are wired to the other
 * lanes (semantic, language, machine/realization, world, verify/evidence,
 * effect requests). Nothing here mints capabilities, publishes, promotes or
 * submits effects: the only execution path is visor_realization_run_pure,
 * reached only after the effect classifier says the subject is pure.
 *
 * Define OMEGA_TOOL_NO_MAIN to include this file for tests without main().
 */
#include "visor.h"
#include "visor_console.h"
#include "visor_semantic.h"
#include "visor_machine.h"
#include "visor_realization.h"
#include "visor_world.h"
#include "visor_verify.h"
#include "visor_evidence.h"
#include "visor_effect_request.h"
#include "omega_lower.h"
#include "omega_core.h"
#include "omega_canonical.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------- shared scratch storage (single-threaded; views are large) ---------------- */

#define OM_BUF_LEN 65536
static char om_buf[OM_BUF_LEN];
static char om_buf2[OM_BUF_LEN];
static VisorObjectView om_obj;
static VisorRealizationView om_rv, om_rv2;
static VisorRealizationView om_alt_views[4];
static VisorRealizationEntry om_alt_entries[4];
static VisorMachineView om_mv;
static VisorWorldView om_wv;
static VisorCostView om_cv;
static VisorVerifyReport om_vr;
static VisorEvidenceView om_ev;
static VisorEffectRequest om_er;

#define OM_FMT(call, text_fn, json_fn, view, buf) \
    ((call)->json ? json_fn((view), (buf), sizeof(buf)) : text_fn((view), (buf), sizeof(buf)))

/* Formatters return bytes written (or 0) on success, -1 when the output did not fit. */
static int om_put(FILE *out, int rc, const char *buf, char *err, size_t errn) {
    if (rc < 0) { snprintf(err, errn, "output too large"); return 1; }
    fputs(buf, out);
    return 0;
}

static void om_id(const SemanticId *id, char out[72]) { visor_format_id(id, out); }

static const char *om_type_name(TypeTag t, uint16_t w, char *buf, size_t n) {
    if (t == TYPE_UNSIGNED_INT) snprintf(buf, n, "u%u", (unsigned)w);
    else if (t == TYPE_BOOL) snprintf(buf, n, "bool");
    else snprintf(buf, n, "type(0x%02x,%u)", (unsigned)t, (unsigned)w);
    return buf;
}

/* ---------------- session lookups ---------------- */

static VisorRealizationEntry *om_entry(VisorSession *s, const VisorBinding *b) {
    if (!b || b->kind != VISOR_BIND_REALIZATION || b->index < 0 || (size_t)b->index >= s->realization_count) return NULL;
    return &s->realizations[b->index];
}

static OmegaProgram *om_program(VisorSession *s, const VisorBinding *b) {
    if (!b || b->kind != VISOR_BIND_PROGRAM || b->index < 0 || (size_t)b->index >= s->program_count) return NULL;
    return &s->programs[b->index];
}

/* First (earliest) session realization whose subject is `id`, or -1. The first
 * one is the one `realize`/`run` made, not an alternative added later. */
static int om_find_real_for(const VisorSession *s, const SemanticId *id) {
    for (size_t i = 0; i < s->realization_count; i++)
        if (omega_compare_semantic_id(&s->realizations[i].subject_id, id) == 0) return (int)i;
    return -1;
}

/* Add an entry, reusing an identical realization already in the session. */
static int om_add_entry(VisorSession *s, const VisorRealizationEntry *e) {
    for (size_t i = 0; i < s->realization_count; i++) {
        const VisorRealizationEntry *q = &s->realizations[i];
        if (omega_compare_semantic_id(&q->real.realization_id, &e->real.realization_id) == 0 &&
            omega_compare_semantic_id(&q->subject_id, &e->subject_id) == 0 &&
            strcmp(q->target_name, e->target_name) == 0)
            return (int)i;
    }
    return visor_realization_add(s, e);
}

/* Realization for a binding: the entry itself, or an existing one for the
 * subject, or (when `create`) a new one realized now and added to the session. */
static VisorRealizationEntry *om_realization_for(VisorSession *s, const VisorBinding *b, bool create,
                                                 char *err, size_t errn) {
    VisorRealizationEntry *e = om_entry(s, b);
    if (e) return e;
    if (!b || (b->kind != VISOR_BIND_OBJECT && b->kind != VISOR_BIND_PROGRAM)) {
        snprintf(err, errn, "not a realization, object or program");
        return NULL;
    }
    int idx = om_find_real_for(s, &b->id);
    if (idx >= 0) return &s->realizations[idx];
    if (!create) {
        snprintf(err, errn, "no realization for %s in this session; use `realize %s` first", b->name, b->name);
        return NULL;
    }
    VisorRealizationEntry ne;
    int rc;
    if (b->kind == VISOR_BIND_PROGRAM) {
        OmegaProgram *p = om_program(s, b);
        if (!p) { snprintf(err, errn, "program binding has no program"); return NULL; }
        rc = visor_realize_program(p, &s->machine, &ne, &om_rv);
        ne.program_index = b->index;
        ne.subject_is_program = true;
    } else {
        rc = visor_realize_apply(s->graph, &b->id, &s->machine, &ne, &om_rv);
    }
    if (rc != 0) { snprintf(err, errn, "%s", om_rv.why[0] ? om_rv.why : "realization failed"); return NULL; }
    idx = om_add_entry(s, &ne);
    if (idx < 0) { snprintf(err, errn, "session realization table full"); return NULL; }
    return &s->realizations[idx];
}

/* ---------------- program summary (inspect/type/graph on a PROGRAM) ---------------- */

static int om_program_summary(const VisorViewCall *call, const OmegaProgram *p, FILE *out) {
    char pid[72], ti[32], to[32];
    om_id(&p->program_id, pid);
    om_type_name(p->contract.input_type, p->contract.input_width, ti, sizeof(ti));
    om_type_name(p->contract.output_type, p->contract.output_width, to, sizeof(to));
    unsigned code_len = p->is_realized ? (unsigned)p->realization.code_len : 0;
    if (call->json) {
        fputs("{\"program\":", out); visor_json_string(out, p->name);
        fprintf(out, ",\"contract\":\"%s -> %s\",\"program_id\":\"%s\",\"is_realized\":%s,\"is_verified\":%s,\"code_len\":%u}",
                ti, to, pid, p->is_realized ? "true" : "false", p->is_verified ? "true" : "false", code_len);
    } else {
        fprintf(out, "program %s\n  contract: %s -> %s\n  program_id: %s\n  is_realized: %s\n  is_verified: %s\n  code_len: %u\n",
                p->name, ti, to, pid, p->is_realized ? "true" : "false", p->is_verified ? "true" : "false", code_len);
    }
    return 0;
}

static int om_realization_show(const VisorViewCall *call, VisorRealizationEntry *e, FILE *out, char *err, size_t errn) {
    VisorSession *s = call->session;
    if (visor_realization_view(e, s->has_machine ? &s->machine : NULL, &om_rv) != 0) {
        snprintf(err, errn, "cannot build realization view");
        return 1;
    }
    return om_put(out, OM_FMT(call, visor_realization_format_text, visor_realization_format_json, &om_rv, om_buf),
                  om_buf, err, errn);
}

/* Shared PROGRAM / REALIZATION handling for inspect, type and graph. 1 = handled. */
static int om_nonobject(const VisorViewCall *call, FILE *out, char *err, size_t errn, int *rc) {
    const VisorBinding *b = call->subject;
    OmegaProgram *p = om_program(call->session, b);
    if (p) { *rc = om_program_summary(call, p, out); return 1; }
    VisorRealizationEntry *e = om_entry(call->session, b);
    if (e) { *rc = om_realization_show(call, e, out, err, errn); return 1; }
    return 0;
}

/* ---------------- hooks ---------------- */

static int om_inspect(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    int rc;
    if (om_nonobject(call, out, err, errn, &rc)) return rc;
    int ir = visor_semantic_inspect(call->session->graph, &call->subject->id, &om_obj);
    if (ir == -1) { snprintf(err, errn, "object not in graph"); return 1; }
    if (om_put(out, OM_FMT(call, visor_semantic_format_text, visor_semantic_format_json, &om_obj, om_buf),
               om_buf, err, errn) != 0) return 1;
    if (ir == -2) { snprintf(err, errn, "malformed object: %s", om_obj.malformed_reason); return VISOR_VIEW_ERROR_WITH_OUTPUT; }
    return 0;
}

static int om_type_of(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    int rc;
    if (om_nonobject(call, out, err, errn, &rc)) return rc;
    SemanticId tid;
    char text[VISOR_SEM_TYPE_TEXT];
    int tr = visor_semantic_type_of(call->session->graph, &call->subject->id, &tid, text, sizeof(text));
    if (tr == -2) { snprintf(err, errn, "malformed object"); return 1; }
    if (tr != 0) { snprintf(err, errn, "no type (a TYPE or opaque object, or unresolved type)"); return 1; }
    char tids[72];
    om_id(&tid, tids);
    if (call->json) {
        fputs("{\"type\":", out); visor_json_string(out, text);
        fprintf(out, ",\"type_id\":\"%s\"}", tids);
    } else {
        fprintf(out, "%s\n", text);
    }
    return 0;
}

static int om_id_of(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    VisorSession *s = call->session;
    const VisorBinding *b = call->subject;
    char a[72], c2[72];
    if (b->kind == VISOR_BIND_OBJECT) {
        if (visor_semantic_inspect(s->graph, &b->id, &om_obj) == -1) { snprintf(err, errn, "object not in graph"); return 1; }
        if (call->json)
            fprintf(out, "{\"kind\":\"object\",\"id\":\"%s\",\"canonical_len\":%zu,\"canonical_sha256\":\"%s\"}",
                    om_obj.id_text, om_obj.canonical_len, om_obj.canonical_sha256);
        else
            fprintf(out, "%s\n  canonical_len: %zu\n  canonical_sha256: %s\n",
                    om_obj.id_text, om_obj.canonical_len, om_obj.canonical_sha256);
        return 0;
    }
    OmegaProgram *p = om_program(s, b);
    if (p) {
        om_id(&p->program_id, a);
        if (p->is_realized) om_id(&p->realization.realization_id, c2); else snprintf(c2, sizeof(c2), "none");
        if (call->json)
            fprintf(out, "{\"kind\":\"program\",\"program_id\":\"%s\",\"realization_id\":\"%s\"}", a, c2);
        else
            fprintf(out, "program_id: %s\nrealization_id: %s\n", a, c2);
        return 0;
    }
    VisorRealizationEntry *e = om_entry(s, b);
    if (e) {
        om_id(&e->real.realization_id, a);
        om_id(&e->subject_id, c2);
        if (call->json)
            fprintf(out, "{\"kind\":\"realization\",\"realization_id\":\"%s\",\"subject_id\":\"%s\"}", a, c2);
        else
            fprintf(out, "realization_id: %s\nsubject_id: %s\n", a, c2);
        return 0;
    }
    snprintf(err, errn, "binding refers to nothing in this session");
    return 1;
}

static int om_graph(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    if (!call->subject) { snprintf(err, errn, "give a name, _ or id (e.g. `graph _`)"); return 1; }
    int rc;
    if (om_nonobject(call, out, err, errn, &rc)) return rc;
    int n = visor_semantic_graph_text(call->session->graph, &call->subject->id, om_buf, sizeof(om_buf));
    if (n < 0) { snprintf(err, errn, "graph not available (root missing, too deep, or output too large)"); return 1; }
    if (call->json) { fputs("{\"graph\":", out); visor_json_string(out, om_buf); fputc('}', out); }
    else fputs(om_buf, out);
    return 0;
}

static int om_eval_line(VisorSession *s, const char *line, VisorBinding *out, char *value, size_t n,
                        char *err, size_t errn) {
    OmegaLangResult r;
    memset(&r, 0, sizeof(r));
    if (omega_language_eval_line(s, line, &r, err, errn) != 0) {
        if (!err[0]) snprintf(err, errn, "language error");
        return 1;
    }
    memset(out, 0, sizeof(*out));
    out->index = -1;
    uint64_t v = 0;
    int er;
    switch (r.kind) {
        case OMEGA_LANG_BINDING:
            snprintf(out->name, sizeof(out->name), "%s", r.name);
            out->kind = VISOR_BIND_OBJECT;
            out->id = r.id;
            if (visor_semantic_eval_u64(s->graph, &r.id, &v) == 0) snprintf(value, n, "%" PRIu64, v);
            else snprintf(value, n, "%s", r.type_text);
            break;
        case OMEGA_LANG_EXPRESSION:
            snprintf(out->name, sizeof(out->name), "_");
            out->kind = VISOR_BIND_OBJECT;
            out->id = r.id;
            er = visor_semantic_eval_u64(s->graph, &r.id, &v);
            if (er == 0) snprintf(value, n, "%" PRIu64, v);
            else snprintf(value, n, "<unevaluated: %s>",
                          er == -2 ? "malformed" : er == -3 ? "refused by evaluator" : "unsupported");
            break;
        case OMEGA_LANG_PROGRAM:
            snprintf(out->name, sizeof(out->name), "%s", r.name);
            out->kind = VISOR_BIND_PROGRAM;
            out->id = r.id;
            out->index = r.program_index;
            snprintf(value, n, "fn %s: %s", r.name, r.type_text[0] ? r.type_text : "u64 -> u64");
            break;
        default:
            out->kind = VISOR_BIND_NONE;
            value[0] = '\0';
            break;
    }
    return 0;
}

static int om_machine(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    VisorSession *s = call->session;
    if (!s->has_machine) { snprintf(err, errn, "no machine model in this session"); return 1; }
    if (visor_machine_view(&s->machine, &om_mv) != 0) { snprintf(err, errn, "cannot build machine view"); return 1; }
    return om_put(out, OM_FMT(call, visor_machine_format_text, visor_machine_format_json, &om_mv, om_buf), om_buf, err, errn);
}

static int om_realize(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    VisorSession *s = call->session;
    const VisorBinding *b = call->subject;
    VisorRealizationEntry *have = om_entry(s, b);
    if (have) return om_realization_show(call, have, out, err, errn);
    /* `for current.machine` is the same call: the session machine IS the current machine. */
    VisorRealizationEntry e;
    int rc;
    if (b->kind == VISOR_BIND_PROGRAM) {
        OmegaProgram *p = om_program(s, b);
        if (!p) { snprintf(err, errn, "program binding has no program"); return 1; }
        rc = visor_realize_program(p, &s->machine, &e, &om_rv);
        e.program_index = b->index;
        e.subject_is_program = true;
    } else {
        rc = visor_realize_apply(s->graph, &b->id, &s->machine, &e, &om_rv);
    }
    if (rc != 0) { snprintf(err, errn, "%s", om_rv.why[0] ? om_rv.why : "realization failed"); return 1; }
    int idx = om_add_entry(s, &e);
    if (idx < 0) { snprintf(err, errn, "session realization table full"); return 1; }
    VisorBinding last;
    memset(&last, 0, sizeof(last));
    snprintf(last.name, sizeof(last.name), "_");
    last.kind = VISOR_BIND_REALIZATION;
    last.id = s->realizations[idx].real.realization_id;
    last.index = idx;
    visor_set_last(s, &last);
    return om_realization_show(call, &s->realizations[idx], out, err, errn);
}

static int om_cost(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    VisorSession *s = call->session;
    VisorRealizationEntry *e = om_realization_for(s, call->subject, false, err, errn);
    if (!e) return 1;
    if (visor_realization_cost(e, s->has_machine ? &s->machine : NULL, &om_cv) != 0) {
        snprintf(err, errn, "cost not available");
        return 1;
    }
    return om_put(out, OM_FMT(call, visor_cost_format_text, visor_cost_format_json, &om_cv, om_buf), om_buf, err, errn);
}

static int om_parse_u64(const char *t, uint64_t *v) {
    if (!t || !*t || *t == '-' || *t == '+') return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long x = strtoull(t, &end, 0);
    if (errno || !end || *end) return -1;
    *v = (uint64_t)x;
    return 0;
}

static int om_run(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    VisorSession *s = call->session;
    VisorRealizationEntry *e = om_realization_for(s, call->subject, true, err, errn);
    if (!e) return 1;
    uint64_t args[3] = {0, 0, 0};
    bool known[3] = {false, false, false};
    size_t argc = 0;
    if (!e->subject_is_program) {
        SemanticId op, operands[4];
        size_t n = 0;
        if (visor_semantic_apply_parts(s->graph, &e->subject_id, &op, operands, &n) == 0) {
            if (n > 3) { snprintf(err, errn, "apply has %zu operands; run takes at most 3", n); return 1; }
            for (size_t i = 0; i < n; i++) {
                if (visor_semantic_value_u64(s->graph, &operands[i], &args[i]) == 0 ||
                    visor_semantic_eval_u64(s->graph, &operands[i], &args[i]) == 0)
                    known[i] = true;
            }
            argc = n;
        }
    }
    size_t extra = call->cmd->argc > 0 ? call->cmd->argc - 1 : 0;
    if (extra > 3) { snprintf(err, errn, "at most 3 arguments"); return 1; }
    for (size_t i = 0; i < extra; i++) {
        if (om_parse_u64(call->cmd->args[i + 1], &args[i]) != 0) {
            snprintf(err, errn, "argument %zu '%s' is not a u64", i + 1, call->cmd->args[i + 1]);
            return 1;
        }
        known[i] = true;
    }
    if (extra > argc) argc = extra;
    for (size_t i = 0; i < argc; i++) {
        if (!known[i]) { snprintf(err, errn, "operand %zu has no known value; pass it on the command line", i + 1); return 1; }
    }
    uint64_t result = 0;
    int rc = visor_realization_run_pure(e, args, argc, &result);
    if (rc != 0) {
        if (rc == -3 && visor_realization_view(e, s->has_machine ? &s->machine : NULL, &om_rv) == 0 && om_rv.why[0])
            snprintf(err, errn, "refused: %s", om_rv.why);
        else
            snprintf(err, errn, "refused by run_pure (rc %d)", rc);
        return 1;
    }
    char rid[72];
    om_id(&e->real.realization_id, rid);
    if (call->json) fprintf(out, "{\"result\":\"%" PRIu64 "\",\"realization\":\"%s\"}", result, rid);
    else fprintf(out, "%" PRIu64 "\n", result);
    return 0;
}

static int om_alternatives(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    VisorSession *s = call->session;
    const VisorBinding *b = call->subject;
    int pidx = -1;
    if (b->kind == VISOR_BIND_PROGRAM && om_program(s, b)) pidx = b->index;
    VisorRealizationEntry *re = om_entry(s, b);
    if (re && re->subject_is_program) pidx = re->program_index;
    if (pidx < 0 || (size_t)pidx >= s->program_count) {
        snprintf(err, errn, "alternatives: only programs have alternative realizations in V1");
        return 1;
    }
    size_t count = 0;
    if (visor_realization_alternatives_ex(&s->programs[pidx], &s->machine, om_alt_entries, om_alt_views, 4, &count) != 0) {
        snprintf(err, errn, "alternatives not available for this program");
        return 1;
    }
    if (call->json) fputc('[', out);
    for (size_t i = 0; i < count; i++) {
        om_alt_entries[i].program_index = pidx;
        om_alt_entries[i].subject_is_program = true;
        /* Only compatible alternatives enter the session: an entry does not carry the
         * differential-check verdict, so a later `run` could not see it. */
        if (om_alt_views[i].compatible) (void)om_add_entry(s, &om_alt_entries[i]);
        const VisorRealizationView *v = &om_alt_views[i];
        int cr = OM_FMT(call, visor_cost_format_text, visor_cost_format_json, &v->cost, om_buf2);
        if (cr < 0) { snprintf(err, errn, "output too large"); return 1; }
        if (call->json) {
            if (i) fputc(',', out);
            fputs("{\"label\":", out); visor_json_string(out, v->label);
            fputs(",\"realization_id\":", out); visor_json_string(out, v->realization_id);
            fprintf(out, ",\"runnable\":%s,\"compatible\":%s,\"added\":%s,\"cost\":%s}", v->runnable ? "true" : "false",
                    v->compatible ? "true" : "false", v->compatible ? "true" : "false", om_buf2);
        } else {
            fprintf(out, "%s  %s%s\n", v->label, v->realization_id, v->compatible ? "" : "  (incompatible; not added to session)");
            fputs(om_buf2, out);
            if (om_buf2[0] && om_buf2[strlen(om_buf2) - 1] != '\n') fputc('\n', out);
        }
    }
    if (call->json) fputc(']', out);
    return 0;
}

static int om_view_for(VisorSession *s, const VisorBinding *b, VisorRealizationView *v, char *err, size_t errn) {
    VisorRealizationEntry *e = om_realization_for(s, b, false, err, errn);
    if (!e) return 1;
    if (visor_realization_view(e, s->has_machine ? &s->machine : NULL, v) != 0) {
        snprintf(err, errn, "cannot build realization view");
        return 1;
    }
    return 0;
}

static int om_compare(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    if (om_view_for(call->session, call->subject, &om_rv, err, errn) != 0) return 1;
    if (om_view_for(call->session, call->subject2, &om_rv2, err, errn) != 0) return 1;
    if (visor_realization_compare(&om_rv, &om_rv2, om_buf, sizeof(om_buf)) < 0) { snprintf(err, errn, "output too large"); return 1; }
    if (call->json) { fputs("{\"comparison\":", out); visor_json_string(out, om_buf); fputc('}', out); }
    else fputs(om_buf, out);
    return 0;
}

static int om_why(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    if (om_view_for(call->session, call->subject, &om_rv, err, errn) != 0) return 1;
    if (visor_realization_why(&om_rv, om_buf, sizeof(om_buf)) < 0) { snprintf(err, errn, "output too large"); return 1; }
    if (call->json) { fputs("{\"why\":", out); visor_json_string(out, om_buf); fputc('}', out); }
    else fputs(om_buf, out);
    return 0;
}

static int om_world(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    visor_world_unattached(&om_wv);   /* never visor_world_snapshot: runtime not linked */
    return om_put(out, OM_FMT(call, visor_world_format_text, visor_world_format_json, &om_wv, om_buf), om_buf, err, errn);
}

static int om_verify(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    VisorSession *s = call->session;
    const VisorBinding *b = call->subject;
    const OmegaMachineGraph *mg = s->has_machine ? &s->machine : NULL;
    int rc;
    OmegaProgram *p = om_program(s, b);
    VisorRealizationEntry *e = om_entry(s, b);
    if (p) {
        rc = visor_verify_program(p, &om_vr);
    } else if (e) {
        if (e->subject_is_program && e->program_index >= 0 && (size_t)e->program_index < s->program_count)
            rc = visor_verify_program(&s->programs[e->program_index], &om_vr);
        else
            rc = visor_verify_object(s->graph, &e->subject_id, &e->real, mg, &om_vr);
    } else {
        int idx = om_find_real_for(s, &b->id);
        rc = visor_verify_object(s->graph, &b->id, idx >= 0 ? &s->realizations[idx].real : NULL, mg, &om_vr);
    }
    if (rc != 0) { snprintf(err, errn, "verification could not run (bad arguments)"); return 1; }
    if (om_put(out, OM_FMT(call, visor_verify_format_text, visor_verify_format_json, &om_vr, om_buf), om_buf, err, errn) != 0)
        return 1;
    if (!om_vr.passed) {
        snprintf(err, errn, "verification failed: %s", om_vr.first_violation[0] ? om_vr.first_violation : "no check ran");
        return VISOR_VIEW_ERROR_WITH_OUTPUT;
    }
    return 0;
}

static int om_evidence(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    const char *root = call->evidence_root ? call->evidence_root : "evidence";
    int rc;
    if (call->cmd->argc == 0) {
        rc = visor_evidence_scan(root, &om_ev);
    } else {
        const char *tok = call->cmd->args[0];
        VisorBinding b;
        SemanticId id;
        if (visor_resolve(call->session, tok, &b) == 0) {
            id = b.id;
        } else {
            const char *hex = strncmp(tok, "sha256:", 7) == 0 ? tok + 7 : tok;
            if (strlen(hex) != 64 || omega_parse_hex_semantic_id(hex, &id) != 0) {
                snprintf(err, errn, "unknown name or id '%s'", tok);
                return 1;
            }
        }
        rc = visor_evidence_for_id(root, &id, &om_ev);
    }
    if (rc != 0) { snprintf(err, errn, "evidence folder '%s' not readable", root); return 1; }
    return om_put(out, OM_FMT(call, visor_evidence_format_text, visor_evidence_format_json, &om_ev, om_buf), om_buf, err, errn);
}

/* One effect object -> request view (never executed). Returns 0 ok, 1 refused. */
static int om_effect_one(const VisorViewCall *call, const SemanticId *id, FILE *out, bool first, char *err, size_t errn) {
    char ids[72];
    om_id(id, ids);
    if (call->json && !first) fputc(',', out);
    if (visor_effect_request_build(call->session->graph, id, &om_er) != 0) {
        if (call->json) fprintf(out, "{\"id\":\"%s\",\"refused\":\"malformed/tampered\"}", ids);
        else fprintf(out, "%s refused (malformed/tampered)\n", ids);
        return 1;
    }
    if (OM_FMT(call, visor_effect_request_format_text, visor_effect_request_format_json, &om_er, om_buf) < 0) {
        snprintf(err, errn, "output too large");
        return -1;
    }
    fputs(om_buf, out);
    if (!call->json && om_buf[0] && om_buf[strlen(om_buf) - 1] != '\n') fputc('\n', out);
    return 0;
}

static int om_effects(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    const OmegaGraph *g = call->session->graph;
    if (call->subject) {
        const OmegaObject *o = call->subject->kind == VISOR_BIND_OBJECT ? omega_graph_find_object_const(g, &call->subject->id) : NULL;
        if (!o || o->kind != KIND_EFFECT) { snprintf(err, errn, "'%s' is not an EFFECT object", call->cmd->args[0]); return 1; }
        int r = om_effect_one(call, &call->subject->id, out, true, err, errn);
        if (r < 0) return 1;
        if (r > 0) { snprintf(err, errn, "effect object refused (malformed/tampered)"); return VISOR_VIEW_ERROR_WITH_OUTPUT; }
        return 0;
    }
    size_t shown = 0;
    if (call->json) fputc('[', out);
    for (size_t i = 0; i < g->object_count; i++) {
        if (g->objects[i].kind != KIND_EFFECT) continue;
        if (om_effect_one(call, &g->objects[i].id, out, shown == 0, err, errn) < 0) return 1;
        shown++;
    }
    if (call->json) fputc(']', out);
    else if (shown == 0) fputs("no effect objects in session\n", out);
    return 0;
}

/* `run` gate. Anything not provably pure is an effect request (fail closed). */
static int om_classify(const VisorViewCall *call, VisorClass *cls, char *reason, size_t rn, char *err, size_t errn) {
    (void)err; (void)errn;
    VisorSession *s = call->session;
    const VisorBinding *b = call->subject;
    *cls = VISOR_CLASS_EFFECT_REQUEST;
    if (b->kind == VISOR_BIND_PROGRAM) {
        *cls = VISOR_CLASS_PURE_EXECUTION;
        snprintf(reason, rn, "programs carry no effects in V0");
        return 0;
    }
    const SemanticId *graph_id = &b->id;
    VisorRealizationEntry *e = om_entry(s, b);
    if (e) {
        if (visor_realization_view(e, s->has_machine ? &s->machine : NULL, &om_rv) != 0 || !om_rv.runnable) {
            snprintf(reason, rn, "%s", om_rv.why[0] ? om_rv.why : "realization is not a runnable pure aarch64 realization");
            return 0;
        }
        if (e->subject_is_program) {
            *cls = VISOR_CLASS_PURE_EXECUTION;
            snprintf(reason, rn, "pure aarch64 realization of a program");
            return 0;
        }
        graph_id = &e->subject_id;
    }
    bool req = true;
    if (visor_effect_request_classify(s->graph, graph_id, &req) != 0) {
        snprintf(reason, rn, "graph has a missing reference (fail closed)");
        return 0;
    }
    if (req) {
        snprintf(reason, rn, "graph reaches an EFFECT or capability reference");
        return 0;
    }
    *cls = VISOR_CLASS_PURE_EXECUTION;
    snprintf(reason, rn, "no effects reachable");
    return 0;
}

void visor_console_default_ops(VisorConsoleOps *ops) {
    if (!ops) return;
    memset(ops, 0, sizeof(*ops));
    ops->inspect = om_inspect;
    ops->type_of = om_type_of;
    ops->id_of = om_id_of;
    ops->graph_text = om_graph;
    ops->eval_line = om_eval_line;
    ops->verify = om_verify;
    ops->machine = om_machine;
    ops->realize = om_realize;
    ops->cost = om_cost;
    ops->run_pure = om_run;
    ops->evidence = om_evidence;
    ops->world = om_world;
    ops->effects_classify = om_classify;
    ops->effects = om_effects;
    ops->alternatives = om_alternatives;
    ops->compare = om_compare;
    ops->why = om_why;
}

/* Session with the current machine model (assumed profile, not measured). */
int omega_tool_session_init(VisorSession *s) {
    if (visor_session_init(s) != 0) return -1;
    if (visor_machine_current(&s->machine, &om_mv) == 0) {
        s->has_machine = true;
        s->machine_is_observed = false;
    }
    return 0;
}

#ifndef OMEGA_TOOL_NO_MAIN
static void om_usage(FILE *f) {
    fprintf(f, "usage: omega [--json] [--command \"<line>\"]... [--script <file>|-] [--evidence-root <dir>]\n");
}

int main(int argc, char **argv) {
    bool json = false;
    const char *script = NULL;
    const char *evidence_root = NULL;
    const char **commands = calloc((size_t)argc + 1, sizeof(*commands));
    size_t ncommands = 0;
    if (!commands) { fprintf(stderr, "omega: out of memory\n"); return 2; }

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--json") == 0) {
            json = true;
        } else if (strcmp(a, "--command") == 0 || strcmp(a, "--script") == 0 || strcmp(a, "--evidence-root") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "omega: %s needs a value\n", a); om_usage(stderr); free(commands); return 2; }
            const char *v = argv[++i];
            if (strcmp(a, "--command") == 0) commands[ncommands++] = v;
            else if (strcmp(a, "--script") == 0) {
                if (script) { fprintf(stderr, "omega: only one --script allowed\n"); free(commands); return 2; }
                script = v;
            } else evidence_root = v;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            om_usage(stdout);
            free(commands);
            return 0;
        } else {
            fprintf(stderr, "omega: unknown argument '%s'\n", a);
            om_usage(stderr);
            free(commands);
            return 2;
        }
    }

    FILE *script_f = NULL;
    if (script) {
        if (strcmp(script, "-") == 0) script_f = stdin;
        else if (!(script_f = fopen(script, "r"))) {
            fprintf(stderr, "omega: cannot open script '%s'\n", script);
            free(commands);
            return 2;
        }
    }

    VisorSession *session = calloc(1, sizeof(*session));
    if (!session || omega_tool_session_init(session) != 0) {
        fprintf(stderr, "omega: cannot create session\n");
        free(session); free(commands);
        if (script_f && script_f != stdin) fclose(script_f);
        return 2;
    }
    VisorConsole con;
    visor_console_init(&con, session, stdout, stderr, json);
    VisorConsoleOps ops;
    visor_console_default_ops(&ops);
    visor_console_set_ops(&con, &ops);
    con.evidence_root = evidence_root;

    int any_err = 0;
    /* --command lines run in order; an error does not stop later ones; quit does. */
    for (size_t i = 0; i < ncommands && !con.quit; i++) {
        if (visor_console_exec_line(&con, commands[i]) != 0) any_err = 1;
    }
    if (script_f && !con.quit) {
        if (visor_console_run_script(&con, script_f) != 0) any_err = 1;
    }
    if (!script_f && ncommands == 0) {
        if (!json && isatty(fileno(stdin)))
            printf("Omega Visor V1 -- type `help` for commands, `quit` to leave.\n");
        if (visor_console_repl(&con, stdin) != 0) any_err = 1;
    }

    if (script_f && script_f != stdin) fclose(script_f);
    visor_session_destroy(session);
    free(session);
    free(commands);
    return any_err ? 1 : 0;
}
#endif /* OMEGA_TOOL_NO_MAIN */
