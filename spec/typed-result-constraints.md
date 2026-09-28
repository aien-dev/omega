# Omega typed result constraints (OMEGA_TYPED_RESULT_CONSTRAINTS)

Code: `src/runtime/rx_contract.{c,h}`. Test: `tests/runtime/rx_typed_results.c`,
`make test-typed-results` (`TYPED_RESULTS_N`, default 400 requests per kind
and path). Receipt: `evidence/TYPED_RESULTS/`.

## Purpose

When cognition is asked for a structured semantic object, an invalid answer
must be impossible to generate, or be caught before it is published. The
contract an answer must meet is typed Omega state. JSON is one way to write
an object down; it is never the contract, and a JSON text is only accepted
after it has been parsed back into typed state and checked.

## Contracts

A contract (`RcContract`) is typed fields, a generation order, and a table of
rules. Its identity is SHA-256 over a canonical binary encoding of kind, field
types, derived flags, order, rules and the published object type. Field names
are serialization keys and are not part of the identity.

| Contract | Fields | Published as |
|---|---|---|
| Plan | AIEN plan layout (seq, action, regime, condition, hypothesis, reason, goal) | `RX_OT_PLAN` |
| Hypothesis | AIEN hypothesis layout (seq, kind, prediction, classes, expected, observed, state) | `RX_OT_HYPOTHESIS` |
| CapabilityNeed | principal, operation, resource, rights ceiling, effect classes, locality, latency budget, approval | contract type |
| ActionGraph | digest, subject, node/effect/evidence counts, goal kind; the `AgGraph` is attached | contract type |
| Skill invocation | skill, arity, three arguments, attempts, principal | contract type |
| World mutation | object, generation, field, value, principal, resource (derived) | contract type |
| Effect proposal | effect resource, value, verdict evidence, irreversible, approval, principal, kind | contract type |
| Generation candidate | candidate, parent, authority epoch, evidence, proofs, objects, proposer, lineage | contract type |
| Evidence | subject kind, subject, run, checks, failures, verdict, digest, producer | contract type |

CapabilityNeed follows the capability query's meaning (`rx_capq.h`, what AIEN
needs done, not who does it). Its fields are the part of `CqNeed` a single
semantic object carries; the query's type lists and trade-offs stay in the query.

### Constraint types

| Type | Rule | Examples |
|---|---|---|
| type | field type (u32, bool, enum < 64, bits, object id, subject, resource) | every field |
| range | `f in [k, k2]` | plan regime, hypothesis expected, evidence checks |
| enum | `f in set` | plan reason, hypothesis state, effect kind |
| structural | nonzero, bits within a mask, attached graph validates | rights ceiling, evidence digest, ActionGraph |
| cross-field | guarded comparison between fields | reason GOAL needs a goal; PASS has no failures |
| authority | principal is the producer; principal holds rights now; no privileged rights | every kind; mutation and effect targets |
| generation | object live at that generation; parent is the active generation; next; epoch | mutation, generation candidate |
| referential | context object field, next sequence, skill and arity, live object, resource of object | plan, hypothesis, skill, mutation |
| required evidence | a published PASS evidence record names this subject | effect verdict, generation evidence |
| effect | forbidden resources, allowed effect resources, effect budget, approval for irreversible, graph clear | capability need, effect proposal, ActionGraph |

Authority is checked through the world's authority view (validation, never
mint). `rx_contract.o` is linked alone first and the build fails if it
references an AIENOS admin operation.

## Two paths

**Constrained.** `rc_compile` decides, per rule, whether it can become a
per-field domain given the fields before it in the generation order.
`rc_domain` then gives the allowed values of one field (interval, enum, bit
mask, explicit set, exclusions, excluded interval) from the fields already
chosen and the live context. A backend that decodes under a mask only
chooses inside it. Rules that cannot become a domain (the attached graph's
structure, the graph's forbidden requirements) are checked once the candidate
is complete. An empty domain on the first field means nothing could be valid
in this context: the request ends before anything is generated.
An empty domain later is a dead end and costs a retry (there is no lookahead).

**Free.** `rc_produce` generates, checks, repairs what the contract marks
repairable, otherwise rejects and generates again, up to a bound.

Both paths end in `rc_check`. Nothing is returned as publishable unless it
passed.

### Repair

Two repairs exist, both declared on the rule:

- **derive**: recompute a field the contract marks derived (sequence number,
  an object's resource, a graph's digest and counts);
- **narrow**: clear bits a rule forbids (a capability need asking for fewer
  rights).

Nothing else is repaired. Stale generations, missing evidence, wrong
principals, broken references and effect restrictions reject.

### Generation order

Derived fields come after the fields they are derived from. The World
mutation contract first chose its resource and then objects of that
resource. A slip in the resource then steered the object, so it was
published valid but wrong. It now chooses the object and derives the
resource.

## The publish gate

`rc_gate_register` adds one resident reaction per contract (faculty OMEGA).
Its trigger is the draft object that cognition writes. It holds the only WRITE
capability on the published object. It checks every draft again against the
contract and the live World, applies only the repairs above, and either
publishes all fields or records the rejection in its status object (verdict,
first violated rule, violated constraint types, counts). A draft that
produce accepted can still be rejected there: the World may have changed
between generation and publication.

Cognition holds WRITE on drafts and READ on what is published. A
publication around the gate is refused by the authority. So is a reaction
of cognition that presents the gate's capability.

## Serialization

- canonical binary (`rc_encode` / `rc_decode`): kind, contract identity, fields;
- object identity: SHA-256 of that encoding, with the attached graph's digest;
- JSON (`rc_to_json` / `rc_from_json`): one flat object. The parser is strict:
  exact keys, unsigned decimal integers only, no duplicates, no unknown keys,
  nothing after the object.

## What the test proves

| Claim | How |
|---|---|
| every constraint type is defined and caught | each type used by a contract; each type reported on a real violation |
| the checker is right | every sweep candidate also judged by a hand-written reference per kind; zero disagreements; every single mistake reported under its own type |
| compiled domains are sound | uniform draws inside every domain never violate a compiled rule |
| compiled domains are complete | every field of every intended answer lies inside its domain given its own prefix |
| impossible where fully compiled | constrained path: zero invalid candidates for every contract with no after-check rule |
| never publish invalid | every published object re-read from the World and re-checked by contract and reference |
| no bypass | cognition's direct write and a stolen-capability reaction both blocked; every published field written by its gate |
| hostile drafts | every catalogued mistake written straight into the draft: rejected, or repaired only when repair is declared and restores the intended meaning |
| stale before publication | a mutation target retired after generation is rejected by the gate as a generation violation |
| unsatisfiable context | effect budget exhausted: constrained path stops with zero draws; free path spends every retry |
| JSON is one serialization | binary and JSON round trips keep the identity; malformed texts refused; faulty JSON candidates caught |

### Measures (per path, per kind, in the receipt)

invalid candidate rate, repair rate, retries, draws and checks (compute),
compute avoided (constrained against free), produce time and end-to-end
latency (p50, p99), semantic failure rate (published objects whose
meaning-bearing fields differ from the intended answer), rejected and
unsatisfiable requests.

### The backend

The backend is synthetic. It has an intended answer (valid by construction)
and makes mistakes at a fixed rate (35 % of candidates, a quarter of those
with a second mistake) from a catalogue per kind. Each mistake is labelled
with the constraint type it breaks, or "valid but wrong". Both paths see the
same mistakes for the same request and attempt. Under a mask, a backend
whose preferred value is not allowed takes its intended value if allowed,
otherwise the nearest allowed one. That is the model of masked decoding the
measures rest on.

## Findings

- Constraints guarantee validity, not truth. A valid-but-wrong answer passes
  every contract on both paths and is counted as a semantic failure.
- The constrained path's semantic failure rate is close to the free path's,
  not lower. A mistake that breaks one field and silently changes another is
  half-fixed by the mask and published. The free path rejects the whole
  candidate and draws again. In this model, a retry often clears both.
- Draws avoided are the compute saving that transfers to a backend whose
  draw is expensive. The synthetic draw costs almost nothing. Computing
  domains means World scans, authority validation and graph validation. So
  here the constrained path is slower in wall-clock time, not faster: its
  produce time is a few times the free path's, and end to end (gate
  included) it is somewhat slower. It pays off only when one backend draw
  costs more than one domain computation.

## Limits

- The backend is synthetic, not neural. No LLM or learned decoder is claimed.
- AIEN's live R11 reactions do not yet write through the gate. Plan and
  Hypothesis contracts use their exact layouts, so routing them is a wiring
  step.
- Domains have no lookahead. A choice can lead to an empty later domain, and
  that costs a retry (counted as `dead_ends` in the receipt).
- The gate validates an attached ActionGraph on a worker thread.
  `rx_graph_validate` reads the World's object table without the world lock
  (this predates this gate; TSan was clean on this workload).
- The gate reads World objects outside its declared snapshot (for generation
  and reference checks). An object retired between that read and the commit
  is not re-checked at commit.
- The evidence registry is bounded (256 records). Evidence it could not keep
  cannot be cited.
- Host processor only; the authority is the native C library, not the AIENOS
  kernel.
