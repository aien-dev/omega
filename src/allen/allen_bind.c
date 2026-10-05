/* allen_bind.c -- see allen_bind.h. Pure functions over the subject object,
 * a Cortex store and World references. No I/O except reading the object file. */
#include "allen/allen_bind.h"

#include <stdio.h>
#include <string.h>

int allen_load(const char *path, struct cs_subject *s, uint8_t id[32], const char **why)
{
    static uint8_t buf[CC_MAX_OBJECT_BYTES + 1];
    FILE *f;
    size_t n;
    int rc;
    if (!path || !s || !id) {
        if (why) *why = "null argument";
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        if (why) *why = "cannot open subject file";
        return -1;
    }
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if ((rc = cs_subject_decode(buf, n, s, why)) != CC_OK) return -1;
    if (cs_subject_id(buf, n, id) != CC_OK) {
        if (why) *why = "object id";
        return -1;
    }
    return 0;
}

int allen_lineage(const CxStore *cx, uint8_t out[32])
{
    const CxObject *o = cx ? cx_get(cx, 1) : NULL;
    if (!o) {
        memset(out, 0, 32);
        return -1;
    }
    memcpy(out, o->digest, 32);
    return 0;
}

int allen_check_memory(const struct cs_subject *s, const CxStore *cx, const char **why)
{
    uint8_t ref[32];
    static const uint8_t zero[32] = {0};
    if (memcmp(s->cortex, zero, 32) == 0) {
        if (why) *why = "subject has no Cortex lineage binding";
        return -1;
    }
    if (allen_lineage(cx, ref) != 0) {
        if (why) *why = "Cortex journal holds no genesis record";
        return -1;
    }
    if (memcmp(ref, s->cortex, 32) != 0) {
        if (why) *why = "Cortex lineage mismatch: this journal is not the subject's memory";
        return -1;
    }
    return 0;
}

void allen_bind_identity(const RxWorld *w, const struct cs_subject *s, const uint8_t id[32],
                         AllenBinding *out)
{
    memset(out, 0, sizeof *out);
    out->external_subject = w->external_subject;
    memcpy(out->agent, s->agent, 32);
    memcpy(out->subject_id, id, 32);
    out->sequence = s->sequence;
}

uint32_t allen_goal_mutations(const struct cs_intent *a, RxObjRef goal, uint64_t seq,
                              RxMutation out[4])
{
    uint64_t id_lo = 0;
    if (!a || a->state != CS_ACTIVE || a->kind != CS_INTENT_GOAL_LATENCY) return 0;
    for (int i = 7; i >= 0; i--) id_lo = (id_lo << 8) | a->id[i];
    out[0] = (RxMutation){goal, 0, seq};
    out[1] = (RxMutation){goal, 1, a->payload[0]};
    out[2] = (RxMutation){goal, 2, a->payload[1]};
    out[3] = (RxMutation){goal, 3, id_lo};
    return 4;
}

void allen_hex(const uint8_t *b, size_t n, char *out)
{
    static const char *h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = h[b[i] >> 4];
        out[2 * i + 1] = h[b[i] & 15];
    }
    out[2 * n] = 0;
}

int allen_unhex(const char *s, uint8_t *out, size_t n)
{
    if (!s || strlen(s) != 2 * n) return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}
