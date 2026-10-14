# One-layer receipts v1: signed Omega realization receipts (normative)

Stage 3 of the one-layer plan (omega#446). Code: `src/checker2/c2_receipt.{h,c}`. Tests:
`tests/checker2/test_c2_receipt.c`, negative controls `tests/checker2/mutants.sh`.
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

`checker` = SHA-256 of the checker2 version string (`C2_CHECKER_VERSION`) followed by the
SHA-256 of each checker2 source file in a fixed order, computed at build time by
`mk/checker2.mk` and printed by `checker2 version`. A changed checker gives a new identity.

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
6. Binding to what is being admitted: `WRONG_TARGET` (16), `WRONG_FIRMWARE` (17),
   `PROGRAM_MISMATCH` (18), `EXECUTABLE_MISMATCH` (19).

`ARGUMENT` (20) and `KEY_CLASS` (21, signing refused) are tool and signing errors.

The revocation lists, the epoch floor and the current epoch come from the admitting side's
policy (for aienos: its own authority), never from the receipt.

## 7. Storage: content-addressed, never rewritten

`checker2 receipt issue --out-dir D` writes `D/<id hex>.rcpt` with exclusive create. If the file
already exists with identical bytes the write is a no-op; any other existing content is an error,
never an overwrite.

## 8. Not covered (open)

- Translation validation of the executable against the program (plan section 3) is not done;
  evidence class 1 says so in its definition.
- Production acceptance of an OWNER receipt is tested only in its refusals; a positive production
  case needs one receipt signed on Drake's YubiKey (owner presence, decision D3). UNVERIFIED
  until then.
- The admission side (aienos#383) is separate work.
- `src/omega_vc_bridge.h` still self-mints HOST_TEST receipts for OmegaProgram programs; replacing
  that route is not done in this change.
