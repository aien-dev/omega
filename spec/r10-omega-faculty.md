# R10: Omega as a resident realization faculty

ADR 0016 §43. Gate `R10_CONTINUOUS_OMEGA_PASS`.

## Rule

Omega is not a service. Nothing calls Omega and waits for an answer.

```
world state changes
      ↓
Omega dependencies become ready
      ↓
Omega reaction acts
      ↓
publishes objects/evidence
      ↓
other faculties naturally become ready
```

## What runs

Code: `src/runtime/rx_omega.{c,h}`, `src/omega_matvec_quad.{c,h}`.
Test: `tests/runtime/rx_r10_omega.c`, `make test-r10` (AArch64 hosts only).

The operation is Omega's integer matrix-vector product from M12
(`omega_matvec`): `y[i] = Σ_j A[i·N+j]·x[j] mod 2^64`, for M ≥ 1 and N ≥ 1.

| Reaction | Wakes on | Publishes | ADR 0016 object |
|---|---|---|---|
| `workload.matvec.serve` | `request` | `result`, `demand` | production path |
| `omega.watch` | `demand` window field only | `search` | CostEstimate |
| `omega.synthesize.k` | `search` epoch | `candidate[k]` | RealizationCandidate |
| `omega.verify.k` | `candidate[k]` | `verdict[k]` | verification evidence |
| `omega.measure.k` | `verdict[k]` | `measure[k]` | measured cost |
| `omega.select` | every `measure[k]` | `selection` | recorded, eligible |

Production reads `selection` as an ordinary input on its next request. It does
not wait for it. A regime runs the semantic reference until a selection for
that regime exists.

- **Hot:** a regime is hot when its calls and its total measured time both
  cross the configured threshold. Production publishes the evidence every 16
  calls, so `omega.watch` wakes on fresh evidence, not on every call. A regime
  already searched is not searched again.
- **Synthesis:** one reaction per candidate slot. Slots 0–2 are the qualified
  M12 emitters, unchanged. Slot 3 is `quad4` (below). Code sits in a
  content-addressed store owned by the faculty. World objects carry only the
  32-byte realization identity. An identity may never be rebound to different
  bytes.
- **Verification:**
  1. Recompute the identity from the stored bytes.
  2. Run Omega V0 structural verification.
  3. Run a differential check against `omega_matvec_reference` in a forked
     child: 87 shapes × 3 inputs (random, edge values, wrap-around). It also
     checks guard words past `y`, and that `A` and `x` are not modified. A
     crash or hang ends only the child.
  4. Code runs in the world's own process only after its verdict passed, and
     only under its full 32-byte identity.
- **Measurement:** best of 7 rounds. The candidate and the incumbent
  reference are timed back to back in the same rounds, at the searched regime.
  A refused candidate is declined without being run.
- **Selection:** the fastest measured candidate that beats its paired
  reference by 5% or more. If none does, the reference is kept and that is
  recorded too.
- **Authority:** production and Omega are separate subjects under the native
  AIENOS authority. Omega can read `demand` but cannot write any production
  object. A revoked Omega capability blocks that reaction, and production
  carries on.

## Findings

1. **M12's three emitters never beat the compiled reference on the DGX
   Spark.**
   - On Cortex-X925 they land between 4% faster and 8% slower than it.
   - On Cortex-A725 they are 1.8–2.1× slower.
   - Cause: every element load post-increments its base register. Each row
     is therefore one serial chain of address updates.
   - `omega_matvec_dispatch`'s fixed rule (unroll4 when N > 8) therefore picks
     a slower realization on A725 cores.
2. **`quad4`** is a new Omega emitter. It keeps four independent
   accumulators, loads at fixed offsets, and moves each base once per four
   elements. Pinned single-thread timings at 64×256, in µs per call:

   | Core | reference | gcc -O3 acc4 | quad4 |
   |---|---|---|---|
   | X925 | 4.58 | 2.64 | 2.69 |
   | A725 | 6.27 | 5.92 | 6.08 |

   `quad4` matches the compiler on both core types. It lives in its own file
   so the M12 sources keep the digests recorded in
   `evidence/m12_corpus_digests.txt`.
3. **The best realization depends on the core class.** The R10 test pins
   itself to the X925 cores when they exist, and records that in its receipt.
4. **Engine fix (`rx_world.c`).**
   - The bug: an activation invalidated by a change to a read-only input was
     never run again, so the stimulus that woke it was dropped.
   - R10 hit it when a new selection landed while production was serving a
     request.
   - Invalidation now re-arms the activation under the same cause. Conflict
     limits still apply.
   - Regression test: `invalidation_by_read_only_input_reruns` in `test-r3`.
     It fails without the fix.

## Not claimed / limits

- One operation, CPU realizations only. No graphics-processor realization
  here.
- Candidates come from a fixed emitter family. This is not open-ended search
  through `omega_synthesis` or `omega_realize_synth`.
- One `selection` object. Only the most recently searched regime has a
  record. Choosing a realization per core class is not implemented.
- The realization store is process memory. It does not survive a restart and
  does not go through the R9 generation barrier.
- The sandbox is a forked Linux process, not an AIENOS-enforced sandbox.
- The production reaction is labelled `RX_FACULTY_EXTERNAL`. It is not AIEN
  (R11).
- The M12 emitters return without writing `y` when N = 0. The reference
  writes zeros. N = 0 is outside the declared domain, and verification does
  not test it.
- Hosted CI runs on a different AArch64 core. There, a win is recorded, not
  required (`R10_ALLOW_NO_WIN=1`, gate `CHAIN_PASS_NO_WIN`). The gate is
  claimed only by a clean, candidate-bound run on the DGX Spark.
