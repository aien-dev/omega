/* trn1.h -- TRN1 execution transcript (aien-protocols
 * specs/execution-transcript/TRN1_TRANSCRIPT_SPEC.md, contract 0.1.0, wire
 * schema 1): an independent C verifier, comparator and corpus runner written
 * from the spec text (no code imported), plus the RXCLOG01 -> TRN1 export.
 * libc + src/sha256.h + rxlog.h only. */
#ifndef OMEGA_TRN1_H
#define OMEGA_TRN1_H

#include "replay/rxlog.h"

#include <stddef.h>
#include <stdint.h>

enum {
    TRN1_OK = 0, TRN1_MAGIC = -1, TRN1_VERSION = -2, TRN1_LENGTH = -3, TRN1_NONCANONICAL = -4,
    TRN1_UNKNOWN = -5, TRN1_SHAPE = -6, TRN1_GAP = -7, TRN1_CHAIN = -8, TRN1_DIGEST = -9,
    TRN1_TRUNCATED = -10
};

#define TRN1_MAX_RECORDS 4096u

typedef struct {
    int code;               /* 0 accept, else a refusal code */
    uint64_t event;         /* refusal position (0 = header) */
    uint64_t records;       /* accepted: record count */
    uint8_t run_id[32], final[32];
    /* Accepted: per record (index k-1) compared digest and subsystem. */
    uint8_t (*cmp)[32];
    uint16_t *subsys;
} trn1_result;

int  trn1_verify(const uint8_t *buf, size_t len, trn1_result *r);
void trn1_result_free(trn1_result *r);
const char *trn1_subsystem_name(uint16_t id);
/* Spec section 8 lines, without newline. */
void trn1_verify_line(const trn1_result *r, char *out, size_t n);
void trn1_compare_line(const uint8_t *a, size_t na, const uint8_t *b, size_t nb, char *out, size_t n);
/* Export an RXCLOG01 log (records up to END) as TRN1, producer omega-world. */
int  trn1_from_rxlog(const rxl_log *l, uint8_t **out, size_t *out_len);

#endif
