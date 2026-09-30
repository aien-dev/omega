# Uncertainty protocol (Turing-profile-v1.0, EXP-001)

Everything here is integer arithmetic with a specified random number generator, so a second implementation
written from this page reproduces every interval exactly (profile `independent_verification_requirements`).

## 1. Unit of resampling: the crumb

Every model's context resets at a crumb start (MODEL_DESCRIPTION_ENCODING.md section 3), so the ideal code length of a
file is exactly the sum of the ideal code lengths of its crumbs. The scorer checks this: sum over crumbs of
L(D_c|M) must equal L(D|M) of the whole file, in micro-bits (ub, 1 bit = 1,000,000 ub). A mismatch is found after scoring started, so it is a terminal S4 FAIL
(refusal code CRUMB_SUM, FAILURE_REPORTING.md section 2), never a void.
Events inside a crumb are never resampled separately. A crumb with zero events is not in the pool.

## 2. Statistic

For group g (g = 1 primary, g = 2 replication), candidate M, baseline B2:

```
d_c   = L(D_c|B2) - L(D_c|M)                       (int64 ub, per crumb, ideal code lengths)
T(M)  = sum_c d_c - (L(M) - L(B2)) * 1000000       (int64 ub)
```

L(M) and L(B2) are the fixed model code lengths in bits (candidate_manifest.json). They are not resampled.
Positive T means M compresses the group's sealed data better than B2 after paying for its own description.

## 3. Pool order

The pool of group g is the list of all nonempty crumbs of the group's 3 sealed control files, ordered first by the
file index j = 0, 1, 2 of the dataset manifest (the seed-commitment order, which is the seed derivation index;
not numeric seed order; DATA_FORMAT.md section 3), then by crumb ordinal inside the file. The pool
has C crumbs. The index i of a crumb in this list is what the resampler draws.

## 4. Random numbers

splitmix64 with state s (uint64, wrapping arithmetic):

```
s = s + 0x9E3779B97F4A7C15
z = s
z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
z = (z ^ (z >> 27)) * 0x94D049BB133111EB
return z ^ (z >> 31)
```

Initial state for group g: s = 0x4558503030315543 + g (profile `bootstrap_seed_base`).

Index draw in [0, C), rejection method (no modulo bias):

```
lim = 2^64 - 1 - (2^64 mod C)        computed as UINT64_MAX - ((UINT64_MAX % C) + 1) % C
repeat r = splitmix64() until r <= lim
index = r mod C
```

## 5. Resampling

For b = 0 .. B-1 with B = 10000 (profile `bootstrap_resamples`): draw C indices in order, and
T_b(M) = sum of d_i over the drawn indices - (L(M) - L(B2)) * 1000000.

**Common random numbers:** one index sequence per group (B x C draws, consumed in order b = 0, 1, ..., each b
drawing C indices) is generated once and reused for every candidate. Differences between candidates in one group
are therefore paired.

## 6. Interval

Sort the B values T_0..T_{B-1} ascending as int64. With lo = floor(B x 25 / 1000) = 250 and hi = B - 1 - lo = 9749
(0-based), the 95% percentile interval is [T_(250), T_(9749)]. The point estimate is T on the unresampled pool.
Reported bits values are ub / 1,000,000, printed with 6 decimals; the decision always uses the int64 ub values.
How the interval decides S6, S7 and S9 (PASS, FAIL or INCONCLUSIVE) is fixed in FAILURE_REPORTING.md section 2 and
prereg section 5a. Also reported, not used in any decision: the number of the B replicates with T_b > 0 for T_ideal
against B2 (uncertainty.json `bootstrap_replicates_T_ideal_vs_B2_gt0`, out of 10000), the protocol's "probability
that delta T > 0 under the bootstrap distribution".

## 7. Coded intervals

For each reference coder (lane B, CODER_SPEC.md) the coded code length of a group under M is the exact size of the
coded output in bits, all headers counted: coded_bits(M) = 8 x the sum of the byte sizes of the group's three
whole-file coded files (three 56-byte headers). Per-crumb coded files are not used here. The coder overhead of M is

```
o(M) = coded_bits(M) * 1000000 - sum_c L(D_c|M)     (ub; whole group, deterministic)
```

The coded statistic is T_coded(M) = T(M) - (o(M) - o(B2)), and its interval is the ideal interval shifted by the
same amount: [T_(250) - (o(M) - o(B2)), T_(9749) - (o(M) - o(B2))]. Coded streams are not resampled, because the
overhead is a whole-stream quantity that does not split into crumbs. The coder envelope (profile
`coder_envelope`, CODER_SPEC.md section 9) is two-sided and applies per coder to every file and every crumb:
|overhead_ub - 448,000,000| <= 64,000,000 + 1,000 x N. A unit outside it fails criterion S4.

## 8. Assumptions

- **A1 (fixed models):** every candidate was frozen before any sealed seed existed; L(M) is a constant, not a
  random variable. Checked by freeze commit ancestry and the overlap audit.
- **A2 (crumb independence):** crumbs are exchangeable within a group, across and within seeds. This ignores
  correlation between crumbs of the same seed (same world). It is the main reason the interval may be too narrow.
- **A3 (spread check):** on dev seeds 1-7 the single-seed crumb bootstrap standard deviation of the per-event gain
  of M_candidate over B2 was 0.00155 bits/event, against a leave-one-seed-out standard deviation of
  0.00316 bits/event (ratio 0.49). A2 is therefore optimistic by about x2.04 in spread. The power simulation
  (calibration/scripts/power_simulation.c) repeats the design with the spread inflated x2.04 and still reaches
  power 1.000 at n = 3 seeds per group for the declared minimum effect. The report states the inflated interval
  next to the protocol interval as a sensitivity result; the verdict uses the protocol interval. The inflated
  interval is computed per side, in integer ub, for T_ideal against B2 of every candidate in each group
  (uncertainty.json key `T_ideal_vs_B2_inflated_x2.04`, written as [point, lo2, hi2]): with d_lo = point - lo and
  d_hi = hi - point, lo2 = point - (204 x d_lo + 50) / 100 and hi2 = point + (204 x d_hi + 50) / 100, where / is
  int64 division truncating toward zero. For d >= 0 this is 2.04 x d rounded half up; the two sides are inflated
  separately, so an asymmetric percentile interval stays asymmetric.

## 9. What is not resampled

Model code lengths, coder overheads, the choice of seeds, the sealed generation itself. The replication group
(g = 2) is the check on between-world variation that the bootstrap does not capture.
