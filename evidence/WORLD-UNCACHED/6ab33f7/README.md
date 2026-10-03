# WORLD-UNCACHED receipt (fix commit 6ab33f7 on omega main 4cca032, physics 6d7cf0d)

Symptom: WORLD_STALE_GENERATION_MARKER_REFUSED failed intermittently (~10% per gate); every world completion wait took ~1039 ms and 2.8% timed out at 5 s (STALEGEN-DIAG-20261003 run-003..009).
Root cause: the world runtime completion marker page, scratch arena, data buffers and code entries used plain nvrm_alloc = NVOS32_ATTR2_GPU_CACHEABLE_YES, which nvos.h:1115-1118 (nvidia-open-580.173.02) says is not coherent with CPU mappings on system memory.
Fix: those four allocations are GPU-uncached; both dispatch builders add the C3 tail (L2_FLUSH_DIRTY mem-op, then a second WFI release at marker page + 0x10) and the synchronous dispatches wait on that second marker. omega_world_drain keeps waiting on marker 1 (caller-built pushbuffers carry one release).

Chip evidence (full sealed logs: ~/workspace/evidence-out/WORLD-UNCACHED-20261003/, SHA256SUMS per run; summaries copied here):
- red-001 (test commit 6da5acd, unfixed): WORLD_COMPLETION_WAIT_BOUNDED FAIL, wait_ms=1038.736, 11/12 gates. [OBSERVED]
- green-001 (6ab33f7): 12/12, wait_ms=0.117. suite-001: lifecycle suite 5/5 x 12/12, WORLD_STALE_GENERATION_MARKER_REFUSED 5/5, wait 0.118-0.123 ms. [OBSERVED]
- host-001: make test 12/12, test-language 407/407, test-numeric-transc 10/10 mutants killed, six gpu-engine host targets rc=0. [OBSERVED]
- regress: M17 18/18 every run. M18 is an intermittent pre-existing flake on main: alternating A/B campaign, 4 batches x 5 runs each under the same host load: main 4cca032 failed 7/20 (CLEAN_CLONE 6, NUMERICAL_BOUND 1); fixed 6ab33f7 failed 6/20 (BOUNDARY_ANNIHILATION 3, CLEAN_CLONE 2, NUMERICAL_BOUND 1). Earlier baseline-m18-001 at low load: 0/5. [OBSERVED]
- Those M18 gates run through src/omega_blackwell_submit.c, untouched by this change and still on plain nvrm_alloc. [INFERRED, medium] same cache family (hive-phases I37 / R11 wrong results under load); next cut.

Limits: one chip, one driver (nvidia-open 580.173.02); the M18 A/B ran while an unrelated cargo test suite loaded the CPU (same for both arms); UNKNOWN whether a cached-marker timeout means the job never ran or sat in L2 (I40).
