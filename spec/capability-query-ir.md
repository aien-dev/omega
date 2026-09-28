# Omega capability query IR (OMEGA_CAPABILITY_QUERY)

Code: `src/runtime/rx_capq.{c,h}`. Test: `tests/runtime/rx_capability_query.c`,
`make test-capability-query`. Receipt: `evidence/CAPABILITY_QUERY/`.

## Purpose

AIEN says what capability it needs. It does not need to know in advance every
Skill, tool, device or machine that could provide it, and it never reads the
catalog. Omega compiles the need into probes of the capability sources and
returns a short list of candidates. Each candidate carries its values on
explicit dimensions. The caller's own priorities decide the order.

## Records

```
CapabilityNeed (CqNeed) {
    semantic_operation         operation id or alias
    accepted_input_types       types the caller can supply (bit set)
    required_output_types      types the result must include (bit set)
    effect_class               accepted effect classes (docs/04: PURE .. SECRET_BEARING)
    authority_ceiling          resource range + most rights it may use
    locality_constraints       local / Fabric, optionally one machine
    latency_budget             microseconds (0 = none)
    energy_budget              microjoules (0 = none)
    reliability_requirement    minimum, ppm
    evidence_requirement       NONE < DECLARED < MEASURED < RECEIPT
    sources                    optional restriction
}

CapabilityCandidate (CqCandidate, 88 bytes) {
    capability_id, skill_id, machine_id, realization_id
    required_authority         resource, rights, held (validated now)
    expected_cost, expected_latency, expected_energy
    confidence, reliability    ppm
    evidence                   ref + level
    source, local, effects
}
```

No field of a candidate points into the catalog. A description never leaves
the catalog.

## Sources

| Source | What it holds |
|---|---|
| Capability Graph | native AIEN capabilities |
| Skill Network | Skill Net procedures (a candidate's `skill_id`) |
| MCP registry | MCP tools; a dead session makes its tools unavailable |
| local physical | this machine's devices |
| Fabric | capabilities another machine advertises; live only while its lease holds |

Each operation declares which sources can realize it (a physical operation
has no MCP realization; a tool operation has no physical one). The catalog
refuses a registration outside its operation's sources, and only the Fabric
may speak for another machine.

## Compile

`cq_compile(catalog, need) -> CqPlan`:

1. resolve the name (alias or id) to the canonical operation;
2. add every specialization of it (an operation's children, recursively; at
   most 32);
3. keep the sources that can realize any of them, that the caller allows,
   and that the locality allows (local only drops the Fabric; Fabric only
   keeps nothing else; a pinned machine keeps one side);
4. the plan's identity is SHA-256 over the resolved operation set, the
   sources and every constraint. Asking by alias or by id gives the same plan.

An unknown operation gives `CQ_E_NO_OP`; no applicable source gives
`CQ_E_NO_SOURCE`.

## Run

`cq_query` visits only the plan's (operation, source) buckets of an index
built once over the catalog. An entry is feasible when all of these hold:

- live: not withdrawn, its MCP session alive, its machine's lease valid;
- locality allowed, and the pinned machine if any;
- inputs it requires are a subset of what the caller can supply;
- outputs it produces include everything required;
- its effect classes are a subset of those accepted;
- its rights are within the ceiling, and a resource it needs is in range;
- latency and energy within budget; reliability and evidence at least the
  minimum; the caller's cost maximum and confidence minimum.

`held` is decided for each feasible candidate by validating the principal's
capabilities through the world's authority view (`rx_world_validate_cap`).
The query mints nothing. `rx_capq.o` is linked alone first and the build fails
if it references any AIENOS admin operation.

## Ranking

There is no relevance score. Each candidate has seven values to minimize:
cost, latency, energy, 1 - confidence, 1 - reliability, authority not held,
not local. The caller (`CqTradeoffs`) names the dimensions that matter in
priority order, a tolerance band on each, a cost maximum, a confidence
minimum and K.

1. **Pareto front** over the named dimensions: drop every candidate some other
   candidate beats or equals on all of them and beats on one. (Sorted by the
   named dimensions, a candidate can only be dominated by one before it, and
   only front members need checking.) `include_dominated` skips this step.
2. **Caller's order**: on the first dimension keep every candidate within
   `best * (1 + tolerance)`, let the next dimension decide among those, and
   so on; the strict order (dimensions, then ids) breaks what remains. Pick,
   remove, repeat until K.

Two callers with different priorities get different winners from the same
candidates; the test checks this.

## What crosses into cognition

`CqResult`: verdict, how many were returned, how many were feasible, how
large the front was, and at most K = 16 candidates. `cq_result_bytes` is the
header (16 bytes) plus 88 bytes per returned candidate, at most 1,424 bytes
whatever the catalog size.

## Consumption

`cq_bind_skill_node(graph, node, candidate)` points an action graph SKILL node
at the candidate's procedure and declares the candidate's authority on it.
`rx_graph_compile` then finds the authority held, or reports exactly that
requirement missing, and routes it to AEGIS as usual.

## Gate evidence (what the test proves)

| Measure | How |
|---|---|
| candidate recall | every query against an oracle in the test: a full scan of every entry with a separately written match rule, Pareto front and selection. The returned list must equal the oracle's ranked list. |
| selection precision | every returned candidate is feasible by the oracle's rule; the top candidate equals the oracle's top |
| query latency | median, p99 and max of compile + lookup + rank at each size, plus a hot operation with 1,000 providers; the oracle's full-scan time alongside |
| bytes into cognition | `cq_result_bytes` per query, against the catalog's description bytes and against handing over just the named operation's descriptions |
| catalog size scaling | 100, 1,000, 10,000, 100,000 capabilities; operations grow with the catalog (about 6 providers each), so the index is what is tested |

Pass requires, at every size: zero ranked-list, feasible-count and front
mismatches with the oracle; recall, precision and top-1 all 1.0; no
description byte read; bytes into cognition within the bound; at least 500
queries with candidates. Across sizes (catalog x1000): query median at most
x10 while the full scan grows more than x100.

Scenarios: alias and specialization, each hard constraint, Fabric leases and
locality, MCP sessions and applicability, tolerance bands, two orders -> two
winners, determinism over repeated queries and registration orders, held /
not held / forged / revoked authority through the native AIENOS authority,
and a chosen candidate driving an action graph (runs when held; reported
missing when not).

## Limits

- The MCP registry and the Fabric are in-process catalogs: no live MCP
  sessions and no network. Leases and session liveness are catalog state.
- Operations are matched by id, alias and specialization. The lexical,
  semantic and model-based stages of the router pipeline (docs/04) are not
  part of this gate.
- The query is a host function call, not yet a resident reaction.
- Confidence and reliability are catalog values, not learned.
- At most 32 operations in a plan (an operation and its specializations).
- Compile computes the plan digest every time; that is most of a query's
  cost (about 1 µs). A caller that keeps a plan pays only the lookup.
- Host processor only.
