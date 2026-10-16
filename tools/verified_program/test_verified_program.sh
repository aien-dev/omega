#!/usr/bin/env bash
# tools/verified_program/test_verified_program.sh <vp_dir> <omegese> <omega_emit> <checker2>
# Checks for the verified-program entrypoint and its independent verifier (omega#505,
# docs/one_layer/VERIFIED_PROGRAM.md). Host CPU only: no model, no GPU, no held-out data.
#
#  1. positives: fixed reference functions (public dev tasks gcd, collatz_steps, checked_mul with its
#     overflow traps) and one model-written answer kept verbatim (tests/one_layer/omegese/trial2/dev08)
#     run through omegese -> omega_emit -> checker2 -> authorized execution, and vp_verify recomputes
#     every result with its own oracle: VERIFIED. Two runs of one request give the same receipt id.
#  2. negatives, each with the exit and the named reason it must give:
#     incorrect but parseable programs (a wrong gcd; the model's collatz on an input past 32 bits),
#     checker2 rejection, emit refusal, tampered IR / code / output, stale or mismatched objective,
#     stale grant, unauthorized effect, bytes changed between check and execution and a forbidden
#     system call inside the sandbox (canary builds),
#     unknown versions, pin mismatch, signature mismatch, unbounded inputs, a GPU tier request.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
[ $# -eq 4 ] || { echo "usage: test_verified_program.sh <vp_dir> <omegese> <omega_emit> <checker2>" >&2; exit 2; }
VPD="$1" G="$2" OME="$3" C2="$4"
VP="$VPD/verified_program" VV="$VPD/vp_verify"
FX="$HERE/fixtures" DEV="$ROOT/tests/one_layer/omegese"
T="$(mktemp -d "${TMPDIR:-/tmp}/vp-test.XXXXXXXX")"; trap 'rm -rf "$T"' EXIT
fails=0 passes=0
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
ok() { echo "ok   $*"; passes=$((passes + 1)); }

oid() { printf 'AIEN_E2E_OBJECTIVE_V1\n%s' "$1" | sha256sum | cut -c1-64; }
TXT_GCD="Pi asks: the greatest common divisor of each pair of numbers"
TXT_COL="Pi asks: how many Collatz steps each number takes to reach 1"
TXT_MUL="Pi asks: multiply each pair of 16-bit signed numbers, refusing overflow"
TXT_OTHER="Pi asks: something else entirely"
C2ID="$("$C2" version | sed -n 's/^checker_id=//p')"
GSHA="$(sha256sum "$G" | cut -c1-64)" OSHA="$(sha256sum "$OME" | cut -c1-64)"

# grant <file> <objective text> <effects> [epoch_first epoch_last max_calls tier]
grant() {
  printf 'schema aien.verified_program.grant.v1\nobjective_id %s\ntier %s\neffects %s\nepoch_first %s\nepoch_last %s\nmax_calls %s\n' \
    "$(oid "$2")" "${7:-host-cpu-aarch64}" "$3" "${4:-1}" "${5:-10}" "${6:-64}" > "$1"
}
# req <file> <objective text> <format> <program> <entry> <params> <ret> <inputs> <grant> [key=value overrides]
req() {
  local f="$1" text="$2" fmt="$3" prog="$4" entry="$5" params="$6" ret="$7" in="$8" gr="$9"; shift 9
  local schema="omega.verified_program.request.v1" id; id="$(oid "$text")" tier="host-cpu-aarch64" epoch=1
  local pc="$C2ID" pg="$GSHA" po="$OSHA" kv
  for kv in "$@"; do case "$kv" in
    schema=*) schema="${kv#*=}" ;; objective_id=*) id="${kv#*=}" ;; tier=*) tier="${kv#*=}" ;;
    epoch=*) epoch="${kv#*=}" ;; pin_checker2_id=*) pc="${kv#*=}" ;; esac; done
  printf 'schema %s\nobjective_id %s\nobjective_text %s\nprogram_format %s\nprogram %s\nentry %s\nparams %s\nret %s\ninputs %s\ntier %s\ngrant %s\nepoch %s\npin_checker2_id %s\npin_omegese_sha256 %s\npin_omega_emit_sha256 %s\n' \
    "$schema" "$id" "$text" "$fmt" "$prog" "$entry" "$params" "$ret" "$in" "$tier" "$gr" "$epoch" "$pc" "$pg" "$po" > "$f"
}
# run <name> <request> [runner]: runs into $T/<name>; sets RC and leaves result.txt there
run() {
  RC=0; "${3:-$VP}" run "$2" "$T/$1" --omegese "$G" --omega-emit "$OME" --checker2 "$C2" --commit test > "$T/$1.out" 2>&1 || RC=$?
}
field() { sed -n "s/^$2=//p" "$T/$1/result.txt" 2>/dev/null | head -1; }
# expect_exec <name>: run exit 0 and verdict EXECUTED
expect_exec() {
  if [ "$RC" -eq 0 ] && [ "$(field "$1" verdict)" = EXECUTED ]; then return 0; fi
  bad "$1: want EXECUTED, got exit $RC: $(cat "$T/$1.out") $(cat "$T/$1/result.txt" 2>/dev/null | tr '\n' ' ')"; return 1
}
# expect_refused <name> <CODE>: run exit 1, verdict REFUSED with that rejection, and no receipt written
expect_refused() {
  local got; got="$(field "$1" rejection)"
  if [ "$RC" -eq 1 ] && [ "$(field "$1" verdict)" = REFUSED ] && [ "$got" = "$2" ] && [ -z "$(ls "$T/$1/receipts" 2>/dev/null)" ]; then
    ok "$1: REFUSED $2 ($(field "$1" stage): $(field "$1" detail))"
  else bad "$1: want REFUSED $2, got exit $RC rejection=$got: $(cat "$T/$1.out")"; fi
}
# verify <name> <objective text> <oracle> [receipt id] [extra args]: sets VRC and VOUT
verify() {
  local id="$4"; [ -n "$id" ] || id="$(field "$1" receipt_id)"
  VRC=0; VOUT="$("$VV" --receipt-id "$id" --store "$T/$1" --objective-text "$2" --oracle "$3" --checker2 "$C2" --epoch 1 --host-check 2>&1)" || VRC=$?
}
expect_verified() {
  verify "$1" "$2" "$3" "${4:-}"
  if [ "$VRC" -eq 0 ] && [ "${VOUT%% *}" = VERIFIED ]; then ok "$1: $VOUT"; else bad "$1: want VERIFIED, got exit $VRC: $VOUT"; fi
}
expect_fail() { # <name> <objective text> <oracle> <CODE> [receipt id]
  verify "$1" "$2" "$3" "${5:-}"
  case "$VOUT" in "FAIL $4"*) [ "$VRC" -eq 1 ] && { ok "$1: verifier $VOUT"; return; } ;; esac
  bad "$1: want verifier FAIL $4, got exit $VRC: $VOUT"
}

[ -x "$VP" ] && [ -x "$VV" ] || { echo "FAIL tools missing: $VP $VV"; exit 1; }

# ---- 1. positives ----------------------------------------------------------------------------------
grant "$T/g_gcd" "$TXT_GCD" pure
grant "$T/g_col" "$TXT_COL" pure
grant "$T/g_mul" "$TXT_MUL" pure
req "$T/r_gcd" "$TXT_GCD" omegese-v1 "$DEV/dev/dev04.omegese" gcd "u32 u32" u32 "$FX/gcd.in" "$T/g_gcd"
run gcd "$T/r_gcd"; expect_exec gcd && expect_verified gcd "$TXT_GCD" gcd_u32
run gcd2 "$T/r_gcd"; expect_exec gcd2
if [ -n "$(field gcd receipt_id)" ] && [ "$(field gcd receipt_id)" = "$(field gcd2 receipt_id)" ]; then
  ok "determinism: two runs, one receipt id $(field gcd receipt_id)"; else bad "determinism: receipt ids differ"; fi
r="$(field gcd receipt_id)"
if [ -f "$T/gcd/receipts/$r.vpr" ] && [ "$(sha256sum < "$T/gcd/receipts/$r.vpr" | cut -c1-64)" = "$r" ] &&
   grep -qx "outputs_sha256=$(field gcd outputs_sha256)" "$T/gcd/receipts/$r.vpr" &&
   grep -qx "objective_id=$(oid "$TXT_GCD")" "$T/gcd/receipts/$r.vpr" && grep -qx 'gpu_used=no' "$T/gcd/receipts/$r.vpr"; then
  ok "receipt: content-addressed, bound to the objective and the result, gpu_used=no"
else bad "receipt layout"; fi
if grep -Eq '^wall_ns=[0-9]+$' "$T/gcd/measure.txt" && grep -Eq '^maxrss_kb=[0-9]+$' "$T/gcd/measure.txt" &&
   grep -qx "receipt_id=$r" "$T/gcd/measure.txt"; then ok "measurements recorded beside the receipt"; else bad "measure.txt"; fi

req "$T/r_col" "$TXT_COL" omegese-v1 "$DEV/dev/dev08.omegese" collatz_steps u32 u32 "$FX/collatz.in" "$T/g_col"
run col "$T/r_col"; expect_exec col && expect_verified col "$TXT_COL" collatz_steps_u32
req "$T/r_colm" "$TXT_COL" omegese-v1 "$DEV/trial2/dev08.omegese" collatz_steps u32 u32 "$FX/collatz.in" "$T/g_col"
run colm "$T/r_colm"; expect_exec colm && expect_verified colm "$TXT_COL" collatz_steps_u32
req "$T/r_mul" "$TXT_MUL" omegese-v1 "$DEV/dev/dev05.omegese" checked_mul "i16 i16" i16 "$FX/mul.in" "$T/g_mul"
run mul "$T/r_mul"; expect_exec mul && expect_verified mul "$TXT_MUL" checked_mul_i16
if grep -qx 'T 1' "$T/mul/store/$(field mul outputs_sha256)" 2>/dev/null; then ok "mul: overflow traps recorded as T 1"; else bad "mul: no trap line"; fi
# the op-list form of the same gcd (control for the checker2 rejection below)
"$G" emit "$DEV/dev/dev04.omegese" "$T/dev04.json" > /dev/null
req "$T/r_gcdop" "$TXT_GCD" oplist-v1 "$T/dev04.json" gcd "u32 u32" u32 "$FX/gcd.in" "$T/g_gcd"
run gcdop "$T/r_gcdop"; expect_exec gcdop && expect_verified gcdop "$TXT_GCD" gcd_u32
# heap effect granted (control for the unauthorized-effect refusal below)
grant "$T/g_gcdh" "$TXT_GCD" "pure heap"
req "$T/r_heapok" "$TXT_GCD" omegese-v1 "$FX/gcd_heap.omegese" gcd "u32 u32" u32 "$FX/gcd.in" "$T/g_gcdh"
run heapok "$T/r_heapok"; expect_exec heapok && expect_verified heapok "$TXT_GCD" gcd_u32
# a designer collatz on the input past 32 bits (control for the model answer below)
req "$T/r_colbig" "$TXT_COL" omegese-v1 "$DEV/dev/dev08.omegese" collatz_steps u32 u32 "$FX/collatz_big.in" "$T/g_col"
run colbig "$T/r_colbig"; expect_exec colbig && expect_verified colbig "$TXT_COL" collatz_steps_u32

# ---- 2. negatives ----------------------------------------------------------------------------------
# incorrect but parseable: accepted by emit and checker2, executed, and caught only by the verifier
req "$T/r_wrong" "$TXT_GCD" omegese-v1 "$FX/gcd_wrong.omegese" gcd "u32 u32" u32 "$FX/gcd.in" "$T/g_gcd"
run wrong "$T/r_wrong"; expect_exec wrong && expect_fail wrong "$TXT_GCD" gcd_u32 WRONG_RESULT
req "$T/r_colmbig" "$TXT_COL" omegese-v1 "$DEV/trial2/dev08.omegese" collatz_steps u32 u32 "$FX/collatz_big.in" "$T/g_col"
run colmbig "$T/r_colmbig"; expect_exec colmbig && expect_fail colmbig "$TXT_COL" collatz_steps_u32 WRONG_RESULT
# checker2 rejection (the op list passes omega_emit), and an emit refusal
req "$T/r_unb" "$TXT_GCD" oplist-v1 "$FX/gcd_unbounded.json" gcd "u32 u32" u32 "$FX/gcd.in" "$T/g_gcd"
run unb "$T/r_unb"; expect_refused unb CHECKER_REJECT
case "$(field unb detail)" in *C2-LOOP-UNBOUNDED*) ok "unb: rule C2-LOOP-UNBOUNDED named" ;; *) bad "unb: rule not named: $(field unb detail)" ;; esac
req "$T/r_arity" "$TXT_GCD" omegese-v1 "$FX/gcd_arity.omegese" gcd "u32 u32" u32 "$FX/gcd.in" "$T/g_gcd"
run arity "$T/r_arity"; expect_refused arity EMIT_REFUSED
# tampered artifacts in the store after a good run
for what in ir_sha256 code_sha256 outputs_sha256; do
  rm -rf "$T/tam_$what"; cp -r "$T/gcd" "$T/tam_$what"
  h="$(sed -n "s/^$what=//p" "$T/gcd/receipts/$r.vpr")"; f="$T/tam_$what/store/$h"
  b="$(od -An -tu1 -j8 -N1 "$f" | tr -d ' ')"   # flip the low bit of byte 8 (a real change, whatever it held)
  chmod u+w "$f"; printf "\\$(printf '%03o' $(( b ^ 1 )))" | dd of="$f" bs=1 seek=8 conv=notrunc status=none
  cmp -s "$f" "$T/gcd/store/$h" && bad "tam_$what: the tamper changed nothing"
  expect_fail "tam_$what" "$TXT_GCD" gcd_u32 STORE_TAMPERED "$r"
done
# a forged result stored under its own hash, with the receipt rewritten to name it: new receipt id
rm -rf "$T/forge"; cp -r "$T/gcd" "$T/forge"; o="$(field gcd outputs_sha256)"
sed '1s/.*/R 13/' "$T/gcd/store/$o" > "$T/forged.out"; fo="$(sha256sum < "$T/forged.out" | cut -c1-64)"
cp "$T/forged.out" "$T/forge/store/$fo"
sed "s/^outputs_sha256=$o\$/outputs_sha256=$fo/" "$T/gcd/receipts/$r.vpr" > "$T/forged.vpr"; fr="$(sha256sum < "$T/forged.vpr" | cut -c1-64)"
rm "$T/forge/receipts/$r.vpr"; cp "$T/forged.vpr" "$T/forge/receipts/$fr.vpr"
expect_fail forge "$TXT_GCD" gcd_u32 RECEIPT_MISSING "$r" # the Pi holds the original id: the forged receipt is not it
cp "$T/forged.vpr" "$T/forge/receipts/$r.vpr"
expect_fail forge "$TXT_GCD" gcd_u32 RECEIPT_ID_MISMATCH "$r"
rm "$T/forge/receipts/$r.vpr"
expect_fail forge "$TXT_GCD" gcd_u32 WRONG_RESULT "$fr"   # even with the forged id, the oracle refuses it
# stale or mismatched objective, stale grant
req "$T/r_oid" "$TXT_GCD" omegese-v1 "$DEV/dev/dev04.omegese" gcd "u32 u32" u32 "$FX/gcd.in" "$T/g_gcd" objective_id="$(oid "$TXT_OTHER")"
run oid "$T/r_oid"; expect_refused oid OBJECTIVE_MISMATCH
grant "$T/g_other" "$TXT_OTHER" pure
req "$T/r_gobj" "$TXT_GCD" omegese-v1 "$DEV/dev/dev04.omegese" gcd "u32 u32" u32 "$FX/gcd.in" "$T/g_other"
run gobj "$T/r_gobj"; expect_refused gobj GRANT_OBJECTIVE
grant "$T/g_old" "$TXT_GCD" pure 1 3
req "$T/r_gold" "$TXT_GCD" omegese-v1 "$DEV/dev/dev04.omegese" gcd "u32 u32" u32 "$FX/gcd.in" "$T/g_old" epoch=4
run gold "$T/r_gold"; expect_refused gold GRANT_EPOCH
expect_fail gcd "$TXT_OTHER" gcd_u32 OBJECTIVE_MISMATCH  # the Pi asked something else
# unauthorized effect: the heap program under a grant for pure compute only
req "$T/r_heap" "$TXT_GCD" omegese-v1 "$FX/gcd_heap.omegese" gcd "u32 u32" u32 "$FX/gcd.in" "$T/g_gcd"
run heap "$T/r_heap"; expect_refused heap UNAUTHORIZED_EFFECT
# a tampered grant after a good heap run: the verifier recomputes the effects from the IR
rm -rf "$T/tgr"; cp -r "$T/heapok" "$T/tgr"; hr="$(field heapok receipt_id)"
gh="$(sed -n 's/^grant_sha256=//p' "$T/heapok/receipts/$hr.vpr")"
sed 's/^effects .*/effects pure/' "$T/heapok/store/$gh" > "$T/g_forged"; gf="$(sha256sum < "$T/g_forged" | cut -c1-64)"
cp "$T/g_forged" "$T/tgr/store/$gf"
sed "s/^grant_sha256=$gh\$/grant_sha256=$gf/; s/^effects_granted=.*/effects_granted=pure/" "$T/heapok/receipts/$hr.vpr" > "$T/tgr.vpr"
tr="$(sha256sum < "$T/tgr.vpr" | cut -c1-64)"; cp "$T/tgr.vpr" "$T/tgr/receipts/$tr.vpr"
expect_fail tgr "$TXT_GCD" gcd_u32 UNAUTHORIZED_EFFECT "$tr"
# bytes changed between the check and the execution (canary builds of the runner)
for k in 1 2; do
  want=PREFLIGHT_IR_CHANGED; [ "$k" = 2 ] && want=PREFLIGHT_CODE_CHANGED
  if [ -x "$VPD/verified_program_tamper$k" ]; then run "tamper$k" "$T/r_gcd" "$VPD/verified_program_tamper$k"; expect_refused "tamper$k" "$want"
  else bad "canary build verified_program_tamper$k missing"; fi
done
# the sandbox is live: a canary child that makes one forbidden system call is killed by the kernel
if [ -x "$VPD/verified_program_tamper3" ]; then
  run tamper3 "$T/r_gcd" "$VPD/verified_program_tamper3"; expect_refused tamper3 EXEC_FAILED
  case "$(field tamper3 detail)" in *signal_9*) ok "tamper3: killed by SIGKILL (seccomp strict)" ;; *) bad "tamper3: not a seccomp kill: $(field tamper3 detail)" ;; esac
else bad "canary build verified_program_tamper3 missing"; fi
# unknown versions
req "$T/r_v2" "$TXT_GCD" omegese-v1 "$DEV/dev/dev04.omegese" gcd "u32 u32" u32 "$FX/gcd.in" "$T/g_gcd" schema=omega.verified_program.request.v2
run v2 "$T/r_v2"; expect_refused v2 UNKNOWN_VERSION
"$OME" emit "$T/dev04.json" "$T/dev04.oscir" > /dev/null
cp "$T/dev04.oscir" "$T/dev04v9.oscir"; printf '\011' | dd of="$T/dev04v9.oscir" bs=1 seek=7 conv=notrunc status=none
req "$T/r_irv" "$TXT_GCD" oscir-v1 "$T/dev04v9.oscir" gcd "u32 u32" u32 "$FX/gcd.in" "$T/g_gcd"
run irv "$T/r_irv"; expect_refused irv CHECKER_REJECT
req "$T/r_irok" "$TXT_GCD" oscir-v1 "$T/dev04.oscir" gcd "u32 u32" u32 "$FX/gcd.in" "$T/g_gcd"
run irok "$T/r_irok"; expect_exec irok && expect_verified irok "$TXT_GCD" gcd_u32
# pins, signature, inputs, tier
req "$T/r_pin" "$TXT_GCD" omegese-v1 "$DEV/dev/dev04.omegese" gcd "u32 u32" u32 "$FX/gcd.in" "$T/g_gcd" pin_checker2_id="$(printf '0%.0s' $(seq 64))"
run pin "$T/r_pin"; expect_refused pin PIN_MISMATCH
req "$T/r_sig" "$TXT_GCD" omegese-v1 "$DEV/dev/dev04.omegese" gcd "u32 u32" u64 "$FX/gcd.in" "$T/g_gcd"
run sig "$T/r_sig"; expect_refused sig SIGNATURE_MISMATCH
printf '4294967296 1\n' > "$T/range.in"
req "$T/r_range" "$TXT_GCD" omegese-v1 "$DEV/dev/dev04.omegese" gcd "u32 u32" u32 "$T/range.in" "$T/g_gcd"
run range "$T/r_range"; expect_refused range INPUT_RANGE
grant "$T/g_few" "$TXT_GCD" pure 1 10 4
req "$T/r_many" "$TXT_GCD" omegese-v1 "$DEV/dev/dev04.omegese" gcd "u32 u32" u32 "$FX/gcd.in" "$T/g_few"
run many "$T/r_many"; expect_refused many INPUT_LIMIT
grant "$T/g_gpu" "$TXT_GCD" pure 1 10 64 gb10-gpu
req "$T/r_gpu" "$TXT_GCD" omegese-v1 "$DEV/dev/dev04.omegese" gcd "u32 u32" u32 "$FX/gcd.in" "$T/g_gpu" tier=gb10-gpu
run gpu "$T/r_gpu"; expect_refused gpu TIER_UNSUPPORTED
# the verifier refuses an oracle for another signature
expect_fail gcd "$TXT_GCD" collatz_steps_u32 ORACLE_SIGNATURE

echo "verified-program: $passes checks passed, $fails failed"
[ "$fails" -eq 0 ] && echo "VERIFIED_PROGRAM_PASS" || { echo "VERIFIED_PROGRAM_FAIL"; exit 1; }
