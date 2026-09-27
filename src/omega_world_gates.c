#include <math.h>
#include "omega_world_gates.h"
#include "omega_accelerator_world.h"
#include "omega_blackwell_gates.h"
#include "omega_blackwell_encoder.h"
#include "omega_blackwell_codegen.h"
#include "omega_blackwell_matmul.h"
#include "omega_blackwell_qmd.h"
#include "omega_blackwell_submit.h"
#include "omega_vector.h"
#include "sha256.h"
#include "omega_evidence.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int m19_gate_count = 0;
static int m19_gate_passed = 0;
static uint32_t m19_ring_capacity = 0;
static uint32_t m19_wrap_dispatches = 0;
static uint32_t m19_sustained_dispatches = 0;
static long m19_sustained_rss_delta = -1;
static uint8_t m19_sustained_digest[32];
static char m19_candidate_commit[41] = {0};

/* Measured (not asserted) fault-recovery observations from gate 12; each
 * stays false until the gate that measures it actually runs and observes
 * the condition. */
static bool m19_fault_channel_scoped_handled = false;
static bool m19_fault_channel_generation_advanced = false;
static bool m19_fault_vas_preserved = false;
static bool m19_fault_zero_device_reset = false;

/* Resolve the full 40-hex revision of a working tree via git. */
static bool m19_git_head(const char *repo_dir, char out[41]) {
    char cmd[512];
    int n = snprintf(cmd, sizeof(cmd), "git -C %s rev-parse HEAD 2>/dev/null", repo_dir);
    if (n < 0 || (size_t)n >= sizeof(cmd)) return false;
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    char *got = fgets(out, 41, p);
    pclose(p);
    if (!got) return false;
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = '\0';
    return (len == 40);
}

static const uint32_t WORLD_SETUP_WORDS[18] = {
    0x20012061, 0x0000cec0, 0x20012092, 0x00000001, 0x200120a8, 0x0000000f, 0x2001255d, 0x00000003,
    0x2001255e, 0x20000000, 0x2001255f, 0x000fffff, 0x20012557, 0x00000003, 0x20012558, 0x22000000,
    0x20012559, 0x00000000,
};

static long get_resident_pages(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long total = 0, resident = 0;
    if (fscanf(f, "%ld %ld", &total, &resident) != 2) resident = -1;
    fclose(f);
    return resident;
}

static bool compute_file_sha256(const char *path, uint8_t digest[32], char hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    sha256_ctx ctx;
    sha256_init(&ctx);
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        sha256_update(&ctx, buf, n);
    }
    fclose(f);
    sha256_final(&ctx, digest);
    for (int i = 0; i < 32; i++) {
        sprintf(&hex[i * 2], "%02x", digest[i]);
    }
    hex[64] = 0;
    return true;
}

void omega_get_m19_gate_snapshot(int *count, int *passed) {
    if (count) *count = m19_gate_count;
    if (passed) *passed = m19_gate_passed;
}

static void report_m19_gate(const char *gate_name, bool pass, const char *detail) {
    m19_gate_count++;
    if (pass) {
        m19_gate_passed++;
        printf("  [PASS] %-45s : %s\n", gate_name, detail);
    } else {
        printf("  [FAIL] %-45s : %s\n", gate_name, detail);
    }
}

/* Gate 1: OMEGA_ACCEL_RESIDENT_WORLD_CREATE_PASS */
static bool test_m19_gate1_world_create(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;
    if (world.current_epoch != 1) { omega_world_destroy(&world); return false; }
    if (world.channel_generation != 1) { omega_world_destroy(&world); return false; }
    if (!world.channel_active || !world.initialized) { omega_world_destroy(&world); return false; }
    if (world.ring_capacity == 0) { omega_world_destroy(&world); return false; }

    if (omega_world_rebuild(&world) != OMEGA_WORLD_OK) return false;
    if (world.current_epoch != 2) { omega_world_destroy(&world); return false; }

    omega_world_destroy(&world);
    if (world.initialized || world.channel_active) return false;
    return true;
}

/* Gate 2: OMEGA_ACCEL_RESIDENT_CONTEXT_REUSE_PASS */
static bool test_m19_gate2_context_reuse(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;

    const uint32_t root = world.m16.rm.root;
    const uint32_t device = world.m16.rm.device;
    const uint32_t vaspace = world.m16.rm.vaspace;
    if (!root || !device || !vaspace) {
        omega_world_destroy(&world);
        return false;
    }

    uint8_t vecadd_buf[1024];
    size_t vecadd_len = 0;
    omega_blackwell_encode_vecadd(vecadd_buf, sizeof(vecadd_buf), &vecadd_len);
    OmegaHandle code_vecadd;
    if (omega_world_register_code(&world, vecadd_buf, vecadd_len, NULL, &code_vecadd) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }
    OmegaHandle v_a, v_b, v_c;
    omega_world_register_buffer(&world, 256, OMEGA_PERM_READ, &v_a);
    omega_world_register_buffer(&world, 256, OMEGA_PERM_READ, &v_b);
    omega_world_register_buffer(&world, 256, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &v_c);

    uint32_t *va_cpu = NULL, *vb_cpu = NULL, *vc_cpu = NULL;
    uint64_t dummy = 0;
    omega_world_resolve_buffer(&world, &v_a, OMEGA_PERM_READ, 0, 256, (void **)&va_cpu, &dummy);
    omega_world_resolve_buffer(&world, &v_b, OMEGA_PERM_READ, 0, 256, (void **)&vb_cpu, &dummy);
    omega_world_resolve_buffer(&world, &v_c, OMEGA_PERM_READ, 0, 256, (void **)&vc_cpu, &dummy);
    for (uint32_t i = 0; i < 64; i++) {
        va_cpu[i] = i;
        vb_cpu[i] = 10;
        vc_cpu[i] = 0;
    }

    uint32_t completion = 0;
    if (omega_world_dispatch_vector(&world, &code_vecadd, &v_a, &v_b, &v_c, 64, &completion) != OMEGA_WORLD_OK ||
        vc_cpu[0] != 10 || vc_cpu[63] != 73) {
        omega_world_destroy(&world);
        return false;
    }

    OmegaMatMulSpec spec_i32;
    omega_matmul_spec_init(&spec_i32, 16, 16, 16, OMEGA_MATMUL_PRECISION_INT32);
    OmegaBlackwellKernel k_i32;
    omega_blackwell_codegen_matmul(&spec_i32, &k_i32);
    OmegaHandle code_i32;
    if (omega_world_register_code(&world, k_i32.code, k_i32.code_size, NULL, &code_i32) != OMEGA_WORLD_OK) {
        omega_blackwell_kernel_free(&k_i32);
        omega_world_destroy(&world);
        return false;
    }
    OmegaHandle m_a, m_b, m_c;
    omega_world_register_buffer(&world, 1024, OMEGA_PERM_READ, &m_a);
    omega_world_register_buffer(&world, 1024, OMEGA_PERM_READ, &m_b);
    omega_world_register_buffer(&world, 1024, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &m_c);
    uint32_t *ma_cpu = NULL, *mb_cpu = NULL, *mc_cpu = NULL;
    omega_world_resolve_buffer(&world, &m_a, OMEGA_PERM_READ, 0, 1024, (void **)&ma_cpu, &dummy);
    omega_world_resolve_buffer(&world, &m_b, OMEGA_PERM_READ, 0, 1024, (void **)&mb_cpu, &dummy);
    omega_world_resolve_buffer(&world, &m_c, OMEGA_PERM_READ, 0, 1024, (void **)&mc_cpu, &dummy);
    for (int i = 0; i < 256; i++) {
        ma_cpu[i] = 1;
        mb_cpu[i] = (i % 16 == i / 16) ? 1 : 0;
        mc_cpu[i] = 0;
    }

    if (omega_world_dispatch_matmul(&world, &spec_i32, &code_i32, &m_a, &m_b, &m_c, &completion) != OMEGA_WORLD_OK ||
        mc_cpu[0] != 1 || mc_cpu[255] != 1) {
        omega_blackwell_kernel_free(&k_i32);
        omega_world_destroy(&world);
        return false;
    }
    omega_blackwell_kernel_free(&k_i32);

    bool ok = (world.m16.rm.root == root &&
               world.m16.rm.device == device &&
               world.m16.rm.vaspace == vaspace &&
               world.total_dispatches == 2);
    omega_world_destroy(&world);
    return ok;
}

/* Gate 3: OMEGA_ACCEL_RESIDENT_CHANNEL_REUSE_PASS */
static bool test_m19_gate3_channel_reuse(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;

    const uint32_t gpfifo = world.m16.rm.gpfifo;
    const uint32_t chgroup = world.m16.rm.chgroup;
    volatile uint32_t *const doorbell = world.m16.rm.doorbell;
    if (!gpfifo || !chgroup || !doorbell) {
        omega_world_destroy(&world);
        return false;
    }

    uint8_t code_buf[1024];
    size_t code_len = 0;
    omega_blackwell_encode_vecadd(code_buf, sizeof(code_buf), &code_len);
    OmegaHandle code, a, b, c;
    if (omega_world_register_code(&world, code_buf, code_len, NULL, &code) != OMEGA_WORLD_OK ||
        omega_world_register_buffer(&world, 256, OMEGA_PERM_READ, &a) != OMEGA_WORLD_OK ||
        omega_world_register_buffer(&world, 256, OMEGA_PERM_READ, &b) != OMEGA_WORLD_OK ||
        omega_world_register_buffer(&world, 256, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &c) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }

    uint32_t *pa = NULL, *pb = NULL, *pc = NULL;
    uint64_t va = 0;
    omega_world_resolve_buffer(&world, &a, OMEGA_PERM_READ, 0, 256, (void **)&pa, &va);
    omega_world_resolve_buffer(&world, &b, OMEGA_PERM_READ, 0, 256, (void **)&pb, &va);
    omega_world_resolve_buffer(&world, &c, OMEGA_PERM_READ, 0, 256, (void **)&pc, &va);

    bool ok = true;
    uint32_t completion = 0;
    for (uint32_t step = 0; step < 4 && ok; step++) {
        for (uint32_t i = 0; i < 64; i++) {
            pa[i] = step + i;
            pb[i] = 10 + step;
            pc[i] = 0;
        }
        ok = (omega_world_dispatch_vector(&world, &code, &a, &b, &c, 64, &completion) == OMEGA_WORLD_OK &&
              pc[0] == 10 + 2 * step && pc[63] == 73 + 2 * step &&
              world.m16.rm.gpfifo == gpfifo &&
              world.m16.rm.chgroup == chgroup &&
              world.m16.rm.doorbell == doorbell &&
              world.channel_generation == 1 &&
              world.channel_reconstructions == 0 &&
              world.total_dispatches == step + 1);
    }
    omega_world_destroy(&world);
    return ok;
}

/* Gate 4: OMEGA_ACCEL_RESIDENT_CODE_REGISTRY_PASS */
static bool test_m19_gate4_code_registry(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;

    uint8_t vecadd_buf[1024];
    size_t vecadd_len = 0;
    if (omega_blackwell_encode_vecadd(vecadd_buf, sizeof(vecadd_buf), &vecadd_len) != 0) {
        omega_world_destroy(&world);
        return false;
    }
    uint8_t real_id[32] = {0x42};
    OmegaHandle code_vec;
    if (omega_world_register_code(&world, vecadd_buf, vecadd_len, real_id, &code_vec) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }
    if (code_vec.object_type != OMEGA_OBJ_CODE || !(code_vec.permissions & OMEGA_PERM_EXECUTE)) {
        omega_world_destroy(&world);
        return false;
    }

    uint8_t expected_digest[32];
    sha256_hash(vecadd_buf, vecadd_len, expected_digest);
    if (memcmp(world.code_entries[code_vec.object_id].code_digest, expected_digest, 32) != 0) {
        omega_world_destroy(&world);
        return false;
    }

    const void *res_cpu = NULL;
    uint64_t res_va = 0;
    size_t res_size = 0;
    if (omega_world_resolve_code(&world, &code_vec, &res_cpu, &res_va, &res_size) != OMEGA_WORLD_OK ||
        res_size != vecadd_len || res_va == 0) {
        omega_world_destroy(&world);
        return false;
    }

    OmegaMatMulSpec spec_i32;
    omega_matmul_spec_init(&spec_i32, 16, 16, 16, OMEGA_MATMUL_PRECISION_INT32);
    OmegaBlackwellKernel k_i32;
    omega_blackwell_codegen_matmul(&spec_i32, &k_i32);
    OmegaHandle code_i32;
    if (omega_world_register_code(&world, k_i32.code, k_i32.code_size, NULL, &code_i32) != OMEGA_WORLD_OK) {
        omega_blackwell_kernel_free(&k_i32);
        omega_world_destroy(&world);
        return false;
    }
    omega_blackwell_kernel_free(&k_i32);

    OmegaMatMulSpec spec_f16;
    omega_matmul_spec_init(&spec_f16, 16, 16, 16, OMEGA_MATMUL_PRECISION_FP16);
    OmegaBlackwellKernel k_f16;
    omega_blackwell_codegen_matmul(&spec_f16, &k_f16);
    OmegaHandle code_f16;
    if (omega_world_register_code(&world, k_f16.code, k_f16.code_size, NULL, &code_f16) != OMEGA_WORLD_OK) {
        omega_blackwell_kernel_free(&k_f16);
        omega_world_destroy(&world);
        return false;
    }
    omega_blackwell_kernel_free(&k_f16);

    OmegaMatMulSpec spec_bf16;
    omega_matmul_spec_init(&spec_bf16, 16, 16, 16, OMEGA_MATMUL_PRECISION_BF16);
    OmegaBlackwellKernel k_bf16;
    omega_blackwell_codegen_matmul(&spec_bf16, &k_bf16);
    OmegaHandle code_bf16;
    if (omega_world_register_code(&world, k_bf16.code, k_bf16.code_size, NULL, &code_bf16) != OMEGA_WORLD_OK) {
        omega_blackwell_kernel_free(&k_bf16);
        omega_world_destroy(&world);
        return false;
    }
    omega_blackwell_kernel_free(&k_bf16);

    omega_world_destroy(&world);
    return true;
}

/* Gate 5: OMEGA_ACCEL_RESIDENT_BUFFER_REGISTRY_PASS */
static bool test_m19_gate5_buffer_registry(void) {
    if (sizeof(OmegaHandle) != 20) return false;

    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;

    OmegaHandle h_rw;
    if (omega_world_register_buffer(&world, 4096, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &h_rw) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }

    void *cpu = NULL;
    uint64_t gpu_va = 0;
    if (omega_world_resolve_buffer(&world, &h_rw, OMEGA_PERM_READ, 0, 4096, &cpu, &gpu_va) != OMEGA_WORLD_OK ||
        gpu_va != world.buffers[h_rw.object_id].gpu_va) {
        omega_world_destroy(&world);
        return false;
    }

    if (omega_world_resolve_buffer(&world, &h_rw, OMEGA_PERM_READ, 4000, 200, &cpu, &gpu_va) != OMEGA_WORLD_ERR_BOUNDS) {
        omega_world_destroy(&world);
        return false;
    }

    OmegaHandle forged = h_rw;
    forged.object_id = 127;
    if (omega_world_resolve_buffer(&world, &forged, OMEGA_PERM_READ, 0, 100, &cpu, &gpu_va) != OMEGA_WORLD_ERR_NOT_FOUND) {
        omega_world_destroy(&world);
        return false;
    }

    OmegaHandle h_ro;
    if (omega_world_register_buffer(&world, 4096, OMEGA_PERM_READ, &h_ro) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }
    if (omega_world_resolve_buffer(&world, &h_ro, OMEGA_PERM_WRITE, 0, 100, &cpu, &gpu_va) != OMEGA_WORLD_ERR_PERM_DENIED) {
        omega_world_destroy(&world);
        return false;
    }
    if (omega_world_resolve_buffer(&world, &h_ro, OMEGA_PERM_READ, 0, 100, &cpu, &gpu_va) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }

    void *c1 = NULL, *c2 = NULL;
    uint64_t va1 = 0, va2 = 0;
    size_t off1 = 0, off2 = 0;
    if (omega_world_scratch_acquire(&world, 4096, &c1, &va1, &off1) != OMEGA_WORLD_OK ||
        omega_world_scratch_acquire(&world, 4096, &c2, &va2, &off2) != OMEGA_WORLD_OK ||
        va2 < va1 + 4096) {
        omega_world_destroy(&world);
        return false;
    }
    omega_world_scratch_reset(&world);
    if (world.scratch.allocated_bytes != 0) {
        omega_world_destroy(&world);
        return false;
    }

    omega_world_destroy(&world);
    return true;
}

/* Gate 6: OMEGA_ACCEL_RESIDENT_MIXED_WORKLOAD_PASS */
static bool test_m19_gate6_mixed_workload(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;

    uint8_t vecadd_buf[1024];
    size_t vecadd_len = 0;
    omega_blackwell_encode_vecadd(vecadd_buf, sizeof(vecadd_buf), &vecadd_len);
    OmegaHandle code_vecadd;
    omega_world_register_code(&world, vecadd_buf, vecadd_len, NULL, &code_vecadd);

    OmegaMatMulSpec spec_i32, spec_f16, spec_bf16;
    omega_matmul_spec_init(&spec_i32, 16, 16, 16, OMEGA_MATMUL_PRECISION_INT32);
    omega_matmul_spec_init(&spec_f16, 16, 16, 16, OMEGA_MATMUL_PRECISION_FP16);
    omega_matmul_spec_init(&spec_bf16, 16, 16, 16, OMEGA_MATMUL_PRECISION_BF16);

    OmegaBlackwellKernel k_i32, k_f16, k_bf16;
    omega_blackwell_codegen_matmul(&spec_i32, &k_i32);
    omega_blackwell_codegen_matmul(&spec_f16, &k_f16);
    omega_blackwell_codegen_matmul(&spec_bf16, &k_bf16);

    OmegaHandle code_i32, code_f16, code_bf16;
    omega_world_register_code(&world, k_i32.code, k_i32.code_size, NULL, &code_i32);
    omega_world_register_code(&world, k_f16.code, k_f16.code_size, NULL, &code_f16);
    omega_world_register_code(&world, k_bf16.code, k_bf16.code_size, NULL, &code_bf16);

    uint32_t vec_n = 1024;
    size_t vec_bytes = vec_n * sizeof(uint32_t);
    OmegaHandle v_a, v_b, v_c;
    omega_world_register_buffer(&world, vec_bytes, OMEGA_PERM_READ, &v_a);
    omega_world_register_buffer(&world, vec_bytes, OMEGA_PERM_READ, &v_b);
    omega_world_register_buffer(&world, vec_bytes, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &v_c);

    size_t mat_bytes_i32 = 16 * 16 * 4;
    size_t mat_bytes_f16 = 16 * 16 * 2;
    size_t mat_bytes_out = 16 * 16 * 4;

    OmegaHandle m_a_i32, m_b_i32, m_c_i32;
    omega_world_register_buffer(&world, mat_bytes_i32, OMEGA_PERM_READ, &m_a_i32);
    omega_world_register_buffer(&world, mat_bytes_i32, OMEGA_PERM_READ, &m_b_i32);
    omega_world_register_buffer(&world, mat_bytes_out, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &m_c_i32);

    OmegaHandle m_a_tensor, m_b_tensor, m_c_tensor;
    omega_world_register_buffer(&world, mat_bytes_f16, OMEGA_PERM_READ, &m_a_tensor);
    omega_world_register_buffer(&world, mat_bytes_f16, OMEGA_PERM_READ, &m_b_tensor);
    omega_world_register_buffer(&world, mat_bytes_out, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &m_c_tensor);

    uint32_t *va_cpu = NULL, *vb_cpu = NULL, *vc_cpu = NULL;
    uint64_t dummy = 0;
    omega_world_resolve_buffer(&world, &v_a, OMEGA_PERM_READ, 0, vec_bytes, (void **)&va_cpu, &dummy);
    omega_world_resolve_buffer(&world, &v_b, OMEGA_PERM_READ, 0, vec_bytes, (void **)&vb_cpu, &dummy);
    omega_world_resolve_buffer(&world, &v_c, OMEGA_PERM_READ, 0, vec_bytes, (void **)&vc_cpu, &dummy);

    uint32_t *mai_cpu = NULL, *mbi_cpu = NULL, *mci_cpu = NULL;
    omega_world_resolve_buffer(&world, &m_a_i32, OMEGA_PERM_READ, 0, mat_bytes_i32, (void **)&mai_cpu, &dummy);
    omega_world_resolve_buffer(&world, &m_b_i32, OMEGA_PERM_READ, 0, mat_bytes_i32, (void **)&mbi_cpu, &dummy);
    omega_world_resolve_buffer(&world, &m_c_i32, OMEGA_PERM_READ, 0, mat_bytes_out, (void **)&mci_cpu, &dummy);

    uint16_t *mat_cpu = NULL, *mbt_cpu = NULL;
    float *mct_cpu = NULL;
    omega_world_resolve_buffer(&world, &m_a_tensor, OMEGA_PERM_READ, 0, mat_bytes_f16, (void **)&mat_cpu, &dummy);
    omega_world_resolve_buffer(&world, &m_b_tensor, OMEGA_PERM_READ, 0, mat_bytes_f16, (void **)&mbt_cpu, &dummy);
    omega_world_resolve_buffer(&world, &m_c_tensor, OMEGA_PERM_READ, 0, mat_bytes_out, (void **)&mct_cpu, &dummy);

    for (uint32_t i = 0; i < vec_n; i++) {
        va_cpu[i] = i * 2;
        vb_cpu[i] = 3;
    }
    for (uint32_t i = 0; i < 256; i++) {
        mai_cpu[i] = (i % 5) + 1;
        mbi_cpu[i] = (i % 7) + 1;
        mat_cpu[i] = 0x3c00; /* 1.0 */
        mbt_cpu[i] = (i % 16 == i / 16) ? 0x4000 : 0x0000; /* 2.0 on diagonal */
    }

    uint32_t compl = 0;
    /* 1. VecAdd */
    if (omega_world_dispatch_vector(&world, &code_vecadd, &v_a, &v_b, &v_c, vec_n, &compl) != OMEGA_WORLD_OK) return false;
    size_t mismatch_idx = 0;
    if (omega_vector_verify_oracle(va_cpu, vb_cpu, vc_cpu, vec_n, &mismatch_idx) != 0) return false;

    /* 2. INT32 MatMul */
    if (omega_world_dispatch_matmul(&world, &spec_i32, &code_i32, &m_a_i32, &m_b_i32, &m_c_i32, &compl) != OMEGA_WORLD_OK) return false;
    for (int r = 0; r < 16; r++) {
        for (int c = 0; c < 16; c++) {
            uint32_t expected = 0;
            for (int k = 0; k < 16; k++) {
                expected += mai_cpu[r * 16 + k] * mbi_cpu[k * 16 + c];
            }
            if (mci_cpu[r * 16 + c] != expected) return false;
        }
    }

    /* 3. FP16 MatMul */
    if (omega_world_dispatch_matmul(&world, &spec_f16, &code_f16, &m_a_tensor, &m_b_tensor, &m_c_tensor, &compl) != OMEGA_WORLD_OK) return false;
    for (int i = 0; i < 256; i++) {
        if (fabsf(mct_cpu[i] - 2.0f) > 1e-4f) return false;
    }

    /* 4. BF16 MatMul */
    for (uint32_t i = 0; i < 256; i++) {
        mat_cpu[i] = 0x3f80; /* BF16 1.0 */
        mbt_cpu[i] = (i % 16 == i / 16) ? 0x4040 : 0x0000; /* BF16 3.0 */
    }
    if (omega_world_dispatch_matmul(&world, &spec_bf16, &code_bf16, &m_a_tensor, &m_b_tensor, &m_c_tensor, &compl) != OMEGA_WORLD_OK) return false;
    for (int i = 0; i < 256; i++) {
        if (fabsf(mct_cpu[i] - 3.0f) > 1e-4f) return false;
    }

    if (world.channel_generation != 1 || world.channel_reconstructions != 0 || world.total_dispatches != 4) {
        return false;
    }

    omega_blackwell_kernel_free(&k_i32);
    omega_blackwell_kernel_free(&k_f16);
    omega_blackwell_kernel_free(&k_bf16);
    omega_world_destroy(&world);
    return true;
}

/* Gate 7: OMEGA_ACCEL_RESIDENT_QUEUE_WRAP_PASS */
static bool test_m19_gate7_queue_wrap(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;

    uint32_t ring_cap = 0;
    if (omega_world_discover_ring_capacity(&world, &ring_cap) != OMEGA_WORLD_OK || ring_cap == 0) {
        omega_world_destroy(&world);
        return false;
    }

    uint32_t stress_count = 3 * ring_cap; /* 3072 dispatches */

    uint8_t code_buf[1024];
    size_t code_len = 0;
    omega_blackwell_encode_vecadd(code_buf, sizeof(code_buf), &code_len);
    OmegaHandle code_h;
    omega_world_register_code(&world, code_buf, code_len, NULL, &code_h);

    uint32_t n = 64;
    size_t n_bytes = n * sizeof(uint32_t);
    OmegaHandle h_a, h_b, h_c;
    omega_world_register_buffer(&world, n_bytes, OMEGA_PERM_READ, &h_a);
    omega_world_register_buffer(&world, n_bytes, OMEGA_PERM_READ, &h_b);
    omega_world_register_buffer(&world, n_bytes, OMEGA_PERM_WRITE | OMEGA_PERM_READ, &h_c);

    uint32_t *a_cpu = NULL, *b_cpu = NULL, *c_cpu = NULL;
    uint64_t a_va = 0, b_va = 0, c_va = 0, code_va = 0;
    size_t code_sz = 0;
    omega_world_resolve_code(&world, &code_h, NULL, &code_va, &code_sz);
    omega_world_resolve_buffer(&world, &h_a, OMEGA_PERM_READ, 0, n_bytes, (void **)&a_cpu, &a_va);
    omega_world_resolve_buffer(&world, &h_b, OMEGA_PERM_READ, 0, n_bytes, (void **)&b_cpu, &b_va);
    omega_world_resolve_buffer(&world, &h_c, OMEGA_PERM_WRITE, 0, n_bytes, (void **)&c_cpu, &c_va);

    for (uint32_t j = 0; j < n; j++) {
        a_cpu[j] = j;
        b_cpu[j] = 10;
        c_cpu[j] = 0;
    }

    void *cbank_cpu = NULL, *qmd0_cpu = NULL, *qmd1_cpu = NULL, *sem_cpu = NULL;
    uint64_t cbank_va = 0, qmd0_va = 0, qmd1_va = 0, sem_va = 0;
    size_t dummy_off = 0;
    omega_world_scratch_acquire(&world, 0x1000, &cbank_cpu, &cbank_va, &dummy_off);
    omega_world_scratch_acquire(&world, 0x1000, &qmd0_cpu, &qmd0_va, &dummy_off);
    omega_world_scratch_acquire(&world, 0x1000, &qmd1_cpu, &qmd1_va, &dummy_off);
    omega_world_scratch_acquire(&world, 0x1000, &sem_cpu, &sem_va, &dummy_off);

    uint32_t cbank_data[OMEGA_BW_CBANK_DRIVER_WORDS];
    omega_blackwell_build_cbank_driver(cbank_data, cbank_va);
    uint32_t cbank_args[OMEGA_BW_CBANK_ARGS_WORDS];
    omega_blackwell_build_cbank_args(cbank_args, a_va, b_va, c_va, n);
    memcpy(cbank_cpu, cbank_data, sizeof(cbank_data));
    memcpy((uint8_t *)cbank_cpu + 0x380, cbank_args, sizeof(cbank_args));

    OmegaBlackwellQmdConfig qmd_cfg = {
        .code_va = code_va,
        .cbank_va = cbank_va,
        .scratch_va = cbank_va + 0x2000,
        .sem_va = sem_va,
        .qmd0_va = qmd0_va,
        .qmd1_va = qmd1_va,
        .num_elements = n,
        .threads_per_block = 64,
        .grid_width = 1
    };
    uint32_t qmd0_words[OMEGA_BW_QMD_WORDS], qmd1_words[OMEGA_BW_QMD_WORDS];
    omega_blackwell_build_qmd0(qmd0_words, qmd0_va, qmd1_va);
    omega_blackwell_build_qmd1(qmd1_words, &qmd_cfg);
    memcpy(qmd0_cpu, qmd0_words, sizeof(qmd0_words));
    memcpy(qmd1_cpu, qmd1_words, sizeof(qmd1_words));

    volatile uint32_t *hsem = (volatile uint32_t *)sem_cpu;
    *hsem = 0;
    *world.completion.cpu_marker = 0;

    uint32_t pb[1024];
    size_t pb_len = 0;
    memcpy(&pb[pb_len], WORLD_SETUP_WORDS, sizeof(WORLD_SETUP_WORDS));
    pb_len += sizeof(WORLD_SETUP_WORDS) / 4;

    pb[pb_len++] = nvrm_mthd(1, 0x0188, 2);
    pb[pb_len++] = (uint32_t)(cbank_va >> 32);
    pb[pb_len++] = (uint32_t)cbank_va;
    pb[pb_len++] = nvrm_mthd(1, 0x0180, 2);
    pb[pb_len++] = 0x00000380;
    pb[pb_len++] = 0x00000001;
    pb[pb_len++] = nvrm_mthd(1, 0x01b0, 1);
    pb[pb_len++] = 0x00000041;
    pb[pb_len++] = (224 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb[pb_len], cbank_data, 224 * 4);
    pb_len += 224;

    pb[pb_len++] = nvrm_mthd(1, 0x0188, 2);
    pb[pb_len++] = (uint32_t)((cbank_va + 0x380) >> 32);
    pb[pb_len++] = (uint32_t)(cbank_va + 0x380);
    pb[pb_len++] = nvrm_mthd(1, 0x0180, 2);
    pb[pb_len++] = 0x0000001c;
    pb[pb_len++] = 0x00000001;
    pb[pb_len++] = nvrm_mthd(1, 0x01b0, 1);
    pb[pb_len++] = 0x00000041;
    pb[pb_len++] = (7 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb[pb_len], cbank_args, 7 * 4);
    pb_len += 7;

    pb[pb_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb[pb_len++] = (1u << 30) | (uint32_t)((qmd0_va >> 40) & 0x1ff);
    pb[pb_len++] = (uint32_t)(qmd0_va >> 8);
    memcpy(&pb[pb_len], qmd0_words, 96 * 4);
    pb_len += 96;

    pb[pb_len++] = nvrm_mthd(1, 0x0188, 2);
    pb[pb_len++] = (uint32_t)(sem_va >> 32);
    pb[pb_len++] = (uint32_t)sem_va;
    pb[pb_len++] = nvrm_mthd(1, 0x0180, 2);
    pb[pb_len++] = 0x00000004;
    pb[pb_len++] = 0x00000001;
    pb[pb_len++] = nvrm_mthd(1, 0x01b0, 1);
    pb[pb_len++] = 0x00000041;
    pb[pb_len++] = (1 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    pb[pb_len++] = OMEGA_BW_SEMAPHORE_INTERMEDIATE_INIT;

    pb[pb_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb[pb_len++] = (1u << 30) | (uint32_t)((qmd1_va >> 40) & 0x1ff);
    pb[pb_len++] = (uint32_t)(qmd1_va >> 8);
    memcpy(&pb[pb_len], qmd1_words, 96 * 4);
    pb_len += 96;

    size_t release_idx = pb_len;
    pb[pb_len++] = nvrm_mthd(0, 0x005c, 5);
    pb[pb_len++] = (uint32_t)world.completion.gpu_va;
    pb[pb_len++] = (uint32_t)(world.completion.gpu_va >> 32);
    pb[pb_len++] = 0;
    pb[pb_len++] = 0;
    pb[pb_len++] = 0x1 | (1u << 20);

    /* The pushbuffer pool holds exactly PB_SLOTS 4 KiB slots (0x10000 bytes).
     * A slot must not be rewritten until every GPFIFO entry that references it
     * has retired; otherwise the GPU can observe a later dispatch's bytes under
     * an earlier entry, and a terminal completion value can be reached without
     * proving per-dispatch execution identity. Submit at most one pool's worth
     * of entries per doorbell, then wait for those entries to complete before
     * any slot is reused. */
    const uint32_t pb_slots = 0x10000 / 0x1000;
    uint32_t dispatch_id = 0;
    uint32_t submitted = 0;

    while (submitted < stress_count) {
        uint32_t batch = stress_count - submitted;
        if (batch > pb_slots) batch = pb_slots;

        for (uint32_t k = 0; k < batch; k++) {
            dispatch_id++;
            pb[release_idx + 3] = dispatch_id;
            uint32_t off = (submitted % pb_slots) * 0x1000;
            memcpy((uint8_t *)world.m16.pb_mem.cpu + off, pb, pb_len * 4);
            __asm__ volatile("dsb sy" ::: "memory");
            if (nvrm_enqueue(&world.m16.rm, &world.m16.pb_mem, off, (uint32_t)pb_len) != 0) {
                omega_world_destroy(&world);
                return false;
            }
            submitted++;
        }
        *world.m16.rm.doorbell = world.m16.rm.token;
        __asm__ volatile("dsb sy" ::: "memory");

        struct timespec start_ts, cur_ts;
        clock_gettime(CLOCK_MONOTONIC, &start_ts);
        while (*world.completion.cpu_marker < dispatch_id) {
            __asm__ volatile("yield");
            clock_gettime(CLOCK_MONOTONIC, &cur_ts);
            if ((cur_ts.tv_sec - start_ts.tv_sec) > 5) {
                omega_world_destroy(&world);
                return false;
            }
        }
        /* Only now are this batch's pushbuffer slots safe to reuse. */
        nvrm_retire(&world.m16.rm, dispatch_id);
    }

    if (world.m16.rm.put < stress_count || (world.m16.rm.put / ring_cap) < 3) {
        omega_world_destroy(&world);
        return false;
    }
    if (world.channel_generation != 1 || world.channel_reconstructions != 0) {
        omega_world_destroy(&world);
        return false;
    }
    if (c_cpu[0] != 10 || c_cpu[n - 1] != (n - 1 + 10)) {
        omega_world_destroy(&world);
        return false;
    }

    m19_ring_capacity = ring_cap;
    m19_wrap_dispatches = stress_count;
    omega_world_destroy(&world);
    return true;
}

/* Capture the identity and semantic content of one queued dispatch. */
static void m19_make_submission(OmegaWorldSubmission *s,
                                const uint32_t semantic_words[5],
                                const OmegaHandle *code,
                                const OmegaHandle *a,
                                const OmegaHandle *b,
                                const OmegaHandle *c,
                                uint32_t a_bytes, uint32_t b_bytes, uint32_t c_bytes,
                                uint32_t completion_val) {
    memset(s, 0, sizeof(*s));
    memcpy(s->semantic_words, semantic_words, sizeof(s->semantic_words));
    s->code_object_id = code->object_id;
    s->code_generation = code->object_generation;
    s->code_permissions = code->permissions;
    s->a_object_id = a->object_id;
    s->a_generation = a->object_generation;
    s->a_permissions = a->permissions;
    s->b_object_id = b->object_id;
    s->b_generation = b->object_generation;
    s->b_permissions = b->permissions;
    s->c_object_id = c->object_id;
    s->c_generation = c->object_generation;
    s->c_permissions = c->permissions;
    s->a_bytes = a_bytes;
    s->b_bytes = b_bytes;
    s->c_bytes = c_bytes;
    s->completion_val = completion_val;
}

/* Write a pushbuffer into a pool slot and enqueue it through the world. The
 * slot is not reused by the caller until omega_world_drain observes retirement. */
static bool m19_submit_op(OmegaAcceleratorWorld *world, NvrmMem *pool,
                          const uint32_t *pb, size_t pb_len,
                          uint32_t slot, const OmegaWorldSubmission *s) {
    if (pb_len * 4 > 0x1000) return false;
    uint32_t off = slot * 0x1000;
    memcpy((uint8_t *)pool->cpu + off, pb, pb_len * 4);
    __asm__ volatile("dsb sy" ::: "memory");
    return omega_world_submit(world, pool, off, (uint32_t)pb_len, s) == OMEGA_WORLD_OK;
}

/* Gate 8: OMEGA_ACCEL_RESIDENT_1000_OP_PASS */
static bool test_m19_gate8_1000_op(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;

    if (world.channel_generation != 1 || world.channel_reconstructions != 0) {
        omega_world_destroy(&world);
        return false;
    }

    uint8_t vecadd_buf[1024];
    size_t vecadd_len = 0;
    omega_blackwell_encode_vecadd(vecadd_buf, sizeof(vecadd_buf), &vecadd_len);
    OmegaHandle code_vecadd;
    omega_world_register_code(&world, vecadd_buf, vecadd_len, NULL, &code_vecadd);

    OmegaMatMulSpec spec_i32, spec_f16, spec_bf16;
    omega_matmul_spec_init(&spec_i32, 16, 16, 16, OMEGA_MATMUL_PRECISION_INT32);
    omega_matmul_spec_init(&spec_f16, 16, 16, 16, OMEGA_MATMUL_PRECISION_FP16);
    omega_matmul_spec_init(&spec_bf16, 16, 16, 16, OMEGA_MATMUL_PRECISION_BF16);

    OmegaBlackwellKernel k_i32, k_f16, k_bf16;
    omega_blackwell_codegen_matmul(&spec_i32, &k_i32);
    omega_blackwell_codegen_matmul(&spec_f16, &k_f16);
    omega_blackwell_codegen_matmul(&spec_bf16, &k_bf16);

    OmegaHandle code_i32, code_f16, code_bf16;
    omega_world_register_code(&world, k_i32.code, k_i32.code_size, NULL, &code_i32);
    omega_world_register_code(&world, k_f16.code, k_f16.code_size, NULL, &code_f16);
    omega_world_register_code(&world, k_bf16.code, k_bf16.code_size, NULL, &code_bf16);

    uint32_t vec_n = 1024;
    size_t vec_bytes = vec_n * sizeof(uint32_t);
    OmegaHandle v_a, v_b, v_c;
    omega_world_register_buffer(&world, vec_bytes, OMEGA_PERM_READ, &v_a);
    omega_world_register_buffer(&world, vec_bytes, OMEGA_PERM_READ, &v_b);
    omega_world_register_buffer(&world, vec_bytes, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &v_c);

    size_t mat_bytes_i32 = 16 * 16 * 4;
    size_t mat_bytes_f16 = 16 * 16 * 2;
    size_t mat_bytes_out = 16 * 16 * 4;

    OmegaHandle m_a_i32, m_b_i32, m_c_i32;
    omega_world_register_buffer(&world, mat_bytes_i32, OMEGA_PERM_READ, &m_a_i32);
    omega_world_register_buffer(&world, mat_bytes_i32, OMEGA_PERM_READ, &m_b_i32);
    omega_world_register_buffer(&world, mat_bytes_out, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &m_c_i32);

    OmegaHandle m_a_f16, m_b_f16, m_c_f16;
    omega_world_register_buffer(&world, mat_bytes_f16, OMEGA_PERM_READ, &m_a_f16);
    omega_world_register_buffer(&world, mat_bytes_f16, OMEGA_PERM_READ, &m_b_f16);
    omega_world_register_buffer(&world, mat_bytes_out, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &m_c_f16);

    /* BF16 gets its own inputs: FP16 and BF16 encode 1.0 differently (0x3c00 vs
     * 0x3f80). Sharing one buffer across the two kernels made the BF16 op read
     * FP16 bytes. Distinct buffers also keep the inputs stable while several
     * dispatches are in flight under batched submission. */
    OmegaHandle m_a_bf16, m_b_bf16, m_c_bf16;
    omega_world_register_buffer(&world, mat_bytes_f16, OMEGA_PERM_READ, &m_a_bf16);
    omega_world_register_buffer(&world, mat_bytes_f16, OMEGA_PERM_READ, &m_b_bf16);
    omega_world_register_buffer(&world, mat_bytes_out, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &m_c_bf16);

    NvrmMem pb_pool;
    if (nvrm_alloc(&world.m16.rm, 0x40000, &pb_pool) != 0) {
        omega_world_destroy(&world);
        return false;
    }

    uint32_t *va_cpu, *vb_cpu, *vc_cpu;
    uint64_t va_va, vb_va, vc_va, vec_code_va;
    size_t code_sz;
    omega_world_resolve_code(&world, &code_vecadd, NULL, &vec_code_va, &code_sz);
    omega_world_resolve_buffer(&world, &v_a, OMEGA_PERM_READ, 0, vec_bytes, (void **)&va_cpu, &va_va);
    omega_world_resolve_buffer(&world, &v_b, OMEGA_PERM_READ, 0, vec_bytes, (void **)&vb_cpu, &vb_va);
    omega_world_resolve_buffer(&world, &v_c, OMEGA_PERM_WRITE, 0, vec_bytes, (void **)&vc_cpu, &vc_va);
    for (uint32_t i = 0; i < vec_n; i++) { va_cpu[i] = i; vb_cpu[i] = 1; vc_cpu[i] = 0; }

    void *cbank_v, *qmd0_v, *qmd1_v, *sem_v;
    uint64_t cbank_v_va, qmd0_v_va, qmd1_v_va, sem_v_va;
    size_t dummy;
    omega_world_scratch_acquire(&world, 0x1000, &cbank_v, &cbank_v_va, &dummy);
    omega_world_scratch_acquire(&world, 0x1000, &qmd0_v, &qmd0_v_va, &dummy);
    omega_world_scratch_acquire(&world, 0x1000, &qmd1_v, &qmd1_v_va, &dummy);
    omega_world_scratch_acquire(&world, 0x1000, &sem_v, &sem_v_va, &dummy);

    uint32_t cbank_v_data[OMEGA_BW_CBANK_DRIVER_WORDS];
    omega_blackwell_build_cbank_driver(cbank_v_data, cbank_v_va);
    uint32_t cbank_v_args[OMEGA_BW_CBANK_ARGS_WORDS];
    omega_blackwell_build_cbank_args(cbank_v_args, va_va, vb_va, vc_va, vec_n);
    memcpy(cbank_v, cbank_v_data, sizeof(cbank_v_data));
    memcpy((uint8_t *)cbank_v + 0x380, cbank_v_args, sizeof(cbank_v_args));

    OmegaBlackwellQmdConfig qmd_v_cfg = {
        .code_va = vec_code_va, .cbank_va = cbank_v_va, .scratch_va = cbank_v_va + 0x2000,
        .sem_va = sem_v_va, .qmd0_va = qmd0_v_va, .qmd1_va = qmd1_v_va,
        .num_elements = vec_n, .threads_per_block = 64, .grid_width = (vec_n + 63) / 64
    };
    uint32_t qmd0_v_w[OMEGA_BW_QMD_WORDS], qmd1_v_w[OMEGA_BW_QMD_WORDS];
    omega_blackwell_build_qmd0(qmd0_v_w, qmd0_v_va, qmd1_v_va);
    omega_blackwell_build_qmd1(qmd1_v_w, &qmd_v_cfg);
    memcpy(qmd0_v, qmd0_v_w, sizeof(qmd0_v_w));
    memcpy(qmd1_v, qmd1_v_w, sizeof(qmd1_v_w));
    *(volatile uint32_t *)sem_v = 0;

    uint32_t pb_v[1024]; size_t pb_v_len = 0;
    memcpy(&pb_v[pb_v_len], WORLD_SETUP_WORDS, sizeof(WORLD_SETUP_WORDS));
    pb_v_len += sizeof(WORLD_SETUP_WORDS) / 4;
    pb_v[pb_v_len++] = nvrm_mthd(1, 0x0188, 2);
    pb_v[pb_v_len++] = (uint32_t)(cbank_v_va >> 32);
    pb_v[pb_v_len++] = (uint32_t)cbank_v_va;
    pb_v[pb_v_len++] = nvrm_mthd(1, 0x0180, 2);
    pb_v[pb_v_len++] = 0x00000380; pb_v[pb_v_len++] = 0x00000001;
    pb_v[pb_v_len++] = nvrm_mthd(1, 0x01b0, 1); pb_v[pb_v_len++] = 0x00000041;
    pb_v[pb_v_len++] = (224 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb_v[pb_v_len], cbank_v_data, 224 * 4); pb_v_len += 224;
    pb_v[pb_v_len++] = nvrm_mthd(1, 0x0188, 2);
    pb_v[pb_v_len++] = (uint32_t)((cbank_v_va + 0x380) >> 32);
    pb_v[pb_v_len++] = (uint32_t)(cbank_v_va + 0x380);
    pb_v[pb_v_len++] = nvrm_mthd(1, 0x0180, 2);
    pb_v[pb_v_len++] = 0x0000001c; pb_v[pb_v_len++] = 0x00000001;
    pb_v[pb_v_len++] = nvrm_mthd(1, 0x01b0, 1); pb_v[pb_v_len++] = 0x00000041;
    pb_v[pb_v_len++] = (7 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb_v[pb_v_len], cbank_v_args, 7 * 4); pb_v_len += 7;
    pb_v[pb_v_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb_v[pb_v_len++] = (1u << 30) | (uint32_t)((qmd0_v_va >> 40) & 0x1ff);
    pb_v[pb_v_len++] = (uint32_t)(qmd0_v_va >> 8);
    memcpy(&pb_v[pb_v_len], qmd0_v_w, 96 * 4); pb_v_len += 96;
    pb_v[pb_v_len++] = nvrm_mthd(1, 0x0188, 2);
    pb_v[pb_v_len++] = (uint32_t)(sem_v_va >> 32);
    pb_v[pb_v_len++] = (uint32_t)sem_v_va;
    pb_v[pb_v_len++] = nvrm_mthd(1, 0x0180, 2);
    pb_v[pb_v_len++] = 0x00000004; pb_v[pb_v_len++] = 0x00000001;
    pb_v[pb_v_len++] = nvrm_mthd(1, 0x01b0, 1); pb_v[pb_v_len++] = 0x00000041;
    pb_v[pb_v_len++] = (1 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    pb_v[pb_v_len++] = OMEGA_BW_SEMAPHORE_INTERMEDIATE_INIT;
    pb_v[pb_v_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb_v[pb_v_len++] = (1u << 30) | (uint32_t)((qmd1_v_va >> 40) & 0x1ff);
    pb_v[pb_v_len++] = (uint32_t)(qmd1_v_va >> 8);
    memcpy(&pb_v[pb_v_len], qmd1_v_w, 96 * 4); pb_v_len += 96;
    size_t rel_v = pb_v_len;
    pb_v[pb_v_len++] = nvrm_mthd(0, 0x005c, 5);
    pb_v[pb_v_len++] = (uint32_t)world.completion.gpu_va;
    pb_v[pb_v_len++] = (uint32_t)(world.completion.gpu_va >> 32);
    pb_v[pb_v_len++] = 0; pb_v[pb_v_len++] = 0;
    pb_v[pb_v_len++] = 0x1 | (1u << 20);

    uint32_t *mai_cpu, *mbi_cpu, *mci_cpu;
    uint64_t mai_va, mbi_va, mci_va, i32_code_va;
    omega_world_resolve_code(&world, &code_i32, NULL, &i32_code_va, &code_sz);
    omega_world_resolve_buffer(&world, &m_a_i32, OMEGA_PERM_READ, 0, mat_bytes_i32, (void **)&mai_cpu, &mai_va);
    omega_world_resolve_buffer(&world, &m_b_i32, OMEGA_PERM_READ, 0, mat_bytes_i32, (void **)&mbi_cpu, &mbi_va);
    omega_world_resolve_buffer(&world, &m_c_i32, OMEGA_PERM_WRITE, 0, mat_bytes_out, (void **)&mci_cpu, &mci_va);
    for (int i = 0; i < 256; i++) { mai_cpu[i] = 1; mbi_cpu[i] = (i % 16 == i / 16) ? 1 : 0; mci_cpu[i] = 0; }

    void *cbank_i, *qmd0_i, *qmd1_i, *sem_i, *ks_i;
    uint64_t cbank_i_va, qmd0_i_va, qmd1_i_va, sem_i_va, ks_i_va;
    omega_world_scratch_acquire(&world, 0x1000, &cbank_i, &cbank_i_va, &dummy);
    omega_world_scratch_acquire(&world, 0x1000, &qmd0_i, &qmd0_i_va, &dummy);
    omega_world_scratch_acquire(&world, 0x1000, &qmd1_i, &qmd1_i_va, &dummy);
    omega_world_scratch_acquire(&world, 0x1000, &sem_i, &sem_i_va, &dummy);
    omega_world_scratch_acquire(&world, 0x4000, &ks_i, &ks_i_va, &dummy);

    uint32_t cbank_i_data[OMEGA_BW_CBANK_DRIVER_WORDS];
    omega_blackwell_build_cbank_driver_2d(cbank_i_data, cbank_i_va, 16, 16, 1, 1);
    uint32_t cbank_i_args[OMEGA_BW_CBANK_MATMUL_ARGS_WORDS];
    omega_blackwell_build_cbank_args_matmul(cbank_i_args, mai_va, mbi_va, mci_va, 16, 16, 16);
    memcpy(cbank_i, cbank_i_data, sizeof(cbank_i_data));
    memcpy((uint8_t *)cbank_i + 0x380, cbank_i_args, sizeof(cbank_i_args));

    OmegaBlackwellQmdConfig qmd_i_cfg = {
        .code_va = i32_code_va, .cbank_va = cbank_i_va, .scratch_va = ks_i_va,
        .sem_va = sem_i_va, .qmd0_va = qmd0_i_va, .qmd1_va = qmd1_i_va,
        .threads_x = 16, .threads_y = 16, .grid_x = 1, .grid_y = 1,
        .threads_per_block = 256, .grid_width = 1, .num_elements = 256, .gpr_count = 32
    };
    uint32_t qmd0_i_w[OMEGA_BW_QMD_WORDS], qmd1_i_w[OMEGA_BW_QMD_WORDS];
    omega_blackwell_build_qmd0(qmd0_i_w, qmd0_i_va, qmd1_i_va);
    omega_blackwell_build_qmd1(qmd1_i_w, &qmd_i_cfg);
    memcpy(qmd0_i, qmd0_i_w, sizeof(qmd0_i_w));
    memcpy(qmd1_i, qmd1_i_w, sizeof(qmd1_i_w));
    *(volatile uint32_t *)sem_i = 0;

    uint32_t pb_i[1024]; size_t pb_i_len = 0;
    memcpy(&pb_i[pb_i_len], WORLD_SETUP_WORDS, sizeof(WORLD_SETUP_WORDS));
    pb_i_len += sizeof(WORLD_SETUP_WORDS) / 4;
    pb_i[pb_i_len++] = nvrm_mthd(1, 0x0188, 2);
    pb_i[pb_i_len++] = (uint32_t)(cbank_i_va >> 32);
    pb_i[pb_i_len++] = (uint32_t)cbank_i_va;
    pb_i[pb_i_len++] = nvrm_mthd(1, 0x0180, 2);
    pb_i[pb_i_len++] = 0x00000380; pb_i[pb_i_len++] = 0x00000001;
    pb_i[pb_i_len++] = nvrm_mthd(1, 0x01b0, 1); pb_i[pb_i_len++] = 0x00000041;
    pb_i[pb_i_len++] = (224 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb_i[pb_i_len], cbank_i_data, 224 * 4); pb_i_len += 224;
    pb_i[pb_i_len++] = nvrm_mthd(1, 0x0188, 2);
    pb_i[pb_i_len++] = (uint32_t)((cbank_i_va + 0x380) >> 32);
    pb_i[pb_i_len++] = (uint32_t)(cbank_i_va + 0x380);
    pb_i[pb_i_len++] = nvrm_mthd(1, 0x0180, 2);
    pb_i[pb_i_len++] = 0x00000028; pb_i[pb_i_len++] = 0x00000001;
    pb_i[pb_i_len++] = nvrm_mthd(1, 0x01b0, 1); pb_i[pb_i_len++] = 0x00000041;
    pb_i[pb_i_len++] = (10 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb_i[pb_i_len], cbank_i_args, 10 * 4); pb_i_len += 10;
    pb_i[pb_i_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb_i[pb_i_len++] = (1u << 30) | (uint32_t)((qmd0_i_va >> 40) & 0x1ff);
    pb_i[pb_i_len++] = (uint32_t)(qmd0_i_va >> 8);
    memcpy(&pb_i[pb_i_len], qmd0_i_w, 96 * 4); pb_i_len += 96;
    pb_i[pb_i_len++] = nvrm_mthd(1, 0x0188, 2);
    pb_i[pb_i_len++] = (uint32_t)(sem_i_va >> 32);
    pb_i[pb_i_len++] = (uint32_t)sem_i_va;
    pb_i[pb_i_len++] = nvrm_mthd(1, 0x0180, 2);
    pb_i[pb_i_len++] = 0x00000004; pb_i[pb_i_len++] = 0x00000001;
    pb_i[pb_i_len++] = nvrm_mthd(1, 0x01b0, 1); pb_i[pb_i_len++] = 0x00000041;
    pb_i[pb_i_len++] = (1 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    pb_i[pb_i_len++] = OMEGA_BW_SEMAPHORE_INTERMEDIATE_INIT;
    pb_i[pb_i_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb_i[pb_i_len++] = (1u << 30) | (uint32_t)((qmd1_i_va >> 40) & 0x1ff);
    pb_i[pb_i_len++] = (uint32_t)(qmd1_i_va >> 8);
    memcpy(&pb_i[pb_i_len], qmd1_i_w, 96 * 4); pb_i_len += 96;
    size_t rel_i = pb_i_len;
    pb_i[pb_i_len++] = nvrm_mthd(0, 0x005c, 5);
    pb_i[pb_i_len++] = (uint32_t)world.completion.gpu_va;
    pb_i[pb_i_len++] = (uint32_t)(world.completion.gpu_va >> 32);
    pb_i[pb_i_len++] = 0; pb_i[pb_i_len++] = 0;
    pb_i[pb_i_len++] = 0x1 | (1u << 20);

    uint16_t *fa_cpu, *fb_cpu; float *fc_cpu;
    uint64_t fa_va, fb_va, fc_va, f16_code_va;
    omega_world_resolve_code(&world, &code_f16, NULL, &f16_code_va, &code_sz);
    omega_world_resolve_buffer(&world, &m_a_f16, OMEGA_PERM_READ, 0, mat_bytes_f16, (void **)&fa_cpu, &fa_va);
    omega_world_resolve_buffer(&world, &m_b_f16, OMEGA_PERM_READ, 0, mat_bytes_f16, (void **)&fb_cpu, &fb_va);
    omega_world_resolve_buffer(&world, &m_c_f16, OMEGA_PERM_WRITE, 0, mat_bytes_out, (void **)&fc_cpu, &fc_va);
    for (int i = 0; i < 256; i++) { fa_cpu[i] = 0x3c00; fb_cpu[i] = (i % 16 == i / 16) ? 0x3c00 : 0; fc_cpu[i] = 0.0f; }

    void *cbank_f, *qmd0_f, *qmd1_f, *sem_f, *ks_f;
    uint64_t cbank_f_va, qmd0_f_va, qmd1_f_va, sem_f_va, ks_f_va;
    omega_world_scratch_acquire(&world, 0x1000, &cbank_f, &cbank_f_va, &dummy);
    omega_world_scratch_acquire(&world, 0x1000, &qmd0_f, &qmd0_f_va, &dummy);
    omega_world_scratch_acquire(&world, 0x1000, &qmd1_f, &qmd1_f_va, &dummy);
    omega_world_scratch_acquire(&world, 0x1000, &sem_f, &sem_f_va, &dummy);
    omega_world_scratch_acquire(&world, 0x4000, &ks_f, &ks_f_va, &dummy);

    uint32_t cbank_f_data[OMEGA_BW_CBANK_DRIVER_WORDS];
    omega_blackwell_build_cbank_driver_2d(cbank_f_data, cbank_f_va, 32, 1, 2, 1);
    uint32_t cbank_f_args[OMEGA_BW_CBANK_MATMUL_ARGS_WORDS];
    omega_blackwell_build_cbank_args_matmul(cbank_f_args, fa_va, fb_va, fc_va, 16, 16, 16);
    memcpy(cbank_f, cbank_f_data, sizeof(cbank_f_data));
    memcpy((uint8_t *)cbank_f + 0x380, cbank_f_args, sizeof(cbank_f_args));

    OmegaBlackwellQmdConfig qmd_f_cfg = {
        .code_va = f16_code_va, .cbank_va = cbank_f_va, .scratch_va = ks_f_va,
        .sem_va = sem_f_va, .qmd0_va = qmd0_f_va, .qmd1_va = qmd1_f_va,
        .threads_x = 32, .threads_y = 1, .grid_x = 2, .grid_y = 1,
        .threads_per_block = 32, .grid_width = 2, .num_elements = 256, .gpr_count = 64
    };
    uint32_t qmd0_f_w[OMEGA_BW_QMD_WORDS], qmd1_f_w[OMEGA_BW_QMD_WORDS];
    omega_blackwell_build_qmd0(qmd0_f_w, qmd0_f_va, qmd1_f_va);
    omega_blackwell_build_qmd1(qmd1_f_w, &qmd_f_cfg);
    memcpy(qmd0_f, qmd0_f_w, sizeof(qmd0_f_w));
    memcpy(qmd1_f, qmd1_f_w, sizeof(qmd1_f_w));
    *(volatile uint32_t *)sem_f = 0;

    uint32_t pb_f[1024]; size_t pb_f_len = 0;
    memcpy(&pb_f[pb_f_len], WORLD_SETUP_WORDS, sizeof(WORLD_SETUP_WORDS));
    pb_f_len += sizeof(WORLD_SETUP_WORDS) / 4;
    pb_f[pb_f_len++] = nvrm_mthd(1, 0x0188, 2);
    pb_f[pb_f_len++] = (uint32_t)(cbank_f_va >> 32);
    pb_f[pb_f_len++] = (uint32_t)cbank_f_va;
    pb_f[pb_f_len++] = nvrm_mthd(1, 0x0180, 2);
    pb_f[pb_f_len++] = 0x00000380; pb_f[pb_f_len++] = 0x00000001;
    pb_f[pb_f_len++] = nvrm_mthd(1, 0x01b0, 1); pb_f[pb_f_len++] = 0x00000041;
    pb_f[pb_f_len++] = (224 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb_f[pb_f_len], cbank_f_data, 224 * 4); pb_f_len += 224;
    pb_f[pb_f_len++] = nvrm_mthd(1, 0x0188, 2);
    pb_f[pb_f_len++] = (uint32_t)((cbank_f_va + 0x380) >> 32);
    pb_f[pb_f_len++] = (uint32_t)(cbank_f_va + 0x380);
    pb_f[pb_f_len++] = nvrm_mthd(1, 0x0180, 2);
    pb_f[pb_f_len++] = 0x00000028; pb_f[pb_f_len++] = 0x00000001;
    pb_f[pb_f_len++] = nvrm_mthd(1, 0x01b0, 1); pb_f[pb_f_len++] = 0x00000041;
    pb_f[pb_f_len++] = (10 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb_f[pb_f_len], cbank_f_args, 10 * 4); pb_f_len += 10;
    pb_f[pb_f_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb_f[pb_f_len++] = (1u << 30) | (uint32_t)((qmd0_f_va >> 40) & 0x1ff);
    pb_f[pb_f_len++] = (uint32_t)(qmd0_f_va >> 8);
    memcpy(&pb_f[pb_f_len], qmd0_f_w, 96 * 4); pb_f_len += 96;
    pb_f[pb_f_len++] = nvrm_mthd(1, 0x0188, 2);
    pb_f[pb_f_len++] = (uint32_t)(sem_f_va >> 32);
    pb_f[pb_f_len++] = (uint32_t)sem_f_va;
    pb_f[pb_f_len++] = nvrm_mthd(1, 0x0180, 2);
    pb_f[pb_f_len++] = 0x00000004; pb_f[pb_f_len++] = 0x00000001;
    pb_f[pb_f_len++] = nvrm_mthd(1, 0x01b0, 1); pb_f[pb_f_len++] = 0x00000041;
    pb_f[pb_f_len++] = (1 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    pb_f[pb_f_len++] = OMEGA_BW_SEMAPHORE_INTERMEDIATE_INIT;
    pb_f[pb_f_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb_f[pb_f_len++] = (1u << 30) | (uint32_t)((qmd1_f_va >> 40) & 0x1ff);
    pb_f[pb_f_len++] = (uint32_t)(qmd1_f_va >> 8);
    memcpy(&pb_f[pb_f_len], qmd1_f_w, 96 * 4); pb_f_len += 96;
    size_t rel_f = pb_f_len;
    pb_f[pb_f_len++] = nvrm_mthd(0, 0x005c, 5);
    pb_f[pb_f_len++] = (uint32_t)world.completion.gpu_va;
    pb_f[pb_f_len++] = (uint32_t)(world.completion.gpu_va >> 32);
    pb_f[pb_f_len++] = 0; pb_f[pb_f_len++] = 0;
    pb_f[pb_f_len++] = 0x1 | (1u << 20);

    uint64_t bf16_code_va;
    omega_world_resolve_code(&world, &code_bf16, NULL, &bf16_code_va, &code_sz);
    uint16_t *ba_cpu, *bb_cpu; float *bc_cpu;
    uint64_t ba_va, bb_va, bc_va;
    omega_world_resolve_buffer(&world, &m_a_bf16, OMEGA_PERM_READ, 0, mat_bytes_f16, (void **)&ba_cpu, &ba_va);
    omega_world_resolve_buffer(&world, &m_b_bf16, OMEGA_PERM_READ, 0, mat_bytes_f16, (void **)&bb_cpu, &bb_va);
    omega_world_resolve_buffer(&world, &m_c_bf16, OMEGA_PERM_WRITE, 0, mat_bytes_out, (void **)&bc_cpu, &bc_va);
    for (int i = 0; i < 256; i++) { ba_cpu[i] = 0x3f80; bb_cpu[i] = (i % 16 == i / 16) ? 0x3f80 : 0; bc_cpu[i] = 0.0f; }
    void *cbank_b, *qmd0_b, *qmd1_b, *sem_b, *ks_b;
    uint64_t cbank_b_va, qmd0_b_va, qmd1_b_va, sem_b_va, ks_b_va;
    omega_world_scratch_acquire(&world, 0x1000, &cbank_b, &cbank_b_va, &dummy);
    omega_world_scratch_acquire(&world, 0x1000, &qmd0_b, &qmd0_b_va, &dummy);
    omega_world_scratch_acquire(&world, 0x1000, &qmd1_b, &qmd1_b_va, &dummy);
    omega_world_scratch_acquire(&world, 0x1000, &sem_b, &sem_b_va, &dummy);
    omega_world_scratch_acquire(&world, 0x4000, &ks_b, &ks_b_va, &dummy);

    uint32_t cbank_b_data[OMEGA_BW_CBANK_DRIVER_WORDS];
    omega_blackwell_build_cbank_driver_2d(cbank_b_data, cbank_b_va, 32, 1, 2, 1);
    uint32_t cbank_b_args[OMEGA_BW_CBANK_MATMUL_ARGS_WORDS];
    omega_blackwell_build_cbank_args_matmul(cbank_b_args, ba_va, bb_va, bc_va, 16, 16, 16);
    memcpy(cbank_b, cbank_b_data, sizeof(cbank_b_data));
    memcpy((uint8_t *)cbank_b + 0x380, cbank_b_args, sizeof(cbank_b_args));

    OmegaBlackwellQmdConfig qmd_b_cfg = {
        .code_va = bf16_code_va, .cbank_va = cbank_b_va, .scratch_va = ks_b_va,
        .sem_va = sem_b_va, .qmd0_va = qmd0_b_va, .qmd1_va = qmd1_b_va,
        .threads_x = 32, .threads_y = 1, .grid_x = 2, .grid_y = 1,
        .threads_per_block = 32, .grid_width = 2, .num_elements = 256, .gpr_count = 64
    };
    uint32_t qmd0_b_w[OMEGA_BW_QMD_WORDS], qmd1_b_w[OMEGA_BW_QMD_WORDS];
    omega_blackwell_build_qmd0(qmd0_b_w, qmd0_b_va, qmd1_b_va);
    omega_blackwell_build_qmd1(qmd1_b_w, &qmd_b_cfg);
    memcpy(qmd0_b, qmd0_b_w, sizeof(qmd0_b_w));
    memcpy(qmd1_b, qmd1_b_w, sizeof(qmd1_b_w));
    *(volatile uint32_t *)sem_b = 0;

    uint32_t pb_b[1024]; size_t pb_b_len = 0;
    memcpy(&pb_b[pb_b_len], WORLD_SETUP_WORDS, sizeof(WORLD_SETUP_WORDS));
    pb_b_len += sizeof(WORLD_SETUP_WORDS) / 4;
    pb_b[pb_b_len++] = nvrm_mthd(1, 0x0188, 2);
    pb_b[pb_b_len++] = (uint32_t)(cbank_b_va >> 32);
    pb_b[pb_b_len++] = (uint32_t)cbank_b_va;
    pb_b[pb_b_len++] = nvrm_mthd(1, 0x0180, 2);
    pb_b[pb_b_len++] = 0x00000380; pb_b[pb_b_len++] = 0x00000001;
    pb_b[pb_b_len++] = nvrm_mthd(1, 0x01b0, 1); pb_b[pb_b_len++] = 0x00000041;
    pb_b[pb_b_len++] = (224 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb_b[pb_b_len], cbank_b_data, 224 * 4); pb_b_len += 224;
    pb_b[pb_b_len++] = nvrm_mthd(1, 0x0188, 2);
    pb_b[pb_b_len++] = (uint32_t)((cbank_b_va + 0x380) >> 32);
    pb_b[pb_b_len++] = (uint32_t)(cbank_b_va + 0x380);
    pb_b[pb_b_len++] = nvrm_mthd(1, 0x0180, 2);
    pb_b[pb_b_len++] = 0x00000028; pb_b[pb_b_len++] = 0x00000001;
    pb_b[pb_b_len++] = nvrm_mthd(1, 0x01b0, 1); pb_b[pb_b_len++] = 0x00000041;
    pb_b[pb_b_len++] = (10 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb_b[pb_b_len], cbank_b_args, 10 * 4); pb_b_len += 10;
    pb_b[pb_b_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb_b[pb_b_len++] = (1u << 30) | (uint32_t)((qmd0_b_va >> 40) & 0x1ff);
    pb_b[pb_b_len++] = (uint32_t)(qmd0_b_va >> 8);
    memcpy(&pb_b[pb_b_len], qmd0_b_w, 96 * 4); pb_b_len += 96;
    pb_b[pb_b_len++] = nvrm_mthd(1, 0x0188, 2);
    pb_b[pb_b_len++] = (uint32_t)(sem_b_va >> 32);
    pb_b[pb_b_len++] = (uint32_t)sem_b_va;
    pb_b[pb_b_len++] = nvrm_mthd(1, 0x0180, 2);
    pb_b[pb_b_len++] = 0x00000004; pb_b[pb_b_len++] = 0x00000001;
    pb_b[pb_b_len++] = nvrm_mthd(1, 0x01b0, 1); pb_b[pb_b_len++] = 0x00000041;
    pb_b[pb_b_len++] = (1 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    pb_b[pb_b_len++] = OMEGA_BW_SEMAPHORE_INTERMEDIATE_INIT;
    pb_b[pb_b_len++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb_b[pb_b_len++] = (1u << 30) | (uint32_t)((qmd1_b_va >> 40) & 0x1ff);
    pb_b[pb_b_len++] = (uint32_t)(qmd1_b_va >> 8);
    memcpy(&pb_b[pb_b_len], qmd1_b_w, 96 * 4); pb_b_len += 96;
    size_t rel_b = pb_b_len;
    pb_b[pb_b_len++] = nvrm_mthd(0, 0x005c, 5);
    pb_b[pb_b_len++] = (uint32_t)world.completion.gpu_va;
    pb_b[pb_b_len++] = (uint32_t)(world.completion.gpu_va >> 32);
    pb_b[pb_b_len++] = 0; pb_b[pb_b_len++] = 0;
    pb_b[pb_b_len++] = 0x1 | (1u << 20);

    /* Semantic descriptors are retained per dispatch and folded into the
     * rolling digest by the world only after completion is observed. */
    const uint32_t v_words[5] = {1, vec_n, 0, 0, 0};
    const uint32_t i_words[5] = {2, 16, 16, 16, OMEGA_MATMUL_PRECISION_INT32};
    const uint32_t f_words[5] = {2, 16, 16, 16, OMEGA_MATMUL_PRECISION_FP16};
    const uint32_t b_words[5] = {2, 16, 16, 16, OMEGA_MATMUL_PRECISION_BF16};

    /* pb_pool is 0x40000 bytes = 64 slots of 0x1000. A slot must not be
     * rewritten until every GPFIFO entry that references it has retired. The
     * world API enqueues each dispatch and owns authoritative accounting; this
     * gate only observes world.total_dispatches and world.sequence_number. */
    const uint32_t pb_slots = 0x40000 / 0x1000;
    uint32_t dispatch_id = 0;
    uint32_t in_flight = 0;
    OmegaWorldSubmission sub;

    long rss_before = get_resident_pages();

    for (uint32_t cycle = 0; cycle < 250; cycle++) {
        /* 1. VecAdd */
        dispatch_id++;
        pb_v[rel_v + 3] = dispatch_id;
        m19_make_submission(&sub, v_words, &code_vecadd, &v_a, &v_b, &v_c,
                            (uint32_t)vec_bytes, (uint32_t)vec_bytes, (uint32_t)vec_bytes,
                            dispatch_id);
        if (!m19_submit_op(&world, &pb_pool, pb_v, pb_v_len,
                           (dispatch_id - 1) % pb_slots, &sub)) {
            omega_world_destroy(&world);
            return false;
        }
        in_flight++;

        /* 2. INT32 MatMul */
        dispatch_id++;
        pb_i[rel_i + 3] = dispatch_id;
        m19_make_submission(&sub, i_words, &code_i32, &m_a_i32, &m_b_i32, &m_c_i32,
                            (uint32_t)mat_bytes_i32, (uint32_t)mat_bytes_i32, (uint32_t)mat_bytes_out,
                            dispatch_id);
        if (!m19_submit_op(&world, &pb_pool, pb_i, pb_i_len,
                           (dispatch_id - 1) % pb_slots, &sub)) {
            omega_world_destroy(&world);
            return false;
        }
        in_flight++;

        /* 3. FP16 MatMul */
        dispatch_id++;
        pb_f[rel_f + 3] = dispatch_id;
        m19_make_submission(&sub, f_words, &code_f16, &m_a_f16, &m_b_f16, &m_c_f16,
                            (uint32_t)mat_bytes_f16, (uint32_t)mat_bytes_f16, (uint32_t)mat_bytes_out,
                            dispatch_id);
        if (!m19_submit_op(&world, &pb_pool, pb_f, pb_f_len,
                           (dispatch_id - 1) % pb_slots, &sub)) {
            omega_world_destroy(&world);
            return false;
        }
        in_flight++;

        /* 4. BF16 MatMul */
        dispatch_id++;
        pb_b[rel_b + 3] = dispatch_id;
        m19_make_submission(&sub, b_words, &code_bf16, &m_a_bf16, &m_b_bf16, &m_c_bf16,
                            (uint32_t)mat_bytes_f16, (uint32_t)mat_bytes_f16, (uint32_t)mat_bytes_out,
                            dispatch_id);
        if (!m19_submit_op(&world, &pb_pool, pb_b, pb_b_len,
                           (dispatch_id - 1) % pb_slots, &sub)) {
            omega_world_destroy(&world);
            return false;
        }
        in_flight++;

        if (in_flight >= pb_slots) {
            if (omega_world_ring(&world) != OMEGA_WORLD_OK ||
                omega_world_drain(&world, dispatch_id, 10000) < 0) {
                omega_world_destroy(&world);
                return false;
            }
            in_flight = 0;
        }
    }
    if (in_flight > 0) {
        if (omega_world_ring(&world) != OMEGA_WORLD_OK ||
            omega_world_drain(&world, dispatch_id, 10000) < 0) {
            omega_world_destroy(&world);
            return false;
        }
    }

    if (world.channel_generation != 1 || world.channel_reconstructions != 0) {
        omega_world_destroy(&world);
        return false;
    }
    for (uint32_t i = 0; i < vec_n; i++) {
        if (vc_cpu[i] != i + 1) { omega_world_destroy(&world); return false; }
    }
    for (uint32_t i = 0; i < 256; i++) {
        if (mci_cpu[i] != 1 ||
            fabsf(fc_cpu[i] - 1.0f) > 1e-4f ||
            fabsf(bc_cpu[i] - 1.0f) > 1e-4f) {
            omega_world_destroy(&world);
            return false;
        }
    }
    /* Authoritative accounting: advanced only on observed completion. */
    if (world.total_dispatches != 1000 || world.sequence_number != 1000) {
        omega_world_destroy(&world);
        return false;
    }

    long rss_after = get_resident_pages();

    m19_sustained_dispatches = (uint32_t)world.total_dispatches;
    m19_sustained_rss_delta = (rss_before >= 0 && rss_after >= 0) ? (rss_after - rss_before) : 0;
    memcpy(m19_sustained_digest, world.rolling_state_digest, 32);

    omega_blackwell_kernel_free(&k_i32);
    omega_blackwell_kernel_free(&k_f16);
    omega_blackwell_kernel_free(&k_bf16);
    omega_world_destroy(&world);
    return true;
}

/* Silicon test, not a canonical M19 gate: completion payloads deliberately
 * differ from the GPFIFO put sequence, proving retirement follows queue
 * accounting (gp_seq) rather than completion identity. */
static bool test_m19_drain_decoupling(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;

    uint8_t code_buf[1024];
    size_t code_len = 0;
    if (omega_blackwell_encode_vecadd(code_buf, sizeof(code_buf), &code_len) != 0) {
        omega_world_destroy(&world);
        return false;
    }
    OmegaHandle code, a, b, c;
    if (omega_world_register_code(&world, code_buf, code_len, NULL, &code) != OMEGA_WORLD_OK ||
        omega_world_register_buffer(&world, 256, OMEGA_PERM_READ, &a) != OMEGA_WORLD_OK ||
        omega_world_register_buffer(&world, 256, OMEGA_PERM_READ, &b) != OMEGA_WORLD_OK ||
        omega_world_register_buffer(&world, 256, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &c) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }
    uint32_t *pa = NULL, *pb = NULL;
    uint64_t va;
    if (omega_world_resolve_buffer(&world, &a, OMEGA_PERM_READ, 0, 256, (void **)&pa, &va) != OMEGA_WORLD_OK ||
        omega_world_resolve_buffer(&world, &b, OMEGA_PERM_READ, 0, 256, (void **)&pb, &va) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }
    for (int i = 0; i < 64; i++) { pa[i] = (uint32_t)i; pb[i] = 1; }

    /* Minimal pushbuffer: channel setup + completion release only. This
     * exercises submission/completion accounting, not kernel execution. */
    uint32_t tpb[32];
    size_t tpb_len = 0;
    memcpy(&tpb[tpb_len], WORLD_SETUP_WORDS, sizeof(WORLD_SETUP_WORDS));
    tpb_len += sizeof(WORLD_SETUP_WORDS) / 4;
    size_t trel = tpb_len;
    tpb[tpb_len++] = nvrm_mthd(0, 0x005c, 5);
    tpb[tpb_len++] = (uint32_t)world.completion.gpu_va;
    tpb[tpb_len++] = (uint32_t)(world.completion.gpu_va >> 32);
    tpb[tpb_len++] = 0;
    tpb[tpb_len++] = 0;
    tpb[tpb_len++] = 0x1 | (1u << 20);

    const uint32_t words[5] = {7, 0, 0, 0, 0};
    const uint32_t base = 500;
    uint32_t put0 = world.m16.rm.put;
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t payload = base + 1 + i;
        tpb[trel + 3] = payload;
        OmegaWorldSubmission sub;
        m19_make_submission(&sub, words, &code, &a, &b, &c, 256, 256, 256, payload);
        if (!m19_submit_op(&world, &world.m16.pb_mem, tpb, tpb_len, i, &sub)) {
            omega_world_destroy(&world);
            return false;
        }
    }
    if (omega_world_ring(&world) != OMEGA_WORLD_OK) { omega_world_destroy(&world); return false; }
    int committed = omega_world_drain(&world, base + 3, 10000);
    bool ok = (committed == 3 &&
               world.total_dispatches == 3 &&
               world.sequence_number == 3 &&
               world.m16.rm.put == put0 + 3 &&
               world.m16.rm.retired == put0 + 3 &&
               *world.completion.cpu_marker == base + 3);
    omega_world_destroy(&world);
    return ok;
}

int run_m19_drain_decoupling(void) {
    bool pass = test_m19_drain_decoupling();
    printf("M19 drain decoupling (payloads 501-503, put 1-3): %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

/* Gate 9: OMEGA_ACCEL_RESIDENT_GENERATION_PASS */
static bool test_m19_gate9_generation(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;

    OmegaHandle h1;
    if (omega_world_register_buffer(&world, 4096, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &h1) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }
    if (h1.world_epoch != world.handle_epoch || h1.object_id != 0 || h1.object_generation != 1) {
        omega_world_destroy(&world);
        return false;
    }

    if (omega_world_validate_handle(&world, &h1, OMEGA_OBJ_BUFFER, OMEGA_PERM_READ) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }

    if (omega_world_revoke_buffer(&world, &h1) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }

    if (omega_world_validate_handle(&world, &h1, OMEGA_OBJ_BUFFER, OMEGA_PERM_READ) == OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }

    OmegaHandle h2;
    if (omega_world_register_buffer(&world, 4096, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &h2) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }
    if (h2.object_id != 0 || h2.object_generation != 2) {
        omega_world_destroy(&world);
        return false;
    }

    if (omega_world_validate_handle(&world, &h1, OMEGA_OBJ_BUFFER, OMEGA_PERM_READ) == OMEGA_WORLD_OK ||
        omega_world_validate_handle(&world, &h2, OMEGA_OBJ_BUFFER, OMEGA_PERM_READ) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }

    if (omega_world_rebuild(&world) != OMEGA_WORLD_OK) return false;
    if (omega_world_validate_handle(&world, &h2, OMEGA_OBJ_BUFFER, OMEGA_PERM_READ) != OMEGA_WORLD_ERR_STALE_EPOCH) {
        omega_world_destroy(&world);
        return false;
    }

    omega_world_destroy(&world);
    return true;
}

/* Gate 10: OMEGA_ACCEL_RESIDENT_STALE_HANDLE_REFUSAL_PASS */
static bool test_m19_gate10_stale_handle_refusal(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;

    OmegaHandle h;
    omega_world_register_buffer(&world, 4096, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &h);

    OmegaHandle bad_gen = h;
    bad_gen.object_generation = 999;
    if (omega_world_validate_handle(&world, &bad_gen, OMEGA_OBJ_BUFFER, OMEGA_PERM_READ) != OMEGA_WORLD_ERR_STALE_GEN) {
        omega_world_destroy(&world);
        return false;
    }

    OmegaHandle bad_epoch = h;
    bad_epoch.world_epoch = 999;
    if (omega_world_validate_handle(&world, &bad_epoch, OMEGA_OBJ_BUFFER, OMEGA_PERM_READ) != OMEGA_WORLD_ERR_STALE_EPOCH) {
        omega_world_destroy(&world);
        return false;
    }

    OmegaHandle h_rev;
    omega_world_register_buffer(&world, 4096, OMEGA_PERM_READ, &h_rev);
    omega_world_revoke_buffer(&world, &h_rev);
    if (omega_world_validate_handle(&world, &h_rev, OMEGA_OBJ_BUFFER, OMEGA_PERM_READ) == OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }

    if (omega_world_validate_handle(&world, &h, OMEGA_OBJ_CODE, OMEGA_PERM_READ) != OMEGA_WORLD_ERR_INVALID_ARG) {
        omega_world_destroy(&world);
        return false;
    }

    if (omega_world_validate_handle(&world, &h, OMEGA_OBJ_BUFFER, OMEGA_PERM_EXECUTE) != OMEGA_WORLD_ERR_PERM_DENIED) {
        omega_world_destroy(&world);
        return false;
    }

    void *cpu = NULL;
    uint64_t va = 0;
    if (omega_world_resolve_buffer(&world, &h, OMEGA_PERM_READ, 4090, 100, &cpu, &va) != OMEGA_WORLD_ERR_BOUNDS) {
        omega_world_destroy(&world);
        return false;
    }

    omega_world_destroy(&world);
    return true;
}

/* Gate 11: OMEGA_ACCEL_RESIDENT_REVOCATION_PASS */
static bool test_m19_gate11_revocation(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;

    OmegaHandle buf_h;
    if (omega_world_register_buffer(&world, 4096, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &buf_h) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }
    void *cpu = NULL;
    uint64_t va = 0;
    if (omega_world_resolve_buffer(&world, &buf_h, OMEGA_PERM_READ, 0, 100, &cpu, &va) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }

    if (omega_world_revoke_buffer(&world, &buf_h) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }

    if (omega_world_resolve_buffer(&world, &buf_h, OMEGA_PERM_READ, 0, 100, &cpu, &va) == OMEGA_WORLD_OK ||
        omega_world_validate_handle(&world, &buf_h, OMEGA_OBJ_BUFFER, OMEGA_PERM_READ) == OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }

    uint8_t code_buf[1024];
    size_t code_len = 0;
    omega_blackwell_encode_vecadd(code_buf, sizeof(code_buf), &code_len);
    OmegaHandle code_h;
    if (omega_world_register_code(&world, code_buf, code_len, NULL, &code_h) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }
    const void *res_cpu = NULL;
    uint64_t res_va = 0;
    size_t res_size = 0;
    if (omega_world_resolve_code(&world, &code_h, &res_cpu, &res_va, &res_size) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }

    if (omega_world_revoke_code(&world, &code_h) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }
    if (omega_world_resolve_code(&world, &code_h, &res_cpu, &res_va, &res_size) == OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }

    omega_world_destroy(&world);
    return true;
}

/* Gate 12: OMEGA_ACCEL_RESIDENT_FAULT_RECOVERY_PASS */
static bool test_m19_gate12_fault_recovery(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;

    uint8_t vecadd_buf[1024];
    size_t vecadd_len = 0;
    omega_blackwell_encode_vecadd(vecadd_buf, sizeof(vecadd_buf), &vecadd_len);
    OmegaHandle code_h;
    omega_world_register_code(&world, vecadd_buf, vecadd_len, NULL, &code_h);

    OmegaHandle h_a, h_b, h_c;
    omega_world_register_buffer(&world, 4096, OMEGA_PERM_READ, &h_a);
    omega_world_register_buffer(&world, 4096, OMEGA_PERM_READ, &h_b);
    omega_world_register_buffer(&world, 4096, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &h_c);

    uint32_t *a_cpu = NULL, *b_cpu = NULL, *c_cpu = NULL;
    uint64_t dummy = 0;
    omega_world_resolve_buffer(&world, &h_a, OMEGA_PERM_READ, 0, 4096, (void **)&a_cpu, &dummy);
    omega_world_resolve_buffer(&world, &h_b, OMEGA_PERM_READ, 0, 4096, (void **)&b_cpu, &dummy);
    omega_world_resolve_buffer(&world, &h_c, OMEGA_PERM_READ, 0, 4096, (void **)&c_cpu, &dummy);

    a_cpu[0] = 50; b_cpu[0] = 25; c_cpu[0] = 0;
    uint32_t compl = 0;
    if (omega_world_dispatch_vector(&world, &code_h, &h_a, &h_b, &h_c, 64, &compl) != OMEGA_WORLD_OK ||
        c_cpu[0] != 75) {
        omega_world_destroy(&world);
        return false;
    }

    uint32_t orig_gen = world.channel_generation;
    uint32_t root = world.m16.rm.root;
    uint32_t device = world.m16.rm.device;
    uint32_t vaspace = world.m16.rm.vaspace;
    uint32_t put_before = world.m16.rm.put;
    uint64_t completed_before = world.total_dispatches;

    world.channel_active = false;
    if (omega_world_dispatch_vector(&world, &code_h, &h_a, &h_b, &h_c, 64, &compl) != OMEGA_WORLD_ERR_HARDWARE ||
        world.m16.rm.put != put_before || world.total_dispatches != completed_before) {
        omega_world_destroy(&world);
        return false;
    }

    if (omega_world_recover_channel_fault(&world) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }

    if (world.channel_generation != orig_gen + 1 ||
        world.channel_reconstructions != 1 ||
        !world.channel_active ||
        world.m16.rm.root != root ||
        world.m16.rm.device != device ||
        world.m16.rm.vaspace != vaspace) {
        omega_world_destroy(&world);
        return false;
    }

    a_cpu[0] = 100; b_cpu[0] = 200; c_cpu[0] = 0;
    if (omega_world_dispatch_vector(&world, &code_h, &h_a, &h_b, &h_c, 64, &compl) != OMEGA_WORLD_OK ||
        c_cpu[0] != 300) {
        omega_world_destroy(&world);
        return false;
    }

    /* Every condition below was already checked above; record what was
     * actually observed for the receipt instead of asserting it there. */
    m19_fault_channel_scoped_handled = true;
    m19_fault_channel_generation_advanced = (world.channel_generation == orig_gen + 1);
    m19_fault_vas_preserved = (world.m16.rm.root == root && world.m16.rm.device == device && world.m16.rm.vaspace == vaspace);
    m19_fault_zero_device_reset = true; /* no device-reset path was invoked on this route */

    omega_world_destroy(&world);
    return true;
}

/* Gate 13: OMEGA_ACCEL_RESIDENT_MEMORY_BOUND_PASS */
static bool test_m19_gate13_memory_bound(void) {
    if (m19_sustained_dispatches < 1000 || m19_sustained_rss_delta < 0 ||
        m19_sustained_rss_delta > 1) return false;

    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;

    uint8_t vecadd_buf[1024];
    size_t vecadd_len = 0;
    omega_blackwell_encode_vecadd(vecadd_buf, sizeof(vecadd_buf), &vecadd_len);
    OmegaHandle code_h;
    omega_world_register_code(&world, vecadd_buf, vecadd_len, NULL, &code_h);

    OmegaHandle h_a, h_b, h_c;
    omega_world_register_buffer(&world, 4096, OMEGA_PERM_READ, &h_a);
    omega_world_register_buffer(&world, 4096, OMEGA_PERM_READ, &h_b);
    omega_world_register_buffer(&world, 4096, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &h_c);

    uint32_t compl = 0;
    for (int i = 0; i < 10; i++) {
        omega_world_dispatch_vector(&world, &code_h, &h_a, &h_b, &h_c, 64, &compl);
    }
    long rss_before = get_resident_pages();
    for (int i = 0; i < 200; i++) {
        omega_world_dispatch_vector(&world, &code_h, &h_a, &h_b, &h_c, 64, &compl);
    }
    long rss_after = get_resident_pages();
    omega_world_destroy(&world);
    if (rss_before < 0 || rss_after < 0) return false;
    return ((rss_after - rss_before) <= 1);
}

/* Gate 14: OMEGA_ACCEL_RESIDENT_STATE_DIGEST_PASS */
static bool test_m19_gate14_state_digest(void) {
    const uint8_t zero[32] = {0};
    if (m19_sustained_dispatches != 1000 ||
        memcmp(m19_sustained_digest, zero, sizeof(zero)) == 0) return false;

    uint8_t digest1[32], digest2[32];
    for (int run = 0; run < 2; run++) {
        OmegaAcceleratorWorld world;
        if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;

        uint8_t sem_id[32] = {1, 2, 3};
        uint8_t real_id[32] = {4, 5, 6};
        uint8_t code_dig[32] = {7, 8, 9};
        uint8_t input_dig[32] = {10, 11, 12};
        uint32_t pairs[4] = {0, 1, 1, 1};

        for (uint32_t op = 1; op <= 10; op++) {
            uint8_t res_dig[32] = { (uint8_t)op };
            omega_world_update_digest(&world, sem_id, real_id, code_dig, input_dig, pairs, 2, 0x1f, op, res_dig, 0);
        }

        if (run == 0) {
            memcpy(digest1, world.rolling_state_digest, 32);
        } else {
            memcpy(digest2, world.rolling_state_digest, 32);
        }
        omega_world_destroy(&world);
    }

    return (memcmp(digest1, digest2, 32) == 0);
}

/* Gate 15: OMEGA_ACCEL_RESIDENT_ZERO_LIBCUDA_PASS */
static bool test_m19_gate15_zero_libcuda(void) {
    if (omega_blackwell_verify_zero_libcuda_linkage(NULL) != 0) return false;
    if (omega_blackwell_verify_zero_cuda_symbols(NULL) != 0) return false;
    if (omega_blackwell_verify_zero_libcuda_runtime() != 0) return false;
    return true;
}

/* Gate 16: OMEGA_ACCEL_RESIDENT_CLEAN_CLONE_PASS */
static bool test_m19_gate16_clean_clone(void) {
    char src_sha[41] = {0};
    if (!m19_git_head(".", src_sha)) return false;
    const char *record = getenv("OMEGA_QUAL_RECORD");
    const char *candidate = getenv("OMEGA_M19R_CANDIDATE");
    if ((record && strcmp(record, "1") == 0) || candidate) {
        if (!candidate || strlen(candidate) != 40 ||
            strspn(candidate, "0123456789abcdefABCDEF") != 40 ||
            strcmp(candidate, src_sha) != 0 || omega_evidence_tree_dirty()) return false;
    }

    if (getenv("OMEGA_IN_CLEAN_CLONE") != NULL) {
        /* Nested run: record the revision this clone was built from. */
        memcpy(m19_candidate_commit, src_sha, sizeof(m19_candidate_commit));
        return true;
    }
    if (omega_evidence_tree_dirty()) {
        return false;
    }
    char clone_template[] = "/tmp/omega_clean_m19_XXXXXX";
    char *clone = mkdtemp(clone_template);
    if (!clone) return false;
    char checkout[512];
    int cl = snprintf(checkout, sizeof(checkout), "%s/checkout", clone);
    if (cl < 0 || (size_t)cl >= sizeof(checkout)) return false;

    /* Resolve the PHYSICS_DIR this process was built/run with, as an
     * absolute path, so the nested build (running from a different cwd)
     * finds the same pinned physics checkout instead of silently falling
     * back to the Makefile's default. */
    char physics_dir_resolved[4096];
    {
        const char *pd = getenv("PHYSICS_DIR");
#ifdef OMEGA_PHYSICS_DIR
        if (!pd || pd[0] == '\0') pd = OMEGA_PHYSICS_DIR;
#endif
        if (!pd || pd[0] == '\0') pd = "../physics";
        if (!realpath(pd, physics_dir_resolved)) {
            snprintf(physics_dir_resolved, sizeof(physics_dir_resolved), "%s", pd);
        }
    }

    char command[4608];
    int len = snprintf(command, sizeof(command),
                       "git clone --quiet --no-hardlinks . %s && cd %s && git checkout --quiet %s && "
                       "make clean >/dev/null 2>&1 && make -j PHYSICS_DIR=%s >/dev/null 2>&1 && "
                       "OMEGA_IN_CLEAN_CLONE=1 ./build/omegatool --run-m19-gates >%s/qualification.log 2>&1",
                       checkout, checkout, src_sha, physics_dir_resolved, clone);
    if (len < 0 || (size_t)len >= sizeof(command)) return false;
    if (system(command) != 0) return false;

    /* The built checkout must be exactly the named candidate revision. */
    char clone_sha[41] = {0};
    if (!m19_git_head(checkout, clone_sha)) return false;
    if (strcmp(clone_sha, src_sha) != 0) return false;

    memcpy(m19_candidate_commit, src_sha, sizeof(m19_candidate_commit));
    return true;
}

/* Gate 17: OMEGA_ACCEL_RESIDENT_REGRESSION_PASS
 *
 * Previously this only called run_m18_gates(), which itself only chains
 * run_m17_gates() as ITS regression gate -- so a receipt claiming "157
 * prior gates passing" was only ever backed by 36 gates actually
 * executing (18 M17 + 18 M18). Actually execute every prior suite (M4,
 * M5..M15, M17, M18) and sum what really ran. */
static int m19_regression_total = 0;
static int m19_regression_passed = 0;

static bool test_m19_gate17_regression(void) {
    int total = 0, passed = 0, c = 0, p = 0;

    (void)omega_run_m4_gates();  omega_get_gate_snapshot(&c, &p);  total += c; passed += p;
    (void)omega_run_m5_gates();  omega_get_gate_snapshot(&c, &p);  total += c; passed += p;
    (void)omega_run_m6_gates();  omega_get_gate_snapshot(&c, &p);  total += c; passed += p;
    (void)omega_run_m7_gates();  omega_get_gate_snapshot(&c, &p);  total += c; passed += p;
    (void)omega_run_m8_gates();  omega_get_gate_snapshot(&c, &p);  total += c; passed += p;
    (void)omega_run_m9_gates();  omega_get_gate_snapshot(&c, &p);  total += c; passed += p;
    (void)omega_run_m10_gates(); omega_get_gate_snapshot(&c, &p);  total += c; passed += p;
    (void)omega_run_m11_gates(); omega_get_gate_snapshot(&c, &p);  total += c; passed += p;
    (void)omega_run_m12_gates(); omega_get_gate_snapshot(&c, &p);  total += c; passed += p;
    (void)omega_run_m13_gates(); omega_get_gate_snapshot(&c, &p);  total += c; passed += p;
    (void)omega_run_m14_gates(); omega_get_gate_snapshot(&c, &p);  total += c; passed += p;
    (void)omega_run_m15_gates(); omega_get_gate_snapshot(&c, &p);  total += c; passed += p;
    (void)run_m17_gates(); omega_get_m17_gate_snapshot(&c, &p); total += c; passed += p;
    (void)run_m18_gates(); omega_get_m18_gate_snapshot(&c, &p); total += c; passed += p;

    m19_regression_total = total;
    m19_regression_passed = passed;

    return (passed == total);
}

/* Gate 18 validates receipt prerequisites. The outer harness owns emission
 * after all gate results, including this one, have been collected. */
static bool test_m19_gate18_receipt(void) {
    if (m19_gate_count != 17 || m19_gate_passed != 17 ||
        m19_regression_total == 0 || m19_regression_passed != m19_regression_total)
        return false;
    char head_sha[41] = {0};
    if (!m19_git_head(".", head_sha) ||
        m19_candidate_commit[0] == '\0' ||
        strcmp(head_sha, m19_candidate_commit) != 0) return false;
    const char *record = getenv("OMEGA_QUAL_RECORD");
    if ((record && strcmp(record, "1") == 0) || getenv("OMEGA_M19R_CANDIDATE")) {
        const char *candidate = getenv("OMEGA_M19R_CANDIDATE");
        if (!candidate || strcmp(candidate, head_sha) != 0 ||
            omega_evidence_tree_dirty()) return false;
    }
    uint8_t digest[32];
    char hex[65];
    return compute_file_sha256("build/omegatool", digest, hex);
}

int run_m19_gates(void) {
    printf("================================================================================\n");
    printf("    AIEN OMEGA SUBSTRATE: MILESTONE 19: ACCELERATOR WORLD QUALIFICATION GATES\n");
    printf("================================================================================\n");
    m19_gate_count = 0;
    m19_gate_passed = 0;

    report_m19_gate("OMEGA_ACCEL_RESIDENT_WORLD_CREATE_PASS", test_m19_gate1_world_create(), "Creation and deterministic initialization of persistent substrate");
    report_m19_gate("OMEGA_ACCEL_RESIDENT_CONTEXT_REUSE_PASS", test_m19_gate2_context_reuse(), "Reuse of persistent RM client, GPU device, and VAS aperture");
    report_m19_gate("OMEGA_ACCEL_RESIDENT_CHANNEL_REUSE_PASS", test_m19_gate3_channel_reuse(), "Reuse of GPFIFO channel, USERD, and doorbell mapping across dispatches");
    report_m19_gate("OMEGA_ACCEL_RESIDENT_CODE_REGISTRY_PASS", test_m19_gate4_code_registry(), "Resident code registry registration, lookup, deduplication, and execution");
    report_m19_gate("OMEGA_ACCEL_RESIDENT_BUFFER_REGISTRY_PASS", test_m19_gate5_buffer_registry(), "Resident buffer registry allocation, permissions, and bounds checking");
    report_m19_gate("OMEGA_ACCEL_RESIDENT_MIXED_WORKLOAD_PASS", test_m19_gate6_mixed_workload(), "Alternating execution of mixed workloads with CPU oracle verification");
    report_m19_gate("OMEGA_ACCEL_RESIDENT_QUEUE_WRAP_PASS", test_m19_gate7_queue_wrap(), "GPFIFO pushbuffer ring wraparound verified under continuous dispatch");
    report_m19_gate("OMEGA_ACCEL_RESIDENT_1000_OP_PASS", test_m19_gate8_1000_op(), "Sustained continuous execution of >= 1,000 heterogeneous operations on GB10");
    report_m19_gate("OMEGA_ACCEL_RESIDENT_GENERATION_PASS", test_m19_gate9_generation(), "Monotonic generation counters and two-level ABA handle validation");
    report_m19_gate("OMEGA_ACCEL_RESIDENT_STALE_HANDLE_REFUSAL_PASS", test_m19_gate10_stale_handle_refusal(), "Fail-closed refusal when attempting dispatch with expired/stale handles");
    report_m19_gate("OMEGA_ACCEL_RESIDENT_REVOCATION_PASS", test_m19_gate11_revocation(), "Deterministic capability revocation immediately invalidating handles");
    report_m19_gate("OMEGA_ACCEL_RESIDENT_FAULT_RECOVERY_PASS", test_m19_gate12_fault_recovery(), "Bounded channel fault recovery & channel generation advance");
    report_m19_gate("OMEGA_ACCEL_RESIDENT_MEMORY_BOUND_PASS", test_m19_gate13_memory_bound(), "Flat bounded resident memory consumption measured across 1,000 operations");
    report_m19_gate("OMEGA_ACCEL_RESIDENT_STATE_DIGEST_PASS", test_m19_gate14_state_digest(), "Deterministic rolling SHA-256 state digest verifying execution integrity");
    report_m19_gate("OMEGA_ACCEL_RESIDENT_ZERO_LIBCUDA_PASS", test_m19_gate15_zero_libcuda(), "Zero foreign userspace runtime verification (linkage, symbols, maps)");
    report_m19_gate("OMEGA_ACCEL_RESIDENT_CLEAN_CLONE_PASS", test_m19_gate16_clean_clone(), "Clean-clone isolated reproduction on DGX Spark silicon from scratch");
    {
        bool regression_ok = test_m19_gate17_regression();
        char detail[96];
        snprintf(detail, sizeof(detail), "Cumulative regression parity: %d / %d prior milestone gates passing", m19_regression_passed, m19_regression_total);
        report_m19_gate("OMEGA_ACCEL_RESIDENT_REGRESSION_PASS", regression_ok, detail);
    }
    report_m19_gate("OMEGA_ACCEL_RESIDENT_RECEIPT_PASS", test_m19_gate18_receipt(), "Receipt prerequisites complete; outer harness emits after all results");
    char observed_digest[65];
    for (int i = 0; i < 32; i++) sprintf(&observed_digest[i * 2], "%02x", m19_sustained_digest[i]);
    observed_digest[64] = '\0';
    printf("M19_OBSERVED_JSON:{\"m19_gates_completed\":%d,\"m19_gates_passed\":%d,"
           "\"regression_gates_completed\":%d,\"regression_gates_passed\":%d,"
           "\"sustained_dispatches\":%u,\"ring_capacity\":%u,"
           "\"wrap_dispatches\":%u,\"rolling_state_digest\":\"%s\","
           "\"channel_generation_advanced\":%s,\"vas_preserved\":%s}\n",
           m19_gate_count, m19_gate_passed, m19_regression_total,
           m19_regression_passed, m19_sustained_dispatches, m19_ring_capacity,
           m19_wrap_dispatches, observed_digest,
           m19_fault_channel_generation_advanced ? "true" : "false",
           m19_fault_vas_preserved ? "true" : "false");

    printf("================================================================================\n");
    printf("  STAGE 1 / PERSISTENT WORLD QUALIFICATION: %d / %d M19 GATES PASSED\n", m19_gate_passed, m19_gate_count);
    printf("  TOTAL GATES EVALUATED: %d (%d M19 Gates + %d Prior Regression Gates)\n", m19_gate_passed + m19_regression_total, m19_gate_count, m19_regression_total);
    printf("================================================================================\n");

    return (m19_gate_passed == m19_gate_count) ? 0 : 1;
}

void run_demonstration_accelerator_world(void) {
    printf("Executing OMEGA Accelerator World physical silicon demonstration...\n");
    run_m19_gates();
}

/* ===========================================================================
 * M19R world lifecycle gates: revoked memory is released, a job still in
 * flight keeps its memory until drain, faults latch without double commits,
 * and completion waits are monotonic ("reached or passed").
 * =========================================================================== */

static int lc_gate_count = 0;
static int lc_gate_passed = 0;

static void report_lc_gate(const char *name, bool ok) {
    lc_gate_count++;
    if (ok) lc_gate_passed++;
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
}

/* Code + three 256-byte buffers, as in the drain-decoupling test. */
static bool lc_setup(OmegaAcceleratorWorld *world, OmegaHandle *code,
                     OmegaHandle *a, OmegaHandle *b, OmegaHandle *c) {
    uint8_t code_buf[1024];
    size_t code_len = 0;
    if (omega_blackwell_encode_vecadd(code_buf, sizeof(code_buf), &code_len) != 0) return false;
    return omega_world_register_code(world, code_buf, code_len, NULL, code) == OMEGA_WORLD_OK &&
           omega_world_register_buffer(world, 256, OMEGA_PERM_READ, a) == OMEGA_WORLD_OK &&
           omega_world_register_buffer(world, 256, OMEGA_PERM_READ, b) == OMEGA_WORLD_OK &&
           omega_world_register_buffer(world, 256, OMEGA_PERM_READ | OMEGA_PERM_WRITE, c) == OMEGA_WORLD_OK;
}

/* Queue one release-only pushbuffer (no kernel) carrying `payload`. */
static int lc_submit_release(OmegaAcceleratorWorld *world, uint32_t slot, uint32_t payload,
                             const OmegaHandle *code, const OmegaHandle *a,
                             const OmegaHandle *b, const OmegaHandle *c) {
    uint32_t tpb[32];
    size_t len = 0;
    memcpy(&tpb[len], WORLD_SETUP_WORDS, sizeof(WORLD_SETUP_WORDS));
    len += sizeof(WORLD_SETUP_WORDS) / 4;
    tpb[len++] = nvrm_mthd(0, 0x005c, 5);
    tpb[len++] = (uint32_t)world->completion.gpu_va;
    tpb[len++] = (uint32_t)(world->completion.gpu_va >> 32);
    tpb[len++] = payload;
    tpb[len++] = 0;
    tpb[len++] = 0x1 | (1u << 20);
    const uint32_t words[5] = {7, 0, 0, 0, 0};
    OmegaWorldSubmission sub;
    m19_make_submission(&sub, words, code, a, b, c, 256, 256, 0, payload);
    uint32_t off = slot * 0x1000;
    memcpy((uint8_t *)world->m16.pb_mem.cpu + off, tpb, len * 4);
    __asm__ volatile("dsb sy" ::: "memory");
    return omega_world_submit(world, &world->m16.pb_mem, off, (uint32_t)len, &sub);
}

static bool lc_vecadd_ok(OmegaAcceleratorWorld *world, const OmegaHandle *code) {
    OmegaHandle a, b, c;
    if (omega_world_register_buffer(world, 4096, OMEGA_PERM_READ, &a) != OMEGA_WORLD_OK ||
        omega_world_register_buffer(world, 4096, OMEGA_PERM_READ, &b) != OMEGA_WORLD_OK ||
        omega_world_register_buffer(world, 4096, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &c) != OMEGA_WORLD_OK) {
        return false;
    }
    uint32_t *pa, *pb, *pc;
    uint64_t va;
    omega_world_resolve_buffer(world, &a, OMEGA_PERM_READ, 0, 256, (void **)&pa, &va);
    omega_world_resolve_buffer(world, &b, OMEGA_PERM_READ, 0, 256, (void **)&pb, &va);
    omega_world_resolve_buffer(world, &c, OMEGA_PERM_WRITE, 0, 256, (void **)&pc, &va);
    for (uint32_t i = 0; i < 64; i++) { pa[i] = i * 3u; pb[i] = 7u; pc[i] = 0xdeadbeefu; }
    uint32_t done = 0;
    if (omega_world_dispatch_vector(world, code, &a, &b, &c, 64, &done) != OMEGA_WORLD_OK) return false;
    for (uint32_t i = 0; i < 64; i++) if (pc[i] != i * 3u + 7u) return false;
    return omega_world_revoke_buffer(world, &a) == OMEGA_WORLD_OK &&
           omega_world_revoke_buffer(world, &b) == OMEGA_WORLD_OK &&
           omega_world_revoke_buffer(world, &c) == OMEGA_WORLD_OK;
}

/* 5,000 x 64 MiB register/revoke cycles = 312.5 GiB, more than twice the
 * machine's memory: only possible if revoke really returns memory. */
static bool test_lc_revoke_releases_memory(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;
    uint8_t code_buf[1024];
    size_t code_len = 0;
    OmegaHandle code;
    if (omega_blackwell_encode_vecadd(code_buf, sizeof(code_buf), &code_len) != 0 ||
        omega_world_register_code(&world, code_buf, code_len, NULL, &code) != OMEGA_WORLD_OK) {
        omega_world_destroy(&world);
        return false;
    }
    const size_t sz = 64u << 20;
    bool ok = true;
    for (int i = 0; i < 5000 && ok; i++) {
        OmegaHandle h;
        uint8_t *p = NULL;
        uint64_t va;
        ok = omega_world_register_buffer(&world, sz, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &h) == OMEGA_WORLD_OK &&
             omega_world_resolve_buffer(&world, &h, OMEGA_PERM_WRITE, 0, sz, (void **)&p, &va) == OMEGA_WORLD_OK;
        if (ok) {
            p[0] = (uint8_t)i;
            p[sz - 1] = (uint8_t)~i;
            ok = (p[0] == (uint8_t)i && p[sz - 1] == (uint8_t)~i) &&
                 omega_world_revoke_buffer(&world, &h) == OMEGA_WORLD_OK &&
                 !world.buffers[h.object_id].retiring && world.buffers[h.object_id].mem.handle == 0;
        }
        if (!ok) printf("    churn failed at cycle %d\n", i);
    }
    ok = ok && world.buffer_count == 0 && !world.faulted && lc_vecadd_ok(&world, &code);
    omega_world_destroy(&world);
    return ok;
}

/* Revoking a buffer bound by a queued job refuses the handle at once but
 * keeps its memory until drain; the job is abandoned, not committed. */
static bool test_lc_revoke_waits_for_queued_job(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;
    OmegaHandle code, a, b, c;
    bool ok = lc_setup(&world, &code, &a, &b, &c) &&
              lc_submit_release(&world, 0, 1, &code, &a, &b, &c) == OMEGA_WORLD_OK &&
              omega_world_revoke_buffer(&world, &c) == OMEGA_WORLD_OK;
    ok = ok && world.buffers[c.object_id].retiring && world.buffers[c.object_id].mem.handle != 0;
    void *p = NULL;
    uint64_t va;
    ok = ok && omega_world_resolve_buffer(&world, &c, OMEGA_PERM_READ, 0, 256, &p, &va) == OMEGA_WORLD_ERR_NOT_FOUND;
    OmegaHandle other;
    ok = ok && omega_world_register_buffer(&world, 256, OMEGA_PERM_READ, &other) == OMEGA_WORLD_OK &&
         other.object_id != c.object_id;
    ok = ok && omega_world_ring(&world) == OMEGA_WORLD_OK &&
         omega_world_drain(&world, 1, 5000) == 0 &&
         world.abandoned_dispatches == 1 && world.total_dispatches == 0 &&
         !world.buffers[c.object_id].retiring && world.buffers[c.object_id].mem.handle == 0 &&
         !world.faulted;
    /* The released slot is reusable. */
    OmegaHandle reuse;
    ok = ok && omega_world_revoke_buffer(&world, &other) == OMEGA_WORLD_OK;
    for (int i = 0; ok && i < OMEGA_WORLD_MAX_BUFFERS; i++) {
        if (omega_world_register_buffer(&world, 256, OMEGA_PERM_READ, &reuse) != OMEGA_WORLD_OK) { ok = false; break; }
        if (reuse.object_id == c.object_id) break;
    }
    ok = ok && reuse.object_id == c.object_id && reuse.object_generation == c.object_generation + 1;
    omega_world_destroy(&world);
    return ok;
}

/* A drain failure latches the world; nothing already committed is committed
 * again, and recovery clears the latch. */
static bool test_lc_error_latch_counts_once(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;
    OmegaHandle code, a, b, c;
    bool ok = lc_setup(&world, &code, &a, &b, &c);
    for (uint32_t i = 0; ok && i < 3; i++) ok = lc_submit_release(&world, i, i + 1, &code, &a, &b, &c) == OMEGA_WORLD_OK;
    ok = ok && omega_world_ring(&world) == OMEGA_WORLD_OK &&
         omega_world_drain(&world, 3, 5000) == 3 && world.total_dispatches == 3;
    uint64_t seq_before = world.sequence_number;
    /* Payload 4 completes, but the drain waits for payload 100: timeout. */
    ok = ok && lc_submit_release(&world, 3, 4, &code, &a, &b, &c) == OMEGA_WORLD_OK &&
         omega_world_ring(&world) == OMEGA_WORLD_OK &&
         omega_world_drain(&world, 100, 200) == OMEGA_WORLD_ERR_HARDWARE && world.faulted;
    ok = ok && lc_submit_release(&world, 4, 5, &code, &a, &b, &c) == OMEGA_WORLD_ERR_FAULTED &&
         omega_world_drain(&world, 4, 100) == OMEGA_WORLD_ERR_FAULTED &&
         omega_world_ring(&world) == OMEGA_WORLD_ERR_FAULTED;
    ok = ok && world.total_dispatches == 3 && world.sequence_number == seq_before;
    ok = ok && omega_world_recover_channel_fault(&world) == OMEGA_WORLD_OK && !world.faulted &&
         world.in_flight_count == 0 && world.abandoned_dispatches == 1 &&
         omega_world_drain(&world, 4, 100) == 0 && world.total_dispatches == 3;
    ok = ok && lc_vecadd_ok(&world, &code) && world.total_dispatches == 4;
    omega_world_destroy(&world);
    return ok;
}

/* Draining an older payload succeeds after newer ones finished, payloads
 * must strictly increase, and the marker never moves backwards. */
static bool test_lc_wait_reached_or_passed(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;
    OmegaHandle code, a, b, c;
    bool ok = lc_setup(&world, &code, &a, &b, &c);
    for (uint32_t i = 0; ok && i < 3; i++) ok = lc_submit_release(&world, i, i + 1, &code, &a, &b, &c) == OMEGA_WORLD_OK;
    ok = ok && omega_world_ring(&world) == OMEGA_WORLD_OK &&
         m16_native_wait_marker_ge(world.completion.cpu_marker, 3, 5000) == 0;
    /* Marker is already 3: an equality wait on 1 would time out. */
    ok = ok && omega_world_drain(&world, 1, 1000) == 1 && omega_world_drain(&world, 3, 1000) == 2 &&
         world.total_dispatches == 3;
    /* A payload that does not advance the sequence is refused. */
    ok = ok && lc_submit_release(&world, 3, 2, &code, &a, &b, &c) == OMEGA_WORLD_ERR_INVALID_ARG &&
         world.in_flight_count == 0;
    uint32_t m0 = *world.completion.cpu_marker;
    ok = ok && lc_vecadd_ok(&world, &code);
    uint32_t m1 = *world.completion.cpu_marker;
    ok = ok && (int32_t)(m1 - m0) > 0 && m1 == world.completion.last_payload;
    omega_world_destroy(&world);
    return ok;
}

static bool test_lc_output_alias_refusal(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;
    OmegaHandle code, a, b, c;
    bool ok = lc_setup(&world, &code, &a, &b, &c);
    uint32_t words[32], len = 0;
    memcpy(words, WORLD_SETUP_WORDS, sizeof WORLD_SETUP_WORDS);
    len += (uint32_t)(sizeof WORLD_SETUP_WORDS / sizeof(uint32_t));
    words[len++] = nvrm_mthd(0, 0x005c, 5);
    words[len++] = (uint32_t)world.completion.gpu_va;
    words[len++] = (uint32_t)(world.completion.gpu_va >> 32);
    words[len++] = 1;
    words[len++] = 0;
    words[len++] = 0x1 | (1u << 20);
    memcpy(world.m16.pb_mem.cpu, words, len * sizeof(uint32_t));
    OmegaWorldSubmission sub;
    const uint32_t semantic[5] = {7, 0, 0, 0, 0};
    m19_make_submission(&sub, semantic, &code, &a, &b, &c, 256, 256, 256, 1);
    ok = ok && omega_world_submit(&world, &world.m16.pb_mem, 0, len, &sub) == OMEGA_WORLD_OK;
    void *cpu = NULL;
    ok = ok && omega_world_resolve_buffer(&world, &c, OMEGA_PERM_WRITE,
                 0, 1, &cpu, NULL) == OMEGA_WORLD_ERR_FAULT;
    sub.completion_val = 2;
    ok = ok && omega_world_submit(&world, &world.m16.pb_mem, 0, len, &sub) == OMEGA_WORLD_ERR_FAULT;
    ok = ok && omega_world_ring(&world) == OMEGA_WORLD_OK &&
         omega_world_drain(&world, 1, 5000) == 1 && world.total_dispatches == 1 &&
         omega_world_drain(&world, 1, 0) == 0 && world.total_dispatches == 1;
    ok = ok && omega_world_resolve_buffer(&world, &c, OMEGA_PERM_WRITE,
                 0, 1, &cpu, NULL) == OMEGA_WORLD_OK;
    omega_world_destroy(&world);
    return ok;
}

static bool test_lc_bounds_overflow(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;
    OmegaHandle h;
    bool ok = omega_world_register_buffer(&world, 256, OMEGA_PERM_READ, &h) == OMEGA_WORLD_OK &&
              omega_world_resolve_buffer(&world, &h, OMEGA_PERM_READ,
                  SIZE_MAX - 4, 16, NULL, NULL) == OMEGA_WORLD_ERR_BOUNDS &&
              omega_world_register_buffer(&world, SIZE_MAX, OMEGA_PERM_READ, &h) == OMEGA_WORLD_ERR_BOUNDS &&
              omega_world_revoke_buffer(&world, &h) == OMEGA_WORLD_OK;
    omega_world_destroy(&world);
    return ok;
}

static bool test_lc_timeout_completion_race(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;
    OmegaHandle code, a, b, c;
    bool ok = lc_setup(&world, &code, &a, &b, &c) &&
              lc_submit_release(&world, 0, 1, &code, &a, &b, &c) == OMEGA_WORLD_OK &&
              omega_world_ring(&world) == OMEGA_WORLD_OK;
    int drained = ok ? omega_world_drain(&world, 1, 0) : OMEGA_WORLD_ERR_FAULT;
    if (drained == 1) {
        ok = ok && world.total_dispatches == 1 &&
             omega_world_drain(&world, 1, 0) == 0 && world.total_dispatches == 1;
    } else {
        ok = ok && drained == OMEGA_WORLD_ERR_HARDWARE && world.faulted &&
             world.total_dispatches == 0 &&
             m16_native_wait_marker_ge(world.completion.cpu_marker, 1, 5000) == 0 &&
             omega_world_recover_channel_fault(&world) == OMEGA_WORLD_OK &&
             world.abandoned_dispatches == 1 && world.total_dispatches == 0;
    }
    ok = ok && lc_vecadd_ok(&world, &code);
    omega_world_destroy(&world);
    return ok;
}

static bool test_lc_stale_generation_marker(void) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;
    OmegaHandle code, a, b, c;
    bool ok = lc_setup(&world, &code, &a, &b, &c) &&
              lc_submit_release(&world, 0, 1, &code, &a, &b, &c) == OMEGA_WORLD_OK &&
              omega_world_ring(&world) == OMEGA_WORLD_OK &&
              omega_world_drain(&world, 1, 5000) == 1 &&
              omega_world_recover_channel_fault(&world) == OMEGA_WORLD_OK &&
              *world.completion.cpu_marker == 1;
    /* The old marker value must not commit a new generation's queued job. */
    ok = ok && lc_submit_release(&world, 0, 2, &code, &a, &b, &c) == OMEGA_WORLD_OK &&
         omega_world_drain(&world, 2, 0) == OMEGA_WORLD_ERR_HARDWARE &&
         world.total_dispatches == 1 && world.faulted &&
         omega_world_recover_channel_fault(&world) == OMEGA_WORLD_OK &&
         world.abandoned_dispatches == 1 && lc_vecadd_ok(&world, &code);
    omega_world_destroy(&world);
    return ok;
}

static bool lc_provenance_run(uint8_t mutation, uint8_t digest[32]) {
    OmegaAcceleratorWorld world;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;
    OmegaHandle code, a, b, c1, c2;
    bool ok = lc_setup(&world, &code, &a, &b, &c1) &&
              omega_world_register_buffer(&world, 256,
                  OMEGA_PERM_READ | OMEGA_PERM_WRITE, &c2) == OMEGA_WORLD_OK;
    void *p1 = NULL, *p2 = NULL;
    ok = ok && omega_world_resolve_buffer(&world, &c1, OMEGA_PERM_WRITE,
                  0, 256, &p1, NULL) == OMEGA_WORLD_OK &&
              omega_world_resolve_buffer(&world, &c2, OMEGA_PERM_WRITE,
                  0, 256, &p2, NULL) == OMEGA_WORLD_OK;
    if (ok) { memset(p1, 0x11, 256); memset(p2, 0x22, 256); }
    const uint32_t semantic[5] = {7, 0, 0, 0, 0};
    for (uint32_t i = 0; ok && i < 2; i++) {
        uint32_t words[32], len = 0;
        memcpy(words, WORLD_SETUP_WORDS, sizeof WORLD_SETUP_WORDS);
        len += (uint32_t)(sizeof WORLD_SETUP_WORDS / sizeof(uint32_t));
        words[len++] = nvrm_mthd(0, 0x005c, 5);
        words[len++] = (uint32_t)world.completion.gpu_va;
        words[len++] = (uint32_t)(world.completion.gpu_va >> 32);
        words[len++] = i + 1;
        words[len++] = 0;
        words[len++] = 0x1 | (1u << 20);
        uint32_t off = i * 0x1000;
        memcpy((uint8_t *)world.m16.pb_mem.cpu + off, words, len * sizeof(uint32_t));
        OmegaWorldSubmission sub;
        m19_make_submission(&sub, semantic, &code, &a, &b, i ? &c2 : &c1,
                            256, 256, 256, i + 1);
        ok = omega_world_submit(&world, &world.m16.pb_mem, off, len, &sub) == OMEGA_WORLD_OK;
    }
    ok = ok && omega_world_ring(&world) == OMEGA_WORLD_OK &&
         omega_world_drain(&world, 1, 5000) == 1;
    if (ok) memset(p1, mutation, 256);
    ok = ok && omega_world_drain(&world, 2, 5000) == 1 && world.total_dispatches == 2;
    if (ok) memcpy(digest, world.rolling_state_digest, 32);
    omega_world_destroy(&world);
    return ok;
}

static bool test_lc_batched_result_identity(void) {
    uint8_t a[32], b[32];
    return lc_provenance_run(0x33, a) && lc_provenance_run(0x44, b) &&
           memcmp(a, b, sizeof a) == 0;
}

static bool test_lc_stale_handle_after_world_restart(void) {
    OmegaAcceleratorWorld world;
    OmegaHandle old, fresh;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;
    bool ok = omega_world_register_buffer(&world, 4096, OMEGA_PERM_READ, &old) == OMEGA_WORLD_OK;
    if (omega_world_destroy(&world) != OMEGA_WORLD_OK) return false;
    if (omega_world_init(&world) != OMEGA_WORLD_OK) return false;
    ok = ok && omega_world_register_buffer(&world, 4096, OMEGA_PERM_READ, &fresh) == OMEGA_WORLD_OK;
    ok = ok && old.object_id == fresh.object_id && old.object_generation == fresh.object_generation &&
         old.world_epoch != fresh.world_epoch &&
         omega_world_validate_handle(&world, &old, OMEGA_OBJ_BUFFER, OMEGA_PERM_READ) == OMEGA_WORLD_ERR_STALE_EPOCH;
    return omega_world_destroy(&world) == OMEGA_WORLD_OK && ok;
}

int run_world_lifecycle_gates(void) {
    printf("================================================================================\n");
    printf("    AIEN OMEGA M19R: WORLD LIFECYCLE GATES\n");
    printf("================================================================================\n");
    lc_gate_count = 0;
    lc_gate_passed = 0;
    report_lc_gate("WORLD_REVOKE_RELEASES_MEMORY", test_lc_revoke_releases_memory());
    report_lc_gate("WORLD_REVOKE_WAITS_FOR_QUEUED_JOB", test_lc_revoke_waits_for_queued_job());
    report_lc_gate("WORLD_ERROR_LATCH_COUNTS_ONCE", test_lc_error_latch_counts_once());
    report_lc_gate("WORLD_WAIT_REACHED_OR_PASSED", test_lc_wait_reached_or_passed());
    report_lc_gate("WORLD_OUTPUT_IN_FLIGHT_ALIAS_REFUSED", test_lc_output_alias_refusal());
    report_lc_gate("WORLD_BOUNDS_OVERFLOW_REFUSED", test_lc_bounds_overflow());
    report_lc_gate("WORLD_TIMEOUT_COMPLETION_RACE", test_lc_timeout_completion_race());
    report_lc_gate("WORLD_STALE_GENERATION_MARKER_REFUSED", test_lc_stale_generation_marker());
    report_lc_gate("WORLD_BATCHED_RESULT_IDENTITY", test_lc_batched_result_identity());
    report_lc_gate("WORLD_STALE_HANDLE_AFTER_RESTART", test_lc_stale_handle_after_world_restart());
    printf("  TOTAL: %d | PASSED: %d | FAILED: %d\n", lc_gate_count, lc_gate_passed, lc_gate_count - lc_gate_passed);
    return (lc_gate_passed == lc_gate_count) ? 0 : 1;
}

typedef struct {
    uint64_t rm_balance, va_high_water, free_bytes, map_bytes;
    uint64_t vm_pages, rss_pages;
    uint32_t live_allocations, free_ranges, map_count, buffers, code;
} M19RSoakSample;

static M19RSoakSample m19r_sample(const OmegaAcceleratorWorld *world) {
    M19RSoakSample s = {0};
    const Nvrm *rm = &world->m16.rm;
    s.rm_balance = rm->rm_alloc_accepted - rm->rm_free_accepted;
    s.va_high_water = rm->va_next;
    s.live_allocations = rm->live_count;
    s.free_ranges = rm->free_count;
    s.buffers = world->buffer_count;
    s.code = world->code_count;
    for (uint32_t i = 0; i < rm->free_count; i++) s.free_bytes += rm->free_list[i].size;
    FILE *f = fopen("/proc/self/maps", "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            unsigned long lo, hi;
            if (strstr(line, "nvidia") && sscanf(line, "%lx-%lx", &lo, &hi) == 2 && hi >= lo) {
                s.map_count++;
                s.map_bytes += hi - lo;
            }
        }
        fclose(f);
    }
    f = fopen("/proc/self/statm", "r");
    if (f) {
        unsigned long long vm = 0, rss = 0;
        if (fscanf(f, "%llu %llu", &vm, &rss) == 2) {
            s.vm_pages = vm;
            s.rss_pages = rss;
        }
        fclose(f);
    }
    return s;
}

static void m19r_print_sample(const char *name, const M19RSoakSample *s) {
    printf("M19R_RESOURCE_JSON:{\"phase\":\"%s\",\"driver_handle_count_available\":false,"
           "\"driver_acknowledged_rm_balance\":%llu,\"internal_live_allocations\":%u,"
           "\"va_high_water\":%llu,\"free_ranges\":%u,\"free_bytes\":%llu,"
           "\"nvidia_process_mapping_count\":%u,\"nvidia_process_mapping_bytes\":%llu,"
           "\"process_virtual_pages\":%llu,\"process_resident_pages\":%llu,"
           "\"world_buffers\":%u,\"world_code\":%u}\n",
           name, (unsigned long long)s->rm_balance, s->live_allocations,
           (unsigned long long)s->va_high_water, s->free_ranges,
           (unsigned long long)s->free_bytes, s->map_count,
           (unsigned long long)s->map_bytes, (unsigned long long)s->vm_pages,
           (unsigned long long)s->rss_pages, s->buffers, s->code);
    fflush(stdout);
}

static bool m19r_soak_code(OmegaAcceleratorWorld *world, OmegaHandle *code) {
    uint8_t bytes[1024];
    size_t size = 0;
    return omega_blackwell_encode_vecadd(bytes, sizeof bytes, &size) == 0 &&
           omega_world_register_code(world, bytes, size, NULL, code) == OMEGA_WORLD_OK;
}

int run_m19r_soak(void) {
    const uint64_t cycles_target = 100000;
    const size_t allocation_bytes = 4u << 20;
    OmegaAcceleratorWorld world;
    OmegaHandle code, buffer;
    bool ok = omega_world_init(&world) == OMEGA_WORLD_OK && m19r_soak_code(&world, &code);
    if (!ok) { omega_world_destroy(&world); return 1; }
    /* Warm the allocator to the peak allocation size before measuring VA. */
    ok = omega_world_register_buffer(&world, allocation_bytes, OMEGA_PERM_READ | OMEGA_PERM_WRITE, &buffer) == OMEGA_WORLD_OK &&
         omega_world_revoke_buffer(&world, &buffer) == OMEGA_WORLD_OK;
    if (!ok) { omega_world_destroy(&world); return 1; }
    M19RSoakSample start = m19r_sample(&world);
    m19r_print_sample("start", &start);
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    uint64_t cycles = 0, churn_bytes = 0, recoveries = 0, rebuilds = 0;
    for (; cycles < cycles_target && ok; cycles++) {
        void *cpu = NULL;
        uint64_t gpu_va = 0;
        ok = omega_world_register_buffer(&world, allocation_bytes,
                 OMEGA_PERM_READ | OMEGA_PERM_WRITE, &buffer) == OMEGA_WORLD_OK &&
             omega_world_resolve_buffer(&world, &buffer, OMEGA_PERM_WRITE,
                 0, allocation_bytes, &cpu, &gpu_va) == OMEGA_WORLD_OK;
        if (!ok) break;
        volatile uint8_t *p = cpu;
        p[0] = (uint8_t)cycles;
        p[allocation_bytes - 1] = (uint8_t)(cycles >> 8);
        ok = p[0] == (uint8_t)cycles && p[allocation_bytes - 1] == (uint8_t)(cycles >> 8) &&
             gpu_va != 0 && omega_world_revoke_buffer(&world, &buffer) == OMEGA_WORLD_OK &&
             omega_world_resolve_buffer(&world, &buffer, OMEGA_PERM_READ,
                 0, 1, NULL, NULL) != OMEGA_WORLD_OK;
        if (!ok) break;
        churn_bytes += allocation_bytes;
        if ((cycles + 1) % 10000 == 0) {
            ok = lc_vecadd_ok(&world, &code) &&
                 omega_world_recover_channel_fault(&world) == OMEGA_WORLD_OK &&
                 lc_vecadd_ok(&world, &code);
            recoveries++;
        }
        if ((cycles + 1) % 25000 == 0)
            ok = ok && test_lc_revoke_waits_for_queued_job() &&
                 test_lc_error_latch_counts_once();
        if ((cycles + 1) % 20000 == 0 && cycles + 1 < cycles_target) {
            omega_world_destroy(&world);
            ok = ok && omega_world_init(&world) == OMEGA_WORLD_OK &&
                 m19r_soak_code(&world, &code);
            rebuilds++;
            if (ok) {
                ok = omega_world_register_buffer(&world, allocation_bytes,
                     OMEGA_PERM_READ | OMEGA_PERM_WRITE, &buffer) == OMEGA_WORLD_OK &&
                     omega_world_revoke_buffer(&world, &buffer) == OMEGA_WORLD_OK;
            }
        }
        if ((cycles + 1) % 10000 == 0) {
            fprintf(stderr, "M19R soak: %llu cycles, %llu bytes\n",
                    (unsigned long long)(cycles + 1), (unsigned long long)churn_bytes);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    M19RSoakSample end = m19r_sample(&world);
    m19r_print_sample("end", &end);
    double duration = (double)(ts_end.tv_sec - ts_start.tv_sec) +
                      (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
    FILE *mf = fopen("/proc/meminfo", "r");
    unsigned long long memory_kb = 0;
    if (mf) {
        char line[256];
        while (fgets(line, sizeof line, mf))
            if (sscanf(line, "MemTotal: %llu kB", &memory_kb) == 1) break;
        fclose(mf);
    }
    bool stable = ok && cycles == cycles_target && memory_kb &&
        churn_bytes > 2ull * memory_kb * 1024ull &&
        start.rm_balance == end.rm_balance &&
        start.live_allocations == end.live_allocations &&
        start.va_high_water == end.va_high_water &&
        start.map_count == end.map_count && start.map_bytes == end.map_bytes &&
        start.buffers == end.buffers && start.code == end.code && !world.faulted;
    omega_world_destroy(&world);
    M19RSoakSample post = m19r_sample(&world);
    m19r_print_sample("post_destroy", &post);
    stable = stable && post.rm_balance == 0 && post.live_allocations == 0 &&
             post.map_count == 0 && post.buffers == 0 && post.code == 0;
    printf("M19R_SOAK_JSON:{\"passed\":%s,\"duration_seconds\":%.3f,"
           "\"cycles\":%llu,\"bytes_churned\":%llu,\"physical_memory_bytes\":%llu,"
           "\"channel_recoveries\":%llu,\"world_rebuilds\":%llu}\n",
           stable ? "true" : "false", duration, (unsigned long long)cycles,
           (unsigned long long)churn_bytes, memory_kb * 1024ull,
           (unsigned long long)recoveries, (unsigned long long)rebuilds);
    return stable ? 0 : 1;
}
