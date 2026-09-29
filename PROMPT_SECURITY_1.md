# OMEGA SECURITY-1 — EXECUTION ENVELOPE VERTICAL SLICE

Use the accepted SECURITY-0 ADR as authoritative.

Do not reopen settled architecture unless implementation exposes a concrete contradiction.

Your task is to build the smallest executable vertical slice proving that Omega can bind execution to:

```text
semantic program
+
realization
+
subject
+
World generation
+
authority
+
object access
+
effects
+
resource bounds
+
verification requirements
```

without creating another orchestration layer.

## CORE PROPERTY

The milestone must prove:

```text
code execution
    does not imply
authority acquisition
```

and:

```text
authority
    does not imply
arbitrary effect execution
```

## IMPLEMENTATION RULE

Prefer an ExecutionEnvelope as a canonical composition/reference over existing Omega semantic objects.

Avoid duplicating:

```text
Program identity
CapabilityRef
World object identity
Generation
Effect identity
Realization identity
resource accounting
verification state
```

If a new structure is required, it should contain stable IDs/references to those objects rather than private copies of their semantics.

## REQUIRED FIELDS / SEMANTICS

The implemented representation must support at minimum:

```text
program
realization
subject
World generation

read-set
write-set

required capabilities
effect set

memory budget
compute/work budget
expiration / lifetime

safety classification
verification requirement
dependency root
```

Use existing canonical integer widths and IDs.

Do not introduce a parallel authority encoding.

## FAIL-CLOSED VALIDATION

Before execution, validate:

```text
ProgramId matches canonical semantic body
RealizationId is valid for ProgramId
World generation is current/acceptable
subject matches capability subject
capability generation is current
capability not expired
capability not revoked
delegation chain valid
requested rights are contained
read-set allowed
write-set allowed
effects contained
resource envelope valid
required verification tier satisfied
dependencies match admitted identities
```

A failure must produce a typed refusal and evidence.

Never silently downgrade.

## RESIDENT-RUNTIME INTEGRATION

Integrate through the resident reaction architecture.

Do NOT create:

```text
execution manager thread
security polling loop
central dispatcher
run-until-complete security orchestrator
```

Security-relevant state changes should enter the World and wake dependent reactions through normal dependency mechanisms.

Execution must remain compatible with schedule independence.

## SAFETY CLASS

Implement the minimum safety-class representation accepted in SECURITY-0.

Safety class may tighten:

```text
verification requirements
isolation requirements
resource bounds
capability admissibility
```

It may never mint or enlarge a capability.

Add a regression proving this.

## FOREIGN EXECUTION ADAPTER

Add one minimal foreign-runtime adapter.

Prefer the smallest deterministic implementation available in the current repository.

This is NOT yet a full Python product.

Its purpose is to prove that an untrusted realization can:

```text
receive typed input
perform computation
return typed output
```

while failing to:

```text
gain an undeclared capability
write outside its object write-set
publish unauthorized World state
stage an undeclared external effect
reuse stale authority
```

If Python is used for the demonstration, the host security boundary must be explicit and the milestone must not claim kernel-grade isolation unless AIENOS actually provides it.

If Python would distort the milestone, use a deliberately hostile native child process and reserve Python for SECURITY-2.

Be precise about what is and is not isolated.

## SECRETLESS EFFECT PROTOTYPE

Implement one narrow end-to-end example where an untrusted realization requests an operation requiring privileged credential material but does not receive the credential itself.

A mock credential is acceptable only if the authority path is real.

Required shape:

```text
untrusted realization
    ↓
typed effect request
    ↓
AEGIS/capability validation
    ↓
privileged broker-side operation
    ↓
typed result
```

The workload's memory must never contain the credential bytes.

Add a negative test attempting to retrieve or infer the secret directly.

## DEPENDENCY ROOT

Introduce the minimum dependency identity mechanism necessary for later CVE reachability.

An execution envelope should be able to bind to a deterministic dependency root.

Do not build a package manager.

A simple content-addressed dependency manifest is enough for this milestone.

Example logical structure:

```text
DependencyManifest {
    artifacts[]
    each:
        identity
        digest
        version/schema
        provenance_ref
}
```

Canonicalize and hash it.

## REQUIRED ATTACK TESTS

Add deterministic tests for at least:

```text
forged capability
wrong subject
stale capability generation
revoked capability
expired lease
rights escalation
write outside write-set
read outside read-set
undeclared effect
wrong World generation
wrong ProgramId
wrong RealizationId
tampered dependency root
insufficient verification tier
resource budget exhaustion
foreign child crash
foreign child malformed output
secret retrieval attempt
```

All must fail closed.

Valid control cases must continue to succeed.

## EVIDENCE

Every refusal should preserve enough structured evidence to answer:

```text
what executed?
under which subject?
which World generation?
which authority was presented?
which invariant failed?
what effect was attempted?
what was denied?
what causal episode produced the attempt?
```

Do not dump secrets.

## PERFORMANCE

Measure:

```text
envelope validation latency
additional allocations
bytes of metadata
AEGIS/capability validation cost
effect validation cost
resident reaction activation overhead
steady-state overhead
```

This milestone does not need zero overhead.

It does need a baseline.

## ACCEPTANCE GATES

Create explicit gates conceptually equivalent to:

```text
OMEGA_EXEC_ENVELOPE_IDENTITY_PASS
OMEGA_EXEC_ENVELOPE_AUTHORITY_PASS
OMEGA_EXEC_ENVELOPE_WORLD_PASS
OMEGA_EXEC_ENVELOPE_EFFECT_PASS
OMEGA_EXEC_ENVELOPE_RESOURCE_PASS
OMEGA_EXEC_ENVELOPE_FOREIGN_PASS
OMEGA_EXEC_ENVELOPE_SECRETLESS_PASS
OMEGA_EXEC_ENVELOPE_EVIDENCE_PASS
OMEGA_EXEC_ENVELOPE_PERF_RECORDED
```

Do not mark the milestone complete if required gates are skipped.

## NON-CLAIMS

Explicitly state anything not yet proven, especially:

```text
kernel-enforced Python isolation
complete CVE management
symbolic verification
proof-carrying execution
supply-chain security
distributed containment
hardware capability enforcement
```

## RETURN

Return:

```text
branch
commits
owned files
new ABI/types
tests added
tests run
gate results
measured overhead
known limitations
spec deviations
next recommended milestone
```

Do not proceed into V3 adversarial security work until this vertical slice passes.
