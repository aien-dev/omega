#ifndef OMEGA_REALIZE_H
#define OMEGA_REALIZE_H

#include "omega_types.h"
#include "aarch64_target.h"
#include <stddef.h>

typedef struct {
    SemanticId realization_id;
    bool has_id;
    SemanticId semantic_id;
    uint8_t target_profile;
    uint32_t entry_offset;
    size_t code_len;
    uint8_t code_bytes[AARCH64_MAX_CODE_BYTES];
} RealizationObject;

int omega_compute_realization_id(RealizationObject *real);
int omega_realize_pure_binary(const OmegaGraph *g, const SemanticId *op_id, RealizationObject *out_real);
int omega_realize_f_add_sub(const OmegaGraph *g, const SemanticId *root_apply_id, RealizationObject *out_real);
int omega_build_f_add_sub_graph(OmegaGraph *g, SemanticId *out_semantic_id);

#endif /* OMEGA_REALIZE_H */
