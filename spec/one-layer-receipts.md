# One-layer receipts v1: signed Omega realization receipts (normative)

Stage 3 of the one-layer plan (omega#446). Code: `src/checker2/` (receipts `c2_receipt.{h,c}`,
IR decoder `c2_ir.{h,c}`, acceptance rules `c2_check.c`, LRAT checker `c2_lrat.{h,c}`, bit-blaster
`c2_blast.{h,c}`, command line `checker2_main.c`). Tests: `make test-checker2` (section 12).
Pairs with aienos#383 (OSC unit admission), which must verify exactly these bytes.

## 1. What a receipt says, and what it does not

A receipt records that **checker2** (the independent checker in `src/checker2/`) judged one
exact set of bytes and accepted them, and that a key of a stated signer class vouches for that
record. The signature establishes provenance only. It never establishes correctness, and a
receipt grants no capability: aienos grants stay separate, revocable authority.

A receipt is minted only by `checker2 receipt issue`, which runs checker2 on the program bytes
itself and refuses to sign when checker2 rejects. Nothing else in Omega writes receipts.

## 2. Byte layout (416 bytes, little-endian, no padding, no text, no timestamps)

| offset | size | field |
|---:|---:|---|
| 0 | 8 | magic `O M R C P T 1 \0` |
| 8 | 2 | format version, = 1 |
| 10 | 2 | signer class: 1 TEST, 2 OWNER (same numbers as aienos `OSC_SIGNER_TEST`/`OSC_SIGNER_OWNER`) |
| 12 | 2 | signature algorithm, = 1 (pure Ed25519, RFC 8032 section 5.1) |
| 14 | 2 | evidence class (section 4) |
| 16 | 32 | requirements: SHA-256 of the requirements object bytes (all zero: none supplied) |
| 48 | 32 | program: SHA-256 of the canonical program bytes (OSC IR; equals `oscc` `ir_sha256`) |
| 80 | 32 | executable: SHA-256 of the realized executable bytes (equals `oscc` `code_sha256`) |
| 112 | 32 | dependencies: SHA-256(`"OMEGA-RECEIPT-DEPS-V1\0"` ‖ u32 count ‖ receipt ids, strictly ascending) |
| 144 | 32 | checker: identity of the checker2 build (section 5) |
| 176 | 32 | producer checker: SHA-256 of the `oscc` binary that produced the bytes |
| 208 | 32 | target: SHA-256(`"OMEGA-TARGET-V1\0"` ‖ target name), e.g. `aarch64-linux-host` |
| 240 | 32 | firmware: SHA-256 of the firmware image; all zero = none (host) |
| 272 | 32 | input: SHA-256 of the input or dataset identity; all zero = none |
| 304 | 8 | policy epoch the receipt was issued under |
| 312 | 8 | expiry epoch: last epoch at which it is fresh; must be >= policy epoch |
| 320 | 32 | key fingerprint: SHA-256 of the signer's 32-byte Ed25519 public key |
| 352 | 64 | Ed25519 signature |

"\0" is one zero byte, included in the hash. Receipt id = SHA-256 of all 416 bytes. Ed25519
signatures are deterministic, so the same inputs and key always give the same receipt and id.

## 3. Signature

Signed message (65 bytes) = `"OMEGA-REALIZATION-RECEIPT-SIG-V1\0"` ‖
SHA-256(`"OMEGA-REALIZATION-RECEIPT-V1\0"` ‖ bytes [0, 352)).

The verifier takes the public key from its own anchor set for the receipt's class, matched by
fingerprint, never from the receipt (same rule as aienos `osc_unit_admit` step 13).

### Keys

- **OWNER** (class 2): Drake's owner root from the aienos#380 ceremony,
  `578ada3cb0c2d154623ac089191023c8ad4bc5e63b8ccd1a38db2d679b8c9e76`, generated on a YubiKey
  (PIV slot 9c). Compiled in as `c2r_owner_root_pk`. Omega never holds the private key:
  `checker2 receipt tbs` writes the 65-byte message, the YubiKey signs it, and
  `checker2 receipt attach` verifies and attaches the signature. The test suite proves the
  constant is the ceremony key by verifying the real ceremony signature
  (`tests/checker2/fixtures/owner/`, public data).
- **TEST** (class 1): seed = SHA-256(`"OMEGA-CHECKER2-TEST-KEY-v1 NOT FOR PRODUCTION"`), public
  on purpose. `c2r_sign_test` signs with this key only and only TEST class.

## 4. Evidence class

| value | name | meaning |
|---:|---|---|
| 1 | IR_CHECKED | checker2 decoded the canonical program bytes and accepted them. The executable is bound by hash only; it is **not** translation-validated against the program. |
| 2 | IR_CHECKED_LRAT | as 1, plus a declared property proved by an LRAT certificate that checker2's own LRAT checker accepted |

Any other value is refused.

## 5. Checker identity

`checker` = SHA-256(`C2_CHECKER_VERSION` including its terminating NUL ‖ the 64 lowercase hex
characters of SHA-256 over the concatenation of the checker2 sources in the fixed order of
`C2_ID_SRCS` in `mk/checker2.mk`). The sources include the IR decoder, the acceptance rules, the
LRAT checker, the bit-blaster, the receipt code and the command line; the untrusted solver
(`src/checker2/untrusted/`) is deliberately outside. `checker2 version` prints the version,
`sources_sha256` and `checker_id`. A changed checker gives a new identity. The verifier holds a list of
accepted checker identities in its policy (`C2ReceiptPolicy.checkers`); the list is required and an
empty list refuses every receipt. `checker2 receipt verify` accepts its own identity plus any given
with `--accept-checker`.

## 6. Verification order and named reasons

The verifier stops at the first failure and returns its reason. Codes are stable.

1. Structure: `SIZE` (1, not exactly 416 bytes), `MAGIC` (2), `VERSION` (3),
   `SIGNATURE_ALGORITHM` (4), `SIGNER_CLASS` (5), `EVIDENCE_CLASS` (6), `EPOCH_ORDER` (7).
   Code 8 is unassigned.
2. Class policy: in production mode any class other than OWNER is `TEST_SIGNER_IN_PRODUCTION`
   (9). The key must be in the anchor set of the receipt's class, else `UNTRUSTED_SIGNER` (10);
   in production the only owner anchor honoured is the compiled-in owner root; a key anchored in
   both classes is refused (`UNTRUSTED_SIGNER`).
3. Signature: `BAD_SIGNATURE` (11).
4. Revocation: receipt id, then signer key fingerprint, in the verifier's revocation lists:
   `REVOKED` (12).
5. Freshness (epochs, no clock): policy epoch below the verifier's floor is a replay of a
   superseded receipt, `STALE_EPOCH` (13); current epoch above expiry `EXPIRED` (14); current
   epoch below the policy epoch `NOT_YET_VALID` (15). The window is inclusive at both ends.
6. Checker identity: the receipt's `checker` must be in the verifier's list of accepted checker
   identities (section 5), else `UNTRUSTED_CHECKER` (22). The list is part of the policy and is
   checked with or without expectations.
7. Binding to what is being admitted: `WRONG_TARGET` (16), `WRONG_FIRMWARE` (17),
   `PROGRAM_MISMATCH` (18), `EXECUTABLE_MISMATCH` (19), `WRONG_PRODUCER` (23, producer checker
   identity), `DEPENDENCIES_MISMATCH` (24, dependency digest). The dependency digest binds the
   dependency receipt ids only: v1 does not fetch, resolve or verify the dependency receipts
   themselves (section 11).

`ARGUMENT` (20) and `KEY_CLASS` (21, signing refused) are tool and signing errors.

The revocation lists, the epoch floor and the current epoch come from the admitting side's
policy (for aienos: its own authority), never from the receipt.

## 7. Storage: content-addressed, never rewritten

`checker2 receipt issue --out-dir D` writes `D/<id hex>.rcpt` with exclusive create. If the file
already exists with identical bytes the write is a no-op; any other existing content is an error,
never an overwrite.

## 8. Command line

    checker2 check <unit.ir>
    checker2 version
    checker2 receipt issue <unit.ir> <exe> --target T --epoch E --expiry X
                           (--out-dir D | --owner-draft PATH)
                           [--firmware F] [--requirements R | --lrat FUNC=PROOF ...]
                           [--input I] [--producer P] [--dep ID ...]
    checker2 receipt tbs <receipt> <out65>
    checker2 receipt attach <receipt> <sig64> --out-dir D
    checker2 receipt verify <receipt> <unit.ir> <exe> --target T --epoch E
                           [--allow-test-key | --production] [--floor N] [--firmware F]
                           [--revoked-id ID ...] [--revoked-key FP ...] [--lrat FUNC=PROOF ...]
                           [--producer P] [--dep ID ...] [--accept-checker ID ...]
    checker2 prove-cnf <unit.ir> <func> <out.cnf>
    checker2 prove-check <unit.ir> <func> <proof.lrat>

Exit 0 accept or done, 1 reject or refused, 2 usage or I/O error. `issue` refuses to sign bytes
checker2 rejects. `verify` applies the production policy by default (fail closed): a TEST-class
receipt is refused as `TEST_SIGNER_IN_PRODUCTION` unless `--allow-test-key` is given, which only
qualification and test runs may do; `--production` states the default, and giving both is a usage
error. `--dep` ids must be strictly ascending (their digest is order-sensitive). `verify` always
compares the producer and dependency digests: no `--producer` means an all-zero producer and no
`--dep` means an empty dependency list, exactly what `issue` writes without them.

Owner signing, without Omega ever holding the private key: `issue --owner-draft PATH` writes an
OWNER-class receipt with the owner root fingerprint and an all-zero signature; `tbs` writes the
65-byte message of section 3; Drake signs it on the YubiKey (PIV slot 9c); `attach` verifies the
signature against the compiled-in owner root and stores the signed receipt by id (section 7).

## 9. What checker2 accepts, and what it was written from

checker2 decodes the canonical OSC IR bytes itself and applies its own acceptance rules. It links
no file of `src/compiler/` and shares no parser, evaluator or instruction table with the producer
(oscc). Its sources were written from these inputs only; no existing checker implementation
(`src/compiler/osc_check.*` or any other) was read:

- `src/compiler/osc_ir.h`: the IR data structures, opcodes, scalar kinds and trap codes
  (lines 152 to 176); the encoder part of `src/compiler/osc_ir.c` (lines 537 to 729), read for the
  exact byte layout only, as the IR format has no separate written specification;
- `src/compiler/osc_front.h` (front-end interface, used by the test producer only);
- `docs/osc/` (OSC-1 design, including the semantics in section 5 of `OSC-1-DESIGN.md`);
- `src/omega_receipt.h`, `src/omega_vc_bridge.h`, `src/searchtrace/sig/*.h`, `src/sha256.h`;
- aienos `osc_unit.c` (the admission side's receipt rules, step 13);
- the build files under `mk/`.

Every rejection names a stable rule (`C2-*`, listed in `c2_check.c`). Acceptance requires, per
function: valid types and operands; well-formed control flow; every use defined on every path;
ownership, arena, pool and handle discipline (a may-set analysis that rejects definite errors);
every loop reducible and bounded by a guarded counter with a constant limit of at most 10^6.

## 10. Evidence class 2: LRAT proofs

Property `no-trap-except-requires` of a function F: for every argument value, a run of F that
passes its `requires` clauses ends without any trap (overflow, division, shift, cast, loop bound,
`ensures`, or an explicit trap). checker2 builds the CNF of "some argument reaches a trap" itself,
from the IR bytes (`c2_blast.h`: scalar functions only; calls, memory, division and signed right
shift are refused, never approximated). The solver (`src/checker2/untrusted/c2sat.c`) is
untrusted and outside the checker identity. An UNSAT answer counts only when checker2's own LRAT
checker (`c2_lrat.h`: RUP steps only, RAT hints refused, the empty clause required,
clause ids at most input clauses plus proof lines so a huge id cannot size its tables) accepts the
proof against the CNF checker2 rebuilt.

With `--lrat FUNC=PROOF` (functions strictly ascending), `issue` checks every proof and sets
evidence class 2 with `requirements` = SHA-256 of the requirements object:

    OMEGA-REQUIREMENTS-V1\n
    no-trap-except-requires <FUNC> proof_sha256=<64 hex>\n      (one line per proof)

`verify` replays: a class 2 receipt is valid only when the same proofs are given, each checks
against the IR bytes, and the rebuilt object hashes to `requirements`. Reasons: `LRAT-PROOFS-REQUIRED`,
`LRAT-REPLAY-FAILED`, `LRAT-REQUIREMENTS-MISMATCH`, and `LRAT-NOT-CLAIMED` (proofs offered for a
class 1 receipt). Proofs are deterministic: the solver writes byte-identical proofs on every run.

## 11. Not covered (open)

- Translation validation of the executable against the program (plan section 3) is not done;
  evidence classes 1 and 2 say so in their definitions.
- Production acceptance of an OWNER receipt: VERIFIED for one positive case. Receipt a01 was signed
  on Drake's YubiKey (serial 39017074, PIV slot 9c, owner root 578ada3c...9e76) on 2026-10-15T00:02:50Z;
  `checker2 receipt verify --production` accepts it, openssl verifies the Ed25519 signature against the
  ceremony public key, and six tampered controls are refused (`make test-checker2-owner-receipt`,
  fixtures in tests/checker2/fixtures/owner-receipt). The aienos kernel verifier accepts it under an owner
  policy; the shipped aienos release policy does not yet carry an owner receipt anchor, so it reports
  UNTRUSTED_SIGNER there. Adding that anchor is aienos work and is UNVERIFIED.
- The admission side (aienos#383, merged in aienos#384) verifies exactly these bytes.
- Dependency receipts are not resolved in v1: the `dependencies` digest binds their ids, and a
  verifier can require the expected ids (`DEPENDENCIES_MISMATCH`), but no verifier fetches the
  dependency receipts or checks their validity, class or freshness. UNVERIFIED for any chain.
- `src/omega_vc_bridge.h` still self-mints HOST_TEST receipts. That route admits OmegaProgram
  programs (`omega_program_ir.h`), not OSC IR, and records an aien-proof EvidenceReceiptV1
  (`src/omega_receipt.h`, contract owned by aien-protocols). Replacing it needs checker2 to judge
  that IR (or those programs to arrive as OSC) and a change to that shared contract; neither is
  in this change.
- A differential of the planted IR defects against the producer's checker needs an IR-bytes entry
  point on the producer side (omega#444); until then those 96 defects are judged by checker2 only.
- Bit-blaster semantics are cross-checked against oscc's compiled AArch64 code (section 12,
  `blast_native.sh`), not against an independent reference interpreter. Agreement shows checker2's
  CNF and the producer's back end read section 5 of OSC-1-DESIGN the same way; a misreading shared
  by both would pass. Operands wider than 16 bits are checked on edge values and samples, not
  exhaustively; gate programs are compared on trap or no trap only, not on return values. UNVERIFIED
  beyond those inputs.

## 12. Tests

`make test-checker2` (CI job `checker2` in `.github/workflows/host-suites.yml`):

- `test_c2_receipt` (plain and ASan/UBSan): signing, every named refusal, tampered bytes, wrong
  key, TEST key in production mode, checker identity not in the policy list (and an empty list),
  wrong producer and dependency digests, the real owner ceremony signature;
- `differential.sh`: every OSC unit in the repo plus 48 generated units, oscc run as a black box;
  both must accept the same 107 units with identical bytes, and the known-bad units must be
  refused (138 by the front end);
- `test_c2_ir` (plain and ASan/UBSan): 107 units re-encode identically, every truncation refused,
  bit flips never crash, 96 planted defects each refused by its exact rule;
- `test_c2_lrat` (plain and ASan/UBSan): hand-written LRAT cases, each refusal by its exact rule
  (including a huge clause id refused as `LRAT-ID-RANGE` without a large allocation), 20000
  corrupted proofs never verify a satisfiable formula;
- `lrat.sh`: 20 bounded programs proved and replayed, 9 trap programs found to trap, 56 receipt and
  proof controls (corrupted, substituted or misapplied proofs and receipts refused, including a
  TEST receipt under the default production policy, a receipt from another checker identity unless
  accepted with `--accept-checker`, and a wrong producer or dependency digest);
- `test_c2_blast`: non-scalar functions refused; every gate program evaluated input by input
  without a solver (exhaustive to 16 argument bits), agreeing with its expectation;
- `blast_native.sh` + `test_c2_blast_native` (AArch64 host): 180 one-operation programs (every
  blastable operation, comparison, cast pair and unary operation on every integer type and bool) and
  the 29 gate programs, compiled by oscc as a black box; for every input (exhaustive to 16 operand
  bits, else edge values and 3000 samples) checker2's CNF must agree with the compiled code on the
  trap and, through an `ensures result != c` probe, on the exact result value; 8 negative controls
  (the CNF of one operation against the code of another) must disagree;
- `owner_receipt.sh`: the positive production case of section 11. The committed OWNER draft and its
  65-byte message (`tests/checker2/fixtures/owner-receipt`, PROVENANCE.txt) are rebuilt and must be
  byte-identical; the receipt signed once on Drake's YubiKey must verify `--production`, and a changed
  signature byte, a changed program hash, a wrong target, an expired epoch, a revoked owner key and a
  wrong executable must each be refused. It fails `OWNER-RECEIPT PENDING` until that signature is committed;
- `mutants.sh`: each line tagged `GUARD:<name>` in the receipt code, decoder, acceptance rules,
  LRAT checker and bit-blaster is removed in turn; a test must fail every time (104 of 104); the script also refuses a tag count that differs
  from the mutant count.
