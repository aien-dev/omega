# DIRAC-0 known-answer corpus format (v1, written, not built)

Status: NOT_RUN. Public conformance corpus only. Sealed experiment keys are never stored here (ORACLE-SPEC.md section 8).

## 1. File shape
Line oriented UTF-8 text, LF endings, no trailing spaces, no tabs. Order is deterministic: records sorted by `kind` in the order listed in section 3, then by `id` bytewise. File name: `dirac-kat-<sha256>.txt` where the hash is the corpus digest below.

```
OMEGA-DIRAC-KAT v1
domain omega.dirac.kat.v1
record_count <u32 decimal>
<records>
corpus_digest <64 lowercase hex>
end
```

## 2. Canonical encodings
- Integer: decimal, optional leading `-`, no leading zeros, no `+`. Zero is `0`.
- Rational: `n/d`, d >= 1, gcd(|n|,d) = 1, d = 1 written `n/1`.
- Complex: `(re;im)` where re and im are rationals, no spaces. Example `(1/2;-3/1)`.
- Matrix: `n` rows separated by `|`, entries in a row separated by `,`. Example `[(1/1;0/1),(0/1;0/1)|(0/1;0/1),(-1/1;0/1)]`.
- Metric: comma separated integers, diagonal only, in generator order. Example `1,-1`.
- Scaled decimal (high precision): `N@k` meaning N / 10^k, N integer, k >= 0; bound `B@k` is a non-negative integer with the same k.
- No floating point text anywhere (no `.`, no `e`).

Corpus digest: SHA-256 over bytes `omega.dirac.kat.v1` || 0x00 || every line of the file above the `corpus_digest` line, each followed by LF. The digest line itself and `end` are excluded.

## 3. Record kinds (in this order)
Every record is `record <kind> <id>` then `key value` lines then `endrecord`. Fields marked (E) are evaluator-only and must be stripped before any export toward the AIEN side.

1. GAMMA_REP: `rep_name` (E), `dim` (generators d), `size` (matrix size n), `metric`, `g0` .. `g<d-1>` (matrices).
2. CLIFFORD_CHECK: `rep_ref` (id of a GAMMA_REP), `expect` (`PASS` or `FAIL`), `failing_pairs` (list `i:j` or `none`).
3. PLANE_WAVE: `rep_ref`, `m`, `p` (rationals), `energy` (rational or scaled decimal with bound), `amplitude` (matrix n x 1, unnormalized), `convention_note` (E).
4. DISPERSION: `m`, `p`, `energy_squared` (rational), `energy` (rational, or scaled decimal plus `bound`), `error_kind` (EXACT or BOUNDED_DETERMINISTIC).
5. BASIS_CHANGE: `rep_from`, `rep_to`, `S`, `S_inv`, `observable` (`dispersion` or `trace_products`), `expect`.
6. HIPREC_REF: `quantity` (E), `value` (scaled decimal), `bound` (scaled decimal), `route_a_digest`, `route_b_digest` (SHA-256 of each route's decimal output).

## 4. Worked examples (checked by hand)

Representation 1, Dirac style, metric diag(+1,-1): g0 = sigma_z, g1 = i sigma_y.
```
record GAMMA_REP rep-a
rep_name dirac-1p1
dim 2
size 2
metric 1,-1
g0 [(1/1;0/1),(0/1;0/1)|(0/1;0/1),(-1/1;0/1)]
g1 [(0/1;0/1),(1/1;0/1)|(-1/1;0/1),(0/1;0/1)]
endrecord
```
Hand check. g0 g0 = I, so g0^2 = +I = metric(0,0) I with factor 2: {g0,g0} = 2 I. OK.
g1 g1 = [[0,1],[-1,0]] times itself = [[-1,0],[0,-1]] = -I, so {g1,g1} = -2 I = 2 metric(1,1) I. OK.
g0 g1 = [[1,0],[0,-1]] [[0,1],[-1,0]] = [[0,1],[1,0]]. g1 g0 = [[0,1],[-1,0]] [[1,0],[0,-1]] = [[0,-1],[-1,0]]. Sum is the zero matrix, matching 2 metric(0,1) I = 0. OK.
```
record CLIFFORD_CHECK cliff-a
rep_ref rep-a
expect PASS
failing_pairs none
endrecord
```

Representation 2, Weyl style: g0 = sigma_x, g1 = -i sigma_y.
```
record GAMMA_REP rep-b
rep_name weyl-1p1
dim 2
size 2
metric 1,-1
g0 [(0/1;0/1),(1/1;0/1)|(1/1;0/1),(0/1;0/1)]
g1 [(0/1;0/1),(-1/1;0/1)|(1/1;0/1),(0/1;0/1)]
endrecord
```
Hand check. g0^2 = [[0,1],[1,0]]^2 = I. g1^2 = [[0,-1],[1,0]]^2 = [[-1,0],[0,-1]] = -I. g0 g1 = [[0,1],[1,0]] [[0,-1],[1,0]] = [[1,0],[0,-1]]. g1 g0 = [[0,-1],[1,0]] [[0,1],[1,0]] = [[-1,0],[0,1]]. Sum is zero. OK.

Basis change from rep-a to rep-b with S = [[1,1],[1,-1]], S_inv = (1/2) S (S S_inv = (1/2) [[2,0],[0,2]] = I).
S g0 S_inv with g0 = sigma_z: S sigma_z = [[1,-1],[1,1]]; times (1/2) S = (1/2) [[1-1, 1+1],[1+1, 1-1]] = [[0,1],[1,0]] = sigma_x = rep-b g0. OK.
S g1 S_inv with g1 = [[0,1],[-1,0]]: S g1 = [[-1,1],[1,1]]; times (1/2) S = (1/2) [[-1+1, -1-1],[1+1, 1-1]] = [[0,-1],[1,0]] = rep-b g1. OK.
```
record BASIS_CHANGE basis-ab
rep_from rep-a
rep_to rep-b
S [(1/1;0/1),(1/1;0/1)|(1/1;0/1),(-1/1;0/1)]
S_inv [(1/2;0/1),(1/2;0/1)|(1/2;0/1),(-1/2;0/1)]
observable trace_products
expect PASS
endrecord
```

Dispersion, exact case m = 1, p = 3/4: E^2 = 9/16 + 1 = 25/16, E = 5/4.
```
record DISPERSION disp-a
m 1/1
p 3/4
energy_squared 25/16
energy 5/4
error_kind EXACT
endrecord
```

## 5. Verification (written, not run)
`src/oracle_kat_verify.sh` recomputes the corpus digest with `sha256sum` and compares it to the `corpus_digest` line and the file name. `src/oracle_clifford.c` checks CLIFFORD_CHECK style relations on matrices given in the section 2 encoding (the first version reads a simplified fixed layout, see its header comment).
