# M21 OMEGA_AUTODIFF: reverse-mode tape, CPU tier

Status 2026-10-01: **M21 NOT QUALIFIED.** The tape, its tests and its
mutation sweep are written. None of them has run yet: host NOT_RUN (the
forge fills it), GB10 NOT_RUN. M20, which this layer sits on, is itself not
qualified (GB10 parity NOT_RUN).

Plan source: `aien-architecture/CURRENT_EXECUTION_PLAN.md`, section
"E3. M21 OMEGA_AUTODIFF": "Omega forward graph -> differentiate -> Omega
backward graph. The backward pass is ordinary Omega and uses the same
Forge/AEGIS path." ROADMAP row M21: "Sovereign automatic differentiation
generating gradient semantic graphs", PLANNED. Neither document states an
M21 exit gate or a gradient tolerance. The gate below is a **PROPOSED gate,
pending owner approval**, not doctrine.

## Scope

A reverse-mode tape over the M20 OMEGA_TENSOR CPU tier
(`src/tensor/omega_tensor.h`). Forward: each recorded op is one M20 call.
Backward: walks the tape from the root down and builds every gradient with
M20 calls (ADD, SUB, MUL, DIV, matmul, transpose, reshape, broadcast_to,
reduce SUM), so the backward pass runs on the same E1 realization as the
forward pass. One exception: MAX routing is a host-side data move (compare
and copy, no arithmetic) in the caller's scratch buffer, because M20 has no
mask-producing op (CR-3 below).

Out of scope here: GB10 realization, transcendental activations (M20 has
none yet), slicing/concat/gather gradients, second derivatives, dispatch
provenance records for forward/backward ops (open question for E5), and
any edit of `src/tensor` or `src/train`.

## Files

| File | Role |
|---|---|
| `src/autodiff/omega_autodiff.h` | The meaning: ops, ownership, determinism rules, per-op error contract |
| `src/autodiff/omega_autodiff.c` | The tape: recording, backward walk, unbroadcast, M22 flatten. No heap allocation |
| `tests/test_omega_autodiff.c` | Contract, finite-difference, determinism, error-path and leak checks |
| `tools/autodiff_mutations.sh` | Source mutation sweep (10 mutants) |
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

## Test design (PROPOSED gate, pending owner approval)

`tests/test_omega_autodiff.c`, 21 op cases (every op, with and without
broadcasting, batched matmul) plus a composite graph:

1. **Contract layer.** Each tape gradient against an exact reference written
   in the test from the calculus (double precision, index arithmetic on
   shapes only), within the bound in the table above. Exact cases must be
   bit-exact.
2. **Finite-difference layer.** Central differences of the M20 CPU forward
   pass on perturbed float inputs, step h = 2^-6 (actual step taken from the
   rounded floats), loss L = sum(y * R) in double with a fixed random R used
   as the backward seed. Pass if |tape - fd| <= 5e-4 + 1e-3 |fd|. Inputs are
   kept away from kinks (MAX) and singular points (DIV, SQRT).
3. **Composite.** Linear layer + squared error, `mean_i sum_j (XW + b - Y)^2`,
   with a reused node (fan-out): closed form `(2/N) X^T E` within 1e-5 and
   FD on every parameter; X and Y have no gradient.
4. **Determinism.** Two full runs in two M20 contexts: flat gradients
   bit-identical (memcmp) and the loss value id identical.
5. **Error paths.** Tape full (no state change, no leak), shape mismatch
   (`[3,4]+[5]`, `[3,5]x[4,2]`, `[3,5]x[3,5]`, broadcast `[5]->[4]`, bad axis),
   wrong seed shape, scalar seed on a non-scalar root, too little scratch,
   second backward, recording after backward, unreached leaf, no-grad root,
   F16 leaf, flatten count mismatch (buffer untouched), bad node ids, leak
   check of the M20 context.

Counterexamples in process: the contract comparator must reject a gradient
off by 2^-20 relative and accept the exact one; the FD comparator must
reject a sign-flipped gradient; the determinism comparator must see a
one-bit flip.

Mutation sweep (`tools/autodiff_mutations.sh`), each must make the test
fail: TAPE_WALK (skip the root), ACCUMULATE (replace instead of add),
UNBROADCAST (skip size-1 axis sums), SUB_NEGATE, MUL_OPERAND, MATMUL_TRANSPOSE,
SQRT_TWICE, MEAN_DIVISOR (n+1), MAX_ROUTE (reverse comparison), MAX_TIE
(ties to a).

## Qualification checklist

| Item | Status | Evidence |
|---|---|---|
| Reverse-mode tape over M20 CPU ops | WRITTEN, NOT_RUN | `src/autodiff/` |
| Per-op error contract (contract layer) | NOT_RUN | `make test-autodiff` |
| Central finite differences | NOT_RUN | `make test-autodiff` |
| Composite graph end to end | NOT_RUN | `make test-autodiff` |
| Determinism (bit-identical rerun) | NOT_RUN | `make test-autodiff` |
| Error paths and leak check | NOT_RUN | `make test-autodiff` (plain and ASan/UBSan) |
| Mutation sweep (10 mutants) | NOT_RUN | `make test-autodiff-mutations` |
| Host run (forge) | NOT_RUN | forge fills this |
| CI wiring | MISSING | no `autodiff` job in `host-suites-2.yml` yet |
| GB10 | NOT_RUN | no GB10 realization of M20 yet |
| Owner approval of the PROPOSED gate | MISSING | no M21 exit gate in CEP or ROADMAP |
| Reproducible receipt | MISSING_IMPLEMENTATION | no receipt writer for this gate yet |

**M21 verdict: NOT QUALIFIED** until the host run passes and a GB10 chip
receipt exists.

## Tensor API change requests (not made here; owner LT-M20)

- **CR-1** Unary ops beyond SQRT: EXP, LOG and the E1 bounded set (SIGMOID,
  TANH, RSQRT, EXP2, LOG2, ERF, SIN, COS, GELU) in `OmegaTensorUnaryOp`. The
  scalar functions exist; the tensor enum has only SQRT. Needed for any
  non-linear activation except relu.
- **CR-2** Constant helpers (full / zeros / ones / neg). Convenience; the
  tape builds constants with `omega_tensor_from_f32`.
- **CR-3** A mask-producing op (compare returning 1.0 / 0.0, or
  where(pred, x, y) with a separate predicate). Lets MAX/MIN/relu/abs and
  reduce MAX/MIN backward stay inside M20 instead of the host routing step.
- **CR-4** Scatter / pad / zero-embed, for slice, concat and gather
  backward.
- **CR-5** Multi-axis reduce or sum-to-shape, so unbroadcast is one call.

## Open questions

1. Owner approval (or an ADR note) of the PROPOSED gate and tolerances.
2. Whether forward/backward ops should write E5 tier (a) dispatch records.
3. Where NaN/Inf gradients are refused before an optimizer commit.
4. What "the same Forge/AEGIS path" (CEP E3) requires of a CPU-tier tape.
