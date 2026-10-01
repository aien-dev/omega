# Replay with concurrent commits: causal-order equivalence

Lane LD replay suite (`mk/replay.mk test-replay`). This note covers the
4-worker replay checks: what the engine does today, what the suite compares,
and what it leaves out.

## Known engine property (runtime finding I11)

With one worker, a World replay is crumb-identical to its recording (the
strict sequence checks `world-replay-1-worker` and `trn1-replay-1-worker`
are gates and pass). With four workers the engine logs committed history
in schedule order. Decoded from the five 4-worker replays of forge run
LD-174654 (`build/replay/run/world`, decoded by shell/awk, nothing rerun):

- Causally unrelated reactions (`omega.realize`, `omega.risk`) commit in
  either order. All five replays first differ from the recording there.
- `omega.risk`'s merged-wake count (`coalesced_wakes`) is 1 with one worker
  and 0 with four, for the same stimulus.
- A run that read a stale belief is superseded (an INVALIDATED crumb,
  reason STALE_GEN, then a rerun) in only some replays (2 of 5).
- Every crumb id after the first difference shifts, so the CHECKPOINT field
  writers for `plan` and `risk` differ by crumb id.

Runtime finding I11 asks the engine for a deterministic commit order. Until
then the strict 4-worker sequence checks fail. That is an engine property,
not a replay harness bug, and the suite does not hide it (below).

## What the suite gates on

`rx_replay compare-causal EXPECTED ACTUAL` compares committed history as a
causal partial order. The rules are in the comment above `compare_causal`
in `rx_replay.c`. In short:

- **Identity.** An entry's causal identity is a digest of its content (kind,
  reaction, faculty, inputs and outputs with versions and masks, caps,
  reason) plus the identities of its wake cause and of every parent.
  Crumb ids are not part of it. Cause links are, compared by identity all
  the way back to the first crumb.
- **Per stimulus.** The log is cut at INPUT records (the replayer waits for
  quiescence after each). INPUT contents must be equal and in order.
  Within each stimulus, the multiset of entry identities must be equal.
- **Superseded entries** (INVALIDATED) are left out of that multiset. Each
  must be flagged (kind INVALIDATED, nonzero reason, no outputs) and paired
  with its rerun: a later entry of the same reaction, in the same episode
  and stimulus, that is not itself INVALIDATED. A run logged as committed
  that the same reaction then repeats in the stimulus is reported as an
  "unflagged superseded entry" and fails.
- **Snapshot writers** are matched by identity, not crumb id. To make this
  possible, CHECKPOINT records now carry the state table they hash (object
  id, generation, type, version; per field value, field version, writer).
  `rx_replay verify` recomputes the state hash from the table, so the table
  cannot disagree with what the recorder hashed. Values and versions must be
  equal exactly.
- **Excluded:** the merged-wake count (`coalesced_wakes`). It stays in the log,
  in the crumb digest and in the strict sequence compare. Only the causal
  compare leaves it out, because how many wakes the scheduler folds into one
  run is a scheduling fact. Worker and clocks are excluded, as in every
  compare. Positions (crumb ids, INPUT `after_crumb`, CHECKPOINT
  `through_crumb`) are excluded too, but each log must still pass its own
  `verify` rules, which check them.

The gating check is `trn1-replay-4-workers-causal`: all five 4-worker
replays must MATCH the 1-worker recording under compare-causal. It compares
the RXCLOG01 logs that the TRN1 transcripts are exported from, because a TRN1
CHECKPOINT carries only the state hash, which does not let writer identity
be recovered. `world-replay-1-worker-causal` checks the 1-worker replay the
same way.

## Mutants and controls (the gate is only as good as these)

`rxlog_mutate causal IN OUT NAME` forges each change into the recording,
renumbers every id that refers to a crumb, and reseals the log so it passes
`verify` by itself (the suite checks that). Only the comparison can catch it.

Each of these must make the causal gate FAIL:

| mutant | change |
|---|---|
| `causal-cause-link` | an entry wakes on a different earlier crumb |
| `causal-parent-link` | an entry loses a parent |
| `causal-writer-swap` | two snapshot writers swapped (state hash recomputed) |
| `causal-unflagged-superseded` | a stale run of a reaction logged as committed before its real run |
| `causal-entry-changed` | a committed entry's output version changed |
| `causal-superseded-no-rerun` | a flagged superseded entry with no rerun after it |

Every existing sequence-compare world mutant must also fail compare-causal
(except the `excluded-*` worker and clock controls). The three replay
negative controls (value, structure, input) must fail it as well.

Each of these must MATCH under compare-causal and still fail the strict
sequence compare. They show that the tolerance is exactly the stated one:

| control | change |
|---|---|
| `causal-control-swap-independent` | two causally unrelated commits swapped and renumbered |
| `causal-control-flagged-superseded` | a flagged superseded run inserted before its rerun |
| `causal-control-coalesced` | the merged-wake count changed |

## How the strict 4-worker checks are reported

`world-replay-4-workers` (5 replays, RXCLOG01) and `trn1-replay-4-workers`
(TRN1) stay in the suite. They are not gates. Each run reports one of:

- `ok`, if the strict compare matches;
- `KNOWN_FAIL ... [non-gating; runtime finding I11 ...]`, if it reports
  DIVERGENCE;
- `FAIL`, if it gives no verdict at all (tool error or refusal). That fails
  the suite.

A KNOWN_FAIL never counts as a pass. The receipt lists it on
`strict_4_worker_sequence:` and in the verdict itself, for example
`verdict: PASS (KNOWN FAIL (non-gating, runtime finding I11): world-replay-4-workers trn1-replay-4-workers)`.
Once the engine commits in a deterministic order, these lines turn into `ok`
with no change to the suite.

## Second engine property, seen while building this (gate expected to FAIL)

The five 4-worker replays of the next run (2026-10-01 13:07, the old queued
job on a dirty tree, so reported only as decoded evidence) show something
the causal compare does not tolerate, by design. In 4 of 5 replays,
`omega.risk` commits twice for one stimulus. It first runs on the humidity
wake alone, reads the belief before `aien.hypothesis` commits (a consistent
but older snapshot), and commits. Then it runs again on the belief wake and
commits again. With one worker the two wakes are merged and `risk` commits
once. Each unmerged wake adds exactly one committed `risk` run:

| replay | crumbs | `risk` commits | merged wakes |
|---|---|---|---|
| recording (1 worker) | 88 | 24 | 9 |
| replay4-1 | 88 | 24 | 9 |
| replay4-2 | 93 | 29 | 4 |
| replay4-3 | 90 | 26 | 7 |
| replay4-4 | 89 | 25 | 8 |
| replay4-5 | 90 | 26 | 7 |

The extra run is not INVALIDATED. It is a real commit, its value is
overwritten in the same stimulus, and the `risk` object's version stays
ahead from then on. So leaving the merged-wake count out of the comparison
does not make these histories equal. The extra entry and the version offset
are real differences in committed history and state. The causal gate
reports them (as an unflagged superseded entry) and fails, as its
`causal-unflagged-superseded` mutant requires. Accepting these runs would
mean ignoring object versions, and that is out of scope for the verifier.
This belongs with runtime finding I11: a deterministic commit order needs
deterministic wake merging too.
