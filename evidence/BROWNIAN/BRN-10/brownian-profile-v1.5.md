# brownian.profile.v1: frozen evaluation profile

PRIVATE (aien-sealed). This file is the evaluator's contract. Its SHA-256 is bound in the profile record (attribute `spec`) and in the commitment file (Section 20). This file never contains its own SHA-256. Words in capitals (MUST, MUST NOT, MAY) are normative. A number in this file is a frozen value; it is copied from the source named beside it and is never recomputed here. Every item that no source decided is resolved in Section 22 (R3-24, R3-26); an implementer fills none (stop condition 19).

## 0. Status, identity, precedence

| item | value |
|---|---|
| profile id | `brownian.profile.v1` |
| kind | companion profile that cites the digest of Turing-profile-v1.0 (Turing-profile-v1.0.sha256 on omega main). The Turing-profile digest value is not written in this file; it is bound in qfreeze key 13 `calibration_extra` (R3-26 G3, G4; Section 22 U8). |
| unit | T in micro-bits: 1 T = 1 bit = 1,000,000 ub. The continuous L(M) rule of Section 9 lives only in this profile, never in Turing-profile-v1.0. |
| modes | `certification` (reference predictors, scored in process) and `discovery` (one candidate BRW-DL program, produced from the candidate-visible packet only). Mixing modes is a failed receipt. |
| candidate-visible slice | `packet.txt` (SHA-256 4383e7db7804088d323ee964d625fc82e9571f9baf0dcabbde8afba1da19e5c7) and `brw_dl_spec.txt` (SHA-256 86bf360cfa208d91338cf22faa0f6022c93dfe2ee1a574e2ee27a800ec6a557f, a byte copy of brownian/design/BRW_DL_SPEC.md), and the producer-visible development manifest `brw.devman.pub.v1` (Section 3, R3-26 G7). |
| private slice | this file, the thresholds sidecar, hidden parameters and priors, the cell table, seed labels, generator source. |
| design evidence | DESIGN_CHECK_R2 at commit 1fd3189 ("Frozen values"); brownian/design/cell_sizes.txt; brownian/src/brw_cells.h. |

Precedence when sources disagree, highest first: R3-x decisions, including R3-24 to R3-28 (a later number wins); plan Appendix R3; Appendix R2; amendments A1 to A12; Appendix OI; the plan body; BROWNIAN_DISCOVERY_SPEC.md (the original text, used as base text only where nothing later overrides it). Every place this file had to choose between sources is listed in Section 22.

Wave 1 scope: EXP-002A, EXP-002B, EXP-002C (certification) and the discovery run (BRN-10). Out of scope: EXP-003 (Wave 2), Student-t predictions, mixture predictions (Section 4), T/J and physical energy, nonzero equilibrium level, multivariate paths, GPU, TPM signing.

Creed: the machine proposes, the experiment measures, reality decides. A score is evidence about predictions. It is never a grant, a promotion or a permission. A positive T grants nothing.

## 1. Numeric conventions

| item | rule |
|---|---|
| float type | binary64 everywhere. No FP32 path. |
| build flags | `-std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -I$(OMEGA_DIR)/src -I$(OMEGA_DIR)/tools -ffp-contract=off -fno-fast-math`, link `-lm`. One exception: brownian/src/brw_protocol.c alone is built with `-std=gnu11 -D_GNU_SOURCE` in place of `-std=c11 -D_POSIX_C_SOURCE=200809L`. Sources are built gnu11 with `_GNU_SOURCE` where the omega sources need it, and both flag strings are bound into the generator digest (R3-17). |
| rounding | `FE_TONEAREST` asserted at scorer start; else FAIL_PROTOCOL. |
| time | binary64 `t`; `h = t_i - t_{i-1}` in binary64; `h > 0` required, else FAIL_PROTOCOL. |
| log base | log2 for every code length. Natural log only internally, converted by dividing by ln 2 before entering any TURING quantity. |
| Gaussian parameters | (location m, scale s = standard deviation). Variance v = s*s internally only. |
| transition convention | the latent step has variance 2 D h (the "2D" convention). The mean-reverting law uses sigma^2 as the short-time variance rate, sigma^2 = 2D. |
| per-point code length | bits in binary64, then `llrint(1e6 * bits)` under FE_TONEAREST (ties to even) to int64 ub. If `1e6 * bits > 9.0e18` before `llrint`, or bits is not finite: FAIL_PROTOCOL. |
| sums | `ty_add` only; overflow is FAIL_PROTOCOL. |
| description length | `lm_ub = llrint(1e6 * L(M)_bits)`; `DL = ty_add(lm_ub, ld_ub)`; `T = ty_gain(DL_B, DL_M)`. `ty_dl` (whole bits) is NOT used. |
| probability floor | none. |
| sd floor | `sd_min = 2^-10`. A finite scale below sd_min is raised to sd_min by the evaluator (never by the program) and counted as a floor hit. Floor hits above 0.1% of the scored points of a run are FAIL_FLOOR. Any hit at all is a failure on a run so short that 0.1% of its scored points is below one point. |
| refusals (FAIL_PROTOCOL) | NaN or Inf in any prediction field; scale <= 0; a missing prediction; an internal variance <= 0. Never renormalize, never default, never cap. |
| resolution | a T difference below max(0.01 bit, the sum over worlds of numeric_bound) is not a result; the sum is reported per cell (R3-26 G12). |
| bit-exactness scope | the reference host (GB10 Spark, pinned toolchain). Published results are re-scored from stored files; regeneration is a check. Other builds are held to the kind C tolerances of Section 17. |

Exact hidden transitions over a gap h from x:

| family | transition | constraints |
|---|---|---|
| D0 | Normal(x, 2 D h) | D > 0 |
| D1 | Normal(x + mu h, 2 D h) | D > 0 |
| D2 | mean `x*exp(-theta h)`; variance `sigma^2 * h * (-expm1(-u)/u)` with `u = 2 theta h` | theta > 0, sigma > 0, equilibrium level m = 0. Never evaluated at theta = 0 (that world is D0 with 2D = sigma^2). Euler steps are forbidden. |

Generator known-answer tests (mandatory, stop condition 14 on any failure):

| KAT | condition | tolerance |
|---|---|---|
| D2 mean | theta = 1, h = 1, x = 1: mean equals exp(-1) | absolute error <= 1e-12 |
| D2 small step (a) | theta*h = 1e-6: the implementation's D2 variance matches a long-double evaluation of sigma^2*(1-exp(-2 theta h))/(2 theta) | within 1e-12 relative |
| D2 small step (b) | theta*h = 1e-9: `|var_D2/var_D0 - 1|` with 2D = sigma^2 | <= 3e-9 |

## 2. Quantizer qint.v1

| item | rule |
|---|---|
| grain | `delta = 2^-20` in the simulator coordinate |
| symbol | `k = floor(x / delta)` as int64; bin(k) = [k*delta, (k+1)*delta), half-open, lower edge inclusive |
| who quantizes | the generator. QBW1 stores k, never raw x. The latent path evolves in unquantized binary64; only observations are quantized. |
| candidate-visible value | `y = (k + 0.5) * delta`, exact in binary64 |
| range | the generator refuses `abs(x) >= 2^31` with FAIL_PROTOCOL |
| probability of a symbol | `p_M(k)` = the integral of the predictive density over bin(k) |
| identity | the profile record binds delta, edge rule, qint.v1, sd_min, families, n_points, split, the rider formula, the declared baselines and the thresholds. Gains under different profile digests are never compared. |

For a Normal prediction with location m and scale s (s already raised to at least sd_min):

```text
z_c  = ((k + 0.5)*delta - m) / s
b    = delta / s                  (b <= 2^-10 always)
a    = |z_c| * b / 2

-ln p = z_c^2/2 + 0.5*ln(2*pi) - ln(b) - lnsinhc(a)
bits  = (-ln p) / ln 2

lnsinhc(a) = a*a/6                                  if a < 1e-4
           = ln(sinh(a)/a)                          if 1e-4 <= a < 1
           = a - ln(2a) + log1p(-exp(-2a))          if a >= 1
```

This closed form has relative error at most b^2/8 (at most 1.2e-7) in the bin probability, is computed in log space (no underflow), and is used for every legal scale. The oracle is scored under the same quantizer. Known answers come from an independent long-double composite-Simpson integrator in the standardized coordinate (4096 panels, peak factor taken out: `-ln p = z_c^2/2 + 0.5*ln(2*pi) - ln(I_z)`, `I_z` the integral of `exp(-u*(2*z_c+u)/2)` over z from z_lo to z_hi with `u = z - z_c`). A generated row whose own relative Simpson error bound exceeds 1e-12 MUST NOT be emitted. Tolerance per point: `|bits_impl - bits_simpson| <= 2e-7 + 1e-12*bits_simpson`. The KAT table covers s = sd_min, 1 and 1e3 and z_c in {0, 1, 5, 30, 2048, 4096}.

Floor accounting: a run is FAIL_FLOOR when floor hits exceed 0.1% of its scored points.

## 3. File formats

All integers and floats are little-endian.

QBW1 (observation file; trajectory digest = SHA-256 of the whole file):

| offset | size | field |
|---|---|---|
| 0 | 4 | magic ASCII `QBW1` |
| 4 | 4 | u32 version = 1 |
| 8 | 8 | u64 count |
| 16 | 16*count | count records of (binary64 t, int64 k) |

The reader refuses (FAIL_PROTOCOL) a wrong magic, version other than 1, size other than 16 + 16*count, a non-finite t, and a t that is not strictly increasing.

PRD1 (predictions, sibling file; digest = SHA-256 of the whole file):

| field | size |
|---|---|
| magic ASCII `PRD1` | 4 |
| u32 version = 1 | 4 |
| records until end of file, one per held-out index: | |
| u32 index | 4 |
| u32 family (0 Normal; 1 Student-t, refused in v1; 2 mixture) | 4 |
| binary64 location | 8 |
| binary64 scale | 8 |
| family 0 extras | none |
| family 2 extras | u32 K, then K triples (binary64 pi_j, location_j, scale_j) |

The reader refuses (FAIL_PROTOCOL): a wrong magic or version, an unknown family, family 1, K outside 1..8, a truncated record, trailing bytes, indices that are not exactly the held-out set in strictly increasing order, and any refusal of Section 1. Under R3-16 C-2 the only family any Wave 1 predictor can emit is family 0 (BRW-DL emits Normal only). The record layout is kept unchanged so the generic reader and record builders stay family-free. A family-2 record in a Wave 1 run is FAIL_PROTOCOL.

Sidecar files: ASCII, one record per line, fields separated by one space, every line ending in LF (including the last), no other bytes. The digest is the SHA-256 of the file bytes.

| sidecar | format id | line grammar | order | stored |
|---|---|---|---|---|
| trajectory manifest | `brw.traj.v1` | `<rep dec> <QBW1 digest hex>` | rep ascending | beside the world record |
| prediction manifest | `brw.prd.v1` | `<rep dec> <PRD1 digest hex>` | rep ascending | beside the gain record |
| development manifest | `brw.devman.v1` | `<cell> <block dec> <world-record digest hex>` | cell by byte order, then block ascending | evidence tree; evaluator-only, the file qfreeze key 7 digests |
| producer-visible development manifest | `brw.devman.pub.v1` | `<sha8 of qbw bytes> <sha256 of qbw bytes>` (no cell, block or label) | sha8 byte order | handed to the producer with the development worlds (R3-26 G7) |
| cell table | `brw.cells.v1` | `<cell> <field>=<value> ...`, fields in brw_cell_spec order, reals as `%.17g` | cell by byte order | printed by `brownian_bench cells` |
| thresholds | `brw.thr.v1` | `<item_id> <field> <value>`, value as written in this file | byte order of the whole line | aien-sealed only until BRN-10 seals |

## 4. Predictor artifacts and the shared language

1. Every predictor, reference or discovered, is a BRW-DL program. BRW-DL is the small stack bytecode defined by `brw_dl_spec.txt` (digest in Section 0). It contains no operator that bundles a model family. Its only output is one Normal (location, then scale). The language definition is authoritative for the byte form, the opcode table, the contexts, the limits, and the computation of k.
2. The canonical bytes of a program are the byte-exact form of the language definition, section 7. The scorer MUST recompute L(M) from those bytes alone (stop condition 24). The program id is the SHA-256 of the canonical bytes.
3. Discovery artifact form (R3-16 C-1, R3-22c): the discovery artifact is a BRW-DL program (data). The evaluator's own interpreter runs it; there is no candidate binary and no static-C candidate. The interpreter run still executes inside a bwrap sandbox (Section 12) as defense in depth. Resolved by R3-24 (Section 22): whether the artifact digest in the freeze record is taken over the BRW-DL text form or over the canonical bytes.
4. Determinism: a program is deterministic by construction. Replay MUST reproduce its prediction files bit for bit (FAIL_DIGEST, STOPPED on a mismatch).
5. Prediction stream: one PRD1 file per world, one record per held-out index, bound by a prediction manifest.
6. Certification artifact: the byte-exact file brownian/src/brw_predict.c at the freeze commit. One freeze record covers all reference opcodes, so every opcode of a cell is scored on the same sealed worlds. The gain record's candidate is that artifact digest and its candidate_opcode is the opcode byte (1 to 7). A discovery artifact has candidate_opcode 255.
7. The evaluator keeps latent state, transition, observation, measurement noise, parameter uncertainty, model uncertainty and random realization distinct. The primary observation equation is Y = X before quantization.
8. The harness line protocol `brw.line.v1` (evaluator lines `B`, `W`, `O`, `Q`, `X` and replies `N`) is, under C-1, the internal protocol between the harness and the interpreter. It is no longer a candidate-facing protocol. Rules of that protocol: ASCII lines, fields separated by one 0x20, ending in one 0x0A, at most 4096 bytes; evaluator reals printed `%.17g` in the C locale; one session per (cell, run); worlds of the cell in rep order; no cell identity is sent; any protocol violation (timeout of 10 s per reply, malformed line, wrong index, output out of turn) is FAIL_PROTOCOL for that run and the run is not retried. A conformance test MUST show that one reference program run through this protocol gives PRD1 files byte-identical to the in-process run.

## 5. Randomness, replay and provenance

| item | rule |
|---|---|
| generator version | `brownian.eval.xoshiro256ss.v1` (inside every world record and gain record) |
| recurrence | xoshiro256** via the existing ClRng and `cl_rng_next` of omega src/crumbline/cl_common.c |
| seeding | the four 64-bit state words are set directly from a 32-byte stream digest as four little-endian u64 words; if all four are 0 set `s[0] = 1`. `cl_rng_seed` is NEVER called (it truncates to 8 bytes). |
| uniform | `u = ((double)(w >> 11) + 0.5) / 2^53`, strictly inside (0, 1). The word `w >> 11 = 2^53 - 1` gives the clamp 1 - 2^-53. |
| normal | `z = acklam_inv(u)`, coefficients below, no refinement step; the body of `norm_inv` is copied from omega src/runtime/rx_costmodel.c at f7f60dd, attributed, into brownian/src/brw_normal.c; that file is never included or linked |
| forbidden samplers | Marsaglia polar and pt_rng_gauss (stream position depends on rejections); the 8-byte crumbline truncation; splitmix or Fisher-Yates history selectors in production code |
| hash | SHA-256 via omega src/sha256.c, all 32 bytes |
| world seed | `world_seed = SHA-256(label || 0x00 || parent_seed || rep_u64_le)` |
| world label | UTF-8 `brownian.v1|<mode>|<cell>|<block>`, mode `dev` or `seal`, cell name as in Section 6, block as unsigned decimal |
| rep | u64 little endian, 0 to W-1 where W is the worlds-per-block of the cell (Section 6) |
| stream | `stream = SHA-256(world_seed || 0x00 || stream_name_utf8)` |
| development parent seed | the 15 ASCII bytes `brownian.dev.v1` |
| sealed parent seed | `SHA-256("brownian.seal.v1" || 0x00 || qfreeze_digest)`, 32 raw bytes |
| null-calibration | a dedicated block from the label `brownian.nullcal.v1` (Section 10). Resolved by R3-24 (Section 22): the exact derivation of the null-calibration worlds' world_seed (the sources give only the string `brownian.nullcal.v1`, one block, 1000000 worlds per cell, and do not say whether it enters the label or the parent seed). |
| generator digest | SHA-256 over: the generator source files sorted by path, each as `path || 0x00 || bytes`, concatenated (relative path labels); then the exact CFLAGS string; then the value of `$(CC)` and the first line of `$(CC) --version` |

Generator source files bound by the generator digest: brownian/src/brw_rng.{h,c}, brw_normal.{h,c}, brw_world.{h,c}, brw_qbw.{h,c}, brw_cells.h, and, from the pinned omega checkout, src/crumbline/cl_common.{h,c} and src/sha256.{h,c}. The digest of record is the one bound at freeze (BRN-5). The digest changes whenever brw_cells.h or a generator source changes (verified by the integrator in R3-23a).

Streams (9, fixed; each an independent 256-bit state; none is a subsequence of another):

| name | used for |
|---|---|
| param | hidden parameters per world (D, theta, drift, affine scale and offset) |
| latent | X_0, irregular gaps, transition innovations |
| meas | measurement noise (hostile cell only; advanced zero times when Y = X) |
| missing | row dropping (hostile) |
| outlier | outlier shifts (hostile) |
| policy-random | random policy (Wave 2) |
| eig-mc | information-gain Monte Carlo (Wave 2); never written into an observation file |
| bootstrap | bootstrap resampling of world indices |
| search | reference-candidate randomness |

Cross-world streams: `analysis_seed = SHA-256("brownian.v1|analysis|<mode>|<cell>|<block>" || 0x00 || parent_seed)`; `bootstrap = SHA-256(analysis_seed || 0x00 || "bootstrap")`; `search` is per world (derived from world_seed like the other world streams); in Wave 2 `eig-mc` and `policy-random` are per world.

Draw order:

1. `param` draws parameters in the order the cell lists them: D first, then any others, then the affine scale and offset for the affine cell. A D2 cell does not draw D.
2. `latent` draws X_0 first, always (the draw is made and discarded in a cell that fixes x0); then, on the irregular grid, all h_i for i = 1..N-1; then one innovation per increment in index order. The Student-t hostile cell draws its 4 extra normals right after each innovation.
3. `meas` draws one normal per index 0..N-1.
4. `missing` draws one uniform per interior index i = 1..N-2 (index 0 and index N-1 are never dropped).
5. `outlier` draws per index one uniform (a hit if < 0.01) and, on a hit, one uniform for the sign (< 0.5 is negative).
6. Sealed = block 0. Development calibration = development blocks 0..19. The sign-stability check = development blocks 0, 1, 2 plus the sealed block.

Bootstrap use: the `bootstrap` stream is re-initialised from its digest for every comparison, so every pair in a cell uses the same paired resamples. For r = 0..B-1 in order, draw n world indices with `cl_rng_below(n)` in order; resample r is the multiset of those n indices. B = 10000.

Acklam coefficients, verbatim (order used by norm_inv):

```text
a = -3.969683028665376e+01,  2.209460984245205e+02,
    -2.759285104469687e+02,  1.383577518672690e+02,
    -3.066479806614716e+01,  2.506628277459239e+00
b = -5.447609879822406e+01,  1.615858368580409e+02,
    -1.556989798598866e+02,  6.680131188771972e+01,
    -1.328068155288572e+01
c = -7.784894002430293e-03, -3.223964580411365e-01,
    -2.400758277161838e+00, -2.549732539343734e+00,
     4.374664141464968e+00,  2.938163982698783e+00
d =  7.784695709041462e-03,  3.224671290700398e-01,
     2.445134137142996e+00,  3.754408661907416e+00
```

Branches: `p < 0.02425`: q = sqrt(-2 log p), lower tail (c, d). `p > 1 - 0.02425`: q = sqrt(-2 log(1-p)), negated (c, d). Otherwise q = p - 0.5, r = q*q, central (a, b).

KAT 1 (state row and raw words from the original text; the z row is amended by R3-17 to the values obtained with -ffp-contract=off). The splitmix64 expansion of seed `0x123456789abcdef0` as `pt_rng_seed` does it (four successive splitmix64 outputs into s[0..3]) is used only in the test file. Then four draws; u0..u3 are the raw 64-bit outputs of `cl_rng_next`; `z_i = acklam_inv(((u_i >> 11) + 0.5) / 2^53)`:

```text
state  161922c645ce50e8 ad760cafa1697b60 3501ff44902ca50d 417cb9a826d831df
u0     e01d6fafc557f1b9   z0   1.152534074247803
u1     bd627ebe4406b404   z1   0.642680143178051
u2     2c23132b578b57db   z2  -0.94468366251373759
u3     2e8b319d4d1f276a   z3  -0.90848344208339182
```

KAT 2 (function-level test of the stream-derivation routine on the literal input bytes; it is NOT in world_seed form and MUST NOT be "corrected"): `stream = SHA-256(bytes("brownian.eval.v1") || 0x00 || bytes("param"))`:

```text
digest  6154810633d05c068bf31387e76dc5734f450f966690d53f557cdc67d9672b8b
words   065cd03306815461 73c56de78713f38b 3fd59066960f454f 8b2b67d967dc7c55
```

A generator that does not reproduce both KATs is not this profile. A KAT 1 mismatch goes to the integrator and is never corrected.

Replay: every published T is recomputed from the frozen artifact digest, the profile digest, the QBW1 files and digests, the PRD1 files and digests, and the freeze record digest (which carries the calibration constants). Regeneration of QBW1 from seeds on the reference host MUST match bit for bit; it is a check, not the source.

## 6. World families and the cell table

Wave 1 families: D0, D1, D2 of Section 1. Prior for D in every D0/D1 cell: `D = exp(U)`, U uniform on [ln 0.05, ln 20], stream `param`, independent per world. `X_0 ~ Normal(0, 1)` from stream `latent` in every D0/D1 cell. D2 cells: sigma = 1, x0 = 0 fixed (the X_0 draw is made and discarded), no D drawn. Regular grid: `t_i = i` (h = 1). Irregular grid: `t_0 = 0`, `t_i = t_{i-1} + h_i`, `h_i = exp(V_i)`, V_i uniform on [ln 0.25, ln 4], stream `latent`. Observation Y = X, then qint.v1. Drift: `mu = s * sqrt(2 D / 255)` (T_full = 255, T_ref = 255 for every s), so `s = mu / sqrt(v_rate / T_full)` with `v_rate = 2 D`.

Per cell: W = worlds per block (sealed block 0 has W worlds, rep 0..W-1; each development block also has W worlds); H = held-out points per world; SPLIT = number of prefix points = 128 (4 for B0-short); `N_POINTS = SPLIT + H`. Prefix = indices [0, SPLIT); held-out = [SPLIT, N_POINTS). The first point is conditioned on and never scored. `n_prefix_incr = SPLIT - 1` (127 primary; 3 for B0-short). Development: 20 blocks (0..19) of W worlds per cell. No sample size is ever chosen after results (R3-6f); every value below comes from DESIGN_CHECK_R2 at commit 1fd3189 (R3-19a) and brownian/design/cell_sizes.txt. All twelve cells pass sizing criteria (a), (b) and (c); B2-emerging keeps the literal criterion (c) with no waiver.

| cell | family | grid | W | H | SPLIT | N_POINTS | parameter | law fixed by |
|---|---|---|---|---|---|---|---|---|
| B0-regular | D0 | regular | 400 | 128 | 128 | 256 | D prior above | EXP-002A |
| B0-irregular | D0 | irregular | 400 | 128 | 128 | 256 | D prior above | EXP-002A |
| B0-short | D0 | regular | 400 | 4 | 4 | 8 | D prior above (S = 4, N = 8) | EXP-002A |
| B1-0 | D1 | regular | 400 | 128 | 128 | 256 | s = 0 | EXP-002B |
| B1-1 | D1 | regular | 400 | 128 | 128 | 256 | s = +1 | EXP-002B |
| B1-3 | D1 | regular | 400 | 256 | 128 | 384 | s = +4.5 | EXP-002B |
| B1-4n | D1 | regular | 400 | 256 | 128 | 384 | s = -5 | EXP-002B |
| B2-noise | D0 | regular | 400 | 512 | 128 | 640 | D prior above | EXP-002C |
| B2-weak | D2 | regular | 200 | 128 | 128 | 256 | theta = 0.01 | EXP-002C |
| B2-clear | D2 | regular | 200 | 128 | 128 | 256 | theta = 0.5 | EXP-002C |
| B2-fast | D2 | regular | 200 | 256 | 128 | 384 | theta = 2 | EXP-002C |
| B2-emerging | D2 | regular | 1600 | 512 | 128 | 640 | theta = 0.05 | EXP-002C |

The plan's draft value "200 worlds, rep 0..199, N_POINTS 256 for every cell" is superseded by R3-19a. The qworld attribute `rep_count` carries the cell's W, and `n_points` and `split` in the qprofile are the primary-cell values `256` and `128`; every cell's own value is in the cell table sidecar (`brw.cells.v1`).

Hostile cells (never sealed; development blocks 0, 1 and 2 only; no qfreeze, no sealed qworld, no official qgain for an H- cell). Fields not listed take the B0-regular value (D0, regular grid, n_points 256, split 128, D from the prior, X_0 ~ Normal(0, 1), h = 1).

| name | family | observation rule |
|---|---|---|
| H-meas | D0 | measurement noise: `k = floor((x + tau*eps)/delta)`, `tau^2 = 2 D h` with h = 1, eps from stream `meas` |
| H-t4 | D0 | Student-t(4) innovation `N0 / sqrt((N1^2+N2^2+N3^2+N4^2)/4)` scaled by `sqrt(D h)` (variance 2 D h), all from `latent`, the 4 extra normals drawn right after each innovation |
| H-outlier | D0 | each latent point shifted by `10*sqrt(2 D h)` with probability 0.01 (hit if the outlier uniform < 0.01; sign uniform < 0.5 is negative), stream `outlier` |
| H-missing | D0 | generated on the full grid, then interior index i = 1..254 dropped with probability 0.1 (stream `missing`); split by ORIGINAL index (prefix [0,128), held-out [128,256)); only kept held-out rows are scored; h is the difference of kept times |
| H-boundary-D | D0 | D fixed at 0.05, exact observation |
| H-boundary-theta | D2 | theta = 0.01, sigma = 1, x0 = 0, exact observation |
| H-affine | D0 | `y = c*x + b` before quantization, `c = exp(U)`, U uniform on [ln 0.1, ln 10], `b = 10*N(0,1)`, both from `param` (scale drawn first, then offset) |

The very-short, irregular, tiny-drift and fast-reversion hostile rows reuse B0-short, B0-irregular, B1-1 and B2-fast and add no cell. The cell name enters the world seed label, so these names are part of profile identity. Hostile cells are not recovery cells; the affine cell is development-classified only and never used to tune a D interval.

Frozen cell constants: the hostile constants above (outlier probability 0.01, outlier multiplier 10, missing probability 0.1, boundary D 0.05, boundary theta 0.01, affine scale range and offset), the D prior, the irregular gap range, T_full = 255, sigma = 1, 20 development blocks, and one null-calibration block of 1000000 worlds per cell.

## 7. Reference programs and baselines

Opcodes 0x01 to 0x07 are labels only. Each is a BRW-DL program from brownian/design/ref_programs (canonical bytes; program id = SHA-256 of the canonical bytes). L(M) bytes and k come from the BRW-DL checker.

| opcode | name | program file | bytes | k | program id (SHA-256) |
|---|---|---|---|---|---|
| 0x01 | IID_NORMAL | iid.brwdl | 23 | 2 | dfd684841e5a7253b9bcf43d6b362dab744eeab5c397404a28f3dbacd5c970b8 |
| 0x02 | LINEAR_TREND | linear_trend.brwdl | 37 | 3 | 1a1101d8ed139739e43ae75c599d3889bd2b959d886ed033c7a7bc80ae4e3be0 |
| 0x03 | CUBIC_TREND | cubic_trend.brwdl | 55 | 5 | 7ab181423e6f3ead84d3bb7aa525c83f07c1bb8ec47faf7cc9ef97aee3574d2f |
| 0x04 | DIFFUSION | diffusion.brwdl | 21 | 1 | 56aa08301dd994895b453d448b86099251a44802df4de779e6486df987599959 |
| 0x05 | DIFFUSION_DRIFT | diffusion_drift.brwdl | 37 | 2 | bd63f40f07d53c94d14365343b7c3fd40707fc0a93bb11e36b1c2f1c93d67dfc |
| 0x06 | MEAN_REVERT | mean_revert_exact.brwdl | 93 | 2 | ba1d71e60ea81b5d681011e68f0a18d90ddba133f3df80c06c9513b4873d0edc |
| 0x07 | LOOKUP_FLOOR | lookup_floor.brwdl | 10 | 0 | ed195471df662d485872de75c2c63ed13e395e2833dea04059491c78b9cb6a66 |
| (oracle) | true transition, true parameters | instrument only | n/a | n/a | not a candidate; scored under qint.v1 |

Canonical bytes (hex) of the reference programs, as printed by `brw_dl info`:

```text
0x01 00106001545633906005548031403256339104808137e0
0x02 02010105508031813310600150563390595831023391650200010aa08232a18332308437e0
0x03 04010105508031813303a1403203a2a1321260015056339059583102339167020001020312a08232a1833230a2843230a38532308637e0
0x04 000c610754553140325133573390065580513237e0
0x05 00185b5a315958313390610b54553180513231403251335733910a558051323081513237e0
0x06 00436103545532610355403233232b8716d9cef7ef3f3a012200007a44333b36345958315733339061185480513234355532314032805132023234383402803233335733911780513234355532818051320232343834320280323337e0
0x07 00000755220000803ae0
```

The regular-grid MEAN_REVERT program (mean_revert_regular.brwdl, 48 bytes, k 2, id 4023417c47e49d7d25f19aa730adae472d9e77eb45838a3520c3700ce8fc7aed) is NOT a reference program of this profile (R3-14). The stored-pairs example program (stored_pairs_example.brwdl, 53 bytes) is a memorization test fixture only.

Exact arithmetic (binding for goldens). S = SPLIT, `n_inc = S - 1`. For j = 1..S-1: `d_j = y_j - y_{j-1}`, `h_j = t_j - t_{j-1}`. Every sum runs in increasing index order in plain binary64 (no compensated summation, no reordering). Every residual variance uses the maximum-likelihood divisor, the number of terms summed (S or n_inc), never n - k. Parameters are fit once on the prefix and never refit on held-out values. Held-out index i is predicted with `h = t_i - t_{i-1}` (t_{i-1} is the previous revealed time; on a missing-rows cell the previous kept row). A computed variance <= 0 or non-finite is FAIL_PROTOCOL. The evaluator, not the program, applies the scale floor sd_min.

| opcode | rule |
|---|---|
| 0x01 | `m = (1/S) sum_{j=0..S-1} y_j`; `v = (1/S) sum (y_j - m)^2`. Predict Normal(m, sqrt(v)). |
| 0x02, 0x03 | `tbar = (1/S) sum t_j`; `half = (t_{S-1} - t_0)/2`; `u(t) = (t - tbar)/half`. Rows (1, u_j) for 0x02 and (1, u_j, u_j^2, u_j^3) for 0x03 (powers by repeated multiplication). Least squares by unpivoted Householder QR: for column c = 0..p-1, x = column c rows c..S-1, `norm = sqrt(sum x_r^2)`, `alpha = -copysign(norm, x_0)`, `v = x` with `v_0 = x_0 - alpha`, apply `H = I - 2 v v^T/(v^T v)` to columns c+1..p-1 and to y in increasing column order; back substitution. Residuals `r_j = y_j - row_j * beta` computed directly; `v = (1/S) sum r_j^2`. Predict Normal(row(u(t_i))*beta, sqrt(v)). |
| 0x04 | `v_rate = (1/n_inc) sum d_j^2/h_j`. Predict Normal(y_{i-1}, sqrt(v_rate*h)). |
| 0x05 | `mu_hat = (y_{S-1} - y_0)/(t_{S-1} - t_0)`; `v_rate = (1/n_inc) sum (d_j - mu_hat*h_j)^2/h_j`. Predict Normal(y_{i-1} + mu_hat*h, sqrt(v_rate*h)). |
| 0x06 | `phi_bar = sum_{j=1..S-1} y_j*y_{j-1} / sum_{j=1..S-1} y_{j-1}^2`, clipped to [0.001, 0.999]; `hbar = (t_{S-1} - t_0)/n_inc`; `theta_hat = -ln(phi_bar)/hbar`. Per step `phi_j = exp(-theta_hat*h_j)`, `q_j = -expm1(-2*theta_hat*h_j)/(2*theta_hat)`, `r_j = y_j - phi_j*y_{j-1}`, `sigma2 = (1/n_inc) sum r_j^2/q_j`. Predict Normal(exp(-theta_hat*h)*y_{i-1}, sqrt(sigma2 * (-expm1(-2*theta_hat*h))/(2*theta_hat))). One program for every grid. |
| 0x07 | Predict Normal(y_{i-1}, sd_min) ("nearest revealed" = y_{i-1}). |

Hostile variants (not opcodes): the h = 1 DIFFUSION variant (B0-irregular; charged 22 bytes, parent bytes + 1, k = 1), variance x4, variance /4, mean shifted by one predictive sd, and the gap-ignoring models. Each is charged its parent opcode's byte count plus one byte (8 extra bits, once per cell) and keeps its parent's k.

Every reference fit uses the maximum-likelihood divisor n, as in the merged brw_predict.c (R3-27d); the R1 independent implementation MUST use divisor n, and the design-check R1 value 2.457 used n-1 and is not a replication target. Reference diffusion estimator (EXP-002A): prefix increments only, `D_hat = (1/(2 n)) sum increment_i^2 / h_i`. Reference drift interval (EXP-002B): `mu_hat = (y_last - y_first)/(t_last - t_first)` on the prefix; `se = sqrt(v_rate_hat/(t_last - t_first))`; interval `mu_hat +/- 1.959963984540054 * se`; `v_rate_hat = (1/n_inc) sum_j (d_j - mu_hat*h_j)^2/h_j` (ML divisor, the same v_rate as 0x05). D interval in D0/D1 cells: `[n*D_hat/c_hi, n*D_hat/c_lo]`, c_lo and c_hi the 0.025 and 0.975 chi-square quantiles with n degrees of freedom (n = number of prefix increments used); in B1 cells `D_hat = v_rate_hat/2` with n_inc degrees of freedom. A fixed-percent recovery rule is forbidden.

Chi-square quantiles: brw_chi2_quantile solves P(n/2, x/2) = p for x by Newton iteration on the regularised lower incomplete gamma in long double (series for x < n/2 + 1, continued fraction otherwise), started from the Wilson-Hilferty approximation, stopped when the relative step is below 1e-15. KATs at n = 3 and n = 127 for p = 0.025 and 0.975 against a long-double bisection reference, tolerance 1e-12 relative.

Declared baselines (frozen in the qprofile):

| benchmark | declared baseline |
|---|---|
| EXP-002A | IID_NORMAL (0x01) |
| EXP-002B | DIFFUSION (0x04) |
| EXP-002C | DIFFUSION_DRIFT (0x05) |
| EXP-003 (Wave 2, out of scope) | fixed heuristic policy's posterior predictive |

A gain with official = 1 whose B is not the declared baseline is FAIL_BASELINE. Ladder comparisons carry official = 0. The ladder (IID_NORMAL, LINEAR_TREND, CUBIC_TREND, DIFFUSION, DIFFUSION_DRIFT, MEAN_REVERT, LOOKUP_FLOOR) is always reported and never substituted.

Oracle and leak ceiling: the oracle uses the true family and true parameters and conditions on the revealed `y = (k+0.5)*delta`, never on the latent x (OI-8). Statistic: per world `diff_w = L(D_w|oracle) - L(D_w|M)`, predictive micro-bits only, no L(M) on either side; FAIL_LEAK if and only if the world-bootstrap 2.5% bound of the sum of diff_w is above 0. The ceiling is evaluated only where the true one-step predictive given revealed y is Gaussian up to the quantizer (all primary Y = X cells and the irregular, short, missing-rows, boundary-parameter and affine cells; affine in the transformed coordinate with the true scale and offset). On the H-t4, H-meas and H-outlier cells the ceiling is INCONCLUSIVE (leak_flag `INCONCLUSIVE`) and FAIL_LEAK is not evaluated; all other gates apply there.

## 8. Scoring and the L(M) rule

Scoring is fit-once on the prefix, not prequential (R3-15: "rule (iii)" of R3-1 names the shared BRW-DL language, not a prequential rule). A predictor is fit on the prefix of each world and scores the held-out points one at a time under qint.v1. Every predictor, reference or discovered, is charged by the same rule (stop 2).

```text
L(M)_bits per cell = 8 * |canonical BRW-DL bytes|              (charged ONCE per cell)
                   + worlds * rider                            (rider charged in EVERY world)
rider (bits, per world) = (k/2) * log2(n_prefix_incr)           per plug-in fitted parameter, k from the checker
n_prefix_incr = SPLIT - 1                                       (127 primary, 3 for B0-short)
```

k is the count fixed by the BRW-DL checker from the program bytes by static y-dependence analysis, never self-declared. Literals embedded in a program are charged as program bytes (a float literal is 9 bytes, 72 bits); a stored pair costs 18 bytes (144 bits). Programs that fit nothing (a Bayesian or lookup program) pay program length and no rider. The scorer MUST recompute L(M) from the canonical bytes alone (stop 24). Rider example at n = 127: about 3.494 bits per parameter.

Cell description length: `DL(M, D) = ty_add(lm_ub, ld_ub)`, with `lm_ub = llrint(1e6 * (8*bytes)) + worlds * llrint(1e6 * rider)` in micro-bits and `ld_ub = sum of the per-point ub`. This departs on purpose from the ty_gain_rec precedent (which charges each model's full L(M) per held file): here only the rider repeats per world. `L(D|M)` is the sum of predictive micro-bits.

Prequential arm (R3-26 G8): the check of Section 11 uses `brw_predict_prequential` (BRW_MODE_PREQUENTIAL, rider 0; no new evaluator code). For held-out index j = 0..H-1 the evaluator calls the same program's fit on the first SPLIT + j points (all observed so far) and scores its one-step prediction for point SPLIT + j; no program byte changes. The fit-once arm is the rule above, unchanged. Structural KAT: on development block 0 of B1-3 the two arms' mean per-world data_ub (predictive data codelength only, rider excluded) differ by more than 1e-6 bit (R3-28e), which proves they are different code paths; the design-check pair 2.436 versus 2.679 bits per world (s = +3) is descriptive only and is not pinned.

Official gain: `T = DL(B, D) - DL(M, D)` in ub (`ty_gain`); positive means M beat B.

Per-world, per-comparison bootstrap statistic: `per_world_diff_ub[w] = L(D_w|B) - L(D_w|M) + llrint(1e6*rider_B) - llrint(1e6*rider_M)` (all in micro-bits: predictive micro-bits plus the per-world riders in micro-bits, R3-26 G5; positive means M is better); the cell term `8*(bytes_B - bytes_M)*1e6` ub is added once to every resampled sum; `T = cell term + sum_w per_world_diff_ub[w]`.

Interval: B = 10000 paired resamples of world indices from stream `bootstrap` (Section 5); sort ascending; the 2.5% bound is element index 249 and the 97.5% bound is element index 9749 (0-based). Studentized intervals are allowed only as a second column. Bootstrap Monte Carlo error: split the 10000 resamples in order into 10 batches of 1000; each batch's 2.5% bound is its element index 24 after sorting; `mc_err = (sd of the 10 batch bounds)/sqrt(10)`, reported in `mc_err_ub`. Beside every mean, report `world sd / sqrt(W)` (the sampling standard error).

Classification (every cell, every comparison): POSITIVE = lower end > 0 and no fail flag; NEGATIVE = upper end < 0; WEAK = interval covers 0 and the point estimate of T is > 0; AMBIGUOUS = interval covers 0 and the point estimate of T is <= 0 (the four labels are disjoint, R3-26 G6). WEAK is reported as "ambiguous, leaning toward the candidate", earning no credit and never a positive claim; STOPPED = harness stop. A zero-width claim on an ambiguous cell fails the cell.

Refused: a conclusion from one seed; a threshold or baseline chosen after sealed scores; fitting on held-out indices; scoring a point with a later point; an in-sample fit reported as T; pooling time steps as independent (the unit is the world).

Numeric bound (deterministic, reported as `numeric_bound_ub`, must be below 0.01 bit per world): at most 0.5 ub per point per model, plus the qint.v1 tolerance for both models, plus both L(M) roundings. For a 128-point world this is about 1.73e-4 bit.

Actual coded lengths (R3-6d, R3-10): the sealed probability streams are also encoded by EXP-001's two reference coders through a family-free TPS1 adapter and reported beside the ideal lengths (`coded_lengths`). The adapter binarises each qint.v1 bin index (sign, Elias-gamma-style magnitude class, then binary refinement), each step's probability from the same Normal CDF, and MUST pass two gates per observation before coders run, each failure being a STOP (R3-25, amending R3-10). Hard gate: |sum of TPS1 step codelengths - exact normalised Normal bin codelength| <= 1e-9 bits. qint gate: |sum of steps - qint.v1 codelength| <= -log2(1 - b^2/8) + 1e-9 bits, with b = delta/sd, sd floored at sd_min (1.72e-7 bits at sd_min), computed per observation from that observation's sd (not a fixed constant), using the Section 2 relative error bound b^2/8 because qint.v1 is an unnormalised midpoint-style bin mass (R3-28a). Lane T tests MUST include sd = sd_min at z_c in {0, 1, 5, 30, 1448, 2048, 4096}. A gate failure on any observation is stop 27. The adapter requires 113-bit long double (aarch64 target) and refuses to build otherwise. Ideal lengths remain the scored quantity; coded lengths are never substituted.

Encoder-lengthening test (OI-7, calibration item): in each primary-cell comparison lengthen ONE side, the candidate side M, by one byte (8 bits, once per cell) with the rider k unchanged, and recompute; the classification of every primary-cell comparison MUST be unchanged, else STOP. Lengthening both sides equally proves nothing and is not the test.

Companion profile: brownian.profile.v1 cites the digest of Turing-profile-v1.0; the digest is bound in qfreeze key 13 `calibration_extra` (R3-26 G3, G4; see Section 22). Uncertainty mirrors v1.0: block bootstrap with the world as the resampling unit, fixed B and seed, 95% interval. Numbers are copied from v1.0 only after its TOML is frozen.

## 9. Description-length rule id and identity

The qprofile attribute `lm_rule` carries the id of the rule in Section 8. Resolved by R3-24 (Section 22): the exact id string (the plan says the accepted spec fixes it and that the constructor takes it as input; the summary form is "8*|canonical BRW-DL bytes| + k/2*log2(n_prefix_incr) per plug-in parameter per world"). Gains under different profile digests are never compared.

## 10. Diagnostics and nulls

Definitions. On held-out points, after the prediction is committed: `z_i = (y_i - m_i)/s_i` with y = (k+0.5)*delta. Six statistics per world (H points): mean of z (null 0 asymptotically); mean of z^2 (null 1); lag-1 autocorrelation (null 0); Spearman correlation of z^2 with t (null 0); slope of z on previous y (null 0); excess kurtosis (null 0). Across worlds the summary is the mean of the per-world statistics, with a percentile bootstrap interval over worlds (paired resamples, Section 5). The matching reference per cell: DIFFUSION on D0 cells, DIFFUSION_DRIFT on D1 cells, MEAN_REVERT on D2 cells.

The asymptotic nulls are NOT used (they are biased at n = 128). The frozen diagnostic nulls are the finite-sample per-cell nulls below (R3-3, R3-19b): each is a per-statistic null center with a [q0.42%, q99.58%] interval of block means (Holm-level 0.05/12 two-sided), simulated from the dedicated null-calibration block (label `brownian.nullcal.v1`, one block of 1000000 worlds per cell, produced by `diag 1000000 400`), never a rate-measurement, development-calibration or sealed world, and frozen into qfreeze through the brw.calib.v1 constants file bound by key 13 `calibration_extra` (R3-26 G3). Resolved by R3-24 (Section 22): the layout of that calibration constants file.

Rejection rule: Holm step-down at familywise 0.05 over the six statistics. Each statistic's two-sided bootstrap p-value against its frozen null is `min(1, 2*min(F_le, F_ge))`, F_le and F_ge the fractions of resampled statistics <= and >= the null value. The familywise rejection is computed per cell over the 20 development blocks (0..19, disjoint from the null-calibration block); the harness MUST stop before seal if the matching reference is rejected in 5 or more of the 20 blocks (stop 13). The frozen null test is the nullcal-quantile test (DESIGN_CHECK_R2 Diagnostics column "nullcal-quantile test rate", the [q0.42%, q99.58%] interval test): the worst-case per-block rate is 0.065 (B1-0) and the worst-case P(stop 13) is 0.0079 (R3-26 G16). A single rejection by a matching reference on a sealed run is a recorded warning and does not overturn T. Resolved by R3-24 (Section 22): the "permitted pre-seal statistic change" the plan says the accepted spec names; none is permitted here, and `calibration_extra` is the binding slot defined in Section 20 (R3-26 G3).

Frozen finite-sample nulls, copied verbatim from the "Frozen values" table of DESIGN_CHECK_R2 (commit 1fd3189). Statistic order in each cell: (mean z; mean z^2; lag-1 ac; Spearman(z^2,t); slope z~y_prev; excess kurt). Format: null center [q0.42%, q99.58%].

| cell | matching ref | (1) mean z | (2) mean z^2 | (3) lag-1 ac | (4) Spearman(z^2,t) | (5) slope z~y_prev | (6) excess kurt |
|---|---|---|---|---|---|---|---|
| B0-regular | DIFFUSION | -0.000003 [-0.01247, 0.01243] | 1.016106 [0.99252, 1.03957] | -0.007941 [-0.01943, 0.00308] | 0.000006 [-0.01225, 0.01127] | -0.041444 [-0.04855, -0.03492] | -0.047156 [-0.10432, 0.00650] |
| B0-irregular | DIFFUSION | 0.000030 [-0.01193, 0.01217] | 1.016016 [0.99328, 1.04010] | -0.007886 [-0.01955, 0.00318] | 0.000111 [-0.01141, 0.01237] | -0.033252 [-0.03958, -0.02761] | -0.046048 [-0.09683, 0.00970] |
| B0-short | DIFFUSION | not run (N=8) | | | | | |
| B1-0 | DIFFUSION_DRIFT | -0.000004 [-0.01610, 0.01722] | 1.032034 [1.00877, 1.05799] | -0.007774 [-0.01949, 0.00343] | 0.000108 [-0.01168, 0.01219] | -0.041577 [-0.04956, -0.03441] | -0.045930 [-0.09674, 0.00781] |
| B1-1 | DIFFUSION_DRIFT | -0.000043 [-0.01669, 0.01672] | 1.032094 [1.00929, 1.05749] | -0.007785 [-0.01962, 0.00301] | 0.000007 [-0.01132, 0.01155] | -0.036539 [-0.04368, -0.03001] | -0.046489 [-0.09941, 0.00877] |
| B1-3 | DIFFUSION_DRIFT | 0.000190 [-0.01410, 0.01415] | 1.032211 [1.01162, 1.05435] | -0.003956 [-0.01285, 0.00464] | -0.000126 [-0.00854, 0.00809] | -0.001354 [-0.00198, -0.00076] | -0.024216 [-0.06221, 0.01783] |
| B1-4n | DIFFUSION_DRIFT | 0.000042 [-0.01394, 0.01401] | 1.032336 [1.01001, 1.05512] | -0.004001 [-0.01187, 0.00464] | -0.000026 [-0.00906, 0.00813] | -0.001069 [-0.00164, -0.00058] | -0.022737 [-0.06016, 0.01692] |
| B2-noise | DIFFUSION | -0.000070 [-0.00598, 0.00578] | 1.015938 [0.99690, 1.03485] | -0.001987 [-0.00786, 0.00398] | 0.000014 [-0.00576, 0.00577] | -0.010517 [-0.01246, -0.00867] | -0.011802 [-0.04005, 0.01637] |
| B2-weak | MEAN_REVERT | -0.000257 [-0.03599, 0.03658] | 1.058483 [1.01528, 1.11334] | 0.007922 [-0.00991, 0.02656] | 0.003848 [-0.01291, 0.02165] | -0.023742 [-0.03242, -0.01557] | -0.047772 [-0.12015, 0.02884] |
| B2-clear | MEAN_REVERT | 0.000050 [-0.01728, 0.01822] | 1.032807 [0.99854, 1.06831] | 0.005485 [-0.01577, 0.02708] | -0.000108 [-0.01739, 0.01640] | -0.015919 [-0.04090, 0.00900] | -0.047443 [-0.12129, 0.03399] |
| B2-fast | MEAN_REVERT | 0.000030 [-0.01163, 0.01143] | 1.031100 [1.00081, 1.06444] | -0.003420 [-0.02261, 0.01521] | 0.000077 [-0.01187, 0.01183] | -0.012522 [-0.05278, 0.02664] | -0.023492 [-0.07887, 0.03215] |
| B2-emerging | MEAN_REVERT | -0.000081 [-0.00472, 0.00412] | 1.039859 [1.02938, 1.05021] | 0.022832 [0.01874, 0.02710] | 0.000195 [-0.00285, 0.00338] | 0.007210 [0.00478, 0.00970] | -0.012865 [-0.02588, 0.00269] |

Hostile diagnostic gates (in addition to T, not a replacement): linear or cubic on D0: lag-1 excludes 0; drift omitted on B1-3: mean of z excludes 0 (or T fails); mean reversion omitted on B2-clear: slope on previous y excludes 0; variance x4: mean z^2 excludes 1; Student-t or outlier cell with a Gaussian model: excess kurtosis excludes 0; measurement-noise cell with naive diffusion: lag-1 excludes 0.

PIT (Gaussian predictions): `u_i = norm_cdf(z_i)` with the erfc form copied (attributed) from omega src/runtime/rx_costmodel.c:232; ten equal bins on [0, 1]; a bin is a warning when its world-bootstrap interval excludes 0.1; bins never overturn T on a primary cell.

Development worlds are never filtered (R3-6c): chance-trend worlds stay in every rate, coverage and bootstrap statistic and are counted separately (results `chance_trend_worlds`).

## 11. Blind protocol: freeze and seal

Sequence (all steps in order; the generator MUST refuse to write a sealed trajectory if the qfreeze is absent, if the artifact bytes do not hash to the qfreeze artifact digest, if the qprofile digest differs, or if a trajectory file for that repetition already exists):

| step | action |
|---|---|
| 1 | Commitment file PROFILE_COMMITMENT.txt (Section 20); build and digest the turing.qprofile.v0 record. |
| 2 | Generate development worlds from parent `brownian.dev.v1`; digest each QBW1; write a qworld (mode `dev`). |
| 3 | Fit, inspect and discard candidate ideas on development worlds only. |
| 4 | Freeze the artifact bytes; artifact digest = SHA-256. |
| 5 | Write turing.qfreeze.v0: artifact digest, qprofile digest, development manifest digest (the digest of the evaluator-only brw.devman.v1), and key 13 `calibration_extra`, which binds the brw.calib.v1 digest and the Turing-profile-v1.0.toml digest (R3-26 G3, G4; Section 20). Created with O_CREAT and O_EXCL, mode 0444, digest-named. Never overwritten. |
| 6 | Only then derive the sealed parent `SHA-256("brownian.seal.v1" || 0x00 || qfreeze_digest)`. |
| 7 | Generate sealed QBW1 files; digest; write the sealed qworld. |
| 8 | Run the candidate (discovery: the evaluator's BRW-DL interpreter in the sandbox; certification: the linked reference programs). |
| 9 | Seal the PRD1 digests and the qgain records. The candidate sees no score; no second try. |

Pre-seal calibration (all MUST hold before step 5; on failure fix the harness and return to step 1, never open the sealed seed):

| item | criterion |
|---|---|
| generator KATs | KAT 1, KAT 2 and both D2 small-step KATs plus the D2 mean KAT pass |
| development only | no sealed file and no qfreeze exists |
| ladder | per-cell ladder tables written for every reference program |
| EXP-002A signs | items 1 to 6 (Section 13) reproduce their signs on development blocks 0, 1 and 2 |
| EXP-002B and EXP-002C signs | match Sections 14 and 15 on development blocks 0, 1 and 2 |
| fit-once versus prequential (`bayes_plugin_check`, R3-22a, R3-26 G15) | fit-once plug-in versus PREQUENTIAL plug-in of the same program (Section 8), run by the BRN-5 calibration tool on each of the 8 cells B0-regular, B0-short, B1-0, B1-1, B1-3, B2-noise, B2-weak, B2-clear: the sign of T agrees on every cell (AGREE), else STOP; do not pick the version that passes. Both values and their relative gap are recorded per cell (descriptive). The attribute name is kept; no Bayesian predictors exist (limitations in Section 21) |
| diagnostic rate | matching reference rejected in fewer than 5 of 20 blocks on every primary cell (stop 13) |
| B2-emerging label | development block 0 of B2-emerging (the full block of W = 1600 worlds, never pooled blocks): POSITIVE if the development interval of T(MEAN_REVERT vs DIFFUSION_DRIFT) is entirely above 0, NEGATIVE if entirely below 0, else AMBIGUOUS (R3-22b). This sign label is three-way, distinct from the four-way qgain classification of Section 8 (R3-28b) |
| encoder lengthening | classification unchanged (Section 8) |
| stability | sign and label stable across development blocks 0, 1, 2 (kind B) |
| coverage | reference coverage in [0.90, 0.99]; stop outside [0.88, 0.995]; the gap is a warning (repeat development, do not edit the interval formula) |

Post-FAIL rule and one attempt: one sealed attempt per (candidate, profile); a second attempt is STOPPED (stop 16). Every qfreeze of a candidate lineage is listed in the results; a ninth predecessor in a lineage is refused (seed shopping). A sealed certification FAIL ends certification under this profile. Any harness or threshold change needs `brownian.profile.v(n+1)`: a new spec file, a new qprofile digest, the old development manifest retired (its digests are never reused under a new profile digest), Grok sign-off and Drake acceptance. At most 3 profiles in Wave 1. Every sealed attempt under every profile is listed in results (`failed_runs[]` or `cells[]`); none is dropped. A harness FAIL at BRN-6 to BRN-8 blocks BRN-9 and BRN-10; a scientific FAIL of BRN-10 is a result.

Known limitation: a dishonest operator could re-freeze to shop seeds. Mitigation: the lineage listing above and one sealed attempt. TPM or P-256 signing is deferred.

Modes: certification (reference programs, linked) and discovery (a BRW-DL candidate from the packet and development worlds). Mixing modes is a failed receipt. A certification pass is never reported as a discovery claim. Parameter recovery and overconfidence items are certification-only (A2): a discovery candidate has no channel for a parameter interval, so BRN-10 is judged by T and its interval, the residual diagnostics and the outcome sentence.

Certification freeze: the artifact is brownian/src/brw_predict.c byte-exact at the freeze commit; one qfreeze covers all reference opcodes and every opcode of a cell is scored against it. Discovery freeze: one frozen artifact covers every primary cell without being told the cell.

## 12. Packet, producer and sandbox

Candidate-visible slice (R3-5): exactly the files `packet.txt` and `brw_dl_spec.txt`, plus the development observation files named `world_<sha8>.qbw` (the first 8 hex of the file digest, so the names carry no cell, block or seed), digested in qfreeze, plus the producer-visible development manifest `brw.devman.pub.v1` (Section 3; lines `<sha8> <sha256>` only, no cell, block or label; R3-26 G7). The packet carries the task statement, the observation encoding (QBW1 and qint.v1), the BRW-DL specification hash, the description-length rule and the opaque baseline id `ref.v1`; it MUST NOT contain a family or process name, a hidden parameter value, a seed label, an acceptance threshold of Sections 13 to 15, a diagnostic null or a cell size (the qint.v1 scoring constants grain 2^-20, sd_min 2^-10 and floor fail rate 1000 ppm are part of the public scoring rule, omega #89 and #90, and are allowed; R3-26 G14) (packet scanner, Section 16). Line 1 is the title; line 2 carries `brw_dl_spec sha256 <hex64>` (R3-23b). The packet omits the public profile id and digest. The producer never sees any private slice item.

Producer isolation (BRN-10, R3-12, R3-21):

1. The producer runs as a separate OS user (for example `brnprod`) with no access to the operator's home, inside `bwrap` with `--share-net` (it needs its model API); its filesystem is its working root plus the toolchain read-only; separate HOME; no git or gh credentials.
2. Its root holds only packet.txt, brw_dl_spec.txt, the development world_<sha8>.qbw files, the brw.devman.pub.v1 manifest, the producer tool binary and its configuration, and an empty /work directory for its output. No omega or aien-sealed checkout, no spec, no evaluator binary, no seeds, no evidence tree. Resolved by R3-24 (Section 22): under C-1 whether any assembler or checker for BRW-DL is placed in the root (the plan text lists a static C toolchain under /toolchain that C-1 makes unnecessary).
3. Claude Code tools are restricted: Read, Write and Edit inside the root only; Bash allowed only for programs under /toolchain and binaries the producer built in /work; WebFetch, WebSearch, gh and git disabled and absent.
4. The full transcript is stored (evidence tree; digest in qfreeze `transcript_digest`) and scanned after the session for any path outside the root and any URL. A hit is STOPPED and the candidate is not scored.
5. Network egress is not blocked at packet level. The limit is the absence of any client in the root, the tool restrictions and the transcript scan. This is a known limitation.
6. Training cutoff (R3-9, R3-21): the producer model's training cutoff MUST predate 2026-09-29, the first public naming of the candidate families; a cutoff that cannot be established from published documentation is treated as not predating. Producer identity (tool, model id, prompt digest, transcript digest) is recorded in qfreeze and results.
7. Default producer: a fresh Claude Opus 5.5 session in that root; Drake may substitute another before BRN-10 starts.

Candidate execution (R3-16 C-1, R3-22c, R3-12): the discovery artifact is a BRW-DL program. The evaluator's own interpreter runs it inside `bwrap` as defense in depth: `prctl(PR_SET_NO_NEW_PRIVS, 1)`, RLIMIT_CPU = 10 s x the cell's held-out points + 10 s, RLIMIT_AS 1 GiB, RLIMIT_FSIZE 0, RLIMIT_NOFILE 16, RLIMIT_CORE 0 (one rule for every interpreter invocation; the fixed 600 s of U2 is withdrawn; the 10 s per reply of Section 4 stays as the per-reply wall limit inside the process limit; R3-26 G2), `execve` of `/usr/bin/bwrap` with a FIXED argv (never built from candidate or operator input) and an empty environment; the argv contains at least `--unshare-all --die-with-parent`, `--proc /proc`, `--dev /dev`. brownian/src/brw_protocol.c is the only file built with the protocol flags (Section 1). Resolved by R3-24 (Section 22): the exact frozen bwrap argv under C-1 (the plan's argv binds a candidate binary onto `/cand`, which no longer exists). The raw unshare plus pivot_root path is dropped everywhere (the reference host sets `kernel.apparmor_restrict_unprivileged_userns = 1`); no sysctl change and no AppArmor profile are needed. Predictions are committed per index before the next value is released; the evaluator assembles PRD1 from the replies and the PRD1 files are the digested artifact. Determinism is checked by replay (stop 23).

Producer and candidate are two processes, never merged; neither sees evaluator state.

Confidentiality (R2 binding, R3-11, R3-21): evaluator-only material is the profile bytes, the thresholds sidecar, hidden parameters and priors, the cell table, seed labels, preregistered signs, generator source and this profile's companion documents. It lives in aien-sealed and never enters a public repository before BRN-10 seals. Family names are public since omega 737f8b3 and are "never given to the producer or candidate". Omega receives before the seal only docs/brownian/PROFILE_COMMITMENT.txt and family-free generic code. After the seal the evaluator tree and the profile bytes are published by PR and the commitment is checked against them (recomputed digests MUST equal the committed ones; a mismatch is stop 25).

## 13. EXP-002A acceptance (certification; cells B0-regular, B0-irregular, B0-short)

Sealed block 0, W = 400 worlds per cell (Section 6). Declared baseline IID_NORMAL. Interval = Section 8 (percentile, B = 10000). "Upper below 0" means the 97.5% bound < 0; "lower above 0" means the 2.5% bound > 0.

| # | item | B0-regular | B0-irregular | B0-short |
|---|---|---|---|---|
| 1 | T of DIFFUSION against IID_NORMAL: lower above 0 | required | required | not required |
| 2 | T of LINEAR_TREND against DIFFUSION: upper below 0 | required | not required | not required |
| 3 | T of CUBIC_TREND against DIFFUSION: upper below 0 | required | not required | not required |
| 4 | T of DIFFUSION_DRIFT against DIFFUSION: upper below 0 | required | not required | required |
| 5 | T of MEAN_REVERT against DIFFUSION: upper below 0 | required | required | required |
| 6 | T of LOOKUP_FLOOR against DIFFUSION: upper below 0 | required | not required | not required |
| 7 | reference D interval coverage in [0.90, 0.99] (stop outside [0.88, 0.995]) | required | required | no requirement |
| 7b | reference mu interval coverage in [0.90, 0.99] (R3-26 G10) | required | not required | no requirement |
| 8 | no FAIL_LEAK, no FAIL_FLOOR, no FAIL_PROTOCOL | required | required | required |
| 9 | h = 1 DIFFUSION variant (22 bytes, k = 1): T against true-gap DIFFUSION has upper interval below 0 | not applicable | required | not applicable |

Reading of the B0-irregular row: items 1, 5, 7, 8 plus the h = 1 variant, per the plan text. B0-regular also requires reference mu interval coverage in [0.90, 0.99] per the F2 band list; this is numbered row 7b, so brw.thr.v1 has one reading (R3-26 G10). B0-short: wide uncertainty is the correct report (relative SE at 3 increments about 0.8).

Pre-seal: development reproduces the sign of items 1 through 6 on development blocks 0, 1 and 2 before the qfreeze is written.

## 14. EXP-002B acceptance (certification; cells B1-0, B1-1, B1-3, B1-4n)

Sealed block 0, W = 400 per cell. Declared baseline DIFFUSION. IID_NORMAL stays on the ladder and is not the baseline. Every coverage and exclusion item is certification-only (A2).

| id | cell | item |
|---|---|---|
| B1-0.1 | B1-0 | T of DIFFUSION_DRIFT against DIFFUSION: upper below 0 |
| B1-0.2 | B1-0 | T of MEAN_REVERT against DIFFUSION: upper below 0 |
| B1-0.3 | B1-0 | coverage of the reference mu interval in [0.90, 0.99] |
| B1-0.4 | B1-0 | fraction of worlds whose reference interval excludes 0 in [0.01, 0.10] (acceptance band and stop band; outside it the harness stops, stop 15) |
| B1-1.1 | B1-1 | T of DIFFUSION_DRIFT against DIFFUSION: upper below 0 (the richer program must lose; design P = 1.000) |
| B1-1.2 | B1-1 | fraction of reference intervals that exclude 0 in [0.05, 0.20] |
| B1-1.3 | B1-1 | certification only: a reference predictor whose mu interval excludes 0 on more than half of the worlds is overconfident and fails the cell, even if its T is negative; not evaluated in discovery mode |
| B1-1.4 | B1-1 | coverage of true mu in [0.90, 0.99] |
| B1-3.1 | B1-3 | T of DIFFUSION_DRIFT against DIFFUSION: lower above 0 |
| B1-3.2 | B1-3 | coverage of true mu in [0.90, 0.99] |
| B1-3.3 | B1-3 | exclusion of 0 on at least 0.40 of worlds |
| B1-3.4 | B1-3 | T of MEAN_REVERT against DIFFUSION_DRIFT: upper below 0 |
| B1-4n.1 | B1-4n | T of DIFFUSION_DRIFT against DIFFUSION: lower above 0 |
| B1-4n.2 | B1-4n | T of MEAN_REVERT against DIFFUSION_DRIFT: upper below 0 |
| B1-4n.3 | B1-4n | on at least 0.70 of worlds the reference mu interval lies entirely below 0 |
| B1-4n.4 | B1-4n | coverage of true mu in [0.90, 0.99] |
| all | all | no FAIL_LEAK, FAIL_FLOOR, FAIL_PROTOCOL |

Design values behind B1-3 (copied): s = +4.5 at W 400, H 256; fit-once expected predictive gain at the earlier s = +3 was 2.436 bits per world (R3-15; descriptive only, not pinned, R3-26 G8-amend). Pre-seal: development reproduces these signs, including the fit-once versus prequential sign check of Section 11.

Occam curve deliverable (R3-6b): the certification T of DIFFUSION_DRIFT against DIFFUSION (dT) as a function of the drift-to-noise ratio `|mu|/sqrt(2D)`, with world-bootstrap intervals at each ratio and an estimate of the crossing region (where dT changes sign, with its interval). The left end of the curve is the cell OCC-s+0.00-H<h> for each H (R3-28c). The curve uses only the OCC-* cells of Section 22 U10 (own labels, own seeds, W = 400), including s = 0; B1-0, B1-1, B1-3 and B1-4n are separate certification cells and are not curve points (R3-26 G11). The s grid is 0 to 6 in steps of 0.5 plus 3.25, 3.5, 3.75 and 4.25, for both signs, with s defined on T_ref = 255. Preregistered crossing points, labelled "per-world crossing, rider only, byte term excluded" (descriptive): s* = 3.471 (H = 128), 2.653 (H = 256), 2.137 (H = 512), 1.828 (H = 1024), the directly solved per-world mean-dT zeros (R3-27b, R3-28; descriptive only). The certification-T crossing at W = 400 including the -128 bit byte term is computed per H by development simulation before the BRN-5 freeze and written into brw.calib.v1 (R3-26 G17). The Occam gating item stays "s = 0 earns no credit": P(drift earns credit | s = 0) = 0 at every H. The curve is reported whether or not it crosses where expected; the crossing-region estimate carries its interval and is never described as a threshold of the world. Resolved by R3-24 (Section 22): W and H (and seed labels) for the extra Occam sweep points beyond the four B1 cells, and the qworld/qgain cell names under which they are recorded.

## 15. EXP-002C acceptance (certification; cells B2-noise, B2-weak, B2-clear, B2-fast, B2-emerging)

Declared baseline DIFFUSION_DRIFT (B2-noise is scored against DIFFUSION per item 1). Positive official T means shrinkage toward 0 beat a constant drift.

| # | cell | item |
|---|---|---|
| 1 | B2-noise | T of MEAN_REVERT against DIFFUSION: upper below 0; and T of DIFFUSION against IID_NORMAL: lower above 0 |
| 2 | B2-weak | T of MEAN_REVERT against DIFFUSION_DRIFT: lower interval NOT above 0 (design P = 0.9993) |
| 3 | B2-clear, B2-fast | T of MEAN_REVERT against DIFFUSION_DRIFT: lower above 0; T of CUBIC_TREND against MEAN_REVERT: upper below 0 |
| 4 | B2-fast | IID_NORMAL reported on the ladder; if T of IID_NORMAL against MEAN_REVERT has lower above 0 on sealed data the cell fails and the harness stops |
| 5 | B2-emerging | the sealed class is not the opposite of the development label: a POSITIVE label with a NEGATIVE sealed class, or the reverse, fails; an AMBIGUOUS class on either side does not fail this item (design P = 1.000). Re-labeling after sealed scores stops the benchmark |
| 6 | all | no positive cell passes on prefix fit alone; the interval is held-out T |
| 7 | all | no FAIL_LEAK, FAIL_FLOOR, FAIL_PROTOCOL |

B2-emerging label: written into qfreeze at freeze (Section 11).

Intervention phase EXP-002C-INT (R3-6a, R3-19c). After the observational scoring, the candidates and reference programs FROZEN on the observational regime (no refit, no new freeze) are scored again, once, on new worlds of B2-emerging (theta = 0.05, W = 1600, H = 512, sigma = 1) started from a set state `x = d * sd_stat` (d in units of the stationary standard deviation). The worlds are generated after the qfreeze from the sealed parent with their own seed labels. Resolved by R3-24 (Section 22): those seed labels (stop 19 forbids inventing them).

| item | d | criterion | design P |
|---|---|---|---|
| I-far (gating) | 8 | MEAN_REVERT beats both DIFFUSION_DRIFT and DIFFUSION (lower above 0 on both) | 0.996 |
| I-near (gating) | 2 | class of MEAN_REVERT versus DIFFUSION is not NEGATIVE | 0.998 |
| I-drift (gating) | 0, 1, 2, 5, 8 | MEAN_REVERT beats DIFFUSION_DRIFT | 1.000 |
| I-control (gating) | 0, 1, 2, 5, 8 | pure-diffusion control at the same displacements never earns MEAN_REVERT credit under the both-beat criterion | at least 0.995 |
| near d = 0 (descriptive) | 0 | class of MEAN_REVERT versus DIFFUSION | 0.971 |
| near d = 1 (descriptive) | 1 | class of MEAN_REVERT versus DIFFUSION | 0.978 |
| moderate d = 5 (descriptive) | 5 | class (would need H = 1024 for a gate) | 0.82 |
| theta = 0.5 (descriptive) | 0, 1, 2, 5, 8 | MEAN_REVERT wins at every d, so no near or far contrast exists; a finding, not a failure | not gated |

F2 scope (R3-27a): the 1% false-fail cap and the achieved 0.00106 (= 4 x 0.000265) cover the 12 observational certification cells only. The gating items above are outside F2; their correct-reference false-fail is stated separately as at most 1 - 0.996*0.998*1*0.995 = 0.0110. Intervention sizes are not changed; the budget is disclosed, not tuned.

Mapping to near, moderate and far: near = d = 2 (gating) with d = 0, 1 descriptive; moderate = d = 5 (descriptive); far = d = 8 (gating). The word "explanatory" MAY be used for a candidate only if the SAME frozen law keeps its preregistered sign of T in every gating item (I-far, I-near, I-drift, I-control); a law that holds in one regime and fails in another is reported as observational only. The phase adds rows to results and to the claim-ceiling matrix (rung "interventional replication") and never changes an observational verdict.

## 16. Packet scanner and allow-list

The scanner runs on every TEXT file handed to the producer. Words and phrases are matched case-insensitively at word boundaries, with hyphen, underscore and space variants: brownian, wiener, diffusion, ornstein, uhlenbeck, langevin, gaussian increment, drift coefficient, mean reversion, mean-reverting, white noise; and the withhold-list words mass, force, energy, momentum, charge, temperature, velocity, acceleration. The symbols G, c, h-bar, k_B, epsilon_0 are matched case-sensitively as whole tokens (a token is a maximal run of letters, digits, underscore and hyphen). Binary files (.qbw) are not text-scanned; each MUST pass the QBW1 reader exactly and is otherwise refused. Development files are named `world_<sha8>.qbw` (sha8 = first 8 lowercase hex characters of the file's SHA-256), listed in byte order of name; no cell name, family, block or label appears in any name. A scanner hit refuses the packet: the evaluator writes verdict FAIL_PROTOCOL on the run record and the lane status becomes STOPPED (stop 5) before BRN-10 starts (R3-26 G9). Beyond the scanner the packet MUST NOT carry any hidden parameter value, seed label, acceptance threshold of Sections 13 to 15, diagnostic null or cell size; the qint.v1 scoring constants (grain 2^-20, sd_min 2^-10, floor fail rate 1000 ppm) are allowed (R3-26 G14); a hit is verified by a grep of packet.txt and brw_dl_spec.txt against this list. The candidate never receives: hidden family, generator source, any seed, hidden parameters, an answer key, evaluator logs, this profile, the plan, development seeds mixed into the sealed stream, future measurements through any channel, cell identity, or the reference predictors' rules.

## 17. Reproducibility

| kind | definition | test |
|---|---|---|
| A, deterministic replay | same code, profile, candidate, seed and data give an identical qgain digest on the reference host | `replay` twice from stored files and once with regeneration; digests byte-identical |
| B, statistical robustness | preregistered repetition blocks give the same classification | at least 3 development blocks plus the sealed block; every cell's classification identical |
| C, implementation robustness | gcc only (clang is absent on the reference host; "clang: no" is preregistered, R3-13): the evaluator built at -O0, -O2 and -O3, each with -ffp-contract=off and with -ffp-contract=fast (six builds); each build re-scores the stored sealed QBW1 and PRD1 files of every cell into its own output directory; the outputs are compared | every per-point code length within 1e-9 relative; T within 1 ub per scored point; classification identical |

The encoder-lengthening test is a calibration item (Section 8), not part of kind C. Bit-exactness is claimed for the reference host and toolchain only. Kind A on the certification path is byte-identical on the reference host; a compile with contract=fast is a non-semantic change tested under kind C.

## 18. Hostile and adversarial tests

Every row MUST produce its expected verdict on the reference build. Verdict codes: FAIL_PROTOCOL, FAIL_FLOOR, FAIL_LEAK, FAIL_DIGEST, FAIL_PROFILE, FAIL_BASELINE, KAT FAIL, STOPPED. Broken variants (BI) live only in test files; a BI test proves rejection of a deliberately broken implementation.

| # | case | rejecting mechanism | expected verdict | BI |
|---|---|---|---|---|
| 1 | wrong diffusion factor of 2 | D2 small-step KAT versus D0; development D coverage outside [0.88, 0.995] | KAT FAIL; STOPPED | BI |
| 2 | wrong dt scaling (h = 1 or h^2) | irregular-grid coverage out of band; known h != 1 KAT | KAT FAIL; STOPPED | BI |
| 3 | packet contains a process label | packet scanner | FAIL_PROTOCOL on the run record; lane STOPPED (stop 5) | |
| 4 | parameter file in the packet | allow-list; scanner; isolated root | FAIL_PROTOCOL; read fails | |
| 5 | candidate embeds a guessed seed | sealed seed is a function of the qfreeze digest; scanner | no advantage; FAIL_LEAK if it beats the oracle | |
| 6 | held-out leakage (y_i released before its prediction is committed) | protocol order check; oracle leak ceiling | FAIL_LEAK | BI |
| 7 | NaN in location, scale or weight | PRD1 validation | FAIL_PROTOCOL | |
| 8 | Inf in any field | PRD1 validation | FAIL_PROTOCOL | |
| 9 | scale <= 0; internal variance <= 0 | PRD1 validation; internal check | FAIL_PROTOCOL | |
| 10 | probability underflow, z_c = 1e6 and 1e150 | log-space qint.v1; if 1e6*bits > 9.0e18 before llrint or bits is not finite: FAIL_PROTOCOL | z_c = 1e6: finite bits, scored; z_c = 1e150: FAIL_PROTOCOL; never capped | |
| 11 | probability-floor gaming (point masses) | sd_min raise and floor count above 0.1% | FAIL_FLOOR | |
| 12 | zero-variance overconfidence (sd below sd_min; sd = 0) | floor raise and count; scale <= 0 refusal | FAIL_FLOOR; FAIL_PROTOCOL for 0 | |
| 13 | infinite-variance gaming (sd = Inf; sd > 1e6 on more than half the points) | Inf refused; huge sd loses log score | FAIL_PROTOCOL; T upper below 0 | |
| 14 | correct mean, wrong variance (variance x4 and /4) | T against true-variance model upper below 0; mean z^2 excludes 1 | cell FAIL for the variant | |
| 15 | wrong mean, correct variance (shift one predictive sd) | T against unshifted upper below 0; mean z diagnostic | cell FAIL for the variant | |
| 16 | deterministic polynomial overfit (LINEAR_TREND, CUBIC_TREND) | T upper below 0 versus DIFFUSION on D0; lag-1 excludes 0 | no positive T | |
| 17 | memorization (LOOKUP_FLOOR; stored development pairs at 144 bits each) | T loses; floor hits | T upper below 0 and/or FAIL_FLOOR | |
| 18 | single-seed lucky result | world-unit bootstrap; at least 3 block stability | refused; STOPPED if signs flip | |
| 19 | tampered candidate (one byte) | artifact digest versus qfreeze | FAIL_DIGEST; sealed generation refused | |
| 20 | tampered profile (one qprofile field or the spec bytes) | qprofile digest versus qfreeze, qworld, qgain | FAIL_PROFILE | |
| 21 | tampered data (one byte of a QBW1 or PRD1 file) | digest versus qworld or qgain | FAIL_DIGEST | |
| 22 | tampered receipt (one byte of a qgain, qfreeze or qworld) | record digest recompute | FAIL_DIGEST | |
| 23 | replay mismatch | `verify` and `replay` | FAIL_DIGEST | |
| 24 | wrong quantizer (trunc for negative x; different delta; midpoint scorer) | symbol KAT (x = -0.5*delta gives k = -1); Simpson KAT; profile digest binding | KAT FAIL; FAIL_PROFILE | BI |
| 25 | quantizer changed after preregistration | qfreeze binds the qprofile digest | FAIL_PROFILE; STOPPED | |
| 26 | model-description cost omitted (lm_ub = 0 or rider dropped) | the scorer computes L(M) from canonical bytes; verify recomputes | FAIL_DIGEST; STOPPED | BI |
| 27 | Euler generator (D2 mean factor 1 - theta h) | D2 mean KAT | KAT FAIL | BI |
| 28 | mixture weights not summing to 1, negative weight | PRD1 validation; no renormalization | FAIL_PROTOCOL | |
| 29 | baseline swapped after results | declared baseline in the qprofile | FAIL_BASELINE | |
| 30 | Student-t prediction submitted (PRD1 family 1) | refusal | FAIL_PROTOCOL | |
| 31 | mode mixing | mode field check | FAIL_PROTOCOL | |
| 32 | second sealed attempt | lineage check; O_EXCL | STOPPED | |
| 33 | sandbox escape (open a path outside the root, open a socket, exec a helper; /proc content is checked too) | bwrap `--unshare-all`; only the interpreter and bound paths visible; no network | every escape attempt fails; the run completes; the verdict is unchanged; if bwrap is absent the test is BLOCKED, never PASS | |
| 34 | nondeterministic candidate | replay compares PRD1 digests | FAIL_DIGEST; STOPPED | |

Hostile cells (Section 6): expected verdicts on development blocks 0, 1 and 2 only.

| case | cell | expected |
|---|---|---|
| very short path | B0-short | items 4 and 5 upper below 0 |
| irregular h | B0-irregular | h = 1 variant T upper below 0 |
| tiny drift | B1-1 | no positive T for drift; intervals cover 0 most of the time |
| large measurement noise | H-meas | naive diffusion: lag-1 excludes 0; no coverage gate on naive D |
| wrong noise family | H-t4 | Gaussian model fails the kurtosis diagnostic (excludes 0); smooth path loses T |
| outliers | H-outlier | kurtosis excludes 0; lookup does not generalize |
| missing rows | H-missing | gap-ignoring model T upper below 0 |
| boundary parameters | H-boundary-D, H-boundary-theta | as B0 and as B2-weak |
| fast reversion | B2-fast | still mean reversion; IID_NORMAL must not beat the reference (EXP-002C item 4) |
| overparameterized, memorization, false deterministic structure | B0-regular and B2-clear development worlds | T upper below 0; lag-1 gate |
| huge or zero uncertainty | any | log score loses; floor count |
| right mean, wrong variance | variance x4 | about 0.46 bits per point extra if too large, about 1.16 if too small; T upper below 0; mean z^2 gate |
| wrong mean, right variance | shift one sd | mean-residual diagnostic; T upper below 0 |
| future leakage | any predictor | FAIL_LEAK on the bootstrap test |
| affine disguise | H-affine | development record only; T only |

Negative scientific results the harness MUST be able to return (they are successes when true): on B0, B1-0, B2-noise "no additional structure justified"; on B1-1, B2-weak and an AMBIGUOUS B2-emerging "uncertainty stays uncertainty"; a longer program with the same predictions does not improve T; a program that fits the prefix and misses the suffix does not improve T. A harness that gives positive T whenever a concept is added is broken.

## 19. Stop conditions

Stop the lane, record BLOCKED (STOPPED for a sealed receipt), state the reason, do not work around. A stopped sealed receipt is not a scientific claim. Any of:

| # | condition |
|---|---|
| 1 | Declared baseline weaker than the one the question is about. |
| 2 | Candidate programs and reference opcodes not charged by the same rider rules. |
| 3 | A threshold edited after sealed scores exist. |
| 4 | Sealed trajectories from Euler-Maruyama. |
| 5 | Candidate packet, process or prompt contains a forbidden string or a hidden parameter (the run record carries verdict FAIL_PROTOCOL and the lane status is STOPPED, R3-26 G9). |
| 6 | Sign of T on a primary cell flips across development repetition blocks. |
| 7 | A positive T disappears when floor hits are counted, or the run is FAIL_FLOOR. |
| 8 | The classification of a primary cell changes when the candidate side alone is lengthened by one byte (Section 8). |
| 9 | On a cell whose preregistered sign is positive, parameter coverage is inside its band while the upper interval end of the true-family opcode's official T is below 0. |
| 10 | The median over worlds of (chi-square interval width / reported interval width) is at least 2 while the claimed coverage is 0.95. |
| 11 | LOOKUP_FLOOR or a cubic trend wins a primary cell. |
| 12 | (Wave 2) action ranking unstable when N and M are doubled; not evaluated in Wave 1. |
| 13 | A matching reference is rejected by the familywise diagnostic in 5 or more of the 20 development blocks for any primary cell (Section 10). |
| 14 | Generator fails either RNG KAT or either D2 KAT. |
| 15 | Coverage outside [0.88, 0.995]; B1-0 exclusion outside [0.01, 0.10]; fit-once versus prequential sign disagreement; B2-fast IID_NORMAL beats MEAN_REVERT. The other coverage bands are within the familywise false-fail budget of sizing rule (b) (at most 0.01/12 per cell, product across the 12 cells at most 1%, achieved 0.00106). |
| 16 | Second sealed attempt for the same (candidate, profile). |
| 17 | BRN-6 or later started before the EXP-001 receipt (calibration/experiments/EXP-001/final_receipt.json on omega main with "EXP_001_COMPRESSION_BRIDGE = PASS") and the frozen Turing profile (calibration/profiles/Turing-profile-v1.0.sha256 with "TURING_PROFILE_V1_FROZEN = PASS") exist. BRN-1 to BRN-5 do not wait. |
| 18 | Any edit to a protected file, or creation of a forbidden component (architecture bypass). |
| 19 | Any need to invent an acceptance number, a stream draw order, a seed label, or anything the original spec lists as not to be improvised. |
| 20 | Blind evaluation impossible: the candidate cannot be run without evaluator-only material reaching it. |
| 21 | TURING needs a competing evidence system (anything other than turing.q*.v0 and the existing Turing records). |
| 22 | Numeric ambiguity: a needed number has two defensible values and no source settles it. |
| 23 | Replay fails: `verify` or `replay` does not reproduce a stored qgain digest, or a candidate's PRD1 differs on replay. |
| 24 | L(M) of a candidate or reference cannot be recomputed from its canonical bytes by the scorer alone. |
| 25 | Evaluator-only material (profile bytes, hidden-world parameters, thresholds, evaluator code, the plan) published or made reachable to a producer before BRN-10 seals; or a post-seal commitment mismatch. |
| 26 | EXP-001 FAIL: sealed runs are blocked and the FAIL is reported as a result, not repaired. |
| 27 | A TPS1 adapter gate failure (hard gate or qint gate, Section 8) on any observation (R3-28f). |

Development worlds are retired when the profile bytes change; their digests are never reused under a new profile digest.

## 20. Records, sidecars and the commitment

Record conventions (same as ty_record.c at omega 1b75aa8): one OMG0 object per record, at most 32 attributes including `record` (added first with the domain name minus ".v0"); every key shorter than 64 bytes; every value at most 512 bytes (longer is a refusal TY_E_FORMAT, never truncated). Renderings: `dec` decimal via PRIu64/PRId64, no padding; `hex` 64 lowercase hex of a SHA-256; `hex40` 40 lowercase hex of a git commit; `text` the literal ASCII shown; `list` tokens separated by one space. An absent digest is 64 "0" characters; an absent number inside a list is -1. Attributes sort by key (strcmp) in the canonical bytes; the tables give insertion order. Digest = `SHA-256(domain || 0x00 || canonical OMG0 bytes)`. Domains: `turing.qprofile.v0`, `turing.qworld.v0`, `turing.qfreeze.v0`, `turing.qgain.v0`.

turing.qprofile.v0 (23 attributes):

| # | key | rendering | value |
|---|---|---|---|
| 1 | record | text | `turing.qprofile` |
| 2 | profile | text | `brownian.profile.v1` |
| 3 | spec | hex | SHA-256 of this file's bytes as accepted |
| 4 | packet | hex | SHA-256 of packet.txt |
| 5 | protocol | text | `brw.line.v1` (the internal harness-to-interpreter protocol under C-1) |
| 6 | quantizer | text | `qint.v1` |
| 7 | delta_log2 | dec | `-20` |
| 8 | edge_rule | text | `floor.halfopen.lower` |
| 9 | sd_min_log2 | dec | `-10` |
| 10 | floor_rule | text | `raise.count.fail_above_ppm.1000` |
| 11 | families | list | `0` (R3-24 U12; Normal only under C-2) |
| 12 | kmax | dec | `1` (R3-24 U12) |
| 13 | n_points | dec | `256` (primary cells; every cell's own value is in the cell table) |
| 14 | split | dec | `128` |
| 15 | cells | hex | digest of the brw.cells.v1 cell table |
| 16 | rider | text | `k*0.5*log2(n_prefix_increments).per_world` |
| 17 | lm_rule | text | `brw.lm.brwdl.v0.fitonce` (R3-24 U1) |
| 18 | baselines | list | `EXP-002A:0x01 EXP-002B:0x04 EXP-002C:0x05` |
| 19 | thresholds | hex | digest of the brw.thr.v1 thresholds sidecar |
| 20 | thresholds_format | text | `brw.thr.v1` |
| 21 | gen_version | text | `brownian.eval.xoshiro256ss.v1` |
| 22 | bootstrap_b | dec | `10000` |
| 23 | unit | text | `micro-bits (1 T = 1 bit = 1000000 ub)` |

turing.qworld.v0 (14 attributes; one per mode, cell, block):

| # | key | rendering | value |
|---|---|---|---|
| 1 | record | text | `turing.qworld` |
| 2 | qprofile | hex | qprofile digest |
| 3 | mode | text | `dev`, `certification` or `discovery` |
| 4 | parent_kind | text | `dev` or `sealed` |
| 5 | qfreeze | hex | qfreeze digest (sealed); 64 zeros (dev) |
| 6 | cell | text | cell name |
| 7 | block | dec | block number |
| 8 | label | text | the world label bytes (Section 5) |
| 9 | gen_version | text | `brownian.eval.xoshiro256ss.v1` |
| 10 | generator | hex | generator digest |
| 11 | rep_first | dec | `0` |
| 12 | rep_count | dec | the cell's W (Section 6; the plan's draft `200` is superseded by R3-19a) |
| 13 | traj_manifest | hex | digest of the brw.traj.v1 sidecar |
| 14 | traj_format | text | `brw.traj.v1` |

Consistency: mode dev if and only if parent_kind dev if and only if qfreeze all-zero.

turing.qfreeze.v0 (21 fixed attributes plus n_lineage lineage entries, at most 29):

| # | key | rendering | value |
|---|---|---|---|
| 1 | record | text | `turing.qfreeze` |
| 2 | mode | text | `certification` or `discovery` |
| 3 | artifact | hex | certification: SHA-256 of brownian/src/brw_predict.c at the freeze commit; discovery: SHA-256 of the candidate artifact (Resolved by R3-24 (Section 22): BRW-DL text or canonical bytes, Section 4) |
| 4 | artifact_kind | text | certification `brw_predict.c`; discovery Resolved by R3-24 (the plan says `candidate.c`, which C-1 makes wrong) |
| 5 | artifact_bin | hex | certification: 64 zeros; discovery Resolved by R3-24 (the plan requires a nonzero digest of the static candidate binary, which C-1 removes) |
| 6 | qprofile | hex | qprofile digest |
| 7 | dev_manifest | hex | digest of the brw.devman.v1 sidecar |
| 8 | b2_emerging_label | text | `POSITIVE`, `NEGATIVE` or `AMBIGUOUS` (three-way preregistered sign label, not the four-way qgain classification; R3-28b) |
| 9 | bayes_plugin_check | text | `AGREE` or `STOP` (fit-once versus prequential, Section 11) |
| 10 | diag_fw_rate_ppm | list | `<cell>:<ppm>` per primary cell, cells in byte order; the frozen null values ride in the brw.calib.v1 constants file bound by qfreeze key 13 |
| 11 | lengthening_test | text | `PASS` or `FAIL` |
| 12 | stability_test | text | `PASS` or `FAIL` |
| 13 | calibration_extra | text | the binding slot (R3-26 G3): `calib=<sha256 of brw.calib.v1 bytes> turing_profile=<sha256 of Turing-profile-v1.0.toml>`, one line, under 512 bytes; swapping any frozen null quantile changes the qfreeze digest; qfreeze has no key for brw.calib.v1 and none may be added |
| 14 | run_commit | hex40 | aien-sealed commit of the harness build |
| 15 | tree_dirty | dec | `0` or `1` (1 is a refusal at seal time) |
| 16 | base_commits | list | grammar below |
| 17 | producer_tool | text | discovery: tool name and version; certification: `none` |
| 18 | producer_model | text | discovery: model id; certification: `none` |
| 19 | prompt_digest | hex | discovery: SHA-256 of the producer prompt; certification: 64 zeros |
| 20 | transcript_digest | hex | discovery: SHA-256 of the stored transcript; certification: 64 zeros |
| 21 | n_lineage | dec | predecessor qfreezes of this lineage, 0..8 |
| 22-29 | lineage.00 to lineage.07 | hex | predecessor digests, oldest first, only i < n_lineage present; a ninth is refused (seed shopping) |

`base_commits` grammar (R3-22d): exactly `A=<hex40> B=<hex40> C=<hex40> D=<hex40> O=<hex40>`: five fields in that order, single ASCII spaces, uppercase key letter, "=", 40 lowercase hex, no trailing space. A and D are aien-sealed lane base commits; B and C are omega lane base commits; O is the pinned omega checkout given by OMEGA_DIR. Certification qfreeze has zero artifact_bin, prompt and transcript digests and producer `none`; discovery requires them (R3-18, artifact_bin per R3-24 U3).

turing.qgain.v0 (29 attributes):

| # | key | rendering | value |
|---|---|---|---|
| 1 | record | text | `turing.qgain` |
| 2 | qprofile | hex | qprofile digest |
| 3 | qworld | hex | qworld digest |
| 4 | qfreeze | hex | the qfreeze that sealed the qworld |
| 5 | mode | text | `certification` or `discovery` (equals the qworld's) |
| 6 | cell | text | cell name |
| 7 | gen_version | text | `brownian.eval.xoshiro256ss.v1` |
| 8 | baseline | list | `<opcode dec> <artifact hex>`: B, an opcode of brw_predict.c identified by that file's digest |
| 9 | candidate | hex | M: the qfreeze artifact digest |
| 10 | candidate_opcode | dec | 1..7 for a reference opcode; 255 for a discovery artifact |
| 11 | prd_b | hex | PRD1 manifest digest for B |
| 12 | prd_m | hex | PRD1 manifest digest for M |
| 13 | official | dec | 1 = against the declared baseline; 0 = ladder comparison |
| 14 | ladder_item | text | acceptance item id when official = 0 (for example `B1-3.4`); `-` when official = 1 |
| 15 | lengths | list | `L(B) L(D|B) L(M) L(D|M) DL(B,D) DL(M,D)`, six int64 ub, cell totals |
| 16 | t_ub | dec | T = DL(B,D) - DL(M,D) via ty_sub |
| 17 | t_lo_ub | dec | bootstrap 2.5% bound |
| 18 | t_hi_ub | dec | bootstrap 97.5% bound (t_lo_ub <= t_hi_ub) |
| 19 | numeric_bound_ub | dec | deterministic numeric bound |
| 20 | mc_err_ub | dec | bootstrap Monte Carlo error of the lower bound |
| 21 | counts | list | `B worlds heldout_per_world scored_points` |
| 22 | floor_hits | dec | floor hits of M |
| 23 | protocol_failures | dec | 0 for any scored qgain unless STOPPED |
| 24 | leak_flag | text | `0`, `1` or `INCONCLUSIVE` |
| 25 | coverage | list | `d_ppm mu_ppm excl0_ppm`, -1 where the cell has none |
| 26 | diag_warnings | list | rejected diagnostic ids after Holm, or `-` |
| 27 | classification | text | `POSITIVE`, `NEGATIVE`, `AMBIGUOUS`, `WEAK` or `STOPPED` |
| 28 | status | text | `PASS`, a FAIL_* code, or `STOPPED` |
| 29 | unit | text | `micro-bits (1 T = 1 bit = 1000000 ub)` |

Which gains exist: per cell, one official qgain (official = 1, B = the declared baseline) for every ladder model other than the baseline, plus one qgain with official = 0 for every acceptance-item pair whose B is not the declared baseline (for example CUBIC_TREND against MEAN_REVERT). In certification mode every opcode is scored against the single certification qfreeze, so all pairs of a cell share one sealed dataset. Gain, world and freeze MUST name the same qprofile digest, mode, cell, gen_version and candidate (TY_E_PROFILE otherwise).

Sidecars: Section 3. The thresholds sidecar `brw.thr.v1` holds the acceptance and stop values of Sections 13 to 15 and 19 exactly as written there; its digest is the qprofile `thresholds` attribute; no acceptance value is compiled into public code. `brownian_bench` reads the sidecar from a path given on its command line and refuses it if its digest differs.

Attribute tables for qprofile, qworld, qfreeze and qgain are the only ones this profile defines; a record builder never adds, drops, renames or re-renders an attribute.

PROFILE_COMMITMENT.txt (docs/brownian/PROFILE_COMMITMENT.txt in omega; R3-22e): exactly five LF-terminated lines, in order:

```text
brownian.profile.v1 spec sha256 <hex64>
brownian.profile.v1 evaluator-tree sha256 <hex64>
brownian.profile.v1 slice-candidate sha256 <hex64>
brownian.profile.v1 slice-brwdl sha256 <hex64>
brownian.profile.v1 slice-private sha256 <hex64>
```

Line 1: SHA-256 of this file. Line 2: SHA-256 of the bytes of `git archive --format=tar <commit> brownian/src brownian/tests brownian/Makefile` from aien-sealed at the freeze commit. Line 3: SHA-256 of packet.txt. Line 4: SHA-256 of brw_dl_spec.txt. Line 5: SHA-256 of the thresholds sidecar.

## 21. Results, claims and known limitations

Two outputs, both required: BROWNIAN_DISCOVERY_RESULTS.md (human) and results.json (machine-readable, printed by `brownian_bench` from the q-records and stored files, never hand-written; compact JSON, keys in byte order, integers decimal, reals `%.17g`; its SHA-256 is recorded in the results document). Before the seal both live in aien-sealed brownian/evidence/ (R3-16 C-3); after the seal they are published to omega docs/brownian/ and evidence/BROWNIAN/ by PR. results.json carries: commits, profile ids and digests, generator version, digest and flags, quantizer, RNG KAT status, seeds and cell labels, per-cell qworld and QBW1 digests, per-gain fields (digest, mode, candidate, candidate_opcode, baseline, official, lengths, rider {k_b, k_m, n}, counts, t_ub, t_lo_ub, t_hi_ub, leak_flag, mc_err_ub, numeric_bound_ub, floor_hits, classification, status, outcome sentence, coded_lengths), coverage in ppm (certification only), diagnostics, calibration report, negative tests (every row of Section 18), failed runs (every FAIL, STOPPED or INCOMPLETE run and every sealed attempt under every profile), replay commands, the Occam curve, the intervention rows, chance-trend world counts, the claim-ceiling matrix and the known limitations.

Claim template: "Under brownian.profile.v1, candidate C against baseline B on cell <name>, mode <mode>, T = <bits> [<low>, <high>]." Allowed outcome sentences:

| sentence | rule |
|---|---|
| found structure | lower end above 0 and no fail flag |
| no additional structure | upper end below 0 |
| uncertain (AMBIGUOUS) | interval covers 0 and the point estimate of T is at most 0 |
| weak evidence (WEAK) | interval covers 0 and the point estimate of T is above 0; "ambiguous, leaning toward the candidate"; no credit; never a positive claim |
| hypothesis failed | the preregistered expected sign of Sections 13 to 15 is not obtained on sealed data; a valid result |

Fail flags take their own sentence: protocol failure; leakage; floor exploitation; STOPPED. Prohibited sentence: never write "AIEN discovered Brownian motion" or any sentence naming a hidden law as discovered. A positive T grants nothing. A certification pass is not a discovery claim.

Claim-ceiling matrix (R3-6e): a claim is limited by the highest rung that PASSED under the frozen profile; a downstream success never erases an upstream failure; every failed rung is listed. Rungs: CAL-0 (evaluator calibrated); EXP-001 (compression bridge, R3-7 receipt); EXP-002A (scorer certifies D0); EXP-002B (drift structure, with the Occam curve); EXP-002C (emerging-structure cells and the intervention phase); EXP-002D (hostile and adversarial cells give expected verdicts); interventional replication; EXP-003 (discovery-mode result, BRN-10); H3 replay (stored scores replay byte for byte) and H3 statistical; H4 (GPU rows, run only when no chip test is running; otherwise INCOMPLETE, never skipped); H5 (independent reproduction of the harness); sensitivity; independent replication ("independent implementation (in-house), not external lab", R3-8). A claim may not name a rung it did not pass.

BRN-10 verdicts: PASS = protocol complete and every primary cell classified (any of the five outcome sentences is a valid result); INCOMPLETE = some cell unclassified; FAIL = protocol breach (a stop condition). Verdict vocabulary: PASS, FAIL, BLOCKED, INCOMPLETE only.

Known limitations (listed in results.json and the results document): a human operator can still see development data before freezing (re-freeze limitation); bit-exactness only on the reference host and toolchain; the Student-t predictive family and the mixture family are deferred (A2, A12, C-2), and the qprofile lists families `0` and kmax `1` (R3-24 U12, R3-26 G1); T/J is not reported (energy attribution absent); the EXP-001 instrument is controlled by CI and re-verify while the paper's own criteria are unchecked (A10); producer network egress is outside the evaluator; the "independent laboratory" step is an in-house independent implementation by a different model family; H4 GPU rows may be INCOMPLETE; family names are public since omega 737f8b3 (R3-21), so the controls are producer isolation and the training cutoff; every PENDING item that BRN-0 resolved, with where it was resolved; (i) a defect shared by both arms of the fit-once versus prequential check (quantizer, unit conversion, log score, rider) is not detected by that check and is covered by the KATs and the R1 independent implementation; (ii) no Bayesian marginal-likelihood arm exists because no prior is frozen and inventing one is forbidden (stop 19) (R3-26 G15). F2 scope (R3-27a): the 1% false-fail cap and the achieved 0.00106 cover the 12 observational certification cells only; the EXP-002C intervention gating items are outside F2, with a correct-reference false-fail of at most 0.0110 stated separately (Section 15).

Uncertainty envelope (one row each, never merged into one confidence number): T by the world-bootstrap interval; T numeric by the deterministic bound; bootstrap Monte Carlo error by batch means; D and mu by interval plus empirical coverage band; calibration by diagnostic intervals, Holm and PIT bins; multi-seed by at least 3 development blocks plus sealed; measurement noise none on primary cells (Y = X); physical instrumentation not applicable; run-to-run variance zero by construction on the reference host and bounded by kind C across builds.

## 22. Resolved items (R3-24) and source choices

Every item that no source decided is resolved by integrator decision R3-24 (binding, restated verbatim):

R3-24 (BRN-0 resolution of spec U1-U12, 2026-09-29, integrator on Drake's behalf):
U1 lm_rule id = `brw.lm.brwdl.v0.fitonce` (8*canonical bytes once per cell + k/2*log2(n_prefix_incr) per world; fit-once).
U2 Candidate interpreter = brownian/src/brw_dl.c built with -DBRW_DL_CLI -static (gcc, BRW_CFLAGS). Frozen bwrap argv: `/usr/bin/bwrap --unshare-all --die-with-parent --new-session --clearenv --ro-bind <interp> /interp --ro-bind <prog> /prog --proc /proc --dev /dev --chdir / /interp --line /prog`, where only the two paths are substituted by the evaluator from its own frozen files (never from candidate or operator text); Lane D3 may rename the `--line` subcommand to what brw_dl's CLI actually exposes, then the argv text is frozen in the harness source and bound via the evaluator-tree digest. Limits set by the parent before execve (as amended by R3-26 G2, one rule for every interpreter invocation): RLIMIT_CPU = 10 s x held-out points of the cell + 10 s, RLIMIT_AS 1 GiB, RLIMIT_FSIZE 0, RLIMIT_NOFILE 16, RLIMIT_CORE 0, PR_SET_NO_NEW_PRIVS; the fixed 600 s is withdrawn.
U3 Discovery qfreeze: artifact_kind `brwdl`; artifact_bin = SHA-256 of the static interpreter binary used to run the candidate (nonzero, satisfies R3-18).
U4 Discovery artifact digest = SHA-256 of the canonical BRW-DL bytes (the bytes charged by L(M)); the producer's source text is stored beside it and its digest recorded as supplementary, not as the artifact.
U5 No new seed labels for EXP-002C-INT: its worlds are extra cells (INT-d0, INT-d1, INT-d2, INT-d5, INT-d8 and CTL-d0 .. CTL-d8) in the same world_seed scheme under `brownian.dev.v1` (development) and the sealed parent (sealed).
U6 Calibration constants file brw.calib.v1: ASCII, LF lines `<cell> <stat> <center %.17g> <lo %.17g> <hi %.17g>`, cells in byte order, stats in the fixed order (mean_z, mean_z2, ac1, spearman_z2_t, slope_z_yprev, exkurt); preceded by one header line `brw.calib.v1`; after the null lines it may carry lines `occ_cross <H dec> <s %.17g>` (certification-T crossing at W = 400, R3-26 G17), H ascending, and nothing else (R3-28d); its SHA-256 is bound in qfreeze key 13 `calibration_extra` (R3-26 G3).
U7 Permitted pre-seal statistic change: none. Any change to a statistic, band or threshold after BRN-0 is a new profile id.
U8 The spec names Turing-profile-v1.0 by file name only; its digest value is not in the spec (so the spec can freeze now). The value is bound in qfreeze key 13 `calibration_extra`, not qprofile (R3-26 G3, G4); it comes from the CAL-0 receipt on omega main before BRN-5; BRN-6+ wait for it anyway (R3-7).
U9 Producer root: packet.txt, brw_dl_spec.txt, development world_<sha8>.qbw files + the producer-visible dev manifest brw.devman.pub.v1 (R3-26 G7), a static `brw_dl` binary (same source as the evaluator's interpreter, assembler + checker + runner for local testing), the producer tool binary and config, empty /work. Nothing else.
U10 Occam sweep (EXP-002B): s in {0, +-0.5, +-1, +-1.5, +-2, +-2.5, +-3, +-3.25, +-3.5, +-3.75, +-4, +-4.25, +-4.5, +-5, +-5.5, +-6} (the DESIGN_CHECK_R2 grid), H in {128, 256, 512, 1024}, W = 400, cells named OCC-s<+x.xx>-H<h>, same world_seed scheme, no new labels. Gating: at s = 0, drift earns no credit at every H (P = 1.000 in DESIGN_CHECK_R2). The crossing points (R3-19d) are descriptive predictions compared with the sealed curve.
U11 Null-calibration worlds: world_seed computed by exactly the development function with the label `brownian.nullcal.v1` in place of `brownian.dev.v1`, one block (block index 0), world index 0..999999 per cell.
U12 Under C-2 (Normal only): qprofile `families` = `0`, `kmax` = `1`.

R3-25 to R3-28 (binding, BRN-0 Grok spec review and Lane T tolerance, 2026-09-29, integrator on Drake's behalf; each resolution below is applied in the section named, and where they differ from R3-24 restated above they win):
R3-25 TPS1 adapter: two gates per observation, hard gate 1e-9 bits against the exact normalised Normal bin codelength, and the qint gate log2(1 + (delta/sd)^2/24) + 1e-9 bits against qint.v1; scored quantity unchanged; 113-bit long double required (Section 8).
G1 Families `0`, kmax `1` in the limitations text (Section 21).
G2 One rlimit rule: CPU = 10 s x held-out points + 10 s, AS 1 GiB, FSIZE 0, NOFILE 16, CORE 0; U2 600 s withdrawn (Section 12, U2 above).
G3 qfreeze key 13 `calibration_extra` binds calib and turing_profile digests (Sections 10, 11, 20, U6).
G4 Turing-profile digest is bound in qfreeze key 13, not qprofile (Section 0, Section 8, U8 above).
G5 per_world_diff_ub carries riders in micro-bits via llrint (Section 8).
G6 Four disjoint labels POSITIVE, NEGATIVE, WEAK, AMBIGUOUS (Section 8).
G7 Producer-visible manifest brw.devman.pub.v1; full brw.devman.v1 is evaluator-only and is what qfreeze key 7 digests (Sections 0, 3, 12, U9 above).
G8 Prequential arm is brw_predict_prequential, rider 0, refit on the first SPLIT + j points; structural KAT on B1-3 block 0 (Section 8).
G9 Scanner hit: verdict FAIL_PROTOCOL on the run record and lane STOPPED (Sections 16, 18 row 3, 19 stop 5).
G10 The mu-coverage band is numbered row 7b (Section 13).
G11 Occam curve uses OCC-* cells only (Section 14).
G12 Result resolution max(0.01 bit, sum of numeric_bound), reported per cell (Section 1).
G13 Preamble: every unresolved item is resolved here (Section 0).
G14 qint.v1 scoring constants are public and allowed (Sections 12, 16).
G15 Check runs on eight cells, both values and relative gap recorded, two known limitations (Sections 11, 21).
G16 Nullcal-quantile figures 0.065 and 0.0079 replace the superseded nullcal-mean figure (Section 10).
G17 s* relabelled; certification crossing computed before BRN-5 into brw.calib.v1 (Section 14).
R3-27 (Grok round-2 arithmetic check): (a) F2 scope, 0.00106 for the 12 observational cells and intervention false-fail at most 0.0110 separately (Sections 15, 21); (b) s* values 3.471, 2.653, 2.137, 1.828 (Section 14, descriptive); (c) cost ratios 3.87x and 2.05x (not present in this spec, corrected in the plan); (d) reference fits use divisor n, R1 MUST use n (Section 7).
R3-28 (Grok second pass): (a) qint gate -log2(1 - b^2/8) + 1e-9 (Section 8); (b) two labels kept distinct, four-way classification and three-way B2-emerging sign label (Sections 8, 11, 20, 21); (c) B1-0 left-end sentence deleted (Section 14); (d) U6 gains occ_cross lines; (e) structural KAT compares data_ub only (Section 8); (f) stop 27, TPS1 gate failure (Section 19); (g) R3-27 edits applied together.

Choices made between conflicting sources (later decision wins per the precedence rule):

| # | conflict | choice |
|---|---|---|
| S1 | Plan section 10 packet (C program on a pipe, mixture family 2) versus R3-16 C-1, C-2 and R3-22c | BRW-DL packet, Normal only; the harness line protocol is internal. |
| S2 | Plan "200 worlds, rep 0..199, sd/sqrt(200)" versus R3-19a W per cell (400, 200, 1600) | rep 0..W-1; W per cell; qworld `rep_count` = W. |
| S3 | Appendix R3 "B1-0 band open" versus R3-16 | [0.01, 0.10] is both acceptance and stop band. |
| S4 | F2 budget "pending" versus R3-20 | sizing rule (b), achieved 0.00106. |
| S5 | B18 Bayesian versus plug-in versus R3-22a | fit-once versus prequential plug-in sign agreement on eight cells (R3-26 G15). |
| S6 | B23 "source pending, default dev block 0 of 200 worlds" versus R3-22b and W = 1600 | development block 0 of B2-emerging at its full W. |
| S7 | KAT 1 z row in the original spec versus R3-17 | contract=off values. |
| S8 | Original text "1e-9 D2 small step" versus OI-3 | the two-part KAT of OI-3. |
| S9 | Static-C candidate versus C-1 in sandbox and artifact sections | C-1. |
| S10 | Packet header: plan omits any BRW-DL hash slot versus R3-23b | the packet carries only the brw_dl_spec hash and not the profile id or digest (the word "brownian" is a scanner word and the profile digest would be circular). |
| S11 | OI-7 wording | bytes charged once per cell; rider once per record set per world. |
| S12 | Nullcal and cell seed constants (0x26B886BDE57B2090, 0x52000001...) are design-check internals | left out; only the label, one block and 1000000 worlds per cell are frozen. |
| S13 | Plan draft `lm_rule` and stop 15 wording "provisional" | R3-20 settles it as above; no provisional wording remains. |

## Amendment 1 (v1.1, R3-38, 2026-09-30): exactly-determined ladder fits are not scored

Everything above is brownian-profile-v1 (sha256 e74ae947951b85e0207ecc91ed95d63ebea851c1f34ca7e9e98b476868bf980b) unchanged. This amendment adds one rule and changes nothing else.

Rule A1. A reference ladder model is not scorable on a cell when the number of its fitted mean coefficients is at least the number of rows it is fitted on: S prefix points for LINEAR_TREND (2 coefficients) and CUBIC_TREND (4 coefficients); S - 1 increments for DIFFUSION_DRIFT (1) and MEAN_REVERT (2). Such a fit reproduces its rows exactly, its maximum-likelihood residual variance is 0, and its predictive scale is refused (Section 18 row 12), whatever the data. A non-scorable model is dropped from Section 20 "Which gains exist" for that cell (no official qgain, no ladder qgain), and item 8 of Sections 13-15 counts the qgains that exist. Under the v1 cell table the rule removes exactly one model on one cell: CUBIC_TREND on B0-short (S = 4, 4 coefficients). The hostile row that reuses B0-short inherits it. No acceptance item names CUBIC_TREND on B0-short.

Reason: v1 required an official CUBIC_TREND qgain on every cell (Section 20) and no FAIL_PROTOCOL on B0-short (Section 13 item 8); on S = 4 both cannot hold. Found when EXP-002A was sealed under v1 (verdict BLOCKED, R3-38); the same refusal occurs on development blocks 0, 1 and 2, so it is a property of the profile, not of any data.

## Amendment 2 (v1.2, R3-39, 2026-09-30): record of a failed seal attempt; no rule change

Everything above is brownian-profile-v1.1 (sha256 a1e232a39e77bea3be25a29795b6a3e5f9f13837933c17352b8563e53de4c198) unchanged. This amendment changes no rule, band, threshold, cell or pair.

Record: under v1.1 (qfreeze 5da4b22b), the first seal attempt (B0-regular) failed on an output-directory error after its attempt ledger was written and before any sealed record was written. No sealed world, qgain or verdict exists for v1.1. The ledger stands; a retry under v1.1 would be a second attempt (stop 16). This version exists only so the successor has a new (candidate, profile) pair. Lineage: 6b121b1c (v1), 5da4b22b (v1.1), then the v1.2 freeze.

## Amendment 3 (v1.3, R3-47/R3-48, 2026-09-30): Occam cells sealed, EXP-002C re-sealed on fresh worlds, TPS1 gate evaluated, verdict text not cut

Everything above is brownian-profile-v1.2 (sha256 a1ade49523b26ba7e581d31f1858df4c1a65fd661117c8492cf7b089c9c06e00) unchanged. No band, threshold, cell definition, pair rule or seed label above is changed. This amendment fixes, before any v1.3 sealed data exists, the scope of the v1.3 seal and the mechanical analysis rules that v1.2 left open. v1.2 sealed scores are never re-analysed under v1.3.

A3.1 Seal scope. Under v1.3 exactly these cells are sealed, block 0, once each: the 124 OCC-* cells of U10 (experiment tag EXP-002B-OCC), and the EXP-002C set B2-noise, B2-weak, B2-clear, B2-fast, B2-emerging, INT-d0/1/2/5/8, CTL-d0/1/2/5/8, INT05-d0/1/2/5/8 (experiment tag EXP-002C). No other cell is sealed under v1.3. The sealed worlds are fresh because the sealed parent is derived from the v1.3 qfreeze digest (Section 5); no new seed label exists.

A3.2 Pairs. OCC-* cells: the declared baseline is DIFFUSION (as EXP-002B), and each OCC-* cell gets exactly one official qgain, DIFFUSION_DRIFT against DIFFUSION (Section 11), subject to Amendment 1. That is the only pair the Occam curve (A3.4) and s14.occ0 (A3.3) read. Reason (R3-48, fixed before freeze from a scratch rehearsal on worlds that are not the sealed worlds): the other ladder models extrapolate far outside the data at long horizons, so on OCC cells their summed description can exceed the 64-bit micro-bit range of the scorer (a scoring abort) and their far-tail predictions cannot be verified by the TPS1 adapter to its 1e-9 tolerance (stop 27). Neither pair is read by any OCC deliverable. EXP-002C set: exactly the official and ladder pairs used under v1.2 for the same cells.

A3.3 Occam gating item s14.occ0. For each H in {128, 256, 512, 1024}: on OCC-s+0.00-H<h>, the classification (Section 8) of T(DIFFUSION_DRIFT vs DIFFUSION) is not POSITIVE, and no FAIL_LEAK, FAIL_FLOOR or FAIL_PROTOCOL occurs on any OCC-* cell. "Drift earns credit" means POSITIVE. PASS iff this holds at all four H. A scoring abort writes no qgain, so s14.occ0 also requires that every one of the 124 OCC-* cells carries its DIFFUSION_DRIFT vs DIFFUSION qgain; a missing one makes the item NOT_COMPUTABLE (INCOMPLETE), never PASS, and a FAIL still wins.

A3.4 Occam curve (descriptive, Section 14 deliverable). For each OCC cell the point is (s, ratio = |mu|/sqrt(2D) from the cell's generator parameters, dT, dT lower, dT upper), dT = certification T of DIFFUSION_DRIFT vs DIFFUSION with its world-bootstrap interval as stored in the qgain. For each H and each sign of s separately, order the points by |s| ascending (s = 0 shared):
- crossing point: the first adjacent pair with dT(s_i) <= 0 < dT(s_{i+1}); |s| where the straight line between the two points is 0. If none: "no crossing in grid". If dT changes sign more than once, the first crossing is reported and the curve is flagged "non-monotone".
- crossing interval: from the smallest |s| whose dT upper end is >= 0 to the smallest |s| whose dT lower end is > 0; a missing end is reported as "beyond grid".
- comparison: occ_cross is one value per H in brw.calib.v1; it is reported beside the result for both signs of that H, with "inside" or "outside" each sign's crossing interval. Descriptive only; never a verdict item and never described as a threshold of the world.
Claim-ceiling rows after v1.3: EXP-002B stays "PASS under v1.2 (without the Occam curve)"; the Occam curve is its own row under v1.3; no single-profile claim merging the two is made. The Occam curve row is PASS when s14.occ0 PASSES and the curve with crossing estimates for all four H and both signs is printed from the sealed qgains; FAIL when s14.occ0 fails.

A3.5 EXP-002C on fresh worlds. The Section 15 items (s15.*) and the EXP-002C-INT items are evaluated on the v1.3 sealed worlds; the s15 items are unchanged from v1.2 and the code is that of the v1.3 freeze commit. The interventional replication rung PASSES iff every gating EXP-002C-INT item (s15.I-far, s15.I-near, s15.I-drift and s15.I-control, the four rows marked gating in Section 15) PASSES on v1.3 worlds. Every other v1.3 EXP-002C item is reported; any FAIL is listed as a failed run and the EXP-002C claim must then say it did not repeat on v1.3 worlds. The v1.2 result is neither replaced nor erased.

A3.6 TPS1 gate (stop 27) and coded lengths. The harness pins omega at 008c5ebd0b878cd72b3e3bb1e143675b1349e6e4, whose difference from 23aab4e adds files only (the TPS1 adapter and the range and rANS coders) and modifies nothing the scoring build compiled before. For every v1.3 qgain the TPS1 check runs (`brownian_bench tps --write`) and writes its sidecar before verdict and results. Stop 27 fires if either arm fails the TPS1 gate (adapter code -204), exactly as Section 19 row 27; a qgain without a valid sidecar is "not evaluated", which makes the experiment INCOMPLETE, never PASS. Coded lengths (range bytes, rANS bytes, ideal length, per arm) are reported in results.json `coded_lengths` (R3-6d); they are descriptive and not a verdict item. EXP-002D row 27 is re-run on development blocks 0 to 2 with the adapter. TPS1 verified range (R3-48, fixed before freeze): the qint half of the gate compares the adapter's code length with the double-precision reference (ty_qcont_bits) to an absolute 1e-9 bits, and one unit in the last place of a double already reaches that size near 2^22 bits, so an observation that alone costs millions of bits cannot pass it for rounding reasons. Measured on synthetic single-observation streams only (no sealed or scratch qgain used to set it): the exact half never deviates above 2e-23 bits; the qint half first fails between 3.05e6 and 3.32e6 bits across sd from 1e-3 to 1e5 (3.08e8 at the sd floor). An observation whose qint code length exceeds 2^20 = 1048576 bits is outside the TPS1 verified range. The TPS1 check skips any world of an arm that contains such an observation and runs the gate on every other world of both arms; an arm with a skipped world is OUT_OF_RANGE (sidecar state 3), a gate failure in any in-range world still wins. OUT_OF_RANGE is admissible only on the arm that lost: on the candidate arm when the qgain class is NEGATIVE and the baseline arm passed, on the baseline arm when the class is POSITIVE and the candidate arm passed. Any other qgain with an OUT_OF_RANGE arm is a stop 27 hit. An admissible OUT_OF_RANGE qgain counts as evaluated (not missing), is listed by name in the stop 27 record, and reports no coded lengths for that qgain.

A3.7 Verdict description text is no longer cut at 71 characters (buffer only; no verdict logic changed).

A3.8 Lineage. Wave 1 profiles: 6b121b1c (v1), 5da4b22b (v1.1), 61140fbb (v1.2), then the v1.3 freeze. This is the fourth and last Wave 1 profile the integrator authorizes (R3-47).

## Amendment 4 (v1.4, R3-50 to R3-52, 2026-10-09): discovery seal scope for BRN-10

Everything above is brownian-profile-v1.3 (sha256 76ea08ab517ef9bcb7141c077cd0bf5263311dcc5a93432f5df8ab0f12a80991) unchanged. No band, threshold, cell definition, statistic, reference program, pair rule or seed label above is changed. v1.4 exists only so that the BRN-10 discovery run has a seal scope: v1.3 A3.1 seals no primary cell of B0 or B1. v1.4 is the fifth Wave 1 profile; Drake approved raising the Wave 1 profile cap from 4 to 5 for this purpose only on 2026-10-09. The Grok sign-off of a new profile is deferred to after BRN-10 seals, on the R3-43 and R3-44 precedent (sending profile text to a cloud AI before the seal risks stop 25), as for v1.1 to v1.3. Sealed scores of v1.2 and v1.3 are never re-analysed under v1.4, and gains under different profile digests are never compared.

A4.1 Seal scope. Under v1.4 exactly the 12 primary cells of Section 6 are sealed, block 0, once each, mode discovery, for exactly one candidate artifact (the BRN-10 artifact): B0-regular, B0-irregular, B0-short, B1-0, B1-1, B1-3, B1-4n, B2-noise, B2-weak, B2-clear, B2-fast, B2-emerging. No certification seal is made under v1.4, and no OCC-*, INT-*, CTL-*, INT05-* or H- cell is sealed. The sealed worlds are fresh because the sealed parent is derived from the v1.4 discovery qfreeze digest (Section 5). One sealed attempt (stop 16). One frozen artifact covers all 12 cells and is never told the cell.

A4.2 Pairs. On each cell exactly one official qgain: the discovery artifact against the cell's declared baseline of Section 7 (B0 cells IID_NORMAL, B1 cells DIFFUSION, B2 cells DIFFUSION_DRIFT except B2-noise against DIFFUSION per Section 15 item 1). Ladder qgains (official 0) of the artifact against other ladder programs are reported when computed, are descriptive only, never substitute for the official pair, and their absence never makes a cell INCOMPLETE.

A4.3 Outcome per cell (Section 21). Each cell gets exactly one outcome:
- a classification sentence from its official qgain (found structure, no additional structure, uncertain (AMBIGUOUS), weak evidence (WEAK)) by the Section 21 rule, when the qgain exists and carries no fail flag;
- a fail-flag sentence (protocol failure, leakage, floor exploitation) when the official qgain carries a fail flag, or when the candidate causes the cell's scoring to end without a qgain (a refusal, a non-finite or out-of-family prediction, an exceeded resource or language limit, a sandbox kill; the packet states that any of these fails the run): that cell's outcome is "protocol failure" and it counts as classified;
- no outcome only when the evaluator side fails (harness crash, host or disk failure, an evaluator bug) with no candidate cause; that cell is INCOMPLETE, its partial files are kept, and it is never re-sealed (stop 16).
Clarification, not a rule change: the Section 21 sentence "hypothesis failed" refers to the signs preregistered in Sections 13 to 15 for the reference programs; no sign is preregistered for a free candidate, so that sentence is not defined in discovery mode. Certification-only items (parameter recovery, coverage, overconfidence, B1-1.3) are not applied (A2). Section 10 diagnostics are reported per cell. Claim template as Section 21 with mode discovery; the prohibited sentence of Section 21 holds.

A4.4 Stops and BRN-10 verdict. A Section 19 stop applies in discovery mode unless its condition names reference programs, certification items or a cell outside A4.1; each such stop is recorded as not applicable, never as passed. BRN-10 verdict: PASS when the protocol completed with no applicable stop and every one of the 12 cells has an outcome under A4.3 (any mix of outcomes is a valid result); INCOMPLETE when some cell has no outcome; FAIL when an applicable stop occurs, including the producer stops of A4.6. A positive T grants nothing beyond its sentence.

A4.5 Producer inputs (Section 12 and U9 made concrete). The producer's working directory (its root) holds exactly: packet.txt (sha256 4383e7db7804088d323ee964d625fc82e9571f9baf0dcabbde8afba1da19e5c7), brw_dl_spec.txt (sha256 86bf360cfa208d91338cf22faa0f6022c93dfe2ee1a574e2ee27a800ec6a557f), every world of development blocks 0, 1 and 2 of the 12 primary cells as world_<sha8>.qbw, their brw.devman.pub.v1 manifest as devman.pub.txt, brw_dl (the static brw_interp binary built by the Makefile from brownian/src/brw_dl.c with -DBRW_DL_CLI at the freeze commit; asm, info, run and memo commands), the producer tool's configuration, a private HOME and tmp directory, and an empty work directory. Mounted read-only beside it: /toolchain (the producer tool binary and a minimal shell set: bash, a coreutils subset, awk, sed, grep, getent and their shared libraries; no compiler, no git, gh, curl, wget or python). Network (Drake, 2026-10-09, narrower than Section 12 items 1 and 5, which allowed open egress through --share-net only so that the producer could reach its model API): the sandbox has its own network namespace with loopback only. The producer tool reaches the model only through a broker that runs outside the sandbox and is reached through one bound unix socket (relayed to a loopback port inside the sandbox). The broker holds the credential (atlas-vault entry BRNPROD_ANTHROPIC_KEY, a dedicated key for this run) and adds it to each request; it forwards only the model message endpoints to the model provider, only for model claude-opus-5-5, logs token usage per request without content, and refuses further requests once its spending cap is reached. The credential is never inside the root, the sandbox environment, the prompt, the transcript, a candidate or any repository, and it is revoked when the producer session ends. The isolation proof (brownian/producer/ISOLATION-PROOF.md) is re-run on this exact bind set with a dummy producer before the session.

A4.6 Producer session. Tool Claude Code (version recorded in qfreeze), model claude-opus-5-5 (training data cutoff Jun 2026 per platform.claude.com/docs/en/models/overview, fetched 2026-10-09; predates 2026-09-29, Section 12 item 6). Prompt: the exact bytes of brownian/producer/prompt.txt at the freeze commit; the packet scanner runs on it (stop 5). Output: the file work/candidate.brwdl (BRW-DL text), copied out of the root by the launcher before the root is removed; the evaluator assembles the canonical bytes (U4). One producer session. It may be restarted once, and only if it ended before any model output for a technical reason (authentication, broker or network failure, tool start-up); every transcript is kept and listed. Producer stops (each is BRN-10 FAIL, lane STOPPED, no seal, recorded): a session that produced model output and ended without a candidate file; a transcript scan hit (Section 12 item 4); a candidate that the evaluator cannot assemble. No human edits the candidate, and nothing about development or sealed scores is returned to the producer.

A4.7 Interpreter build (R3-52, pre-freeze harness fix). U2 requires the interpreter to be built with the frozen rounding flags; the Makefile's static build lacked -ffp-contract=off -fno-fast-math and now carries both. The interpreter's SHA-256 is recorded in qfreeze (U3).

A4.8 Order. Spec v1.4 merged; v1.4 fingerprint published in omega (commitment and the append-only history); producer root built and isolation re-proved; packet scanner on every text file and the prompt; producer session; transcript scan; freeze --discovery; attempt ledger; seal; score; verify; replay (twice from stored files, once with --regenerate); results.json and the results document; after the seal the evaluator tree is published by PR and the commitment is checked (R3-11).

## Amendment 5 (v1.5, R3-55, 2026-10-09): BRN-10 attempt 2 after a transcript-scan stop

Everything above is brownian-profile-v1.4 (sha256 b30c8c3a57e6ff9bf6c5492414a5e402ad80e9fdedf5eaa722aaaef23cf3b0ba) unchanged, except that A5.2 and A5.3 replace the named v1.4 items for BRN-10. No band, threshold, cell definition, statistic, reference program, pair rule or seed label is changed, and A4.1 (seal scope), A4.2 (pairs), A4.3 (outcome per cell), A4.4 (stops and verdict) and A4.7 (interpreter flags) are unchanged. v1.5 is the sixth Wave 1 profile; Drake approved raising the Wave 1 profile cap from 5 to 6 for this purpose only on 2026-10-09. The Grok sign-off is deferred to after BRN-10 seals, as for v1.1 to v1.4.

A5.1 Record of attempt 1. Under v1.4, producer session 1 (2026-10-09, transcript sha256 ce63d47836c7d748b9fcca10cc0346ca50c0d90341d4cb7e0883999e835422ae) ended with model output and the v1.4 transcript scan reported 121 hits, so BRN-10 attempt 1 is FAIL (protocol failure, producer stop under A4.6), lane STOPPED, recorded in evidence/BROWNIAN/BRN-10/PRODUCER-STOP-2026-10-09.md (R3-54). That record is permanent. The attempt-1 candidate was never copied out of the root, frozen, assembled, run or scored; no development or sealed result from it exists; nothing from attempt 1 (candidate, transcript, scan, diagnosis) is given to the attempt-2 producer. The evaluator's audit of every tool call found no access outside the root and no URL; the hits came from the scanner reading model-side text (signature blobs, reasoning, tool results) and from the tool configuration inside the root quoting outside paths. Attempt 1 ran with the network and credential that A5.2 now states, not the broker that the A4.5 text describes: Drake's decision of 2026-10-09 to use his Claude subscription was recorded in R3-52 but not carried into the A4.5 text. This deviation is recorded in R3-55 and does not change the attempt-1 verdict.

A5.2 Network, credential and configuration (replaces the network paragraph of A4.5 and its root item "the producer tool's configuration"). Network: the sandbox shares the host network namespace (bwrap --share-net), as Section 12 items 1 and 5 allow; open egress and reachable host loopback services are a known limitation. The controls are the absence of any network client in the root and /toolchain other than the producer tool, the tool restrictions (WebFetch and WebSearch denied, no git, gh, curl or wget) and the transcript scan. Credential: Drake's Claude subscription token (claude setup-token, atlas-vault entry BRNPROD_CLAUDE_OAUTH_TOKEN). The launcher reads it into memory and passes it as the first line of the tool's standard input to a wrapper that exports CLAUDE_CODE_OAUTH_TOKEN for the tool process; it is never in an argv, a root file, the prompt or a repository. After the session the launcher redacts it from the transcript and stderr, and if any trace remains anywhere in the evidence directory the launcher exits 3 and the session is a producer stop. The token is revoked after BRN-10. Configuration: the producer tool's settings file (brownian/producer/producer_settings.json, sha256 55d131407ac76164f20d7a2076f63f6f0fef4224c73a1344bfa7fd9cc8898b5a) is not in the root; it is mounted read-only at /etc/brn-producer-settings.json. The root holds exactly the other A4.5 items.

A5.3 Transcript scan (replaces the scan method of Section 12 item 4 for BRN-10; "a hit is STOPPED and the candidate is not scored" is unchanged). The scanner is brownian/producer/brn_scan_transcript.sh, tested by brownian/producer/test_scanner.sh; its header comment is the full rule and this paragraph summarises it. It scans the producer's actions: every string in the input of every tool call in the stream-json transcript (commands, file paths, and the text of files the producer writes or edits). Model reasoning and its signatures, assistant prose and tool results are stored and audited but not scanned. The pattern of a Grep call, the description of a Bash call and the old_string of an Edit call are checked for URLs only (text to match, prose, or text being removed). A hit (STOP) is a reference through which the producer could have obtained anything from outside its root: (a) any scheme:// URL; (b) an escaped slash (\x2f or \057) followed by a letter; (c) a path that resolves to a location that exists inside the sandbox outside the allowed areas, namely "/" itself, /etc, /proc, or anything under /dev except /dev/null, /dev/zero, /dev/random, /dev/urandom, /dev/stdin, /dev/stdout, /dev/stderr, /dev/tty and /dev/fd (so /dev/tcp and /dev/udp, the shell's network devices, are hits); (d) etc, proc or dev spelled with quotes, braces, brackets or backslashes inside the name. The allowed areas are /sandbox, /toolchain, /bin and /lib (the last two are links to /toolchain); the sandbox top level holds only bin, dev, etc, lib, proc, sandbox and toolchain (ISOLATION-PROOF.md). A path whose first component names a top-level directory of an ordinary host that does not exist inside the sandbox (home, tmp, var, usr, root and the like), the bare directory /dev, and a cd that climbs to "/" through ".." cannot reach anything; they are printed as NOTE lines for the audit and are not hits. A transcript that is not valid JSON lines is a hit. How words are read: a string is split at whitespace and at " ' ` = < > | ; & ( ) , : { } [ ] \ ! @. A word starting with "/" is a path unless it directly follows ")", "}", "]", or a closing quote that follows a letter, digit, "_", "}" or ")" (division or a path built at run time, such as (a+b)/var); an option glued to a path (-I/etc) is read as the path. A bare "/" counts only directly after cd, ls, find, cat, ln, cp, mv, rm, du, stat or tree, or after option words that follow one of them. A glob in the first component counts if it contains a letter and can match etc, proc or dev. "~" is /sandbox/home. A relative word is resolved against the producer's working directory when it has a ".." component, starts with "~", or the working directory is outside /sandbox; the working directory starts at /sandbox and follows each cd in the Bash commands in order, and a cd into "/", /etc, /proc or /dev by name is a hit; file text the producer writes is resolved from /sandbox/work/a/b/c, and a cd inside it applies only within that text. Known false positive: a regular expression that spells a hit path exactly (for example awk '/etc/' or sed '/proc/p'). Known limitation: an access that names no path in a tool input (a path assembled at run time, or a variable such as $PWD), a cd inside a subshell that the scanner treats as persistent, and the NOTE class are not stops; the operating-system isolation (A4.5, A5.2, ISOLATION-PROOF.md) is the enforcing control and the scan is its audit.

A5.4 Acceptance before attempt 2. Before the attempt-2 session: test_scanner.sh passes (every dirty fixture exits 1, every clean fixture exits 0 with no NOTE, every note fixture exits 0 with a NOTE, malformed JSON exits 2) and the A5.3 scanner reports SCAN CLEAN with no NOTE on the attempt-1 transcript; the isolation proof (test_isolation.sh) passes on the A5.2 bind set with the dummy producer, including the refusal of an unpinned prompt in claude mode; the v1.5 fingerprint is published in omega (commitment and append-only history). The launcher refuses a settings file other than the A5.2 one and, in claude mode, a prompt other than the A5.5 one. The scanner and launcher used are those at the v1.5 merge commit; their sha256 are recorded in the attempt-2 evidence and the results.

A5.5 Attempt 2. One new producer session under v1.5, with the A4.6 tool, model, prompt bytes (sha256 669c962564b492a7156eb67b46cf401d0f39a89ce0145bd66541c42bf15dc624), packet, brw_dl_spec, development worlds, devman.pub.txt and interpreter. The A4.6 restart rule (only for a session that ended before any model output, for a technical reason) and the A4.6 producer stops apply, with the A5.3 scan. If attempt 2 stops, BRN-10 is FAIL under v1.5 as well and no further producer session exists for Wave 1 BRN-10. If attempt 2 yields a candidate, A4.7, A4.8 and Sections 19 to 21 apply under v1.5 exactly as they were written for v1.4 (one sealed attempt per candidate and profile, stop 16), with the qprofile built from this file. The BRN-10 record lists both attempts; the attempt-2 outcome is reported as attempt 2 beside the attempt-1 FAIL and never replaces it.

A5.6 Order. v1.5 merged; v1.5 fingerprint published in omega; A5.4 acceptance; producer session; transcript scan (A5.3); then A4.8 from "freeze --discovery" onward.
