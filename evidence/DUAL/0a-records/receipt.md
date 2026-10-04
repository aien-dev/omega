# DUAL-0a receipt: records and registry (ADR 0031 section 4)

| Field | Value |
|---|---|
| repository | aien-dev/omega |
| base commit | cb7cc216147664549484fcca8e8ceadbd6f6cdbf (GitHub main, fetched 2026-10-04) |
| candidate commit | (this commit; see git log) |
| working tree | clean at commit (crumb compiled last) |
| compiler | cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 |
| flags | EST_CFLAGS: -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -ffp-contract=off -fno-fast-math; ASan/UBSan: -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all |
| host | Spark (aarch64 Linux); host-only; no GPU exercised |
| record schema | format version 1, kind bytes 1..7, domains omega.dual.*.v1 (docs/dual/DUAL_RECORDS.md) |

## Baseline before changes (same tree at cb7cc216147664549484fcca8e8ceadbd6f6cdbf)
`make test-estimation test-inertial-alignment`: test_est_types PASS, test_est_kf PASS, est_ref PASS, test_ans 1372 checks PASS (plain and ASan); exit 0.

## Commands and results
| Command | Result |
|---|---|
| `make dual-purity` | PASS: rx_dual.o references only memmove/memset/strlen/sha256_* (nm -u) |
| `make test-dual-types` (plain, PROP_ROUNDS=2000) | test_dual_types: 14107 checks, 0 failures: PASS |
| `make test-dual-types` (ASan/UBSan, PROP_ROUNDS=200) | test_dual_types: 1667 checks, 0 failures: PASS |

## Hostile cases covered (all refused)
invalid kind byte; kind confusion (resource bytes decoded as constraint/outcome, constraint bytes as resource); unsupported version;
truncated encoding; trailing bytes; capacity-short buffer; negative scale; zero scale; NaN; +Inf; -Inf (scale, lambda, estimate,
budget, uncertainty, eta, lambda_max, predicted); negative lambda; negative uncertainty; unit NONE / unknown unit; unit mismatch
against registry; unknown resource id (registry find, controller lambda_max); duplicate resource (registry, controller, price vector,
recommendation consequences); unordered resource list (controller, price vector, builder); undeclared class; INVARIANT construction
(checker, digest, decoder bytes, price-vector builder); broken parent linkage both ways; missing estimate_ref / controller_id /
evidence_root / budget_contract / observation_ref (MEASURED); FRESH without calibration; recommendation authority != NONE (struct and
bytes); out-of-range actual/recommended; lying error field; unmeasured row with a value; outcome generation going backwards;
controller count above capacity in bytes; site name canonical padding / unterminated / non-printable.

## Negative controls (mutants of src/dual/rx_dual.c, same suite)
| Mutant | Result |
|---|---|
| allow INVARIANT to construct | 14155 checks, 5 failures: FAIL (killed) |
| drop -0.0 canonicalization | 14107 checks, 1 failure: FAIL (killed) |
| accept trailing bytes | 14107 checks, 8 failures: FAIL (killed) |
| drop authority check | 14107 checks, 2 failures: FAIL (killed) |

## Verdict
DUAL-0a: IMPLEMENTED, tests PASS on host. `DUAL_CONSTRAINT_CONTRACT`: NOT CLAIMED (needs DUAL-0b binding and staleness).
No production influence. No runtime linkage.
