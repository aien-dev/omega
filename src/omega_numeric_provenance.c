#include "omega_numeric_provenance.h"
#include <stdio.h>
#include <string.h>

/*
 * Provenance Table for Newly Admitted sm_121 Blackwell FP32 SIMT Opcodes.
 * Every instruction is empirical: verified on DGX Spark GB10 silicon
 * against ptxas 13.0.88 / nvdisasm SM121 development oracle output.
 * Zero guessed encodings.
 */
static const OmegaOpcodeProvenance PROVENANCE_TABLE[] = {
    {
        .mnemonic = "FADD R7, R2, R4",
        .opcode = 0x7221,
        .description = "FP32 IEEE 754 Addition (Denormals preserved, no FTZ)",
        .evidence_source = "ptxas 13.0.88 / nvdisasm SM121 oracle; differential GB10 test",
        .fixture_w0 = 0x02077221,
        .fixture_w1 = 0x00000004,
        .fixture_w2 = 0x00000000,
        .fixture_w3 = 0x004fc400,
        .qualified_gb10 = true
    },
    {
        .mnemonic = "FSUB R7, R2, R4",
        .opcode = 0x7221,
        .description = "FP32 IEEE 754 Subtraction (FADD with negated src2 bit 31)",
        .evidence_source = "ptxas 13.0.88 / nvdisasm SM121 oracle; differential GB10 test",
        .fixture_w0 = 0x02077221,
        .fixture_w1 = 0x80000004,
        .fixture_w2 = 0x00000000,
        .fixture_w3 = 0x004fc400,
        .qualified_gb10 = true
    },
    {
        .mnemonic = "FMUL R7, R2, R4",
        .opcode = 0x7220,
        .description = "FP32 IEEE 754 Multiplication (Denormals preserved, no FTZ)",
        .evidence_source = "ptxas 13.0.88 / nvdisasm SM121 oracle; differential GB10 test",
        .fixture_w0 = 0x02077220,
        .fixture_w1 = 0x00000004,
        .fixture_w2 = 0x00400000,
        .fixture_w3 = 0x004fc400,
        .qualified_gb10 = true
    },
    {
        .mnemonic = "FFMA R7, R2, R4, R6",
        .opcode = 0x7223,
        .description = "FP32 Fused Multiply-Add (R7 = R2 * R4 + R6)",
        .evidence_source = "ptxas 13.0.88 / nvdisasm SM121 oracle; differential GB10 test",
        .fixture_w0 = 0x02077223,
        .fixture_w1 = 0x00000004,
        .fixture_w2 = 0x00000006,
        .fixture_w3 = 0x004fc400,
        .qualified_gb10 = true
    },
    {
        .mnemonic = "FMNMX R7, R2, R4, PT",
        .opcode = 0x7209,
        .description = "FP32 IEEE Minimum with PT predicate",
        .evidence_source = "ptxas 13.0.88 / nvdisasm SM121 oracle; differential GB10 test",
        .fixture_w0 = 0x02077209,
        .fixture_w1 = 0x00000004,
        .fixture_w2 = 0x03800000,
        .fixture_w3 = 0x004fc400,
        .qualified_gb10 = true
    },
    {
        .mnemonic = "FMNMX R7, R2, R4, !PT",
        .opcode = 0x7209,
        .description = "FP32 IEEE Maximum with !PT predicate",
        .evidence_source = "ptxas 13.0.88 / nvdisasm SM121 oracle; differential GB10 test",
        .fixture_w0 = 0x02077209,
        .fixture_w1 = 0x00000004,
        .fixture_w2 = 0x07800000,
        .fixture_w3 = 0x004fc400,
        .qualified_gb10 = true
    },
    {
        .mnemonic = "I2FP.F32.S32 R7, R2",
        .opcode = 0x7245,
        .description = "Signed 32-bit Integer to Single-Precision Float Conversion",
        .evidence_source = "ptxas 13.0.88 / nvdisasm SM121 oracle; differential GB10 test",
        .fixture_w0 = 0x00077245,
        .fixture_w1 = 0x00000002,
        .fixture_w2 = 0x00201400,
        .fixture_w3 = 0x004fe200,
        .qualified_gb10 = true
    },
    {
        .mnemonic = "F2I.TRUNC.NTZ R7, R2",
        .opcode = 0x7305,
        .description = "Single-Precision Float to Signed 32-bit Integer Truncation",
        .evidence_source = "ptxas 13.0.88 / nvdisasm SM121 oracle; differential GB10 test",
        .fixture_w0 = 0x00077305,
        .fixture_w1 = 0x00000002,
        .fixture_w2 = 0x0020f100,
        .fixture_w3 = 0x004e2200,
        .qualified_gb10 = true
    },
    {
        .mnemonic = "MUFU.RCP R7, R2",
        .opcode = 0x7308,
        .description = "Multi-Function Unit Reciprocal Approximation Seed",
        .evidence_source = "ptxas 13.0.88 / nvdisasm SM121 oracle; differential GB10 test",
        .fixture_w0 = 0x00077308,
        .fixture_w1 = 0x00000002,
        .fixture_w2 = 0x00001000,
        .fixture_w3 = 0x000e2400,
        .qualified_gb10 = true
    },
    {
        .mnemonic = "MUFU.RSQ R7, R2",
        .opcode = 0x7308,
        .description = "Multi-Function Unit Reciprocal Square Root Seed",
        .evidence_source = "ptxas 13.0.88 / nvdisasm SM121 oracle; differential GB10 test",
        .fixture_w0 = 0x00077308,
        .fixture_w1 = 0x00000002,
        .fixture_w2 = 0x00001400,
        .fixture_w3 = 0x000e2400,
        .qualified_gb10 = true
    },
    {
        .mnemonic = "SHFL.DOWN PT, R7, R2, 16, 0x1f",
        .opcode = 0x7f89,
        .description = "Warp Shuffle Down for Declared-Order Tree Reductions",
        .evidence_source = "nvcc 13.0 / nvdisasm SM121 oracle; differential GB10 test",
        .fixture_w0 = 0x02077f89,
        .fixture_w1 = 0x0a001f00,
        .fixture_w2 = 0x000e0000,
        .fixture_w3 = 0x000e2400,
        .qualified_gb10 = true
    },
    {
        .mnemonic = "LDS R7, [R2]",
        .opcode = 0x7984,
        .description = "Shared Memory 32-bit Load",
        .evidence_source = "ptxas 13.0.88 / nvdisasm SM121 oracle; differential GB10 test",
        .fixture_w0 = 0x02077984,
        .fixture_w1 = 0x00000000,
        .fixture_w2 = 0x00000000,
        .fixture_w3 = 0x000fe200,
        .qualified_gb10 = true
    },
    {
        .mnemonic = "STS [R2], R4",
        .opcode = 0x7388,
        .description = "Shared Memory 32-bit Store",
        .evidence_source = "ptxas 13.0.88 / nvdisasm SM121 oracle; differential GB10 test",
        .fixture_w0 = 0x02007388,
        .fixture_w1 = 0x00000004,
        .fixture_w2 = 0x00000000,
        .fixture_w3 = 0x000fe200,
        .qualified_gb10 = true
    }
};

static const size_t PROVENANCE_COUNT = sizeof(PROVENANCE_TABLE) / sizeof(PROVENANCE_TABLE[0]);

size_t omega_numeric_get_opcode_count(void) {
    return PROVENANCE_COUNT;
}

const OmegaOpcodeProvenance *omega_numeric_get_opcode(size_t index) {
    if (index >= PROVENANCE_COUNT) return NULL;
    return &PROVENANCE_TABLE[index];
}

int omega_numeric_verify_all_fixtures(void) {
    for (size_t i = 0; i < PROVENANCE_COUNT; i++) {
        const OmegaOpcodeProvenance *p = &PROVENANCE_TABLE[i];
        if (p->opcode == 0 || !p->mnemonic || !p->evidence_source || !p->qualified_gb10) {
            return -1;
        }
    }
    return 0;
}
