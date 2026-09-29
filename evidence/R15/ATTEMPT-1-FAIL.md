# R15 qualification attempt 1: FAIL (recorded, not a pass)

Run `20260929T020536Z-ad8e1f2ea4e4-silicon` (GB10, 2026-09-28 21:05-21:39 CDT),
candidate `ad8e1f2`, clean tree, 12 rounds + 5 Level-1 + 5 Level-2 runs.
Raw evidence: `raw/20260929T020536Z-ad8e1f2ea4e4-silicon/` (all files kept,
25 MB). Raw digest (SHA-256 of `SHA256SUMS`):
`062f5b6ac812d305620bc5ab4e0b97f505cdf7f8c93a996e99e1329ebe83f8dd`.

Outcome: **FAIL**, 15 of 16 gates pass. Per spec §11 a failed gate means no
PASS: profile, fix at its layer, rerun the correctness gates, repeat the
entire qualification. This run is not rerun away and no criterion changes
without a spec commit.

| Gate | Outcome | Value / note |
|---|---|---|
| G1 correctness | PASS | 48 trials correct |
| G2 AFTER RES-1/SEQ | PASS | median 1.086 (12 pairs) |
| G3 ADAPT RES-4/SEQ | PASS | CI lo 104.5 |
| G4 RES-4 ADAPT/BEFORE | PASS | 1.609 |
| G5 L1-A latency | PASS | p50 7376 ns, p99 11792 ns |
| G6 AEGIS fast path | PASS | p50 21 ns, p99 23 ns |
| G7 generation barrier | PASS | 0.966; 348 barriers, 0 failed, p99 107 ms |
| G8 RES-1/NODIGEST | PASS | 0.832 |
| G9 energy RES-1/SEQ | PASS | 0.796 |
| **G10 CPU-GPU syncs per GPU result** | **FAIL** | **RES-1 80.8 vs SEQ 61.6 (needs RES-1 <= SEQ)** |
| G11 crossover | PASS | 28,936 ops |
| G12 wake amplification | PASS | RES-4 1.188 vs SEQ 1184.9 |
| G13 wasted RES-1/SEQ | PASS | 1.000 |
| G14 conflict rate | PASS | 0 |
| G15 GPU residency | PASS | 1.0 (58 silicon processes) |
| G16 metrics present | PASS | all present |

G10 detail. Claims and results are fixed at 256 on both sides; all the
variance is in `gpu_polls_empty` (empty completion polls) per Level-2 run:

| Run | RES-1 empty polls | SEQ empty polls |
|---|---|---|
| 1 | 22,254 | 14,754 |
| 2 | 8,158 | 13,313 |
| 3 | 19,924 | 24,401 |
| 4 | 10,486 | 12,703 |
| 5 | 24,828 | 29,434 |

The ranges overlap; practice run 2 on the same commit gave RES-1 40 vs SEQ 104
(PASS). Being profiled before any fix.

Run health:
- 133 processes, 0 failed (every `exit 0` in `runs.txt`).
- 0 new GPU Xid during the run (the only Xid since boot, 109 at 19:10 CDT,
  came from an earlier practice run, before this run started).
- Reducer deterministic: `build/r15_reduce` rerun on this raw directory
  produced a byte-identical `summary.json`.

Known gap in this attempt: `machine.json` records `aienos_commit` as blank.
`../aienos-capability-c` was a git worktree whose `.git` file pointed to a
deleted temporary directory, so `git rev-parse HEAD` failed there. Its
`native/capability` sources were identical to the pinned `aienos.lock`
commit `c8ab65e3ba2fddbe99245f747fdfdb99f6042fd3` (checked after the
directory was re-cloned at that commit on 2026-09-28), but the run itself
did not record it, so this attempt could not have been receipted as a PASS
in any case.
