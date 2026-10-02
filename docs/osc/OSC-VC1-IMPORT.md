# OSC import resolution (VC1 stages 4 and 6)

Authority: ADR 0029 and aien-protocols `specs/verified-crumb/SPEC.md` (sections 5 to 8). This file records what the C
implementation does and where it stops. Code: `src/omega_resolve.c` (resolver, lock, identity, the admission door),
`src/omega_receipt.c` (receipt reader), `src/omega_blake3.c`, `src/omega_genesis.h` (the pinned Genesis Set),
`src/omega_program_ir.c` (the IR the program id is recomputed from), `src/omega_resolve_osc.c` (front end adapter),
`src/oscv_main.c` (`oscv` driver), `src/omega_vc_bridge.c` (how Crumbline and discovery admit programs).
Tests: `make test-resolve test-resolve-genesis test-vcstore test-vcstore-genesis test-genesis test-genesis-real
test-program-ir`.

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
2. Per node: lock line -> store get by semantic id -> identity (the record names the same semantic id) ->
   source/IR recheck (below) -> the lock's receipt equals the record's receipt -> taint capability -> verifier profile and version -> receipt check (SPEC 5.1 rules 1 to
   6) -> edges (each dependency repeats the chain; contract ids must match; a node already on the path is
   DEPENDENCY_CYCLE) -> genesis gate for a BOOTSTRAP record (below, checked last so cycle and dependency refusals
   still come first).
3. The closure is sorted by semantic id; its digest is `SHA-256("OMEGA.CLOSURE.V1" 0x00 || u32be n || n x (semantic_id ||
   receipt_id || kind))`. Any refusal returns no closure.

Refusal codes: UNVERIFIED_DEPENDENCY, MISSING_RECEIPT, RECEIPT_HASH_MISMATCH, DEPENDENCY_NOT_PINNED, DEPENDENCY_CYCLE,
STALE_RECEIPT, UNDECLARED_IMPORT, TAINTED_ARTIFACT, UNKNOWN_VERIFIER_PROFILE, VERIFIER_TOO_OLD (enum in
`src/omega_resolve.h`). A BOOTSTRAP record that is not on the Genesis Set comes back as UNVERIFIED_DEPENDENCY with the
store code GENESIS_NOT_LISTED.

## Mandatory source/IR recheck (SPEC 6 step 3)

In the build domain the resolver must be given a source/IR blob store (`fetch_blob`; `oscv` reads `blobs/` next to the
source, or `--blobs DIR`). Without one, every node is refused ("needs the source or IR store"). For each record:

1. The blob named by `source_or_ir_digest` is fetched and SHA-256 of its bytes must equal that digest.
2. A record whose digest kind is OSC source is refused in the build domain: a source digest cannot be turned back into
   a program id by this resolver, so only an IR digest can satisfy the recheck.
3. For an IR digest, the program id is RECOMPUTED from the IR bytes (`omega_program_ir_recompute_id`, which decodes the
   IR exactly and calls `omega_program_compute_id`) and compared with the record's semantic id. A mismatch, or bytes
   that are not a valid IR, are UNVERIFIED_DEPENDENCY ("does not recompute to its semantic id").

The IR layout is in `src/omega_program_ir.h`. It carries exactly the fields the program id binds, so recomputing it is
possible from the blob alone.

The dev domain may leave the store out. Then none of the three steps runs and the output stays tainted (as every dev
output is). If a store is given, the dev domain runs steps 1 and 3 too; step 2 (the source kind refusal) is a build
domain rule only.

## Build identity

`build_id = SHA-256("OSC1.BUILD.V1" 0x00 || ir_digest || domain byte || closure_digest)` in `omega_build_id`. That is
the one place the closure digest enters a build's identity. `oscv` prints it.

## Domains

`--domain build` (default) and `--domain dev` run the same checks except the blob store requirement above; the domain
never loosens any other one. omega-dev output is TAINTED: its artifact header says `tainted 1`, its build id commits to
the domain, and `omega_resolve_admit` refuses to insert it (and refuses any record that lists the capability
`omega-dev.taint`). A record listing that capability can never satisfy an import in either domain.

## The artifact header is unauthenticated; the receipt is the gate

The text header `omega_artifact_meta_text` writes (domain, tainted mark, IR digest, closure digest, build id) is
integrity checked only against itself: `omega_artifact_meta_parse` recomputes the build id and checks the taint mark
agrees with the domain. Anyone who can write the file can rewrite the domain and the mark and recompute the build id,
and the parse will accept it. Nothing in the store or the resolver trusts that header to GRANT anything. What gates a
record into the store is the receipt (SPEC 5.1), checked by `omega_resolve_admit`; the `origin` argument of admit is
optional and only ever makes admission stricter (a tainted origin refuses). So the header is a label and a tripwire,
not a credential. If it is ever made to gate something, it must be signed or bound to the receipt first; until then it
must not be read as proof of the domain it names. (ADR 0029 carry item 6: documented, not changed.)

## Genesis (BOOTSTRAP) records

A BOOTSTRAP record is trusted without a receipt, so which ids may be BOOTSTRAP is a pinned list in source:
`src/omega_genesis.h`, set name VC-GENESIS-1, described and audited in `docs/osc/VC-GENESIS-1.md`. It is currently
EMPTY (the audit found no program that must exist without a receipt). The list is the only input:

- the store refuses to insert or to load a BOOTSTRAP record whose id is not listed (GENESIS_NOT_LISTED);
- `omega_resolve_admit_genesis` refuses an unlisted id and is the only code that may create a BOOTSTRAP record;
- the resolver refuses a BOOTSTRAP record that is not listed, whatever put it in the store.

`allow_genesis` no longer exists in `OmegaResolver`, `oscv` has no genesis option, and nothing is read from a file, a flag
or the environment. A BOOTSTRAP record may NOT depend on a VERIFIED record (the audited base must be closed over itself);
a VERIFIED record may depend on BOOTSTRAP.

### The two raw insert functions are private

`omega_vcstore_insert` and `omega_vcstore_insert_bootstrap` are declared only in `src/omega_vcstore_priv.h`. A record
enters a store only through `omega_resolve_admit` (receipt checked) or `omega_resolve_admit_genesis` (list checked).
`make test-resolve` scans `src/` and `tools/` and fails if any file other than the store and the resolver names either
function or includes that header, or if anything names the admit door, the list or a genesis switch outside its owners.

### Crumbline and discovery

`src/crumbline/cl_program.c` and `src/omega_discovery.c` no longer call `omega_library_insert` with a bare hash. Both go
through `src/omega_vc_bridge.c`, which builds the program's canonical Verified Crumb, mints an evidence receipt of kind
`host-v1`, tier HOST_TEST, and lets `omega_resolve_admit` check it before the library entry is made (ADR 0029 Decision
11). The receipt is SELF-MINTED in the same process that just ran `omega_program_verify`: it proves the record is well
formed and accepted by the resolver chain, not that an independent verifier re-ran anything. The test
`library-insert-in-src-is-named-only-by-the-library-and-the-bridge` keeps new direct inserts out of `src/`.

## The compiler hook cannot be swapped for another

`osc_compile_imports` takes any function pointer, so the compiler cannot enforce what is installed. The check lives in
CI: `import-hook-is-installed-only-by-the-real-driver` fails if any file in `src/` or `tools/` other than the compiler
front end and `oscv` calls it, and `the-real-driver-installs-the-real-resolver-hook` fails unless `oscv` installs
`omega_osc_import_hook`. `src/compiler` itself is unchanged.

## No escape hatch

`oscv --blobs` points the resolver at a source/IR store. Leaving it out is allowed only in the dev domain, where the
output is tainted; in the build domain the default `blobs/` is used and a missing store is a refusal.

No `unsafe import`, no skip switch, no environment variable. `make test-resolve` greps the compiler and resolver
sources for forbidden words and runs `oscv` with unknown switches and with environment variables set.

## Limits (stated, not hidden)

- Receipts are hashes, not signatures. The receipt reader recomputes the BLAKE3 id from the canonical bytes
  (verified against three receipts written by the Rust implementation) but does not replicate ledger or lease store
  bindings or chain walking of aien-proof. The Rust cross-check for the lease, ledger and mutation cases the three
  fixtures do not cover is not in this stage.
- UNDECLARED_IMPORT still means "lock line the source never imports" here. Reconciling that with SPEC 6 step 8 and
  aien-closure, and implementing step 8 proper (the record's stored source imports against `dependencies[]`), is not in
  this stage.
- A source digest cannot satisfy the build domain (it cannot be recomputed to a program id here), so a record kept only
  as OSC source needs an IR record beside it.
- The bridge receipt is self-minted HOST_TEST evidence (see above), not an independent verification.
- `omega_library_insert_bootstrap` still exists on the in-memory library and is not tied to the Genesis Set. No code calls
  it; it is not a way into the Verified Crumb Store.
- A dependency cycle cannot be built through the store API; it is refused when a tampered store holds one.
