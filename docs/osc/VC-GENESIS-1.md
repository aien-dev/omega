# VC-GENESIS-1: the Genesis Set

Authority: ADR 0029 Decision 8 (BOOTSTRAP admission) and Decision 11 (no direct library insert).
Code: `src/omega_genesis.h` (the pinned table, nothing else), enforced in `src/omega_vcstore.c` and
`src/omega_resolve.c`. Tests: `make test-genesis-real test-genesis test-vcstore test-resolve test-vc-bridge`.

Members: 0

## What the Genesis Set is

The Verified Crumb Store holds two kinds of record. A VERIFIED record is let in only together with a
receipt that the resolver checked (SPEC 5.1). A BOOTSTRAP record is let in with no receipt, because
something has to start the chain of trust, and it is trusted only because a person audited it by hand.
The Genesis Set is the complete list of semantic ids (Omega program ids) that are allowed to be
BOOTSTRAP records. It is the one start of trust that does not come from a receipt, so it is part of the
trusted computing base and is kept as small as the audit allows.

How it is enforced, and the only ways:

- `omega_vcstore_insert_bootstrap` refuses a record whose semantic id is not listed
  (`GENESIS_NOT_LISTED`).
- `omega_vcstore_load` refuses a store file that holds a BOOTSTRAP record whose id is not listed
  (`GENESIS_NOT_LISTED`), so a hand edited file cannot add a member.
- `omega_resolve_admit_genesis` (the only door a BOOTSTRAP record can use to enter a store) refuses an
  unlisted id before the store is touched.
- The resolver, when it walks an import, refuses any BOOTSTRAP record that is not listed, whatever put it
  in the store.
- The resolver has no genesis permission field. Membership of this list is the only input. There is no
  flag, no environment variable, no file and no store content that adds a member. The list is a
  compile time constant in `src/omega_genesis.h`.
- No wildcard and no prefix match: an id is a member only if all 32 bytes equal a row. The all-zero id is
  never a member.

Changing the set (adding or removing a member, or renaming it) is an ADR level change. Two tests pin it:
`tests/test_omega_genesis.c` fails until the table and this file name exactly the same members, and until
the count matches the number in the "Members:" line above.

## Members

None. VC-GENESIS-1 is empty.

| id (64 hex) | what it is | why it is trusted without a receipt | audit |
|---|---|---|---|
| (no rows) | | | |

## Audit

Question: which programs must exist in the store without a receipt so that Crumbline and discovery can
start? The audit looked at every place that creates or admits a program and at every place that asks for
a BOOTSTRAP record.

Method: read the code, do not assume. Searches run over `src/`, `tools/` and `tests/` for
`omega_library_insert_bootstrap`, `omega_vcstore_insert_bootstrap`, `omega_resolve_admit_genesis`,
`OMEGA_LIB_ADMISSION_BOOTSTRAP`, `OMEGA_VCS_ADMISSION_BOOTSTRAP` and `bootstrap` generally.

Findings:

1. `src/crumbline/cl_program.c` (the Crumbline learner) admits programs it has just synthesized and
   checked with `omega_program_verify`. Before stage 6 it called `omega_library_insert` with a bare hash.
   It never calls a bootstrap function. It starts with an EMPTY library and builds up. Stage 6 migrates it
   through `src/omega_vc_bridge.c`, which now runs `omega_program_verify` and recomputes the program id
   itself, and then writes a SELF-MINTED receipt (see the next paragraph). It needs no BOOTSTRAP record.
2. `src/omega_discovery.c` mines abstractions from a corpus of programs the caller already holds. It
   admitted a discovered abstraction with a bare hash. A discovered abstraction is a program with a
   verification result, so it too goes through the bridge. It needs no BOOTSTRAP record.
3. Nothing in `src/`, `tools/` or the test drivers calls `omega_library_insert_bootstrap`,
   `omega_vcstore_insert_bootstrap` (outside the store and the resolver) or `omega_resolve_admit_genesis`.
   The only users of the BOOTSTRAP kind before stage 6 were unit tests that exercised the kind itself.
4. The OSC import path (`oscv`) resolves imports from a store that someone else filled. It never creates
   a record. Its test store holds VERIFIED records with receipts.
5. The primitive operations and the type constructors that every program is built from are part of the C
   program (`omega_program.c`), not records in the store. A record exists only for a program that has a
   body, a contract and evidence. There is no record that the language needs in order to compile its
   first program.

WHY THE SET IS EMPTY, stated plainly: it is empty because the bridge supplies Crumbline and discovery
programs with a receipt that the bridge writes itself (kind host-v1, tier HOST_TEST). That receipt is
SELF-MINTED: the code asking for admission writes it, and no independent run (aien-test) backs it.
Without the bridge those programs would have needed either a real receipt, which does not exist yet, or
BOOTSTRAP status, which would have made them audited Genesis members. So the empty set is honest only
together with these two facts, both enforced by tests (`make test-vc-bridge`): the bridge verifies the
program and recomputes its id before it mints anything, and every record the bridge mints lists the
capability `omega-bridge.selfminted`, which the build domain refuses. A bridge record can sit in a
store and satisfy a dev import, but it can never satisfy `oscv --domain build`. Real aien-test receipts
for these programs are owed (ADR 0029 Decisions 5 and 11); until they exist, the store admission of
Crumbline and discovery programs is a dev-grade fact, not a build-grade one.

Decision: the minimum that bootstraps Crumbline and discovery is nothing. The set is EMPTY. No BOOTSTRAP
record is trusted without being listed, and a BOOTSTRAP record in any store is refused by every door.

What an empty set costs: the BOOTSTRAP admission path exists and is tested (with a one member variant
build, `make test-genesis`) but nothing uses it. The day a real member is needed it is added by an ADR
level change that edits `src/omega_genesis.h` and adds a row and an audit entry to this file; the test
refuses any other order of events.

Audit record: this section, dated 2026-10-02, stage 6 of VC1. The audit hash a future member would carry
in its `receipt_id` field is the SHA-256 of that member's audit entry text in this file.

## What is NOT claimed

- The empty set does not mean the learner's programs are independently qualified. The bridge is a named
  trust root: it mints self-attested HOST_TEST receipts that the build domain refuses. Removing the
  bridge from the store path, or replacing its receipts with real aien-test receipts, is the carry item
  that would make the claim stronger.
- The empty set does not make the library's own `omega_library_insert_bootstrap` safe: that function
  works on the in-memory library, not on the Verified Crumb Store, and no caller uses it. It is listed as
  an open item in OSC-VC1-IMPORT.md.
- Nothing here checks a signature. A BOOTSTRAP record would be trusted on the strength of a human audit
  of the program and the commit that added its id to the table.
