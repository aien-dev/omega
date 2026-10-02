# OSC import resolution (VC1 stage 4)

Authority: ADR 0029 and aien-protocols `specs/verified-crumb/SPEC.md` (sections 5 to 8). This file records what the C
implementation does and where it stops. Code: `src/omega_resolve.c` (resolver, lock, identity), `src/omega_receipt.c`
(receipt reader), `src/omega_blake3.c`, `src/omega_resolve_osc.c` (front end adapter), `src/oscv_main.c` (`oscv` driver).
Tests: `make test-resolve`.

## Grammar

```
unit   := import* ( struct | fn )*
import := "import" NAME ";"
```

Imports come first. `import` is now a keyword (it was a reserved word). More than 64 imports is CAPACITY, a repeated
name is REDEFINED_NAME, an import after a struct or fn is SYNTAX. `osc_compile` (and `oscc`) refuses a unit with
imports (UNSUPPORTED). Only `osc_compile_imports` with a resolver hook accepts one, and the hook is called once per
compile even with zero imports (so a lock line with no import is caught).

## Resolver flow (`omega_resolve_imports`)

1. Declaration check, before any store access: every import needs a lock line (DEPENDENCY_NOT_PINNED); every lock line
   needs an import (UNDECLARED_IMPORT). Direction chosen: an extra lock line is refused, not ignored.
2. Per node: lock line -> store get by semantic id -> identity (record id, optional blob digest, lock receipt equals
   record receipt) -> kind (BOOTSTRAP needs `allow_genesis`) -> taint capability -> verifier profile and version ->
   receipt check (SPEC 5.1 rules 1 to 6) -> edges (each dependency repeats the chain; contract ids must match; a node
   already on the path is DEPENDENCY_CYCLE).
3. The closure is sorted by semantic id; its digest is `SHA-256("OMEGA.CLOSURE.V1" 0x00 || u32be n || n x (semantic_id ||
   receipt_id || kind))`. Any refusal returns no closure.

Refusal codes: UNVERIFIED_DEPENDENCY, MISSING_RECEIPT, RECEIPT_HASH_MISMATCH, DEPENDENCY_NOT_PINNED, DEPENDENCY_CYCLE,
STALE_RECEIPT, UNDECLARED_IMPORT, TAINTED_ARTIFACT, UNKNOWN_VERIFIER_PROFILE, VERIFIER_TOO_OLD (enum in
`src/omega_resolve.h`).

## Build identity

`build_id = SHA-256("OSC1.BUILD.V1" 0x00 || ir_digest || domain byte || closure_digest)` in `omega_build_id`. That is
the one place the closure digest enters a build's identity. `oscv` prints it.

## Domains

`--domain build` (default) and `--domain dev` run the same checks; the domain never loosens one. omega-dev output
is TAINTED: its artifact header says `tainted 1`, its build id commits to the domain, and `omega_resolve_admit` refuses
to insert it (and refuses any record that lists the capability `omega-dev.taint`). A record listing that capability
can never satisfy an import in either domain.

BOOTSTRAP (genesis) records satisfy an import only with `allow_genesis`. That is a field inside the resolver library.
The `oscv` driver has no option, file or environment variable that sets it (the old `--allow-genesis` flag was removed), so a store
that holds only BOOTSTRAP records always yields UNVERIFIED_DEPENDENCY in both domains. The stage 6 genesis loader will be the
only caller that sets the field. A BOOTSTRAP record may NOT depend on a
VERIFIED record (the audited base must be closed over itself); a VERIFIED record may depend on BOOTSTRAP.

## No escape hatch

`oscv --blobs` is optional: leaving it out skips only the recheck of source digests against stored blobs.

No `unsafe import`, no skip switch, no environment variable. `make test-resolve` greps the compiler and resolver
sources for forbidden words and runs `oscv` with unknown switches and with environment variables set.

## Limits (stated, not hidden)

- Receipts are hashes, not signatures. The receipt reader recomputes the BLAKE3 id from the canonical bytes
  (verified against three receipts written by the Rust implementation) but does not replicate ledger or lease store
  bindings or chain walking of aien-proof.
- The program id is not recomputed from source; only the source digest is checked, and only when a blob store is given
  (stage 6 supplies it).
- A dependency cycle cannot be built through the store API; it is refused when a tampered store holds one.
- `cl_program.c` and `omega_discovery.c` are not migrated here (stage 6).
