# OMEGA Blackwell Tensor MatMul Specification (Milestone 18)

Canonical Reference: `SPEC-ACCEL-M18` (`docs/milestone-18-spec.md` in `aien-architecture`)

## 1. Scope & Sovereignty Mandate
Milestone 18 establishes native Blackwell tensor matrix multiplication on physical NVIDIA DGX Spark GB10 silicon.

Milestone 18 strictly forbids qualifying through static precompiled instruction tables. OMEGA must operate as a dynamic sm_121 machine code generator:
1. **Instruction Selection**: Lowering matrix multiplication semantic graph ($G_S$) into native sm_121 instruction nodes.
2. **Bounded Deterministic Register Allocation**: Live-interval computation and physical register assignment (R0-R255, UR0-UR63) scoped to matrix multiplication kernels. Rejects predetermined hardcoded register tables.
3. **Operand & Field Encoding**: Programmatic synthesis of 128-bit sm_121 machine code words.
4. **Instruction Sequencing**: Dynamic assembly of load stages, tensor-core matrix multiply-accumulate operations, synchronization barriers, and global memory stores.
5. **Dynamic QMD v5.0 Launch Descriptors**: 2D CTA grid rasterization configured for matrix tile geometry.
6. **Native M16 GPFIFO Pushbuffer Submission**: Direct hardware launch and coherent semaphore completion on physical GB10 silicon.
7. **Zero Foreign Userspace Runtime**: Zero dynamic linkage to `libcuda.so` or `libcudart.so`.

## 2. Mandatory Tensor-Core Completion Invariant
INT32 matrix multiplication serves strictly as an intermediate codegen qualification step. Milestone 18 cannot complete until:
1. FP16 or BF16 tensor-core MMA executes on physical GB10 silicon.
2. Accumulation executes in FP32 precision.
3. Machine code is generated dynamically by OMEGA without static instruction tables.
4. Numerical parity falls strictly within bounded reference tolerances.
5. Empirical hardware evidence proves that the physical tensor-core MMA execution path was utilized.

## 3. Mathematical Contract
$$C[m, n] = \sum_{k=0}^{K-1} A[m, k] B[k, n] \quad \text{for } m \in [0, M-1], n \in [0, N-1]$$

- Intermediate Stage: Signed/unsigned 32-bit integer arithmetic ($C[m, n] \pmod{2^{32}}$) with exact bit-for-bit mathematical parity against CPU reference oracle.
- Mandatory Final Stage: FP16/BF16 tensor-core arithmetic with FP32 accumulator within ULP bounds.
- Dimensions: Square canonical tiles ($16\times 16$, $32\times 32$) and rectangular sweeps ($16\times 64 \times 32$).

## 4. Four-Component Realization Identity
$$\text{REALIZATION\_ID} = \text{SHA-256}(\text{spec\_id} \parallel \text{machine\_id} \parallel \text{code\_digest} \parallel \text{sm\_arch})$$
Where `code_digest` is computed over the dynamically generated machine code.

## 5. Ordered Development Sequence
1. INT32 semantic matrix multiplication contract.
2. Minimal IR and selected instruction nodes.
3. Bounded deterministic register allocator.
4. Dynamic scalar integer instruction encoder.
5. Physical INT32 matrix multiplication pass on GB10.
6. Stage-1 Dynamic codegen variation proof (`M18_CODEGEN_VARIATION_PASS`).
7. FP16/BF16 semantic matrix contract with FP32 accumulation.
8. Empirical determination of minimum sm_121 tensor MMA instruction forms using allowed research oracles.
9. Integration of tensor MMA instruction forms into OMEGA dynamic encoder.
10. Tensor tile register allocation.
11. Dynamic tensor-core kernel generation.
12. Physical GB10 tensor execution.
13. FP32-accumulation numerical parity verification.
14. Hardware evidence proof of tensor-core utilization (9-point evidence bundle).
15. Clean-clone isolated reproduction, zero-libcuda proof, and durable qualification receipt.

## 6. Milestone 18 Qualification Gates
- Gate 1: `OMEGA_BW_MATMUL_SEMANTIC_CONTRACT_PASS`
- Gate 2: `OMEGA_BW_MATMUL_MACHINE_GRAPH_PASS`
- Gate 3: `OMEGA_BW_MATMUL_ENCODER_UNIT_PASS`
- Gate 4: `OMEGA_BW_MATMUL_BOUNDED_REGALLOC_PASS`
- Gate 5: `OMEGA_BW_MATMUL_INSTRUCTION_SEQUENCING_PASS`
- Gate 6: `OMEGA_BW_MATMUL_CODE_TRUTH_PASS`
- Gate 7: `OMEGA_BW_MATMUL_CODEGEN_VARIATION_PASS` (Stage-1: 16x16x16 INT32 vs 32x16x64 INT32 distinct allocation, code bytes, digests)
- Gate 8: `OMEGA_BW_MATMUL_REALIZATION_ID_PASS`
- Gate 9: `OMEGA_BW_MATMUL_QMD_2D_PASS`
- Gate 10: `OMEGA_BW_MATMUL_NATIVE_SUBMIT_PASS`
- Gate 11: `OMEGA_BW_MATMUL_INT32_INTERMEDIATE_PASS`
- Gate 12: `OMEGA_BW_MATMUL_TENSOR_CORE_EXECUTION_PASS` (Mandatory completion gate with 9-point evidence bundle & FP16 != BF16 variation)
- Gate 13: `OMEGA_BW_MATMUL_NUMERICAL_BOUND_PASS`
- Gate 14: `OMEGA_BW_MATMUL_BOUNDARY_ANNIHILATION_PASS`
- Gate 15: `OMEGA_BW_MATMUL_ZERO_LIBCUDA_PASS`
- Gate 16: `OMEGA_BW_MATMUL_CLEAN_CLONE_PASS`
- Gate 17: `OMEGA_BW_MATMUL_REGRESSION_PASS` (139 / 139 prior milestone gates)
- Gate 18: `OMEGA_BW_MATMUL_RECEIPT_PASS`
