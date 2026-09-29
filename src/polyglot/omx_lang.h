/* POLYGLOT-0 language registry for Omega-X candidates. spec/polyglot-0.md
 *
 * A polyglot candidate is an MA-3 realization (oma_rz_impl: same pack/run ABI,
 * same exact contract) plus the language and toolchain it was written in.
 * C candidates are MA-3's own realizations, referenced read-only.
 * Lead-owned interface: workers add entries only in their own .c files.
 */
#ifndef OMX_LANG_H
#define OMX_LANG_H

#include <stddef.h>
#include "algebra/realize_common.h"

/* Code-size and build-time facts for candidates whose run/pack are thin
 * adapters around the real kernel (review G-B2). The bench counts every part:
 * a function part by its symbol size in the running executable, a byte part
 * (code that is not in the symbol table, e.g. emitted at run time) by the
 * byte count given. */
typedef struct {
    const char *name;     /* symbol name, or what the bytes are */
    void (*fn)(void);     /* function whose symbol size is counted; NULL: use bytes */
    size_t bytes;         /* used when fn is NULL */
} omx_code_part;

#define OMX_CODE_PARTS_MAX 6
typedef struct {
    omx_code_part part[OMX_CODE_PARTS_MAX];
    size_t nparts;
    const char *rule;     /* one line: which code the parts are */
    /* In-process build of the realization's code (own encoder: emission
     * time), reported instead of the manifest compile command; NULL: none. */
    int (*build_ns)(double *ns);
    const char *build_what;
} omx_code_desc;

typedef struct {
    const oma_rz_impl *impl;   /* pack/run, id, family (representation) */
    const char *language;      /* "c", "asm-aarch64", "omega-encoder", "mojo" */
    const char *toolchain;     /* e.g. "gcc 13.3 -O2", "gnu-as 2.42" */
    int compiler_derived;      /* 1: started from compiler output (spec section 5) */
    int toolchain_only;        /* 1: own-encoder rule, not a language result (spec section 4) */
    const char *source;        /* repo path of the main source file */
    /* NULL: the code is the run and pack functions themselves. Otherwise
     * fills d with the parts that make up the realization (call after a
     * successful pack). */
    void (*code)(omx_code_desc *d);
} omx_candidate;

/* All polyglot candidates in fixed order (C first, then others). */
size_t omx_candidate_count(void);
const omx_candidate *omx_candidate_get(size_t i);

/* Per-lane tables, each defined in the owning lane's file. Lanes that are not
 * built provide an empty table through a weak definition in omx_lang.c. */
extern const omx_candidate omx_lane_asm[];
extern const size_t omx_lane_asm_count;
extern const omx_candidate omx_lane_encoder[];
extern const size_t omx_lane_encoder_count;
extern const omx_candidate omx_lane_mojo[];
extern const size_t omx_lane_mojo_count;

#endif /* OMX_LANG_H */
