# omega#323 GB10 window, 2026-10-07 (hold d6d82f-attn, 02:17:34Z to 02:27:46Z, exit 1)

Code under test: omega 5a9f759 (draft PR #323), physics at physics.lock. Nothing patched or rerun after the run.

- hd64 regression (tools/run_gpu_attention_chip.sh): rc 0. run.log PASS 122/0, sweep PASS 73/0, timing PASS 2/0 (hd64-regression/).
- hd128 (`./build/gpu_attention_test --hd 128 --out receipt-hd128.json`): rc 1, "FAIL: 126 checks, 71 failed" (run-hd128.log).
  - ctx1 PASS (worst_scaled_err 0), ctx17 PASS (0.00198 of the allowed 1.0) on the first two launches.
  - ctx256 (first multi-chunk context): the launch never completed; the host waited the full 600000 ms and returned CHIP_FAIL (call_ns 600000552302). The session then latched (by design) so every later case was refused: ctx2048 .. ctx1000 and all later cases are NOT a chip result, they are the latch. Only ctx1 and ctx17 are valid hd128 chip observations.
  - The hd128_checks results at the end also ran after the latch: not valid chip results.
- Observed on the chip: a 128-thread CTA with 64 GPR and 2048 B declared shared memory launched and completed correctly for contexts 1 and 17 (single, partial chunk). Whether 2048 B shared memory, four warps in BAR.SYNC, or the second-chunk loop is the cause of the ctx256 stall is UNKNOWN (ctx127/128 never ran validly).
- DOCS SILENT items remain: exact 12.1 per-block shared limit, per-block thread cap, SM_CONFIG code 9 meaning.
- Disclosure from the coordinator: session 711736 ran CPU-only load 02:11:23Z to 02:17:41Z (no GPU use). The hold began 02:17:34Z, 7 s before it stopped; timing figures from the first seconds (build step) are possibly CPU-contended. Parity results are not affected.
