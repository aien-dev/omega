# R14 living recovery

Status: see the receipt under `evidence/R14/`. The receipt names its exact
candidate commit. Only a clean, candidate-bound GB10 run with the resident seat
observed and every scenario passing may say `R14_LIVING_RECOVERY = PASS`.
R15 and R16 are not claimed.

This is the component acceptance specification for ADR 0016 R14. It attacks
the R13 organism. It does not add a recovery system: every containment and
recovery below is done by a mechanism R3 to R13 already had, plus the seams in
the table at the end.

## Integration map

Each scenario starts a fresh R13 body (`rx_living`, same objects, same
authority, same R9 store), lets it produce, injects one fault, and observes.
The harness injects faults and publishes outside requests. It never writes a
faculty's object, selects a fallback, wakes a reaction, edits a generation
file or restores an authority table.

| Fault | Mechanism reused | Seam that was missing | Invariant | Evidence |
|---|---|---|---|---|
| A corrupt candidate | R10 `omega.verify.k` (identity re-hash, V0 structure, sandboxed differential in a forked child); R10 test defects `TAMPER` (bytes changed after the identity was computed), `SKIP_REMAINDER` (semantically wrong), `CRASH` | none | a refused realization is never verified, selected, proposed, promoted or run by production; the valid one still promotes | verdict object (state REFUSED, reason), verdict crumb, R9 root and provenance of the valid generation |
| B forged authority | R7 native validation at run and at publication; R8 slots, `root.install` origin checks, revoke; R9 promotion right check | R8 renewal revoked the reference the seat claim still presented (see seams) | no forged, borrowed, stale, stuffed or promoted-by-the-wrong-subject authority produces an effect or a mint; honest authority works after revocation | BLOCKED_AUTHORITY / REJECTED crumbs naming the reference; slot refusal reason; R9 `RX_GEN_ERR_AUTHORITY`; authority sweep |
| C reaction cycle | R3 reactions, R6 activation budget per causal episode, oscillation and no-progress limits | the budget never reset for reactions woken one step away from an outside event; containment left no crumb | a cycle is quarantined inside its episode; production, AIEN and Omega are not | QUARANTINE crumbs naming the reaction, the limit and the episode root |
| D GPU saturation | R5 admission (memory held per claim), resident claim ring, R12 seat, R3 publication | none | work waits, it is not dropped; every submission publishes exactly once; charges return to zero | per-lane commit crumbs from the Blackwell worker; peaks of held claims and blocked reactions |
| E seat killed mid-claim | R12 `rx_gpu_seat_kill`, `rx_resident_seat_lost(w, 1)`, `rx_gpu_seat_relaunch`; the seat hold word | none | the claim ends once with a crumb, its charge is released once, the dead seat's result never publishes, the retry publishes once | FAILED crumb (RX_ERR_SEAT_LOST) naming the stimulus, retry caused by it, stale result refused |
| F crash in promotion | R9 crash points (the process exits inside `rx_gen_promote`), `rx_gen_recover`, `rx_gen_open` | nothing rebuilt the in-force record from the durable generation after a restart | old or new generation, never torn; a committed flip is not rolled back; production restarts on the recovered generation's re-verified bytes | R9 root, blobs, journal, receipt, events; the restart's own restore crumb |

## Seams

| Where | What | Why |
|---|---|---|
| `rx_world.{c,h}` | Every crumb carries the root of its causal episode (the outside publication its wake chain started from; derived, checked by `rx_world_verify_crumbs`). The R6 activation budget counts per episode. | Defect found by R14: with a budget set, `omega.watch` and `aien.observe` were quarantined after 256 activations in the R13 body because only a direct outside wake ever reset the count. |
| `rx_world.{c,h}` | `RX_CRUMB_QUARANTINE` when a limit engages: reaction, limit, cause. Suppressed wakes still leave nothing. | R14 asks containment to be causally recorded, and the record must stay bounded. |
| `rx_world.c` | A Blackwell claim presents the reference its reaction's R8 slot holds now. | Defect found by R14: after an R8 renewal the root revokes the old grant, but the claim still carried the reference bound at start, so honest GPU work was refused forever. |
| `rx_resident_gpu.c` | The seat's object-id bound is the table it reads (`RX_MAX_OBJECTS`, 256), not the frozen ABI's 64. | Defect found by R14 scenario D on the GB10: every claim on an object id of 64 or more came back as a fault, while the host stand-in accepted it. R12/R13 worlds were small enough never to reach id 64. Regression: "objects past id 64" in `test-r12-silicon`. |
| `rx_generation.c` | A draft that is promoted, or can never be (stale), frees its slot. | Defect found by R14: the four draft slots were never freed, so a body could promote at most four times per process. Regression in `test-r9`. |
| `rx_generation.{c,h}` | `rx_gen_read_blob`: one blob of the active generation, checked against its root. | The restart must read the durable realization through R9, not by path. |
| `rx_omega.{c,h}` | `rx_omega_readmit`: the verifier R10 uses, run on durable bytes. | Production executes only verified bytes; a restart has none until they are verified again. |
| `rx_living.{c,h}` | `generation.restore`, run by the promotion subject when the body boots: reads the active generation, re-verifies, writes the in-force record or leaves production on the reference. | The in-force record lives in the world; after a process restart it has to be rebuilt from R9 by the authority that writes it. |

## How to run

`make test-r14-host` (processor stand-in; `HOST_PASS_NON_SILICON` at best) and
`make test-r14-silicon` (GB10 seat for D and E; the F processes and every
body also run the seat). Both need a machine with Cortex-X925 and Cortex-A725
cores, like R13. One letter or several as the first argument runs only those
scenarios (`build/rx_r14_recovery_silicon DE`).

## Limits found and left in place

- R8: a principal wrongly given WRITE on a decision object can jam decisions
  until it is revoked. It cannot cause a mint (scenario B shows both).
- R9: `rx_gen_propose` has no authority check. An in-process proposer can hold
  up to four drafts that can never be promoted until the generation moves.
- R10: Omega's store keeps the first bytes filed under an identity. Bytes
  that falsely claim an identity block the honest bytes for that identity in
  that process. Found by reading; not exercised.
- The authority table is the host-library C port and does not survive a
  process restart; the restarted body mints its grants again.

## Not claimed

R15, R16; neural or general cognition beyond R11; open-ended invention beyond
R10; AIENOS kernel isolation (the authority is the host-library C port, its
table does not survive a process restart); immunity to every hardware fault,
Byzantine hardware or a compromised kernel; a chip-raised fault (the seat is
killed by destroying its channel); performance.
