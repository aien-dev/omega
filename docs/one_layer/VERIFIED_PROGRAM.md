# Verified program: objective-bound Omegese to executable result with an independent receipt

**Status**: v1, host CPU only (omega#505, lane F of aien-dev/aien-architecture-mv#231).
**Code**: `tools/verified_program/vp.c` (runner), `tools/verified_program/vp_verify.c` (verifier),
`mk/verified-program.mk`, gate `tests/gates/omega-verified-program.gate`.

## 1. What it does

One request goes through the existing chain and nothing else:

```
program (Omegese, op list or OSC IR)
  -> omegese emit -> omega_emit emit -> canonical OSC IR
  -> checker2 check (ACCEPT or a named rule)
  -> grant check (objective, tier, epoch window, effects, call limit)
  -> code image (osc_cg_compile) -> checker2 receipt issue (TEST class: binds IR, code, binding, inputs)
  -> preflight on the exact bytes to run
  -> execution in a child under seccomp strict mode
  -> execution receipt, content-addressed store, measurements
```

`verified_program run` says only that the program ran under its grant. Whether the result is right is
decided separately by `vp_verify`, which believes nothing the runner wrote and recomputes every answer
with its own reference function for the task (the oracle). Syntax acceptance and checker2 acceptance
never count as "correct": the negative controls include programs that pass both and are still wrong.

## 2. Authority

- The **objective id** is SHA-256 of `AIEN_E2E_OBJECTIVE_V1`, one newline byte, and the objective text
  (the same rule as aien-sovereign-core `objective.rs`). Both tools recompute it.
- The **grant** is written by the runtime authority (AIENOS / sovereign-core approval desk). Omega does not
  mint, widen or authenticate grants. The runner checks that the grant names the same objective and tier,
  that the epoch is inside its window, that the call count is within its limit and that its effects cover
  what the checked IR needs. The verifier repeats every one of these checks from the stored grant.
- **checker2** judges the IR bytes. It grants nothing.
- Model output never authorizes anything (PST1 section 8 item 5).

## 3. Request (`omega.verified_program.request.v1`)

Text, one `key value` line per key, exactly this order, newline-terminated. Relative paths are resolved
against the request file's directory.

| key | value |
|---|---|
| `schema` | `omega.verified_program.request.v1` (anything else: `UNKNOWN_VERSION`) |
| `objective_id` | 64 lowercase hex |
| `objective_text` | the text, printable, one line |
| `program_format` | `omegese-v1`, `oplist-v1` or `oscir-v1` |
| `program` | path to the program bytes (at most 1 MiB) |
| `entry` | function name |
| `params` | space-separated scalar kinds (`bool u8 u16 u32 u64 i8 i16 i32 i64`), at most 6, may be empty |
| `ret` | scalar kind |
| `inputs` | path: one call per line, decimal values separated by single spaces |
| `tier` | `host-cpu-aarch64` (anything else: `TIER_UNSUPPORTED`) |
| `grant` | path to the grant |
| `epoch` | positive integer |
| `pin_checker2_id` | checker2 `checker_id`, or `-` for no pin |
| `pin_omegese_sha256` | SHA-256 of the omegese binary, or `-` |
| `pin_omega_emit_sha256` | SHA-256 of the omega_emit binary, or `-` |

Only scalar signatures are accepted: the entry function takes and returns plain values, so the inputs
and the result are fully described by the input and output text.

## 4. Grant (`aien.verified_program.grant.v1`)

Same text layout: `schema`, `objective_id`, `tier`, `effects` (space-separated, from `pure`, `heap`,
`caller_memory`; `pure` is required), `epoch_first`, `epoch_last`, `max_calls` (1 to 4096).

Effects are recomputed from the IR, never declared by the program:

| effect | needed when the IR contains |
|---|---|
| `pure` | always |
| `heap` | any runtime-managed memory: `alloc`/`release` arrays, arenas, pools, handles |
| `caller_memory` | `sload`/`sstore`, or any register of the bytes or cells kind |

## 5. Outputs

`<outdir>/` (one run per directory):

- `store/<sha256>`: every artifact by the SHA-256 of its bytes, read-only: objective text, program
  bytes, op list, IR, code image, canonical inputs, grant, binding object, checker2 receipt, outputs.
- `receipts/<id>.vpr`: the execution receipt; `id` is the SHA-256 of the file. Written only on EXECUTED.
- `result.txt`: `verdict` (EXECUTED or REFUSED), `rejection`, `stage`, `detail`, digests.
- `measure.txt`: `wall_ns`, `user_us`, `sys_us`, `maxrss_kb`, `calls`, `ir_bytes`, `code_bytes`. Kept
  out of the receipt so the receipt is deterministic: the same request gives the same receipt id.

Outputs text: one line per call, `R <value>` (the return value, decimal in the `ret` kind) or `T <code>`
(the call stopped with that OSC trap code, for example 1 for arithmetic overflow).

The **binding object** (`omega.verified_program.binding.v1`: objective id, grant digest, entry, params,
ret, tier, epoch) is passed to `checker2 receipt issue --requirements`, and the canonical inputs to
`--input`, so the signed checker2 receipt covers the IR, the code image, the objective binding and the
inputs. The execution receipt (`omega.verified_program.receipt.v1`) records, in fixed order: objective,
grant, binding, program, op list, IR, code, inputs and outputs digests; entry and signature; call count;
effects required and granted; checker2 verdict, identity, receipt id and signer class; digests of the
omegese, omega_emit, checker2 and runner binaries; commit; tier; host CPU identity (architecture plus the
distinct `implementer:variant:part:revision` core kinds from `/proc/cpuinfo`); `machine_id=absent` (no
provisioned machine identity on this host); `gpu_used=no`; `sandbox=seccomp-strict`; epoch.

## 6. Refusals

Runner (`verified_program run`, exit 1, no receipt): `UNKNOWN_VERSION`, `REQUEST_MALFORMED`,
`TIER_UNSUPPORTED`, `TIER_UNAVAILABLE`, `OBJECTIVE_MISMATCH`, `GRANT_MISSING`, `GRANT_MALFORMED`,
`GRANT_OBJECTIVE`, `GRANT_TIER`, `GRANT_EPOCH`, `PIN_MISMATCH`, `PROGRAM_MISSING`, `EMIT_REFUSED`,
`LOWER_REFUSED`, `CHECKER_REJECT` (detail names the checker2 rule), `DECODE_REFUSED`,
`SIGNATURE_MISMATCH`, `UNAUTHORIZED_EFFECT`, `INPUT_MALFORMED`, `INPUT_RANGE`, `INPUT_LIMIT`,
`BACKEND_REFUSED`, `PREFLIGHT_IR_CHANGED`, `PREFLIGHT_CODE_CHANGED`, `PREFLIGHT_CODE_INVALID`,
`EXEC_FAILED`.

Preflight, immediately before execution: the IR and code bytes are read back from the store and
re-hashed, checker2 runs again on the IR file, every code word is decoded and re-encoded
(`osc_cg_verify`), and the IR is recompiled independently of the first compile; the result must be the
same bytes and entry offset. Execution then maps exactly those bytes.

Verifier (`vp_verify`, exit 1, first failure only), in check order: `RECEIPT_MISSING`,
`RECEIPT_ID_MISMATCH`, `UNKNOWN_VERSION`, `RECEIPT_MALFORMED`, `OBJECTIVE_MISMATCH`, `STORE_MISSING`,
`STORE_TAMPERED`, `GRANT_MALFORMED`, `GRANT_OBJECTIVE`, `GRANT_TIER`, `GRANT_EPOCH`, `GRANT_LIMIT`,
`CHECKER_REJECT`, `UNAUTHORIZED_EFFECT`, `BINDING_MISMATCH`, `CHECKER_ID`, `C2_RECEIPT_INVALID`,
`C2_BINDING`, `C2_INPUT`, `HOST_MISMATCH`, `ORACLE_SIGNATURE`, `ORACLE_DOMAIN`, `OUTPUT_MALFORMED`,
`WRONG_RESULT`.

`vp_verify` links checker2's own decoder and `src/sha256.c`; it links nothing from `src/compiler`. It
calls the checker2 binary for `check`, `version` and `receipt verify`, and reads the requirements
(offset 16) and input (offset 272) fields of the 416-byte checker2 receipt itself, because `receipt
verify` does not compare them.

## 7. Mapping to PST1 (aien-dev/aien-protocols-mv#34, draft)

| PST1 | here |
|---|---|
| `objective_id` header field | `objective_id` (same derivation) |
| `completed` payload (32-byte result digest) | `outputs_sha256` |
| `receipt_reference` payload (32-byte receipt digest) | the execution receipt id |
| `effect_class` (a claim) | not trusted: effects are recomputed from the IR and checked against the grant |

## 8. Limits (not covered by v1)

- **Host CPU only.** A GPU tier is refused (`TIER_UNSUPPORTED`); `gpu_used=no` in every receipt.
- **TEST-class signature.** The checker2 receipt is signed with the TEST key. OWNER signing (the offline
  key) is not exercised. The execution receipt itself is unsigned text bound by its hash.
- **Grants are not authenticated here.** A signed grant from the runtime authority is future work; v1
  checks the grant's binding and records its digest.
- **Oracles** exist for three public dev tasks (`gcd_u32`, `collatz_steps_u32`, `checked_mul_i16`). A
  new task needs its own reference function in `vp_verify.c`.
- **No fresh model program yet.** The model generation lane (lane E) is not wired in; the model-written
  program in the test is a kept fixture (`tests/one_layer/omegese/trial2/dev08.omegese`).
- **No provisioned machine identity** on this host (`machine_id=absent`); the host is identified by CPU
  kind only.
