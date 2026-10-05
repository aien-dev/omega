# Branch-native shared cognitive state

Status: see the receipt the gate writes (`BRANCH_REUSE/rx_branch_reuse_receipt.json`
under the run's evidence directory; a reviewed copy lives in `evidence/BRANCH_REUSE/`).
Only a run on a clean tree, with at least 100 branches, every correctness check
passing, all four comparisons won for every realizer and all eight actions taken
by construction and policy may print `OMEGA_BRANCH_STATE_REUSE_PASS`.

## Problem

J-Space explores many candidate branches that share most of their past. If each
branch realizes its whole state on its own, memory, compute, latency and energy
all grow with the number of branches times the length of the shared past. The
shared part should be realized once and reused, without tying what a state
*means* to how it is held.

## Two layers

**Semantic.** A branch is an ordered sequence of state units.

| Field | Meaning |
|---|---|
| `branch_id` | this branch |
| `parent_branch` | the branch it forked from (none for a root) |
| `common_ancestor` | the root of its lineage |
| shared semantic state | units inherited at the fork, still held in common |
| branch-local state | units this branch derived or edited itself |
| `divergence_point` | number of units inherited at the fork |

A unit's semantic identity is a SHA-256 chain over the semantic events that made
it: the root seed, a derive step and its token, or an edit (offset and patch
digest). A branch's identity is SHA-256 over its ordered unit identities. No
identity mentions a realization type, bytes, placement or residency.

**Physical.** A realization (`JsReal`, reported per branch as
`JsSharedStateRealization`) is one way the bytes of one semantic unit are held
right now.

| Field | Meaning |
|---|---|
| `semantic_state_id` | the unit it realizes |
| `realization_type` | latent checkpoint, activation checkpoint, KV state, compiled features, World projection, Cortex projection, physical tensor |
| `parent_realization` | recipe input: the realization it is rebuilt from |
| shared / private segments | a realization held by more than one holder is shared and immutable |
| `placement` | hot arena, cold arena, compressed (delta against parent), spilled (file), evicted (recipe only) |
| residency | bytes held in memory now |
| recompute / transfer / retain / eviction cost | measured, see below |

A realizer is a vtable (`type`, `unit_bytes`, `derive(prev, token)`). The store
and the policy never look inside the bytes and have no case for any type. The
test uses four realizers with different cost shapes: an integer latent
recurrence, a dense 128x128 neural layer, a sliding KV window, and a sparse World
projection where each step changes 8 of 128 objects.

## Operations and the rules that keep the ancestor safe

- `fork` gives the child references to every parent unit and freezes the parent.
  A frozen branch refuses derive and edit (`JS_ERR_FROZEN`): it is now a shared
  ancestor. Exploration continues in children.
- `derive` appends a unit computed from the last one. If a realization of the
  same semantic unit and type already exists anywhere, it is reused instead.
- `edit` never writes a realization anyone else holds. It makes a new
  realization with an EDIT recipe. The bytes come from a copy when the old unit
  is resident, from a rebuild when it is not, or from taking the old buffer when
  this branch was its only holder and nothing depends on it.
- A read returns the realized bytes wherever they are. Every rebuilt unit is
  checked against the content check recorded when it was first realized.

## Policy (FORGE)

`js_calibrate` measures on this machine, per realizer: derive time, copy,
compress and decompress per byte and the delta ratio of a derive step, spill
write and read per byte, and the price of residency (faulting fresh memory per
byte). The policy reads nothing else.

| Action | When |
|---|---|
| reference | fork: the child points at the parent's realization |
| reuse | derive or edit whose semantic unit is already realized with that type |
| copy | edit of a shared resident unit |
| recompute | rebuild an evicted unit (or chain) from its recipe |
| move | hot arena over its share: move to the cold arena |
| compress | under pressure, when predicted delta size and decode cost win |
| spill | under pressure, when file write and read win |
| evict | under pressure, when rebuilding later wins |

Under pressure, for each resident realization the policy prices every way of
giving up its bytes. The price is the work now plus the expected reads times the
restore cost, per byte given back. Restoring a delta includes producing its
parent's bytes, and rebuilding includes the chain to the nearest unit that is
held. It applies the cheapest action, cheapest candidates first, until resident
bytes are under budget. Keeping wins when the retain price is lower than the
best alternative. Expected reads are the number of branches holding the unit.

## Test and gate

`make test-branch-reuse PHYSICS_DIR=...` runs `build/rx_branch_reuse`
(defaults: 128 branches, 512-unit prefix plus root, 16 private units and 2 edits
per branch, 64 KiB units, so a 32 MiB shared prefix). Branch k and branch k+96
take the same steps. For every realizer the same branch program runs twice, each
in a fresh child process pinned to one Cortex-X925 core:

- independent: every branch recomputes and holds its whole state;
- shared: prefix once, fork by reference, copy on write, reuse, then the policy
  under a budget of a quarter of the resident bytes.

Measured: bytes held and resident-set growth; derive calls and processor time;
time until all branches are ready, first branch ready, and per-branch ready;
energy from the GB10 `aien_spbm` counters (package, efficiency and performance
clusters). The counters tick every 100 ms. A run shorter than 2 s is repeated
(whole construction and teardown), starting and ending on a tick, and energy is
reported per construction. Package energy includes the rest of the machine.
Branch creation time is also measured: for shared, the fork call; for
independent, building that branch's whole state.

Correctness checked:

1. every shared branch's bytes (SHA-256 over all units) equal its independent
   recomputation, before and after the policy moved them around;
2. the ancestor's identity and bytes equal a fresh prefix built from scratch;
   writes to it are refused and change nothing;
3. copy on write: exactly the edited units differ from the ancestor's
   realizations, in every branch;
4. branches that took the same steps hold the same realizations; branches that
   did not, do not;
5. semantic identities do not change when the policy changes placement, and one
   branch's every unit survives restore, move, compress, spill, evict, restore
   with identical bytes and identity;
6. the same branch program gives identical semantic identities under all four
   realizers and different bytes.

The gate also requires the shared realization to use less memory, less compute,
less time and less package energy than independent recomputation for every
realizer, and all eight actions to have been taken by construction, policy and
the reads that followed. The forced placement cycle in check 5 is not counted.

## Not claimed

- Graphics-processor residency. Both arenas and the spill file are host memory
  and a host file. On GB10 the processor and graphics memory are the same
  LPDDR5X, but no graphics realization is exercised here.
- Integration with the ADR 0016 object world (`rx_world`), the R9 generation
  store or the existing FORGE numeric pipeline. This is a standalone store and
  policy. Wiring J-Space candidates into World branches is later work.
- Concurrency. The store is single-threaded; both runs are too.
- Real models. The realizers are deterministic stand-ins with the cost shapes
  of the named representations. They are not trained networks.
- The policy's reuse estimate (expected reads = holders) is a simple prior, not
  a learned one. The spill file is never compacted.
- Energy isolation. Package energy includes whatever else the machine was doing.

If the `aien_spbm` counters cannot be read, the energy leg is reported as
`NOT_RUN` and the gate result is `PASS_ENERGY_NOT_RUN`, which never prints
`OMEGA_BRANCH_STATE_REUSE_PASS`. Memory, compute, latency and correctness stay
hard checks. When the counters are readable, energy must still measure lower.
