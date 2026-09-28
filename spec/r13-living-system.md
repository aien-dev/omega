# R13 living system

Status: see the receipt under `evidence/R13/`. The receipt names its exact
candidate commit. Only a clean, candidate-bound GB10 run with the resident seat
observed and every control passing may say `R13_LIVING_SYSTEM = PASS`.
R14, R15 and R16 are not claimed.

This is the component acceptance specification for ADR 0016 R13. It composes
R3 to R12. It does not change what any of them means.

## What is proved

We gave the running body a goal. After that, the test only serves production
requests (on their own thread) and reads the world. Nobody called AIEN, Omega,
AEGIS, the seat or R9.

```text
human goal (outside publication: matvec 64x256 below 55% of the confirmed cost)
  -> aien.assess -> aien.plan            (reason GOAL, names goal seq 1)
  -> omega.reconsider                    (search epoch 2 takes up the plan)
  -> omega.synthesize.k -> verify.k -> measure.k        (R10, unchanged)
  -> living.experiment.prepare           (trial 0 from plan seq and epoch)
  -> living.blackwell.add  x16           (R5 admits the Blackwell need, R7
                                          validates the grant read from an R8
                                          slot, the resident GB10 seat claims,
                                          executes IADD3 and publishes)
  -> living.experiment.evidence x16      (each sum checked, next trial posed)
  -> aien.experiment                     (belief: SUPPORTED, 16 replicates)
  -> omega.select                        (waits for that belief for its epoch)
  -> generation.prepare                  (R9 draft proposed, subject 63)
  -> generation.promote                  (subject 64 holds PROMOTE; R9 barrier)
  -> in-force record                     (only now does production switch)
  -> aien.observe/predict/assess         (confirmed prediction on the new
                                          record; goal status MET)
```

The order comes only from object dependencies, capability checks, R5
admission, the seat's claim ring, and R9's generation state.

## Seams added

| Where | What | Why |
|---|---|---|
| `rx_living.{c,h}` | five reactions and six objects in the caller's world | the experiment, its evidence, the R9 draft, the promotion, the in-force record |
| `rx_aien.c` | `aien.experiment` (opt-in); goal branch of `aien.plan` first; `aien.assess` keeps a confirmed cost from another core class as the expected value while status stays UNKNOWN | AIEN must read physical evidence; a goal must be able to cause the plan when the body has moved to an unexplored core class |
| `rx_omega.c` | `rx_omega_require_evidence` (opt-in): a later selection waits for AIEN's belief for its epoch. `rx_omega_serve_from` (opt-in): production reads the in-force record. `rx_omega_identity_of` | Omega reacts to the evidence; a selection is only eligible until promoted; the durable bytes must rebuild the identity |
| `rx_world.c` | a seat descriptor may carry one more trigger when it is its own R8 capability slot | defect, below |

R10 and R11 never call the opt-in functions. The two `rx_aien.c` goal changes
matter only when a human goal is present: the goal is considered before an
open hypothesis, and a cost confirmed on another core class, above the target,
with the current class unexplored, plans once (reason GOAL) instead of waiting
for the old prediction to fail. `make test-r10` (198 checks) and
`make test-r11` (355 checks, living run on both core classes) pass unchanged.

## The GPU bridge

R10's matvec and R12's add share no operation. The bridge is the smallest one
that keeps every rule: an Rx reaction (`living.blackwell.add`) on canonical
objects, asking R5 for the Blackwell feature, holding its output WRITE through
an R8 slot, executed by the resident R12 seat. Its operands come from the plan
sequence and search epoch; 16 trials wrap past 2^32. The add is a physical
witness of the experiment and its authority path. It does not measure matvec
cost and is not the promoted realization. The receipt says so.

## Authority

- The experiment grant is acquired once, before the goal, through the real R8
  path: a client reaction publishes a request, `aegis.decide` applies policy,
  `root.install` mints through the AIENOS admin and fills slot 0.
- During the episode the seat runs 16 claims on that slot:
  `aegis.decide` and `root.install` activations during the episode are 0.
- The authority table is swept at the end of every mode. Only subject 64 holds
  `RX_GEN_RIGHT_PROMOTE` (on `RX_GEN_RES_PROMOTION`, nothing else); AIEN,
  Omega, serve, the experiment subjects, AEGIS and the outside hold no mint,
  revoke, reclaim, epoch, clock or promote right. Entry 0 is the AIENOS office
  itself.
- R9 refuses a promoter that is the proposer, and validates the promotion
  right against the native authority.

## Generation

`generation.prepare` checks who wrote each input (selection by `omega.select`,
belief by `aien.experiment`, evidence by the evidence reaction, GPU output by
the seat, search by `omega.reconsider`, plan by `aien.plan`, goal from
outside), re-derives the realization identity from the stored bytes, walks the
crumbs selection <- measure <- verdict <- synthesis and GPU output <- ... <-
trial 0, and proposes a draft binding:

- 11 canonical objects `{id, generation, digest}` (snapshot content digest, or
  the publishing crumb's digest for objects outside its snapshot);
- evidence (epoch, sums, trials, selected and reference cost);
- model (AIEN's experiment belief fields);
- realization bytes; config (regime, margin, kind, length, identity);
- provenance: for each of the 11 nodes, its crumb id, publishing reaction and
  crumb digest;
- the authority epoch and generation of the experiment grant.

The R9 proposal is an effect outside the world; it is keyed by epoch so a
re-run after an invalidated publication reuses it. Promotion is keyed by the
active generation for the same reason.

## Liveness

Production runs on its own thread from before the goal. The receipt counts
production commits between crumbs: goal to AIEN's evidence belief, plan to
selection, trial 0 to evidence, goal to promotion, candidate to promotion
(the R9 barrier, about 100 ms of disk sync, runs inside `generation.promote`
while production continues). It also reports production that committed while
an AIEN activation itself was executing; AIEN activations take microseconds,
so that number is usually 0 and is not gated. The AIEN, Omega and GPU windows
largely coincide: Omega's own search finishes before the experiment does, and
its selection then waits for the belief.

## Causal proof

- `rx_world_verify_crumbs` succeeds; no illegal lifecycle transition.
- Promotion's crumb reaches goal, plan, search, trial 0, GPU output, evidence,
  belief, selection and candidate.
- The durable provenance is read back from the promoted generation's files:
  each crumb exists, has the recorded digest and publishing reaction, and is
  an ancestor of the promotion; adjacent nodes are ancestors of each other.
- The realization bytes on disk rebuild the identity in the durable config and
  in the in-force record.
- After the goal, every outside publication is a production request.
- No production result used a realization before it was in force.

## Controls

| Control | Setup | Must hold |
|---|---|---|
| A no AIEN | R11 and `aien.experiment` not registered | no plan, no GPU claim, no candidate, generation unchanged |
| B no promotion authority | promoter holds no PROMOTE capability | candidate proposed; R9 returns `RX_GEN_ERR_AUTHORITY`; nothing in force; production stays on the reference; recovery sees the old generation |
| C revoked experiment | grant revoked before the goal | seat blocked on authority, no claim, no evidence, production continues |
| D stale generation | GPU output retired while the seat holds the claim | result refused, no evidence, no candidate |
| E failed verification | a crashing candidate added to the search | verifier refuses it; the promoted generation holds a verified one |

## Defect found in an earlier gate

Before R13 the world refused any resident-seat reaction with more than one
trigger. R8's `rx_aegis_use_slot` adds a slot trigger to every slotted
reaction, so a seat could not use the R8 fast path at all. The fix accepts
exactly one extra trigger, only when it is that reaction's own declared
capability slot. Regression checks are in `rx_r12_resident.c` (valid slot wake
accepted, unrelated second trigger refused). R8, R12 host and R12 GB10 were
rerun.

## Qualification

`make test-r13-host` runs on the Spark with R12's processor stand-in in place of
the chip and reports `HOST_PASS_NON_SILICON` at best. `make test-r13-silicon`
uses the resident Blackwell seat. Hosted CI only builds R13: its runners have
one core class and no GB10.

## Not claimed

R14, R15, R16; cognition beyond R11's statistical model; realization invention
beyond R10's emitters; kernel isolation of the authority (it is the host-library
C port of AIENOS); that the GPU add measures matvec cost.
