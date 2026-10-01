/* rx_crumb_export.c -- RxCrumb -> RXCLOG01 record. */
#include "replay/rx_crumb_export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert((int)RXL_MAX_DEPS == (int)RX_MAX_DEPS, "rxlog input limit drifted from rx_world.h");
_Static_assert((int)RXL_MAX_WRITES == (int)RX_MAX_WRITES, "rxlog output limit drifted from rx_world.h");
_Static_assert((int)RXL_MAX_CAPS == (int)RX_MAX_CAPS, "rxlog cap limit drifted from rx_world.h");
_Static_assert((int)RXL_MAX_PARENTS == (int)RX_MAX_PARENTS, "rxlog parent limit drifted from rx_world.h");
_Static_assert((int)RXL_MAX_MUTS == (int)RX_MAX_MUTATIONS, "rxlog mutation limit drifted from rx_world.h");
_Static_assert((int)RXL_K_EXTERNAL == (int)RX_CRUMB_EXTERNAL && (int)RXL_K_CREATE == (int)RX_CRUMB_CREATE &&
               (int)RXL_K_MAX == (int)RX_CRUMB_QUARANTINE, "rxlog crumb kinds drifted from rx_world.h");

void rxx_crumb(const RxCrumb *k, rxl_crumb *o) {
    memset(o, 0, sizeof *o);
    o->id = k->id;
    o->kind = (uint32_t)k->kind;
    o->reaction = k->reaction;
    o->faculty = k->faculty;
    o->worker = k->worker;
    o->wake_cause = k->wake_cause;
    o->coalesced = k->coalesced_wakes;
    o->n_inputs = k->n_inputs;
    for (uint32_t i = 0; i < k->n_inputs && i < RX_MAX_DEPS; i++)
        o->inputs[i] = (rxl_io){ k->inputs[i].obj.id, k->inputs[i].obj.generation,
                                 k->inputs[i].version, k->inputs[i].mask };
    o->n_caps = k->n_caps;
    for (uint32_t i = 0; i < k->n_caps && i < RX_MAX_CAPS; i++)
        o->caps[i] = (rxl_cap){ k->caps[i].cap_id, k->caps[i].generation, k->cap_issuer[i] };
    o->n_outputs = k->n_outputs;
    for (uint32_t i = 0; i < k->n_outputs && i < RX_MAX_WRITES; i++)
        o->outputs[i] = (rxl_io){ k->outputs[i].obj.id, k->outputs[i].obj.generation,
                                  k->outputs[i].version, k->outputs[i].mask };
    o->reason = k->reason;
    o->n_parents = k->n_parents;
    for (uint32_t i = 0; i < k->n_parents && i < RX_MAX_PARENTS; i++) o->parents[i] = k->parents[i];
    o->t_start = k->t_start_ns;
    o->t_end = k->t_end_ns;
    o->episode = k->episode;
    memcpy(o->digest, k->digest, 32);
}

void rxx_init(rxx_ctx *x, uint64_t cap_base) {
    memset(x, 0, sizeof *x);
    x->cap_base = cap_base;
    x->next = 1;
}

void rxx_free(rxx_ctx *x) {
    free(x->raw);
    free(x->rel);
    memset(x, 0, sizeof *x);
}

static int grow(rxx_ctx *x) {
    if (x->n < x->cap) return 0;
    size_t nc = x->cap ? x->cap * 2 : 256;
    uint8_t (*r)[32] = realloc(x->raw, nc * sizeof *r);
    if (!r) return -1;
    x->raw = r;
    uint8_t (*l)[32] = realloc(x->rel, nc * sizeof *l);
    if (!l) return -1;
    x->rel = l;
    x->cap = nc;
    return 0;
}

int rxx_append_crumbs(rxl_log *log, RxWorld *w, rxx_ctx *x) {
    for (;; x->next++) {
        const RxCrumb *k = rx_world_crumb(w, x->next);
        if (!k) return x->next > w->n_crumbs ? 0 : -1;
        if (x->n != x->next - 1 || grow(x)) return -1;
        rxl_rec r;
        memset(&r, 0, sizeof r);
        r.type = RXL_CRUMB;
        rxx_crumb(k, &r.u.c);
        uint8_t d[32];
        rxl_crumb_digest(&r.u.c, (const uint8_t (*)[32])x->raw, x->n, d);
        if (memcmp(d, k->digest, 32)) {
            fprintf(stderr, "export: crumb %llu: runtime digest is not reproduced from its fields\n",
                    (unsigned long long)k->id);
            return -1;
        }
        memcpy(x->raw[x->n], k->digest, 32);
        for (uint32_t i = 0; i < r.u.c.n_caps && i < RXL_MAX_CAPS; i++) {
            if (r.u.c.caps[i].gen < x->cap_base) {
                fprintf(stderr, "export: crumb %llu: capability generation below the run's root\n",
                        (unsigned long long)k->id);
                return -1;
            }
            r.u.c.caps[i].gen -= x->cap_base;
        }
        rxl_crumb_digest(&r.u.c, (const uint8_t (*)[32])x->rel, x->n, r.u.c.digest);
        memcpy(x->rel[x->n], r.u.c.digest, 32);
        x->n++;
        if (rxl_push(log, &r)) return -1;
    }
}
