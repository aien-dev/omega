# OWN-30B re-qualification at omega main (after #439), 2026-10-15

Base: omega main 710d438c (62833658 #439 plus #455: checker2 fixtures, mk/checker2.mk, spec/one-layer-receipts.md and crumbs; no src change),
clean tree. Physics 6d7cf0d4 (physics.lock), clean. Driver nvidia-open 580.173.02, GB10.
libomega_gpu.a at this base: sha256 6a6cb4df479ddbcc5cd4ef0fda1aea968832b2b70be2ff3312051c3e9921aad5, byte-identical to
the pre-merge RC archive (d0889a94 notes) and to the archive aien-sovereign-core now locks.

Each gate ran in its own `quietlock hold --owner OWN30B --minutes 20` under `flock /tmp/aien-gb10.lock`, one after the
other, nothing killed or timed out. Commands: `omega-gates.sh` (series) and `stage2.sh` (reruns, cross-base, #457).
Verdict lines: `gates/summary.txt`; full output: `gates/<name>.out`; receipts: `gates/<name>/`.

## Gate results (gates/)

| gate | verdict | vs pre-merge |
|---|---|---|
| bf16_gemv | PASS | numbers UNVERIFIED (no committed pre-merge log) |
| cmdlist | PASS, chip 31/31 | same as DECODE-LAUNCH-COLLAPSE-20261013/cmdlist-r5-stamps (31/31) |
| decode_attn | PASS | UNVERIFIED |
| decode_graph main | PASS, chip 13/13; C one list 3978.7 us/token (2 layers) vs A 5002.5, B 5337.5 | pre-merge graph-r3-stamps C 3928.2, A 4891.4, B 5320.6 (C +1.3%, calc:c370931847535fb8) |
| decode_graph red | FAILED_AS_EXPECTED | same |
| decode_graph cached | PASS | UNVERIFIED |
| fp8, fp8_v2, fp8gemm | PASS | UNVERIFIED |
| l2d_barrier, l2d_prefetch | PASS | same verdicts as OWN30B-L2D-*-20261014/r1 |
| matmul_api, router, tcgemm, attention, cut4b, elementwise | PASS | UNVERIFIED |
| reconv | PASS 235/235 (run through `sh`, see below) | UNVERIFIED |
| prefill_attn | FAIL (2 checks, fused step only) | pre-existing, see below |
| l1g (qmma pfgemm qgemm) | FAIL (pfgemm only) | pre-existing, identical to OWN30B-L1G-GEMM-1e76e728 |

### prefill_attn FAIL: pre-existing test defect, not a merge regression
- Failing checks (step `main-fused`, `--fused --quick`): `continuation: mirror view valid 0 (want 369)` and
  `negative control: a foreign rewrite without invalidate must show (stale mirror), 0 outputs differ`. All other steps PASS
  (host 69/0, sim 97/0, main 164/0, warps4 127/0, spread 103/0 x4). [OBSERVED gates/prefill_attn/main-fused.log]
- Cause [OBSERVED in code]: the fused path never uses the K/V mirror (`src/omega_gpu_prefill_attn_api.c:298,333`
  `if (!g.fused && !g.mirror_off && ...)`), but `chip_continuation()` (added in bba7688a, cut 5) only skips when the build
  has no mirror or `--no-mirror`; it does not skip under `--fused`. The cut 5 evidence (OWN30B-L1A-PATTN5-PERSIST-bba7688a)
  ran only the non-fused main step, so this combination had no committed chip run before.
- Cross-base: the same gate at the pre-merge RC d0889a94 fails identically (`rc-d0889a94/prefill_attn/main-fused.log`).
  The gate script, the test and the prefill src are byte-identical between d0889a94 and 710d438c.
- Fix (not made here): `chip_continuation` should also skip in fused mode.

### l1g FAIL: pre-existing, identical to the pre-merge evidence
- `pfgemm-chip` (`gpu_pfgemm_test --shape-knob 4 16 1 --chip`): `gateup bit-identity on 48 sampled rows: 101 differ (beyond
  1 bf16 ulp; 1 within 1 ulp of 36864)`, GPU_PFGEMM: FAIL; down, qkv, o 0 differ. OWN30B-L1G-GEMM-1e76e728/pfgemm-chip.log
  has the same 101 / 1 / FAIL. Timings faster now (gateup median 3.216 ms vs 3.999 ms; down 2.666 vs 3.499).
  qmma probe, qgemm chip, qgemm cached, qgemm cached wait0 gateup/down: exit 0. qmma-words.bin is byte-identical to
  OWN30B-L1G-QMMA-PFGEMM-c742aa13/qmma-words.bin.

### reconv runner mode
`tools/run_gpu_reconv_chip.sh` is committed as mode 100644, so the series got rc 126 (Permission denied); the rerun through
`sh tools/run_gpu_reconv_chip.sh` is `gates/reconv-sh.out` (PASS 235/235).

## omega #457: cached CPU_WRITE buffers, stale L2 after a host write (omega457/)

ANSWER: YES. Sealed investigation folder (plan.md Result, SHA256SUMS; the two repro binaries
`l2_stale_test` d484de7b... and `l2_stale_test_fix` abfb86ae... are listed in SHA256SUMS but not committed).
- DOCS: nvos.h:1117-1118 "For system memory this will not be coherent with direct CPU mappings."; clc96f.h:73
  L2_SYSMEM_NCOH_INVALIDATE 0x11. DOCS SILENT on next-launch visibility after a host write.
- Baseline (frozen 6a6cb4df): rewrite of a GPU-cached FP8 weight buffer the GPU already read: STALE_READ 5/5, 4096 of 4096
  lines stale on each of 3 later launches. Negative control (uncached): FRESH 5/5. Free + re-upload: FRESH 5/5 both modes.
- Fix candidate (proposed in its own PR, not merged here): the platform records a host sync into a GPU-cached buffer and
  starts the next submission with L2_SYSMEM_NCOH_INVALIDATE. variant=fix 20/20 FRESH.
- Regression gate `tools/run_gpu_l2_stale_chip.sh`: red on unfixed 710d438c (FAIL, rewrite cached=1 STALE 5/5), green on
  the fix (PASS 20/20). fp8, decode_graph cached and cmdlist gates PASS on the fix.
- Production today writes cached weights once before any GPU read (`AIEN_OMEGA_FP8_WEIGHT_CACHE=1` only), so no live
  corruption was observed; any future host rewrite of a cached buffer would be read stale without the fix.

### #457 fix after review (omega457-gate-fix-r2/)
The fix branch was revised after review (pending flag cleared on device close, distinct refusal for the prefix copy). The
regression gate was rerun on that revision: GPU_L2_STALE_CHIP PASS, 20/20 FRESH. device.txt shows 8f491df2 dirty: the run
used the uncommitted revision, committed unchanged as 6c87d849 (its only src diff from 8f491df2).
