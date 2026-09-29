# TURING Wave 0: proposal (A-K), revision 2

Inputs: TURING_CURRENT_STATE.md (live audit, omega 4b217aa, aienos 603c91d), TURING_PRIOR_ART_MATRIX.md (30 sources opened),
polyglot review def197d (fixes in progress on feat/polyglot-0), Fable review of revision 1 (9 REQUIRED items, adopted in
full; they override revision 1 wherever the two differ). Standing rules applied: C (+asm), no Rust, no Python, no outside
deps; OSC-0B identity freeze (omega#76) open; src/runtime/ frozen until R16 closes (R16 at W5, omega#68 draft).

## Revision log (revision 1 -> 2)

| Fable item | Where it landed |
|---|---|
| 1. Name the one contract digest | K.1: no existing digest names the Omega-X contract; V0 defines a PROVISIONAL contract_digest (not an Omega semantic id) and shows all 10 algebra realizations carry it |
| 2. spec_id stable across rebuild | F: build_digest, git_commit and toolchain moved from the spec to the evidence record |
| 3. Canonical encoding + supersedes | K.5: every record uses the OMG0 layout and the repo SHA-256 with a domain prefix; decisions carry `supersedes` |
| 4. No dependency on feat/polyglot-0 | F, I, J: Wave 1 adapts oma_rz_impl only (10 candidates); the omx_candidate adapter follows that branch's merge (28 later) |
| 5. Post hoc, outside the runtime | D, F: Wave 1 records are built from existing receipts by new code under src/turing/; no selector in src/runtime writes records yet |
| 6. Pre-register the kill test | K.4 (thresholds, justification, verdict rule); K.2 (Field selection rule in one paragraph) |
| 7. Exact control arm | K.3 |
| 8. Split H | H1 (selectors, observation off, runnable now) and H2 (observation levels, blocked on the runtime freeze); budgets absolute vs ARGUS-off |
| 9. Tiers are PROPOSED | F: tier stored as text plus the ADR revision it came from |
| Optional: retire the plane name | The earlier name for the observation plane is gone from code and from this doc; it is "observation levels on ARGUS" |
| Optional: cpu frequency + quiet flag | F: both are evidence fields; receipts that did not record them say "not recorded" |
| Optional: seeded shuffle | H1: interleaving order comes from a seeded shuffle whose seed is written into the run receipt |

## A. Live implementation audit (summary; detail in TURING_CURRENT_STATE.md)
- Realizations: IMPLEMENTED twice, unlinked: oma_rz_impl (src/algebra/realize_common.h, 10 algebra realizations, merged #78)
  and omx_candidate (src/polyglot/omx_lang.h, 18 language candidates, feat/polyglot-0, unmerged).
- Selection: three selectors: rx_costmodel (8 fixed arms, matvec only, in the frozen runtime), oma_select (stand-in),
  polyglot_explain (unmerged branch).
- Evidence: receipts in evidence/MIXED_ALGEBRA (identified by the SHA-256 of their bytes, which ma2_select_receipt.json
  already records per source), evidence/POLYGLOT (branch); ADR 0019 (PROPOSED) defines tiers E0-E4.
- J-Space (rx_jspace.h) already separates semantic identity from physical realization with measured cost.
- Observation: ARGUS (src/runtime/rx_argus.{h,c}, aienos native/argus/argus_abi.h) = per-thread lock-free rings, counters
  for routine, full events on change, critical never dropped, others counted as TELEMETRY_DROPPED. 2.25 ns/use,
  +2.1% emit-only, +3.9% shipped default on R8 (target 2%). Visor does not read ARGUS. Security events only.
- Fabric-like: rx_semcomm (need -> projection -> deltas), OMG0 canonical encoding. No dialects, no round-trip tests.
- Identity: program id v2 binds canonical body+contract, cannot name Omega-X (scalar only).

## B. Duplication / conflict report
| Proposed | Would duplicate | Decision |
|---|---|---|
| Observation plane | ARGUS + causal crumbs | New ARGUS event class, not a new plane. Name in code and docs: "observation levels on ARGUS" |
| Field selector | rx_costmodel, oma_select, polyglot_explain | One decision-record format all three will write (after the runtime freeze lifts for rx_costmodel). Wave 1 adds a Field selector only as the kill-test arm; if it fails K.4 it is removed, not kept as a fourth selector |
| Turing Fabric | rx_semcomm, rx_capq "Fabric", aienos Personal Fabric | Deferred to Wave 5; built as rx_semcomm dialects; code name "dialect", never "fabric" |
| Content-addressed store | receipts, Cortex, plan cache, J-Space chains, Store v1 | None. Field records are digest-named, beside existing receipts |
| Explanation | Visor why, polyglot_explain | Extend Visor to read decision records (Wave 8) |
| BuildIdentity | TRUST-1 BuildIdentityV1, OSC-0B II.9 | Reference whatever lands; V0 stores git commit + build digest as plain evidence fields |
Conflicts with brief: Rust and Python experiments (rejected by standing rule); models as Fabric participants imply outside
runtimes (deferred); new semantic ids (blocked by OSC-0B; V0 uses a PROVISIONAL contract digest, K.1).

## C. Prior-art kill test
Every individual mechanism exists: content addressing (Git, Unison), provenance (SLSA, Atomic), multi-implementation +
autotuning (PetaBricks, SPIRAL, StarPU, Ansor/MetaSchedule), accuracy-aware selection (ApproxHPVM), heterogeneous dispatch
(HPVM, IREE, IRIS-ORNL, TVM), analog compilers (Arco, Legno, DIANA/HTVM), ternary kernels (BitNet, T-MAC), tracing (Dapper,
LTTng, eBPF), sub-text model communication (CIPHER, Cache-to-Cache, LatentMAS).
Survives only as candidate novelty: (1) every selection is an immutable decision record citing replayable receipts;
(2) number system as a contract-verified axis next to language and hardware; (3) compact dialects gated by round-trip.
Kill criterion: pre-registered in K.4.

## D. Responsibility boundaries
Omega = meaning + contract. Field = records linking contract -> realization specs -> evidence -> decisions (data, not a
subsystem). Selection = existing selectors, all writing Field decision records once they can (rx_costmodel only after the
runtime freeze lifts). **Wave 1 records are produced outside the runtime, post hoc, from existing receipts**, by new code
under src/turing/; nothing in src/runtime, src/algebra or src/polyglot changes. ARGUS = observation. Visor = explanation
from records. J-Space = exploring candidates. Cortex = learned summaries only, never the store. Compute Fabric = placement
(unchanged). Dialects (later) = rx_semcomm. AEGIS/Effect Broker untouched.

## E. First semantic operation
Omega-X exact ternary GEMV (spec/mixed-algebra-ma2.md; spec/polyglot-0.md on the branch). Exact contract. Wave 1: the 10
algebra candidates. Later: +18 language candidates. Known workload-dependent winner (int8 when W fits L2, 2-bit ternary
DRAM-bound), which `turing-field` must reproduce from stored evidence alone.

## F. Field V0 data model (C, new dir src/turing/, no runtime edits)
- `turing_contract`: name, statement, exactness, overflow bound, oracle, source, max_n. contract_digest (K.1).
- `turing_rz_spec`: contract_digest, rz_id, algorithm, algebra, representation, precision, language, backend,
  machine_class, max_n, exact, weak_baseline. **No build fields.** spec_id = digest of the canonical bytes; identical
  whether built from the oma_rz_impl registry or reconstructed from a receipt's realizations list.
- `turing_evidence`: spec_id, workload (n, m, sparsity), receipt_digest (SHA-256 of the receipt file), run_id, tier as
  text (e.g. "E2") plus tier_source ("ADR 0019 PROPOSED, aien-architecture PR #58 head ed4474a"), verified, forced blocks,
  retries, samples, median/q25/q75/min ps, pack ps, noise, weight and working-set bytes, **build_digest, git_commit,
  tree_dirty, toolchain, cpu_freq_state, quiet_flag, thermal**. The MA-3 receipts did not capture cpu frequency or the
  quiet flag, so V0 writes "not recorded by this receipt"; H1 receipts must record both. The receipt path is where to find
  the bytes and is not part of the digest.
- `turing_decision`: contract_digest, selector id, constraint set, query, cell used (exact or nearest), every candidate
  spec_id with a reason code and its cost, chosen, verdict, tie resolution, margin, noise band, cited evidence digests,
  `supersedes` (digest of the decision it replaces, or none). decision_id = digest. Never mutated.
- Adapters: oma_rz_impl registry (read-only) and MA-3 bench receipts (read-only, post hoc). omx_candidate adapter is a
  follow-up after feat/polyglot-0 merges.
- Tool `turing-field` prints the history view (git log analogue: commit = decision, parent = supersedes, tree = cited
  evidence, blob = receipt), the winners per shape, the selector comparison and the kill-test dry run.

## G. Observation levels on ARGUS (needs a src/runtime edit: NOT in Wave 1)
Levels map onto ARGUS: HOT = existing counters; OBSERVED = new ARGUS class REALIZATION (one event per selection or
promotion, never per kernel call: carries decision_id + spec_id + 3 counters); FORENSIC = existing full events + decision
record dump. Required (never drop): promotions, effects, commits (already ARGUS critical). Reader: ARGUS ring -> Field
evidence file (async, off hot path). Budgets are absolute against ARGUS off, not stacked on today's ARGUS cost (which is
already +3.9% against a 2% target): HOT <= 0.5% median vs ARGUS off on Omega-X; OBSERVED <= 2% vs ARGUS off. If ARGUS
itself already exceeds a budget, the level fails; that is reported, not waived.

## H. Experiments
- **H1 (runnable now, observation off):** Omega-X x {10 candidates now, 28 later} x shapes S1-S4 (K.4) x selector
  {Field, control arm K.3}. Pinned cores, interleaved in an order drawn from a seeded shuffle whose seed is written into
  the run receipt, N >= 20, .spark-quiet up, cpu frequency state and quiet-flag status recorded per run, Grok checks the
  statistics. Scored by K.4. Prerequisite for the 28-candidate run only: polyglot harness fixes (review def197d) merged.
- **H2 (blocked on the runtime freeze):** observation levels {off, HOT, OBSERVED, FORENSIC} against the G budgets, plus a
  flood test proving required events are never dropped.

## I. Agent wave 1 (non-overlapping); manifest in TURING_AGENT_WAVE_1.md
- FIELD: owns src/turing/field.{h,c}, src/turing/field_select.c, src/turing/replay.c, src/turing/select.h,
  tools/turing_field.c, tests/turing/; read-only src/algebra and evidence/MIXED_ALGEBRA.
- CONTROL ARM: owns src/turing/history_selector.c; read-only everything else.
- REVIEW (hostile) after both. Observation lane only after Drake's ruling on the runtime freeze. Fabric: not in this wave.
- Shared file: the TURING block at the end of the Makefile (integration owner). Nothing touches feat/polyglot-0.

## J. Gates
- TURING_W0_ARCHITECTURE_VALID: this doc + Fable review + Drake go.
- ONE_MEANING_MULTIPLE_REALIZATIONS (Wave 1 scope: 10 candidates; 28 after polyglot merges): every candidate
  reconstructed as a spec record from existing receipts; one contract_digest across all; spec_id stable across rebuild;
  every decision cites receipt digests that verify; `turing-field` shows the S1 vs S4 winner flip from stored evidence.
- OBSERVATION_OVERHEAD_ACCEPTABLE: G budgets, flood test (H2).
- Kill gate: K.4, scored on H1, reported either way.

## K. Wave 1 specification

### K.1 Contract digest (Fable item 1)
Searched on omega main 4b217aa: src/algebra (oma_rz_impl registry, realize_common.h), spec/mixed-algebra-ma2.md,
spec/mixed-algebra-reference.md, evidence/MIXED_ALGEBRA/*.json. **No digest names the Omega-X contract.** The contract
exists only as C comments plus `oma_rz_oracle`; receipts carry an `operation` text string whose wording differs between
the bench receipts and the select receipt, so it cannot serve as identity. Program id v2 cannot express a GEMV, and OSC-0B
(omega PR #76) freezes new Omega semantic ids.

Decision: V0 defines `contract_digest` = SHA-256("turing.contract.v0.provisional" 0x00 || OMG0 bytes of the contract
record: name, statement, exactness, overflow bound, oracle, source, max_n, and a status attribute saying PROVISIONAL).
Value: `75772afe8a98f73064f2933d76bc58f7bfe5b9353eb40b703e173b4cd3f22b5f`. It is **PROVISIONAL and not an Omega semantic
id**: the domain prefix makes it differ from any SHA-256 over OMG0 bytes, it is never written into an Omega object, and
it is replaced by whatever id OSC-0B assigns to Omega-X. All 10 oma_rz_impl entries are registered against the single
contract in realize_common.h (each `exact = 1`); test-turing checks that all 10 spec records carry this digest.

### K.2 Field selection rule (Fable item 6)
The Field selector considers every spec in the store and records a reason code for each: CONTRACT_MISMATCH if the spec
names another contract digest, NOT_EXACT, MAX_N if the query's n exceeds the spec's limit; it then keeps only evidence
rows whose receipt file still hashes to the recorded digest (re-hashed at decision time; otherwise RECEIPT_UNVERIFIED),
picks the measured cell nearest the query (|log2 n ratio| + |log2 m ratio| + 4 x sparsity difference, so sparsity counts),
drops candidates with an unverified run (UNVERIFIED_RUN), a forced contention block (CONTENTION_FORCED) or a tier outside
{E2, E3, E4} (TIER), ranks the rest by mean per-call cost over receipts, computes a noise band (the larger of within-run
and across-run spread of the cheapest and the runner-up), and when more than one candidate lies inside the band resolves
the TIE by ADR 0019 section 9.1 (incumbent, else the reference R1_plain, else the cheapest). The decision cites every
evidence digest it used. **How it differs from the control arm:** it filters on contract, exactness, receipt integrity,
verification and tier with reason codes; it is sparsity-aware through the nearest-cell metric; it refuses to separate
candidates inside the noise band and prefers the reference there; it cites evidence. **Plainly:** on the stored MA-3 data
the ranking step is the same "cheapest measured mean" that the control arm uses. Every filter passes every candidate on
the stored grid, so apart from the tie rule the Field selector's extra content is the audit trail. The only place the
rules can choose differently on this data is the tie rule, and K.6 shows it choosing worse there.

### K.3 Control arm (Fable item 7)
StarPU-style history-based model, in C (src/turing/history_selector.c). State: per (spec_id, footprint bucket) a count
and running mean of observed per-call cost. Footprint = the data sizes (n, m) plus the pack mode, as StarPU hashes a
codelet's buffer sizes; sparsity changes values, not sizes, so it is not in the footprint. Eligible = specs that can
execute the query (n <= max_n). Cold start: while any eligible spec has zero observations in the bucket, choose the next
one in a round-robin order that is a seeded Fisher-Yates shuffle of the spec indices (splitmix64, seed recorded). After
that: the minimum running mean (lowest index on an exact tie). No noise band, no tie rule, no receipt citations. Warm
mode (the K.4 comparison) loads every stored evidence row as an observation, so both arms see the same information.

### K.4 Kill test, pre-registered (Fable item 6)
Shapes (sparsity 0.30): S1 4096 x 64 (256 KiB int8 W, L2-fit), S2 16384 x 64 (1 MiB, L2 edge), S3 4096 x 4096 (16 MiB,
L3/DRAM), S4 16384 x 4096 (64 MiB, DRAM-bound); both pack modes (once amortized, per call): 8 decisions per run.
- Regret of a decision = cost(chosen) / cost(oracle) - 1, where cost = mean per-call cost of the exact cell over every
  receipt and oracle = the cheapest verified candidate. Raw regret; no discount for noise.
- **Q1 selection quality:** Field mean regret <= control mean regret + 0.02 AND Field max regret <= 0.10. Scored twice:
  in-sample (control warm on the same receipts) and leave-one-cell-out (the queried cell's evidence withheld from both).
- **Q2 replayability:** fraction of Field decisions whose cited receipts verify = 1.00 (any failure fails Q2).
- **Verdict:** FAIL if Q1 or Q2 fails: Turing selection is integration-only and stays a documentation concept.
  "BEAT on quality" if Q1 and Q2 hold and the Field mean regret is lower than the control's by more than 0.02 in either
  scoring. Otherwise "SURVIVES on explainability at equal quality", which the C criterion allows but which is not a quality
  win and is reported as such.
- **Why these numbers:** 0.02 is the 90th percentile of within-run noise (q75-q25)/median in the MA-3 run 1 cost table
  (0.019); a gap smaller than that is not a measurable difference. 0.10 is the smallest cross-family gap MA-2 reports as a
  decisive loss (binary vs packed forms at m <= 64: 1.1x); a selector that exceeds it on any shape has picked a
  realization the evidence already calls wrong.
- The thresholds are fixed in tools/turing_field.c before any comparison was run and may not change after results;
  any rule change must be registered here first and the dry run re-reported.

### K.5 Canonical bytes and ordering (Fable item 3)
Every record (contract, spec, evidence, decision) is laid out as an OMG0 object by the existing
`omega_canonical_encode` (src/omega_canonical.c: magic OMG0, version, kind, attributes sorted by key with u16 big-endian
lengths, relations sorted by kind then target, constraints, payload). Values are text; integers in decimal; fractions as
parts per million. Decision candidates are relations DEPENDS_ON(spec_id) and cited evidence is DERIVED_FROM(evidence
digest). Digest = SHA-256(domain || 0x00 || OMG0 bytes) using the repo's own src/sha256.c; domains
`turing.contract.v0.provisional`, `turing.spec.v0`, `turing.evidence.v0`, `turing.decision.v0`. The prefix separates
Field digests from Omega semantic ids (plain SHA-256 of OMG0 bytes). Ordering: a decision names the one decision it
replaces in `supersedes`; the current decision for a (contract, query) is the one nothing supersedes. Two decisions
superseding the same parent is a recorded fork, resolved only by a later decision that supersedes one of them.

### K.6 Wave 1 result on stored data (dry run of K.4; H1 is the binding run)
Built post hoc from evidence/MIXED_ALGEBRA/ma3_bench_run{1,2}.json (10 specs, 720 evidence rows, 36 cells).
Winners from stored evidence alone (pack once): S1 R1_smmla, S2 R1_sdot_il (TIE, cheapest), S3 R2c_crumb, S4 R2c_crumb.
The int8 to 2-bit flip between L2-fit and DRAM-bound shapes is reproduced.

| Scoring | Field mean / max regret | Control mean / max regret | Field receipts verify | Control |
|---|---|---|---|---|
| S1-S4 x 2 pack, in-sample | 3.58% / 25.51% | 0.01% / 0.07% | 8/8 | 0/8 (cites none) |
| S1-S4 x 2 pack, leave-one-cell-out | 3.65% / 25.51% | 0.01% / 0.07% | 8/8 | 0/8 |
| pack once only (S1-S4) | 0.00% / 0.00% | 0.00% / 0.00% | 4/4 | 0/4 |
| per call only (S1-S4) | 7.16% / 25.51% | 0.02% / 0.07% | 4/4 | 0/4 |
| all 36 cells x 2 pack, in-sample | 2.15% / 25.81% | 0.03% / 1.04% | 72/72 | 0/72 |
| all 36 cells x 2 pack, leave-one-cell-out | 2.18% / 25.81% | 0.03% / 1.04% | 72/72 | 0/72 |
| cold online, 720 queries (control learns) | 2.15% / 25.81% | 104.50% / 9398.91% | n/a | n/a |

Dry-run verdict: **Q1 FAIL, Q2 PASS -> FAIL.** All Field regret comes from the per-call mode, where pack-cost noise widens
the band (32 of 36 per-call cells are TIEs, bands 1.0% to 27.5%, matching MA-2's 32 per-call ties) and the ADR 0019 tie
rule picks the reference R1_plain inside it. With packing amortized the two
selectors are identical in quality (35 of 36 cells same pick, both at the oracle). The cold online row compares
different information (stored evidence vs learning from scratch) and is not part of the verdict. Before H1, either the
rule stays and the kill test is expected to fail, or a rule change is registered here first; thresholds do not move.
