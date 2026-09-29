# Omega Visor V1: authority and effect safety (lane 7)

**Invariant: the Visor can ask. The Visor cannot grant.**

The Visor may describe an effect and turn it into a request. It cannot mint,
revoke, promote, publish or execute, and a request it builds is never
authorized. The only things that may act on a request are the existing
authority code paths outside the Visor, listed below.

## 1. The existing authority path (mapped, not changed)

| Stage | File | Function(s) | What it decides |
|---|---|---|---|
| Capability root | `src/runtime/rx_caproot.h/.c` | `rx_caproot_validate`, `rx_caproot_inspect` | Is `(cap_id, generation)` live, current epoch, unexpired, chain intact, for this subject, resource and rights? (`RX_CAP_ERR_BOUNDS/STALE_GEN/REVOKED/EPOCH/SUBJECT/RESOURCE/RIGHTS/...`) |
| Root administration | same | `rx_capadmin_mint/revoke/reclaim/advance_clock/bump_epoch` | Only the admin channel holder with the right token and an office authority may change the table (`RX_CAP_ERR_UNAUTHORIZED` otherwise). Reclaim advances the slot generation. |
| World check | `src/runtime/rx_world.h`, `rx_world.c` | `rx_world_validate_cap`, `rx_world_inspect_cap` | Same question, routed to the Linux oracle or the native AIENOS view (`rx_world_init_with_auth`). |
| World publish | same | `rx_world_publish_external` | An outside stimulus needs a cap for `(external_subject, object resource, WRITE)` and a live object ref (`RX_ERR_AUTHORITY`, `RX_ERR_STALE_GEN`). |
| AEGIS policy | `src/runtime/rx_aegis.h/.c` | `rx_aegis_evaluate` (pure), `aegis.decide.k` / `root.install.k` reactions | GRANT / DENY (`WHY_NO_RULE`, `WHY_PRIVILEGED`, `WHY_BAD_REQUEST`) / ESCALATE. Only `root.install` mints (through the native AIENOS admin). Nothing calls AEGIS; a reaction is simply not ready until its slot holds authority. |
| Generation barrier | `src/runtime/rx_generation.h/.c` | `rx_gen_propose`, `rx_gen_add_work`, `rx_gen_promote`, `rx_gen_reject_replay` | Promotion needs `RX_RIGHT_PROMOTE` on `RX_GEN_RES_PROMOTION`; the promoted root records committed external effect ids, and `rx_gen_reject_replay` refuses a recorded id (`RX_GEN_ERR_REPLAY`). |
| Design references | `spec/r7-aienos-authority-mapping.md`, `spec/r8-aegis-resident.md` | | Host authority onto the AIENOS table; AEGIS resident authority and its attack list. |

Route of a legitimate effect:
`Visor -> EffectIntent -> AEGIS/PHYSICS authority -> bounded execution -> receipt`.
The Visor owns only the first arrow.

## 2. Adapter contract (`src/visor/visor_effect_request.h/.c`)

```c
typedef struct { SemanticId effect_id; uint16_t resource_class; uint16_t operation_code;
  uint32_t capability_slot; uint64_t capability_generation; SemanticId capability_ref;
  uint16_t param_len; uint8_t param_bytes[128]; uint8_t request_digest[32];
  bool authorized; char status[64]; char route[128]; } VisorEffectRequest;
int visor_effect_request_build(const OmegaGraph *g, const SemanticId *effect_object_id, VisorEffectRequest *out);
int visor_effect_request_classify(const OmegaGraph *g, const SemanticId *id, bool *requires_authority);
int visor_effect_request_format_text(const VisorEffectRequest *r, char *out, size_t n);
int visor_effect_request_format_json(const VisorEffectRequest *r, char *out, size_t n);
```

- **build** reads only an existing `KIND_EFFECT` object. Refuses (-1, output
  zeroed and unauthorized) when the object is missing or not an effect,
  `omega_validate_object` rejects it, the payload is not a strict 178-byte
  v2 effect payload (`omega_effect_read`),
  `param_len > 128`, it has no id, or `sha256(omega_canonical_encode(obj))`
  differs from its stored SemanticId (it was edited after being named).
  It computes the digest into a local buffer and never writes the graph.
- `request_digest` = sha256 over the canonical object encoding = the
  object's SemanticId when the object is intact.
- `authorized` is always false, `status` = `UNAUTHORIZED_REQUEST`, `route`
  as above. There is no setter. The formatters print `authorized: false` as a
  constant, whatever the struct holds.
- **classify** walks relations and payload references (type `elem_type`,
  value `type_id`, operation types, apply `op_id`/operands, effect
  `capability_ref`) from `id`. True when it reaches a `KIND_EFFECT` or a TYPE
  tagged `CAPABILITY_REF / EFFECT_INTENT_REF / EFFECT_RECEIPT_REF`. Returns
  -1 (and leaves `*requires_authority = true`) when `id` or any non-zero
  reference is missing: fail closed.
- The capability fields are copied as data. The Visor never validates,
  inspects, resolves or holds a capability.
- Physics-free and runtime-free: depends only on `omega_core` (lookup),
  `omega_canonical`, `omega_validate`, `sha256`.

## 3. Hostile test matrix (`tests/visor/test_visor_authority.c`, `make test-visor-authority`)

The harness plays the authority side (it mints, revokes, promotes); the
hostile UI builds requests with the adapter. Every refusal has a positive
control through the same call.

Test conventions, not runtime semantics: resource = `0x7100 + resource_class`;
rights: op 1 READ, 2 WRITE, 3 EFFECT, 16 MINT; generation-record effect id =
first 8 bytes (little endian) of `request_digest`.

| # | Attack | Refused by (existing code) | Result |
|---|---|---|---|
| 1 | Forged cap: random slot | `rx_caproot_validate`, `rx_world_validate_cap` | `BOUNDS` |
| 1 | Forged cap: right slot, random generation | `rx_caproot_validate` | `STALE_GEN` |
| 1 | Wrong slot (another subject's live cap) | `rx_caproot_validate`, `rx_world_validate_cap` | `SUBJECT` |
| 1 | Hostile caller sets `authorized = true` | nothing reads it; root still refuses; formatter prints false | refused |
| 2 | Stale generation (revoke, then reclaim) | `rx_caproot_validate`, `rx_world_validate_cap` | `REVOKED`, then `STALE_GEN` |
| 3 | Replayed authorization after generation advance | `rx_caproot_validate` (new grant, old ref) and `rx_gen_reject_replay` | `STALE_GEN`, `RX_GEN_ERR_REPLAY` |
| 4 | UI direct privileged call | `rx_aegis_evaluate` (`WHY_PRIVILEGED`), `rx_caproot_validate` (`RIGHTS`), `rx_capadmin_mint` under a non-office authority / forged token (`UNAUTHORIZED`) / no channel (`IO`); link check (sec. 4) | refused |
| 5 | Malformed request (short / oversized payload, `param_len` > 128, resource 0) | `omega_validate_object` + `visor_effect_request_build` | -1 |
| 6 | Operation outside the grant | `rx_caproot_validate` (`RIGHTS`), `rx_aegis_evaluate` (`DENY`, `WHY_NO_RULE`) | refused |
| 6 | Resource outside the client's policy | `rx_aegis_evaluate` | `DENY`, `WHY_NO_RULE` |
| 7 | Resource changed after authorization | `visor_effect_request_build` digest check (-1); re-identified object has a different digest and `rx_caproot_validate` / `rx_world_validate_cap` return `RESOURCE` | refused |
| 8 | Arguments changed after authorization | digest check (-1); re-identified digest differs; changed operation -> `RIGHTS` | params-only: see non-claims |
| 9 | Duplicate submission | `rx_gen_reject_replay` after `rx_gen_promote` recorded the effect | `RX_GEN_ERR_REPLAY` |
| 10 | Bypass publish: stale / junk / read-only / other subject's cap, stale object | `rx_world_publish_external` | `RX_ERR_AUTHORITY` / `RX_ERR_STALE_GEN` |
| 11 | Bypass AEGIS: present a slot nobody granted | `rx_caproot_validate`, `rx_world_validate_cap`; `rx_aegis_evaluate` with no policy -> `DENY` | refused |
| 0 | Contract | build/classify never mutate; `authorized` false; formats deterministic; truncation -1; classify fails closed on missing refs | |

Result on the DGX Spark host (2026-09-28, start commit ceb68d6): `PASS 81/81`,
`OMEGA_VISOR_AUTHORITY_HOSTILE_PASS`, 4 non-claims printed.
After merging main (2026-09-29, 64-bit capability generations): `PASS 84/84`
real checks, 20 non-claims (see §5 item 7), `OMEGA_VISOR_AUTHORITY_HOSTILE_PASS`.

## 4. Link check (`tests/visor/check_authority_link.sh`, `make visor-authority-check`)

Runs `nm` over `build/visor/*.o`, `build/language/*.o`, `build/omega_main.o`
and `build/omega` (missing files are skipped with a note; an empty
`build/visor` fails). It fails when a file:

- references (undefined) any symbol in `tests/visor/forbidden_symbols.txt`;
- references any symbol matching `^rx_`, `^aienos_cap_` or `^rc_` (the omega
  binary links no runtime at all);
- defines a forbidden name (a local reimplementation).

It also requires `visor_effect_request.o` to export exactly its four public
functions, and fails if any Visor source (`src/visor`, `src/language`,
`tools/omega.c`) assigns `authorized` anything other than `false`. Success
prints `OMEGA_VISOR_AUTHORITY_ISOLATION_PASS`.

"No authorized setter" is enforced by that export allowlist and source grep,
and in the test by file-scope definitions of `visor_effect_request_authorize`,
`_grant`, `_submit` and `_set_authorized` that conflict with any declaration
of those names in the header. C cannot stop a caller writing the field; the
design point is that nothing trusts it.

## 5. Explicit non-claims

1. The capability root binds subject, resource, rights, generation, epoch
   and lease, not effect parameters. A params-only change validates at
   `rx_caproot_validate` (observed in the test); only the request digest
   differs. No existing runtime call compares effect digests.
2. The World does not deduplicate external stimuli (observed: the same
   publish succeeds twice). Duplicate refusal exists only in the generation
   record of committed external effects (`rx_gen_reject_replay`).
3. The typed-result publish gate (`rc_gate_register`, a resident reaction) is
   not driven; only the World's external publish check is.
4. AEGIS is not on a call path. The resident `aegis.decide` / `root.install`
   reactions are not run; `rx_aegis_evaluate` is exercised as the pure policy
   function. The native AIENOS authority is linked (for `rx_aegis.c`) but
   validation in this test uses the Linux oracle (`rx_caproot_validate`).
5. The operation-code-to-rights and resource-class-to-resource mappings are
   test conventions. Omega's `EffectPayload` has no canonical mapping to
   runtime rights yet.
6. No effect is executed and no receipt is produced by anything in the Visor.
7. **Resolved by spec/effect-cap64-migration.md (was: honest 64-bit
   generations unrepresentable in the effect format).** Runtime capability
   generations are 64-bit and seeded from `CLOCK_BOOTTIME` ns << 8, so they
   exceed 2^32. Omega's `EffectPayload` / `VisorEffectRequest` used to carry a
   32-bit generation, so every honest reference carried through the Visor was
   truncated and refused as `STALE_GEN`, and the Visor-path checks printed a
   NON-CLAIM instead of being counted (before the fix: `PASS 84/84`, 20
   non-claims). Effect objects are now version 0x02 with a 178-byte
   big-endian payload carrying the full 64-bit generation; the GEN_FITS gating
   is removed, every Visor-path check is counted, and case 12 asserts the full
   generation is carried and validates, and that the same reference cut to 32
   bits is `STALE_GEN`.
