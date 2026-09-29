# Omega Visor V1 qualification (lane 8)

Verdict: **OMEGA_VISOR_V1_PASS**  
Receipt: `evidence/VISOR/98a63b2e17af68c97cf021612b43387497a57c82bbeb9d206a9b32f6bb21e7e6.json`  
Commit: `23feaad305c07977ef0115e59474295a679cfd0b` (tree dirty: False, see dirty_files)  
Run: 20260929T045019Z on aarch64 7.0.0-1019-nvidia (NVIDIA_DGX_Spark)

Scope: host-only: no GPU/silicon claim; no QEMU claim by the Visor (the pre-existing test-m5 gate runs its own QEMU check)

## The five gates in plain words

| Gate | What it checks | Result | Tests | Failed |
|---|---|---|---|---|
| OMEGA_VISOR_SEMANTIC_PASS | Same meaning gets the same fingerprint (7, 07, 0x07, 0b111; spacing; comments); different widths differ; repeat runs match byte for byte | PASS | 541 | 0 |
| OMEGA_VISOR_VERIFY_PASS | The checker and the evidence viewer report what is really there, and a new user can find it from `help` | PASS | 151 | 0 |
| OMEGA_VISOR_REALIZE_PASS | Machine code built for an expression gives the same answer as the language; cost is labelled estimate, never measured; scripting behaves | PASS | 441 | 0 |
| OMEGA_VISOR_AUTHORITY_ISOLATION_PASS | The console can ask but never grant; bad or hostile input is refused cleanly and never crashes it | PASS | 177 | 0 |
| OMEGA_VISOR_REGRESSION_PASS | The older CPU milestone gates still pass and the tool still builds without the physics checkout | PASS | 92 | 0 |

Hostile inputs: 69 tried, 60 refused cleanly, 0 crashed, 4 harmless blank/comment lines.
Repeatability: 3 runs, identical: True.  Memory-checker build: {"status": "RUN", "sanitizer_reports": "none", "crashed": 0, "same_case_failures_as_release": true}.

## Receipt history

| # | Receipt | Commit | Verdict | What changed |
|---|---|---|---|---|
| 1 | `evidence/VISOR/12785aab7bb22bc1fd1d4e71143c15f98f1e4bc94885e9b2a725487b023b7c93.json` | `ec2ec0b` | OMEGA_VISOR_V1_FAIL | first run: REALIZE failed on run arity (D2), AUTHORITY_ISOLATION failed on `--script /` exit 0 (D3); zero authority-path failures |
| 2 | `evidence/VISOR/98a63b2e17af68c97cf021612b43387497a57c82bbeb9d206a9b32f6bb21e7e6.json` | `23feaad` | OMEGA_VISOR_V1_PASS | this run |

## Why the verdict is what it is

- All five gates passed.

## Defects found

- **D1** (high; FIXED in ec2ec0b (found at 4fab549)): The ./omega shipped at commit 4fab549 (sha256 543d874d...d4d3) crashed on every realize/run path (exit 139 SIGSEGV in om_realization_show; 'stack smashing detected', exit 134). Cause: omega_main.d missing from VISOR_DEPS, so tools/omega.c was not rebuilt when src/visor/visor.h changed. ec2ec0b adds it; a clean-checkout build of 4fab549 was not tested.
- **D2** (medium; FIXED in d7e8a4c (open in receipt #1)): `run` did not check arity: `run f` -> 1, `run f 5 6` -> 11, `run _ 1` -> 12, `run _ 1 2 3` -> 3, all exit 0. At d7e8a4c: 'program f takes 1 input; give it on the command line', 'program f takes 1 input; got 2', '_ takes 2 operands; got 1 (give none, or all 2)', '... got 3 ...'; exit 1.
- **D3** (low; FIXED in d7e8a4c): `./omega --script /` exited 0; at d7e8a4c it prints 'error: --script: / is a directory' and exits 2.
- **D4** (cosmetic; FIXED in d7e8a4c): Doubled 'alternatives:' prefix, `effects x` error without 'error:', help `graph [x]` and `let x = <expr>`, parser jargon for `authorize x`/`execute _`: all corrected (hostile.expected and usability.expected regenerated; the only other changed line is the `run <type-id>` message, now '... is not an APPLY; only pure binary u64 applies run in V1', still exit 1).

## Test suites

| Target | Result | Tests | Failed | Note |
|---|---|---|---|---|
| test-visor-semantic | PASS | 95 | 0 |  |
| test-language | PASS | 407 | 0 |  |
| test-visor-verify | PASS | 47 | 0 |  |
| test-visor-evidence | PASS | 39 | 0 |  |
| test-visor-machine | PASS | 36 | 0 |  |
| test-visor-realization | PASS | 63 | 0 |  |
| test-visor-console | PASS | 306 | 0 |  |
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
- `why <x>` is described as 'explain where x came from' but only explains realizations; on a value it says `use realize x first`, and `realize x` then refuses a value, a dead end.
- Errors raised before parsing, and unknown commands, are labelled `unknown` (`error: unknown: invalid UTF-8 at byte 4`, `error: unknown: unknown command or invalid source line: 'authorize'`).
- `id` prints the same hash three times (id, canonical_sha256) plus canonical_len; the relation is not explained.
- After `realize _`, `_` silently becomes the realization, so `inspect _`/`type _` now refer to a different object than one line earlier.
- Every u64 ADD has the same realization id (`realized` = the ADD operation, not the apply), so `x + y` and `x + 1` share one realization id; correct per spec but surprising in `bindings`/`compare`.
- `run _ a b` (exactly all operands) on a binary-apply realization runs the operation on the given numbers, not on the object's operands (7+11 -> `run _ 1 2` prints 3); allowed by design after the D2 fix, but the output does not say the object's own operands were replaced.
- Machine/realization output uses hex profile codes (`profile 0x01 vs machine 0x01`), `physics flag=set seal=builder-constant placeholder`, and C function names (`omega_machine_estimate_latency(...)`, `omega_exec_native_f3`) as explanations; not readable without the C source.
- `verify` row INVARIANTS reports 399 generic checks that are 'not object-specific'; a user may read the PASS as evidence about their object.
- `evidence <name>` for a session object always says `no evidence`: receipts are repository files, not linked to session objects; 'what evidence supports this object' cannot be answered in V1.
- Blank lines produce no JSON object while comment-only lines produce one (`kind: none`); a script driver counting lines must know this.
- Commands after `quit` in --command/--script mode are silently dropped (exit 0).
- `--script /dev/null` (a character device, not a regular file) is accepted as an empty script (exit 0), although the round-2 note says non-regular files exit 2; harmless, but the claim and behaviour differ.

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
