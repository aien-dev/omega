# M21 OMEGA_AUTODIFF: reverse-mode tape, CPU tier

Status 2026-10-01: **M21 NOT QUALIFIED.** The tape, its tests and its
mutation sweep are written. Forge host run on commit c098a74 (before the
rebase onto omega#178/#179 and before the gate fixes): `test-autodiff` PASS
182 pass / 0 fail (log HIVE-M21-181152.log), mutation sweep 10/10 caught (log
HIVE-M21-181157.log). The current commit (rebased, bit-exact layer, pure
1e-3 relative FD, 12 mutants) is NOT_RUN until the forge reruns it. GB10 NOT_RUN. M20, which this layer sits on, is itself not
qualified (GB10 parity NOT_RUN).

Plan source: `aien-architecture/CURRENT_EXECUTION_PLAN.md`, section
"E3. M21 OMEGA_AUTODIFF": "Omega forward graph -> differentiate -> Omega
backward graph. The backward pass is ordinary Omega and uses the same
Forge/AEGIS path." ROADMAP row M21: "Sovereign automatic differentiation
generating gradient semantic graphs", PLANNED. Neither document states an
M21 exit gate or a gradient tolerance. The owner approved the M21 gate
(queen, 2026-10-01):

> **M21 gate: gradients bit-exact against the same M20 operations, plus a
> finite-difference check within 1e-3 relative error.**

See "Gate conformance" below for how the current tests map onto it.

## Scope

A reverse-mode tape over the M20 OMEGA_TENSOR CPU tier
(`src/tensor/omega_tensor.h`). Forward: each recorded op is one M20 call.
Backward: walks the tape from the root down and builds every gradient with
M20 calls (ADD, SUB, MUL, DIV, matmul, transpose, reshape, broadcast_to,
reduce SUM), so the backward pass runs on the same E1 realization as the
forward pass. One exception: MAX routing is a host-side data move (compare
and copy, no arithmetic) in the caller's scratch buffer, because M20 has no
mask-producing op (CR-3 below).

Out of scope here: GB10 realization, gradients for the M20 transcendental
unary ops (EXP2, LOG2, SIGMOID, TANH, added by omega#178; a later cut,
pending LT-M20), slicing/concat/gather gradients, second derivatives, dispatch
provenance records for forward/backward ops (open question for E5), and
any edit of `src/tensor` or `src/train`.

## Files

| File | Role |
|---|---|
| `src/autodiff/omega_autodiff.h` | The meaning: ops, ownership, determinism rules, per-op error contract |
| `src/autodiff/omega_autodiff.c` | The tape: recording, backward walk, unbroadcast, M22 flatten. No heap allocation |
| `tests/test_omega_autodiff.c` | Contract, bit-exact replay, finite-difference, determinism, error-path and leak checks |
| `tools/autodiff_mutations.sh` | Source mutation sweep (12 mutants) |
| `mk/autodiff.mk` | `make test-autodiff`, `make test-autodiff-mutations` (alias `autodiff-mutants`) |

## Determinism and resources

- No global state, no heap allocation in `src/autodiff`. The caller passes
  the node array (explicit capacity), an optional float scratch buffer
  (needed only by MAX: `3 * elements(y)` floats, see
  `omega_ad_scratch_need`) and the M20 context, whose slot capacity bounds
  every tensor made.
- Tape full: `OMEGA_AD_ERR_TAPE_FULL`, nothing recorded, nothing leaked.
- Fixed order: nodes are walked in strictly decreasing index; per node, the
  contribution to the first input is accumulated before the second; several
  contributions to one node are added in that order with M20 ADD.
- Every gradient is a dense row-major F32 tensor with exactly its node's
  shape; a contribution of any other shape is refused (`OMEGA_AD_ERR_SHAPE`)
  instead of being silently broadcast.

## Ops, backward rules and error bounds

g = incoming gradient, y = stored forward value, unb = sum back to the
input's shape (extra leading axes by reduce SUM over axis 0, then every axis
where the input has size 1 by reduce SUM with keepdims). u = 2^-24,
gamma(k) = k u / (1 - k u). L(m) = max(1, ceil(log32 m)) E1 reduction levels,
5 tree steps each, so a declared-order sum of m terms has error at most
gamma(5 L(m)) times the sum of absolute terms (E1 reduction contract; padding
with -0.0 is exact). D(x) = sum of 5 L(m) over every reduction unb makes for
input x (0 when x was not broadcast).

Bounds are local: each gradient against exact real arithmetic on the float32
values that are on the tape (stored forward values and the incoming g), in
the normal range (no overflow, no subnormal intermediates).

| Op | M20 forward | Backward (M20 calls) | Bound per gradient element |
|---|---|---|---|
| ADD | binary ADD | ga = unb(g), gb = unb(g) | gamma(D(x)) sum abs(g); exact when not broadcast |
| SUB | binary SUB | ga = unb(g), gb = unb(g * -1) | same as ADD (negation is exact) |
| MUL | binary MUL | ga = unb(g * b), gb = unb(g * a) | gamma(D(x)+1) sum abs(g * other) |
| DIV | binary DIV | ga = unb(g / b); gb = unb(-(g * (y / b))) | a: gamma(D(a)+1) sum abs(g/b); b: gamma(D(b)+3) sum abs(g a / b^2) |
| MAX | binary MAX | g routed: to a if b is NaN and a is not, or a > b; else to b | gamma(D(x)) sum abs(routed g); routing exact |
| SQRT | unary SQRT | ga = g / (y + y) | gamma(2) abs(g / (2 sqrt a)); inf at a = 0 |
| MATMUL [..,M,K]x[..,K,N] | matmul | gA = unb(g @ B^T), gB = unb(A^T @ g) | A: gamma(1 + 5 L(N) + D(A)) sum abs(g) abs(B); B: gamma(1 + 5 L(M) + D(B)) sum abs(A) abs(g) |
| TRANSPOSE | transpose + contiguous | ga = g^T | exact |
| BROADCAST | broadcast_to + contiguous | ga = unb(g) | gamma(D(a)) sum abs(g) |
| SUM(axis) | reduce SUM | ga = broadcast_to(reshape(g)) | exact |
| MEAN(axis) | reduce MEAN | ga = broadcast_to(reshape(g)) / n | gamma(1) abs(g / n), n <= 2^24 |
| fan-out | | k contributions added in tape order | extra gamma(k-1) sum abs(contribution) |

MAX tie rule: ties (including -0 vs +0) and a NaN `a` send g to `b`, so
`relu(x) = MAX(x, 0)` has gradient 0 at x = 0.

## M22 bridge (tg_sgd compatibility)

`tg_sgd_step(st, sh, const float *grad, size_t n, lr, momentum, grad_ref)`
takes one flat float32 gradient whose length equals the shadow's parameter
count. `omega_ad_grads_flatten(tape, ids, n, out, count)` writes the
gradients of the given parameter nodes back to back, each in logical
row-major order, and refuses (writing nothing) unless the total equals
`count`. Compatible as long as the caller lays the shadow's parameters out
in the same node order. `src/train` is unchanged and still includes no
tensor header (its purity rule). `grad_ref` is the caller's: a natural
choice is a prefix of `omega_tensor_value_id` of the gradient tensors
(`omega_ad_grad` gives the handle). NaN/Inf gradients are not refused by the
tape or by `tg_sgd_step`; where to refuse them (tape end or the `tg_commit`
validate callback) is an open decision.

## Test design (M21 gate, approved 2026-10-01)

Approved M21 gate (owner, via the queen, 2026-10-01): **"gradients bit-exact
against the same M20 operations, plus a finite-difference check within 1e-3
relative error"**. Layers 2 and 3 below are that gate; layers 1, 4 and 5 are
extra.

`tests/test_omega_autodiff.c`, 21 op cases (every op, with and without
broadcasting, batched matmul) plus a composite graph:

1. **Contract layer.** Each tape gradient against an exact reference written
   in the test from the calculus (double precision, index arithmetic on
   shapes only), within the bound in the table above.
2. **Bit-exact layer (gate).** For every op case the test replays the
   backward rule itself with direct M20 calls in the tape's order (for
   example MUL: `binary MUL (g, b)`, then reduce SUM over the extra leading
   axes and the size-1 axes of a, keepdims) and compares the tape's gradient
   with it bit for bit (`memcmp`), for a and for b. MAX routing, a host data
   move, is replayed with the routing rule written independently in the
   test. In-process counterexample: a one-bit flip of the replay must make
   `memcmp` differ.
3. **Finite-difference layer (gate).** Central differences of the M20 CPU
   forward pass, step h = 2^-6 (actual step taken from the rounded floats),
   loss L = sum(y * R) in double. Pass if `|tape - fd| <= 1e-3 |fd|`
   (`FD_RTOL`, `fd_ok`): pure relative, **no absolute term**. Near-zero
   gradients are handled by choosing the test points, not by loosening:
   the FD layer runs on its own data set (`gen_fd_data`, seed R in
   [0.5, 1]; MUL and MATMUL operands in [0.5, 1], DIV numerator in [1, 2],
   SQRT input in [1, 4]) where every gradient element is a sum of same-sign
   terms of magnitude about 0.1 or more, or exactly zero (MAX routing, where
   the perturbed forward is unchanged and fd is exactly 0 too). Every
   element, near zero or not, is still covered by layers 1 and 2 on the
   mixed-sign data. Inputs are kept away from kinks (MAX) and singular
   points (DIV, SQRT). In-process counterexamples: the FD comparator accepts
   the exact gradient on all 12 elements of a MUL case, rejects a sign flip,
   and rejects a gradient 2e-3 too large on all 12.
4. **Composite.** Linear layer + squared error, `mean_i sum_j (XW + b - Y)^2`,
   with a reused node (fan-out): closed form `(2/N) X^T E` within 1e-5 on
   mixed-sign data; FD (pure 1e-3 relative) on every parameter on a second
   data set (`lin_data_fd`: X, W, b in [0.5, 1], Y in [-0.5, 0], so every
   gradient is >= 1.25), with the FD loss computed in double from the tape's
   float `d` values. X and Y have no gradient.
5. **Determinism.** Two full runs in two M20 contexts: flat gradients
   bit-identical (memcmp) and the loss value id identical.
6. **Error paths.** Tape full (no state change, no leak), shape mismatch
   (`[3,4]+[5]`, `[3,5]x[4,2]`, `[3,5]x[3,5]`, broadcast `[5]->[4]`, bad axis),
   wrong seed shape, scalar seed on a non-scalar root, too little scratch,
   second backward, recording after backward, unreached leaf, no-grad root,
   F16 leaf, flatten count mismatch (buffer untouched), bad node ids, leak
   check of the M20 context.

Counterexamples in process: the contract comparator must reject a gradient
off by 2^-20 relative and accept the exact one; the FD comparator as above;
the bit-exact and determinism comparators must see a one-bit flip.

Mutation sweep (`tools/autodiff_mutations.sh`), each must make the test
fail (12 mutants): TAPE_WALK (skip the root), ACCUMULATE (replace instead of
add), UNBROADCAST (skip size-1 axis sums), SUB_NEGATE, MUL_OPERAND,
MATMUL_TRANSPOSE, SQRT_TWICE, MEAN_DIVISOR (n+1), MAX_ROUTE (reverse
comparison), MAX_TIE (ties to a), DIV_B_SIGN (drop the minus on the DIV
b-gradient) and DIV_FORMULA. DIV_FORMULA computes the DIV b-gradient as
`-((g * y) / b)` instead of `-(g * (y / b))`: the same real number with the
same number of roundings, so by construction it stays inside the contract
bound gamma(D(b)+3) and the 1e-3 FD bound, and only the bit-exact layer is
expected to catch it (UNVERIFIED until the forge runs the sweep).

## Qualification checklist

Two commits matter here. **c098a74** (before the rebase, before the fixes
below) has a forge receipt. **The current commit** (rebased onto omega#178
and #179, with the bit-exact layer, the pure relative FD check and 12
mutants) is NOT_RUN until the forge reruns it.

| Item | c098a74 (forge receipt) | Current commit | Evidence |
|---|---|---|---|
| Reverse-mode tape over M20 CPU ops | WRITTEN | WRITTEN | `src/autodiff/` |
| Contract layer | PASS | NOT_RUN | `make test-autodiff` |
| Bit-exact layer (gate) | not present | NOT_RUN | `make test-autodiff` |
| Finite differences, pure 1e-3 relative (gate) | not present (c098a74 used 5e-4 + 1e-3 \|fd\|) | NOT_RUN | `make test-autodiff` |
| Composite graph end to end | PASS | NOT_RUN | `make test-autodiff` |
| Determinism (bit-identical rerun) | PASS | NOT_RUN | `make test-autodiff` |
| Error paths and leak check | PASS | NOT_RUN | `make test-autodiff` (plain and ASan/UBSan) |
| Host test totals | PASS, 182 pass / 0 fail (log HIVE-M21-181152.log) | NOT_RUN | forge |
| Mutation sweep | PASS, 10/10 caught (log HIVE-M21-181157.log) | NOT_RUN (12 mutants) | `sh tools/autodiff_mutations.sh` |
| CI wiring | MISSING | MISSING | no `autodiff` job in `host-suites-2.yml` yet |
| GB10 | NOT_RUN | NOT_RUN | no GB10 realization of M20 yet |
| Owner approval of the gate | | APPROVED | queen, 2026-10-01 (wording above) |
| Reproducible receipt | MISSING_IMPLEMENTATION | MISSING_IMPLEMENTATION | no receipt writer for this gate yet; the forge receipt lines do not name the commit hash |

**M21 verdict: NOT QUALIFIED** until the host run passes on the current
commit and a GB10 chip receipt exists.

## Gate conformance (approved wording vs current tests)

- **FD within 1e-3 relative.** Met by construction in the current commit:
  `fd_ok` is `|tape - fd| <= 1e-3 |fd|` with no absolute term (the 5e-4
  floor of c098a74 is removed), on FD data sets chosen so no gradient
  element is near zero. NOT_RUN until the forge reruns it.
- **Bit-exact against the same M20 operations.** Met by construction in the
  current commit: layer 2 replays every op case's backward rule with direct
  M20 calls and `memcmp`s it against the tape, for both inputs. The
  composite graph is covered by the per-op replays plus the bit-identical
  determinism check, not by its own whole-graph replay. NOT_RUN until the
  forge reruns it.
- Owed (inspector, recommended, not blocking): separate mutants for
  TRANSPOSE backward, SUM expand axis, the unbroadcast leading-axis loop
  and the capacity check; a forge receipt that records the commit hash.

## Tensor API change requests (not made here; owner LT-M20)

- **CR-1** PARTLY RESOLVED by omega#178 (1a470c2): `OmegaTensorUnaryOp` now
  has `OMEGA_TU_EXP2`, `OMEGA_TU_LOG2`, `OMEGA_TU_SIGMOID`, `OMEGA_TU_TANH`
  (E1 bounded contract, via the optional `transc` realization hook).
  Still open: EXP, LOG, RSQRT, ERF, SIN, COS, GELU. Autodiff for the four
  new ops is not in this cut (later cut, pending LT-M20).
- **CR-2** Constant helpers (full / zeros / ones / neg). Convenience; the
  tape builds constants with `omega_tensor_from_f32`.
- **CR-3** A mask-producing op (compare returning 1.0 / 0.0, or
  where(pred, x, y) with a separate predicate). Lets MAX/MIN/relu/abs and
  reduce MAX/MIN backward stay inside M20 instead of the host routing step.
- **CR-4** Scatter / pad / zero-embed, for slice, concat and gather
  backward.
- **CR-5** Multi-axis reduce or sum-to-shape, so unbroadcast is one call.

## Open questions

1. Whether forward/backward ops should write E5 tier (a) dispatch records.
2. Where NaN/Inf gradients are refused before an optimizer commit.
3. What "the same Forge/AEGIS path" (CEP E3) requires of a CPU-tier tape.
