# Turing Yield measurement profile V0 (pre-registered)

Status: PRE-REGISTERED 2026-09-29. This file, the TY-0 audit (TURING_YIELD_CURRENT_STATE.md) and the trace manifest
(evidence/TURING_YIELD/trace_manifest_rep10_control.sha256) are committed and pushed before any scoring of the held-out
seeds 8, 9 and 10. Tuning and the margin below used seeds 1-7 only. For seeds 8-10 nothing has been read except file
size, the 4-byte magic and SHA-256 (for the manifest). The only code path that scores them is `turing-yield heldout`,
which runs once and refuses to run again when its receipt exists. `turing-yield manifest-check` only hashes them;
`turing-yield verify` re-derives an existing held-out receipt and never replaces it.

Digests (SHA-256 with domain prefix over OMG0 bytes, as in src/turing/field.c; record digests, never Omega semantic ids):

| item | value |
|---|---|
| manifest file SHA-256 | `6efc04b525af60706bd0f3176013fe234d8e8a52e2554cff3add8a4e16171dca` |
| profile `turing.yprofile.v0` | `f4292df9748e306165f30b250673a0062391054321ea6dbe68855ecaa0fa8be8` |
| split `turing.ysplit.v0` (fit 1-7, held-out 8-10) | `63bef76b9af6bd291f71ddd77163f1c4e08ebd5033e967a395bf9434893f7306` |

The profile digest is computed by `ty_yprofile_v0` (src/turing/ty_profile.c) plus the manifest digest. Any change to a
field below changes it, and every model and gain record made under V0 is then refused (`TY_E_PROFILE`).

## 1. Definitions

- L(D|M) = sum over t of -log2 P_M(x_t | ctx_t), in bits (never nats).
- DL(M, D) = L(M) + L(D|M).
- T(M; B, D) = DL(B, D) - DL(M, D), computed only on the held-out split.
- **1 T = 1 bit of net held-out description-length gain versus the declared baseline under this profile.**
- Claim form: "Under measurement profile V0, candidate C reduced held-out total description length by X bits relative
  to baseline B, so C scored +X T."

## 2. Dataset and reader

- Corpus: `~/aien-data/crumbline/exp-20260927-rep10/seed-{1..10}/control/trace.ctr` (outside git, 4,992,178,750 bytes,
  20,211,250 records). Per-file SHA-256 and byte size are in the manifest. Control condition only: the learning
  condition changes its bank as library operations are admitted, so it is not stationary (audit section 3).
- Split: **fit = seeds 1-7**, **held-out = seeds 8-10**, whole seeds. The audit found 0 repeated crumb digests across
  the ten control seeds, so no crumb appears on both sides. Control seed s is never paired with learning seed s.
- Hyperparameters were chosen only on **validation seed 7** with models fit on seeds 1-6 (section 7), then every model
  is refit on seeds 1-7 and frozen.
- CTR1 format, checked against the source definition in aien-sovereign-core (read-only reference; Rust is not built):
  `crates/crumbs/src/trace.rs:26` (247 bytes per record), `:406-436` (field order of `TraceRecord::encode_body`),
  `crates/crumbs/src/canon.rs:23-30` (little-endian integers), `trace.rs:460-475` (215-byte body then 32-byte BLAKE3
  chain digest), `crates/crumbs/src/session.rs:196-203` (records appended per crumb, headerless file). The C reader is
  src/turing/ty_ctr1.c; its header lists every byte offset.
- Checks run on seeds 1-7 (every record decoded): size is an exact multiple of 247, record count = size / 247, magic
  `CTR1` and version 1 on every record, no event_index gaps.

| seed | records | nonempty crumbs |
|---|---|---|
| 1 | 2,172,776 | 181 |
| 2 | 1,945,549 | 188 |
| 3 | 2,117,110 | 185 |
| 4 | 2,097,910 | 186 |
| 5 | 1,950,803 | 187 |
| 6 | 2,141,574 | 186 |
| 7 | 1,950,683 | 185 |

  Seeds 8-10: size / 247 = 1,969,906 / 1,884,427 / 1,980,512 with remainder 0, first 4 bytes `CTR1`. Each session has
  188 crumbs in its ledger; crumbs with zero events write nothing to trace.ctr (seed 1: 7 ledger entries share one
  trace_stream_digest, the empty stream, and 188 - 7 = 181). Empty crumbs are invisible to every model and cost nothing.
- The BLAKE3 chain is not verified (no C BLAKE3 in the tree). Integrity is pinned by the per-file SHA-256 manifest,
  which the fit and scoring paths re-hash before reading any file.
- The reader **refuses** a file on any value outside the pinned sets: op_origin != 0, op_index >= 15 on EXPAND, kind
  not 1 or 2, prune > 4, verify > 4, fit > 2, a pruned EXPAND whose result_class is not Pruned, an unpruned EXPAND whose
  result_class is not 2-4, a SUBMIT with prune != 0 or result_class not 5-7, or an event_index that decreases without
  restarting at 0.

## 3. What is coded (x_t) and what is context

x_t is **one outcome symbol per CTR1 record**, alphabet K = 9:

| symbol | meaning | from |
|---|---|---|
| 0-3 | EXPAND pruned: equivalent / over cost / frontier cap / step cap | prune 1-4 (`src/crumbline/cl_search.h` CL_PRUNE_*) |
| 4-6 | EXPAND not pruned: failed / partial / improved | result_class 2-4 |
| 7 | SUBMIT accepted | result_class 5 |
| 8 | SUBMIT rejected | result_class 6 or 7 |

Every CTR1 field, classified:

| field | role | reason |
|---|---|---|
| kind | in x_t | implied by the outcome symbol (EXPAND vs SUBMIT) |
| prune | in x_t | learner's prune decision |
| result_class | in x_t (merged) | carries the evaluation outcome. It is derived from prune when prune != 0 (`trace.rs:253`), so prune and result_class are merged into one symbol and never scored twice. RejectedRobust vs RejectedHidden is decided by `fit` (`trace.rs:291-295`), so both map to one "rejected" symbol |
| op_index | context (side information) | the learner expands each parent with ops 0..14 in order: on seed 1, op_t = (op_(t-1) + 1) mod 15 holds for a fraction 1.0000 (four decimals) of consecutive EXPAND pairs in a crumb. Coding it would score the search loop counter, not prediction. SUBMIT events use op value 15 |
| depth | context (side information) | derived from the parent's depth (`trace.rs:242`), clipped at 7 |
| event_index | segmentation only | equals the event's position (`trace.rs:200`); 0 opens a crumb |
| search_cost | excluded | equals event_index (`trace.rs:223`) |
| improved | excluded | equals result_class Improved (`trace.rs:252`) |
| hidden | excluded | the verdict byte, already in result_class on SUBMIT (`trace.rs:290`) |
| contributed | excluded, never used as context | back-filled from the accepted solution after the crumb ends (`trace.rs:334,337`): a future leak |
| op_origin | excluded | constant 0 in the control condition (library disabled); reader refuses anything else |
| program_steps | excluded | equals depth for base ops (`trace.rs:250`) |
| op_id, state/child/program digests, mismatch, hamming, residual | excluded | computed by the sealed side from states and programs (`trace.rs:241-251,279-288`) |
| parent, child | excluded | node identifiers (search bookkeeping), not outcomes |
| verify, fit, exec_cost, oracle_index | excluded | learner telemetry (verify/fit nonzero on 334 of 2,172,776 seed-1 records); exec_cost and oracle_index are numeric side values outside this symbol stream |
| chain digest | excluded | integrity only |

Side information (not coded, available identically to every model): op feature, clipped depth, crumb boundaries,
crumb ordinal and event_index within the file. D is therefore the outcome stream given the side information; the
baseline and the candidate see the same side information, and only the candidate uses op.

Context resets **per crumb**: previous-outcome features restart at a start-of-crumb value at event_index 0.

## 4. Estimator, smoothing, floor

Fit procedure `ty.fit.kt16.mdl.v0` (src/turing/ty_model.c, `ty_model_fit`):
1. Count n(c, x) over the fit seeds for every context key c (mixed-radix key of the model's features).
2. Default row = the order-0 counts. Every row is **KT smoothed** (add 1/2 to each count, as doubled integers
   2n + 1) and **quantized to 16 bits**: each of the K entries gets 1, the remaining 65536 - K units go by floor of
   (65536 - K)(2n_x + 1) / sum(2n + 1), the leftover by largest remainder (ties to the lower symbol). Every row sums to
   exactly 65536.
3. **Floor**: every probability is at least 1/65536 by construction (at most 16 bits per symbol). No hidden epsilon
   anywhere: the scorer never clamps, and a decoded row with an entry of 0 is refused.
4. A context row is kept only if, on the fit data, it saves more micro-bits against the default row than its own
   code length (key bits + 16(K - 1) bits). An unseen or pruned context uses the default row.

## 5. L(M): the model code

The model that is scored is the decoded model code, and L(M) is that code's exact bit length (src/turing/ty_model.h):
`"TYM0"` 32 | version 8 | K 8 | feature mask 8 | qbits 8 | key bits 8 | row count 32 | default row 16(K - 1) | per row,
keys strictly increasing: key (key bits = ceil(log2 of the key space)) + 16(K - 1). The K-th entry of each row is
implied. So L(M) = 104 + 16(K - 1) + rows x (key bits + 16(K - 1)). A fit-on-train lookup table pays for every row it
stores: a position-keyed memorizer (key = crumb ordinal x event_index, 36 bits) costs 164 bits per stored event, more
than the 3.17 bits a uniform code spends on the event it memorized.

## 6. Fixed point

- Unit: **micro-bits (ub) as int64**, 1 bit = 1,000,000 ub. Model lengths are integer bits (exact).
- -log2(q / 65536) is computed once per q in 1..65536 by an integer-only binary logarithm (`ty_log2_q32`: Q62
  mantissa squaring, 32 fractional bits, error < 2^-31 bits) and rounded to the nearest ub. No libm in the scoring path.
- Summation: file order, exact int64 addition (order-independent because integer), overflow-checked.
- **Error bound**: each scored symbol is within 0.5 ub (rounding) + 0.001 ub (logarithm) of the exact real value, so a
  code length over N symbols is within 0.501 N ub, and T (two code lengths) within 1.002 N ub. For the held-out split
  N = 5,834,845, so |T error| <= 5.85 bits. The smallest T acted on is the margin, 234,602 bits: 40,000 times larger.
  The pass rule subtracts this bound before comparing (`qerr_bound_ub` in the gain record).

## 7. Baseline B and the ladder

B is the **strongest simple baseline**, chosen on validation (fit seeds 1-6, score seed 7) among order-0, order-1 (the
previous outcome) and op-only. Validation result (seed 7, 1,950,683 symbols):

| model | L(M) bits | L(D\|M) bits | DL bits | bits/symbol |
|---|---|---|---|---|
| uniform (quantized) | 232 | 6,183,433.015 | 6,183,665.015 | 3.170 |
| order-0 | 232 | 3,338,148.866 | 3,338,380.866 | 1.711 |
| **order-1 = B** | 1,156 | 2,499,920.598 | 2,501,076.598 | 1.282 |
| op-only | 2,344 | 3,001,493.756 | 3,003,837.756 | 1.540 |

Why not order-0: the order-1 model already captures the search's own rhythm (runs of frontier-cap prunes, equivalence
prunes following each other). A candidate that beats only order-0 could pass on that search artifact alone. B = order-1
forces the candidate to earn its bits from something order-1 does not know. "Beats uniform" never passes. The held-out
receipt reports the full ladder (uniform, order-0, order-1, op-only, every candidate) with T against B and against
order-0.

## 8. Candidate C

Grid (10 models): op + optional clipped depth + the previous 1..h outcomes in the crumb, h = 1..5. Chosen on the same
validation seed by minimum DL:

| candidate | L(M) bits | L(D\|M) bits | DL bits |
|---|---|---|---|
| op+prev1 | 11,248 | 2,061,842.809 | 2,073,090.809 |
| op+prev1..2 | 33,314 | 1,888,934.866 | 1,922,248.866 |
| op+prev1..3 | 79,184 | 1,740,384.295 | 1,819,568.295 |
| **op+prev1..4 = C** | 176,892 | 1,582,420.133 | **1,759,312.133** |
| op+prev1..5 | 357,087 | 1,467,420.463 | 1,824,507.463 |
| op+depth+prev1 | 37,762 | 2,001,867.294 | 2,039,629.294 |
| op+depth+prev1..2 | 93,242 | 1,834,610.991 | 1,927,852.991 |
| op+depth+prev1..3 | 201,347 | 1,690,532.262 | 1,891,879.262 |
| op+depth+prev1..4 | 400,148 | 1,536,330.209 | 1,936,478.209 |
| op+depth+prev1..5 | 669,184 | 1,433,007.426 | 2,102,191.426 |

The optimum is interior in both branches (h = 4), so the grid is not truncating it. The first grid stopped at h = 3
and picked its deepest entry; the grid was widened to h = 5 on the validation seed before anything here was frozen.
No neural nets. C and B are both refit on seeds 1-7 and frozen before scoring.

## 9. TY-2 pass threshold

Seed-to-seed variation, measured inside the fit seeds only: for each s in 1-7, fit B and C on the other six seeds and
score s, charging each model's full L(M) to that seed (`turing-yield loso`).

| left-out seed | symbols | T bits | bits/symbol |
|---|---|---|---|
| 1 | 2,172,776 | 840,914.954 | 0.3870 |
| 2 | 1,945,549 | 737,020.300 | 0.3788 |
| 3 | 2,117,110 | 814,694.249 | 0.3848 |
| 4 | 2,097,910 | 806,675.104 | 0.3845 |
| 5 | 1,950,803 | 739,583.977 | 0.3791 |
| 6 | 2,141,574 | 824,146.164 | 0.3848 |
| 7 | 1,950,683 | 741,764.465 | 0.3803 |

Mean 786,399.888 bits, sample standard deviation sd = 45,149.240 bits. The pooled held-out T sums three seeds, so its
seed-variation scale is sqrt(3) sd.

**Margin = 3 sqrt(3) sd = 234,602.334 bits (`margin_ub` = 234,602,333,141).**

**PASS** if and only if both hold:
1. pooled held-out T (seeds 8-10, each model's L(M) charged once) minus the fixed-point bound exceeds the margin;
2. for each held-out seed separately, with each model's **full** L(M) charged to that seed alone, T minus the
   fixed-point bound is > 0.

Otherwise **FAIL: STOP**, reported plainly and kept. The thresholds do not move after the held-out run.

## 10. Energy denominator for TY-7 (declared now, used later)

T/J = T / E_C, with E_C = idle-subtracted energy of scoring the held-out split with C alone, measured in its own
window. The marginal form (E_C - E_B) is not used: C and B do similar table lookups, so E_C - E_B can be near zero or
negative within meter error (1 mJ step, up to 2 J per window edge; audit section 5) and the ratio becomes unbounded.
Fitting energy is reported separately and never folded into the denominator.

## 11. Refusals the code enforces (TY-1)

- Probabilities from an outside predictor: NaN, Inf, negative, |sum - 1| > tolerance, zero on the realized outcome,
  subnormal or below 2^-62 on the realized outcome (`ty_ubits_f64`).
- A model fit on any held-out file (`TY_E_LEAK`), or whose fit files differ from the split's fit files
  (`TY_E_PROVENANCE`).
- A model or record made under a different profile, quantization, floor, fit rule or encoder version
  (`TY_E_PROFILE`); a baseline other than the declared one (`TY_E_BASELINE`).
- A model code, dataset file or gain record that does not hash to its recorded digest (`TY_E_DIGEST`).
- `ty_gain_verify` refits B and C from the fit files and re-scores the held-out files; any difference fails.

## 12. Limits

- T is an ideal (Shannon) code length. No arithmetic coder is built; a decodable bitstream is future work.
- The chain digests inside CTR1 are not checked in C.
- The corpus can only be regenerated by the Rust crumbs crate; this profile reads it and never rebuilds it.
- Side information is free by declaration. T measures prediction of outcomes given which operation was tried, not the
  cost of describing the search order itself.
