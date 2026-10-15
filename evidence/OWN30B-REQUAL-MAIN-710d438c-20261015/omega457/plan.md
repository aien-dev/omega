# Plan (omega #457)
DOCS READ: nvos.h:1117-1118 "YES - Enable gpu caching if supported on this surface type. For system memory this will not be coherent with direct CPU mappings." clc96f.h:73 L2_SYSMEM_NCOH_INVALIDATE 0x11 (no comment). DOCS SILENT on whether the next launch sees a host write to a GPU-cached sysmem buffer without an L2 invalidate.
QUESTION (one line): after the host rewrites a GPU-cached (CPU_WRITE|GPU_READ) buffer the GPU already read, does the next launch read the old bytes from L2?
OUR CODE: omega src/omega_gpu_mem_cached.c (alloc GPU_CACHED), omega_gpu_platform_linux.c:180-187 (comment: written once before any GPU read), ref_sync = dsb sy only (:233-241). Only user: omega_gpu_fp8_api.c:268 (AIEN_OMEGA_FP8_WEIGHT_CACHE=1).
SMALLEST REPRO: src/l2_stale_test.c on the real FP8 GEMV path. Tensor 1 expert x 256 x 2048 (512 KB, L2-sized), scales 1.0, x = 1.0.
  A = all weights e4m3 +1.0 (0x38) -> y = +2048 per row. B = all -1.0 (0xB8) -> y = -2048.
  CAMERA: y decodes exactly into stale 128-byte lines per row: stale_lines = (y + 2048) / 256 (16 lines per row).
VARIANTS (one variable: cache policy; each run is one process):
  rewrite:  upload A, GEMV (GPU reads A), host memcpy B into the same buffer + sync_to_gpu (what upload does), GEMV, decode.
  realloc:  upload A, GEMV, free, upload B (new buffer, VA printed, same-VA noted), GEMV, decode.
  each run under cached=0 (NEGATIVE CONTROL: uncached mapping, must read 0 stale) and cached=1; 5 runs each.
  Instrument-can-say-yes check: the first GEMV on A must give +2048 (if the decode is broken it cannot show A).
RULES: candidate untouched (repro lives here, links the frozen build/libomega_gpu.a); no chip kill; GB10 lock + quiet hold.
## Deviations
- PINS first recorded scaffold defaults; corrected (appended) to omega-requal-main 710d438c clean, physics 6d7cf0d4 clean.
- After the answer (YES) a fix candidate was built in a separate worktree (~/workspace/omega-457-fix, branch fix/457-l2-inval-after-host-write)
  and measured as one variable (variant=fix). It is proposed, not merged here.
## Result
ANSWER: YES. A host rewrite of a GPU-cached buffer the GPU already read is NOT seen by later launches.
BASELINE (variant=baseline, lib 6a6cb4df = omega main archive, runs/summary.txt):
  rewrite cached=1: STALE_READ 5/5, 4096 of 4096 lines stale on each of 3 launches after the rewrite (y = +2048, want -2048) [OBSERVED runs/rewrite-c1-r*.log]
  rewrite cached=0 (negative control): FRESH 5/5. realloc cached=0|1: FRESH 5/5 each (same VA 0x1004133000 reused; phys UNKNOWN).
  Instrument can say yes: gemv-A read +2048 on all 256 rows in every run.
VARIANT fix (lib 8b7c0678, fix-variant.diff: pending L2_SYSMEM_NCOH_INVALIDATE at the start of the next submission after
  sync_cpu_to_gpu on a GPU-cached buffer): 20/20 FRESH, incl. rewrite cached=1 5/5 (runs/summary-fix.txt).
REGRESSION GATE tools/run_gpu_l2_stale_chip.sh: red on unfixed 710d438c = FAIL (rewrite c1 STALE_READ 5/5, others FRESH 15/15,
  gate-red/); green on fix = PASS 20/20 (gate-fix/). Same base 710d438c, one variable (platform change).
NO-REGRESSION on fix: fp8, decode_graph cached, cmdlist chip gates PASS (fixgates/).
LIMITS: one tensor shape (512 KB, L2-sized); fp8 GEMV path only; production code today writes cached weights once before any
  GPU read, so no live corruption was observed; realloc FRESH does not prove RM invalidates at free (DOCS SILENT).
