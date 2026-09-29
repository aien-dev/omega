# Turing Yield TY-2 result: PASS

**Under measurement profile V0, candidate C (op + previous 4 outcomes) reduced held-out total description length by
2,559,679.825 bits relative to baseline B (order-1), so C scored +2,559,679.825 T.**

That is 0.4387 bits per event over 5,834,845 held-out search events (crumbline exp-20260927-rep10, control, seeds 8,
9 and 10), after both models paid for their own tables. Only T against B counts. T against order-0 and uniform is
reported below for context and does not count toward the pass.

## Pass rule (pre-registered in TURING_YIELD_PROFILE_V0.md section 9)

| check | value | required | result |
|---|---|---|---|
| pooled T minus fixed-point bound | 2,559,679.825 - 5.847 = 2,559,673.978 bits | > margin 234,602.334 bits | pass |
| seed 8: T with full L(B), L(C) charged, minus bound | 745,936.441 - 5.847 bits | > 0 | pass |
| seed 9 | 707,551.494 - 5.847 bits | > 0 | pass |
| seed 10 | 746,835.890 - 5.847 bits | > 0 | pass |

The held-out seeds were scored once (`turing-yield heldout`; a second run is refused because the receipt exists).
`turing-yield verify` independently refit B and C from the seven fit files (re-hashed against the manifest), re-scored
the three held-out files (re-hashed) and reproduced the gain record digest: VERIFY OK.

## Code lengths (held-out seeds 8-10, every model fit on seeds 1-7)

| model | L(M) bits | L(D\|M) bits | DL bits | T vs B bits | T vs order-0 bits |
|---|---|---|---|---|---|
| uniform (quantized) | 232 | 18,495,764.415 | 18,495,996.415 | -11,011,971.547 | -8,511,573.000 |
| order-0 | 232 | 9,984,191.414 | 9,984,423.414 | -2,500,398.547 | 0 |
| **order-1 = B** | 1,156 | 7,482,868.867 | 7,484,024.867 | 0 | 2,500,398.547 |
| op-only | 2,344 | 8,989,545.097 | 8,991,889.097 | -1,507,864.229 | 992,534.318 |
| op+prev1 | 11,384 | 6,182,570.256 | 6,193,954.256 | 1,290,070.611 | 3,790,469.158 |
| op+prev1..2 | 34,148 | 5,664,088.437 | 5,698,236.437 | 1,785,788.430 | 4,286,186.977 |
| op+prev1..3 | 80,888 | 5,219,788.778 | 5,300,676.778 | 2,183,348.090 | 4,683,746.637 |
| **op+prev1..4 = C** | 180,834 | 4,743,511.043 | 4,924,345.043 | **2,559,679.825** | 5,060,078.372 |
| op+prev1..5 | 367,368 | 4,393,805.069 | 4,761,173.069 | 2,722,851.799 | 5,223,250.346 |
| op+depth+prev1 | 38,040 | 6,002,044.054 | 6,040,084.054 | 1,443,940.813 | 3,944,339.360 |
| op+depth+prev1..2 | 96,082 | 5,499,528.612 | 5,595,610.612 | 1,888,414.255 | 4,388,812.802 |
| op+depth+prev1..3 | 207,147 | 5,067,698.009 | 5,274,845.009 | 2,209,179.858 | 4,709,578.405 |
| op+depth+prev1..4 | 416,091 | 4,599,850.066 | 5,015,941.066 | 2,468,083.801 | 4,968,482.348 |
| op+depth+prev1..5 | 699,888 | 4,285,280.099 | 4,985,168.099 | 2,498,856.769 | 4,999,255.315 |

(The uniform row's "T vs order-0" column is printed as 0 in the receipt by the tool's convention for the first rung;
the value shown here is DL(order-0) - DL(uniform) from the same receipt.)

Receipt consistency checked by hand: the order-1 row has T vs B exactly 0; the C row's DL equals `dl_c_ub` in the gain
record; each row's three per-seed code lengths sum to its L(D|M).

## What this means, and what it does not

- The candidate predicts the outcome of each search step (pruned as equivalent, pruned at the frontier cap, failed,
  partial, improved, submission accepted or rejected) from which operation was tried and the last four outcomes in the
  same crumb. The baseline knows only the last outcome. The extra 0.44 bits per event is structure in how Omega's
  learner search behaves that order-1 does not capture, and it held on three seeds the models never saw.
- Which operation is tried is side information, open to both models by declaration. T does not include the cost of
  describing the search order itself (profile section 12).
- **Post hoc, not counted:** on the held-out seeds op+prev1..5 would have scored higher (+2,722,851.799 T). Validation
  (fit seeds 1-6, score seed 7) chose h = 4, and the pre-registered C stands. With seven fit seeds instead of six, the
  deeper table's cost is spread over more data, so more history still pays. That is a lead for a later profile, not a
  change to this one.
- Held-out T per event (0.4387) is above the leave-one-seed-out range inside seeds 1-7 (0.3788 to 0.3870). The LOSO
  folds fit on six seeds; this run fit on seven, and seeds 8-10 are different sessions. The pass does not depend on
  this: the margin is about one tenth of the observed T.
- T is an ideal code length (sum of -log2 P). No arithmetic coder produced a decodable bitstream of the data.
- No energy was measured. T/J (TY-7) will use the denominator declared in profile section 10.

## Provenance

| item | value |
|---|---|
| scoring code | commit 487de8d (code), 3e492a4 (profile wording fix only, before the run) |
| binary | `build/tests-turing-yield/turing-yield`, SHA-256 `e4edd1f743d06cd3bf711c81a53db3e0c3dcaaabbb9f84d18b65c1b2343e2033`, gcc `-std=c11 -O2` |
| run | 2026-09-29, CPU only, 45 s wall, `~/workspace/.spark-quiet` absent |
| manifest | evidence/TURING_YIELD/trace_manifest_rep10_control.sha256, SHA-256 `6efc04b525af60706bd0f3176013fe234d8e8a52e2554cff3add8a4e16171dca` |
| profile `turing.yprofile.v0` | `f4292df9748e306165f30b250673a0062391054321ea6dbe68855ecaa0fa8be8` |
| split `turing.ysplit.v0` | `63bef76b9af6bd291f71ddd77163f1c4e08ebd5033e967a395bf9434893f7306` |
| B model (`turing.ymodel.v0`) | `59ae9398da24e809d92ab39e7456382a0a4b7e93e203862be6d2ca55450dfd04`, file evidence/TURING_YIELD/ty2_baseline.tym |
| C model | `64a57ba9cf6f8b6e4558751ef2145ae2aa729c5214bfa29373eabfa220a57bc7`, file evidence/TURING_YIELD/ty2_candidate.tym |
| B fit record `turing.yfit.v0` | `dd3ff48686994318ca196c64573c4c9b788f1fc52ca19a438e3792ba05f7cc6f` |
| C fit record | `2abf9eea92ff3dc6eb26c8be34857cee5e6f40c8bbc17975d0df0559cdb67e9d` |
| gain record `turing.yield.v0` | `823cd3a6589218cd5e1550fe8a61ac97ca539e289fd89f3f7397ded99e9e5af6` |
| receipt file SHA-256 | `11df23e1a213f408a74d2c9303643b8e2f29ce2c6a51fe3f59ccc1e168401f66` (evidence/TURING_YIELD/ty2_heldout_receipt.txt) |

Re-check locally (needs the corpus under `~/aien-data/crumbline`):
`make turing-yield && build/tests-turing-yield/turing-yield verify evidence/TURING_YIELD/trace_manifest_rep10_control.sha256 ~/aien-data/crumbline evidence/TURING_YIELD/ty2_heldout_receipt.txt`
