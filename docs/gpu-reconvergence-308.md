# GPU structured warp reconvergence (omega #308)

Branch feat/gpu-reconvergence-308, code at 0a1812f, receipts in evidence/gpu-reconv-308/.
Evidence classes used below: PROVEN BY TEST (a test in this repo passes, command and receipt
named), MEASURED (read off the real chip or the real nvcc), DERIVED FROM REFERENCE (taken from
Mesa NAK source, not checked by us on the chip), INFERRED (reasoned from documentation, not
checked), UNKNOWN.

## 1. The failure mode

On the GB10 (sm_121) a warp is 32 lanes. When lanes take different paths (a lane-dependent
branch or loop exit) and the code then runs a warp-wide step (SHFL, or a shared-memory exchange
guarded by BAR.SYNC), the lanes must first be brought back together. Without an explicit
reconvergence point the chip may run the later step with only some lanes active and reads the
inactive lanes' registers, which are undefined.

Two places in AIEN hit this and were worked around by removing the divergence:
- The original divergent prime sieve (omega 888a011, lane-dependent mark loop) gave wrong
  results; the production C5 sieve avoids it with a warp-uniform loop.
- Attention with a `j >= ctx` branch gave wrong results in the SHFL reduction after it
  (2026-10-04); production attention is branch-free.

nvcc brackets such code with BSSY (arm a barrier register, name the join point) and BSYNC (wait
for every armed lane, continue as one warp). Omega's Blackwell code generator had neither. #308
adds them, plus a host simulator that rejects code which misuses them.
Chip observation (MEASURED, T1b): the diamond probe with its BSYNC dropped runs on the chip
without error and returns 466 of 1000 words wrong.

## 2. Encodings (BSSY / BSYNC, sm_121)

| Item | Value | Class |
|---|---|---|
| BSSY.RECONVERGENT | w0 = 0x945 \| pred<<12 \| bar<<16; w1 = (delta_insns - 1) * 16; w2 = 0x03800200; control 0x000fe200 (stall 1, yield) | MEASURED (nvcc 13.0.88 -arch=sm_121, evidence/gpu-reconv-308/oracle/) |
| BSYNC.RECONVERGENT | w0 = 0x941 \| pred<<12 \| bar<<16; w1 = 0; w2 = 0x03800200; control 0x000fea00 (stall 5, yield) | MEASURED (same oracle) |
| Opcodes 0x945 / 0x941, barrier id in bits 16..19 | Mesa NAK sm70_encode.rs OpBSSy / OpBSync | DERIVED FROM REFERENCE |
| RECONVERGENT bit (bit 73) | set by nvcc, omitted by NAK | MEASURED (nvcc) / DERIVED (NAK). We emit nvcc's form. |
| Branch target of BSSY | the instruction after the BSYNC; loop exits and breaks jump to the BSYNC itself (where nvcc sends them) | MEASURED (oracle k_break, k_loop, k_nested) |

Golden fixtures for both words and refusals -101..-105 pass in `build/gpu_reconv_test`
(PROVEN BY TEST, 220 checks, T1a). nvdisasm decodes every emitted listing with zero
undecodable words (PROVEN BY TEST, same run).

## 3. Region API and fail-closed rules

Bookkeeping in src/omega_bw_reconv.h (`bw_regions_*`), wrapped by region_begin / region_exit /
region_join in src/omega_gpu_elementwise_api.c and src/omega_gpu_attention_api.c.

- begin allocates barrier register Bn with n = nesting depth, then emits BSSY. 16 barrier
  registers (B0..B15) are encodable; a 17th nested region is refused and the build fails.
  Whether the chip really provides 16 is not documented to us; we ran 16 deep on the chip
  (rc_depth16, MEASURED, T1a) and no deeper.
- exit records a forward branch that leaves the region; join patches it to the BSYNC and patches
  the BSSY target to the instruction after the BSYNC.
- Refused at codegen: SHFL, BAR.SYNC or EXIT inside an open region; an unclosed region; a BSYNC
  with no BSSY; on the launch path, SHFL or BAR.SYNC after a predicated EXIT (lanes already gone).
  (PROVEN BY TEST: RC_REFUSE_* probes and RC_DEPTH16 in `build/gpu_reconv_test`.)
- This is why R6 (lanes exit early, then BAR.SYNC/SHFL on the rest) is refused at codegen rather
  than proven correct on the chip.

## 4. Host warp simulator (tests/bw_warp_sim.h)

A warp is a set of fragments (pc, lane mask). A predicated branch whose lanes disagree splits the
fragment. Fragments run one at a time in a chosen order; tests run both lowest-pc-first and
highest-pc-first. Fragments merge only at a BSYNC.

Errors (first wins, each names the pc): SHFL_SPLIT, SHFL_EXITED, BSYNC_UNARMED, BSSY_REARM,
JOIN_MISMATCH, NONMEMBER, STUCK, NEVER_JOINED, plus range and resource errors.

BAR.SYNC reached by a split warp is allowed and counted (`bar_split_arrivals`). Basis: the CUDA
programming guide says the CTA barrier counts arriving threads since Volta. Class: INFERRED for
sm_121, not verified by us as a rule. Supporting observation (MEASURED earlier, prime-race
receipts): the production C5 staged sieve has 128 split arrivals at 1e6 in the simulator and is
chip-correct. Fragments stay split after the barrier (a barrier is not a reconvergence point), so a
later SHFL without a BSYNC still fails with SHFL_SPLIT.

Limits: EX2 and RCP are exact in the simulator, approximate on the chip. LDCU64 is not modelled.
Warp issue counts are a model, not chip time (UNKNOWN how well they track the chip).

## 5. Hazard / scheduling metadata

From NAK sm120: BSSY, BSYNC and BClear are Decoupled (variable latency, tracked by the
scoreboard). DERIVED FROM REFERENCE. Omega does not enforce this: `schedule_fixed_pairs` ignores
control ops, and the control words above (stall 1 / stall 5, both yield) are copied from nvcc.
Chip correctness of the shapes we emit is MEASURED (T1, T2, T4); a different placement is not covered.

## 6. Evidence table

| # | Claim | Class | Command / receipt |
|---|---|---|---|
| 1 | BSSY/BSYNC words match nvcc; refusals -101..-105 | PROVEN BY TEST + MEASURED | `build/gpu_reconv_test` PASS 220; oracle/cf.{cu,sass} |
| 2 | Region API bounds and refusals (16 ok, 17 refused, SHFL/BAR/EXIT in region, unclosed) | PROVEN BY TEST | gpu_reconv_test PASS 220 |
| 3 | Simulator catches dropped BSYNC, corrupted target, exit past join, early exit then SHFL, split SHFL | PROVEN BY TEST | gpu_reconv_test; `gpu_attention_test --sim --divergent` PASS 124 |
| 4 | R3 diamond, R4 loop break, R5 nested, R6 exit+BAR+SHFL, 16-deep nest: 19 sizes each (incl. partial warps), bit-exact vs CPU oracle on the chip | PROVEN BY TEST (chip) | T1a/GPU-RECONV-0a1812f/receipt.json: OMEGA_GPU_RECONV_PASS, 220 checks, 95 chip cases, 0 failed. Repeat T1b PASS 220 |
| 5 | Chip catches math mutants; dropped BSYNC gives wrong words | MEASURED | T1 receipts: mutants diamond 755, loopbreak 377, nested 110 violations, exit_bar CODEGEN_FAIL; dropped BSYNC 466/1000 words wrong (T1b) |
| 6 | R2 attention j>=ctx branch inside a region, SHFL after the join, bit-correct incl. partial warps | PROVEN BY TEST (chip) | T2-attention: production run PASS 122, sweep PASS 73; `--divergent` run PASS 122, sweep PASS 73; timing PASS 2 |
| 7 | Existing gates unchanged | PROVEN BY TEST (chip) | T3: elementwise 31 checks PASS; cut4b (elementwise, attention, matmul 18 + timing 4, session probe 0/30 first-call failures) PASS; prime-race run_gb10_chip.sh VERDICT PASS (585 audit runs, 1172 passes, mutant caught) in T3b |
| 8 | R1 original divergent sieve (888a011 mark loop in a region) and nested variant bit-identical to oracle | PROVEN BY TEST (chip) | T4: PR_GB10_MARK=divergent and divergent2, 585 runs each (every DESIGN limit plus 1e7), 0 mismatches |
| 9 | BAR.SYNC on a split warp is legal on sm_121 | INFERRED | see section 4 |
| 10 | 16 barrier registers exist on the chip beyond depth 16 / refusal of 17 matches hardware | UNKNOWN | only depth 16 tested |
| 11 | R6 correctness after early exit | not proven: refused at codegen | section 3 |
| 12 | Simulator issue counts predict chip time | UNKNOWN | model only |

## 7. Performance (R1, honest)

Setup: `PR_GB10_CTA_BUDGET=64`, limit 1e6, 5 s per run, same session, three marks interleaved,
8 rounds (T4/perf*.json, perf64.log, perf64-repeat.log). Unit: passes per second. MEASURED.

| mark | per-round passes/s | note |
|---|---|---|
| uniform (C5, production) | 7618 7530 5213 6354 6914 7485 7936 6960 | noisy, 5213..7936 |
| divergent (888a011 loop in a region) | 9155 9147 9134 9127 9158 9107 9113 9162 | 9107..9162 |
| divergent2 (plus prime loop, nested) | 9092 9247 9119 9263 9004 9081 9126 9183 | 9004..9263 |

The brief's C5 baseline was 9139 passes/s. In this session the C5 uniform build measured lower
and much noisier than that figure, so the table does not support a claim that divergent beats C5.
What it does show: the regioned divergent kernels lose no throughput against the 9139 baseline and
sit flat near 9140. A flat figure that matches the old baseline to within 0.2 percent suggests a
fixed per-pass cost (launch, host base-prime sieve, marker wait) dominates at this size
(INFERRED, not investigated; out of scope). Why the uniform run is noisier is UNKNOWN.
Kernel size: 88 instructions for all three; registers 29 (uniform) vs 26 (divergent).
The warp-simulator model predicts divergent warp issues 2.53M (util .656) and divergent2 2.01M
(util .830) against C5 global 2.89M (util .774) at 1e6 (model only, not chip time).
Correctness was the objective; this section is reported for completeness.

## 7a. Invariant for new GPU code (Drake, 2026-10-05)

Any new lane-divergent code emitted for the Blackwell backend must either

1. wrap the divergent stretch in `region_begin` -> `region_exit` -> `region_join` and pass the host warp simulator (`build/gpu_reconv_test` plus the suite that exercises the kernel), or
2. show in the kernel comment why every branch is provably warp-uniform (all 32 lanes take the same path), as the C5 sieve mark loop does.

A SHFL or BAR.SYNC that depends on a lane-divergent branch without an enclosing region is a defect, not a style choice: on the chip the dropped-BSYNC mutant returned 466 of 1000 words wrong with no error (evidence/gpu-reconv-308/T1b-*). The emitter refuses the cases it can see (section 3); the simulator catches the rest. Reviewers should ask for one of the two proofs on every PR that adds a predicated branch.

## 8. What was not changed

Production C5 sieve, branch-free attention, test thresholds, omega main. Divergent kernels are
opt-in (`PR_GB10_MARK`, `--divergent`).

## 9. Limitations

- Only the shapes in the probes are chip-verified. BSSY/BSYNC placement other than "BSSY before
  the divergence, every exit to the BSYNC, SHFL/BAR after the join" is not covered.
- R6 is refused, not proven. Barrier registers beyond 16 deep untested (and refused at 17).
- Control-op latency metadata is documented, not enforced.
- Chip runs were taken on one machine (GB10, driver and kernel in each receipt's device.txt).

## 10. Reproduce

    make build/gpu_reconv_test PHYSICS_DIR=../physics-reconv-lock
    sh tools/run_gpu_reconv_chip.sh [OUT] [-- --observe-dropped-join]
    build/gpu_attention_test [--divergent] [--sweep] --out receipt.json
    PR_GB10_MARK=divergent bench/prime_race/build/gb10_native --limit 1000000 --audit-passes 5

Chip runs need a quiet window and the GPU lock (/tmp/aien-gb10.lock).
