#ifndef OMEGA_EXEC_H
#define OMEGA_EXEC_H

#include "omega_realize.h"
#include <stdint.h>
#include <stdbool.h>

/* Native in-process execution on AArch64 host */
int omega_exec_native_f3(const RealizationObject *real, uint64_t a, uint64_t b, uint64_t c, uint64_t *out_res);

/* Bare-metal QEMU runner generation and execution */
int omega_build_qemu_runner(const RealizationObject *real, const char *out_bin_path);
int omega_exec_qemu_virt(const char *runner_bin_path, char *out_log, size_t out_log_len);

#endif /* OMEGA_EXEC_H */
