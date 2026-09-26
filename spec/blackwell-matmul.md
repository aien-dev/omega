# OMEGA Blackwell Tensor MatMul Specification (Milestone 18)

Canonical Reference: `SPEC-ACCEL-M18` (`docs/milestone-18-spec.md` in `aien-architecture`)

## 1. Scope & Sovereignty Mandate
Milestone 18 establishes native Blackwell tensor matrix multiplication on physical NVIDIA DGX Spark GB10 silicon.

Milestone 18 strictly forbids qualifying through static precompiled instruction tables. OMEGA must operate as a dynamic sm_121 machine code generator:
1. **Instruction Selection**: Lowering matrix multiplication semantic graph ($G_S$) into native sm_121 instruction nodes.
2. **Register Allocation**: Programmatic mapping of live values to physical General Purpose Registers (R0-R255) and Uniform Registers (UR0-UR63).
3. **Operand & Field Encoding**: Bitfield synthesis of 128-bit sm_121 SASS machine instructions.
4. **Instruction Sequencing**: Dynamic assembly of load stages, tensor-core matrix multiply-accumulate operations, synchronization barriers, and global memory stores.
5. **Dynamic QMD v5.0 Launch Descriptors**: 2D CTA grid rasterization configured for matrix tile geometry.
6. **Native M16 GPFIFO Pushbuffer Submission**: Direct hardware launch and coherent semaphore completion on physical GB10 silicon.
7. **Zero Foreign Userspace Runtime**: Zero dynamic linkage to `libcuda.so` or `libcudart.so`.

## 2. Mathematical Contract
$$C[m, n] = \sum_{k=0}^{K-1} A[m, k] B[k, n] \quad \text{for } m \in [0, M-1], n \in [0, N-1]$$

- Integer Phase: Exact modulo wrap ($C[m, n] \pmod{2^{32}}$) with zero tolerance against reference oracle.
- Floating-Point Phase: FP16/BF16 with FP32 accumulator within ULP bounds.
- Dimensions: Square canonical tiles ($16\times 16$, $32\times 32$) and rectangular sweeps ($16\times 64 \times 32$).

## 3. Four-Component Realization Identity
$$\text{REALIZATION\_ID} = \text{SHA-256}(\text{spec\_id} \parallel \text{machine\_id} \parallel \text{code\_digest} \parallel \text{sm\_arch})$$
Where `code_digest` is computed over the dynamically generated machine code.
