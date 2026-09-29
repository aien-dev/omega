/* Turing Yield companion records. See ty_record.h. */
#include "turing/ty_record.h"

#include "omega_canonical.h"
#include "sha256.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WHY(...)                                              \
    do {                                                      \
        if (why && whylen) snprintf(why, whylen, __VA_ARGS__); \
    } while (0)

#define CANON_CAP 40000

/* ---------------------------------------------------------------- digests */

void ty_hex(const ty_digest *d, char out[65]) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[2 * i] = hx[d->b[i] >> 4];
        out[2 * i + 1] = hx[d->b[i] & 15];
    }
    out[64] = 0;
}

int ty_parse_hex(const char *hex, ty_digest *out) {
    if (!hex || strlen(hex) < 64) return TY_E_FORMAT;
    for (int i = 0; i < 32; ++i) {
        int v = 0;
        for (int j = 0; j < 2; ++j) {
            char c = hex[2 * i + j];
            int n = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (n < 0) return TY_E_FORMAT;
            v = v * 16 + n;
        }
        out->b[i] = (uint8_t)v;
    }
    return TY_OK;
}

int ty_digest_eq(const ty_digest *a, const ty_digest *b) { return memcmp(a->b, b->b, 32) == 0; }

int ty_file_sha256(const char *path, ty_digest *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return TY_E_IO;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t *buf = malloc(1 << 20);
    if (!buf) {
        fclose(f);
        return TY_E_IO;
    }
    size_t k;
    while ((k = fread(buf, 1, 1 << 20, f)) > 0) sha256_update(&c, buf, k);
    int err = ferror(f);
    fclose(f);
    free(buf);
    if (err) return TY_E_IO;
    sha256_final(&c, out->b);
    return TY_OK;
}

/* OMG0 object builder (same layout rules as src/turing/field.c). */
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

static void ob_i64(obj_b *b, const char *key, int64_t v) {
    char t[24];
    snprintf(t, sizeof t, "%" PRId64, v);
    ob_text(b, key, t);
}

static void ob_digest(obj_b *b, const char *key, const ty_digest *d) {
    char h[65];
    ty_hex(d, h);
    ob_text(b, key, h);
}

static int ob_new(obj_b *b, SemanticKind kind, const char *record) {
    b->err = 0;
    b->o = calloc(1, sizeof(OmegaObject));
    if (!b->o) return -1;
    b->o->kind = kind;
    ob_text(b, "record", record);
    return 0;
}

static int ob_finish(obj_b *b, const char *domain, ty_digest *out) {
    int rc = TY_E_FORMAT;
    uint8_t *buf = NULL;
    size_t n = 0;
    if (!b->o || b->err) goto done;
    buf = malloc(CANON_CAP);
    if (!buf) goto done;
    if (omega_canonical_encode(b->o, buf, CANON_CAP, &n) != 0) goto done;
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)domain, strlen(domain) + 1); /* includes the 0x00 separator */
    sha256_update(&c, buf, n);
    sha256_final(&c, out->b);
    rc = TY_OK;
done:
    free(buf);
    free(b->o);
    b->o = NULL;
    return rc;
}

int ty_profile_digest(const ty_yprofile *p, ty_digest *out) {
    obj_b b;
    if (!p || !out || ob_new(&b, KIND_CONSTRAINT, "turing.yprofile") != 0) return TY_E_ARG;
    ob_text(&b, "name", p->name);
    ob_text(&b, "dataset", p->dataset);
    ob_digest(&b, "manifest", &p->manifest);
    ob_text(&b, "x_t", p->x_t);
    ob_text(&b, "side_info", p->side_info);
    ob_text(&b, "context_reset", p->context_reset);
    ob_u64(&b, "alphabet_k", p->K);
    ob_u64(&b, "qbits", p->qbits);
    ob_u64(&b, "floor_q", p->floor_q);
    ob_text(&b, "estimator", p->estimator);
    ob_text(&b, "lm_code", p->lm_code);
    ob_u64(&b, "fit_rule", p->fit_rule);
    ob_text(&b, "baseline_rule", p->baseline_rule);
    ob_u64(&b, "baseline_mask", p->baseline_mask);
    ob_text(&b, "candidate_rule", p->candidate_rule);
    ob_u64(&b, "candidate_mask", p->candidate_mask);
    ob_text(&b, "pass_rule", p->pass_rule);
    ob_i64(&b, "margin_ub", p->margin_ub);
    ob_u64(&b, "qerr_milli_ub_per_symbol", p->qerr_milli_ub);
    ob_text(&b, "energy_denominator", p->energy_denominator);
    ob_text(&b, "unit", "micro-bits (1 T = 1 bit = 1000000 ub); model lengths in whole bits");
    return ob_finish(&b, TY_DOMAIN_PROFILE, out);
}

static void file_attr(obj_b *b, const char *pre, size_t i, const char *label, const ty_digest *d) {
    char k[32], v[160], h[65];
    snprintf(k, sizeof k, "%s.%02zu", pre, i);
    ty_hex(d, h);
    snprintf(v, sizeof v, "%s %s", h, label ? label : "");
    ob_text(b, k, v);
}

int ty_split_digest(const ty_split *s, ty_digest *out) {
    obj_b b;
    if (!s || !out || s->nfit > TY_MAX_FILES || s->nheld > TY_MAX_FILES) return TY_E_ARG;
    if (ob_new(&b, KIND_EVIDENCE, "turing.ysplit") != 0) return TY_E_ARG;
    ob_u64(&b, "nfit", s->nfit);
    ob_u64(&b, "nheld", s->nheld);
    for (size_t i = 0; i < s->nfit; ++i) file_attr(&b, "fit", i, s->fit_label[i], &s->fit[i]);
    for (size_t i = 0; i < s->nheld; ++i) file_attr(&b, "held", i, s->held_label[i], &s->held[i]);
    return ob_finish(&b, TY_DOMAIN_SPLIT, out);
}

int ty_fit_digest(const ty_fit_rec *f, ty_digest *out) {
    obj_b b;
    if (!f || !out || f->nfit > TY_MAX_FILES) return TY_E_ARG;
    if (ob_new(&b, KIND_EVIDENCE, "turing.yfit") != 0) return TY_E_ARG;
    ob_digest(&b, "profile", &f->profile);
    ob_digest(&b, "split", &f->split);
    ob_text(&b, "role", f->role == TY_ROLE_BASELINE ? "baseline" : "candidate");
    ob_u64(&b, "mask", f->mask);
    ob_u64(&b, "rule", f->rule);
    ob_u64(&b, "nfit", f->nfit);
    for (size_t i = 0; i < f->nfit; ++i) file_attr(&b, "fit", i, NULL, &f->fit[i]);
    ob_digest(&b, "model", &f->model);
    ob_u64(&b, "model_bits", f->model_bits);
    return ob_finish(&b, TY_DOMAIN_FIT, out);
}

int ty_gain_digest(const ty_gain_rec *g, ty_digest *out) {
    obj_b b;
    if (!g || !out || g->nheld > TY_MAX_FILES) return TY_E_ARG;
    if (ob_new(&b, KIND_EVIDENCE, "turing.yield") != 0) return TY_E_ARG;
    ob_digest(&b, "profile", &g->profile);
    ob_digest(&b, "split", &g->split);
    ob_digest(&b, "baseline_fit", &g->baseline_fit);
    ob_digest(&b, "candidate_fit", &g->candidate_fit);
    ob_digest(&b, "baseline_model", &g->baseline_model);
    ob_digest(&b, "candidate_model", &g->candidate_model);
    ob_u64(&b, "nsym", g->nsym);
    ob_u64(&b, "lm_b_bits", g->lm_b_bits);
    ob_u64(&b, "lm_c_bits", g->lm_c_bits);
    ob_i64(&b, "ld_b_ub", g->ld_b_ub);
    ob_i64(&b, "ld_c_ub", g->ld_c_ub);
    ob_i64(&b, "dl_b_ub", g->dl_b_ub);
    ob_i64(&b, "dl_c_ub", g->dl_c_ub);
    ob_i64(&b, "t_microbits", g->t_ub);
    ob_u64(&b, "nheld", g->nheld);
    for (size_t i = 0; i < g->nheld; ++i) {
        char k[32], v[64];
        snprintf(k, sizeof k, "file.%02zu", i);
        snprintf(v, sizeof v, "%" PRIu64 " %" PRId64, g->file_nsym[i], g->file_t_ub[i]);
        ob_text(&b, k, v);
    }
    ob_i64(&b, "qerr_bound_ub", g->qerr_bound_ub);
    ob_i64(&b, "margin_ub", g->margin_ub);
    ob_text(&b, "verdict", g->verdict);
    return ob_finish(&b, TY_DOMAIN_GAIN, out);
}

/* ------------------------------------------------------------- fit + gain */

static int in_set(const ty_digest *d, const ty_digest *set, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (ty_digest_eq(d, &set[i])) return 1;
    return 0;
}

int ty_fit_files(const ty_yprofile *p, const ty_split *sp, const char *const *paths, const ty_digest *digests,
                 size_t nfiles, unsigned role, unsigned mask, ty_fit_rec *rec, uint8_t **code, size_t *ncode,
                 char *why, size_t whylen) {
    if (!p || !sp || !paths || !digests || !rec || !code || !ncode || nfiles == 0 || nfiles > TY_MAX_FILES)
        return TY_E_ARG;
    memset(rec, 0, sizeof *rec);
    ty_stream *st = calloc(nfiles, sizeof *st);
    const ty_stream **sv = calloc(nfiles, sizeof *sv);
    int rc = (st && sv) ? TY_OK : TY_E_IO;
    size_t loaded = 0;
    for (size_t i = 0; rc == TY_OK && i < nfiles; ++i) {
        ty_digest d;
        if ((rc = ty_file_sha256(paths[i], &d)) != TY_OK) {
            WHY("cannot hash %s", paths[i]);
            break;
        }
        if (!ty_digest_eq(&d, &digests[i])) {
            WHY("fit file %s does not match its manifest digest", paths[i]);
            rc = TY_E_DIGEST;
            break;
        }
        ty_stream_init(&st[i]);
        loaded = i + 1;
        int64_t n = ty_ctr1_read(paths[i], &st[i], why, whylen);
        if (n < 0) {
            rc = (int)n;
            break;
        }
        sv[i] = &st[i];
        rec->fit[i] = d;
    }
    ty_model m;
    memset(&m, 0, sizeof m);
    if (rc == TY_OK) rc = ty_model_fit(&m, sv, nfiles, mask, p->K, (int)p->fit_rule);
    for (size_t i = 0; i < loaded; ++i) ty_stream_free(&st[i]);
    free(st);
    free(sv);
    if (rc != TY_OK) return rc;
    rc = ty_model_encode(&m, code, ncode, &rec->model_bits);
    ty_model_free(&m);
    if (rc != TY_OK) return rc;
    ty_model_digest(*code, *ncode, rec->model.b);
    rec->nfit = nfiles;
    rec->role = role;
    rec->mask = mask;
    rec->rule = p->fit_rule;
    if ((rc = ty_profile_digest(p, &rec->profile)) != TY_OK) return rc;
    return ty_split_digest(sp, &rec->split);
}

static int check_rec(const ty_yprofile *p, const ty_split *sp, const ty_digest *pd, const ty_digest *sd,
                     const ty_fit_rec *r, const uint8_t *code, size_t n, ty_model *m, const char *who, char *why,
                     size_t whylen) {
    if (!ty_digest_eq(&r->profile, pd)) {
        WHY("%s was fit under another measurement profile", who);
        return TY_E_PROFILE;
    }
    if (!ty_digest_eq(&r->split, sd)) {
        WHY("%s names another split manifest", who);
        return TY_E_PROVENANCE;
    }
    for (size_t i = 0; i < r->nfit; ++i)
        if (in_set(&r->fit[i], sp->held, sp->nheld)) {
            WHY("%s was fit on a held-out file (fit.%02zu)", who, i);
            return TY_E_LEAK;
        }
    if (r->nfit != sp->nfit) {
        WHY("%s fit on %zu files, split declares %zu", who, r->nfit, sp->nfit);
        return TY_E_PROVENANCE;
    }
    for (size_t i = 0; i < r->nfit; ++i)
        if (!ty_digest_eq(&r->fit[i], &sp->fit[i])) {
            WHY("%s fit file %zu is not the split's fit file %zu", who, i, i);
            return TY_E_PROVENANCE;
        }
    if (r->rule != p->fit_rule) {
        WHY("%s fit rule %u, profile pins %u", who, r->rule, p->fit_rule);
        return TY_E_PROFILE;
    }
    ty_digest md;
    ty_model_digest(code, n, md.b);
    if (!ty_digest_eq(&md, &r->model)) {
        WHY("%s model code does not match its fit record", who);
        return TY_E_DIGEST;
    }
    uint64_t bits;
    char w[160];
    int rc = ty_model_decode(code, n, m, &bits, w, sizeof w);
    if (rc != TY_OK) {
        WHY("%s model refused: %s", who, w);
        return rc;
    }
    if (m->K != p->K || bits != r->model_bits || m->mask != r->mask) {
        WHY("%s model K/length/mask disagree with profile or fit record", who);
        ty_model_free(m);
        return TY_E_PROFILE;
    }
    return TY_OK;
}

int ty_gain_compute(const ty_yprofile *p, const ty_split *sp, const ty_fit_rec *brec, const uint8_t *bcode,
                    size_t nb, const ty_fit_rec *crec, const uint8_t *ccode, size_t nc,
                    const char *const *held_paths, ty_gain_rec *out, char *why, size_t whylen) {
    if (!p || !sp || !brec || !bcode || !crec || !ccode || !held_paths || !out || sp->nheld == 0) return TY_E_ARG;
    memset(out, 0, sizeof *out);
    ty_digest pd, sd;
    int rc = ty_profile_digest(p, &pd);
    if (rc == TY_OK) rc = ty_split_digest(sp, &sd);
    if (rc != TY_OK) return rc;
    if (p->qbits != TY_QBITS || p->floor_q != 1) {
        WHY("profile quantization/floor not supported by this scorer");
        return TY_E_PROFILE;
    }
    if (brec->role != TY_ROLE_BASELINE || brec->mask != p->baseline_mask) {
        WHY("baseline mask %u is not the profile's declared baseline (mask %u)", brec->mask, p->baseline_mask);
        return TY_E_BASELINE;
    }
    if (crec->role != TY_ROLE_CANDIDATE) return TY_E_ARG;
    ty_model mb, mc;
    if ((rc = check_rec(p, sp, &pd, &sd, brec, bcode, nb, &mb, "baseline", why, whylen)) != TY_OK) return rc;
    if ((rc = check_rec(p, sp, &pd, &sd, crec, ccode, nc, &mc, "candidate", why, whylen)) != TY_OK) {
        ty_model_free(&mb);
        return rc;
    }
    int64_t lb_ub, lc_ub;
    rc = ty_bits_to_ub(brec->model_bits, &lb_ub);
    if (rc == TY_OK) rc = ty_bits_to_ub(crec->model_bits, &lc_ub);
    for (size_t i = 0; rc == TY_OK && i < sp->nheld; ++i) {
        ty_digest d;
        if ((rc = ty_file_sha256(held_paths[i], &d)) != TY_OK) {
            WHY("cannot hash %s", held_paths[i]);
            break;
        }
        if (!ty_digest_eq(&d, &sp->held[i])) {
            WHY("held-out file %s does not match the split manifest", held_paths[i]);
            rc = TY_E_DIGEST;
            break;
        }
        ty_stream s;
        ty_stream_init(&s);
        int64_t n = ty_ctr1_read(held_paths[i], &s, why, whylen);
        int64_t ub_b = 0, ub_c = 0;
        uint64_t ns = 0;
        if (n < 0) rc = (int)n;
        if (rc == TY_OK) rc = ty_model_score(&mb, &s, &ub_b, &ns);
        if (rc == TY_OK) rc = ty_model_score(&mc, &s, &ub_c, &ns);
        ty_stream_free(&s);
        if (rc != TY_OK) break;
        int64_t fb, fc;
        rc = ty_add(lb_ub, ub_b, &fb);
        if (rc == TY_OK) rc = ty_add(lc_ub, ub_c, &fc);
        if (rc == TY_OK) rc = ty_gain(fb, fc, &out->file_t_ub[i]);
        if (rc == TY_OK) rc = ty_add(out->ld_b_ub, ub_b, &out->ld_b_ub);
        if (rc == TY_OK) rc = ty_add(out->ld_c_ub, ub_c, &out->ld_c_ub);
        out->file_nsym[i] = ns;
        out->nsym += ns;
    }
    ty_model_free(&mb);
    ty_model_free(&mc);
    if (rc != TY_OK) return rc;
    out->nheld = sp->nheld;
    out->profile = pd;
    out->split = sd;
    out->baseline_model = brec->model;
    out->candidate_model = crec->model;
    out->lm_b_bits = brec->model_bits;
    out->lm_c_bits = crec->model_bits;
    if ((rc = ty_fit_digest(brec, &out->baseline_fit)) != TY_OK) return rc;
    if ((rc = ty_fit_digest(crec, &out->candidate_fit)) != TY_OK) return rc;
    if ((rc = ty_dl(out->lm_b_bits, out->ld_b_ub, &out->dl_b_ub)) != TY_OK) return rc;
    if ((rc = ty_dl(out->lm_c_bits, out->ld_c_ub, &out->dl_c_ub)) != TY_OK) return rc;
    if ((rc = ty_gain(out->dl_b_ub, out->dl_c_ub, &out->t_ub)) != TY_OK) return rc;
    /* Both code lengths carry at most 0.501 ub per symbol of fixed-point error. */
    out->qerr_bound_ub = (int64_t)((2 * out->nsym * (uint64_t)p->qerr_milli_ub + 999) / 1000);
    out->margin_ub = p->margin_ub;
    int pass = out->t_ub - out->qerr_bound_ub > (p->margin_ub > 0 ? p->margin_ub : 0);
    for (size_t i = 0; i < out->nheld; ++i)
        if (out->file_t_ub[i] - out->qerr_bound_ub <= 0) pass = 0;
    snprintf(out->verdict, sizeof out->verdict, "%s", pass ? "PASS" : "FAIL");
    return TY_OK;
}

int ty_gain_verify(const ty_yprofile *p, const ty_split *sp, const char *const *fit_paths,
                   const char *const *held_paths, const ty_gain_rec *rec, const ty_digest *rec_digest, char *why,
                   size_t whylen) {
    if (!p || !sp || !fit_paths || !held_paths || !rec || !rec_digest) return TY_E_ARG;
    ty_digest d;
    int rc = ty_gain_digest(rec, &d);
    if (rc != TY_OK) return rc;
    if (!ty_digest_eq(&d, rec_digest)) {
        WHY("gain record does not hash to its recorded digest");
        return TY_E_DIGEST;
    }
    ty_fit_rec br, cr;
    uint8_t *bc = NULL, *cc = NULL;
    size_t nb = 0, nc = 0;
    rc = ty_fit_files(p, sp, fit_paths, sp->fit, sp->nfit, TY_ROLE_BASELINE, p->baseline_mask, &br, &bc, &nb, why,
                      whylen);
    if (rc == TY_OK)
        rc = ty_fit_files(p, sp, fit_paths, sp->fit, sp->nfit, TY_ROLE_CANDIDATE, p->candidate_mask, &cr, &cc, &nc,
                          why, whylen);
    ty_gain_rec again;
    if (rc == TY_OK) rc = ty_gain_compute(p, sp, &br, bc, nb, &cr, cc, nc, held_paths, &again, why, whylen);
    free(bc);
    free(cc);
    if (rc != TY_OK) return rc;
    ty_digest d2;
    if ((rc = ty_gain_digest(&again, &d2)) != TY_OK) return rc;
    if (!ty_digest_eq(&d2, rec_digest)) {
        WHY("re-derived gain record differs from the recorded one (T %lld vs %lld ub)", (long long)again.t_ub,
            (long long)rec->t_ub);
        return TY_E_DIGEST;
    }
    return TY_OK;
}
