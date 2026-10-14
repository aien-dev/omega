/* MAT v5 reader (see res_mat.h). Format reference: MathWorks, "MAT-File
 * Format" (version 5 MAT-file: 128-byte header, then data elements with an
 * 8-byte tag or a 4-byte small-element tag, 8-byte aligned inside miMATRIX;
 * miCOMPRESSED holds one zlib stream that decodes to one element). */
#include "res_mat.h"

#include "res_inflate.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MI_INT8 = 1, MI_UINT8 = 2, MI_INT16 = 3, MI_UINT16 = 4, MI_INT32 = 5, MI_UINT32 = 6, MI_SINGLE = 7,
       MI_DOUBLE = 9, MI_INT64 = 12, MI_UINT64 = 13, MI_MATRIX = 14, MI_COMPRESSED = 15 };

#define MAX_NUMEL ((size_t)1 << 31)

int res_mat_verbose = 1;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

typedef struct {
    uint32_t type, nbytes;
    size_t data, next;
} Tag;

/* Tag at p[pos] within n bytes. align: the element is padded to 8 bytes
 * (every element inside miMATRIX and a top-level miMATRIX). */
static ResMatStatus read_tag(const uint8_t *p, size_t n, size_t pos, int align, Tag *t)
{
    if (pos > n || n - pos < 8) return RES_MAT_ERR_FORMAT;
    uint32_t w0 = rd32(p + pos);
    if (w0 >> 16) {
        t->type = w0 & 0xffffu;
        t->nbytes = w0 >> 16;
        if (t->nbytes > 4) return RES_MAT_ERR_FORMAT;
        t->data = pos + 4;
        t->next = pos + 8;
        return RES_MAT_OK;
    }
    t->type = w0;
    t->nbytes = rd32(p + pos + 4);
    t->data = pos + 8;
    if (n - t->data < t->nbytes) return RES_MAT_ERR_FORMAT;
    size_t end = t->data + t->nbytes;
    if (align) {
        end = (end + 7u) & ~(size_t)7u;
        if (end > n) end = n; /* padding of the last element may be absent */
    }
    t->next = end;
    return RES_MAT_OK;
}

static size_t type_size(uint32_t type)
{
    switch (type) {
    case MI_INT8: case MI_UINT8: return 1;
    case MI_INT16: case MI_UINT16: return 2;
    case MI_INT32: case MI_UINT32: case MI_SINGLE: return 4;
    case MI_DOUBLE: case MI_INT64: case MI_UINT64: return 8;
    default: return 0;
    }
}

static ResMatStatus convert(uint32_t type, const uint8_t *d, size_t nbytes, size_t numel, double *out)
{
    size_t sz = type_size(type);
    if (sz == 0 || nbytes != numel * sz) return RES_MAT_ERR_FORMAT;
    for (size_t i = 0; i < numel; i++, d += sz) {
        switch (type) {
        case MI_INT8: out[i] = (double)(int8_t)d[0]; break;
        case MI_UINT8: out[i] = (double)d[0]; break;
        case MI_INT16: out[i] = (double)(int16_t)(uint16_t)(d[0] | (d[1] << 8)); break;
        case MI_UINT16: out[i] = (double)(uint16_t)(d[0] | (d[1] << 8)); break;
        case MI_INT32: out[i] = (double)(int32_t)rd32(d); break;
        case MI_UINT32: out[i] = (double)rd32(d); break;
        case MI_SINGLE: {
            uint32_t b = rd32(d);
            float f;
            memcpy(&f, &b, 4);
            out[i] = (double)f;
            break;
        }
        case MI_DOUBLE: {
            uint64_t b = rd64(d);
            memcpy(&out[i], &b, 8);
            break;
        }
        case MI_INT64: {
            int64_t v = (int64_t)rd64(d);
            if (v > ((int64_t)1 << 53) || v < -((int64_t)1 << 53)) return RES_MAT_UNSUPPORTED;
            out[i] = (double)v;
            break;
        }
        case MI_UINT64: {
            uint64_t v = rd64(d);
            if (v > ((uint64_t)1 << 53)) return RES_MAT_UNSUPPORTED;
            out[i] = (double)v;
            break;
        }
        }
    }
    return RES_MAT_OK;
}

static void free_array(ResMatArray *a)
{
    if (a->cell) {
        for (size_t i = 0; i < a->numel; i++) free_array(&a->cell[i]);
        free(a->cell);
    }
    free(a->re);
    a->cell = NULL;
    a->re = NULL;
}

/* Content (after the miMATRIX tag) of nbytes = n. */
static ResMatStatus parse_matrix(const uint8_t *p, size_t n, ResMatArray *a, int depth)
{
    memset(a, 0, sizeof *a);
    if (n == 0) { /* an empty element: [] */
        a->cls = RES_MX_DOUBLE;
        a->ndims = 2;
        a->has_data = 1;
        return RES_MAT_OK;
    }
    if (depth > RES_MAT_MAX_DEPTH) return RES_MAT_UNSUPPORTED;
    Tag t;
    ResMatStatus e = read_tag(p, n, 0, 1, &t);
    if (e) return e;
    if (t.type != MI_UINT32 || t.nbytes != 8) return RES_MAT_ERR_FORMAT;
    uint32_t flags = rd32(p + t.data);
    a->cls = (int)(flags & 0xffu);
    a->is_complex = (flags & 0x800u) != 0;
    a->is_logical = (flags & 0x200u) != 0;
    size_t pos = t.next;
    if (a->cls == RES_MX_OPAQUE) { /* flags, name, then class metadata: keep the name only */
        if ((e = read_tag(p, n, pos, 1, &t))) return e;
        if (t.type != MI_INT8) return RES_MAT_ERR_FORMAT;
        size_t nl = t.nbytes < RES_MAT_MAX_NAME - 1u ? t.nbytes : RES_MAT_MAX_NAME - 1u;
        memcpy(a->name, p + t.data, nl);
        a->name[nl] = 0;
        return RES_MAT_OK;
    }
    if ((e = read_tag(p, n, pos, 1, &t))) return e;
    if (t.type != MI_INT32 || t.nbytes % 4u || t.nbytes / 4u < 2u || t.nbytes / 4u > RES_MAT_MAX_DIMS)
        return RES_MAT_ERR_FORMAT;
    a->ndims = t.nbytes / 4u;
    a->numel = 1;
    for (uint32_t i = 0; i < a->ndims; i++) {
        int32_t d = (int32_t)rd32(p + t.data + 4u * i);
        if (d < 0) return RES_MAT_ERR_FORMAT;
        a->dims[i] = (uint32_t)d;
        if (d != 0 && a->numel > MAX_NUMEL / (size_t)d) return RES_MAT_ERR_FORMAT;
        a->numel *= (size_t)d;
    }
    pos = t.next;
    if ((e = read_tag(p, n, pos, 1, &t))) return e;
    if (t.type != MI_INT8) return RES_MAT_ERR_FORMAT;
    size_t nl = t.nbytes < RES_MAT_MAX_NAME - 1u ? t.nbytes : RES_MAT_MAX_NAME - 1u;
    memcpy(a->name, p + t.data, nl);
    a->name[nl] = 0;
    pos = t.next;
    if (a->cls >= RES_MX_DOUBLE && a->cls <= RES_MX_UINT64) {
        if (a->is_complex) return RES_MAT_OK; /* recorded, no data */
        if ((e = read_tag(p, n, pos, 1, &t))) return e;
        size_t sz = type_size(t.type); /* sizes checked before allocating: dims are untrusted */
        if (sz == 0 || t.nbytes != a->numel * sz) return RES_MAT_ERR_FORMAT;
        a->re = malloc(a->numel ? a->numel * sizeof(double) : 1u);
        if (!a->re) return RES_MAT_ERR_NOMEM;
        if ((e = convert(t.type, p + t.data, t.nbytes, a->numel, a->re))) return e;
        a->has_data = 1;
        return RES_MAT_OK;
    }
    if (a->cls == RES_MX_CELL) {
        if (pos > n || a->numel > (n - pos) / 8u) return RES_MAT_ERR_FORMAT; /* each element needs a tag */
        a->cell = calloc(a->numel ? a->numel : 1u, sizeof(ResMatArray));
        if (!a->cell) return RES_MAT_ERR_NOMEM;
        for (size_t i = 0; i < a->numel; i++) {
            if ((e = read_tag(p, n, pos, 1, &t))) return e;
            if (t.type != MI_MATRIX) return RES_MAT_ERR_FORMAT;
            if ((e = parse_matrix(p + t.data, t.nbytes, &a->cell[i], depth + 1))) return e;
            pos = t.next;
        }
        a->has_data = 1;
        return RES_MAT_OK;
    }
    return RES_MAT_OK; /* char, struct, object, sparse: class and dims only */
}

static ResMatStatus add_element(ResMatFile *f, const uint8_t *p, size_t n)
{
    Tag t;
    ResMatStatus e = read_tag(p, n, 0, 1, &t);
    if (e) return e;
    if (t.type != MI_MATRIX) return RES_MAT_ERR_FORMAT;
    if (f->var_count >= RES_MAT_MAX_VARS) return RES_MAT_UNSUPPORTED;
    ResMatArray *a = &f->var[f->var_count];
    e = parse_matrix(p + t.data, t.nbytes, a, 0);
    f->var_count++; /* counted even on failure so res_mat_free releases it */
    return e;
}

ResMatStatus res_mat_parse(const uint8_t *p, size_t n, ResMatFile *f)
{
    if (!p || !f) return RES_MAT_ERR_FORMAT;
    uint8_t keep[32];
    size_t keep_n = f->file_bytes;
    memcpy(keep, f->sha256, 32);
    memset(f, 0, sizeof *f);
    memcpy(f->sha256, keep, 32);
    f->file_bytes = keep_n;
    if (n < 128) return RES_MAT_ERR_FORMAT;
    if (p[126] != 'I' || p[127] != 'M') return RES_MAT_UNSUPPORTED; /* big-endian file */
    if (p[124] != 0x00 || p[125] != 0x01) return RES_MAT_ERR_FORMAT;
    if (memcmp(p, "MATLAB 5.0 MAT-file", 19) != 0) return RES_MAT_ERR_FORMAT;
    memcpy(f->header_text, p, 116);
    f->header_text[116] = 0;
    size_t pos = 128;
    while (pos < n) {
        Tag t;
        ResMatStatus e = read_tag(p, n, pos, 0, &t);
        if (e) return e;
        if (t.type == MI_COMPRESSED) {
            uint8_t *buf;
            size_t len, used;
            ResInflateStatus z = res_zlib_inflate(p + t.data, t.nbytes, &buf, &len, &used);
            if (z != RES_INF_OK) {
                if (res_mat_verbose) fprintf(stderr, "res_mat: element at byte %zu: %s\n", pos, res_inflate_strerror(z));
                return RES_MAT_ERR_INFLATE;
            }
            if (used != t.nbytes) {
                free(buf);
                return RES_MAT_ERR_FORMAT;
            }
            e = add_element(f, buf, len);
            free(buf);
            if (e) return e;
            pos = t.data + t.nbytes;
        } else if (t.type == MI_MATRIX) {
            if ((e = add_element(f, p + pos, n - pos))) return e;
            pos = (t.data + t.nbytes + 7u) & ~(size_t)7u;
        } else {
            return RES_MAT_ERR_FORMAT;
        }
    }
    return RES_MAT_OK;
}

ResMatStatus res_mat_load(const char *path, const uint8_t *want_sha256, ResMatFile *out)
{
    if (!path || !out) return RES_MAT_ERR_IO;
    memset(out, 0, sizeof *out);
    FILE *fp = fopen(path, "rb");
    if (!fp) return RES_MAT_ERR_IO;
    size_t cap = 1u << 20, n = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        fclose(fp);
        return RES_MAT_ERR_NOMEM;
    }
    for (;;) {
        if (n == cap) {
            if (cap >= ((size_t)1 << 30)) {
                free(buf);
                fclose(fp);
                return RES_MAT_ERR_NOMEM;
            }
            uint8_t *q = realloc(buf, cap * 2u);
            if (!q) {
                free(buf);
                fclose(fp);
                return RES_MAT_ERR_NOMEM;
            }
            buf = q;
            cap *= 2u;
        }
        size_t r = fread(buf + n, 1, cap - n, fp);
        n += r;
        if (r == 0) break;
    }
    int bad = ferror(fp);
    fclose(fp);
    if (bad) {
        free(buf);
        return RES_MAT_ERR_IO;
    }
    sha256_hash(buf, n, out->sha256);
    out->file_bytes = n;
    if (want_sha256 && memcmp(out->sha256, want_sha256, 32) != 0) {
        free(buf);
        return RES_MAT_ERR_DIGEST;
    }
    ResMatStatus e = res_mat_parse(buf, n, out);
    free(buf);
    if (e) res_mat_free(out);
    return e;
}

void res_mat_free(ResMatFile *f)
{
    if (!f) return;
    for (uint32_t i = 0; i < f->var_count; i++) free_array(&f->var[i]);
    f->var_count = 0;
}

const ResMatArray *res_mat_find(const ResMatFile *f, const char *name)
{
    for (uint32_t i = 0; f && i < f->var_count; i++)
        if (strcmp(f->var[i].name, name) == 0) return &f->var[i];
    return NULL;
}

const ResMatArray *res_mat_cell_at(const ResMatArray *c, const uint32_t *sub, uint32_t count)
{
    if (!c || c->cls != RES_MX_CELL || !c->cell || count != c->ndims) return NULL;
    size_t idx = 0, stride = 1;
    for (uint32_t i = 0; i < count; i++) {
        if (sub[i] >= c->dims[i]) return NULL;
        idx += (size_t)sub[i] * stride;
        stride *= c->dims[i];
    }
    return &c->cell[idx];
}

ResMatStatus res_mat_values(const ResMatArray *a, size_t want_numel, const double **values)
{
    if (!a) return RES_MAT_ERR_NOTFOUND;
    if (!(a->cls >= RES_MX_DOUBLE && a->cls <= RES_MX_UINT64) || !a->has_data || !a->re) return RES_MAT_UNSUPPORTED;
    if (want_numel && a->numel != want_numel) return RES_MAT_ERR_SHAPE;
    *values = a->re;
    return RES_MAT_OK;
}

const char *res_mat_strerror(ResMatStatus s)
{
    switch (s) {
    case RES_MAT_OK: return "ok";
    case RES_MAT_ERR_IO: return "cannot read file";
    case RES_MAT_ERR_FORMAT: return "not a valid MAT v5 file";
    case RES_MAT_ERR_INFLATE: return "compressed element failed to decode";
    case RES_MAT_ERR_NOMEM: return "out of memory";
    case RES_MAT_ERR_DIGEST: return "sha256 differs from the pinned value";
    case RES_MAT_ERR_NOTFOUND: return "variable not found";
    case RES_MAT_UNSUPPORTED: return "unsupported MAT content";
    case RES_MAT_ERR_SHAPE: return "unexpected shape";
    }
    return "unknown";
}

const char *res_mat_class_name(int cls)
{
    static const char *const N[] = {"?", "cell", "struct", "object", "char", "sparse", "double", "single",
                                    "int8", "uint8", "int16", "uint16", "int32", "uint32", "int64", "uint64"};
    if (cls == RES_MX_OPAQUE) return "opaque";
    return (cls >= 0 && cls <= 15) ? N[cls] : "?";
}
