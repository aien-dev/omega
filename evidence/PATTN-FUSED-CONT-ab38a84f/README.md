# prefill_attn chip gate: continuation check skipped on the fused path

- red-df9bd9c4/: unfixed main, `--reps 9 --fused --quick` on the GB10: 127 checks, 2 failed
  ("mirror view valid 0 (want 369)", "stale control 0").
- this folder: tools/run_gpu_prefill_attn_chip.sh at ab38a84f (pinned physics 6d7cf0d4):
  GPU_PREFILL_ATTN_CHIP: PASS. main (split, mirror) runs the continuation check: mirror valid 369,
  0 differ, stale control 32768, 0 after invalidate. main-fused skips it with the reason: 121/0.
