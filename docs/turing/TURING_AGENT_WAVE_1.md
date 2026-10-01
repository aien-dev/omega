# TURING Wave 1: ownership manifest

Update 2026-09-30: R16 is CLOSED (omega#112, `3dd5eaa`, all gates G1 to G8 PASS) and the `src/runtime/` edit freeze is LIFTED (aien-architecture #70, `e89ba94`). Statements below that the runtime is frozen "until R16" describe the state when this was written.

Branch: feat/turing-w1 (from omega origin/main 4b217aa). Scope: Field records + control-arm selector, post hoc over
stored receipts. Kill test FAILED (K.6); reframed 2026-09-29 as the record-keeping layer (K.7). Spec: TURING_W0_PROPOSAL.md sections F, I, J, K. No PR is opened from this wave by the lanes.

## Lanes

| Lane | Owns (may create and edit) | Reads only | Status |
|---|---|---|---|
| FIELD | src/turing/field.h, src/turing/field.c, src/turing/field_select.c (v1), src/turing/field_select_v0_retired.c (frozen, reproduces the FAIL), src/turing/select.h, src/turing/replay.c, tools/turing_field.c, tests/turing/test_turing.c | src/algebra/realize_common.h (registry), src/omega_canonical.{h,c}, src/omega_types.h, src/sha256.{h,c}, evidence/MIXED_ALGEBRA/*.json | built, test-turing PASS |
| CONTROL ARM | src/turing/history_selector.c (control arm + the shared ranking core) | select.h, field.h | built, test-turing PASS |
| INTEGRATION | Makefile: the TURING block at the end only (targets test-turing, turing-field); docs/turing/* | everything | done |
| REVIEW (hostile) | nothing (report only) | everything on the branch | next |

## Forbidden in this wave
- Any edit under src/runtime/ (frozen until R16), src/algebra/, src/polyglot/, or to branch feat/polyglot-0.
- New or changed Omega semantic ids (OSC-0B, omega PR #76). The V0 contract_digest is PROVISIONAL and domain-separated.
- Rust, Python, systemd, outside dependencies, timed benchmarks, GPU work.
- Editing receipts under evidence/: adapters read them; tamper tests work on copies in /tmp.

## Interfaces between lanes
- `turing_store` (field.h) is the only shared state: specs, evidence, receipts, contract digest.
- Both selectors emit `turing_decision`; only the Field selector fills `cite[]`.
- `turing_rank_min_cost` (history_selector.c) is the one ranking rule; the Field v1 selector calls it, never a copy.
- `turing_compare` / `turing_compare_online` (replay.c) score both against the same oracle.

## Commands
- `make test-turing`: plain and ASan+UBSan suites, then the rebuild check (spec ids from the plain and ASan builds must
  be byte-identical), then `turing-field` under ASan.
- `make turing-field`: history view, winners per shape, selector comparison, kill-test dry run.
- Precondition for any build: /home/drakestapleton/workspace/.spark-quiet must not exist.

## Next steps (after the K.7 reframe)
- PR to main from feat/turing-w1 with the recorded FAIL, the reframe and the v1 tests; not merged by the lanes.
- **Wave 2: ARGUS realization event class** (observation levels, proposal G and H2). BLOCKED until the src/runtime
  freeze lifts at R16 close (omega#68). No src/runtime edit before then.
- **omx_candidate adapter** (18 language candidates) after feat/polyglot-0 merges to main; gate J then covers 28.
- H1 live run with cpu frequency state, quiet flag and seeded shuffle order in its receipt; it measures the control
  rule live and checks every live decision record verifies. It does not re-score Q1 (K.7).
- rx_costmodel writing decision records, after the runtime freeze lifts.
