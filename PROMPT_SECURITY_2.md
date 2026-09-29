# OMEGA SECURITY-2 — V3 ADVERSARIAL VERIFICATION / CVE RESILIENCE

Use SECURITY-0 and SECURITY-1 as authoritative.

Your task is to turn Omega's V3 adversarial verification tier from a placeholder into a real qualification mechanism.

Do not implement V4 symbolic verification or V5 proof-carrying verification yet except for interfaces required so V3 will compose cleanly with them later.

The security oracle for V3 is NOT:

```text
"did the process crash?"
```

The security oracle is:

```text
"did unauthorized authority or an unauthorized effect escape?"
```

## PRIMARY INVARIANT

For every adversarial test:

```text
exploit may succeed locally

but

unauthorized capability acquisition = 0
unauthorized World mutation = 0
unauthorized effect commit = 0
unauthorized secret disclosure = 0
cross-generation stale authority acceptance = 0
```

A local crash or arbitrary-code execution in a deliberately vulnerable foreign workload is acceptable if containment holds.

That distinction is the purpose of this milestone.

## PART A — IMPLEMENT V3

Replace the current V3 adversarial stub with an actual verifier interface and result model.

V3 must produce structured evidence including:

```text
target ProgramId
target RealizationId
ExecutionEnvelope identity
World generation
attack class
attack input digest
expected invariant
observed result
authority attempted
effect attempted
containment boundary
verdict
evidence root
```

A V3 PASS means the declared adversarial suite executed and every required containment invariant held.

It must not mean "tests were unavailable."

## PART B — DELIBERATELY VULNERABLE TEST SUBJECT

Create a test-only vulnerable foreign component.

It should contain a deterministic exploitable defect sufficient to simulate compromise or arbitrary attacker-controlled behavior.

Do NOT introduce the vulnerability into production code.

The purpose is to demonstrate:

```text
successful local compromise
        ↓
failed privilege escalation
        ↓
failed unauthorized effect
        ↓
evidence preserved
```

If a real memory-corruption RCE is too platform-specific for deterministic CI, use a controlled "assume-compromised" payload that deliberately performs every forbidden operation from inside the untrusted boundary.

The test must be honest about which form is used.

## PART C — ATTACK MATRIX

V3 must cover at minimum:

### Authority attacks

```text
forge CapabilityRef
reuse stale generation
use revoked capability
use expired lease
borrow another subject's capability
attempt delegation without DERIVE
attempt rights amplification
attempt slot stuffing / reference substitution
```

### World attacks

```text
publish outside declared write-set
publish against stale World inputs
mutate canonical state directly
replay prior publication
duplicate external stimulus
cross-generation replay
```

### Effect attacks

```text
undeclared effect
tampered effect payload
replay same effect
same authority + same effect twice
stale-generation effect
changed payload with reused authorization
confused-deputy request
effect after capability revocation
```

### Dependency / supply-chain attacks

```text
dependency digest mismatch
artifact substitution
manifest truncation
manifest extension after signing
wrong provenance reference
known-vulnerable dependency flag
```

Do not yet require online CVE lookup.

Use a deterministic synthetic vulnerability record where needed.

### Secret attacks

```text
read broker credential
environment scraping
proc-like metadata scraping if exposed
request credential rather than operation
force error path that logs secret
cause secret to enter evidence
```

### Foreign runtime attacks

```text
malformed IPC
oversized message
invalid UTF-8 / binary where relevant
child process crash
child hang
resource exhaustion
attempted fork/process fan-out
attempted undeclared filesystem access
attempted undeclared network access
```

Only test mechanisms that actually exist.

Do not fabricate PASSes for isolation not yet implemented.

## PART D — EFFECT IDENTITY / REPLAY CLOSURE

Audit the current effect path carefully.

The current architecture must not allow:

```text
valid authorization
+
replayed request
=
effect executed twice
```

Design and implement a canonical EffectId or equivalent identity mechanism if the current implementation lacks one.

It should bind enough data to prevent semantic replay ambiguity.

Consider:

```text
subject
capability identity/generation
semantic operation
target
input digest
World generation
policy/authorization context
idempotency identity
```

Do not blindly copy this list.

Derive the minimal correct canonical set from the actual code.

Add tests for:

```text
exact replay
payload mutation
generation mutation
capability mutation
subject mutation
same request after revocation
same request after World advance
```

External irreversible effects must not execute twice through replay.

## PART E — CVE REACHABILITY PROTOTYPE

Implement a deterministic local vulnerability record:

```text
VulnerabilityRecord {
    vuln_id
    affected_artifact
    affected_range_or_digest
    severity_metadata
    exploitation_state
    evidence_ref
}
```

Then produce a reachability analysis over the currently available semantic data.

At minimum distinguish:

```text
artifact present
artifact selected by realization
realization live
attacker-controlled input reaches it
capability reachable
effect reachable
```

The output must be explanatory, not merely a scalar score.

If required graph information does not exist yet, report the gap rather than inventing it.

## PART F — CONTAINMENT REACTION

Add the smallest resident containment reaction that consumes a vulnerability/security finding and produces a typed containment request.

Correct direction:

```text
finding
    ↓
ARGUS/security reaction
    ↓
ContainmentRequest
    ↓
AEGIS authority evaluation
    ↓
authorized contraction/revocation
```

Incorrect direction:

```text
scanner finds CVE
    ↓
scanner revokes arbitrary capabilities
```

The detection subsystem must not acquire authority merely because it detected something.

## PART G — PYTHON SECURITY QUALIFICATION

If Python support is present or easy to add without distorting architecture, create a test workload representing a hostile Python process.

The Python process should attempt:

```text
read unauthorized object
write unauthorized object
obtain credential bytes
open arbitrary network destination
open undeclared file
mint capability
reuse stale capability
issue undeclared effect
tamper with evidence
```

Record which protections are:

```text
Omega semantic
AEGIS capability
AIENOS/kernel
host compatibility sandbox
not yet implemented
```

Do not claim Python sandbox security based only on Python-level restrictions.

If the kernel boundary is not available yet, state that clearly.

## PART H — SECURITY EVIDENCE RECEIPT

Create a canonical V3 qualification receipt.

It must bind:

```text
candidate commit
dependency commits
machine identity
test corpus digest
attack-suite digest
ExecutionEnvelope schema/version
authority ABI version
World/generation identity
number of attacks
number contained
number failed
required skips
performance overhead
evidence root
```

A skipped required attack is a failed qualification unless the spec explicitly marks it optional.

## PERFORMANCE

V3 qualification itself may be expensive.

Normal production execution must not pay V3's full cost.

Measure separately:

```text
normal runtime security fast path
V3 qualification cost
containment reaction cost
effect replay-check cost
vulnerability reachability analysis cost
```

Do not put symbolic/fuzzing/scanner work in the ordinary hot path.

## REQUIRED FINAL DEMONSTRATION

The milestone does not pass until you can demonstrate:

```text
deliberately compromised workload
        ↓
attacker executes arbitrary hostile behavior inside boundary
        ↓
tries to read secret
        DENIED
        ↓
tries forged/stale authority
        DENIED
        ↓
tries undeclared World write
        DENIED
        ↓
tries irreversible effect
        DENIED
        ↓
tries replay
        DENIED
        ↓
ARGUS/security evidence records incident
        ↓
containment request issued
        ↓
AEGIS authorizes only bounded containment
        ↓
healthy work continues
```

That is the core security proof.

## STOP CONDITION

Do not proceed automatically into:

```text
V4 symbolic verification
V5 proof-carrying verification
full SBOM service
online OSV/NVD integration
automatic patch synthesis
Evolution Arena security mutation
distributed Fabric security
CHERI backend
```

Those become later milestones.

Return:

```text
branch
commits
V3 interface changes
effect/replay changes
attack corpus
tests/gates
qualification receipt
local-compromise demonstration
authority-escape results
performance measurements
remaining attack surface
recommended SECURITY-3 scope
```
