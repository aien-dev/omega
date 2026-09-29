# Omega Visor V1 qualification (lane 8)

Verdict: **OMEGA_VISOR_V1_FAIL**  
Receipt: `evidence/VISOR/12785aab7bb22bc1fd1d4e71143c15f98f1e4bc94885e9b2a725487b023b7c93.json`  
Commit: `ec2ec0ba34e241e6bae87ba57eb2a952853cad0f` (tree dirty: True, only lane-8 files (untracked, not yet committed))  
Run: 20260929T043338Z on aarch64 7.0.0-1019-nvidia (NVIDIA_DGX_Spark)

Scope: host-only: no GPU/silicon claim; no QEMU claim by the Visor (the pre-existing test-m5 gate runs its own QEMU check)

## The five gates in plain words

| Gate | What it checks | Result | Tests | Failed |
|---|---|---|---|---|
| OMEGA_VISOR_SEMANTIC_PASS | Same meaning gets the same fingerprint (7, 07, 0x07, 0b111; spacing; comments); different widths differ; repeat runs match byte for byte | PASS | 541 | 0 |
| OMEGA_VISOR_VERIFY_PASS | The checker and the evidence viewer report what is really there, and a new user can find it from `help` | PASS | 151 | 0 |
| OMEGA_VISOR_REALIZE_PASS | Machine code built for an expression gives the same answer as the language; cost is labelled estimate, never measured; scripting behaves | FAIL | 419 | 4 |
| OMEGA_VISOR_AUTHORITY_ISOLATION_PASS | The console can ask but never grant; bad or hostile input is refused cleanly and never crashes it | FAIL | 177 | 1 |
| OMEGA_VISOR_REGRESSION_PASS | The older CPU milestone gates still pass and the tool still builds without the physics checkout | PASS | 92 | 0 |

Hostile inputs: 69 tried, 59 refused cleanly, 0 crashed, 4 harmless blank/comment lines.
Repeatability: 3 runs, identical: True.  Memory-checker build: {"status": "RUN", "sanitizer_reports": "none", "crashed": 0, "same_case_failures_as_release": true}.

## Why the verdict is what it is

Note: the AUTHORITY_ISOLATION gate has **zero authority-path failures** (81/81 C hostile suite, link check, 16 escalation words refused, AUTHORITY row NONE, `effects` refuses non-effects, World holds no authority). Its single failure is a hostile command-line case: `--script /` (a directory) exits 0 (defect D3). test-visor-console counts 284 = 134 (lane build) + 150 (full build with the e2e script): two separate test programs.

- OMEGA_VISOR_REALIZE_PASS FAIL (4/419 failed)
- OMEGA_VISOR_AUTHORITY_ISOLATION_PASS FAIL (1/177 failed)
- runner realize: run-arity[unary fn, 0 args refused] FAIL {"command":"run","status":"ok","class":"pure-execution","result":{"result":"1","realization":"sha256:dfc4cb844da0044a4fe
- runner realize: run-arity[unary fn, 2 args refused] FAIL {"command":"run","status":"ok","class":"pure-execution","result":{"result":"11","realization":"sha256:dfc4cb844da0044a4f
- runner realize: run-arity[binary apply realization, 1 arg refused] FAIL {"command":"run","status":"ok","class":"pure-execution","result":{"result":"12","realization":"sha256:ffe7656dc91cf25a4a
- runner realize: run-arity[binary apply realization, 3 args refused] FAIL {"command":"run","status":"ok","class":"pure-execution","result":{"result":"3","realization":"sha256:ffe7656dc91cf25a4aa
- runner hostile: script-is-directory FAIL rc=0 (a directory given as a script must not succeed)

## Defects found

- **D1** (high (fixed in ec2ec0b, found at 4fab549)): The ./omega shipped at commit 4fab549 (sha256 543d874d...d4d3) crashes on every realize/run path: `./omega --command 'let x: u64 = 7' --command 'let y: u64 = 11' --command 'x + y' --command 'realize _'` -> exit 139 (SIGSEGV in om_realization_show); `realize b` on a bool -> '*** stack smashing detected ***', exit 134. Campaign on that binary: 37 failures, 6 hostile crashes. Cause: tools/omega.c's object was not rebuilt when src/visor/visor.h changed (omega_main.d missing from VISOR_DEPS), so the binary mixed two struct layouts. The in-process console test (146/146) could not see it. ec2ec0b adds omega_main.d; a clean-checkout build of 4fab549 was not tested.
- **D2** (medium (wrong output, exit 0)): `run` does not check arity. `fn f(x: u64) -> u64 { x * 2 + 1 }` then `run f` prints 1 (f(0), x silently 0); `run f 5 6` prints 11 (6 ignored). On `x + y` (7, 11) after `realize _`: `run _ 1` prints 12 (first operand replaced, second kept), `run _ 1 2 3` prints 3 (third ignored). All status ok, exit 0.
- **D3** (low): `./omega --script /` (a directory) exits 0 with no output instead of refusing the script (exit 2 like a missing file).
- **D4** (low (cosmetic)): `alternatives x` on a value: 'error: alternatives: alternatives: only programs ...' (prefix doubled); `effects x` text-mode error lacks 'error:'; `help` says `graph [x]` but `graph` alone is an error.

## Test suites

| Target | Result | Tests | Failed | Note |
|---|---|---|---|---|
| test-visor-semantic | PASS | 95 | 0 |  |
| test-language | PASS | 407 | 0 |  |
| test-visor-verify | PASS | 47 | 0 |  |
| test-visor-evidence | PASS | 39 | 0 |  |
| test-visor-machine | PASS | 36 | 0 |  |
| test-visor-realization | PASS | 63 | 0 |  |
| test-visor-console | PASS | 284 | 0 |  |
| visor-authority-check | PASS | 1 | 0 |  |
| visor-physics-free-check | PASS | 1 | 0 |  |
| test-visor-world | PASS | 47 | 0 |  |
| test-visor-authority | PASS | 81 | 0 |  |
| test | PASS | 12 | 0 |  |
| test-m5 | PASS | 9 | 0 |  |
| test-m6 | PASS | 10 | 0 |  |
| test-m7 | PASS | 10 | 0 |  |
| test-m8 | PASS | 10 | 0 |  |
| test-m9 | PASS | 10 | 0 |  |
| test-m10 | PASS | 10 | 0 |  |
| test-m13 | PASS | 10 | 0 |  |
| test-m14 | PASS | 10 | 0 |  |
| test-m12 | NOT_RUN | - | - | GPU/silicon target excluded by lane-8 brief: GPU (living matvec) |
| test-m15 | NOT_RUN | - | - | GPU/silicon target excluded by lane-8 brief: accelerator/GPU |
| test-m17 | NOT_RUN | - | - | GPU/silicon target excluded by lane-8 brief: Blackwell GPU |
| test-m18 | NOT_RUN | - | - | GPU/silicon target excluded by lane-8 brief: GPU |
| test-m19 | NOT_RUN | - | - | GPU/silicon target excluded by lane-8 brief: GPU |
| test-m11 | NOT_RUN | - | - | not in the lane-8 regression list |

## Usability findings (not failures unless listed as defects)

- `true`/`false` echo as 1/0 although `type _` says bool; a user reads 1 as an integer.
- `help` shows the source form as `let x = <expr>`, but V0 requires `let x: u64 = <expr>` (the error message does explain).
- `help` lists `graph [x]` (argument optional) but `graph` with no argument is an error: 'give a name, _ or id'.
- `alternatives <value>` prints a doubled prefix: `error: alternatives: alternatives: only programs have alternative realizations in V1`.
- `effects <non-effect>` prints `'x' is not an EFFECT object` without the `error:` prefix in text mode (JSON correctly says status error; exit code is 1).
- `why <x>` is described as 'explain where x came from' but only explains realizations; on a value it says `use realize x first`, and `realize x` then refuses a value, a dead end.
- `authorize x`, `execute _` and `delete x` give parser jargon ('unexpected x after the end of the statement') instead of 'unknown command'; only `mint`/`grant`/... are reserved words, `authorize`/`submit`/`execute` are not.
- Errors raised before parsing are labelled with the command name `unknown` (`error: unknown: invalid UTF-8 at byte 4`, `error: unknown: line too long`).
- `id` prints the same hash three times (id, canonical_sha256) plus canonical_len; the relation is not explained.
- After `realize _`, `_` silently becomes the realization, so `inspect _`/`type _` now refer to a different object than one line earlier.
- Every u64 ADD has the same realization id (`realized` = the ADD operation, not the apply), so `x + y` and `x + 1` share one realization id; correct per spec but surprising in `bindings`/`compare`.
- `run _ a b` on a binary-apply realization runs the operation on the given numbers, not on the object's operands; nothing in the output says the object was not what ran.
- Machine/realization output uses hex profile codes (`profile 0x01 vs machine 0x01`), `physics flag=set seal=builder-constant placeholder`, and C function names (`omega_machine_estimate_latency(...)`, `omega_exec_native_f3`) as explanations; not readable without the C source.
- `verify` row INVARIANTS reports 399 generic checks that are 'not object-specific'; a user may read the PASS as evidence about their object.
- `evidence <name>` for a session object always says `no evidence`: receipts are repository files, not linked to session objects; the question 'what evidence supports this object' cannot be answered in V1.
- Blank lines produce no JSON object while comment-only lines produce one (`kind: none`); a script driver counting lines must know this.
- Commands after `quit` in --command/--script mode are silently dropped (exit 0).

## What this does NOT claim

- Host-only qualification on one DGX Spark (aarch64); no GPU, no silicon, no QEMU claim by the Visor.
- No measured or qualified cost: the Visor fills only predicted (static) and estimated (assumed machine model) cost; measured/qualified are ABSENT by design.
- Machine identity is the assumed canonical profile chosen from DMI, not an observed descriptor; the physics seal is a builder placeholder.
- World is unattached in the omega binary: `world` observes nothing; the snapshot API is covered only by the runtime-linked test-visor-world.
- Language V0 scope only: explicit-width unsigned integers, bool, let, + - * / & |, fn chains (x op c)...; realize/run cover u64 binary ADD/SUB/MUL/AND/OR only.
- Effects: the language cannot build an EFFECT object, so `run` on an effect is not reachable from the console; effect-request refusal is covered only by the C hostile suite (test-visor-authority) and the link check.
- GPU targets test-m12, test-m15, test-m17, test-m18, test-m19 and all Blackwell paths were not run (lane-8 brief); test-m11 was not in the list.
- The pre-existing test-m5 gate launches QEMU and writes build/qemu_omega_runner.bin under the shared build/ directory (hard-coded path in omegatool).
- The e2e session golden (tests/visor/sessions/e2e.expected) is compared host-independently only after ec2ec0b; the lane-8 goldens mask the machine block, machine name/id and estimated cycles.
- Determinism is shown across fresh processes on this host only, not across hosts, compilers or ABIs (golden bytes are pinned to LP64/aarch64).
- Visor V1 is not claimed safe against a hostile local user with write access to the binary or the evidence directory.

## How to re-run

```
python3 tools/qualify_visor.py            # everything, writes a new receipt
make OUT_DIR=build/q8r PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 build/q8r/omega
bash tests/visor/qualification/run_qualification.sh build/q8r/omega   # console campaign only
```
