#include "omega_codec.h"
#include "omega_canonical.h"
#include "omega_core.h"
#include "omega_validate.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

static void write_u16_be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xff);
    p[1] = (uint8_t)(v & 0xff);
}

static uint16_t read_u16_be(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void write_u32_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xff);
    p[1] = (uint8_t)((v >> 16) & 0xff);
    p[2] = (uint8_t)((v >> 8) & 0xff);
    p[3] = (uint8_t)(v & 0xff);
}

static uint32_t read_u32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
           ((uint32_t)p[3]);
}

int omega_graph_serialize_binary(const OmegaGraph *graph, uint8_t *out_buf, size_t max_len, size_t *out_len) {
    if (!graph || !out_buf || !out_len) return -1;
    size_t pos = 0;

    if (pos + 6 > max_len) return -1;
    out_buf[pos++] = 0x4F; /* 'O' */
    out_buf[pos++] = 0x4D; /* 'M' */
    out_buf[pos++] = 0x47; /* 'G' */
    out_buf[pos++] = 0x47; /* 'G' for Graph */
    write_u16_be(&out_buf[pos], graph->object_count);
    pos += 2;

    for (uint16_t i = 0; i < graph->object_count; ++i) {
        uint8_t cbuf[4096];
        size_t clen = 0;
        if (omega_canonical_encode(&graph->objects[i], cbuf, sizeof(cbuf), &clen) != 0) {
            return -1;
        }
        if (pos + 4 + clen > max_len) return -1;
        write_u32_be(&out_buf[pos], (uint32_t)clen);
        pos += 4;
        memcpy(&out_buf[pos], cbuf, clen);
        pos += clen;
    }

    *out_len = pos;
    return 0;
}

int omega_graph_deserialize_binary(const uint8_t *in_buf, size_t in_len, OmegaGraph *out_graph) {
    if (!in_buf || !out_graph || in_len < 6) return -1;
    size_t pos = 0;

    if (in_buf[0] != 0x4F || in_buf[1] != 0x4D || in_buf[2] != 0x47 || in_buf[3] != 0x47) {
        return -1;
    }
    pos += 4;
    uint16_t count = read_u16_be(&in_buf[pos]);
    pos += 2;

    memset(out_graph, 0, sizeof(OmegaGraph));

    for (uint16_t i = 0; i < count; ++i) {
        if (pos + 4 > in_len) return -1;
        uint32_t clen = read_u32_be(&in_buf[pos]);
        pos += 4;
        if (pos + clen > in_len || clen < 6) return -1;

        const uint8_t *cbuf = &in_buf[pos];
        if (cbuf[0] != 0x4F || cbuf[1] != 0x4D || cbuf[2] != 0x47 || cbuf[3] != 0x30) {
            return -1;
        }
        if (cbuf[4] != OMEGA_VERSION) return -1;

        OmegaObject *obj = &out_graph->objects[out_graph->object_count++];
        obj->kind = (SemanticKind)cbuf[5];
        size_t cpos = 6;

        /* Attributes */
        if (cpos + 2 > clen) return -1;
        obj->attr_count = read_u16_be(&cbuf[cpos]);
        cpos += 2;
        for (uint16_t a = 0; a < obj->attr_count; ++a) {
            if (cpos + 1 > clen) return -1;
            uint8_t klen = cbuf[cpos++];
            if (cpos + klen + 2 > clen) return -1;
            memcpy(obj->attributes[a].key, &cbuf[cpos], klen);
            obj->attributes[a].key[klen] = '\0';
            cpos += klen;
            obj->attributes[a].val_len = read_u16_be(&cbuf[cpos]);
            cpos += 2;
            if (cpos + obj->attributes[a].val_len > clen) return -1;
            memcpy(obj->attributes[a].value, &cbuf[cpos], obj->attributes[a].val_len);
            cpos += obj->attributes[a].val_len;
        }

        /* Relations */
        if (cpos + 2 > clen) return -1;
        obj->rel_count = read_u16_be(&cbuf[cpos]);
        cpos += 2;
        for (uint16_t r = 0; r < obj->rel_count; ++r) {
            if (cpos + 2 + OMEGA_ID_BYTES > clen) return -1;
            obj->relations[r].kind = read_u16_be(&cbuf[cpos]);
            cpos += 2;
            memcpy(obj->relations[r].target_id.bytes, &cbuf[cpos], OMEGA_ID_BYTES);
            cpos += OMEGA_ID_BYTES;
        }

        /* Constraints */
        if (cpos + 2 > clen) return -1;
        obj->const_count = read_u16_be(&cbuf[cpos]);
        cpos += 2;
        for (uint16_t c = 0; c < obj->const_count; ++c) {
            if (cpos + 4 > clen) return -1;
            obj->constraints[c].kind = read_u16_be(&cbuf[cpos]);
            cpos += 2;
            obj->constraints[c].payload_len = read_u16_be(&cbuf[cpos]);
            cpos += 2;
            if (cpos + obj->constraints[c].payload_len > clen) return -1;
            memcpy(obj->constraints[c].payload, &cbuf[cpos], obj->constraints[c].payload_len);
            cpos += obj->constraints[c].payload_len;
        }

        /* Payload */
        if (cpos + 4 > clen) return -1;
        obj->payload_len = read_u32_be(&cbuf[cpos]);
        cpos += 4;
        if (cpos + obj->payload_len > clen) return -1;
        memcpy(obj->payload, &cbuf[cpos], obj->payload_len);
        cpos += obj->payload_len;

        omega_compute_semantic_id(obj);
        pos += clen;
    }

    return 0;
}

int omega_graph_format_text(const OmegaGraph *graph, char *out_str, size_t max_len) {
    if (!graph || !out_str || max_len == 0) return -1;
    size_t pos = 0;

    int w = snprintf(out_str + pos, max_len - pos, "// OMEGA NON-CANONICAL TEXTUAL FORM\n// Objects: %u\n\n", graph->object_count);
    if (w < 0 || (size_t)w >= max_len - pos) return -1;
    pos += (size_t)w;

    for (uint16_t i = 0; i < graph->object_count; ++i) {
        const OmegaObject *obj = &graph->objects[i];
        char hex[65];
        omega_hex_semantic_id(&obj->id, hex);

        switch (obj->kind) {
            case KIND_TYPE: {
                const TypePayload *tp = (const TypePayload*)obj->payload;
                if (tp->tag == TYPE_UNSIGNED_INT) {
                    w = snprintf(out_str + pos, max_len - pos, "type U%u = UNSIGNED_INTEGER(%u); // id=%s\n", tp->width, tp->width, hex);
                } else if (tp->tag == TYPE_BOOL) {
                    w = snprintf(out_str + pos, max_len - pos, "type Bool = BOOL; // id=%s\n", hex);
                } else {
                    w = snprintf(out_str + pos, max_len - pos, "type Tag%u; // id=%s\n", tp->tag, hex);
                }
                break;
            }
            case KIND_VALUE: {
                const ValuePayload *vp = (const ValuePayload*)obj->payload;
                uint64_t v = 0;
                for (int b = 0; b < vp->byte_len; ++b) {
                    v = (v << 8) | vp->bytes[b];
                }
                w = snprintf(out_str + pos, max_len - pos, "val v_%u = %lu; // id=%s\n", i, (unsigned long)v, hex);
                break;
            }
            case KIND_OPERATION: {
                if (obj->payload_len >= sizeof(OperationPayload)) {
                    const OperationPayload *op = (const OperationPayload*)obj->payload;
                    const char *opname = (op->opcode == OP_ADD) ? "ADD" : (op->opcode == OP_SUB ? "SUB" : "OP");
                    w = snprintf(out_str + pos, max_len - pos, "op %s = %s[WRAP]; // id=%s\n", opname, opname, hex);
                } else if (obj->payload_len >= sizeof(ApplyPayload)) {
                    w = snprintf(out_str + pos, max_len - pos, "apply app_%u; // id=%s\n", i, hex);
                }
                break;
            }
            default:
                w = snprintf(out_str + pos, max_len - pos, "object kind=%u; // id=%s\n", obj->kind, hex);
                break;
        }
        if (w < 0 || (size_t)w >= max_len - pos) return -1;
        pos += (size_t)w;
    }

    return 0;
}

int omega_graph_parse_text(const char *in_str, OmegaGraph *out_graph, char *err_msg, size_t err_msg_len) {
    if (!in_str || !out_graph) {
        snprintf(err_msg, err_msg_len, "Null arguments to parse_text");
        return -1;
    }
    memset(out_graph, 0, sizeof(OmegaGraph));

    /* Simple robust parser for human text declarations */
    const char *p = in_str;
    OmegaObject *u32_type = NULL;
    OmegaObject *val_a = NULL;
    OmegaObject *val_b = NULL;
    OmegaObject *op_add = NULL;
    OmegaObject *op_sub = NULL;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '/' && *(p + 1) == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }

        if (strncmp(p, "type", 4) == 0) {
            if (strstr(p, "UNSIGNED_INTEGER(32)") != NULL) {
                u32_type = omega_build_type_uint(out_graph, 32);
            }
            while (*p && *p != '\n') p++;
        } else if (strncmp(p, "val", 3) == 0) {
            /* Parse integer value */
            const char *eq = strchr(p, '=');
            if (eq && u32_type) {
                unsigned long v = strtoul(eq + 1, NULL, 0);
                if (val_a == NULL) {
                    val_a = omega_build_val_uint(out_graph, &u32_type->id, 32, (uint64_t)v);
                } else if (val_b == NULL) {
                    val_b = omega_build_val_uint(out_graph, &u32_type->id, 32, (uint64_t)v);
                }
            }
            while (*p && *p != '\n') p++;
        } else if (strncmp(p, "op", 2) == 0) {
            if (strstr(p, "ADD") != NULL && u32_type) {
                op_add = omega_build_op_binary(out_graph, OP_ADD, OVERFLOW_WRAP, &u32_type->id);
            } else if (strstr(p, "SUB") != NULL && u32_type) {
                op_sub = omega_build_op_binary(out_graph, OP_SUB, OVERFLOW_WRAP, &u32_type->id);
            }
            while (*p && *p != '\n') p++;
        } else if (strncmp(p, "apply", 5) == 0) {
            if (op_add && val_a && val_b) {
                omega_build_apply(out_graph, &op_add->id, &val_a->id, &val_b->id);
            } else if (op_sub && val_a && val_b) {
                omega_build_apply(out_graph, &op_sub->id, &val_a->id, &val_b->id);
            }
            while (*p && *p != '\n') p++;
        } else {
            while (*p && *p != '\n') p++;
        }
    }

    return 0;
}
