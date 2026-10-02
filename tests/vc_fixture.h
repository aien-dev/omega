/* vc_fixture.h -- real Omega programs for Verified Crumb tests (VC1 stage 6).
 *
 * Since stage 6 the build-domain resolver recomputes the program id from the stored IR
 * (SPEC 6 step 3), so a test record can no longer name a made-up 32-byte id. Fixture n is the real
 * program "x + (n + 1)" over u64, built the way omega_program_build_unary_op builds every unary
 * program. Its semantic id is the real omega_program_compute_id, its IR blob is the real
 * omega_program_ir_encode, its source_or_ir_digest is the SHA-256 of that blob.
 *
 * Two extra blobs are served by vcfx_fetch_blob: a canonical-source stand-in (for records whose
 * digest kind is OSC source) and bytes that are not an IR at all. Include in ONE translation unit. */
#ifndef VC_FIXTURE_H
#define VC_FIXTURE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omega_program.h"
#include "omega_program_ir.h"
#include "sha256.h"

#define VCFX_UNUSED __attribute__((unused))

typedef struct { uint8_t id[32], digest[32]; uint8_t *ir; size_t ir_len; int built; } VcFx;
static VcFx g_vcfx[256];

static const VcFx *vcfx(uint8_t n) VCFX_UNUSED;
static const VcFx *vcfx(uint8_t n) {
    VcFx *f = &g_vcfx[n];
    if (f->built) return f;
    OmegaProgram *p = calloc(1, sizeof *p);
    if (!p || omega_program_build_unary_op(p, "vcfx", OP_ADD, (uint64_t)n + 1u) != 0 ||
        omega_program_ir_encode(p, &f->ir, &f->ir_len) != 0) {
        fprintf(stderr, "vc_fixture: cannot build program %u\n", (unsigned)n);
        exit(2);
    }
    memcpy(f->id, p->program_id.bytes, 32);
    sha256_hash(f->ir, f->ir_len, f->digest);
    f->built = 1;
    omega_program_destroy(p);
    free(p);
    return f;
}

/* canonical-source stand-in and non-IR bytes */
static const char VCFX_SOURCE[] = "fn add(a: u32, b: u32) -> u32 { return a + b; }\n";
static const char VCFX_GARBAGE[] = "these bytes are not an Omega program IR";
static void vcfx_source_digest(uint8_t out[32]) VCFX_UNUSED;
static void vcfx_source_digest(uint8_t out[32]) { sha256_hash((const uint8_t *)VCFX_SOURCE, sizeof VCFX_SOURCE - 1, out); }
static void vcfx_garbage_digest(uint8_t out[32]) VCFX_UNUSED;
static void vcfx_garbage_digest(uint8_t out[32]) { sha256_hash((const uint8_t *)VCFX_GARBAGE, sizeof VCFX_GARBAGE - 1, out); }

static int vcfx_copy(const void *p, size_t n, uint8_t **bytes, size_t *len) {
    *bytes = malloc(n + 1);
    if (!*bytes) return 2;
    memcpy(*bytes, p, n);
    *len = n;
    return 0;
}
/* OmegaBlobFetch over every fixture built so far plus the two extra blobs */
static int vcfx_fetch_blob(void *ctx, const uint8_t digest[32], uint8_t **bytes, size_t *len) VCFX_UNUSED;
static int vcfx_fetch_blob(void *ctx, const uint8_t digest[32], uint8_t **bytes, size_t *len) {
    (void)ctx;
    uint8_t d[32];
    for (int n = 0; n < 256; n++)
        if (g_vcfx[n].built && memcmp(g_vcfx[n].digest, digest, 32) == 0) return vcfx_copy(g_vcfx[n].ir, g_vcfx[n].ir_len, bytes, len);
    vcfx_source_digest(d);
    if (memcmp(d, digest, 32) == 0) return vcfx_copy(VCFX_SOURCE, sizeof VCFX_SOURCE - 1, bytes, len);
    vcfx_garbage_digest(d);
    if (memcmp(d, digest, 32) == 0) return vcfx_copy(VCFX_GARBAGE, sizeof VCFX_GARBAGE - 1, bytes, len);
    return 1;
}
/* a blob store that answers every digest with the same wrong bytes */
static int vcfx_fetch_wrong(void *ctx, const uint8_t digest[32], uint8_t **bytes, size_t *len) VCFX_UNUSED;
static int vcfx_fetch_wrong(void *ctx, const uint8_t digest[32], uint8_t **bytes, size_t *len) {
    (void)ctx; (void)digest;
    return vcfx_copy("not the source", 14, bytes, len);
}

#endif /* VC_FIXTURE_H */
