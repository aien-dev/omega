#include "omega_machine.h"
#include "omega_core.h"
#include "sha256.h"
#include "aarch64_decoder.h"
#include <string.h>
#include <stdio.h>

void omega_machine_init(OmegaMachineGraph *mg, const char *name, uint8_t target_profile) {
    if (!mg) return;
    memset(mg, 0, sizeof(*mg));
    if (name) {
        snprintf(mg->name, sizeof(mg->name), "%s", name);
    }
    mg->target_profile = target_profile;
}

int omega_machine_compute_id(OmegaMachineGraph *mg) {
    if (!mg) return -1;

    uint8_t buffer[4096];
    size_t pos = 0;

    /* Canonical OMG0 wire header */
    buffer[pos++] = 'O';
    buffer[pos++] = 'M';
    buffer[pos++] = 'G';
    buffer[pos++] = '0';
    buffer[pos++] = KIND_MACHINE; /* 0x07 */
    buffer[pos++] = mg->target_profile;

    /* Machine Name */
    size_t name_len = strlen(mg->name);
    buffer[pos++] = (uint8_t)name_len;
    memcpy(&buffer[pos], mg->name, name_len);
    pos += name_len;

    /* Pipeline serialization */
    uint32_t iw = mg->pipeline.issue_width;
    buffer[pos++] = (uint8_t)((iw >> 24) & 0xFF);
    buffer[pos++] = (uint8_t)((iw >> 16) & 0xFF);
    buffer[pos++] = (uint8_t)((iw >> 8) & 0xFF);
    buffer[pos++] = (uint8_t)(iw & 0xFF);

    uint32_t mif = mg->pipeline.max_in_flight;
    buffer[pos++] = (uint8_t)((mif >> 24) & 0xFF);
    buffer[pos++] = (uint8_t)((mif >> 16) & 0xFF);
    buffer[pos++] = (uint8_t)((mif >> 8) & 0xFF);
    buffer[pos++] = (uint8_t)(mif & 0xFF);

    buffer[pos++] = mg->pipeline.out_of_order ? 1 : 0;
    buffer[pos++] = (uint8_t)mg->pipeline.unit_count;

    for (size_t u = 0; u < mg->pipeline.unit_count && u < OMEGA_MACHINE_MAX_UNITS; ++u) {
        const MachineComputeUnit *mcu = &mg->pipeline.units[u];
        buffer[pos++] = (uint8_t)mcu->type;
        buffer[pos++] = (uint8_t)mcu->count;
        buffer[pos++] = (uint8_t)mcu->latency_cycles;
        buffer[pos++] = (uint8_t)mcu->throughput_per_cycle;
    }

    /* Register File serialization */
    uint32_t gpr_c = mg->registers.gpr_count;
    uint32_t gpr_w = mg->registers.gpr_width_bits;
    uint32_t vec_c = mg->registers.vector_count;
    uint32_t vec_w = mg->registers.vector_width_bits;

    buffer[pos++] = (uint8_t)gpr_c;
    buffer[pos++] = (uint8_t)gpr_w;
    buffer[pos++] = (uint8_t)vec_c;
    buffer[pos++] = (uint8_t)vec_w;

    /* Cache hierarchy serialization */
    buffer[pos++] = (uint8_t)mg->cache_count;
    for (size_t c = 0; c < mg->cache_count && c < OMEGA_MACHINE_MAX_CACHES; ++c) {
        const MachineCacheLevel *mcl = &mg->caches[c];
        buffer[pos++] = (uint8_t)mcl->level;
        buffer[pos++] = mcl->is_instruction ? 1 : 0;
        /* Size (8 bytes) */
        for (int b = 7; b >= 0; --b) {
            buffer[pos++] = (uint8_t)((mcl->size_bytes >> (b * 8)) & 0xFF);
        }
        buffer[pos++] = (uint8_t)mcl->line_size_bytes;
        buffer[pos++] = (uint8_t)mcl->associativity;
        buffer[pos++] = (uint8_t)mcl->latency_cycles;
    }

    /* DRAM layout serialization */
    for (int b = 7; b >= 0; --b) {
        buffer[pos++] = (uint8_t)((mg->dram_base >> (b * 8)) & 0xFF);
    }
    for (int b = 7; b >= 0; --b) {
        buffer[pos++] = (uint8_t)((mg->dram_size >> (b * 8)) & 0xFF);
    }

    /* Physics Authority Seal */
    buffer[pos++] = mg->is_physics_authorized ? 1 : 0;
    memcpy(&buffer[pos], mg->physics_receipt_seal, 32);
    pos += 32;

    sha256_hash(buffer, pos, mg->machine_id.bytes);
    return 0;
}

int omega_machine_build_dgx_spark(OmegaMachineGraph *mg) {
    if (!mg) return -1;
    omega_machine_init(mg, "NVIDIA_DGX_SPARK_GRACE_V2", AARCH64_PROFILE_V8A_BAREMETAL);

    /* Neoverse V2: 4-wide dispatch pipeline */
    mg->pipeline.issue_width = 4;
    mg->pipeline.max_in_flight = 256;
    mg->pipeline.out_of_order = true;
    mg->pipeline.unit_count = 6;

    mg->pipeline.units[0] = (MachineComputeUnit){ UNIT_ALU, 4, 1, 4 };
    mg->pipeline.units[1] = (MachineComputeUnit){ UNIT_BRANCH, 2, 1, 2 };
    mg->pipeline.units[2] = (MachineComputeUnit){ UNIT_MULTIPLIER, 2, 3, 2 };
    mg->pipeline.units[3] = (MachineComputeUnit){ UNIT_DIVIDER, 1, 12, 1 };
    mg->pipeline.units[4] = (MachineComputeUnit){ UNIT_LOAD_STORE, 2, 3, 2 };
    mg->pipeline.units[5] = (MachineComputeUnit){ UNIT_VECTOR, 4, 2, 4 };

    /* Register files */
    mg->registers.gpr_count = 31;
    mg->registers.gpr_width_bits = 64;
    mg->registers.vector_count = 32;
    mg->registers.vector_width_bits = 128;

    /* Cache hierarchy */
    mg->cache_count = 4;
    mg->caches[0] = (MachineCacheLevel){ 1, true,  64ULL * 1024, 64, 4, 3 };       /* L1I: 64KB */
    mg->caches[1] = (MachineCacheLevel){ 1, false, 64ULL * 1024, 64, 8, 4 };       /* L1D: 64KB */
    mg->caches[2] = (MachineCacheLevel){ 2, false, 1024ULL * 1024, 64, 8, 10 };    /* L2:  1MB */
    mg->caches[3] = (MachineCacheLevel){ 3, false, 114ULL * 1024 * 1024, 64, 16, 35 }; /* L3: 114MB */

    /* Coherent DRAM */
    mg->dram_base = 0x80000000ULL;
    mg->dram_size = 128ULL * 1024 * 1024 * 1024; /* 128GB LPDDR5X */

    mg->is_physics_authorized = true;
    memset(mg->physics_receipt_seal, 0xA1, 32);

    return omega_machine_compute_id(mg);
}

int omega_machine_build_qemu_virt(OmegaMachineGraph *mg) {
    if (!mg) return -1;
    omega_machine_init(mg, "QEMU_VIRT_AARCH64_GENERIC", AARCH64_PROFILE_V8A_BAREMETAL);

    /* Generic QEMU: 2-wide dispatch pipeline */
    mg->pipeline.issue_width = 2;
    mg->pipeline.max_in_flight = 64;
    mg->pipeline.out_of_order = false;
    mg->pipeline.unit_count = 5;

    mg->pipeline.units[0] = (MachineComputeUnit){ UNIT_ALU, 2, 1, 2 };
    mg->pipeline.units[1] = (MachineComputeUnit){ UNIT_BRANCH, 1, 1, 1 };
    mg->pipeline.units[2] = (MachineComputeUnit){ UNIT_MULTIPLIER, 1, 4, 1 };
    mg->pipeline.units[3] = (MachineComputeUnit){ UNIT_LOAD_STORE, 1, 4, 1 };
    mg->pipeline.units[4] = (MachineComputeUnit){ UNIT_VECTOR, 2, 3, 2 };

    /* Register files */
    mg->registers.gpr_count = 31;
    mg->registers.gpr_width_bits = 64;
    mg->registers.vector_count = 32;
    mg->registers.vector_width_bits = 128;

    /* Cache hierarchy */
    mg->cache_count = 3;
    mg->caches[0] = (MachineCacheLevel){ 1, true,  32ULL * 1024, 64, 2, 2 };    /* L1I: 32KB */
    mg->caches[1] = (MachineCacheLevel){ 1, false, 32ULL * 1024, 64, 4, 3 };    /* L1D: 32KB */
    mg->caches[2] = (MachineCacheLevel){ 2, false, 512ULL * 1024, 64, 8, 12 };  /* L2:  512KB */

    /* DRAM */
    mg->dram_base = 0x40000000ULL;
    mg->dram_size = 1ULL * 1024 * 1024 * 1024; /* 1GB */

    mg->is_physics_authorized = true;
    memset(mg->physics_receipt_seal, 0xB2, 32);

    return omega_machine_compute_id(mg);
}

int omega_machine_ingest_physics_descriptor(OmegaMachineGraph *mg, const PhysicsDescriptor *desc) {
    if (!mg || !desc) return -1;
    if (desc->magic != 0x4D414348) return -1; /* 'MACH' */
    if (desc->issue_width == 0 || desc->gpr_count == 0) return -1;

    omega_machine_init(mg, desc->cpu_name, (uint8_t)desc->target_profile);

    mg->pipeline.issue_width = desc->issue_width;
    mg->pipeline.max_in_flight = desc->issue_width * 32;
    mg->pipeline.out_of_order = (desc->issue_width >= 4);
    mg->pipeline.unit_count = 4;
    mg->pipeline.units[0] = (MachineComputeUnit){ UNIT_ALU, desc->issue_width, 1, desc->issue_width };
    mg->pipeline.units[1] = (MachineComputeUnit){ UNIT_BRANCH, 1, 1, 1 };
    mg->pipeline.units[2] = (MachineComputeUnit){ UNIT_MULTIPLIER, 1, 3, 1 };
    mg->pipeline.units[3] = (MachineComputeUnit){ UNIT_LOAD_STORE, 1, 3, 1 };

    mg->registers.gpr_count = desc->gpr_count;
    mg->registers.gpr_width_bits = 64;
    mg->registers.vector_count = desc->vec_count;
    mg->registers.vector_width_bits = 128;

    mg->cache_count = 0;
    if (desc->l1d_size > 0 && mg->cache_count < OMEGA_MACHINE_MAX_CACHES) {
        mg->caches[mg->cache_count++] = (MachineCacheLevel){ 1, false, desc->l1d_size, 64, 4, 3 };
    }
    if (desc->l2_size > 0 && mg->cache_count < OMEGA_MACHINE_MAX_CACHES) {
        mg->caches[mg->cache_count++] = (MachineCacheLevel){ 2, false, desc->l2_size, 64, 8, 10 };
    }
    if (desc->l3_size > 0 && mg->cache_count < OMEGA_MACHINE_MAX_CACHES) {
        mg->caches[mg->cache_count++] = (MachineCacheLevel){ 3, false, desc->l3_size, 64, 16, 30 };
    }

    mg->dram_base = desc->dram_base;
    mg->dram_size = desc->dram_size;

    mg->is_physics_authorized = true;
    memcpy(mg->physics_receipt_seal, desc->physics_seal, 32);

    return omega_machine_compute_id(mg);
}

int omega_machine_validate_topology(const OmegaMachineGraph *mg, char *err_msg, size_t err_msg_len) {
    if (!mg) return -1;

    if (mg->pipeline.issue_width == 0) {
        if (err_msg) snprintf(err_msg, err_msg_len, "Pipeline issue width is 0");
        return -1;
    }
    if (mg->registers.gpr_count != 31) {
        if (err_msg) snprintf(err_msg, err_msg_len, "Invalid GPR count: %u (expected 31)", mg->registers.gpr_count);
        return -1;
    }
    if (mg->registers.gpr_width_bits != 64) {
        if (err_msg) snprintf(err_msg, err_msg_len, "Invalid GPR width: %u (expected 64)", mg->registers.gpr_width_bits);
        return -1;
    }

    /* Verify cache hierarchy monotonicity */
    uint32_t prev_level = 0;
    for (size_t c = 0; c < mg->cache_count; ++c) {
        if (mg->caches[c].level < prev_level) {
            if (err_msg) snprintf(err_msg, err_msg_len, "Cache hierarchy level inversion: L%u after L%u",
                                  mg->caches[c].level, prev_level);
            return -1;
        }
        if (mg->caches[c].line_size_bytes == 0 || (mg->caches[c].line_size_bytes & (mg->caches[c].line_size_bytes - 1)) != 0) {
            if (err_msg) snprintf(err_msg, err_msg_len, "Invalid cache line size (non-power-of-two): %u",
                                  mg->caches[c].line_size_bytes);
            return -1;
        }
        prev_level = mg->caches[c].level;
    }

    return 0;
}

uint32_t omega_machine_estimate_latency(const OmegaMachineGraph *mg, const RealizationObject *real) {
    if (!mg || !real || real->code_len == 0) return 0;

    size_t insn_count = real->code_len / 4;
    uint32_t total_cycles = 0;

    uint32_t mul_latency = 3;
    uint32_t alu_latency = 1;
    for (size_t u = 0; u < mg->pipeline.unit_count; ++u) {
        if (mg->pipeline.units[u].type == UNIT_MULTIPLIER) {
            mul_latency = mg->pipeline.units[u].latency_cycles;
        }
        if (mg->pipeline.units[u].type == UNIT_ALU) {
            alu_latency = mg->pipeline.units[u].latency_cycles;
        }
    }

    for (size_t i = 0; i < insn_count; ++i) {
        uint32_t word = ((uint32_t)real->code_bytes[i * 4 + 0]) |
                        (((uint32_t)real->code_bytes[i * 4 + 1]) << 8) |
                        (((uint32_t)real->code_bytes[i * 4 + 2]) << 16) |
                        (((uint32_t)real->code_bytes[i * 4 + 3]) << 24);

        DecodedInsn dec;
        if (aarch64_decode_instruction(word, &dec) == 0) {
            if (dec.op == DECODED_MUL) {
                total_cycles += mul_latency;
            } else {
                total_cycles += alu_latency;
            }
        } else {
            total_cycles += alu_latency;
        }
    }

    /* Pipeline issue folding */
    if (mg->pipeline.issue_width > 1 && insn_count > 1) {
        uint32_t folded = (uint32_t)((insn_count + mg->pipeline.issue_width - 1) / mg->pipeline.issue_width);
        if (folded > total_cycles) total_cycles = folded;
    }

    return total_cycles;
}
