/*
 * osh_expand_ref.h -- an independent C reference expander for osh R1, used ONLY for differential testing of the OSC
 * expander (osh_expand.osc). Written from the rules (E-parser-design.md section 4, ABI draft sections 6.3 and 7.1,
 * the DECISIONS in osh_expand.osc.in's header) over the reference tokenizer and parser tables, as "pieces then
 * fields" (a word becomes a sequence of pieces; each piece is applied to a field builder), not as the OSC byte
 * state machine. Not a shell, not for production.
 */
#ifndef OSH_EXPAND_REF_H
#define OSH_EXPAND_REF_H
#include <stddef.h>
#include <stdint.h>

#include "osh_lex_ref.h"
#include "osh_parse_ref.h"

#define OSH_XR_VALCAP 1300 /* room for a value above CAP_VALUE so the test can exceed it */
#define OSH_XR_MAXVARS 24
#define OSH_XR_MAXPOS 24
#define OSH_XR_OUTCAP 8192
#define OSH_XR_MAXREQ 80

typedef struct {
    int set;
    size_t len;
    uint8_t b[OSH_XR_VALCAP];
} OshXrStr;

typedef struct {
    int nvars;
    struct {
        char name[40];
        OshXrStr v;
    } var[OSH_XR_MAXVARS];
    int npos; /* $1..$npos */
    OshXrStr arg0;
    OshXrStr pos[OSH_XR_MAXPOS];
    uint64_t status; /* $? */
} OshXrEnv;

/* The host side of the NEED_VAR protocol (ABI 6.3), shared by the driver (as the host) and the reference.
 * kind 1 name[0..nlen), 2 $?, 3 positional k, 4 $#, 5 count only. *found, *len (may exceed 1024), *npos; bytes
 * copied to val (up to OSH_XR_VALCAP). */
void osh_xr_env_get(const OshXrEnv *env, int kind, const uint8_t *name, size_t nlen, uint64_t k, int *found, size_t *len, uint64_t *npos, uint8_t *val);

typedef struct {
    int kind;
    uint64_t a, len;
} OshXrReq;

typedef struct {
    unsigned status; /* 103 ready, else a refusal code */
    uint64_t err_off;
    uint64_t rec[8 + 8 * 164];
    size_t nrec;
    uint8_t out[OSH_XR_OUTCAP];
    size_t out_used;
    OshXrReq req[OSH_XR_MAXREQ];
    unsigned nreq;
} OshXrOut;

/* First pipeline index >= cand that runs, given the connector rules and the status of the last executed pipeline
 * (noskip: cand itself). Returns npipe when none is left. */
unsigned osh_xr_next(const OshRefParse *pr, unsigned cand, uint64_t last_status, int noskip);

/* Expand pipeline p of a completed list. */
void osh_xr_pipe(const uint8_t *in, size_t n, const OshRefLex *lx, const OshRefParse *pr, const OshXrEnv *env, unsigned p, OshXrOut *out);
#endif
