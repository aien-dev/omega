/* omega_gpu_tgemm_api.c: the trainer's device-resident bf16 GEMM (lane E1, cut 1). See the header. */
#include "omega_gpu_tgemm_api.h"
#include "omega_gpu_prefill.h"
#include <string.h>

#define TG_TIMEOUT_MS 20000u  /* the prefill module's launch limits (private there; same values) */
#define TG_SPIN_US 200000u
static struct { uint32_t rt, warps, aqn; int mutant; OmegaGpuMem tab; } g;
void omega_gpu_tgemm_set_shape(uint32_t rt, uint32_t warps, uint32_t aqn) { g.rt = rt; g.warps = warps; g.aqn = aqn; }
void omega_gpu_tgemm_test_set_mutant(int m) { g.mutant = m; }
uint32_t omega_gpu_tgemm_bm(uint32_t n) { if (g.rt) return 16u * g.rt; const uint32_t w = g.warps ? g.warps : (n % 256u ? 8u : 16u); return w == 16u ? 128u : 64u; } /* mirrors omega_gpu_tgemm_spec */
_Static_assert(sizeof(OmegaTgemmTile) == OMEGA_TGEMM_TILE_BYTES, "tile entry");

int omega_gpu_tgemm_spec(const OmegaTgemmCall *c, OmegaTgemmSpec *out) {
    if (!c || !out) return OMEGA_GPU_PF_BAD_ARGS;
    OmegaTgemmSpec s; memset(&s, 0, sizeof s);
    s.k = c->k; s.n = c->n; s.rt = g.rt ? g.rt : 8u; s.ct = 2; s.kc = 32; s.aqn = g.aqn ? g.aqn : 1u; s.layout = c->layout; s.epi = c->epi; s.mutant = g.mutant;
    s.warps = g.warps ? g.warps : (c->n % 256u ? 8u : 16u);
    if (!g.rt) s.rt = s.warps == 16u ? 8u : 4u; /* chip sweep TGEMM-CUT1: rt 8 x 16 warps fastest on every gate shape; rt 8 x 8 warps does not fit the register file */
    if (!omega_tgemm_spec_ok(&s)) return OMEGA_GPU_PF_BAD_ARGS;
    if (c->m == 0 || c->m % omega_tgemm_bm(&s) || !c->a_va || !c->b_va || !c->c_va) return OMEGA_GPU_PF_BAD_ARGS;
    if (c->m / omega_tgemm_bm(&s) > 65535u) return OMEGA_GPU_PF_TOO_LARGE;
    if ((uint64_t)c->m * c->n >= (1ull << 32)) return OMEGA_GPU_PF_TOO_LARGE; /* the kernel forms row * n + col in 32 bits before the wide scale (review of PR 487) */
    *out = s;
    return OMEGA_GPU_PF_OK;
}

uint32_t omega_gpu_tgemm_tiles(const OmegaTgemmCall *c, OmegaTgemmTile *tab, uint32_t max) {
    OmegaTgemmSpec s;
    if (!tab || omega_gpu_tgemm_spec(c, &s) != OMEGA_GPU_PF_OK) return 0;
    const uint32_t bm = omega_tgemm_bm(&s), nt = c->m / bm;
    if (nt > max) return 0;
    for (uint32_t t = 0; t < nt; t++) { tab[t].c_va = c->c_va; tab[t].row0 = t * bm; tab[t].kb0 = 0; tab[t].nkb = c->k / 128u; tab[t].pad = 0; tab[t].pad2 = 0; }
    return nt;
}

static int gen_tgemm(const OmegaPfKey *key, void *prog) { OmegaTgemmSpec s; memcpy(&s, key->words, sizeof s); return omega_blackwell_codegen_tgemm_prog(&s, prog); }

int omega_gpu_tgemm_fill_launch_locked(const OmegaTgemmCall *c, uint64_t tiles_va, uint32_t ntiles, OmegaGpuLaunch *L, uint32_t *args) {
    OmegaTgemmSpec s;
    if (!L || !args || !tiles_va || !ntiles) return OMEGA_GPU_PF_BAD_ARGS;
    int rc = omega_gpu_tgemm_spec(c, &s);
    if (rc != OMEGA_GPU_PF_OK) return rc;
    OmegaPfKey key; memset(&key, 0, sizeof key); key.kind = OMEGA_TGEMM_KIND; memcpy(key.words, &s, sizeof s);
    _Static_assert(sizeof(OmegaTgemmSpec) <= sizeof key.words, "spec fits the key");
    OmegaPfKernel k;
    rc = omega_gpu_pf_kernel_locked(&key, gen_tgemm, &k);
    if (rc != OMEGA_GPU_PF_OK) return rc;
    const uint32_t threads = 32u * s.warps;
    if ((uint64_t)threads * k.gpr_declared > 65536u) return OMEGA_GPU_PF_TOO_LARGE;
    if (!omega_bw_gpr_declared_ok(k.gpr_declared, k.gpr_peak)) return OMEGA_GPU_PF_CHIP_FAIL;
    const uint64_t va[3] = { tiles_va, c->a_va, c->b_va };
    for (int i = 0; i < 3; i++) { args[2 * i] = (uint32_t)va[i]; args[2 * i + 1] = (uint32_t)(va[i] >> 32); }
    memset(L, 0, sizeof *L);
    L->code_va = k.code_va; L->gpr_count = k.gpr_declared; L->threads_x = threads; L->threads_y = 1;
    L->grid_x = c->n / omega_tgemm_span(&s); L->grid_y = ntiles; L->num_elements = (size_t)threads * L->grid_x * L->grid_y;
    L->args = args; L->n_args = OMEGA_TGEMM_ARGS_WORDS; L->timeout_ms = TG_TIMEOUT_MS; L->shared_bytes = omega_tgemm_shared_bytes(&s); L->spin_us = TG_SPIN_US;
    return OMEGA_GPU_PF_OK;
}

int omega_gpu_tgemm(const OmegaTgemmCall *c, uint64_t *ns) {
    OmegaTgemmSpec s;
    int rc = omega_gpu_tgemm_spec(c, &s);
    if (rc != OMEGA_GPU_PF_OK) return rc;
    const uint32_t nt = c->m / omega_tgemm_bm(&s);
    const size_t need = ((size_t)nt * OMEGA_TGEMM_TILE_BYTES + 4095u) & ~(size_t)4095u;
    OmegaGpuLaunch L; uint32_t args[OMEGA_TGEMM_ARGS_WORDS];
    /* the module table is shared state: (re)built only under the session lock, which also serializes the launch that reads it */
    omega_gpu_session_lock();
    rc = omega_gpu_pf_open_locked();
    if (rc == OMEGA_GPU_PF_OK && g.tab.size < need) { omega_gpu_session_free(&g.tab); if (omega_gpu_session_alloc(need, &g.tab) != 0) rc = OMEGA_GPU_PF_CHIP_FAIL; }
    if (rc == OMEGA_GPU_PF_OK && omega_gpu_tgemm_tiles(c, g.tab.cpu, nt) != nt) rc = OMEGA_GPU_PF_BAD_ARGS;
    if (rc == OMEGA_GPU_PF_OK && omega_gpu_session_sync_to_gpu(&g.tab) != 0) rc = OMEGA_GPU_PF_CHIP_FAIL;
    if (rc == OMEGA_GPU_PF_OK) rc = omega_gpu_tgemm_fill_launch_locked(c, g.tab.va, nt, &L, args);
    if (rc == OMEGA_GPU_PF_OK) {
        uint64_t t = 0;
        if (omega_gpu_session_launch(&L, &t, NULL) != 0) rc = OMEGA_GPU_PF_CHIP_FAIL;
        else if (ns) *ns += t;
    }
    omega_gpu_session_unlock();
    return rc;
}
