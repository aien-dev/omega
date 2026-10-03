# PRD2: mixture-capable prediction format (EXP-002D Stage A)

Status: PROTOCOL FREEZE for EXP-002D row 28. Written and hashed before any test vector or
result existed (receipt `evidence/EXP-002D/01_protocol_freeze.txt`). PRD1 (`src/turing/ty_prd.*`,
`docs/turing/TURING_QCONT_RECORDS_V0.md`) is not changed by this document; every PRD1 receipt
and every Wave 1 sealed outcome stands as recorded.

## Why

EXP-002D (Brownian Wave 1 hostile cases) is INCOMPLETE because one hostile rule is not
evaluable under the Wave 1 prediction format. The rule is the mixture-weight rule of the
Brownian profile (spec section 5.2: "A finite Gaussian mixture with K <= 8 ... Weights must
sum to 1 within 1e-9. The scorer refuses the prediction otherwise. It does not renormalize").
Hostile row 28 expects FAIL_PROTOCOL for weights summing to 0.9, 1.1, and for a negative
weight. PRD1 carries 24 bytes per record (index, family, loc, scale) and has no weight field,
so in Wave 1 every mixture is refused by family before any weight can be read; the sealed
harness prints "PENDING #28" and the row is not counted. This document defines the smallest
format that makes the weight rule evaluable.

## Wire format PRD2 (little-endian, binary64 IEEE values)

Header, 16 bytes: magic `PRD2`, u32 version = 2, u32 count, u32 first_index.
Then `count` records, record i:

| field | type | rule |
| --- | --- | --- |
| index | u32 | exactly first_index + i |
| family | u32 | 0 Gaussian, 2 Gaussian mixture; 1 (Student-t) refused (decision A2 deferred stands) |
| K | u32 | family 0: exactly 1; family 2: 1 <= K <= 8 (TYQ_KMAX) |
| reserved | u32 | 0 |
| K components | each f64 pi, f64 loc, f64 scale | see below |

Component rules: `pi` finite and `0 < pi <= 1`; `loc` finite; `scale` finite and `> 0`.
Family 0 requires `pi == 1.0` exactly. Family 2 requires `|sum(pi) - 1| <= 1e-9`
(TYQ_NORM_TOL). A file is refused as a whole on the first violation; nothing is renormalized,
clipped or repaired. Trailing bytes, a short file, a PRD1 magic, or a version other than 2
are refused. All refusals are TYQ_FAIL_PROTOCOL; a reason code names the violated rule
(see `src/turing/ty_prd2.h`), so a refusal "by weight" is distinguishable from "by family".

## Scoring a mixture under qint.v1

One point: with `b_j` the qint.v1 Gaussian bits of component j (`ty_qcont_bits`, unchanged,
sd floor applied per component), the mixture bits are

```text
bits = -log2( sum_j pi_j * 2^(-b_j) )
```

computed as `m - log2(sum_j pi_j * 2^(-(b_j - m)))` with `m = min_j b_j`. A point is a floor
hit if any component was raised to sd_min. Micro-bit conversion, overflow guard and ty_add
summation are the PRD1 rules verbatim. Consequence used as a check: a K = 1 mixture with
`pi = 1` scores bit-identically to the Gaussian record, and K identical components score the
same as one, so declaring a mixture earns nothing by itself.

## Preregistered evaluation rule R28-v2 (row 28 MATCH iff all hold)

1. weights 0.5 and 0.4 (sum 0.9): FAIL_PROTOCOL, reason WEIGHT_SUM;
2. weights 0.5 and 0.6 (sum 1.1): FAIL_PROTOCOL, reason WEIGHT_SUM;
3. weights 1.5 and -0.5 (sum 1, one negative): FAIL_PROTOCOL, reason WEIGHT_RANGE;
4. weights 0.5 and 0.5: accepted and scored; the scorer called on case 1 returns
   FAIL_PROTOCOL and no number (no renormalization path exists);
5. K = 1, pi = 1 mixture bits equal the Gaussian bits exactly on the qint.v1 KAT grid;
6. eight identical components with pi = 1/8 score within 1e-12 bits of the single Gaussian.

## Preregistered negative controls (v2 must not make everything pass)

On one synthetic path of 2000 Gaussian increments from a fixed-seed generator inside the
test (no sealed world, no Brownian profile data), with the honest predictor H = Gaussian
(previous value, true increment sd):

- N1 format: PRD1 bytes given to the PRD2 reader are refused (reason MAGIC); PRD2 bytes given
  to the unchanged PRD1 reader are refused;
- N2 constant predictor (loc 0 every point, true sd) as a legal K = 2 mixture: summed bits
  strictly greater than H;
- N3 shuffled predictions (H's locs under a fixed permutation): summed bits strictly greater
  than H;
- N4 padded mixture (H plus a pi = 0.5 component with sd 1e6): summed bits exceed H by at
  least 0.9 bits per point (the honest component carries half the mass, so the cost is at
  least 1 bit per point minus the far component's negligible share).

## What this does and does not close

Passing R28-v2 in omega makes row 28 evaluable and gives the omega-side result. EXP-002D is
closed only when the sealed harness test (`hostile_leak.c` "#28" in the private evaluator
repository) is re-pointed at PRD2 and the EXP-002D development runner is re-run under a new
omega pin, with its own receipt. Until then EXP-002D stays INCOMPLETE; this work changes the
reason from "format cannot carry the rule" to "sealed harness re-run owed".
