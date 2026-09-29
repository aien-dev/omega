/*
 * visor_world.c -- types, formatters and the unattached view (lane 6).
 * Physics-free and runtime-free: no rx_* symbol and no runtime header here, so
 * this file links into the V1 `omega` binary. See visor_world.h.
 */
#include "visor_world.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void visor_world_gap_append(VisorWorldView *v, const char *note) {
    if (!v || !note || !note[0]) return;
    size_t cap = sizeof(v->gap_note);
    size_t len = strnlen(v->gap_note, cap);
    if (len >= cap - 1) return;
    int w = snprintf(v->gap_note + len, cap - len, "%s%s", len ? "; " : "", note);
    (void)w; /* snprintf truncates and NUL-terminates; a full note stays bounded */
}

void visor_world_unattached(VisorWorldView *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->attached = false;
    out->state_name = "unattached";
    out->staged_effects_visible = false;
    visor_world_gap_append(out,
        "no resident World attached in this session: the V1 omega binary does not "
        "link the runtime (src/runtime); nothing here is observed");
}

/* ---- bounded writer --------------------------------------------------- */

typedef struct { char *p; size_t n; size_t len; bool overflow; } Buf;

static void bput(Buf *b, const char *fmt, ...) {
    if (b->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(b->p + b->len, b->n - b->len, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= b->n - b->len) { b->overflow = true; return; }
    b->len += (size_t)w;
}

/* JSON string (escapes ", \ and control bytes). NULL -> null. */
static void bjson_str(Buf *b, const char *s) {
    if (!s) { bput(b, "null"); return; }
    bput(b, "\"");
    for (const unsigned char *c = (const unsigned char *)s; *c && !b->overflow; c++) {
        if (*c == '"' || *c == '\\') bput(b, "\\%c", *c);
        else if (*c < 0x20) bput(b, "\\u%04x", (unsigned)*c);
        else bput(b, "%c", *c);
    }
    bput(b, "\"");
}

static int bfinish(Buf *b) {
    if (b->overflow) { b->p[0] = '\0'; return -1; }
    return (int)b->len;
}

static const char *nz(const char *s, const char *dflt) { return (s && s[0]) ? s : dflt; }

int visor_world_format_text(const VisorWorldView *v, char *out, size_t n) {
    if (!v || !out || n == 0) return -1;
    out[0] = '\0';
    Buf b = { out, n, 0, false };
    bput(&b, "world: %s\n", v->attached ? "attached" : "unattached");
    bput(&b, "  state:              %s\n", nz(v->state_name, "-"));
    if (v->generation) bput(&b, "  generation:         %" PRIu64 "\n", v->generation);
    else bput(&b, "  generation:         (not observable)\n");
    bput(&b, "  resident objects:   %zu (shown %zu)\n", v->resident_object_count, v->object_count);
    bput(&b, "  reactions:          %zu\n", v->reaction_count);
    bput(&b, "  active work:        %zu\n", v->active_work);
    bput(&b, "  published results:  %zu%s\n", v->published_results,
         v->published_results ? "" : " (not observable)");
    if (v->staged_effects_visible) bput(&b, "  staged effects:     %zu\n", v->staged_effects);
    else bput(&b, "  staged effects:     (not visible)\n");
    bput(&b, "  publication tail:   %" PRIu64 "\n", v->publication_tail);
    bput(&b, "  digest:             %s\n", nz(v->digest, "-"));
    size_t shown = v->object_count > VISOR_WORLD_MAX_OBJECTS ? VISOR_WORLD_MAX_OBJECTS
                                                             : v->object_count;
    for (size_t i = 0; i < shown; i++)
        bput(&b, "  object %-22s %-10s %s\n", v->objects[i].id,
             nz(v->objects[i].kind, "-"), nz(v->objects[i].state, "-"));
    bput(&b, "  gap: %s\n", nz(v->gap_note, "none"));
    bput(&b, "  note: view only; this view holds no authority over the World\n");
    return bfinish(&b);
}

int visor_world_format_json(const VisorWorldView *v, char *out, size_t n) {
    if (!v || !out || n == 0) return -1;
    out[0] = '\0';
    Buf b = { out, n, 0, false };
    bput(&b, "{\"attached\":%s,\"state\":", v->attached ? "true" : "false");
    bjson_str(&b, v->state_name);
    bput(&b, ",\"generation\":%" PRIu64 ",\"generation_observable\":%s",
         v->generation, v->generation ? "true" : "false");
    bput(&b, ",\"resident_object_count\":%zu,\"reaction_count\":%zu,\"active_work\":%zu",
         v->resident_object_count, v->reaction_count, v->active_work);
    bput(&b, ",\"published_results\":%zu,\"staged_effects\":%zu,\"staged_effects_visible\":%s",
         v->published_results, v->staged_effects, v->staged_effects_visible ? "true" : "false");
    bput(&b, ",\"publication_tail\":%" PRIu64 ",\"digest\":", v->publication_tail);
    bjson_str(&b, v->digest[0] ? v->digest : NULL);
    bput(&b, ",\"objects\":[");
    size_t shown = v->object_count > VISOR_WORLD_MAX_OBJECTS ? VISOR_WORLD_MAX_OBJECTS
                                                             : v->object_count;
    for (size_t i = 0; i < shown; i++) {
        bput(&b, "%s{\"id\":", i ? "," : "");
        bjson_str(&b, v->objects[i].id);
        bput(&b, ",\"kind\":");
        bjson_str(&b, v->objects[i].kind);
        bput(&b, ",\"state\":");
        bjson_str(&b, v->objects[i].state);
        bput(&b, "}");
    }
    bput(&b, "],\"gap_note\":");
    bjson_str(&b, v->gap_note);
    bput(&b, ",\"authority\":false}");
    return bfinish(&b);
}
