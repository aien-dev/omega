/* TURING companion records for continuous-value evidence. See ty_qrecord.h. */
#define _POSIX_C_SOURCE 200809L
#include "turing/ty_qrecord.h"

#include "omega_canonical.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------------
 * Builder helpers ob_new, ob_text, ob_u64, ob_i64, ob_digest and ob_finish
 * are copied from src/turing/ty_record.c (same OMG0 layout rules as
 * src/turing/field.c). Changes: the builder also carries a refusal reason,
 * and ob_finish can hand back the canonical bytes. ty_record.c is unchanged.
 * ---------------------------------------------------------------------- */
typedef struct {
    OmegaObject *o;
    int err;
    char why[160];
} obj_b;

static void ob_fail(obj_b *b, const char *fmt, const char *key) {
    if (b->err) return;
    b->err = 1;
    snprintf(b->why, sizeof b->why, fmt, key);
}

static void ob_text(obj_b *b, const char *key, const char *val) {
    if (b->err) return;
    size_t kl = strlen(key), vl = strlen(val);
    if (b->o->attr_count >= OMEGA_MAX_ATTRIBUTES || kl >= OMEGA_MAX_KEY_LEN || vl > OMEGA_MAX_VAL_LEN) {
        ob_fail(b, "attribute %s: too many attributes, key >= 64 bytes or value > 512 bytes", key);
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
    b->why[0] = 0;
    b->o = calloc(1, sizeof(OmegaObject));
    if (!b->o) return -1;
    b->o->kind = kind;
    ob_text(b, "record", record);
    return 0;
}

/* bytes/blen may be NULL. */
static int ob_finish(obj_b *b, const char *domain, ty_digest *out, uint8_t *bytes, size_t bcap, size_t *blen) {
    int rc = TY_E_FORMAT;
    uint8_t *buf = NULL;
    size_t n = 0;
    if (!b->o || b->err) goto done;
    buf = malloc(TYQR_CANON_CAP);
    if (!buf) goto done;
    if (omega_canonical_encode(b->o, buf, TYQR_CANON_CAP, &n) != 0) {
        snprintf(b->why, sizeof b->why, "canonical encoding failed");
        goto done;
    }
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)domain, strlen(domain) + 1); /* includes the 0x00 separator */
    sha256_update(&c, buf, n);
    sha256_final(&c, out->b);
    if (bytes) {
        if (n > bcap) goto done;
        memcpy(bytes, buf, n);
        *blen = n;
    }
    rc = TY_OK;
done:
    free(buf);
    free(b->o);
    b->o = NULL;
    return rc;
}

/* ------------------------------------------------------- field validation */

static int is_zero(const ty_digest *d) {
    for (int i = 0; i < 32; ++i)
        if (d->b[i]) return 0;
    return 1;
}

/* NUL-terminated within `cap` bytes, length 1..512, printable ASCII. */
static int text_ok(const char *s, size_t cap) {
    const char *z = memchr(s, 0, cap);
    if (!z) return 0;
    size_t n = (size_t)(z - s);
    if (n < 1 || n > OMEGA_MAX_VAL_LEN) return 0;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) return 0;
    return 1;
}

static int list_ok(const char *s, size_t cap) {
    if (!text_ok(s, cap)) return 0;
    size_t n = strlen(s);
    if (s[0] == ' ' || s[n - 1] == ' ') return 0;
    for (size_t i = 1; i < n; ++i)
        if (s[i] == ' ' && s[i - 1] == ' ') return 0;
    return 1;
}

static int in_set(const char *s, const char *const *set) {
    for (; *set; ++set)
        if (strcmp(s, *set) == 0) return 1;
    return 0;
}

static int hex40_ok(const char *s, size_t cap) {
    if (!memchr(s, 0, cap) || strlen(s) != 40) return 0;
    for (int i = 0; i < 40; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

/* A cell or item name: printable ASCII without space. */
static int name_ok(const char *s, size_t cap) {
    if (!text_ok(s, cap)) return 0;
    return strchr(s, ' ') == NULL;
}

#define REFUSE(b, ...)                                                        \
    do {                                                                      \
        if (!(b)->err) {                                                      \
            (b)->err = 1;                                                     \
            snprintf((b)->why, sizeof (b)->why, __VA_ARGS__);                 \
        }                                                                     \
    } while (0)

static void t_text(obj_b *b, const char *key, const char *v, size_t cap) {
    if (!text_ok(v, cap)) REFUSE(b, "field %s: not printable ASCII text of 1..512 bytes", key);
    else ob_text(b, key, v);
}
static void t_list(obj_b *b, const char *key, const char *v, size_t cap) {
    if (!list_ok(v, cap)) REFUSE(b, "field %s: not a one-space-separated list of 1..512 bytes", key);
    else ob_text(b, key, v);
}
static void t_enum(obj_b *b, const char *key, const char *v, size_t cap, const char *const *set) {
    if (!text_ok(v, cap) || !in_set(v, set)) REFUSE(b, "field %s: value outside its allowed set", key);
    else ob_text(b, key, v);
}
static void t_name(obj_b *b, const char *key, const char *v, size_t cap) {
    if (!name_ok(v, cap)) REFUSE(b, "field %s: not a space-free printable name", key);
    else ob_text(b, key, v);
}

static const char *const SET_MODE_W[] = {"dev", "certification", "discovery", NULL};
static const char *const SET_MODE_F[] = {"certification", "discovery", NULL};
static const char *const SET_PARENT[] = {"dev", "sealed", NULL};
static const char *const SET_LABEL[] = {"POSITIVE", "NEGATIVE", "AMBIGUOUS", NULL};
static const char *const SET_CHECK[] = {"AGREE", "STOP", NULL};
static const char *const SET_PF[] = {"PASS", "FAIL", NULL};
static const char *const SET_LEAK[] = {"0", "1", "INCONCLUSIVE", NULL};
static const char *const SET_CLASS[] = {"POSITIVE", "NEGATIVE", "AMBIGUOUS", "WEAK", "STOPPED", NULL};

/* ----------------------------------------------------------------- qprofile */

static int enc_profile(const tyqr_profile *r, ty_digest *out, uint8_t *by, size_t bc, size_t *bl, char *why, size_t wl) {
    obj_b b;
    if (!r || !out || ob_new(&b, KIND_CONSTRAINT, "turing.qprofile") != 0) return TY_E_ARG;
    const size_t V = TYQR_VAL;
    t_text(&b, "profile", r->profile, V);
    ob_digest(&b, "spec", &r->spec);
    ob_digest(&b, "packet", &r->packet);
    t_text(&b, "protocol", r->protocol, V);
    t_text(&b, "quantizer", r->quantizer, V);
    ob_i64(&b, "delta_log2", r->delta_log2);
    t_text(&b, "edge_rule", r->edge_rule, V);
    ob_i64(&b, "sd_min_log2", r->sd_min_log2);
    t_text(&b, "floor_rule", r->floor_rule, V);
    t_list(&b, "families", r->families, V);
    ob_u64(&b, "kmax", r->kmax);
    ob_u64(&b, "n_points", r->n_points);
    ob_u64(&b, "split", r->split);
    ob_digest(&b, "cells", &r->cells);
    t_text(&b, "rider", r->rider, V);
    t_text(&b, "lm_rule", r->lm_rule, V);
    t_list(&b, "baselines", r->baselines, V);
    ob_digest(&b, "thresholds", &r->thresholds);
    t_text(&b, "thresholds_format", r->thresholds_format, V);
    t_text(&b, "gen_version", r->gen_version, V);
    ob_u64(&b, "bootstrap_b", r->bootstrap_b);
    t_text(&b, "unit", r->unit, V);
    if (why && wl) snprintf(why, wl, "%s", b.why);
    return ob_finish(&b, TYQR_DOMAIN_PROFILE, out, by, bc, bl);
}

/* ------------------------------------------------------------------- qworld */

static int enc_world(const tyqr_world *r, ty_digest *out, uint8_t *by, size_t bc, size_t *bl, char *why, size_t wl) {
    obj_b b;
    if (!r || !out || ob_new(&b, KIND_EVIDENCE, "turing.qworld") != 0) return TY_E_ARG;
    const size_t V = TYQR_VAL;
    ob_digest(&b, "qprofile", &r->qprofile);
    t_enum(&b, "mode", r->mode, V, SET_MODE_W);
    t_enum(&b, "parent_kind", r->parent_kind, V, SET_PARENT);
    if (!b.err) {
        int dev_mode = strcmp(r->mode, "dev") == 0, dev_parent = strcmp(r->parent_kind, "dev") == 0;
        if (dev_mode != dev_parent) REFUSE(&b, "mode and parent_kind disagree (dev iff dev)");
        else if (dev_parent != is_zero(&r->qfreeze)) REFUSE(&b, "qfreeze must be all zero iff parent_kind is dev");
    }
    ob_digest(&b, "qfreeze", &r->qfreeze);
    t_name(&b, "cell", r->cell, V);
    ob_u64(&b, "block", r->block);
    t_text(&b, "label", r->label, V);
    t_text(&b, "gen_version", r->gen_version, V);
    ob_digest(&b, "generator", &r->generator);
    ob_u64(&b, "rep_first", r->rep_first);
    ob_u64(&b, "rep_count", r->rep_count);
    ob_digest(&b, "traj_manifest", &r->traj_manifest);
    t_text(&b, "traj_format", r->traj_format, V);
    if (why && wl) snprintf(why, wl, "%s", b.why);
    return ob_finish(&b, TYQR_DOMAIN_WORLD, out, by, bc, bl);
}

/* ------------------------------------------------------------------ qfreeze */

static int enc_freeze(const tyqr_freeze *r, ty_digest *out, uint8_t *by, size_t bc, size_t *bl, char *why, size_t wl) {
    obj_b b;
    if (!r || !out || ob_new(&b, KIND_EVIDENCE, "turing.qfreeze") != 0) return TY_E_ARG;
    const size_t V = TYQR_VAL;
    t_enum(&b, "mode", r->mode, V, SET_MODE_F);
    int cert = !b.err && strcmp(r->mode, "certification") == 0;
    ob_digest(&b, "artifact", &r->artifact);
    t_text(&b, "artifact_kind", r->artifact_kind, V);
    if (!b.err && cert != is_zero(&r->artifact_bin)) REFUSE(&b, "artifact_bin must be all zero iff certification");
    ob_digest(&b, "artifact_bin", &r->artifact_bin);
    ob_digest(&b, "qprofile", &r->qprofile);
    ob_digest(&b, "dev_manifest", &r->dev_manifest);
    t_enum(&b, "b2_emerging_label", r->b2_emerging_label, V, SET_LABEL);
    t_enum(&b, "bayes_plugin_check", r->bayes_plugin_check, V, SET_CHECK);
    t_list(&b, "diag_fw_rate_ppm", r->diag_fw_rate_ppm, V);
    t_enum(&b, "lengthening_test", r->lengthening_test, V, SET_PF);
    t_enum(&b, "stability_test", r->stability_test, V, SET_PF);
    t_text(&b, "calibration_extra", r->calibration_extra, V);
    if (!hex40_ok(r->run_commit, sizeof r->run_commit)) REFUSE(&b, "field run_commit: not 40 lowercase hex");
    else ob_text(&b, "run_commit", r->run_commit);
    if (r->tree_dirty > 1) REFUSE(&b, "field tree_dirty: not 0 or 1");
    ob_u64(&b, "tree_dirty", r->tree_dirty);
    t_list(&b, "base_commits", r->base_commits, V);
    t_text(&b, "producer_tool", r->producer_tool, V);
    t_text(&b, "producer_model", r->producer_model, V);
    if (!b.err && cert) {
        if (strcmp(r->producer_tool, "none") || strcmp(r->producer_model, "none") ||
            !is_zero(&r->prompt_digest) || !is_zero(&r->transcript_digest))
            REFUSE(&b, "certification: producer fields must be none and prompt/transcript all zero");
    }
    if (!b.err && !cert && (is_zero(&r->prompt_digest) || is_zero(&r->transcript_digest)))
        REFUSE(&b, "discovery: prompt and transcript digests must be present");
    ob_digest(&b, "prompt_digest", &r->prompt_digest);
    ob_digest(&b, "transcript_digest", &r->transcript_digest);
    if (r->n_lineage > TYQR_MAX_LINEAGE) REFUSE(&b, "field n_lineage: more than 8 predecessors (stop condition)");
    ob_u64(&b, "n_lineage", r->n_lineage);
    for (uint64_t i = 0; !b.err && i < r->n_lineage; ++i) {
        char k[16];
        snprintf(k, sizeof k, "lineage.%02u", (unsigned)i);
        ob_digest(&b, k, &r->lineage[i]);
    }
    if (why && wl) snprintf(why, wl, "%s", b.why);
    return ob_finish(&b, TYQR_DOMAIN_FREEZE, out, by, bc, bl);
}

/* -------------------------------------------------------------------- qgain */

static void ob_list_i64(obj_b *b, const char *key, const int64_t *v, size_t n) {
    char t[TYQR_VAL];
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        int k = snprintf(t + o, sizeof t - o, i ? " %" PRId64 : "%" PRId64, v[i]);
        if (k < 0 || (size_t)k >= sizeof t - o) {
            REFUSE(b, "field %s: list too long", key);
            return;
        }
        o += (size_t)k;
    }
    ob_text(b, key, t);
}

static int enc_gain(const tyqr_gain *r, ty_digest *out, uint8_t *by, size_t bc, size_t *bl, char *why, size_t wl) {
    obj_b b;
    if (!r || !out || ob_new(&b, KIND_EVIDENCE, "turing.qgain") != 0) return TY_E_ARG;
    const size_t V = TYQR_VAL;
    ob_digest(&b, "qprofile", &r->qprofile);
    ob_digest(&b, "qworld", &r->qworld);
    ob_digest(&b, "qfreeze", &r->qfreeze);
    t_enum(&b, "mode", r->mode, V, SET_MODE_F);
    t_name(&b, "cell", r->cell, V);
    t_text(&b, "gen_version", r->gen_version, V);
    if (r->baseline_opcode < 1 || r->baseline_opcode > 7) REFUSE(&b, "field baseline: opcode outside 1..7");
    if (!b.err) {
        char h[65], t[96];
        ty_hex(&r->baseline_artifact, h);
        snprintf(t, sizeof t, "%" PRIu64 " %s", r->baseline_opcode, h);
        ob_text(&b, "baseline", t);
    }
    ob_digest(&b, "candidate", &r->candidate);
    if (!b.err) {
        int disc = strcmp(r->mode, "discovery") == 0;
        int ok = disc ? r->candidate_opcode == 255 : (r->candidate_opcode >= 1 && r->candidate_opcode <= 7);
        if (!ok) REFUSE(&b, "field candidate_opcode: 1..7 for certification, 255 for discovery");
    }
    ob_u64(&b, "candidate_opcode", r->candidate_opcode);
    ob_digest(&b, "prd_b", &r->prd_b);
    ob_digest(&b, "prd_m", &r->prd_m);
    if (r->official > 1) REFUSE(&b, "field official: not 0 or 1");
    ob_u64(&b, "official", r->official);
    t_text(&b, "ladder_item", r->ladder_item, V);
    if (!b.err && (r->official == 1) != (strcmp(r->ladder_item, "-") == 0))
        REFUSE(&b, "ladder_item must be - iff official is 1");
    ob_list_i64(&b, "lengths", r->lengths, 6);
    if (!b.err) {
        int64_t d;
        if (ty_sub(r->lengths[4], r->lengths[5], &d) != TY_OK || d != r->t_ub)
            REFUSE(&b, "t_ub is not DL(B,D) - DL(M,D)");
        if (r->t_lo_ub > r->t_hi_ub) REFUSE(&b, "t_lo_ub exceeds t_hi_ub");
    }
    ob_i64(&b, "t_ub", r->t_ub);
    ob_i64(&b, "t_lo_ub", r->t_lo_ub);
    ob_i64(&b, "t_hi_ub", r->t_hi_ub);
    ob_i64(&b, "numeric_bound_ub", r->numeric_bound_ub);
    ob_i64(&b, "mc_err_ub", r->mc_err_ub);
    if (!b.err) {
        char t[TYQR_VAL];
        snprintf(t, sizeof t, "%" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64, r->counts[0], r->counts[1],
                 r->counts[2], r->counts[3]);
        ob_text(&b, "counts", t);
    }
    ob_u64(&b, "floor_hits", r->floor_hits);
    int stopped = 0;
    if (!text_ok(r->status, V)) REFUSE(&b, "field status: not text");
    else {
        stopped = strcmp(r->status, "STOPPED") == 0;
        if (!stopped && strcmp(r->status, "PASS") != 0 && strncmp(r->status, "FAIL_", 5) != 0)
            REFUSE(&b, "field status: not PASS, FAIL_* or STOPPED");
        else if (strchr(r->status, ' ')) REFUSE(&b, "field status: contains a space");
    }
    if (!b.err && !stopped && r->protocol_failures != 0) REFUSE(&b, "protocol_failures must be 0 for a scored gain");
    ob_u64(&b, "protocol_failures", r->protocol_failures);
    t_enum(&b, "leak_flag", r->leak_flag, V, SET_LEAK);
    for (int i = 0; i < 3; ++i)
        if (r->coverage[i] < -1) REFUSE(&b, "field coverage: below -1");
    ob_list_i64(&b, "coverage", r->coverage, 3);
    t_list(&b, "diag_warnings", r->diag_warnings, V);
    t_enum(&b, "classification", r->classification, V, SET_CLASS);
    ob_text(&b, "status", r->status);
    t_text(&b, "unit", r->unit, V);
    if (why && wl) snprintf(why, wl, "%s", b.why);
    return ob_finish(&b, TYQR_DOMAIN_GAIN, out, by, bc, bl);
}

/* ------------------------------------------------- public digest / verify */

int tyqr_profile_digest(const tyqr_profile *r, ty_digest *out) { return enc_profile(r, out, NULL, 0, NULL, NULL, 0); }
int tyqr_world_digest(const tyqr_world *r, ty_digest *out) { return enc_world(r, out, NULL, 0, NULL, NULL, 0); }
int tyqr_freeze_digest(const tyqr_freeze *r, ty_digest *out) { return enc_freeze(r, out, NULL, 0, NULL, NULL, 0); }
int tyqr_gain_digest(const tyqr_gain *r, ty_digest *out) { return enc_gain(r, out, NULL, 0, NULL, NULL, 0); }

static int verify_common(int rc, const ty_digest *got, const ty_digest *want, char *why, size_t wl) {
    if (rc != TY_OK) return rc;
    if (!want) return TY_E_ARG;
    if (!ty_digest_eq(got, want)) {
        if (why && wl) snprintf(why, wl, "recomputed digest differs from the recorded digest");
        return TY_E_DIGEST;
    }
    return TY_OK;
}

#define VERIFY_FN(name, T, enc)                                                                     \
    int name(const T *r, const ty_digest *want, char *why, size_t wl) {                            \
        ty_digest d;                                                                                \
        if (why && wl) why[0] = 0;                                                                  \
        int rc = enc(r, &d, NULL, 0, NULL, why, wl);                                                \
        return verify_common(rc, &d, want, why, wl);                                                \
    }
VERIFY_FN(tyqr_profile_verify, tyqr_profile, enc_profile)
VERIFY_FN(tyqr_world_verify, tyqr_world, enc_world)
VERIFY_FN(tyqr_freeze_verify, tyqr_freeze, enc_freeze)
VERIFY_FN(tyqr_gain_verify, tyqr_gain, enc_gain)

int tyqr_gain_bind(const tyqr_gain *g, const ty_digest *gd, const tyqr_world *w, const ty_digest *wd,
                   const tyqr_freeze *f, const ty_digest *fd, const ty_digest *pd, char *why, size_t wl) {
    int rc;
    if (!g || !gd || !w || !wd || !f || !fd || !pd) return TY_E_ARG;
    if ((rc = tyqr_gain_verify(g, gd, why, wl)) != TY_OK) return rc;
    if ((rc = tyqr_world_verify(w, wd, why, wl)) != TY_OK) return rc;
    if ((rc = tyqr_freeze_verify(f, fd, why, wl)) != TY_OK) return rc;
    const char *bad = NULL;
    if (!ty_digest_eq(&g->qprofile, pd)) bad = "gain qprofile differs from the profile digest";
    else if (!ty_digest_eq(&w->qprofile, pd)) bad = "world qprofile differs from the profile digest";
    else if (!ty_digest_eq(&f->qprofile, pd)) bad = "freeze qprofile differs from the profile digest";
    else if (!ty_digest_eq(&g->qworld, wd)) bad = "gain does not name this world";
    else if (!ty_digest_eq(&g->qfreeze, fd)) bad = "gain does not name this freeze";
    else if (!ty_digest_eq(&w->qfreeze, fd)) bad = "world is not sealed by this freeze";
    else if (strcmp(g->mode, w->mode) != 0) bad = "gain mode differs from the world's";
    else if (strcmp(g->mode, f->mode) != 0) bad = "gain mode differs from the freeze's";
    else if (strcmp(g->cell, w->cell) != 0) bad = "gain cell differs from the world's";
    else if (strcmp(g->gen_version, w->gen_version) != 0) bad = "gain gen_version differs from the world's";
    else if (!ty_digest_eq(&g->candidate, &f->artifact)) bad = "gain candidate is not the freeze artifact";
    if (bad) {
        if (why && wl) snprintf(why, wl, "%s", bad);
        return TY_E_PROFILE;
    }
    return TY_OK;
}

/* ------------------------------------------------------------------ writer */

static int write_once(const char *dir, const char *record, const ty_digest *d, const uint8_t *by, size_t n,
                      char *path, size_t pathlen) {
    char h[65], p[1024];
    ty_hex(d, h);
    int k = snprintf(p, sizeof p, "%s/%s.%s.omg0", dir, record, h);
    if (k < 0 || (size_t)k >= sizeof p) return TY_E_ARG;
    if (path) {
        if ((size_t)k >= pathlen) return TY_E_ARG;
        memcpy(path, p, (size_t)k + 1);
    }
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
    if (fd < 0) return errno == EEXIST ? TYQR_E_EXISTS : TY_E_IO;
    const uint8_t *q = by;
    size_t left = n;
    while (left) {
        ssize_t w = write(fd, q, left);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            close(fd);
            unlink(p);
            return TY_E_IO;
        }
        q += w;
        left -= (size_t)w;
    }
    if (close(fd) != 0) {
        unlink(p);
        return TY_E_IO;
    }
    return TY_OK;
}

#define WRITE_FN(name, T, enc, rec)                                                                 \
    int name(const char *dir, const T *r, ty_digest *d, char *path, size_t pl) {                   \
        uint8_t *by = malloc(TYQR_CANON_CAP);                                                       \
        size_t n = 0;                                                                               \
        ty_digest dd;                                                                               \
        if (!dir || !by) {                                                                          \
            free(by);                                                                               \
            return TY_E_ARG;                                                                        \
        }                                                                                           \
        int rc = enc(r, &dd, by, TYQR_CANON_CAP, &n, NULL, 0);                                      \
        if (rc == TY_OK) {                                                                          \
            if (d) *d = dd;                                                                         \
            rc = write_once(dir, rec, &dd, by, n, path, pl);                                        \
        }                                                                                           \
        free(by);                                                                                   \
        return rc;                                                                                  \
    }
WRITE_FN(tyqr_profile_write, tyqr_profile, enc_profile, "turing.qprofile")
WRITE_FN(tyqr_world_write, tyqr_world, enc_world, "turing.qworld")
WRITE_FN(tyqr_freeze_write, tyqr_freeze, enc_freeze, "turing.qfreeze")
WRITE_FN(tyqr_gain_write, tyqr_gain, enc_gain, "turing.qgain")

int tyqr_file_verify(const char *path, const char *domain, const ty_digest *want) {
    if (!path || !domain || !want) return TY_E_ARG;
    FILE *f = fopen(path, "rb");
    if (!f) return TY_E_IO;
    uint8_t *buf = malloc(TYQR_CANON_CAP + 1);
    if (!buf) {
        fclose(f);
        return TY_E_IO;
    }
    size_t n = fread(buf, 1, TYQR_CANON_CAP + 1, f);
    int err = ferror(f);
    fclose(f);
    int rc;
    if (err) rc = TY_E_IO;
    else if (n > TYQR_CANON_CAP) rc = TY_E_FORMAT;
    else {
        sha256_ctx c;
        ty_digest d;
        sha256_init(&c);
        sha256_update(&c, (const uint8_t *)domain, strlen(domain) + 1);
        sha256_update(&c, buf, n);
        sha256_final(&c, d.b);
        rc = ty_digest_eq(&d, want) ? TY_OK : TY_E_DIGEST;
    }
    free(buf);
    return rc;
}

/* --------------------------------------------------------------- sidecars */

void tyqr_sidecar_digest(const char *buf, size_t len, ty_digest *out) {
    sha256_hash((const uint8_t *)buf, len, out->b);
}

/* Parse a decimal with no sign, no leading zero (except "0"), fitting uint64. */
static int parse_dec(const char *s, size_t n, uint64_t *out) {
    if (n < 1 || n > 20 || (n > 1 && s[0] == '0')) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return 0;
        uint64_t dg = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - dg) / 10) return 0;
        v = v * 10 + dg;
    }
    *out = v;
    return 1;
}

static int parse_hex64(const char *s, size_t n, ty_digest *d) {
    if (n != 64) return 0;
    char t[65];
    memcpy(t, s, 64);
    t[64] = 0;
    return ty_parse_hex(t, d) == TY_OK;
}

/* Split one line (without LF) into single-space separated fields. Returns
 * the field count, or -1 if a field is empty or a byte is outside 0x21..0x7e
 * (and not the separating space). At most `max` fields are stored. */
static int split_line(const char *l, size_t n, const char **f, size_t *fl, int max) {
    int c = 0;
    size_t i = 0;
    for (;;) {
        size_t s = i;
        while (i < n && l[i] != ' ') {
            if ((unsigned char)l[i] < 0x21 || (unsigned char)l[i] > 0x7e) return -1;
            ++i;
        }
        if (i == s) return -1;
        if (c < max) {
            f[c] = l + s;
            fl[c] = i - s;
        }
        ++c;
        if (i == n) return c;
        ++i; /* skip the space; a trailing space leaves an empty field */
        if (i == n) return -1;
    }
}

/* Iterate lines; requires the buffer to be non-empty and end with LF. */
static int next_line(const char *buf, size_t len, size_t *pos, const char **l, size_t *ln) {
    if (*pos >= len) return 0;
    const char *e = memchr(buf + *pos, '\n', len - *pos);
    if (!e) return -1; /* last line lacks LF */
    *l = buf + *pos;
    *ln = (size_t)(e - *l);
    *pos = (size_t)(e - buf) + 1;
    return 1;
}

int tyqr_repman_build(const tyqr_repline *e, size_t n, char *out, size_t cap, size_t *len) {
    if (!e || !out || !len || n == 0) return TY_E_ARG;
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i && e[i].rep <= e[i - 1].rep) return TY_E_FORMAT;
        char h[65];
        ty_hex(&e[i].d, h);
        int k = snprintf(out + o, cap - o, "%" PRIu64 " %s\n", e[i].rep, h);
        if (k < 0 || (size_t)k >= cap - o) return TY_E_FORMAT;
        o += (size_t)k;
    }
    *len = o;
    return TY_OK;
}

int tyqr_repman_parse(const char *buf, size_t len, tyqr_repline *out, size_t cap, size_t *n) {
    if (!buf || !out || !n || len == 0) return TY_E_FORMAT;
    size_t pos = 0, c = 0;
    const char *l;
    size_t ln;
    int r;
    while ((r = next_line(buf, len, &pos, &l, &ln)) == 1) {
        const char *f[2];
        size_t fl[2];
        if (c >= cap || split_line(l, ln, f, fl, 2) != 2) return TY_E_FORMAT;
        if (!parse_dec(f[0], fl[0], &out[c].rep) || !parse_hex64(f[1], fl[1], &out[c].d)) return TY_E_FORMAT;
        if (c && out[c].rep <= out[c - 1].rep) return TY_E_FORMAT;
        ++c;
    }
    if (r < 0 || c == 0) return TY_E_FORMAT;
    *n = c;
    return TY_OK;
}

static int devline_before(const tyqr_devline *a, const tyqr_devline *b) {
    int c = strcmp(a->cell, b->cell);
    return c < 0 || (c == 0 && a->block < b->block);
}

int tyqr_devman_build(const tyqr_devline *e, size_t n, char *out, size_t cap, size_t *len) {
    if (!e || !out || !len || n == 0) return TY_E_ARG;
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!name_ok(e[i].cell, sizeof e[i].cell)) return TY_E_FORMAT;
        if (i && !devline_before(&e[i - 1], &e[i])) return TY_E_FORMAT;
        char h[65];
        ty_hex(&e[i].d, h);
        int k = snprintf(out + o, cap - o, "%s %" PRIu64 " %s\n", e[i].cell, e[i].block, h);
        if (k < 0 || (size_t)k >= cap - o) return TY_E_FORMAT;
        o += (size_t)k;
    }
    *len = o;
    return TY_OK;
}

int tyqr_devman_parse(const char *buf, size_t len, tyqr_devline *out, size_t cap, size_t *n) {
    if (!buf || !out || !n || len == 0) return TY_E_FORMAT;
    size_t pos = 0, c = 0;
    const char *l;
    size_t ln;
    int r;
    while ((r = next_line(buf, len, &pos, &l, &ln)) == 1) {
        const char *f[3];
        size_t fl[3];
        if (c >= cap || split_line(l, ln, f, fl, 3) != 3) return TY_E_FORMAT;
        if (fl[0] >= TYQR_CELL_MAX) return TY_E_FORMAT;
        memcpy(out[c].cell, f[0], fl[0]);
        out[c].cell[fl[0]] = 0;
        if (!parse_dec(f[1], fl[1], &out[c].block) || !parse_hex64(f[2], fl[2], &out[c].d)) return TY_E_FORMAT;
        if (c && !devline_before(&out[c - 1], &out[c])) return TY_E_FORMAT;
        ++c;
    }
    if (r < 0 || c == 0) return TY_E_FORMAT;
    *n = c;
    return TY_OK;
}

/* Lines strictly ascending by the given key length (whole line if cmp_key is 0). */
static int table_check(const char *buf, size_t len, int min_fields, int max_fields, int by_cell, int kv) {
    if (!buf || len == 0) return TY_E_FORMAT;
    size_t pos = 0, pl = 0;
    const char *l, *prev = NULL;
    size_t ln;
    int r;
    while ((r = next_line(buf, len, &pos, &l, &ln)) == 1) {
        const char *f[64];
        size_t fl[64];
        int nf = split_line(l, ln, f, fl, 64);
        if (nf < min_fields || nf > 64 || (max_fields && nf > max_fields)) return TY_E_FORMAT;
        if (kv) {
            for (int i = 1; i < nf; ++i) {
                const char *eq = memchr(f[i], '=', fl[i]);
                if (!eq || eq == f[i] || eq == f[i] + fl[i] - 1) return TY_E_FORMAT;
            }
        }
        if (prev) {
            size_t a = pl, b = by_cell ? fl[0] : ln;
            const char *ka = prev, *kb = l;
            if (by_cell) {
                size_t sp = 0;
                while (sp < pl && prev[sp] != ' ') ++sp;
                a = sp;
            }
            size_t m = a < b ? a : b;
            int c = memcmp(ka, kb, m);
            if (c == 0) c = a < b ? -1 : (a > b ? 1 : 0);
            if (c >= 0) return TY_E_FORMAT;
        }
        prev = l;
        pl = ln;
    }
    return (r < 0 || !prev) ? TY_E_FORMAT : TY_OK;
}

int tyqr_celltab_check(const char *buf, size_t len) { return table_check(buf, len, 2, 0, 1, 1); }
int tyqr_thrtab_check(const char *buf, size_t len) { return table_check(buf, len, 3, 3, 0, 0); }
