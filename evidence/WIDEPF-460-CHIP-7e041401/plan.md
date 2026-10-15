# Plan WIDEPF (omega #460 on the chip)
DOCS READ: omega src/omega_gpu_matmul_api.h:100-102 "host oracle (double accumulation, tolerance 1e-5 of the accumulation scale)"; DOCS SILENT on SASS STG immediate width (own-layer rule frag_block_store_fits)
SYMPTOM: omega_gpu_matmul_* at N 151936 with an even 16-row tile count returns CODEGEN_FAIL (pre-fix).
SMALLEST REPRO: src/widepf_repro.c linked against each candidate's build/libomega_gpu.a, shapes m in {16,32,64,128,2048} x k 2048 x n 151936, plus 128x2048x65536 (rt 4->2 step).
CAMERA: per shape line: rule rt/ct, rc, last_error, launches, grid, chip_ns, repeat bit-compare, ref bad/max_rel, poison left.
NEGATIVE CONTROL: perturbed element must be flagged (negctl_bad=1); API mutant kernel must be flagged (--mutant); pre-fix commit must refuse the even shapes.
BASELINE then VARIANTS: pre-fix df9bd9c4 (red) then main 7e041401 (green); same binary source, one variable (omega commit).
RULES: candidates untouched (detached worktrees); quietlock hold --owner WIDEPF --minutes 20; never kill a chip run; MAX :18097 untouched.
## Deviations
