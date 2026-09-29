/* POLYGLOT-0 candidate registry (lane F). spec/polyglot-0.md sections 2-3.
 *
 * Order: C gcc -O2 (MA-3's own objects, unchanged), C gcc -O3 native flavor
 * (MA-3 sources rebuilt by mk/polyglot.mk into separate objects whose public
 * symbols are renamed oma_rz_* -> omx_o3_*), then the lane tables: hand
 * assembly (B1), own encoder (B2), Mojo (B3).
 *
 * Lanes that are not linked leave the weak empty tables below in place.
 * The -O3 objects are optional too (weak references): a binary linked
 * without them simply has no "@O3" candidates.
 */
#include "polyglot/omx_lang.h"

#include <stdio.h>
#include <string.h>

/* ---- weak empty lane tables (overridden by a lane's strong definition) ---- */
__attribute__((weak)) const omx_candidate omx_lane_asm[1] = {{0}};
__attribute__((weak)) const size_t omx_lane_asm_count = 0;
__attribute__((weak)) const omx_candidate omx_lane_encoder[1] = {{0}};
__attribute__((weak)) const size_t omx_lane_encoder_count = 0;
__attribute__((weak)) const omx_candidate omx_lane_mojo[1] = {{0}};
__attribute__((weak)) const size_t omx_lane_mojo_count = 0;

/* ---- -O3 flavor objects (renamed symbols; see mk/polyglot.mk) ---- */
extern const oma_rz_impl omx_o3_r1_plain __attribute__((weak));
extern const oma_rz_impl omx_o3_r1_sdot __attribute__((weak));
extern const oma_rz_impl omx_o3_r1_sdot_il __attribute__((weak));
extern const oma_rz_impl omx_o3_r1_smmla __attribute__((weak));
extern const oma_rz_impl omx_o3_r2_bitplane __attribute__((weak));
extern const oma_rz_impl omx_o3_r2c_crumb __attribute__((weak));

#ifndef OMX_C_O2_TOOLCHAIN
#define OMX_C_O2_TOOLCHAIN "gcc " __VERSION__ " -std=c11 -O2 -march=armv8.6-a+dotprod+i8mm+sve"
#endif
#ifndef OMX_C_O3_TOOLCHAIN
#define OMX_C_O3_TOOLCHAIN "gcc " __VERSION__ " -std=c11 -O3 -march=armv8.6-a+dotprod+i8mm+sve -mcpu=native"
#endif

#define OMX_NC_MAX 12
static omx_candidate g_c[OMX_NC_MAX];
static oma_rz_impl g_o3_impl[OMX_NC_MAX];
static char g_o3_id[OMX_NC_MAX][48];
static char g_o3_label[OMX_NC_MAX][192];
static size_t g_nc;
static int g_init;

static const char *src_of(const char *family) {
    return strcmp(family, "binary") == 0 ? "src/algebra/realize_binary.c" : "src/algebra/realize_bitplane.c";
}

static void add_c(const oma_rz_impl *im) {
    omx_candidate *c = &g_c[g_nc++];
    c->impl = im;
    c->language = "c";
    c->toolchain = OMX_C_O2_TOOLCHAIN;
    c->compiler_derived = 0;
    c->toolchain_only = 0;
    c->source = src_of(im->family);
}

static void add_o3(const oma_rz_impl *im) {
    if (!im) return; /* flavor objects not linked */
    size_t k = g_nc;
    oma_rz_impl *o = &g_o3_impl[k];
    *o = *im;
    /* The renamed object still carries MA-3's id and label strings. */
    snprintf(g_o3_id[k], sizeof g_o3_id[k], "%s@O3", im->id);
    snprintf(g_o3_label[k], sizeof g_o3_label[k], "%s [gcc -O3 native flavor]", im->label);
    o->id = g_o3_id[k];
    o->label = g_o3_label[k];
    omx_candidate *c = &g_c[g_nc++];
    c->impl = o;
    c->language = "c";
    c->toolchain = OMX_C_O3_TOOLCHAIN;
    c->compiler_derived = 0;
    c->toolchain_only = 0;
    c->source = src_of(im->family);
}

static void init(void) {
    if (g_init) return;
    g_init = 1;
    add_c(&oma_rz_r1_plain); /* labelled weak baseline (weak_baseline = 1 in MA-3) */
    add_c(&oma_rz_r1_sdot);
    add_c(&oma_rz_r1_sdot_il);
    add_c(&oma_rz_r1_smmla);
    add_c(&oma_rz_r2c_crumb);
    add_c(&oma_rz_r2_bitplane);
    add_o3(&omx_o3_r1_plain);
    add_o3(&omx_o3_r1_sdot);
    add_o3(&omx_o3_r1_sdot_il);
    add_o3(&omx_o3_r1_smmla);
    add_o3(&omx_o3_r2c_crumb);
    add_o3(&omx_o3_r2_bitplane);
}

/* Weak const objects may not be folded, but read the counts through a
 * volatile pointer anyway so no compiler sees the weak 0 as a constant. */
static size_t cnt(const size_t *p) { return *(const volatile size_t *)p; }

size_t omx_candidate_count(void) {
    init();
    return g_nc + cnt(&omx_lane_asm_count) + cnt(&omx_lane_encoder_count) + cnt(&omx_lane_mojo_count);
}

const omx_candidate *omx_candidate_get(size_t i) {
    init();
    if (i < g_nc) return &g_c[i];
    i -= g_nc;
    size_t a = cnt(&omx_lane_asm_count), e = cnt(&omx_lane_encoder_count), m = cnt(&omx_lane_mojo_count);
    if (i < a) return &omx_lane_asm[i];
    i -= a;
    if (i < e) return &omx_lane_encoder[i];
    i -= e;
    if (i < m) return &omx_lane_mojo[i];
    return NULL;
}
