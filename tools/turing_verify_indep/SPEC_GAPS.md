# SPEC_GAPS: independent scorer (lane D) for EXP-001

Status (CAL-0 fix round): every gap below is resolved or accepted with a reason in
`calibration/docs/CAL0_REVIEW_RESOLUTION.md`. This file is kept as written, as the record of what the docs lacked.

Spec used: `docs-only-v2/` (supersedes `docs-only/`). Line numbers refer to `docs-only-v2` unless marked "old".
Outcome on the dry-run bundle: all 228 values and all 4 compared header fields equal `scorer_primary.json`
bit-exactly; all 2597 per-crumb ideal lengths and event counts equal `ideal_lengths.json`. Gaps G1-G3 are real
spec holes that the dry run did not expose or that I closed only by inspecting data.

## Insufficient, ambiguous or contradictory places (13)

G1. **CTR1 byte layout is not in the docs.** TURING_YIELD_PROFILE_V0.md:39-43, MODEL_DESCRIPTION_ENCODING.md:60-61
and preregistration/EXP-001.md:6 refer to `ty_ctr1.h` and Rust sources for the field offsets. Those are off limits.
The docs give only the record size (247, PROFILE_V0:39-45), the 215-byte body plus 32-byte chain digest
(BLINDING_PROTOCOL.md:202), the magic, version 1 and the pinned value sets (PROFILE_V0:62-65). I inferred the offsets
from the trace bytes (kind @6, result_class @7, event_index u32 @8, op_index @20, prune @23, verify @24, fit @25) and
checked them against the V0 record and crumb counts, the prune-iff-Pruned rule and the event_index continuity.
Still unknown: where op_origin sits (so the refusal "op_origin != 0" at PROFILE_V0:62 is not implemented), the width
of op_index (bytes 21-22 were always 0), and which of two same-distribution bytes (@29, @179) is depth. No EXP-001 mask
uses depth, so the last one has no effect on the numbers.

G2. **The ideal-length rounding rule contradicts itself at two q values.** EVALUATOR.md:154-155 says
`round(1e6 * -log2(q/65536))`. The profile toml:91 and PROFILE_V0:130-131 say a *truncated* 32-fractional-bit log2,
then rounded to the nearest ub. The two disagree at q = 43481 (591903 vs 591902) and q = 46819 (485194 vs 485193).
The squaring algorithm is only sketched, and the tie direction of "nearest" is not stated (I used half-up on the Q32
value). In the dry run, no realized symbol had either q, so both readings give identical totals (`details.json`
field `events_with_undetermined_q` = 0 everywhere). This can still split the two scorers on sealed data.

G3. **Crumb boundary definition.** EVALUATOR.md:152 says "same crumb id". CODER_SPEC.md:202-203 says the "`first` flag".
The docs define neither a crumb-id field nor a `first` flag in CTR1. I used "event_index restarts at 0". It agrees
with the TPS1 crumb field on every record and with the per-crumb counts in `ideal_lengths.json`.

G4. **Three candidate model files exist only as bundle data** (B1_order0, B2_order1 and M_candidate; the docs hold
only their hashes in candidate_manifest.json). I read them from the bundle's `candidates/` after checking their file
SHA-256, model digest and L(M) against the manifest. I rebuilt B0_uniform and B3_heuristic from the docs alone, and
both match the manifest digests bit-for-bit. M_mem and M_mem_seed1 were checked the same way.

G5. **The bundle directory layout is undocumented.** EVALUATOR.md:114-115 says the scorer reads "the bundle" but
gives none of the following:
- the column formats of `bundle/probability_streams/INDEX` and `bundle/encoded_artifacts/INDEX`;
- the `work/` paths;
- where the TSY1 files live, since no INDEX lists them (`work/symbols/g<g>_j<j>_<C>.tsy` taken from the listing).

I learned all of these from the bundle itself.

G6. **What "profile digest" means.** CODER_SPEC.md:44 says "digest of the frozen lane A profile sidecar", and
EVALUATOR.md 6.1 says "copied from profile.digest". It is unclear whether this means the hash of the sidecar file
or the value written inside it, and the format of `profile.digest` is not given. I used the value inside it, which
equals SHA-256 of `profiles/Turing-profile-v1.0.toml`, and checked that it matches the TPS1 header bytes 28..59.

G7. **The old snapshot left the output contract open** (old EVALUATOR.md:106-114). I had worked around it before v2
arrived:
- the top-level header fields were unspecified (I had written a `profile_digest` field);
- there were no per-file `g<g>.f<j>.<C>.*` keys;
- candidate order was unspecified;
- whether `range_bits` includes the 56-byte header was unstated.

v2 (EVALUATOR.md:112-194) resolves all four. The output now follows v2.

G8. **Per-crumb coded sizes** (EVALUATOR.md:47-48, "each coder on the crumb alone"; the `range_bytes`/`rans_bytes`
per crumb in `ideal_lengths.json`) are not reproduced. That needs my own encoders, and the per-crumb coded files are
not in the bundle. These numbers are outside the S8 key set and were not verified independently.

G9. **Event-index gaps.** PROFILE_V0:45 lists "no event_index gaps" as a check, but PROFILE_V0:62-65 only refuses a
*decrease* without a restart. The docs do not say whether a gap is a refusal. I count gaps (0 here) and do not refuse.

G10. **The TPS1 crumb field** is u32, but POS uses `min(crumb, 65535)` (CODER_SPEC.md:206-207 and section 2). The docs
do not say whether the stored crumb is saturated. There is no effect here (at most 186 crumbs).

G11. **TPS1 `flags` and reserved fields** are "0" (CODER_SPEC.md:40-47), with no statement on whether a reader must
refuse non-zero values. I compare the whole header byte for byte, so any difference is flagged.

G12. **The BLAKE3 chain digest is not checked** (PROFILE_V0:61, BLINDING_PROTOCOL.md:202). Integrity rests on the
SHA-256 dataset manifest. My scorer checks that SHA-256 and does the same.

G13. **The TYM0 small-model files cannot be rebuilt by an outsider** for B1, B2 and M_candidate: the fit procedure
is described, but not to the byte level needed. The score is still independent, because every model byte is
decoded by my own TYM0 reader and every probability is rebuilt from it; only the model files themselves are taken
on trust (hash-pinned).

No gap was found in the bootstrap. It is fully specified in EVALUATOR.md:173-180 and UNCERTAINTY_PROTOCOL.md:33-49:
splitmix64, the rejection limit, common random numbers, and elements 250 and 9749. My intervals equal the primary's
exactly.

## Places I looked at bundle data (not docs)

1. The CTR1 trace bytes (dev seeds 6 and 7), used to infer the record layout (G1).
2. `README.md`, `dataset_manifest.json` (groups, indices, paths, hashes) and `bundle/profile.digest`.
3. `bundle/probability_streams/INDEX` and `bundle/encoded_artifacts/INDEX` (G5), plus the `work/` file names.
4. `candidates/*.tym` (G4).
5. `bundle/scorer_primary.json` and `bundle/ideal_lengths.json`. Before my first run, a `head` of the primary file
   showed its header lines and four values: g1.crumbs, g1.events, g1.B0_uniform.lm_bits and g1.B0_uniform.ideal_ub.
   After that, I changed only the bundle loading and output formatting, never the computation code. All other
   primary values, and all of `ideal_lengths.json` (read after the run), were used only for the comparison.

The `~/aien-data/turing-cal/trial/` directory was never opened. No omega source was read and no production tool
was run.
