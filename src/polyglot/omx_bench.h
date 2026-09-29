/* POLYGLOT-0 lane F: shared helpers for the verifier, the benchmark and the
 * explainer (spec/polyglot-0.md sections 6-10). No I/O beyond /proc, /sys and
 * files named by the caller. No Python, no jq: receipts are written and read
 * by C only. */
#ifndef OMX_BENCH_H
#define OMX_BENCH_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---- deterministic RNG (splitmix64, same generator as MA-3 tests) ---- */
typedef struct { uint64_t s; } omx_rng;
uint64_t omx_rng_next(omx_rng *r);
double omx_rng_01(omx_rng *r);

/* ---- hashing ---- */
void omx_hex(const uint8_t *d, size_t n, char *out); /* out: 2n+1 chars */
/* SHA-256 of a whole file; 0 on success. */
int omx_sha256_file(const char *path, char hex[65]);

/* Contract digest (spec section 8). Rule, stated verbatim in every receipt:
 * the raw bytes of the contract file from the first byte of the first line
 * that begins with "## 1. " up to, not including, the first byte of the next
 * line that begins with "## " (end of file if none); SHA-256 of those bytes,
 * lower-case hex. 0 on success, -1 if the file or the section is missing. */
#define OMX_CONTRACT_DIGEST_RULE                                                       \
    "SHA-256 over the raw bytes of spec/polyglot-0.md from the first byte of the first " \
    "line beginning with '## 1. ' up to, not including, the first byte of the next line " \
    "beginning with '## ' (LF line endings, no normalisation)"
int omx_contract_digest(const char *spec_path, char hex[65]);

/* ---- machine facts ---- */
#define OMX_MAX_CPUS 256
#define OMX_MAX_THERMAL 32
int omx_cpu_part(int cpu);                 /* MIDR part number from sysfs, -1 unknown */
const char *omx_cluster_name(int part);    /* "X925", "A725", "unknown" */
const char *omx_core_name(int part);       /* "Cortex-X925", ... */
int omx_cpu_capacity(int cpu);             /* /sys cpu_capacity, -1 unknown */
long omx_cpu_cur_khz(int cpu);             /* scaling_cur_freq, -1 unknown */
void omx_cpu_governor(int cpu, char *buf, size_t cap);
int omx_ncpus(void);
/* Most idle CPU of the given MIDR part over ~0.3 s; -1 if none. */
int omx_pick_idle_cpu(int part, double *idle_frac);
/* Thermal zones in milli-degrees C; returns count (<= OMX_MAX_THERMAL). */
int omx_thermal_read(long mc[OMX_MAX_THERMAL]);
void omx_loadavg(char *buf, size_t cap);
void omx_kernel(char *buf, size_t cap);
uint64_t omx_run_delay_ns(void);            /* /proc/thread-self/schedstat field 2 */
double omx_now_ns(void);                    /* CLOCK_MONOTONIC_RAW */

/* ---- ELF helpers (own reader, ELF64 little-endian) ---- */
/* Sum of sh_size of SHF_EXECINSTR sections of an object/executable; -1 on error. */
long omx_elf_text_bytes(const char *path);
/* Size of the function symbol in the running executable whose range contains
 * addr; 0 if not found (stripped, or code in a shared object). */
size_t omx_self_symbol_size(const void *addr, char *name, size_t name_cap);

/* ---- JSON output ---- */
void omx_json_str(FILE *f, const char *s);  /* writes "escaped" or null */

/* ---- statistics ---- */
void omx_sort_d(double *v, size_t n);
double omx_median_sorted(const double *v, size_t n);
double omx_mad(const double *v, size_t n, double med); /* median |v - med|, v unsorted ok */

#endif /* OMX_BENCH_H */
