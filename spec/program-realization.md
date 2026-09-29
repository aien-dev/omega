# Program realization V0: realization is driven by the actual Omega program

Status: implemented (`OMEGA_PROGRAM_REALIZE_*`). Supersedes the fixed-schedule behaviour of
`omega_synthesize_realization` described in `spec/realize_synth.md` (M14), whose gates still
hold, and fixes the two defects listed in `spec/visor.md` section 6a.

## 1. Defects fixed

1. `omega_program_realize` was declared in `src/omega_program.h` and defined nowhere.
2. `omega_synthesize_realization(task, result)` ignored `task->program` except for its id and
   always emitted `f(x) = 3x - 2` (`movz 3; movz 2; mul; sub; ret`), checking itself against the
   hard-coded `3x - 2` on seven inputs. Every program "realized" to 3x-2 under its own id.

## 2. Pipeline

```
OmegaProgram (canonical body + contract, program id v2)
  -> omega_program_realize_check      verified semantic operation sequence (fail closed + reason)
  -> omega_realize_choose_schedule    machine-aware planning: schedule choice only
  -> omega_program_emit_schedule      AArch64 V0 bytes through the one encoder (aarch64_encoder.c)
  -> REALIZATION_ID                   canonical: OMG_R0(profile, program_id, code)
                                      machine-bound: triple(program_id, machine_id, code)
  -> omega_realization_verify_program bindings + V0 structural + native == semantic evaluator
```

Nothing reads Visor source text: realization reads only `OmegaProgram.body` and `.contract`.

### 2.1 Semantic gate (`omega_program_realize_check`)

Accepted: body present; `input_type == output_type == TYPE_UNSIGNED_INT`;
`input_width == output_width` in {8, 16, 32, 64}; 0 to 64 steps, each `ADD SUB MUL AND OR` with
a constant that fits the width; `program_id` equal to the v2 id recomputed from body + contract
(a stale or forged id is refused). Semantics are those the id binds: each step is
`omega_build_op_binary(op, OVERFLOW_WRAP, uW)`, i.e. arithmetic mod 2^W.

| Semantic | Status | Why |
|---|---|---|
| `x + c`, `x - c`, `x * c` (u8/u16/u32/u64, wrap) | supported | 64-bit ADD/SUB/MUL; for W < 64 one final `AND` with `2^W - 1` (these ops commute with reduction mod 2^W) |
| `x & c`, `x \| c` | supported | AND / ORR (register form) |
| constants up to 64 bits | supported | `MOVZ` chunk 0, then `MOVK lsl 16/32/48` for each nonzero chunk |
| composed unary pipelines (<= 64 steps) and the empty chain (`f(x) = x`) | supported | one instruction group per step |
| `DIV` | fail closed | not in the V0 body op set (no program id), no UDIV emitter, and the evaluator (x/0 = error) and UDIV (x/0 = 0) disagree |
| any other opcode (NOT, SELECT, comparisons, ...) | fail closed | not in the V0 body op set |
| signed integers, bitvectors | fail closed | result representation (sign extension in X0) is not specified |
| widths other than 8/16/32/64; `in != out` width or type | fail closed | V0 never converts |
| constant wider than the declared width | fail closed | would be truncated by the canonical VALUE builder |
| no body / stale or zero program id / more than 64 steps | fail closed | no meaning / not this program |

Builder limits are unchanged: `omega_program_build_unary_op` and Language V0 still refuse
constants above `0xFFFFFFFF` (spec/program-identity.md section 4). Wide constants reach the
realization only through bodies built directly.

### 2.2 Machine-aware planning (what the MachineGraph may affect)

The MachineGraph chooses the **schedule**, never the meaning. Both schedules compute the same
function, and the differential check proves it for every realization.

- `OMEGA_SCHED_SEQUENTIAL` (issue width < 4, or < 16 GPRs; e.g. canonical QEMU virt): per
  step, load the constant into X1 then apply the op. This is byte-identical to the historical
  builder template, so `omega_program_emit_body` is simply this schedule at width 64.
- `OMEGA_SCHED_PRELOAD` (issue width >= 4 and >= 16 GPRs; e.g. canonical DGX Spark): in blocks
  of up to 15 steps, load all constants of the block into X1..X15 first (independent loads),
  then apply the ops in body order.

Only caller-saved scratch registers X1..X15 are ever written besides X0 (never X16..X30), so
the code is safe to call in process. The machine profile must be AArch64 V8A (0x01).
For `(x*3)-2` the two schedules reproduce the historical M14 bytes exactly (checked).

### 2.3 Identities

- `omega_program_realize(prog)` (machine-independent): SEQUENTIAL schedule,
  `realization.semantic_id = program_id`, id `omega_compute_realization_id` (OMG_R0 over
  profile, semantic id, code). Before this change the builder left `semantic_id` zero, so the
  realization id did not bind the program; it now does (golden change, section 5).
- `omega_synthesize_realization(task, result)` (machine-bound):
  `REALIZATION_ID = omega_realize_compute_triple_id(program_id, machine_id, code)`.
- `omega_program_build_unary_op` and `omega_program_compose` now obtain their realization
  from `omega_program_realize` (compose no longer splices the parts' code; for template parts
  the compiled bytes equal the old splice, checked on 50 random pairs).

### 2.4 Verification (`omega_realization_verify_program`)

1. the program passes the semantic gate (its id recomputes from its body);
2. `real.semantic_id == program_id`;
3. the machine graph's id recomputes from its content; `real.machine_id == machine_id`;
   profile matches;
4. V0 structural (decode, terminal RET) including the recomputed triple REALIZATION_ID;
5. differential: native execution of the bytes (mmap'd executable page, `omega_exec_native_f3`)
   equals `omega_program_eval` on 84 inputs per program: 0, 1, 2, 3, 5, 7, 10, 100, max,
   max-1, max-2, 2^(W-1) and neighbours, alternating-bit patterns, 16/32-bit edges, and
   seeded random values (seed = first 8 bytes of the program id).

`omega_program_eval` is independent of the emitter: it lowers the body to the canonical
objects the program id binds and evaluates APPLY/OPERATION/VALUE/PARAM with the core
definition `omega_eval_pure_binary_uint`. It refuses inputs outside `[0, 2^W)`.

Return codes: 0 pass, -1 fail (reason), -3 not verifiable on this host (non-AArch64: native
execution unavailable; never a silent pass). `omega_synthesize_realization` returns 0 solved,
-2 refused by the gate, -3 not verifiable here, -1 verification failed.

## 3. Qualification

`make test-program-realize` (tests/realize/test_program_realize.c, physics-free, AArch64 host;
CI: `omega-faculty-arm` job). Receipt: `tools/program_realize_receipt.sh` ->
`evidence/REALIZE/<sha256>.json`.

| Gate | What it proves |
|---|---|
| `OMEGA_PROGRAM_REALIZE_PASS` | `omega_program_realize` compiles every named program, binds the program id, equals the SEQUENTIAL schedule, executes as the evaluator says; builder/compose realizations equal it; 11 unsupported semantics fail closed with a reason (also in the synthesizer) |
| `OMEGA_REALIZATION_PROGRAM_DRIVEN_PASS` | schedule follows the machine; sequential code lifts back to the body; distinct programs (600: 18 named + 582 seeded random) never share code or RealizationId, per machine and across machines; determinism; M14 3x-2 bytes preserved; the retired fixed-3x-2 synthesizer (kept in the test only) emits identical code for every program and is rejected by verification for 17 of 18 named programs |
| `OMEGA_REALIZATION_DIFFERENTIAL_PASS` | native == semantic evaluator for x+1, x+11, x-2, x*3, (x*3)-2, (x+7)*5, x & mask, x \| mask, wide 64-bit constants, u8/u16/u32 programs, identity, a 64-step pipeline and 582 seeded random pipelines (1-64 steps, 16/32/48/64-bit constants), on both canonical machines, 84 + 32 inputs each |
| `OMEGA_REALIZATION_TRIPLE_BIND_PASS` | triple recomputes; code, machine and semantic id each change it; hostile: A's code re-stamped with B's id and a consistent triple fails the differential; A's realization verified as B fails; altered code (stale or re-hashed id) fails; swapped machine id (stale or re-hashed) fails; forged machine graph fails |
| `OMEGA_REALIZATION_REGRESSION_PASS` | receipt only: all non-energy host suites pass (see the receipt tool) |

## 4. Non-claims

- Only the V0 unary constant-step chain. No calls, control flow, memory, signed arithmetic,
  division, multiple parameters, or register allocation beyond X0..X15.
- No optimisation: no constant folding, immediate forms or strength reduction. The schedule
  choice is a policy, not a measured optimum; no cost is measured.
- The differential is sampling (84-116 inputs per program), not a proof of equivalence.
- The machine graphs are the two canonical profiles (assumed, not observed hardware).
- `omega_program_verify` is unchanged (V0 + V2 only); the program-aware check is
  `omega_realization_verify_program`.

## 5. Golden changes

Code bytes of every existing golden are unchanged (Language V0 `P` rows, M14 schedules,
Visor sessions). Realization ids of canonical program realizations change because the id now
binds the program id:

| Golden | Old | New |
|---|---|---|
| `tests/visor/sessions/e2e.expected` (+ `test_visor_console.c`), `realize transform` id | `sha256:dfc4cb844da0044a4fe0c58c5ecd0b77d1ad04df3f3037609f8ed8581b058bff` | `sha256:87c6fbd6d54c90525d018c65138314eb404cba4efa8d66d42dab17441e609ba6` |
| same block, `realized` | `none` | `sha256:83bd39597742d3bb83e5140c662532c0a2ecb59e5f868e38be16276981644543` (the program id) |
| `tests/visor/test_visor_realization.c` | `synth@` for `add5` flagged MISMATCH (fixed 3x-2) | `synth@` compiled from the program, compatible, runs 10+5 = 15 |
