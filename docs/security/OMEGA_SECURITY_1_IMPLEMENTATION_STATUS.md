# OMEGA SECURITY-1 implementation status

This branch contains a first executable envelope reference-view experiment.
It is intentionally **not** a SECURITY-1 acceptance receipt. The view derives
identity, access and bounds from existing Omega and resident runtime objects;
it creates no semantic object kind, authority representation, world, or
orchestrator.

## Implemented

- `RxExecutionEnvelope` references `OmegaProgram`, `RxReactionDesc`, the
  verification verdict and a dependency manifest. Program and realization IDs
  use the existing `SemanticId` and realization identity.
- Validation recomputes program and realization identity, verifies the
  dependency root, checks current generation, subject-bound capabilities,
  object read/write membership, effects against readable objects and existing
  `RX_RIGHT_EFFECT`, resource limits, lifetime and verification tier.
- Safety labels use the SECURITY-0 closed enum. `NativeUnsafe`,
  `PhysicalUnsafe` and `OpaqueFirmware` refuse. Managed and bytecode classes
  require at least V2. The implementation cannot add capability rights.
- Dependency manifests have a deterministic SHA-256 root over sorted artifact
  identities, content digests, schema versions and provenance references.
- A separate `exec` child demonstrates typed input/computation/output and
  malformed response/crash refusal. It receives no Omega data pointers or
  application descriptors beyond its protocol pipes.
- Envelope refusal evidence identifies the program, realization, subject,
  world generation, capability reference, failed invariant, effect object and
  caller-provided causal episode. No secret values are recorded.

## Acceptance gate status

| Gate | Status | Evidence / missing work |
|---|---|---|
| `OMEGA_EXEC_ENVELOPE_IDENTITY_PASS` | Pass | Program/realization mismatch tests. |
| `OMEGA_EXEC_ENVELOPE_AUTHORITY_PASS` | Pass, partial matrix | Forged, wrong-subject, stale generation, revoked, expired and amplified-right references use the existing root. Delegation-chain attack is covered by the existing R3 suite, not an envelope-specific case. |
| `OMEGA_EXEC_ENVELOPE_WORLD_PASS` | Pass, partial | Active generation mismatch and stale read reference refuse. Runtime `run_one` does not yet invoke this envelope validator. |
| `OMEGA_EXEC_ENVELOPE_EFFECT_PASS` | Skipped | No typed effect broker operation or AEGIS resident reaction path is wired. |
| `OMEGA_EXEC_ENVELOPE_RESOURCE_PASS` | Pass, partial | Memory/work and lifetime checks are present; the work estimate uses canonical instruction count. |
| `OMEGA_EXEC_ENVELOPE_FOREIGN_PASS` | Skipped | Process protocol cases pass, but no seccomp, namespace, filesystem or network containment exists; undeclared effects and world publication are not connected to a broker boundary. |
| `OMEGA_EXEC_ENVELOPE_SECRETLESS_PASS` | Skipped | No privileged broker-side credential operation was implemented. |
| `OMEGA_EXEC_ENVELOPE_EVIDENCE_PASS` | Pass, partial | Structured refusal evidence is tested for identity/world failures; complete causal integration with resident crumbs remains absent. |
| `OMEGA_EXEC_ENVELOPE_PERF_RECORDED` | Partial | Host microbenchmark prints validator ns/call and `sizeof(RxExecutionEnvelope)`. Allocation count, capability-only cost, effect cost, resident activation delta and steady-state delta are not measured. |

## Non-claims

This branch does not prove kernel-enforced Python isolation, complete CVE
management, symbolic verification, proof-carrying execution, supply-chain
security, distributed containment or hardware capability enforcement. V3–V5
verification remain stubs. The child has ordinary host OS access and is only a
process/address-space separation demonstration.

The next milestone is to wire the derived validator at existing resident
reaction pre-run and publication checks, then implement a typed AEGIS-authorized
effect request and broker-side mock credential operation before claiming the
vertical slice passes.
