#include "omega_validate.h"
#include "omega_canonical.h"
#include <string.h>
#include <stdio.h>

static const OmegaObject* find_object_by_id(const OmegaGraph *graph, const SemanticId *id) {
    if (!graph || !id) return NULL;
    for (uint16_t i = 0; i < graph->object_count; ++i) {
        if (graph->objects[i].has_id && omega_compare_semantic_id(&graph->objects[i].id, id) == 0) {
            return &graph->objects[i];
        }
    }
    return NULL;
}

static int check_cycle_dfs(const OmegaGraph *graph, int idx, uint8_t *visited, uint8_t *in_stack, char *err_msg, size_t err_msg_len) {
    visited[idx] = 1;
    in_stack[idx] = 1;

    const OmegaObject *obj = &graph->objects[idx];

    /* Check relation targets */
    for (uint16_t r = 0; r < obj->rel_count; ++r) {
        const OmegaRelation *rel = &obj->relations[r];
        for (uint16_t j = 0; j < graph->object_count; ++j) {
            if (graph->objects[j].has_id && omega_compare_semantic_id(&graph->objects[j].id, &rel->target_id) == 0) {
                if (!visited[j]) {
                    if (check_cycle_dfs(graph, j, visited, in_stack, err_msg, err_msg_len) != 0) {
                        return -1;
                    }
                } else if (in_stack[j]) {
                    snprintf(err_msg, err_msg_len, "Structural cycle detected involving relation target at object index %u", j);
                    return -1;
                }
            }
        }
    }

    /* Check apply operand targets if apply */
    if (obj->kind == KIND_OPERATION && obj->payload_len >= sizeof(ApplyPayload)) {
        const ApplyPayload *app = (const ApplyPayload*)obj->payload;
        for (uint8_t op_idx = 0; op_idx < app->operand_count; ++op_idx) {
            for (uint16_t j = 0; j < graph->object_count; ++j) {
                if (graph->objects[j].has_id && omega_compare_semantic_id(&graph->objects[j].id, &app->operands[op_idx]) == 0) {
                    if (!visited[j]) {
                        if (check_cycle_dfs(graph, j, visited, in_stack, err_msg, err_msg_len) != 0) {
                            return -1;
                        }
                    } else if (in_stack[j]) {
                        snprintf(err_msg, err_msg_len, "Structural cycle detected involving operand target at object index %u", j);
                        return -1;
                    }
                }
            }
        }
    }

    in_stack[idx] = 0;
    return 0;
}

int omega_validate_object(const OmegaGraph *graph, const OmegaObject *obj, char *err_msg, size_t err_msg_len) {
    if (!obj) {
        snprintf(err_msg, err_msg_len, "Null object pointer");
        return -1;
    }
    if (obj->kind == KIND_INVALID || obj->kind > KIND_PROOF) {
        snprintf(err_msg, err_msg_len, "Invalid semantic kind tag 0x%02x", obj->kind);
        return -1;
    }

    /* Attribute keys must be non-empty and bounded */
    for (uint16_t i = 0; i < obj->attr_count; ++i) {
        if (strlen(obj->attributes[i].key) == 0) {
            snprintf(err_msg, err_msg_len, "Empty attribute key at index %u", i);
            return -1;
        }
    }

    /* Check relation kinds and targets */
    for (uint16_t i = 0; i < obj->rel_count; ++i) {
        if (obj->relations[i].kind == REL_INVALID || obj->relations[i].kind > REL_EQUIVALENT_TO) {
            snprintf(err_msg, err_msg_len, "Invalid relation kind 0x%04x at index %u", obj->relations[i].kind, i);
            return -1;
        }
        if (graph) {
            const OmegaObject *target = find_object_by_id(graph, &obj->relations[i].target_id);
            if (!target) {
                char hex[65];
                omega_hex_semantic_id(&obj->relations[i].target_id, hex);
                snprintf(err_msg, err_msg_len, "Dangling relation target reference %s", hex);
                return -1;
            }
        }
    }

    /* Check constraint kinds */
    for (uint16_t i = 0; i < obj->const_count; ++i) {
        if (obj->constraints[i].kind == CONST_INVALID || obj->constraints[i].kind > CONST_INVARIANT) {
            snprintf(err_msg, err_msg_len, "Invalid constraint kind 0x%04x at index %u", obj->constraints[i].kind, i);
            return -1;
        }
    }

    /* Kind-specific validation */
    switch (obj->kind) {
        case KIND_TYPE: {
            if (obj->payload_len < sizeof(TypePayload)) {
                snprintf(err_msg, err_msg_len, "Type payload length too short (%u bytes)", obj->payload_len);
                return -1;
            }
            const TypePayload *tp = (const TypePayload*)obj->payload;
            if (tp->tag == TYPE_INVALID || tp->tag > TYPE_EFFECT_RECEIPT_REF) {
                snprintf(err_msg, err_msg_len, "Invalid type tag 0x%02x", tp->tag);
                return -1;
            }
            if (tp->tag == TYPE_UNSIGNED_INT) {
                if (tp->width < 1 || tp->width > 256) {
                    snprintf(err_msg, err_msg_len, "Unsigned integer width %u out of bounds [1..256]", tp->width);
                    return -1;
                }
            } else if (tp->tag == TYPE_SIGNED_INT) {
                if (tp->width < 2 || tp->width > 256) {
                    snprintf(err_msg, err_msg_len, "Signed integer width %u out of bounds [2..256]", tp->width);
                    return -1;
                }
            } else if (tp->tag == TYPE_BITVECTOR) {
                if (tp->width < 1 || tp->width > 256) {
                    snprintf(err_msg, err_msg_len, "Bitvector width %u out of bounds [1..256]", tp->width);
                    return -1;
                }
            } else if (tp->tag == TYPE_SEQUENCE) {
                if (tp->length > 65535) {
                    snprintf(err_msg, err_msg_len, "Sequence length %u exceeds 65535", tp->length);
                    return -1;
                }
                if (graph) {
                    const OmegaObject *elem_tp = find_object_by_id(graph, &tp->elem_type);
                    if (!elem_tp || elem_tp->kind != KIND_TYPE) {
                        snprintf(err_msg, err_msg_len, "Sequence element type unresolved or not KIND_TYPE");
                        return -1;
                    }
                }
            }
            break;
        }
        case KIND_VALUE: {
            if (obj->payload_len < sizeof(ValuePayload)) {
                snprintf(err_msg, err_msg_len, "Value payload length too short (%u bytes)", obj->payload_len);
                return -1;
            }
            const ValuePayload *vp = (const ValuePayload*)obj->payload;
            if (graph) {
                const OmegaObject *tp_obj = find_object_by_id(graph, &vp->type_id);
                if (!tp_obj || tp_obj->kind != KIND_TYPE) {
                    snprintf(err_msg, err_msg_len, "Value type_id unresolved or not KIND_TYPE");
                    return -1;
                }
                const TypePayload *tp = (const TypePayload*)tp_obj->payload;
                if (tp->tag == TYPE_UNSIGNED_INT || tp->tag == TYPE_SIGNED_INT || tp->tag == TYPE_BITVECTOR) {
                    uint16_t expected_bytes = (tp->width + 7) / 8;
                    if (vp->byte_len != expected_bytes) {
                        snprintf(err_msg, err_msg_len, "Value byte length %u does not match declared type width %u (expected %u bytes)",
                                 vp->byte_len, tp->width, expected_bytes);
                        return -1;
                    }
                }
            }
            break;
        }
        case KIND_OPERATION: {
            if (obj->payload_len >= sizeof(OperationPayload)) {
                const OperationPayload *op = (const OperationPayload*)obj->payload;
                if (op->opcode == OP_INVALID || op->opcode > OP_SLICE) {
                    snprintf(err_msg, err_msg_len, "Invalid opcode 0x%02x", op->opcode);
                    return -1;
                }
                if (op->overflow == OVERFLOW_DEFAULT || op->overflow > OVERFLOW_FAIL_CLOSED) {
                    snprintf(err_msg, err_msg_len, "Operation overflow policy not explicitly declared");
                    return -1;
                }
            } else if (obj->payload_len >= sizeof(ApplyPayload)) {
                const ApplyPayload *app = (const ApplyPayload*)obj->payload;
                if (graph) {
                    const OmegaObject *op_obj = find_object_by_id(graph, &app->op_id);
                    if (!op_obj || op_obj->kind != KIND_OPERATION) {
                        snprintf(err_msg, err_msg_len, "Apply op_id unresolved or not KIND_OPERATION");
                        return -1;
                    }
                    for (uint8_t i = 0; i < app->operand_count; ++i) {
                        const OmegaObject *arg = find_object_by_id(graph, &app->operands[i]);
                        if (!arg || arg->kind != KIND_VALUE) {
                            snprintf(err_msg, err_msg_len, "Apply operand %u unresolved or not KIND_VALUE", i);
                            return -1;
                        }
                    }
                }
            }
            break;
        }
        case KIND_EFFECT: {
            if (obj->payload_len < sizeof(EffectPayload)) {
                snprintf(err_msg, err_msg_len, "Effect payload length too short (%u bytes)", obj->payload_len);
                return -1;
            }
            const EffectPayload *eff = (const EffectPayload*)obj->payload;
            if (eff->resource_class == 0) {
                snprintf(err_msg, err_msg_len, "Effect resource class is 0 (invalid resource)");
                return -1;
            }
            break;
        }
        default:
            break;
    }

    return 0;
}

int omega_validate_graph(const OmegaGraph *graph, char *err_msg, size_t err_msg_len) {
    if (!graph) {
        snprintf(err_msg, err_msg_len, "Null graph pointer");
        return -1;
    }
    if (graph->object_count == 0) {
        snprintf(err_msg, err_msg_len, "Empty graph (0 objects)");
        return -1;
    }
    if (graph->object_count > OMEGA_MAX_GRAPH_OBJECTS) {
        snprintf(err_msg, err_msg_len, "Graph object count %u exceeds maximum %d",
                 graph->object_count, OMEGA_MAX_GRAPH_OBJECTS);
        return -1;
    }

    /* 1. Validate each object individually */
    for (uint16_t i = 0; i < graph->object_count; ++i) {
        if (omega_validate_object(graph, &graph->objects[i], err_msg, err_msg_len) != 0) {
            return -1;
        }
    }

    /* 2. Check for structural cycles across the graph */
    uint8_t visited[OMEGA_MAX_GRAPH_OBJECTS] = {0};
    uint8_t in_stack[OMEGA_MAX_GRAPH_OBJECTS] = {0};

    for (uint16_t i = 0; i < graph->object_count; ++i) {
        if (!visited[i]) {
            if (check_cycle_dfs(graph, i, visited, in_stack, err_msg, err_msg_len) != 0) {
                return -1;
            }
        }
    }

    return 0;
}
