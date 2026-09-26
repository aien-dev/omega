#ifndef OMEGA_CODEC_H
#define OMEGA_CODEC_H

#include "omega_types.h"

/* Binary serialization / deserialization of graph */
int omega_graph_serialize_binary(const OmegaGraph *graph, uint8_t *out_buf, size_t max_len, size_t *out_len);
int omega_graph_deserialize_binary(const uint8_t *in_buf, size_t in_len, OmegaGraph *out_graph);

/* Human-readable representation (NON-CANONICAL) */
int omega_graph_format_text(const OmegaGraph *graph, char *out_str, size_t max_len);
int omega_graph_parse_text(const char *in_str, OmegaGraph *out_graph, char *err_msg, size_t err_msg_len);

#endif /* OMEGA_CODEC_H */
