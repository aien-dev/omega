# AT-0 Agent 4 failure taxonomy

Three layers of classification are used, and they are never mixed.

1. **Case refusal codes** (AT0_CASE_V1 section 5, closed set): what the
   independent codec says about a case file. `CASE_PARSE_ERROR`,
   `CASE_NONCANONICAL`, `CASE_UNSUPPORTED_VERSION`, `CASE_INVALID_PARAMETER`,
   `CASE_IRRATIONAL_SPECTRUM`, `CASE_ID_MISMATCH`, applied in that order.
2. **Physics failure codes** (AT0_RESULT V1/V2, closed set): what the ten
   checks say about the physics of a run. `TRIVIAL_PHYSICAL_STATE`,
   `POVM_NORMALIZATION_EXCEEDED`, `PROBABILITY_SUM_EXCEEDED`,
   `PROBABILITY_OUT_OF_RANGE`, `SCHRODINGER_DEVIATION_EXCEEDED`,
   `CONSTRAINT_RESIDUAL_EXCEEDED`, `BOUND_KIND_INSUFFICIENT`,
   `PRECISION_INSUFFICIENT`, `CONDITIONAL_UNDEFINED`, `NONFINITE_VALUE`. The
   evaluator re-derives this set itself and compares it with the reported set.
3. **Evaluator findings** (`E4-*`, this document): what the evaluator says
   about a result file. A finding is a defect in the record or in the
   implementation that produced it, never a statement about physics.

A negative control that fails with exactly its expected physics codes is a
PASS for the evaluator (the implementation detected the defect). A physically
different model is one whose honest record carries different physics codes;
an incorrect implementation is one whose record carries `E4-*` findings. The
two are reported separately and never collapsed into one verdict.

## Evaluator statuses

| status | meaning |
|---|---|
| PASS | every recomputation and re-derivation agrees with the record; no finding |
| FAIL | at least one finding of FAIL severity: the record is wrong, forged, mismatched or physically contradicted |
| INCONCLUSIVE | only INCONCLUSIVE-severity findings: the record cannot be cited (dirty tree, unverified contract commit) or the evaluator could not form its own opinion (shadow unavailable) |
| NOT_RUN | the test exists but was not executed in this run |
| BLOCKED_NO_CANDIDATE | the test needs a candidate hook that was not supplied |

UNISOLATED is a property of the whole qualification run (ISOLATION_REPORT.md),
not a test status, and it is printed in `results/qualification.json` as such.

## Finding codes

Severity: F = FAIL, I = INCONCLUSIVE. Any F finding makes the result FAIL.

### Structure of the result file (E4-STRUCT-, F)

| code | meaning |
|---|---|
| E4-STRUCT-UNREADABLE | result file cannot be read |
| E4-STRUCT-BYTES | a byte rule is violated (non-ASCII, CR, blank line, leading/trailing/double space, missing final LF) |
| E4-STRUCT-VERSION | header, domain or contract line is not an AT0_RESULT v1/v2 triple that agrees with itself |
| E4-STRUCT-ORDER | a line is out of the fixed order or a required line is missing |
| E4-STRUCT-TRUNCATED | file ends before the required lines |
| E4-STRUCT-TRAILING | bytes after the final `end` |
| E4-STRUCT-PARSE | a line does not parse in its required shape |
| E4-STRUCT-ENCODING | a value, bound or digest is not in canonical encoding (non-canonical hex, scaled decimal, NaN/inf, negative zero) |
| E4-STRUCT-CASE-SHAPE | the embedded case block is not shaped as AT0_CASE_V1 |
| E4-STRUCT-CASE-REFUSED | the embedded case is refused by the independent codec |
| E4-STRUCT-ERROR-CODE | the error code is not in the closed set or disagrees with the outcome |
| E4-STRUCT-UNDEFINED-TOKEN | `undefined` on constraint_residual, povm_residual, a clock_probability or a reference while the kernel is nontrivial (pauli `undefined` is governed by the label status rules below) |
| E4-STRUCT-TRIVIAL-KERNEL-SHAPE | the kernel is trivial but the values block is not in the trivial-kernel shape |
| E4-STRUCT-UNDEFINED-ON-DEFINED | `undefined` is used for a label that is physically defined |
| E4-STRUCT-VALUE-ON-UNDEFINED | a number is reported for a label that is undefined |
| E4-STRUCT-POVM-UNDEFINED | povm_residual is `undefined` (it is always real) |
| E4-STRUCT-REFERENCE-UNDEFINED | a reference probability is `undefined` (it is always real) |
| E4-STRUCT-RESIDUAL-ON-TRIVIAL | constraint_residual carries a value in the trivial-kernel shape |

### Digests and identities (E4-ID-, F)

| code | meaning |
|---|---|
| E4-ID-CASE | recomputed case_id or acceptance_id differs from the embedded one |
| E4-ID-VERDICT | recomputed verdict_id differs (verdict block altered after signing, or wrong tag) |
| E4-ID-EVIDENCE | recomputed evidence_digest differs (any line above it altered) |

### Binding to the supplied case file (E4-BIND-, F)

| code | meaning |
|---|---|
| E4-BIND-CASE-UNREADABLE | the `--case` file cannot be read |
| E4-BIND-CASE-REFUSED | the `--case` file is refused by the independent codec |
| E4-BIND-CASE-FILE | reported case_file_sha256 differs from the supplied file's digest |
| E4-BIND-CASE-ID | embedded case_id differs from the supplied case's |
| E4-BIND-ACCEPTANCE-ID | embedded acceptance_id differs from the supplied case's |
| E4-BIND-SEMANTIC-BLOCK | embedded semantic block differs from the supplied case's (same id claimed for different physics) |
| E4-BIND-ACCEPTANCE-BLOCK | embedded acceptance block differs (tolerances or expected codes altered) |
| E4-BIND-CASE-NAME | embedded case name differs |

### Checks, outcome, codes (F)

| code | meaning |
|---|---|
| E4-CHECK-MISMATCH | a check verdict differs from the exact re-derivation (named check, reported vs derived) |
| E4-OUTCOME-MISMATCH | outcome differs from the re-derived outcome |
| E4-CODES-MISMATCH | failure_codes differ from the re-derived set (missing, extra, unsorted, duplicate) |
| E4-EXPECTATION-MISMATCH | expectation_met differs from the comparison of derived codes with the case's expected codes |
| E4-LABEL-STATUS | a label status (DEFINED/UNDEFINED) disagrees with the exact kernel analysis |

### Bounds (E4-BOUND-, F)

| code | meaning |
|---|---|
| E4-BOUND-NONE-NONZERO | bound_kind NONE with a non-zero bound |
| E4-BOUND-CLAIM-FALSE | a reported value differs from the shadow by more than its stated bound plus the shadow's bound: the bound claim is false |

### Shadow oracle disagreement (E4-SHADOW-, F except where noted)

| code | meaning |
|---|---|
| E4-SHADOW-KERNEL-DIM | reported kernel_dim differs from the exact one |
| E4-SHADOW-POVM | povm_residual contradicts the shadow (exact identity expected but non-zero reported, or vice versa beyond bounds) |
| E4-SHADOW-CLOCK-PROB | a clock probability contradicts the shadow |
| E4-SHADOW-PAULI | a Pauli probability contradicts the shadow (named label and axis) |
| E4-SHADOW-REFERENCE | a reference probability contradicts the shadow |
| E4-SHADOW-UNAVAILABLE (I) | the shadow could not be computed for this case; the evaluator has no opinion on the values |

### Provenance and placeholders

| code | sev | meaning |
|---|---|---|
| E4-PLACEHOLDER-SHAPE | F | a run that did not complete has values where placeholders are required, or vice versa |
| E4-PROV-NONE-IN-COMPLETED-RUN | F | `none` provenance token in a completed run |
| E4-PROV-ORACLE-IS-ENGINE | F | oracle digest equals engine digest: the reference was produced by the thing under test |
| E4-TIME-ORDER | F | end time precedes start time, or times are not UTC in the required form |
| E4-PROV-CONTRACT-UNVERIFIED | I | contract_commit is not a commit verified to hold the frozen contract bytes; record not citable until verified |
| E4-PROV-DIRTY | I | source_tree_clean NO; record may not be cited for a gate |

## Mutant to finding map (red before green)

| planted defect | caught by | discriminating cases |
|---|---|---|
| axis_swap | E4-SHADOW-PAULI | KAT, P1e, P2 |
| y_sign_swap | E4-SHADOW-PAULI | KAT, P1e |
| conjugate_bug | E4-SHADOW-PAULI (Y) | KAT, P1e, P1g (blind: P1f) |
| reversed_reference | E4-SHADOW-REFERENCE (Y) | KAT, P1e, P1g (blind: P2) |
| dephased_state | E4-SHADOW-PAULI, E4-CHECK-MISMATCH | KAT |
| wrong_weight_hidden | E4-SHADOW-CLOCK-PROB, E4-CHECK-MISMATCH | N4, N4b (blind: KAT) |
| hardcoded_table | E4-SHADOW-* | P1b, P1c, P1d (blind: KAT, P2b) |
| kernel_dim_wrong | E4-SHADOW-KERNEL-DIM | KAT |
| residual_over_bound | E4-CHECK-MISMATCH | KAT |
| nonfinite_hidden | E4-STRUCT-ENCODING or E4-CHECK-MISMATCH | KAT |
| verdict_forged | E4-CHECK-/OUTCOME-/CODES-MISMATCH | N1, N2, N3, N6, N7 (blind: KAT) |
| evidence_corrupt | E4-ID-EVIDENCE | KAT |
| verdict_id_corrupt | E4-ID-VERDICT | KAT |
| case_id_altered | E4-ID-CASE, E4-BIND-CASE-ID | KAT |
| tolerance_altered | E4-BIND-ACCEPTANCE-BLOCK, E4-ID-CASE | KAT |
| binding_rebound | E4-BIND-* | KAT |
| contract_commit_wrong | E4-PROV-CONTRACT-UNVERIFIED (I) | KAT |
| dirty_tree | E4-PROV-DIRTY (I) | KAT |
| times_reversed | E4-TIME-ORDER | KAT |
| oracle_is_engine | E4-PROV-ORACLE-IS-ENGINE | KAT |
| bound_none_nonzero | E4-BOUND-NONE-NONZERO | KAT |
| label_status_wrong | E4-LABEL-STATUS | KAT (blind: N7, labels already UNDEFINED) |
| label_claimed_defined | E4-LABEL-STATUS | N7 (blind: KAT, labels already DEFINED) |
| placeholder_with_values | E4-PLACEHOLDER-SHAPE | KAT |
| crlf | E4-STRUCT-BYTES | KAT |
| trailing_space | E4-STRUCT-BYTES | KAT |

Blind pairs are listed in `run.sh` and reported INCONCLUSIVE with the reason
"indistinguishable from truth on this case by construction" so that the
coverage gap is visible in the machine-readable results rather than hidden.

## Additions after independent review (Opus 5.5, 2026-10-09)

| code | sev | meaning |
|---|---|---|
| E4-UNBOUND-NO-CASE-FILE | I | `result` was run without `--case`; the embedded case is trusted, so the verification is not citable |

Mutant grading rule: a planted defect counts as caught only when the evaluator
grades the mutant file FAIL. INCONCLUSIVE, a crash or an empty status is an
escape. The two mutants that are INCONCLUSIVE by design (dirty tree, unknown
contract commit) are tested separately for exactly that status.
