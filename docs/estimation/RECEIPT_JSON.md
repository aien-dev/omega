# Estimation receipt JSON rules (est-json-1)

Status: applies to receipts written by `est5`, `est4`, `est3c_eval` and `est_eval` from this
change on. Receipts already committed are never rewritten (a receipt is named by the SHA-256 of
its bytes). No protocol, parameter, data, threshold or verdict rule changes here.

## The defect this fixes

`receipts/est-v5/receipt-7f0bb701...json` is byte-exact to its digest but is not strict JSON
(RFC 8259): `"hi": inf` for the unbounded `n_scored` threshold (3 times) and
`"ten_log_score": nan` for the F1 and E0 baselines, which have no ten-step score (2 times).
Cause: `js_stats` in `tools/estimation/est5.c` printed doubles with `%.17g`, which writes `inf`
and `nan`. `est4.c` (v4) and `est3c_eval.c` (v3) carried the same function; `est_eval.c` printed
several statistics with `%.17g` and no finite check. The same function also wrote a non-finite
statistic value as the finite number `-1.0`.
Strict parsers reject the file. `jq` 1.7 does not (it reads `inf` as 1.797e308 and `nan` as
`null`), so `jq` is not a validator for these receipts; `test_est_json --validate FILE` is.

## Rules

1. A receipt never contains `inf`, `-inf` or `nan`. Finite measured values stay finite numbers
   (`%.17g`, unchanged).
2. Unbounded threshold: `"lo"` / `"hi"` is `null` and `"lo_unbounded"` / `"hi_unbounded"` is
   `true`. Bounded thresholds keep their number and `false`. A NaN threshold is not unbounded:
   it is invalid (rule 4).
3. Unavailable measurement: a baseline (F1, E0) has no ten-step log score. `"ten_log_score"` is
   `null` and `"ten_log_score_status"` is `"unavailable"`. A measured ten-step score is a number
   with status `"ok"`. v3 receipts have no ten-step field.
   A gated statistic with too few steps (regime idle / in-trial) that cannot be computed is
   `"value": null` with `"value_status": "unmeasured"`. Finite values have `"value_status": "ok"`.
4. Unexpected non-finite measured value (mean log score, width80 mean or median, the selected
   family's ten-step score, a gated statistic value, a NaN threshold) is written as `null`, its
   name is listed in the block's `"invalid_fields"`, a statistic value gets
   `"value_status": "invalid"`, and the top level carries `"invalid_measurements": N`.
   The scoring decision fails closed: `N > 0` forces the verdict to `HELD_OUT_FAIL` (v3: `FAIL`)
   with reason `non-finite measured value (marked invalid in the receipt)`. It can never pass.
   The gate itself already failed such a statistic (`band()` in `est3c_common.c` passes only
   finite values). For `est_eval.c` a non-finite number is written as `null` and its `ok_*` gate
   comparisons are false for NaN, so it fails there too.
5. New receipts carry `"json_rules": "est-json-1"` (est4, est5, est3c_eval).
6. Naming is unchanged: `receipt-<sha256 of the receipt bytes>.json`, written once, never
   overwritten.

## Code

`c3_json_num`, `c3_json_stats`, `c3_stats_invalid` in `tools/estimation/est3c_common.c` (used by
est3c_eval, est4, est5); `N17` in `tools/estimation/est_eval.c`.
Tests: `make test-est-json` (unit checks, strict validator, plain and ASan/UBSan) and
`make test-est5-tools` (strict parse of the synthetic and the binding-path receipts).
