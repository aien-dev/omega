#!/bin/sh
# Positive production case for OWNER receipts (spec/one-layer-receipts.md section 11, omega#446):
# one receipt signed on Drake's YubiKey (owner root, PIV slot 9c) must be accepted by
# `checker2 receipt verify --production`, and its tampered copies refused.
#
# Fixtures (tests/checker2/fixtures/owner-receipt, PROVENANCE.txt):
#   a01.ir, a01.code            program and executable bytes of aienos osc_unit/vectors/a01_valid_min.unit
#   a01.owner-draft.rcpt        `receipt issue --owner-draft` output: OWNER class, all-zero signature
#   a01.owner-draft.tbs         `receipt tbs` output: the 65 bytes the YubiKey signs
#   a01.owner.sig               the 64-byte Ed25519 signature made on the YubiKey (Drake)
#   a01.owner.rcpt              `receipt attach` output: the signed receipt
# Checks: the draft and the message are rebuilt by this checker2 and must be byte-identical (a changed
# checker identity changes them, and the signed receipt must then be made again); the signed receipt is
# the draft with exactly the committed signature in bytes [352, 416); production verify says VALID;
# controls: a changed signature byte, a changed program hash, a wrong target, an expired epoch, a
# revoked owner key and a wrong executable are each refused with their named reason.
# Until the signature is committed the test FAILS with OWNER-RECEIPT PENDING: the gap stays visible.
# Usage: owner_receipt.sh <checker2> <fixture-dir> <work-dir>
set -u
C2=$1 F=$2 W=$3
TARGET=aienos-ck-aarch64-el0 EPOCH=20 EXPIRY=100 FLOOR=10
OWNER_FP=$(od -An -tx1 -j320 -N32 "$F/a01.owner-draft.rcpt" | tr -d ' \n') # key fingerprint written by issue
rm -rf "$W" && mkdir -p "$W" || exit 2
fail=0 ctl=0
bad() { echo "owner_receipt: FAIL $*"; fail=$((fail + 1)); }

"$C2" receipt issue "$F/a01.ir" "$F/a01.code" --target $TARGET --epoch $EPOCH --expiry $EXPIRY \
    --owner-draft "$W/draft.rcpt" || bad "issue --owner-draft"
cmp -s "$W/draft.rcpt" "$F/a01.owner-draft.rcpt" || bad "owner draft is not byte-identical to the committed draft"
"$C2" receipt tbs "$F/a01.owner-draft.rcpt" "$W/tbs" || bad "tbs"
cmp -s "$W/tbs" "$F/a01.owner-draft.tbs" || bad "signed message is not byte-identical to the committed one"

if [ ! -f "$F/a01.owner.rcpt" ] || [ ! -f "$F/a01.owner.sig" ]; then
    echo "owner_receipt: OWNER-RECEIPT PENDING: no YubiKey-signed receipt committed yet (~/bin/owner-sign-receipt-test.sh)"
    exit 1
fi
[ "$(wc -c <"$F/a01.owner.rcpt")" = 416 ] || bad "signed receipt is not 416 bytes"
[ "$(wc -c <"$F/a01.owner.sig")" = 64 ] || bad "signature is not 64 bytes"
head -c 352 "$F/a01.owner.rcpt" >"$W/head.signed"
head -c 352 "$F/a01.owner-draft.rcpt" >"$W/head.draft"
cmp -s "$W/head.signed" "$W/head.draft" || bad "signed receipt differs from the draft in the signed bytes"
tail -c 64 "$F/a01.owner.rcpt" | cmp -s - "$F/a01.owner.sig" || bad "signed receipt does not carry the committed signature"
# attach again from the committed draft and signature: verifies against the compiled-in owner root
mkdir -p "$W/store"
"$C2" receipt attach "$F/a01.owner-draft.rcpt" "$F/a01.owner.sig" --out-dir "$W/store" >"$W/attach.out" || bad "attach refuses the committed signature"
cmp -s "$(sed -n 's/^receipt=//p' "$W/attach.out" | cut -d' ' -f1)" "$F/a01.owner.rcpt" || bad "attach does not rebuild the committed receipt"

v() { "$C2" receipt verify "$1" "$F/a01.ir" "${2:-$F/a01.code}" --target "${3:-$TARGET}" --epoch "${4:-$EPOCH}" --floor $FLOOR $5; }
out=$(v "$F/a01.owner.rcpt" "" "" "" --production)
[ "$out" = receipt=VALID ] || bad "production verify: $out"
out=$(v "$F/a01.owner.rcpt" "" "" "" "")
[ "$out" = receipt=VALID ] || bad "default (production) verify: $out"

# expect NAME WANT RECEIPT EXE TARGET EPOCH EXTRA (empty: the default)
expect() {
    n=$1 want=$2; shift 2
    got=$(v "$@")
    if [ "$got" = "receipt=$want" ]; then ctl=$((ctl + 1)); else bad "control $n: want $want, got $got"; fi
}
flip() { cat "$F/a01.owner.rcpt" >"$2"; printf '\377' | dd of="$2" bs=1 seek="$1" conv=notrunc status=none; }
flip 400 "$W/sig.flip"; expect signature-byte BAD_SIGNATURE "$W/sig.flip" "" "" "" --production
flip 48 "$W/prog.flip"; expect program-hash BAD_SIGNATURE "$W/prog.flip" "" "" "" --production
expect wrong-target WRONG_TARGET "$F/a01.owner.rcpt" "" aarch64-linux-host "" --production
expect expired EXPIRED "$F/a01.owner.rcpt" "" "" $((EXPIRY + 1)) --production
expect revoked-key REVOKED "$F/a01.owner.rcpt" "" "" "" "--production --revoked-key $OWNER_FP"
expect wrong-executable EXECUTABLE_MISMATCH "$F/a01.owner.rcpt" "$F/a01.ir" "" "" --production

if [ "$fail" -ne 0 ]; then echo "owner_receipt: FAIL ($fail)"; exit 1; fi
echo "owner_receipt: PASS owner-signed receipt VALID in production, $ctl controls refused"
