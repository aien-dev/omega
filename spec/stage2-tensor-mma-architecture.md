# OMEGA Blackwell Stage 2 Architecture: Tensor Core MMA

Milestone 18 Stage 2 transitions OMEGA from scalar integer arithmetic to physical Blackwell Tensor Core matrix multiply-accumulate execution.

## 1. Scope & Execution Boundary
- Target: NVIDIA DGX Spark (Grace Blackwell GB10, sm_121, 128 GiB unified LPDDR5x RAM)
- Substrate: M16 Native Libcuda-Free Channel
- Precision Targets:
  1. FP16 input fragments with FP32 accumulator
  2. BF16 input fragments with FP32 accumulator
- Verification Standard: Zero static instruction tables. Full dynamic compilation via IR lowering, bounded regalloc, and programmatic 128-bit sm_121 instruction encoding.

## 2. Research Oracle Methodology (Non-Lineage)
Using DGX Spark native CUDA 13.0 compiler (`/usr/local/cuda-13.0/bin/nvcc`) and disassembler (`/usr/local/cuda-13.0/bin/cuobjdump -sass`) solely as an external verification oracle to isolate:
1. Target SASS instruction opcode and primary/secondary bitfields for sm_121 Tensor Core MMA.
2. Warp-level fragment mapping across threads for matrix tiles (e.g. m16n8k16 or m16n8k8).
3. Register operand allocation: destination quad (4 GPRs for FP32 accumulator), source A pair (2 GPRs for packed FP16/BF16 inputs), source B (1 or 2 GPRs).
4. Control word scoreboard barriers: pipeline latency wait cycles and write barrier dependencies.

## 3. Dynamic Compilation Path (Stage 2)
1. Semantic Specification: `OmegaMatMulSpec` with `OMEGA_MATMUL_PRECISION_FP16` and `OMEGA_MATMUL_PRECISION_BF16`.
2. IR Representation: `BW_IR_MMA` instruction nodes capturing matrix tile coordinates, fragment descriptors, and accumulator registers.
3. Bounded Register Allocation: Contiguous register bundle constraints for multi-word fragments (e.g., 4-register aligned groups for accumulator output).
4. Machine Code Emission: Programmatic 128-bit field synthesis into sm_121 instruction words.
5. QMD v5.0 Launch Descriptors: Warp geometry (32 threads per warp) and CTA tile layout.
6. M16 Native GPFIFO Submission: Silicon launch, cache coherence, and completion synchronization.

## 4. Pending Stage 2 Gates
- Gate 12: `OMEGA_BW_MATMUL_TENSOR_CORE_EXECUTION_PASS` (Physical GB10 silicon MMA execution with 9-point evidence bundle)
- Gate 13: `OMEGA_BW_MATMUL_NUMERICAL_BOUND_PASS` (Numerical error < 10^-4 against IEEE FP32 oracle)
- Gate 14: `OMEGA_BW_MATMUL_BOUNDARY_ANNIHILATION_PASS` (Identity, zero, extreme-scale, and subnormal inputs)
- Gate 16: `OMEGA_BW_MATMUL_CLEAN_CLONE_PASS` (Reproduction in isolated fresh clone)
