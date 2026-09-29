# TURING Wave 1: ownership manifest

Branch: feat/turing-w1 (from omega origin/main 4b217aa). Scope: Field V0 records + control-arm selector, post hoc over
stored receipts. Spec: TURING_W0_PROPOSAL.md sections F, I, J, K. No PR is opened from this wave by the lanes.

## Lanes

| Lane | Owns (may create and edit) | Reads only | Status |
|---|---|---|---|
| FIELD | src/turing/field.h, src/turing/field.c, src/turing/field_select.c, src/turing/select.h, src/turing/replay.c, tools/turing_field.c, tests/turing/test_turing.c | src/algebra/realize_common.h (registry), src/omega_canonical.{h,c}, src/omega_types.h, src/sha256.{h,c}, evidence/MIXED_ALGEBRA/*.json | built, test-turing PASS |
| CONTROL ARM | src/turing/history_selector.c | select.h, field.h | built, test-turing PASS |
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
- `turing_compare` / `turing_compare_online` (replay.c) score both against the same oracle.

## Commands
- `make test-turing`: plain and ASan+UBSan suites, then the rebuild check (spec ids from the plain and ASan builds must
  be byte-identical), then `turing-field` under ASan.
- `make turing-field`: history view, winners per shape, selector comparison, kill-test dry run.
- Precondition for any build: /home/drakestapleton/workspace/.spark-quiet must not exist.

## Follow-ups (not in this wave)
- omx_candidate adapter (18 language candidates) after feat/polyglot-0 merges; gate J then covers 28.
- H1 live run with cpu frequency state, quiet flag and seeded shuffle order in its receipt.
- rx_costmodel writing decision records, after the runtime freeze lifts.
- Observation levels on ARGUS (H2), after Drake's ruling on the runtime freeze.
