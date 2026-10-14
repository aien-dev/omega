#!/bin/sh
# LRAT gate (omega#446): bounded integer programs proved trap-free by checker2.
#   For every tests/checker2/lrat/*.osc (header lines "// prove: <func>", "// expect: holds|trap"):
#     producer emits the IR bytes; checker2 bit-blasts the property NO-TRAP of <func> to CNF;
#     the UNTRUSTED solver c2sat runs under a timeout;
#     holds -> c2sat must answer UNSAT (exit 20) and checker2 must replay the LRAT proof (VERIFIED);
#     trap  -> c2sat must answer SAT (exit 10): the program really can trap.
#   Any disagreement with the expectation, any timeout or any refused proof fails the gate.
#   At least 20 "holds" programs must end VERIFIED.
# Negative controls (each must be REFUSED, or the gate fails):
#   every proof with its final (empty clause) line removed;
#   every proof whose final step has its hints removed;
#   the abs8 proof replayed against the unguarded abs8 IR (a proof binds to one program).
# Receipts (evidence class 2): issue checks the proofs; verify replays them and refuses missing,
#   corrupted, substituted or misapplied proofs, and proofs offered for a class 1 receipt.
# Replay: c2sat is run twice per program; the two proofs must be byte-identical.
# Usage: lrat.sh <osc_emit_ir> <checker2> <c2sat> <work-dir> [timeout-seconds]
set -u
EMIT=$1 C2=$2 SAT=$3 W=$4 T=${5:-60}
D=$(dirname "$0")/lrat
rm -rf "$W" && mkdir -p "$W" || exit 2
fail=0 verified=0 traps=0 ctl=0
bad() { echo "lrat: FAIL $*"; fail=$((fail + 1)); }

for src in "$D"/*.osc; do
    b=$(basename "$src" .osc)
    f=$(sed -n 's,^// prove: *,,p' "$src")
    e=$(sed -n 's,^// expect: *,,p' "$src")
    [ -n "$f" ] && [ -n "$e" ] || { bad "$b: missing prove/expect header"; continue; }
    "$EMIT" "$src" "$W/$b.ir" >/dev/null 2>&1 || { bad "$b: producer refused"; continue; }
    "$C2" prove-cnf "$W/$b.ir" "$f" "$W/$b.cnf" >"$W/$b.info" 2>&1 || { bad "$b: prove-cnf: $(cat "$W/$b.info")"; continue; }
    timeout "$T" "$SAT" "$W/$b.cnf" "$W/$b.lrat" >"$W/$b.sat" 2>&1
    r=$?
    case "$e:$r" in
    holds:20)
        timeout "$T" "$SAT" "$W/$b.cnf" "$W/$b.lrat2" >/dev/null 2>&1
        cmp -s "$W/$b.lrat" "$W/$b.lrat2" || bad "$b: proof not reproducible"
        out=$("$C2" prove-check "$W/$b.ir" "$f" "$W/$b.lrat" 2>&1)
        case "$out" in
        lrat=VERIFIED*) verified=$((verified + 1)); echo "lrat: $b $f VERIFIED $(grep -o 'vars=[0-9]* clauses=[0-9]*' "$W/$b.info") steps=${out##*steps=}" ;;
        *) bad "$b: proof refused: $out"; continue ;;
        esac
        # control 1: drop the final line (the empty clause)
        sed '$d' "$W/$b.lrat" >"$W/$b.cut"
        "$C2" prove-check "$W/$b.ir" "$f" "$W/$b.cut" 2>&1 | grep -q '^lrat=REFUSED' && ctl=$((ctl + 1)) || bad "$b: truncated proof accepted"
        # control 2: final step without hints
        sed '$s/^\([0-9]*\) 0 .*/\1 0 0/' "$W/$b.lrat" >"$W/$b.nohint"
        "$C2" prove-check "$W/$b.ir" "$f" "$W/$b.nohint" 2>&1 | grep -q '^lrat=REFUSED' && ctl=$((ctl + 1)) || bad "$b: hintless proof accepted"
        ;;
    trap:10) traps=$((traps + 1)); echo "lrat: $b $f can trap (counterexample found, as expected)" ;;
    *:124) bad "$b: solver timeout after ${T}s" ;;
    *) bad "$b: expected $e, solver exit $r" ;;
    esac
done

# control 3: a valid proof for one program must not verify another program
if [ -f "$W/p01_abs8.lrat" ] && [ -f "$W/n01_abs8_unguarded.ir" ]; then
    "$C2" prove-check "$W/n01_abs8_unguarded.ir" abs8 "$W/p01_abs8.lrat" 2>&1 | grep -q '^lrat=REFUSED' && ctl=$((ctl + 1)) || bad "abs8 proof verified the unguarded program"
else
    bad "cross-program control inputs missing"
fi

# receipts with LRAT evidence (class 2): proofs are checked at issue and replayed at verify
R=$W/rcpt
mkdir -p "$R/out" "$R/out1"
printf 'exe\n' >"$R/exe"
iss() { "$C2" receipt issue "$1" "$R/exe" --target lrat-gate --epoch 5 --expiry 9 --out-dir "$2" "$3" "$4" >/dev/null 2>&1; }
want() { # want <expected receipt= line> <name> <verify args...>
    w=$1 n=$2
    shift 2
    got=$("$C2" receipt verify "$@" 2>/dev/null | grep '^receipt=')
    [ "$got" = "receipt=$w" ] && ctl=$((ctl + 1)) || bad "receipt $n: got '$got', want receipt=$w"
}
if iss "$W/p01_abs8.ir" "$R/out" --lrat "abs8=$W/p01_abs8.lrat"; then
    rc2=$(ls "$R/out"/*.rcpt)
    V="$rc2 $W/p01_abs8.ir $R/exe --target lrat-gate --epoch 6"
    { cat "$W/p01_abs8.lrat"; echo "c same proof, different bytes"; } >"$W/p01_abs8.other"
    want VALID class2-replayed $V --lrat "abs8=$W/p01_abs8.lrat"
    want LRAT-PROOFS-REQUIRED class2-without-proofs $V
    want LRAT-REPLAY-FAILED class2-bad-proof $V --lrat "abs8=$W/p01_abs8.cut"
    want LRAT-REQUIREMENTS-MISMATCH class2-other-proof $V --lrat "abs8=$W/p01_abs8.other"
    want PROGRAM_MISMATCH class2-wrong-program "$rc2" "$W/n01_abs8_unguarded.ir" "$R/exe" --target lrat-gate --epoch 6 --lrat "abs8=$W/p01_abs8.lrat"
else
    bad "receipt issue with a verified proof failed"
fi
iss "$W/n01_abs8_unguarded.ir" "$R/out" --lrat "abs8=$W/p01_abs8.lrat" && bad "receipt issued for a program its proof does not cover" || ctl=$((ctl + 1))
if iss "$W/p02_avg_u8.ir" "$R/out1" --input "$R/exe"; then
    want LRAT-NOT-CLAIMED class1-with-proof "$(ls "$R/out1"/*.rcpt)" "$W/p02_avg_u8.ir" "$R/exe" --target lrat-gate --epoch 6 --lrat "avg=$W/p02_avg_u8.lrat"
else
    bad "class 1 receipt issue failed"
fi

echo "lrat: verified=$verified expected-traps=$traps controls-refused=$ctl failures=$fail"
[ "$verified" -ge 20 ] || bad "only $verified programs verified (need 20)"
[ "$fail" -eq 0 ] || exit 1
echo "lrat: PASS"
