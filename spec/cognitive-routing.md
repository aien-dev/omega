# Omega cognitive compute routing (OMEGA_COGNITIVE_ROUTING)

Code: `src/runtime/rx_route.{c,h}`. Test: `tests/runtime/rx_cognitive_routing.c`
with stand-in engines in `tests/runtime/rx_cog_engines.{c,h}`,
`make test-cognitive-routing`. Receipt: `evidence/COGNITIVE_ROUTING/`.

## Purpose

Choose the cheapest cognitive realization that is expected to meet the
semantic requirement. No engine is assumed to be a language model, and
cognition is not assumed to be one model. An engine is a function with a
declared class, the operations it supports, hard capacities, and a measured
profile.

## Objects

```
CognitiveRequirement (RxCogRequirement)
    operation_class, minimum_quality, uncertainty_budget, memory_requirement,
    temporal_requirement, planning_depth, precision_requirement,
    latency_budget, energy_budget, hardware_constraints, escalation_allowed

CognitiveRealization (RxCogRealization)
    cognitive_engine_id, generation, supported_operations (in decl),
    quality_profile, uncertainty_profile, resource_cost_model
    (latency_model = mean/p99 ns, energy_model = nJ/call), hardware_requirements
    (in decl), evidence (SHA-256 of the evidence sample)
```

`escalation_allowed` is a count: the escalations permitted after the first
rung (0 = none). Quality is in parts per million.

The declaration (`RxCogDeclaration`: class, supported operations, hardware,
memory capacity, temporal horizon, maximum planning depth, precision,
whether answers carry a sound check) is fixed at registration. The SHA-256 of
all declarations is the registry digest; a model is bound to it.

Realization classes: deterministic compiled procedure, small learned policy,
specialized neural module, general cognition, ensemble, J-Space exploration.

## Routing

1. **Hard filter.** An engine is eligible if it supports the operation, needs
   no hardware the request forbids, and its memory capacity, temporal horizon,
   planning depth and precision cover the request.
2. **Evidence filter.** It must have at least 200 labelled trials for the
   operation in the active generation, a measured cost, and a place in the
   operation's evidence sample.
3. **Acceptance.** For each engine the router derives the lowest confidence
   bin from which an answer may stop the ladder: the pooled answers at or
   above that bin must have a 95% Wilson upper error bound within the
   `uncertainty_budget`. A verifiable engine's checked answer is accepted on
   its check (a sound check has no error to bound). One checked answer
   observed wrong, or none observed, withdraws that.
4. **Ladders.** Every cost-ordered subset of the eligible engines, of length
   at most `escalation_allowed + 1` (and 4), is evaluated on the evidence
   sample: for each recorded trial, walk the rungs; stop at the first rung
   whose recorded bin is accepted, or at the last rung; count whether that
   answer was right and add the mean cost of every rung reached. The sample
   is the joint record of all engines on the same requests, so dependence
   between engines (hard inputs are hard for all of them) is in the estimate.
5. **Choice.** Keep ladders whose 95% Wilson lower quality bound meets
   `minimum_quality`, whose expected time fits `latency_budget`, and whose
   expected energy fits `energy_budget` (an energy budget without an energy
   model is not met). Choose the one with the least expected energy (time when
   any rung has no metered energy). No ladder: `RX_COG_ERR_UNSATISFIABLE`.
   The request is refused, never silently approximated.
6. **No evidence.** When the active generation has no evidence for the
   operation, the operation's reference realization (declared at
   registration) runs alone and the ladder says `no_evidence_reference`.
   Evidence that disqualifies every engine is a refusal, not a bootstrap.

The ladder for a requirement is cached per generation.

## Escalation

```
rung 0 (cheapest) -> accepted? yes: done
                     no, or the engine failed: next rung
...
last rung        -> accepted: RX_COG_OK
                    not accepted: RX_COG_ERR_EXHAUSTED, the answer is still returned
```

The ladder is bounded before it runs: its length and its expected cost are
admission conditions. Nothing measured while it runs changes which rungs run,
only the answers do; a run under scheduling noise takes the same path as a
quiet one. The trace records every rung, its confidence and time, the final
engine and the escalation count.

## Learning

Observations go into a ledger (`RxCogLedger`): labelled joint trials (quality,
failures, confidence bins, per-call time for p99) and batch costs (processor
time per call from a batch, metered energy per call). `rx_route_propose`
serializes the ledger into a generation's `model` blob and proposes it. The
routing does not change until that candidate is promoted by a principal
holding `RX_GEN_RIGHT_PROMOTE` on `RX_GEN_RES_PROMOTION` on the native AIENOS
authority, and the proposer may not promote its own candidate
(`rx_generation`).

The router reads profiles only through `rx_route_load`, which reads the active
generation's `model` blob through its SHA-256 root. A torn blob, or a model
built against another registry, is refused and the loaded profiles are kept.
There is no call that installs profiles from memory. `rx_route.o` is built
alone and the build fails if it references `rx_gen_promote` or any
`aienos_cap_*` / `rx_caproot_mint` / `rx_caproot_revoke` symbol.

## Gate

OMEGA_COGNITIVE_ROUTING_PASS requires, on one held-out workload (20,000
recognitions, 2,000 plans), run routed and on the always-most-expensive
eligible realization (the one with the highest measured cost, alone):

- both meet the same `minimum_quality` for every operation (recognition 0.95,
  planning 0.998), measured against ground truth;
- routed median processor time is lower, and lower in every one of 9
  back-to-back pairs (order alternating);
- where an energy meter exists (DGX Spark `aien_spbm`, CPU P+E clusters),
  routed median metered energy is lower, and lower in every pair;
- escalation never exceeds its bound; no routed request is refused;
- every promotion attempt without the right, or by the proposer, is refused;
- all checks pass (hard constraints, torn and foreign models, engine failure
  escalation, learning after damage).

The meter covers whole CPU clusters, so other work on the machine lands in
both legs of a pair; the receipt records the load average and every pair.

## The test's engines

Stand-ins, not AIEN. Recognition: label a 16-dimensional observation by a
fixed nonlinear rule the engines learn from 20,000 labelled examples.
Engines: small policy (logistic regression on x and x²), neural module
(16-12-1), general (16-96-96-1), ensemble (general + two 16-64-64-1 + neural).
Planning: a path within a length bound on a 96×96 grid with 28% obstacles, or
proof there is none. Engines: greedy best-first (deterministic, checked,
depth 256), weighted A* with an expansion budget (deterministic, checked,
depth 1024), exhaustive breadth-first exploration to the bound (J-Space,
exact).

Learning scenario: the neural module's weights are perturbed. Under the still
active generation routed recognition quality falls below the requirement. A
fresh labelled ledger is proposed; the routing is unchanged until it is
promoted; after promotion the router stops using the damaged module and
quality meets the requirement again, still cheaper than the most expensive.

## Not claimed

- AIEN cognition (the engines are stand-ins);
- graphics-processor realizations;
- energy below idle (the meter is whole-cluster and includes idle draw);
- routing of concurrent requests (one router per thread);
- learning without labelled trials (the ledger needs ground truth);
- a deadline that preempts a running rung.
