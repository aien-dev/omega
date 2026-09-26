#ifndef OMEGA_VALIDATE_H
#define OMEGA_VALIDATE_H

#include "omega_types.h"

int omega_validate_object(const OmegaGraph *graph, const OmegaObject *obj, char *err_msg, size_t err_msg_len);
int omega_validate_graph(const OmegaGraph *graph, char *err_msg, size_t err_msg_len);

#endif /* OMEGA_VALIDATE_H */
