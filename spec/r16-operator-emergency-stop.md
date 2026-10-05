# R16 G6: operator emergency stop (contract)

Status: runtime control IMPLEMENTED and host-tested. G6 item
`operator_emergency_controls_passing` stays **MISSING_IMPLEMENTATION**: the
production R13 program does not wire the control (no `rx_world_set_halt_dir`
call, no operator entry point), so no operator can reach it on the candidate,
and no silicon run exercises a stop. See "Not done" at the end.

Source: `src/runtime/rx_world.c` (`rx_world_emergency_stop`,
`rx_world_emergency_resume`, `rx_world_set_halt_dir`, `rx_world_halt_status`),
`src/runtime/rx_generation.c` (`rx_gen_promote` mark check),
`src/runtime/rx_world.h` (contract comment). Tests: `make test-rx-emergency`
(`tests/runtime/rx_emergency.c` plus `tests/runtime/rx_emergency_mutants.sh`) and
`make test-r12` (`t_emergency_stop` in `tests/runtime/rx_r12_resident.c`).

## 1. What it is

One deterministic operator control over a running reaction world: **stop**
(nothing new commits, nothing from outside publishes, no generation is proposed
or promoted) and **resume** (the world continues from exactly the state it held,
and the work the stop refused runs again). ADR 0016 §57: "Do not remove
deterministic operator control." This is that control for the R16 world. It is
not a kill switch for the process and not a rollback: the world's state is kept,
not changed.

## 2. Authority

A stop or resume is accepted only when all three hold, checked under the world
lock in this order:

1. **Caller credential.** `subject` is enrolled (`rx_world_enroll_caller`) and
   `cred` passes the constant-time caller check (absent, unknown, revoked, stale
   and forged credentials are refused: `RX_ERR_IDENTITY`). This holds whether or
   not the world has bound its callers.
2. **Not a reaction.** `subject` is not the subject of any reaction ever added to
   the world: no reaction can stop or resume the world it runs in
   (`RX_ERR_AUTHORITY`). ADR 0016 I9: intelligence cannot promote itself past
   authority policy; the same applies to pausing or un-pausing.
3. **Capability.** `cap` is a live capability for `subject` on
   `RX_WORLD_RES_CONTROL` (0x906) with right `RX_WORLD_RIGHT_HALT`, validated by
   the world's authority (rx_caproot or the native AIENOS authority). The right
   is `RX_RIGHT_EPOCH`: privileged, never delegable. The right that may void every
   capability may also pause the world. Wrong right, wrong resource, another
   subject's capability or a revoked capability: `RX_ERR_AUTHORITY`.

Authority is never a boolean anyone can set (ADR 0016 §57, "Do not encode
authority as editable booleans"). `w->halted` is the world's *state*, written
only by the two authorized calls and by restart restoration (§4). Every refused
attempt leaves the world digest, the crumb log and the stop state unchanged (E1).
A forged resume is refused before the world says whether it is stopped (E1).

## 3. What a stop does

Taken under `mu` then `callers_mu` (the C7 order), so work already committing
finishes first, and nothing lands between the check and the state change.

| Path | Under a stop | Evidence |
|---|---|---|
| Workers | take no work (`worker_main` waits); resume broadcasts | E3, mutant M2 |
| An activation computing when the stop lands | its result is refused at commit: CANCELLED, `RX_ERR_HALTED`, crumb; it is re-armed and runs again on resume, committing exactly once | E3, mutant M1 |
| Outside publication (`rx_world_publish_external`) | refused, `RX_ERR_HALTED`, world unchanged | E2, E4, mutant M3 |
| Resident seat result (`rx_resident_accept`) | not published: the output window is re-projected from the world, the activation ends CANCELLED and runs again on resume; the claim closes | R12 `t_emergency_stop` (manual mutant: removing the guard fails 5 checks) |
| Sequential activation (`rx_world_seq_activate_timed_locked`) | refused, `RX_ERR_HALTED` | code review only |
| Caller check (`rx_world_caller_check_fn`) | refuses with `RX_CALLER_ERR_HALTED` (CHECK and HOLD; RELEASE unaffected) | E5, mutant M4 |
| Generation store bound to the world | `rx_gen_propose` / `rx_gen_promote` refuse with `RX_GEN_ERR_HALTED`; a promotion is re-checked after its HOLD, right before the flip, so a stop during its live barrier or its disk writes refuses it; active generation unchanged in memory and on disk | E5 |

A second stop answers `RX_HALT_ALREADY` and changes nothing (E2). A resume of a
running world answers `RX_HALT_NOT_STOPPED`, with no crumb (E1).

The stop is in force in memory **before** any disk work (mutant M9). Under racing
publishers and two operators, the crumb log never shows an EXTERNAL or COMMIT
between a STOP and its RESUME, each accepted stop has exactly one STOP crumb,
and the chain verifies (E4).

## 4. Durability and restart

With `rx_world_set_halt_dir(w, dir)` set (normally the generation store's
directory):

* A stop writes `<dir>/OPERATOR_HALT`: a fixed text record (`aien-operator-halt
  v1`, seq, subject, capability, reason, time, the STOP crumb id) followed by
  `sha256 <hex>` over every byte before it. Written to a partial file, fsynced,
  renamed, directory fsynced. If the write fails, the stop still holds in memory
  and `durable` < 0 reports the errno (never hidden); `durable` = 0 means no
  directory was set (E2).
* A world that starts over a directory holding the mark starts **stopped**
  (`restored`, STOP crumb). A mark that does not verify (one byte changed) or
  cannot be read also starts it stopped (`RX_ERR_TORN` / `RX_ERR_IO`): the mark
  fails closed (E6, mutant M7).
* A generation store refuses promotion while the mark exists, even when it is
  not bound to any world (`RX_GEN_ERR_HALTED`; an unreadable directory is
  `RX_GEN_ERR_IO`) (E6, mutant M6).
* Resume first writes the sealed resumed record
  `OPERATOR_HALT.resumed.<seq>.<t_ns>`: the original bytes, then `resumed_by`,
  `resume_cap` and `resume_t_ns`, then a new seal. It is linked in without
  overwriting anything, and only then is the mark removed. If either step fails,
  resume returns `RX_ERR_IO` and the world **stays stopped** (E6, mutant M8).

The mark is the durable stop *state*. The canonical evidence is the crumb log
(OPERATOR_STOP and OPERATOR_RESUME crumbs, with the resume caused by its stop),
per ADR 0016 §57 "Do not use text logs as canonical evidence". These two kinds
have no Cortex kind and are not linked into Cortex (`rx_cortex_record.c`).

## 5. ADR 0016 invariants

| Invariant | How the stop keeps it |
|---|---|
| I2 stale generations cannot mutate current state | a refused activation re-runs on resume against the state *then*; the seat result path re-projects from the world |
| I4 revoked authority cannot execute | a revoked control capability or caller is refused (E1); the stop adds a refusal, never a permission |
| I6 concurrent publication cannot create torn objects | a stop is taken under the world lock; a commit either finishes before it or is refused whole (E3, E4) |
| I7 causal history for meaningful committed changes | STOP and RESUME are crumbs; refused activations leave CANCELLED crumbs with `RX_ERR_HALTED` |
| I8 generation recovery produces a coherent world | no promotion completes under a stop, including one already past its barrier (E5); restart restores the stop (E6) |
| I11 GPU failure cannot destroy durable identity | a seat result under a stop is dropped and re-projected, never half-published (R12 `t_emergency_stop`) |
| I15 irreversible effects remain attributable | every stop, resume and refusal names its subject, capability and reason |

## 6. Tests and what they prove

`make test-rx-emergency` (host CPU, real rx_caproot authority, worker pool,
generation store; built with the normal flags; E1 to E6 in the file header):

* E1 refusals: seven refused stops, each with its code, nothing changed.
* E2/E3: stop, second stop, refused publication, in-flight refusal, queued work
  not taken, resume, each activation commits exactly once.
* E4 race: four workers, two operators, publishers; crumb-log ordering.
* E5 promotion under a stop (live barrier and disk-write windows), proposal
  refused, control promotion after resume.
* E6 durable mark, restart, torn mark, unbound store, read-only directory resume.
* `rx_emergency_mutants.sh`: nine single-guard mutants (M1 to M9 above), each
  applied to a temporary copy; the test must fail against every one and pass
  against the unmutated copy. A mutant whose pattern no longer matches fails the
  script.

`make test-r12` `t_emergency_stop` (native AIENOS authority, resident seat
stand-in): a seat result that lands after the stop is refused, canonical B and
B's window keep the world's value, the dependent does not run, no claim is posted
under the stop; after resume the seat runs again and B = X + Y, C = X + Y + 1.

## 7. Not done (why the G6 item stays MISSING_IMPLEMENTATION)

1. **Production wiring.** The production R13 program
   (`tests/runtime/rx_r13_living.c`) does not set a halt directory and exposes no
   operator entry point. An operator console needs a design for how the
   operator's credential and control capability reach a running program; that
   design is not written. The G6 item names `rx_world_set_halt_dir` in the R13
   program as a required symbol, so it reads MISSING until that lands.
2. **Silicon.** No silicon run exercises a stop. The resident-seat case runs on
   the R12 host stand-in only.
3. **Mark authenticity.** The seal is an unkeyed sha256: it detects a damaged
   mark, not a forged one. A forged mark can only stop the world (fail closed).
   Removing the mark while no world runs bypasses the restart restoration;
   protecting the directory is the operating system's job and is not tested
   here.
4. **Outside executors.** A stop does not reach a durable executor already
   performing an effect outside the world. Its result is not published until
   resume, and the effect stays attributable through its crumbs.
5. **Sequential activation** under a stop is refused in code but has no test.
