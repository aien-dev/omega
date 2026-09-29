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

## 6. Known gaps in V1 (non-claims)

- The `omega` binary does not link the resident runtime, so `world` reports
  "no resident World attached". The snapshot API (`visor_world_snapshot`) is built
  and tested separately against a real `RxWorld` with the physics include path.
- Blackwell: listed as a declared target only if its objects link physics-free
  (see lane 5 report); `run` on it is refused in V1.
- Language V0 covers explicit-width integers, bool, `let`, pure `+ - * / & |`,
  and `fn` bodies of the shape `((x op imm) op imm ...)`. Everything else fails closed.
- No measured or qualified cost is produced by the Visor itself.
- Hardware identity is an assumed canonical profile unless a Physics descriptor was
  ingested (never in V1); the machine view says so in `provenance`.

## 7. Qualification

`tests/visor/qualification/` + `tools/qualify_visor.py` (glue only; every
assertion lives in C tests and `.omega-session` scripts). Gates:

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
