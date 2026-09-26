# Specification: Milestone 14 — Omega Realization Synthesis (`OMEGA_REALIZATION_SYNTHESIS`)

```text
Document ID:     SPEC-OMEGA-M14
Milestone:       Milestone 14 (OMEGA_REALIZATION_SYNTHESIS)
Classification:  Sovereign Machine Canonical Specification
Target Substrate: Automated $G_S \times G_M \to G_R$ Synthesis Targeting Declared Hardware Capabilities
Status:          SPECIFIED / IN PROGRESS (aien-dev/omega#19, aien-dev/aien-architecture#25)
Lineage:         SILICON -> ATLAS (M1) -> PHYSICS (M2/M3) -> OMEGA (M4-M14) -> AIEN
```

---

## 1. Executive Summary & Doctrine

Milestone 14 establishes **automated machine-aware realization synthesis**:

> **LOWERING IS NOT A HARDCODED COMPILER BACKEND. REALIZATION IS SYNTHESIS: $G_S \times G_M \to G_R$. THE MACHINE DISCOVERS HOW BEST TO EXECUTE A GIVEN SEMANTIC GRAPH ON PHYSICAL SILICON.**

In the sovereign hierarchy:
- $G_S$ defines what computation means.
- $G_M$ defines the physical hardware topology (issue width, units, latencies, memory hierarchy), certified by Physics.
- $G_R$ is the synthesized native realization, bound to BOTH $G_S$ and $G_M$ via a cryptographic triple binding:
  $$\text{REALIZATION\_ID} = \text{SHA256}(\text{OMG0} \mid \text{SEMANTIC\_ID} \mid \text{MACHINE\_ID} \mid \text{code\_bytes})$$

---

## 2. Core Invariants

1. **Triple Identity Binding**: $REALIZATION_ID$ deterministically incorporates $SEMANTIC_ID$ ($G_S$), $MACHINE_ID$ ($G_M$), and machine code bytes.
2. **Machine-Aware Scheduling**: Synthesis evaluates candidate instruction schedules and registers to exploit $G_M$'s issue width (e.g., 4-wide dispatch on DGX Spark Neoverse V2 vs 2-wide on QEMU virt) and functional unit concurrency.
3. **Strict Semantic Preservation**: Executing $G_R$ natively produces outputs bit-for-bit identical to semantic evaluation of $G_S$.
4. **Zero Foreign Toolchain**: Direct machine byte generation without LLVM, Clang, GCC assembler, or GNU `as`.
5. **Mandatory M7 Verification**: Every synthesized realization passes $V_0$ structural, $V_1$ differential, and $V_2$ property verification before admission.

---

## 3. Data Structures & Declarations

```c
typedef struct {
    const OmegaProgram *program;
    const OmegaMachineGraph *machine;
    bool optimize_latency;
    uint32_t max_unroll_factor;
} RealizationSynthesisTask;

typedef struct {
    bool solved;
    RealizationObject realization;
    SemanticId realization_id;
    uint32_t estimated_cycles;
    uint32_t code_bytes_len;
    VerifyReport verify_report;
} RealizationSynthesisResult;

int omega_synthesize_realization(const RealizationSynthesisTask *task,
                                 RealizationSynthesisResult *result);
```

---

## 4. Qualification Gates

1. `OMEGA_REAL_SYNTH_INIT_PASS`: Synthesis task initialized with valid $G_S$ and $G_M$.
2. `OMEGA_REAL_SYNTH_TRIPLE_ID_PASS`: `REALIZATION_ID` deterministically incorporates $SEMANTIC_ID$ and $MACHINE_ID$.
3. `OMEGA_REAL_SYNTH_SCHEDULE_OPT_PASS`: Machine-aware schedule optimization generates valid AArch64 machine words.
4. `OMEGA_REAL_SYNTH_DGX_SPARK_PASS`: Realization specialized for 4-wide DGX Spark Grace V2 pipeline.
5. `OMEGA_REAL_SYNTH_QEMU_VIRT_PASS`: Realization specialized for 2-wide QEMU virt pipeline.
6. `OMEGA_REAL_SYNTH_SEMANTIC_PARITY_PASS`: Native execution matches semantic evaluation on test inputs.
7. `OMEGA_REAL_SYNTH_V0_STRUCTURAL_PASS`: Synthesized realization passes M7 V0 structural verification.
8. `OMEGA_REAL_SYNTH_V1_DIFFERENTIAL_PASS`: Synthesized realization passes M7 V1 differential evaluation.
9. `OMEGA_REAL_SYNTH_V2_PROPERTY_PASS`: Synthesized realization passes M7 V2 property verification.
10. `OMEGA_REAL_SYNTH_RECEIPT_PASS`: Master qualification receipt generated binding $G_S$, $G_M$, and $G_R$.
