# OMEGA Blackwell Stage 2 Architecture: Tensor Core MMA

Milestone 18 Stage 2 transitions OMEGA from scalar integer arithmetic to physical Blackwell Tensor Core matrix multiply-accumulate execution.

## 1. Scope & Execution Boundary
- Target: NVIDIA DGX Spark (Grace Blackwell GB10, sm_121, 128 GiB unified LPDDR5x RAM)
- Substrate: M16 Native Libcuda-Free Channel
- Precision Targets:
  1. FP16 input fragments with FP32 accumulator (`HMMA.16816.F32`)
  2. BF16 input fragments with FP32 accumulator (`HMMA.16816.F32.BF16`)
- Verification Standard: Zero static instruction tables. Full dynamic compilation via IR lowering, bounded regalloc, and programmatic 128-bit sm_121 instruction encoding.

## 2. Research Oracle Differential Calibration (OFAT Ground Truth)
Using DGX Spark native CUDA 13.0 compiler (`nvcc`) and disassembler (`nvdisasm`) as a non-lineage research oracle, one-factor-at-a-time (OFAT) differential calibration isolated every bitfield of the sm_121 HMMA instruction:

### Exact 128-bit Instruction Encoding Specification

| Word | Bitfield | Value / Expression | Semantic Meaning |
| :--- | :--- | :--- | :--- |
| `w[0]` | Bits 0..15 | `0x723c` | Base opcode for Blackwell `HMMA` |
| `w[0]` | Bits 16..23 | `rd & 0xff` | Destination quad register ($R_d..R_{d+3}$ FP32 accumulator output) |
| `w[0]` | Bits 24..31 | `ra & 0xff` | Source A quad register ($R_a..R_{a+3}$ packed FP16/BF16 tile A) |
| `w[1]` | Bits 0..7 | `rb & 0xff` | Source B pair register ($R_b..R_{b+1}$ packed FP16/BF16 tile B) |
| `w[1]` | Bits 8..31 | `0x000000` | Unused / reserved operand bits |
| `w[2]` | Bits 0..7 | `rc & 0xff` | Source C quad register accumulator input (or `0xff` for $RZ$) |
| `w[2]` | Bits 8..15 | `0x18` | Tile geometry mode: `16816.F32` ($M=16, N=8, K=16$, FP32 accumulator) |
| `w[2]` | Bit 18 | `0x00040000` | **Precision selector**: `0` = FP16 (`HMMA.16816.F32`), `1` = BF16 (`HMMA.16816.F32.BF16`) |
| `w[3]` | Control | Scheduled | Dynamic control word: stall count, read/write barrier dependencies |

### Verified OFAT Invariants
1. **Precision Toggle**: Toggling strictly bit 18 of `w[2]` (`0x00040000`) with identical register allocations switches between `HMMA.16816.F32` and `HMMA.16816.F32.BF16` with zero other mnemonic or operand modifications.
2. **Operand Attribution**:
   - `Rd`: Tested $R_0, R_4, R_8, R_{12}, R_{16}, R_{20}, R_{24}, R_{32}$ in `w[0]` bits 16..23 with 100% agreement.
   - `Ra`: Tested $R_0, R_4, R_8, R_{12}, R_{16}, R_{20}, R_{24}, R_{32}$ in `w[0]` bits 24..31 with 100% agreement.
   - `Rb`: Tested $R_0, R_2, R_4, R_6, R_8, R_{10}, R_{12}, R_{16}$ in `w[1]` bits 0..7 with 100% agreement.
   - `Rc`: Tested $RZ$ (`255`), $R_0, R_4, R_8, R_{12}, R_{16}, R_{20}$ in `w[2]` bits 0..7 with 100% agreement.
3. **Control Word Independence**: Varying `w[3]` adjusts pipeline latency and scoreboard barrier masks without changing opcode semantics.
4. **Register Bundles**: To avoid register overlap and bank conflicts, the register allocator enforces 4-register aligned groups ($R_{4k}$) for quad bundles (`Rd`, `Ra`, `Rc`) and 2-register aligned groups ($R_{2k}$) for pair bundles (`Rb`).

## 3. Dynamic Compilation Path (Stage 2)
1. Semantic Specification: `OmegaMatMulSpec` with `OMEGA_MATMUL_PRECISION_FP16` and `OMEGA_MATMUL_PRECISION_BF16`.
2. IR Representation: `BW_IR_HMMA_F16` and `BW_IR_HMMA_BF16` instruction nodes capturing matrix tile coordinates, fragment descriptors, and accumulator registers.
3. Bounded Multi-Register Allocation: Contiguous bundle constraints for quad and pair registers.
4. Machine Code Emission: Programmatic 128-bit field synthesis into sm_121 instruction words.
5. QMD v5.0 Launch Descriptors: Warp geometry (32 threads per warp) and CTA tile layout.
6. M16 Native GPFIFO Submission: Silicon launch, cache coherence, and completion synchronization.

## 4. Oracle Quantization & Numerical Parity Contract (Gate 13 & Gate 14)
1. **Pre-Accumulation Input Quantization**: The CPU reference oracle must first cast input values strictly to target precision (IEEE FP16 or BF16 7-bit mantissa) before simulating tensor-core accumulation.
2. **Accumulation**: Summation executes in IEEE FP32 precision.
3. **Tolerance**: Relative error bound $\frac{|C_{\text{device}} - C_{\text{oracle}}|}{|C_{\text{oracle}}| + \epsilon} \le \text{tolerance}$ and ULP bounds calibrated specifically to tile summation depth $K$, preventing false failures due to input truncation.
4. **Subnormal & Annihilation Characterization**: Treat subnormal flush-to-zero (FTZ) behavior on tensor-core paths as an empirical hardware contract rather than assuming scalar FP IEEE semantics.
