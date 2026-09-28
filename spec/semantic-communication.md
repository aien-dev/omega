# Semantic communication pruning (OMEGA_SEMANTIC_COMMUNICATION)

Code: `src/runtime/rx_semcomm.{c,h}`. Test: `tests/runtime/rx_semantic_comm.c`,
`make test-semantic-comm`. Receipt: `evidence/SEMANTIC_COMM/`.

## Purpose

Branches and cognitive workers exchange only the state the receiver needs.
Nobody broadcasts an internal history or the whole J-space World. A receiver
declares what it is about to do; Omega answers with the part of the World
that operation needs, checked against the receiver's own authority; after the
first delivery only what changed travels.

## Boundaries

| Party | Role here |
|---|---|
| Receiver (branch, worker) | declares an `InformationNeed`; holds its own capabilities |
| Omega | computes the `SemanticProjection`, its encoding and deltas. No admin handle. Reads the World only. |
| World (J-space) | objects, fields, per-field versions, writer crumbs (rx_world) |
| AIENOS | the only minter (native C capability authority) |

`rx_semcomm.o` is linked alone first. The build fails if it references any
`aienos_cap_*` admin operation, `rx_caproot_mint`/`revoke`, or a World write
(`rx_world_publish*`, `create`, `retire`, `add_reaction`).

## InformationNeed

```
RxInformationNeed {
    receiver                    principal (capability subject)
    operation                   INSPECT(field mask) | SUM(field) | MAX(field)
    required_types[]            0 = any type
    relevant_objects[]          object references (id + generation); 0 = every object of the types
    relevant_time_range         [from_crumb, to_crumb] on each field's writer crumb; to 0 = open
    evidence_requirement        NONE | WRITER (crumb id) | DIGEST (crumb id + content digest)
    uncertainty_requirement     max ppm (0 = any) and whether to deliver the value
}
```

Time is causal: a field's time is the id of the crumb that last wrote it
(`rx_world_explain`). Crumb ids are dense and monotonic in one world.

Uncertainty is declared, not estimated. A type may name one field as its
producer's uncertainty in parts per million (`RxSemType.uncertainty_field`).
Objects above the bound are left out; `deliver` adds that field.

## SemanticProjection

```
RxSemanticProjection {
    object_refs[]      reference, type, resource, delivered field mask, conclusion flag
    fields[]           (object, field, value, version)
    derived_values[]   SUM or MAX computed by Omega (omega_eval_pure_binary_uint) instead of the inputs
    evidence_refs[]    (object, field, writer crumb [, crumb digest])
}
```

Selection per live object, in id order:

1. type in `required_types`, reference in `relevant_objects`;
2. the fields the operation needs whose writer crumb is inside the time range;
   none left means the object is out of scope;
3. READ on the object's resource, validated through the world's authority
   view for the receiver's principal; otherwise withheld;
4. uncertainty bound;
5. fields the type marks private need READ on the private resource
   (`resource | 1<<62`); otherwise those fields are withheld.

INSPECT delivers the selected fields. SUM and MAX deliver the object
references (provenance) and one derived value; the inputs do not travel.
Evidence, when required, covers every field the operation used, delivered or
folded into a derived value.

## Authority

A projection carries values and references. It carries no capability. A
receiver that wants to write still needs its own WRITE capability, which the
engine validates when the reaction starts and again at publication. The test
tries three writes with nothing but a projection and a READ capability: an
outside publication, a reaction that claims WRITE on the READ capability, and
a reaction that declares READ honestly. All three are refused; a principal
that really holds WRITE commits (control).

Revocation is seen on the next delivery: the next projection no longer
contains the object, so the delta carries an invalidation.

## Encoding

One canonical little-endian format for every mode, so byte counts compare.
Header 21 bytes (magic `SCM1`, kind, receiver, sequence, record count).

| Record | Bytes | Content |
|---|---|---|
| O object | 20 | id, generation, type, resource, conclusion flag |
| F field | 20 | id, field, version, value |
| E evidence | 12 | id, field, writer crumb |
| D evidence + digest | 44 | as E, plus the crumb's SHA-256 |
| V derived | 22 | slot, op, field, value, argument reference, input count |
| X drop object | 8 | id, generation, reason |
| Y drop field | 5 | id, field, reason |

Kinds: FULL_STATE (broadcast baseline), PROJECTION (replaces the receiver's
view), DELTA (edits it).

## Delta delivery

The sender keeps one cursor per receiver: which objects, generations, field
versions and evidence crumbs it last delivered, and the derived values. A
delta carries, in order:

- invalidations: objects that left (reason RETIRED, AUTHORITY, UNCERTAIN or SCOPE);
  a slot reused by a new generation drops the old one first;
- new objects with all their fields and evidence; objects of a conclusion type
  are new conclusions;
- for objects already held: fields that left, fields whose version changed,
  evidence whose writer crumb changed;
- derived values that changed.

Unchanged state never travels. With nothing changed a delta is a bare header.

## Test (swarm simulation)

World: 180 objects in 6 types over 12 capability domains, 16 teams (each READ
on 4 domains, even teams also on one private resource), 24 rounds. Each round:
12 objects get 1–3 field writes; every 4th round an object is retired and
replaced (new generation); every 3rd round a new conclusion object appears;
round 12 revokes team 3's READ on its home domain; round 18 grants it again.

Workers: 32, 100 and 500, with partially overlapping tasks. Nine in ten draw an
operation, one or two types, a window of 12–48 objects from one of 24 task
groups (or all objects of the types), sometimes a time range, an evidence mode
and an uncertainty bound. One in ten is a coordinator: every type, every
object, every data field, writer evidence.

Every round, from the same World at the same moment:

| Mode | What each worker gets |
|---|---|
| full-state broadcast | every live object, every field, its version and writer crumb id |
| semantic projection | its projection, fresh |
| projection + delta | its first projection, then deltas |

The broadcast baseline carries no crumb digests; that makes it smaller than a
true full state, and DIGEST workers in broadcast mode are not charged for
missing digests. Both choices favour the baseline.

Each worker then performs its operation. A reference evaluator, written apart
from `rx_sem_project`, applies the same need to a copy taken straight from the
World under the worker's authority; that is the truth. Broadcast workers run
the same evaluator on what they decoded (they filter, and check authority,
themselves).

Measured per size and mode: encoded bytes, sender and receiver thread CPU
time, per-round latency (sender + all receivers, one thread), receiver held
bytes and sender state bytes (broadcast buffer or delta cursors), authority
validations, task success, missing-required-information failures, and fields
delivered without authority. Energy: the Spark's `aien_spbm` hardware counters
(pkg, cpu_p, microjoules) over whole scenario runs, four interleaved blocks of
at least 1.5 s per mode (world-only, broadcast, delta), medians, raw.

Also checked: a view rebuilt from deltas hashes the same as a view built from
the fresh projection (every worker, every round); the same need on the same
World gives identical bytes whatever order capabilities are held in; after a
round with no change every delta is empty; every worker that held revoked
state is told; a deliberately under-declared need (one type left out) is caught
as missing information (negative control).

## Gate

`OMEGA_SEMANTIC_COMMUNICATION_PASS` iff every check passes and, at every size:
projection bytes < broadcast bytes and delta bytes < projection bytes;
missing-required-information 0 in every mode; fields delivered without
authority 0 for projection and delta; task success 100% in every mode;
delta/fresh equivalence 0 mismatches; determinism 0 mismatches; quiet deltas
empty; revocation delivered to every holder; the negative control detects
missing information; no write through a projection. Energy is measured and
reported, not gated.

## Not claimed

- Transport over a network or between processes: delivery is in-process
  decode of the real encoded bytes.
- Graphics-processor senders or receivers.
- The AIENOS kernel: the authority runs as a host library.
- Learned or inferred needs: needs are declared.
- Uncertainty estimation: Omega carries the producer's declared value.
- Worlds above 256 objects (`RX_MAX_OBJECTS`).
- Sender cost: projection spends sender CPU and a cursor per receiver that
  broadcast does not; the receipt reports both.
