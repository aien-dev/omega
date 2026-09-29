/* Turing Yield CTR1 reader. See ty_ctr1.h. */
#include "turing/ty_ctr1.h"

#include "turing/ty_math.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void ty_stream_init(ty_stream *s) { memset(s, 0, sizeof *s); }

void ty_stream_free(ty_stream *s) {
    free(s->ev);
    memset(s, 0, sizeof *s);
}

int ty_stream_push(ty_stream *s, const ty_ev *e) {
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 4096;
        ty_ev *p = realloc(s->ev, nc * sizeof *p);
        if (!p) return TY_E_IO;
        s->ev = p;
        s->cap = nc;
    }
    s->ev[s->n++] = *e;
    if (e->first) s->ncrumb++;
    return TY_OK;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#define WHY(...)                                              \
    do {                                                      \
        if (why && whylen) snprintf(why, whylen, __VA_ARGS__); \
    } while (0)

int ty_ctr1_decode(const uint8_t r[TY_CTR1_BYTES], int64_t prev_idx, ty_ev *out, char *why, size_t whylen) {
    if (memcmp(r, "CTR1", 4) != 0) {
        WHY("bad magic");
        return TY_E_FORMAT;
    }
    if (rd16(r + 4) != 1) {
        WHY("version %u != 1", rd16(r + 4));
        return TY_E_FORMAT;
    }
    uint8_t kind = r[6], rc = r[7], origin = r[22], prune = r[23], verify = r[24], fit = r[25];
    uint32_t idx = rd32(r + 8);
    uint16_t op = rd16(r + 20), depth = rd16(r + 29);
    memset(out, 0, sizeof *out);
    if (origin != 0) {
        WHY("op_origin %u (control condition has library disabled: all ops are base)", origin);
        return TY_E_FORMAT;
    }
    if (verify > 4 || fit > 2) {
        WHY("verify %u / fit %u out of range", verify, fit);
        return TY_E_FORMAT;
    }
    if (kind == 1) { /* EXPAND */
        if (op >= TY_CTR1_NOPS) {
            WHY("op_index %u >= %u", op, TY_CTR1_NOPS);
            return TY_E_FORMAT;
        }
        if (prune > 4) {
            WHY("prune %u out of range", prune);
            return TY_E_FORMAT;
        }
        if (prune != 0) {
            if (rc != 1) {
                WHY("pruned event with result_class %u (must be Pruned=1)", rc);
                return TY_E_FORMAT;
            }
            out->sym = (uint8_t)(TY_O_P_EQUIV + (prune - 1));
        } else {
            if (rc < 2 || rc > 4) {
                WHY("unpruned EXPAND with result_class %u", rc);
                return TY_E_FORMAT;
            }
            out->sym = (uint8_t)(TY_O_FAILED + (rc - 2));
        }
        out->op = (uint8_t)op;
    } else if (kind == 2) { /* SUBMIT */
        if (prune != 0 || rc < 5 || rc > 7) {
            WHY("SUBMIT with prune %u result_class %u", prune, rc);
            return TY_E_FORMAT;
        }
        out->sym = rc == 5 ? TY_O_ACCEPT : TY_O_REJECT;
        out->op = TY_OP_SUBMIT;
    } else {
        WHY("kind %u", kind);
        return TY_E_FORMAT;
    }
    out->depth = (uint8_t)(depth > TY_DEPTH_CLIP ? TY_DEPTH_CLIP : depth);
    out->idx = idx;
    if (idx == 0) {
        out->first = 1;
    } else if (prev_idx < 0 || (int64_t)idx <= prev_idx) {
        WHY("event_index %u after %lld without a crumb start", idx, (long long)prev_idx);
        return TY_E_FORMAT;
    }
    return TY_OK;
}

int64_t ty_ctr1_read(const char *path, ty_stream *s, char *why, size_t whylen) {
    struct stat st;
    if (stat(path, &st) != 0) {
        WHY("cannot stat %s", path);
        return TY_E_IO;
    }
    if (st.st_size % TY_CTR1_BYTES != 0) {
        WHY("size %lld is not a multiple of %d", (long long)st.st_size, TY_CTR1_BYTES);
        return TY_E_FORMAT;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        WHY("cannot open %s", path);
        return TY_E_IO;
    }
    enum { BATCH = 4096 };
    uint8_t *buf = malloc((size_t)BATCH * TY_CTR1_BYTES);
    if (!buf) {
        fclose(f);
        return TY_E_IO;
    }
    int64_t n = 0, prev = -1;
    uint32_t crumb = 0;
    int rc = TY_OK;
    size_t k;
    while (rc == TY_OK && (k = fread(buf, TY_CTR1_BYTES, BATCH, f)) > 0) {
        for (size_t i = 0; i < k; ++i) {
            ty_ev e;
            char w[160];
            rc = ty_ctr1_decode(buf + i * TY_CTR1_BYTES, prev, &e, w, sizeof w);
            if (rc != TY_OK) {
                WHY("%s record %lld: %s", path, (long long)n, w);
                break;
            }
            if (e.first) {
                if (n > 0) ++crumb;
            } else if ((int64_t)e.idx != prev + 1) {
                s->gaps++;
            }
            e.crumb = (uint16_t)(crumb > 65535 ? 65535 : crumb);
            prev = e.idx;
            if ((rc = ty_stream_push(s, &e)) != TY_OK) break;
            ++n;
        }
    }
    if (rc == TY_OK && ferror(f)) rc = TY_E_IO;
    fclose(f);
    free(buf);
    if (rc != TY_OK) return rc;
    if (n * TY_CTR1_BYTES != (int64_t)st.st_size) {
        WHY("read %lld records, expected %lld", (long long)n, (long long)(st.st_size / TY_CTR1_BYTES));
        return TY_E_IO;
    }
    return n;
}
