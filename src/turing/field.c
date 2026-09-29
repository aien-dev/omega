/* TURING Field V0 records: canonical bytes, digests, store, adapters, verify.
 * See field.h. Plain C11. */
#include "turing/field.h"

#include "algebra/realize_common.h"
#include "omega_canonical.h"
#include "sha256.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TURING_CANON_CAP 40000

static const char *const reason_names[TURING_R__COUNT] = {
    "CHOSEN", "RANKED", "TIED", "CONTRACT_MISMATCH", "NOT_EXACT", "MAX_N", "NO_EVIDENCE",
    "RECEIPT_UNVERIFIED", "UNVERIFIED_RUN", "CONTENTION_FORCED", "TIER", "EXPLORE"};

const char *turing_reason_name(turing_reason r) {
    return (r >= 0 && r < TURING_R__COUNT) ? reason_names[r] : "?";
}

/* ---------------------------------------------------------------- digests */

void turing_hex(const turing_digest *d, char out[65]) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < TURING_DIGEST_BYTES; ++i) {
        out[2 * i] = hx[d->b[i] >> 4];
        out[2 * i + 1] = hx[d->b[i] & 15];
    }
    out[64] = 0;
}

void turing_hex_short(const turing_digest *d, char out[13]) {
    char h[65];
    turing_hex(d, h);
    memcpy(out, h, 12);
    out[12] = 0;
}

int turing_digest_eq(const turing_digest *a, const turing_digest *b) {
    return memcmp(a->b, b->b, TURING_DIGEST_BYTES) == 0;
}

int turing_file_digest(const char *path, turing_digest *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[8192];
    size_t k;
    while ((k = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&c, buf, k);
    int err = ferror(f);
    fclose(f);
    if (err) return -1;
    sha256_final(&c, out->b);
    return 0;
}

/* OMG0 object builder. */
typedef struct {
    OmegaObject *o;
    int err;
} obj_b;

static void ob_text(obj_b *b, const char *key, const char *val) {
    if (b->err) return;
    size_t kl = strlen(key), vl = strlen(val);
    if (b->o->attr_count >= OMEGA_MAX_ATTRIBUTES || kl >= OMEGA_MAX_KEY_LEN || vl > OMEGA_MAX_VAL_LEN) {
        b->err = 1;
        return;
    }
    OmegaAttribute *a = &b->o->attributes[b->o->attr_count++];
    memcpy(a->key, key, kl + 1);
    memcpy(a->value, val, vl);
    a->val_len = (uint16_t)vl;
}

static void ob_u64(obj_b *b, const char *key, uint64_t v) {
    char t[24];
    snprintf(t, sizeof t, "%" PRIu64, v);
    ob_text(b, key, t);
}

static void ob_digest(obj_b *b, const char *key, const turing_digest *d) {
    char h[65];
    turing_hex(d, h);
    ob_text(b, key, h);
}

static void ob_rel(obj_b *b, uint16_t kind, const turing_digest *d) {
    if (b->err) return;
    if (b->o->rel_count >= OMEGA_MAX_RELATIONS) {
        b->err = 1;
        return;
    }
    OmegaRelation *r = &b->o->relations[b->o->rel_count++];
    r->kind = kind;
    memcpy(r->target_id.bytes, d->b, TURING_DIGEST_BYTES);
}

static OmegaObject *ob_new(obj_b *b, SemanticKind kind, const char *record) {
    b->err = 0;
    b->o = calloc(1, sizeof(OmegaObject));
    if (!b->o) return NULL;
    b->o->kind = kind;
    ob_text(b, "record", record);
    return b->o;
}

static int ob_finish(obj_b *b, const char *domain, turing_digest *out, uint8_t *copy, size_t cap, size_t *len) {
    int rc = -1;
    uint8_t *buf = NULL;
    size_t n = 0;
    if (!b->o || b->err) goto done;
    buf = malloc(TURING_CANON_CAP);
    if (!buf) goto done;
    if (omega_canonical_encode(b->o, buf, TURING_CANON_CAP, &n) != 0) goto done;
    if (out) {
        sha256_ctx c;
        sha256_init(&c);
        sha256_update(&c, (const uint8_t *)domain, strlen(domain) + 1); /* includes the 0x00 separator */
        sha256_update(&c, buf, n);
        sha256_final(&c, out->b);
    }
    if (copy) {
        if (n > cap) goto done;
        memcpy(copy, buf, n);
        *len = n;
    }
    rc = 0;
done:
    free(buf);
    free(b->o);
    b->o = NULL;
    return rc;
}

/* --------------------------------------------------------------- contract */

void turing_contract_omega_x(turing_contract *c) {
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "Omega-X ternary GEMV");
    snprintf(c->statement, sizeof c->statement,
             "y = W.x; W in {-1,0,+1}^(m x n) canonical row-major int8; x in int8^n; y in int32^m");
    snprintf(c->exactness, sizeof c->exactness, "EXACT: bit-identical to the naive integer oracle");
    snprintf(c->overflow, sizeof c->overflow, "|y_i| <= 128*n; n <= floor(INT32_MAX/128)");
    snprintf(c->oracle, sizeof c->oracle, "oma_rz_oracle (src/algebra/realize_common.h)");
    snprintf(c->source, sizeof c->source, "spec/mixed-algebra-ma2.md");
    c->max_n = (uint64_t)OMA_RZ_MAX_N;
}

static int contract_obj(const turing_contract *c, obj_b *b) {
    if (!ob_new(b, KIND_OPERATION, "turing.contract")) return -1;
    ob_text(b, "status", "PROVISIONAL V0 contract digest; not an Omega semantic id (OSC-0B open)");
    ob_text(b, "name", c->name);
    ob_text(b, "statement", c->statement);
    ob_text(b, "exactness", c->exactness);
    ob_text(b, "overflow", c->overflow);
    ob_text(b, "oracle", c->oracle);
    ob_text(b, "source", c->source);
    ob_u64(b, "max_n", c->max_n);
    return 0;
}

int turing_contract_digest(const turing_contract *c, turing_digest *out) {
    obj_b b;
    if (!c || contract_obj(c, &b) != 0) return -1;
    return ob_finish(&b, TURING_DOMAIN_CONTRACT, out, NULL, 0, NULL);
}

/* ------------------------------------------------------------------- spec */

static int spec_obj(const turing_rz_spec *s, obj_b *b) {
    if (!ob_new(b, KIND_REALIZATION, "turing.rz_spec")) return -1;
    ob_digest(b, "contract", &s->contract_digest);
    ob_text(b, "rz_id", s->rz_id);
    ob_text(b, "algorithm", s->algorithm);
    ob_text(b, "algebra", s->algebra);
    ob_text(b, "representation", s->representation);
    ob_text(b, "precision", s->precision);
    ob_text(b, "language", s->language);
    ob_text(b, "backend", s->backend);
    ob_text(b, "machine_class", s->machine_class);
    ob_u64(b, "max_n", s->max_n);
    ob_u64(b, "exact", (uint64_t)s->exact);
    ob_u64(b, "weak_baseline", (uint64_t)s->weak_baseline);
    return 0;
}

int turing_spec_digest(const turing_rz_spec *s, turing_digest *out) {
    obj_b b;
    if (!s || spec_obj(s, &b) != 0) return -1;
    return ob_finish(&b, TURING_DOMAIN_SPEC, out, NULL, 0, NULL);
}

int turing_spec_bytes(const turing_rz_spec *s, uint8_t *buf, size_t cap, size_t *len) {
    obj_b b;
    if (!s || !buf || !len || spec_obj(s, &b) != 0) return -1;
    return ob_finish(&b, TURING_DOMAIN_SPEC, NULL, buf, cap, len);
}

static const struct {
    const char *family, *algebra, *representation;
} family_map[] = {
    {"binary", "two's complement integer (int8 weights)", "int8 per weight"},
    {"bitplane", "balanced ternary", "pos/neg bitplanes, 2 bits per weight"},
    {"lut", "balanced ternary", "4-activation subset-sum LUT codes, 2 bits per weight"},
    {"crumb2", "balanced ternary", "2-bit two's-complement crumbs"},
    {"sparse", "balanced ternary (signed index sets)", "uint16 +1/-1 index lists per row"},
    {"rns", "residue number system {256,255,253}", "int8 weights, per-call residues of x"},
    {"dense5", "balanced ternary", "5 trits per byte, 1.6 bits per weight"},
};

int turing_spec_from_fields(const turing_digest *contract, const char *rz_id, const char *family,
                            int exact, int weak_baseline, uint64_t max_n, turing_rz_spec *s) {
    if (!contract || !rz_id || !family || !s || strlen(rz_id) >= sizeof s->rz_id) return -1;
    size_t k = sizeof family_map / sizeof family_map[0], i;
    for (i = 0; i < k; ++i)
        if (strcmp(family_map[i].family, family) == 0) break;
    if (i == k) return -1;
    memset(s, 0, sizeof *s);
    s->contract_digest = *contract;
    snprintf(s->rz_id, sizeof s->rz_id, "%s", rz_id);
    snprintf(s->algorithm, sizeof s->algorithm, "oma_rz_impl %s (pack once, run per x)", rz_id);
    snprintf(s->algebra, sizeof s->algebra, "%s", family_map[i].algebra);
    snprintf(s->representation, sizeof s->representation, "%s", family_map[i].representation);
    snprintf(s->precision, sizeof s->precision, "int8 x trit -> int32, exact");
    snprintf(s->language, sizeof s->language, "C11 + ACLE NEON/I8MM intrinsics");
    snprintf(s->backend, sizeof s->backend, "cpu, single core, single thread");
    snprintf(s->machine_class, sizeof s->machine_class, "aarch64 armv8.6-a+dotprod+i8mm+sve");
    s->max_n = max_n;
    s->exact = exact;
    s->weak_baseline = weak_baseline;
    return 0;
}

/* --------------------------------------------------------------- evidence */

static int evidence_obj(const turing_evidence *e, obj_b *b) {
    if (!ob_new(b, KIND_EVIDENCE, "turing.evidence")) return -1;
    ob_digest(b, "spec", &e->spec_id);
    ob_u64(b, "n", e->n);
    ob_u64(b, "m", e->m);
    ob_u64(b, "sparsity_milli", e->sparsity_milli);
    ob_digest(b, "receipt", &e->receipt_digest);
    ob_text(b, "run_id", e->run_id);
    ob_text(b, "tier", e->tier);
    ob_text(b, "tier_source", e->tier_source);
    ob_u64(b, "verified", (uint64_t)e->verified);
    ob_u64(b, "forced", e->forced);
    ob_u64(b, "retries", e->retries);
    ob_u64(b, "samples", e->samples);
    ob_u64(b, "median_ps", e->median_ps);
    ob_u64(b, "q25_ps", e->q25_ps);
    ob_u64(b, "q75_ps", e->q75_ps);
    ob_u64(b, "min_ps", e->min_ps);
    ob_u64(b, "pack_ps", e->pack_ps);
    ob_u64(b, "noise_ppm", e->noise_ppm);
    ob_u64(b, "pack_noise_ppm", e->pack_noise_ppm);
    ob_u64(b, "weight_bytes", e->weight_bytes);
    ob_u64(b, "working_set_bytes", e->working_set_bytes);
    ob_text(b, "build_digest", e->build_digest);
    ob_text(b, "git_commit", e->git_commit);
    ob_u64(b, "tree_dirty", (uint64_t)e->tree_dirty);
    ob_text(b, "toolchain", e->toolchain);
    ob_text(b, "cpu_freq_state", e->cpu_freq_state);
    ob_text(b, "quiet_flag", e->quiet_flag);
    ob_text(b, "thermal", e->thermal);
    return 0;
}

int turing_evidence_digest(const turing_evidence *e, turing_digest *out) {
    obj_b b;
    if (!e || evidence_obj(e, &b) != 0) return -1;
    return ob_finish(&b, TURING_DOMAIN_EVIDENCE, out, NULL, 0, NULL);
}

uint64_t turing_ev_cost(const turing_evidence *e, int pack) {
    return e->median_ps + (pack == TURING_PACK_PER_CALL ? e->pack_ps : 0);
}

/* --------------------------------------------------------------- decision */

static int decision_obj(const turing_decision *d, obj_b *b) {
    if (d->ncand > TURING_MAX_CAND || d->ncite > TURING_MAX_CITE) return -1;
    if (!ob_new(b, KIND_EVIDENCE, "turing.decision")) return -1;
    ob_digest(b, "contract", &d->contract_digest);
    ob_text(b, "selector", d->selector);
    ob_text(b, "constraints", d->constraints);
    ob_u64(b, "n", d->n);
    ob_u64(b, "m", d->m);
    ob_u64(b, "sparsity_milli", d->sparsity_milli);
    ob_text(b, "pack", d->pack == TURING_PACK_PER_CALL ? "per_call" : "once_amortized");
    ob_u64(b, "cell_n", d->cell_n);
    ob_u64(b, "cell_m", d->cell_m);
    ob_u64(b, "cell_sparsity_milli", d->cell_sparsity_milli);
    ob_u64(b, "exact_cell", (uint64_t)d->exact_cell);
    char list[OMEGA_MAX_VAL_LEN + 1];
    size_t pos = 0;
    list[0] = 0;
    for (size_t i = 0; i < d->ncand; ++i) {
        int w = snprintf(list + pos, sizeof list - pos, "%s%s=%s:%" PRIu64, i ? ";" : "", d->cand_rz[i],
                         turing_reason_name(d->reason[i]), d->cost_ps[i]);
        if (w < 0 || (size_t)w >= sizeof list - pos) return b->err = 1, -1;
        pos += (size_t)w;
    }
    ob_text(b, "candidates", list);
    ob_text(b, "chosen", d->chosen >= 0 && (size_t)d->chosen < d->ncand ? d->cand_rz[d->chosen] : "-");
    ob_text(b, "verdict", d->verdict);
    ob_text(b, "tie_resolution", d->tie_resolution);
    ob_u64(b, "margin_ppm", d->margin_ppm);
    ob_u64(b, "band_ppm", d->band_ppm);
    if (d->has_supersedes)
        ob_digest(b, "supersedes", &d->supersedes);
    else
        ob_text(b, "supersedes", "none");
    for (size_t i = 0; i < d->ncand; ++i) ob_rel(b, REL_DEPENDS_ON, &d->cand[i]);
    for (size_t i = 0; i < d->ncite; ++i) ob_rel(b, REL_DERIVED_FROM, &d->cite[i]);
    return 0;
}

int turing_decision_digest(const turing_decision *d, turing_digest *out) {
    obj_b b;
    b.o = NULL;
    if (!d || decision_obj(d, &b) != 0) {
        free(b.o);
        return -1;
    }
    return ob_finish(&b, TURING_DOMAIN_DECISION, out, NULL, 0, NULL);
}

/* ------------------------------------------------------------------ store */

turing_store *turing_store_new(void) {
    turing_store *st = calloc(1, sizeof *st);
    if (!st) return NULL;
    turing_contract_omega_x(&st->contract);
    if (turing_contract_digest(&st->contract, &st->contract_digest) != 0) {
        free(st);
        return NULL;
    }
    return st;
}

void turing_store_free(turing_store *st) { free(st); }

int turing_find_spec(const turing_store *st, const char *rz_id) {
    for (size_t i = 0; i < st->nspec; ++i)
        if (strcmp(st->spec[i].rz_id, rz_id) == 0) return (int)i;
    return -1;
}

int turing_find_evidence(const turing_store *st, const turing_digest *id) {
    for (size_t i = 0; i < st->nev; ++i)
        if (turing_digest_eq(&st->ev_id[i], id)) return (int)i;
    return -1;
}

/* Add a spec; an existing spec with the same rz_id must have the same id. */
static int store_add_spec(turing_store *st, const turing_rz_spec *s) {
    turing_digest id;
    if (turing_spec_digest(s, &id) != 0) return -1;
    int k = turing_find_spec(st, s->rz_id);
    if (k >= 0) return turing_digest_eq(&st->spec_id[k], &id) ? k : -1;
    if (st->nspec >= TURING_MAX_SPECS) return -1;
    st->spec[st->nspec] = *s;
    st->spec_id[st->nspec] = id;
    return (int)st->nspec++;
}

int turing_ingest_registry(turing_store *st) {
    size_t n = oma_rz_count();
    for (size_t i = 0; i < n; ++i) {
        const oma_rz_impl *r = oma_rz_get(i);
        turing_rz_spec s;
        if (!r || turing_spec_from_fields(&st->contract_digest, r->id, r->family, r->exact, r->weak_baseline,
                                          (uint64_t)r->max_n, &s) != 0)
            return -1;
        if (store_add_spec(st, &s) < 0) return -1;
    }
    return (int)n;
}

/* ---------------------------------------------------- receipt line parser */

/* Find `"key": ` in line; return pointer to the value or NULL. */
static const char *jfind(const char *line, const char *key) {
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\": ", key);
    const char *p = strstr(line, pat);
    return p ? p + strlen(pat) : NULL;
}

static int jstr(const char *line, const char *key, char *out, size_t cap) {
    const char *p = jfind(line, key);
    if (!p || *p != '"') return -1;
    ++p;
    size_t k = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) ++p;
        if (k + 1 >= cap) return -1;
        out[k++] = *p++;
    }
    if (*p != '"') return -1;
    out[k] = 0;
    return 0;
}

static int jnum(const char *line, const char *key, double *out) {
    const char *p = jfind(line, key);
    if (!p) return -1;
    char *end;
    errno = 0;
    double v = strtod(p, &end);
    if (end == p || errno) return -1;
    *out = v;
    return 0;
}

static int ju64(const char *line, const char *key, uint64_t *out) {
    double v;
    if (jnum(line, key, &v) != 0 || v < 0 || v > 1.8e19) return -1;
    *out = (uint64_t)llround(v);
    return 0;
}

static int jbool(const char *line, const char *key, int *out) {
    const char *p = jfind(line, key);
    if (!p) return -1;
    if (strncmp(p, "true", 4) == 0) return *out = 1, 0;
    if (strncmp(p, "false", 5) == 0) return *out = 0, 0;
    return -1;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap + 1);
    size_t k;
    while (buf && (k = fread(buf + n, 1, cap - n, f)) > 0) {
        n += k;
        if (n == cap) {
            char *nb = realloc(buf, cap * 2 + 1);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
            cap *= 2;
        }
    }
    fclose(f);
    if (!buf) return NULL;
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

int turing_ingest_receipt(turing_store *st, const char *path) {
    turing_digest rd;
    if (turing_file_digest(path, &rd) != 0) return -1;
    char *text = read_file(path, NULL);
    if (!text) return -1;
    int added = 0, rc = -1, section = 0; /* 0 header, 1 realizations, 2 cost_table, 3 after */
    char schema[TURING_TEXT] = "", run_id[TURING_TEXT] = "", commit[48] = "", bin[72] = "", cc[TURING_TEXT] = "";
    char thermal[TURING_TEXT] = "not recorded";
    int dirty = -1;
    uint64_t mismatches = UINT64_MAX;
    for (char *line = text; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (section == 0) {
            (void)jstr(line, "schema", schema, sizeof schema);
            (void)jstr(line, "run_id", run_id, sizeof run_id);
            (void)jstr(line, "run_commit", commit, sizeof commit);
            (void)jstr(line, "bench_binary_sha256", bin, sizeof bin);
            (void)jstr(line, "compiler", cc, sizeof cc);
            (void)jbool(line, "tree_dirty", &dirty);
            uint64_t t0, t1;
            if (strstr(line, "\"host\": ") && ju64(line, "thermal_c_start", &t0) == 0 &&
                ju64(line, "thermal_c_end", &t1) == 0)
                snprintf(thermal, sizeof thermal, "SoC %" PRIu64 " C at start, %" PRIu64 " C at end", t0, t1);
            if (strncmp(line, "  \"realizations\": [", 19) == 0) section = 1;
        } else if (section == 1) {
            char id[32], fam[32];
            uint64_t ex, wb, mx;
            if (jstr(line, "id", id, sizeof id) == 0) {
                if (jstr(line, "family", fam, sizeof fam) != 0 || ju64(line, "exact", &ex) != 0 ||
                    ju64(line, "weak_baseline", &wb) != 0 || ju64(line, "max_n", &mx) != 0)
                    goto out;
                turing_rz_spec s;
                if (turing_spec_from_fields(&st->contract_digest, id, fam, (int)ex, (int)wb, mx, &s) != 0 ||
                    store_add_spec(st, &s) < 0)
                    goto out; /* registry and receipt disagree on the spec: refuse */
            } else if (strncmp(line, "  ],", 4) == 0) {
                section = 2;
            }
        } else if (section == 2) {
            if (strncmp(line, "  \"correctness\"", 15) == 0) (void)ju64(line, "mismatches", &mismatches);
            if (strncmp(line, "  \"energy\"", 10) == 0) {
                section = 3;
            } else if (jfind(line, "rz") && jfind(line, "median_ps")) {
                char rz[32];
                double sp, noise, pnoise;
                turing_evidence e;
                memset(&e, 0, sizeof e);
                uint64_t forced, retries, samples;
                if (jstr(line, "rz", rz, sizeof rz) != 0 || ju64(line, "n", &e.n) != 0 ||
                    ju64(line, "m", &e.m) != 0 || jnum(line, "sparsity", &sp) != 0 ||
                    jbool(line, "verified", &e.verified) != 0 || ju64(line, "forced", &forced) != 0 ||
                    ju64(line, "retries", &retries) != 0 || ju64(line, "samples", &samples) != 0 ||
                    ju64(line, "median_ps", &e.median_ps) != 0 || ju64(line, "q25_ps", &e.q25_ps) != 0 ||
                    ju64(line, "q75_ps", &e.q75_ps) != 0 || ju64(line, "min_ps", &e.min_ps) != 0 ||
                    ju64(line, "pack_ps", &e.pack_ps) != 0 || jnum(line, "noise_rel", &noise) != 0 ||
                    jnum(line, "pack_noise_rel", &pnoise) != 0 ||
                    ju64(line, "weight_bytes", &e.weight_bytes) != 0 ||
                    ju64(line, "working_set_bytes", &e.working_set_bytes) != 0)
                    goto out;
                int k = turing_find_spec(st, rz);
                if (k < 0 || st->nev >= TURING_MAX_EVIDENCE) goto out;
                e.spec_id = st->spec_id[k];
                e.sparsity_milli = (uint32_t)llround(sp * 1000.0);
                e.forced = (uint32_t)forced;
                e.retries = (uint32_t)retries;
                e.samples = (uint32_t)samples;
                e.noise_ppm = (uint32_t)llround(noise * 1e6);
                e.pack_noise_ppm = (uint32_t)llround(pnoise * 1e6);
                e.receipt_digest = rd;
                snprintf(e.receipt_path, sizeof e.receipt_path, "%s", path);
                snprintf(e.run_id, sizeof e.run_id, "%s", run_id);
                /* E2 = differential vs the oracle with declared coverage
                 * (spec/mixed-algebra-ma2.md); E0 when this row or the run
                 * reports a mismatch. Set after the correctness line below. */
                snprintf(e.tier_source, sizeof e.tier_source, "%s", TURING_TIER_SOURCE);
                snprintf(e.build_digest, sizeof e.build_digest, "%s", bin);
                snprintf(e.git_commit, sizeof e.git_commit, "%s", commit);
                e.tree_dirty = dirty;
                snprintf(e.toolchain, sizeof e.toolchain, "%s", cc);
                snprintf(e.cpu_freq_state, sizeof e.cpu_freq_state,
                         "not recorded by this receipt (MA-3 bench has no cpufreq capture)");
                snprintf(e.quiet_flag, sizeof e.quiet_flag, "not recorded by this receipt");
                snprintf(e.thermal, sizeof e.thermal, "%s", thermal);
                st->ev[st->nev++] = e;
                ++added;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    if (strncmp(schema, "OMEGA_MIXED_ALGEBRA_MA", 22) != 0 || !strstr(schema, "_BENCH_V1") || !run_id[0] ||
        section != 3 || added == 0 || dirty < 0)
        goto out;
    /* Tier and digests once the whole receipt is read. */
    for (size_t i = st->nev - (size_t)added; i < st->nev; ++i) {
        turing_evidence *e = &st->ev[i];
        snprintf(e->tier, sizeof e->tier, "%s", (mismatches == 0 && e->verified) ? "E2" : "E0");
        if (turing_evidence_digest(e, &st->ev_id[i]) != 0) goto out;
    }
    if (st->nreceipt < TURING_MAX_RECEIPTS) {
        turing_receipt_ref *r = &st->receipt[st->nreceipt++];
        r->digest = rd;
        snprintf(r->path, sizeof r->path, "%s", path);
        snprintf(r->run_id, sizeof r->run_id, "%s", run_id);
    }
    rc = added;
out:
    if (rc < 0) st->nev -= (size_t)added; /* drop a half-read receipt */
    free(text);
    return rc;
}

/* ----------------------------------------------------------------- verify */

int turing_decision_verify(const turing_store *st, const turing_decision *d, char *why, size_t wl) {
    if (!st || !d) return -1;
    if (!turing_digest_eq(&d->contract_digest, &st->contract_digest)) {
        snprintf(why, wl, "decision names another contract");
        return -2;
    }
    if (d->ncite == 0) {
        snprintf(why, wl, "decision cites no evidence");
        return -3;
    }
    const turing_evidence *checked[TURING_MAX_RECEIPTS];
    size_t nchecked = 0;
    for (size_t i = 0; i < d->ncite; ++i) {
        char h[13];
        turing_hex_short(&d->cite[i], h);
        int k = turing_find_evidence(st, &d->cite[i]);
        if (k < 0) {
            snprintf(why, wl, "cited evidence %s not in the store", h);
            return -4;
        }
        turing_digest re;
        if (turing_evidence_digest(&st->ev[k], &re) != 0 || !turing_digest_eq(&re, &d->cite[i])) {
            snprintf(why, wl, "evidence %s no longer matches its digest", h);
            return -5;
        }
        int in_cand = 0;
        for (size_t c = 0; c < d->ncand; ++c) in_cand |= turing_digest_eq(&d->cand[c], &st->ev[k].spec_id);
        if (!in_cand) {
            snprintf(why, wl, "evidence %s is for a spec that is not a candidate", h);
            return -6;
        }
        int cached = 0;
        for (size_t j = 0; j < nchecked; ++j)
            cached |= !strcmp(checked[j]->receipt_path, st->ev[k].receipt_path) &&
                      turing_digest_eq(&checked[j]->receipt_digest, &st->ev[k].receipt_digest);
        if (cached) continue; /* same receipt file already re-hashed in this call */
        turing_digest fd;
        if (turing_file_digest(st->ev[k].receipt_path, &fd) != 0) {
            snprintf(why, wl, "receipt %s unreadable", st->ev[k].receipt_path);
            return -7;
        }
        if (!turing_digest_eq(&fd, &st->ev[k].receipt_digest)) {
            snprintf(why, wl, "receipt %s bytes do not match the cited digest", st->ev[k].receipt_path);
            return -8;
        }
        if (nchecked < TURING_MAX_RECEIPTS) checked[nchecked++] = &st->ev[k];
    }
    if (why && wl) snprintf(why, wl, "ok");
    return 0;
}
