# SECURITY-2 V3 implementation status

## Result

**Qualification status: FAIL / incomplete.** The V3 result model now fails
closed on required skips, but this tree does not yet have a brokered effect
executor or a kernel containment supervisor. Accordingly the SECURITY-2
acceptance demonstration cannot be claimed. The current forked child is a
process/address-space separation test, not a sandbox; the existing deterministic
child-crash case does not demonstrate successful arbitrary hostile behavior.

## Implemented in this slice

- `OmegaV3Evidence`/`OmegaV3Report` carry program and realization identities,
  envelope identity, World generation, attack class/input digest, expected and
  observed invariant, attempted authority/effect, containment boundary, verdict
  and evidence root. The reducer hashes fields in a fixed canonical order and
  cannot pass if required attacks were skipped.
- The old `omega_verify_pipeline(..., V3, ...)` now refuses. It has no envelope,
  World or attack-suite parameters and cannot legitimately qualify V3.
- `RxEffectIdentityInput` defines a domain-separated SHA-256 effect identity
  over subject, capability slot/generation, operation class/code, target,
  input digest, World generation, authorization context and idempotency key.

The effect identity is an API only. No production executor consumes it yet,
and the persistent generation replay ledger still stores a 64-bit work id.
Consequently exact-once irreversible effect execution is **not closed**. The
new identity reduces semantic ambiguity but must not be represented as replay
protection until the broker persists and atomically claims the full identity.

## Attack corpus disposition

| Family | Current coverage / qualification disposition |
|---|---|
| Authority | SECURITY-1 reference checks cover forged, stale, revoked, expired, wrong-subject and amplified rights; existing authority tests cover delegation and slot cases. Not yet assembled as V3 evidence against a single compromised target. |
| World | SECURITY-1 envelope validates generation and sets; runtime tests exercise publish gates. Canonical direct mutation, stimulus duplication and cross-generation suite remain unqualified. |
| Effect | Visor request remains unauthorized; AEGIS/capability checks exist. No broker effect commit path. Replay, tamper and revoke-at-commit suite is required and unavailable. |
| Supply chain | Dependency manifest root is validated by SECURITY-1. Substitution/truncation/extension tests exist only partially; synthetic vulnerability reachability is not implemented. |
| Secret | No credential broker exists. Credential reads, environment/proc scraping and secret-in-evidence protections are unimplemented, not passing. |
| Foreign runtime | Existing child handles typed IPC, malformed reply and deterministic child exit. No timeout/resource limits, seccomp, namespace, filesystem or network isolation. Those attacks are not claimed contained. |
| Python | Python runtime/workload is absent. Host/kernel protections are not implemented. |

The test-only foreign child is not modified to include a production defect.
The available crash opcode is deterministic process termination, not the
requested assume-compromised hostile payload. A compromised-workload
demonstration remains a required SECURITY-2 gate.

## CVE and containment gaps

No local `VulnerabilityRecord` reachability evaluator or resident typed
`ContainmentRequest` reaction is implemented in this slice. Existing AEGIS
policy/root-install is the authority path; detection must submit a bounded
request there and must not revoke directly. The graph currently lacks a
package-artifact-to-selected-realization-to-live-reaction index, so reachability
must report those missing edges rather than infer them.

## Gates run

- `make test-omega-v3-interface`: PASS. Confirms effect identity changes when
  request context changes and required skips force a non-pass.
- This is an interface/unit gate, not a V3 qualification run.

The receipt at `evidence/security/v3-qualification.json` records the failed
qualification and required skips. A successful SECURITY-2 receipt requires the
broker, attack executor, evidence integration and containment reaction to run
the complete matrix against a deliberately compromised test subject.

## Recommended SECURITY-3 scope

First complete SECURITY-2's missing vertical slice: durable full-width
EffectId claim/commit at the effect executor; bounded compromised-child IPC
operations for secret, authority, World and effect attempts; resident finding
to typed containment request to AEGIS decision; then run and receipt every
required case. Keep online CVE lookup, V4/V5, and broader isolation out of that
scope until this local deterministic gate is green.
