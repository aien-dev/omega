# M18-UNCACHED receipt (fix commit b20ff9e on omega main 4cca032, physics 6d7cf0d)

Symptom: M18 matmul gates (src/omega_blackwell_submit.c, both INT32 and tensor executors) fail intermittently under host CPU load: 7/20 runs on main 4cca032 in the WORLD-UNCACHED A/B (CLEAN_CLONE, NUMERICAL_BOUND, BOUNDARY_ANNIHILATION; one INT32_INTERMEDIATE bit mismatch), 0/5 at low load. hive-phases I37, memory "R11 wrong results under load".
Root cause class [INFERRED, medium]: all 16 executor allocations used plain nvrm_alloc = NVOS32_ATTR2_GPU_CACHEABLE_YES, which nvos.h:1115-1118 (nvidia-open-580.173.02) says is not coherent with CPU mappings on system memory. Same family as CHIPWAIT (#237), the engine (#220) and the world runtime (#238).
Fix: the 16 allocations are GPU-uncached; both executors add the C3 tail (L2_FLUSH_DIRTY mem-op, then a second WFI release at marker + 0x10, payload 0x46464646) and the host waits marker1, marker2, dsb sy before reading results back.

Chip evidence (sealed: ~/workspace/evidence-out/M18-UNCACHED-20261003/, this folder holds the per-run summaries, exit codes, heads, binary digests and the campaign scripts):
- Alternating A/B campaign ab_campaign.sh, 4 batches x 5 M18 runs per arm, same host load for both arms: 16 sha256sum loops (load.txt; load average rose 16 -> 37 over the campaign, campaign.txt).
- baseline 4cca032 (same binary digest as the WORLD-UNCACHED baseline arm): 1/20 FAIL (batch L2 rep 5: OMEGA_BW_MATMUL_NATIVE_SUBMIT_PASS). [OBSERVED]
- fix b20ff9e: 0/20 FAIL. [OBSERVED]
- Host battery on b20ff9e: make test 12/12, test-language, test-numeric-transc, test-gpu-engine all rc=0. [OBSERVED]

Reading: the red arm reproduced only weakly under this load (1/20 here vs 7/20 earlier under a memory-heavy cargo test suite), so this A/B alone does not prove the fix; the fix stands on the documented cache attribute plus the three sibling fixes already chip-proven. [INFERRED, medium] The CPU-only sha256sum load stresses the cache family less than the earlier mixed load.
Limits: one chip, one driver; load type differed from the earlier campaign; UNKNOWN whether a cached-buffer failure means stale input, stale output or a late marker (I40 follow-up). Chip receipt digests for M17/M18 are re-pinned by this change (submit.c edit).
