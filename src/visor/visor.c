/* visor.c -- Omega Visor V1 shared session (lead-owned). No authority here. */
#include "visor.h"
#include "omega_core.h"
#include "omega_canonical.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static bool visor_name_ok(const char *name) {
    if (!name || !*name) return false;
    size_t n = strlen(name);
    if (n >= VISOR_NAME_LEN) return false;
    if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) return false;
    for (size_t i = 1; i < n; i++) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_' || name[i] == '.')) return false;
    }
    return true;
}

int visor_session_init(VisorSession *s) {
    if (!s) return -1;
    memset(s, 0, sizeof(*s));
    s->graph = omega_graph_create();
    if (!s->graph) return -1;
    return 0;
}

void visor_session_destroy(VisorSession *s) {
    if (!s) return;
    for (size_t i = 0; i < s->program_count; i++) omega_program_destroy(&s->programs[i]);
    if (s->graph) omega_graph_destroy(s->graph);
    memset(s, 0, sizeof(*s));
}

void visor_session_clear(VisorSession *s) {
    if (!s) return;
    OmegaMachineGraph keep = s->machine;
    bool has_machine = s->has_machine, observed = s->machine_is_observed;
    visor_session_destroy(s);
    (void)visor_session_init(s);
    s->machine = keep;
    s->has_machine = has_machine;
    s->machine_is_observed = observed;
}

int visor_binding_set(VisorSession *s, const char *name, VisorBindKind kind,
                      const SemanticId *id, int index) {
    if (!s || !visor_name_ok(name) || !id || kind == VISOR_BIND_NONE) return -1;
    if (strcmp(name, "_") == 0) return -1; /* `_` is reserved for the last result */
    VisorBinding *slot = NULL;
    for (size_t i = 0; i < s->bindings.count; i++) {
        if (strcmp(s->bindings.items[i].name, name) == 0) { slot = &s->bindings.items[i]; break; }
    }
    if (!slot) {
        if (s->bindings.count >= VISOR_MAX_BINDINGS) return -1;
        slot = &s->bindings.items[s->bindings.count++];
    }
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    slot->kind = kind;
    slot->id = *id;
    slot->index = (kind == VISOR_BIND_OBJECT) ? -1 : index;
    return 0;
}

const VisorBinding *visor_binding_get(const VisorSession *s, const char *name) {
    if (!s || !name) return NULL;
    if (strcmp(name, "_") == 0) return s->has_last ? &s->last : NULL;
    for (size_t i = 0; i < s->bindings.count; i++) {
        if (strcmp(s->bindings.items[i].name, name) == 0) return &s->bindings.items[i];
    }
    return NULL;
}

void visor_set_last(VisorSession *s, const VisorBinding *b) {
    if (!s || !b) return;
    s->last = *b;
    snprintf(s->last.name, sizeof(s->last.name), "_");
    s->has_last = true;
}

int visor_resolve(const VisorSession *s, const char *token, VisorBinding *out) {
    if (!s || !token || !out || !*token) return -1;
    const VisorBinding *b = visor_binding_get(s, token);
    if (b) { *out = *b; return 0; }

    const char *hex = token;
    if (strncmp(hex, "sha256:", 7) == 0) hex += 7;
    if (strlen(hex) != 64) return -1;
    SemanticId id;
    if (omega_parse_hex_semantic_id(hex, &id) != 0) return -1;

    if (omega_graph_find_object_const(s->graph, &id)) {
        memset(out, 0, sizeof(*out));
        snprintf(out->name, sizeof(out->name), "%.63s", token);
        out->kind = VISOR_BIND_OBJECT; out->id = id; out->index = -1;
        return 0;
    }
    for (size_t i = 0; i < s->program_count; i++) {
        if (omega_compare_semantic_id(&s->programs[i].program_id, &id) == 0) {
            memset(out, 0, sizeof(*out));
            snprintf(out->name, sizeof(out->name), "%.63s", token);
            out->kind = VISOR_BIND_PROGRAM; out->id = id; out->index = (int)i;
            return 0;
        }
    }
    for (size_t i = 0; i < s->realization_count; i++) {
        if (omega_compare_semantic_id(&s->realizations[i].real.realization_id, &id) == 0) {
            memset(out, 0, sizeof(*out));
            snprintf(out->name, sizeof(out->name), "%.63s", token);
            out->kind = VISOR_BIND_REALIZATION; out->id = id; out->index = (int)i;
            return 0;
        }
    }
    return -1;
}

int visor_program_add(VisorSession *s, const OmegaProgram *p) {
    if (!s || !p || s->program_count >= VISOR_MAX_PROGRAMS) return -1;
    s->programs[s->program_count] = *p;
    return (int)s->program_count++;
}

int visor_realization_add(VisorSession *s, const VisorRealizationEntry *e) {
    if (!s || !e || s->realization_count >= VISOR_MAX_REALIZATIONS) return -1;
    s->realizations[s->realization_count] = *e;
    return (int)s->realization_count++;
}

void visor_format_id(const SemanticId *id, char out[72]) {
    char hex[65];
    if (!id) { snprintf(out, 72, "sha256:<null>"); return; }
    omega_hex_semantic_id(id, hex);
    snprintf(out, 72, "sha256:%s", hex);
}

const char *visor_kind_name(SemanticKind k) {
    switch (k) {
        case KIND_VALUE: return "VALUE";
        case KIND_TYPE: return "TYPE";
        case KIND_OPERATION: return "OPERATION";
        case KIND_RELATION: return "RELATION";
        case KIND_CONSTRAINT: return "CONSTRAINT";
        case KIND_MEMORY: return "MEMORY";
        case KIND_MACHINE: return "MACHINE";
        case KIND_EFFECT: return "EFFECT";
        case KIND_REALIZATION: return "REALIZATION";
        case KIND_EVIDENCE: return "EVIDENCE";
        case KIND_PROOF: return "PROOF";
        default: return "INVALID";
    }
}
