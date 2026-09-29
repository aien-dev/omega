# Omega Visor V1 — terminal-first semantic interface

Status: V1 (terminal viewport). Start commit `ceb68d69b6d0b6b11066d4a64f9fadc3f7bf21b5`.

## 1. What the Visor is

Omega Visor is the human/agent interface for inspecting, constructing, verifying,
realizing, executing, comparing and understanding Omega semantic state. It is a
*viewport* over the existing canonical objects, not a second semantic system.

```text
OMEGA
├── Omega Language   (spec/omega-language-v0.md)     surface syntax, disposable
├── Omega Compiler   (src/language/omega_lower.c)     lowers into OmegaGraph
├── Omega Runtime    (src/runtime/)                   resident World, generations
├── Omega Library    (src/omega_library.c)
└── Omega Visor      (src/visor/, tools/omega.c)
       ├── Console            visor_console, visor_parse_command
       ├── Semantic Explorer  visor_semantic
       ├── World Explorer     visor_world
       ├── Machine Explorer   visor_machine
       ├── Evidence Explorer  visor_evidence (+ visor_verify)
       ├── Cortex Explorer    (V3, not built)
       ├── J-Space Explorer   (V3, not built)
       └── Realization Lab    visor_realization
```

## 2. Non-negotiable rules (enforced by code and tests)

1. `OmegaGraph` is the only source of meaning. Every view derives from canonical
   objects; nothing in `src/visor` stores a second copy of semantics.
2. No second AST. The language parse tree (`src/language`) is scratch storage that
   is lowered and discarded; its identity is never used.
3. The Visor may inspect, propose, construct, simulate, or *request*. It cannot
   grant. `VisorEffectRequest` has no authorized state and no setter
   (spec/visor-authority.md). The link check `tests/visor/check_authority_link.sh`
   fails the build if any console/visor/language object references an
   authority-mutating runtime symbol.
4. Effectful work: `Visor -> semantic request -> EffectIntent -> AEGIS/PHYSICS
   authority -> bounded execution -> receipt`. The console prints that route and
   refuses to execute.
5. No hidden reasoning is shown. Output is typed views: objects, verification rows,
   realizations, cost classes, evidence records, machine models.
6. `tools/omegatool.c` is untouched; it remains the qualification/reference tool.
7. The `omega` binary is physics-free: it links only the core object set in
   `mk/00-visor-core.mk` and builds with `PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0`.

## 3. Session model (`src/visor/visor.h`, lead-owned)

A `VisorSession` holds one `OmegaGraph`, named bindings (`let x ...`, `fn ...`),
the `_` last result, and bounded tables of `OmegaProgram`s and realizations
(`VisorRealizationEntry`). A binding names an object id, a program index or a
realization index; `visor_resolve` accepts `_`, a name, a 64-hex id or
`sha256:<hex>` and fails closed on anything else.

A realization entry records both `subject_id` (what the user asked to realize, e.g.
an apply) and `real.semantic_id` (what the existing realizer keyed on, e.g. the
operation). Neither is invented; both are shown.

## 4. Commands (V1)

| command | class | backing API |
|---|---|---|
| `help`, `quit`, `bindings`, `clear` | console | visor_console |
| `let x: u64 = 7`, `x + y`, `fn ...` | construction | omega_language_eval_line -> OmegaGraph builders |
| `inspect <x>`, `type <x>`, `id <x>`, `graph <x>` | inspection | visor_semantic |
| `verify <x>` | inspection | visor_verify (existing omega_verify_* only) |
| `machine` | inspection | visor_machine |
| `realize <x> [for current.machine]`, `alternatives`, `compare`, `why`, `cost` | inspection / construction | visor_realization |
| `run <x>` | pure execution | visor_realization_run_pure -> omega_exec_native_f3; refused when `visor_effect_request_classify` says authority is required |
| `evidence <x>` | inspection | visor_evidence over `evidence/` receipts |
| `world` | inspection | visor_world (unattached in the V1 binary; see §6) |
| `effects <x>` | effect request | visor_effect_request (unauthorized request only) |

Modes: interactive `omega`; `omega --command "<line>"` (repeatable);
`omega --script <file>` (`-` = stdin); `--json` prints one JSON object per
command. JSON is a serialization of the *view*, never canonical bytes.

Result printed for `x + y` comes from `visor_semantic_eval_u64`, which walks the
canonical apply and calls the existing `omega_eval_pure_binary_uint` with the
opcode/overflow/width read from the OPERATION object. `run` executes the
realization's machine code. Verify V1 (differential) checks the two agree.

## 5. Cost and evidence vocabulary

`predicted` (static model), `estimated` (machine-model latency), `measured`
(from a receipt), `qualified` (measured under a qualification run) are four
separate slots in `VisorCostView`; V1 never fills `measured` or `qualified`.
Evidence scope is reported exactly as the receipt states it (`host`, `qemu`,
`silicon`, `simulated`, or `unknown`); the Visor never upgrades scope.

## 5a. Behaviour rules worth knowing

- `run` argument count fails closed: an apply with N operands takes either no
  explicit arguments (operands come from the graph) or exactly N (full override);
  a program takes exactly one input. Anything else is an error and nothing runs.
- After `realize <x>`, the `_` binding becomes the new realization (so `cost _`,
  `run _`, `evidence _` refer to it). This is intended; `bindings` shows it.
- Only pure binary u64 applies (ADD/SUB/MUL/AND/OR, wrap overflow) and `fn`
  programs realize and run in V1. Everything else is refused with a reason.
- `--script` refuses directories and other non-regular files with exit 2; a
  character device such as `/dev/null` reads as an empty script (exit 0).
- Blank script lines produce no JSON object; comment lines do.

## 6. Known gaps in V1 (non-claims)

- The `omega` binary does not link the resident runtime, so `world` reports
  "no resident World attached". The snapshot API (`visor_world_snapshot`) is built
  and tested separately against a real `RxWorld`. Lane 6 found the runtime's
  world/caproot/coherent sources are themselves physics-free, so a later version
  can attach a live World without a physics checkout.
- Blackwell: `omega_vector`, `omega_blackwell_qmd/encoder/realize` link
  physics-free and are in the `omega` binary, so the machine view lists the
  GB10 target as declared and linked; `run` on it is always refused
  ("requires GPU submit authority; not linked in Visor V1").
- Language V0 covers explicit-width integers, bool, `let`, pure `+ - * / & |`,
  and `fn` bodies of the shape `((x op imm) op imm ...)`. Everything else fails closed.
- No measured or qualified cost is produced by the Visor itself.
- Hardware identity is an assumed canonical profile unless a Physics descriptor was
  ingested (never in V1); the machine view says so in `provenance`.

## 6a. Findings in existing code (found by the Visor lanes; not fixed here)

- FIXED by program realization V0 (`spec/program-realization.md`): `omega_program_realize`
  was declared in `omega_program.h` but defined nowhere. It now compiles the canonical body;
  `realize` goes through it (never through code the program happens to carry).
- FIXED by program realization V0: `omega_synthesize_realization` ignored its program and
  always emitted the fixed `f(x) = 3x - 2` schedule. It now compiles the program for the
  machine and verifies native execution against the semantic evaluator; the `synth@`
  alternatives are that machine-aware realization, still cross-checked against the direct realization.
- FIXED by program identity v2 (`spec/program-identity.md`): `omega_program_compute_id`
  did not hash the program body (same name and contract with a different body gave the
  same `program_id`, and the language refused the redefinition as an "identity collision").
  The id now binds the canonical semantic body and the contract (not the name); redefining
  `fn t` with a different body yields a distinct id and rebinds `t`. The console still
  prints the realization id next to the program id.
- `evidence/M19R/c4d87451….json` is hash-named but its name does not match the
  SHA-256 of its bytes (observed only, not judged).
- Since main widened capability generations to 64 bits (PR "cap-generation-64",
  merged after this work started), `EffectPayload.capability_generation` in
  `omega_types.h` is still 32-bit, so an Omega EFFECT object cannot carry a live
  capability reference: the runtime root sees every such reference as stale. This
  is fail-closed but means the honest effect path is unrepresentable until the
  canonical effect format is widened (an identity-changing core change, separate PR).
  Resolved by `spec/effect-cap64-migration.md` (effect object version 0x02,
  64-bit generation).
- Both canonical machine builders set `is_physics_authorized=true` with a
  constant placeholder seal; the machine view labels it as such.
- Host CPU part numbers (0xd85/0xd87) are not Neoverse-V2; the DGX Spark profile
  is chosen from DMI and labelled assumed, never observed.

## 7. Qualification

`tests/visor/qualification/` + `tools/qualify_visor.sh` (shell glue only; every
assertion lives in C tests and `.omega-session` scripts). Run it on a committed
tree: `tools/qualify_visor.sh [--skip-regression] [--no-asan] [out-dir]`. It
rebuilds the AIENOS capability library from the commit in `aienos.lock` (as CI
and `tools/effect_cap64_receipt.sh` do) and records its identity (commit, source
tree, lib sha256) in the receipt. Receipts live in `evidence/VISOR/` as
content-addressed JSON, never overwritten; the tool writes only a new receipt and
never edits tracked files (`evidence/VISOR/VISOR_V1_QUALIFICATION.md` is the frozen
summary written by the retired Python glue up to receipt #5):

| receipt | commit | verdict | note |
|---|---|---|---|
| `12785aab…json` | ec2ec0b | OMEGA_VISOR_V1_FAIL | found `run` arg-count and `--script` directory defects |
| `4de74cf3…json` | d7e8a4c | OMEGA_VISOR_V1_PASS | after the fixes; 69 hostile cases, 0 crashes |
| `98a63b2e…json` | 23feaad | OMEGA_VISOR_V1_PASS | clean tree (`tree_dirty=false`), same counts as receipt #2 |
| `15d6703c…json` | 6627da2 | OMEGA_VISOR_V1_FAIL | pre-fix merge of 64-bit effect generations |
| `632ba16d…json` | 8a7d95e | OMEGA_VISOR_V1_PASS | merged tree, authority test adapted to 64-bit generations |

Scope of every receipt: host-only, this DGX Spark, no GPU/silicon claim, GPU gate
suites (m12/m15/m17–m19) not run because the Visor does not touch them. Gates:

```text
OMEGA_VISOR_SEMANTIC_PASS            language golden vectors + semantic inspection tests
OMEGA_VISOR_VERIFY_PASS              verify/evidence tests + end-to-end verify script
OMEGA_VISOR_REALIZE_PASS             machine/realization tests + realize/cost/run script
OMEGA_VISOR_AUTHORITY_ISOLATION_PASS hostile tests + link check
OMEGA_VISOR_REGRESSION_PASS          existing CPU gates unchanged; GPU gates skipped when .spark-quiet exists
OMEGA_VISOR_V1_PASS                  all of the above, recorded in evidence/VISOR/
```

## 8. After V1 (documented, not implemented)

- **V2** semantic graph visualization, machine topology visualization, realization
  overlays, evidence timelines, World generation timeline. Enabled by: every view
  has a JSON serializer and stable ids; `visor_world_snapshot` is versioned.
- **V3** Cortex Explorer, J-Space branch explorer, semantic diff across generations,
  branch simulation controls. Enabled by: the world snapshot family is designed to
  attach J-Space/Cortex views (extension points named in `visor_world.h`).
- **V4** AIEN conversational interface: natural language -> typed Visor requests;
  explanations backed by explicit evidence. Enabled by: the console dispatches
  structured `VisorCommand`s, so a language model can emit them instead of text.
- **V5** native graphical environment over AIENOS/Omega with no desktop dependency.
  Enabled by: no view depends on stdio; all formatters write into caller buffers.
