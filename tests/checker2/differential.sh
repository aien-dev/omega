#!/bin/sh
# Black-box differential: the producer's checker (oscc, run as a program; its source is not read by
# checker2) against checker2, over every OSC unit in the repo plus the generated valid units.
#   oscc accepts  -> osc_emit_ir must write the same bytes (same ir_sha256) and checker2 must ACCEPT
#   oscc rejects  -> the unit never becomes bytes (front-end refusal); osc_emit_ir must agree
# Disagreement fails. Gate (omega#446): at least 100 units accepted by both checkers.
# The accepted IR files are left in <work>/ir for test_c2_ir.
# Usage: sh tests/checker2/differential.sh <oscc> <osc_emit_ir> <checker2> <work-dir>
set -u
OSCC=$1; EMIT=$2; C2=$3; WORK=$4
rm -rf "$WORK"
mkdir -p "$WORK/ir" "$WORK/gen"
sh tests/checker2/gen_valid.sh "$WORK/gen" 48 || { echo "differential: generator failed"; exit 1; }

both=0; fe_reject=0; neg=0; bad=0
check_unit() { # <file.osc> <expect: any|reject>
    src=$1; expect=$2
    name=$(echo "$src" | sed 's|^\./||; s|/|_|g; s|\.osc$||')
    out=$("$OSCC" "$src" 2>/dev/null); orc=$?
    erc=0; eout=$("$EMIT" "$src" "$WORK/ir/$name.ir" 2>/dev/null) || erc=$?
    if [ "$orc" -ne 0 ]; then
        fe_reject=$((fe_reject + 1))
        [ "$expect" = reject ] && neg=$((neg + 1))
        if [ "$erc" -eq 0 ]; then echo "DISAGREE $src: oscc rejects, producer library accepts"; bad=$((bad + 1)); fi
        rm -f "$WORK/ir/$name.ir"
        return
    fi
    if [ "$expect" = reject ]; then echo "DISAGREE $src: known-bad unit accepted by oscc"; bad=$((bad + 1)); return; fi
    osha=$(echo "$out" | sed -n 's/^ir_sha256=//p')
    esha=$(echo "$eout" | sed -n 's/.*ir_sha256=//p')
    if [ "$erc" -ne 0 ] || [ -z "$osha" ] || [ "$osha" != "$esha" ]; then
        echo "DISAGREE $src: emitted bytes are not oscc's bytes ($osha vs $esha)"; bad=$((bad + 1)); return
    fi
    c=$("$C2" check "$WORK/ir/$name.ir")
    case "$c" in
    "checker2=ACCEPT ir_sha256=$osha") both=$((both + 1)) ;;
    *) echo "DISAGREE $src: oscc accepts, $c"; bad=$((bad + 1)) ;;
    esac
}

for f in tests/compiler/progs/*.osc tests/compiler/slices/*.osc src/osh/*.osc tests/physics0/osc/*.osc "$WORK"/gen/*.osc; do
    [ -f "$f" ] && check_unit "$f" any
done
for f in tests/compiler/neg/*.osc; do
    [ -f "$f" ] && check_unit "$f" reject
done

# negative control: the comparison must be able to fail. A truncated copy of an accepted unit has
# to be refused by checker2; otherwise this script could never report a disagreement.
first=$(ls "$WORK"/ir/*.ir | head -n 1)
size=$(wc -c < "$first")
head -c $((size - 1)) "$first" > "$WORK/control.ir"
case $("$C2" check "$WORK/control.ir") in
checker2=REJECT*) ;;
*) echo "differential: negative control failed (truncated unit accepted)"; bad=$((bad + 1)) ;;
esac

echo "differential: accepted by both=$both front-end refusals=$fe_reject (known-bad neg units=$neg) disagreements=$bad"
[ "$bad" -eq 0 ] || exit 1
[ "$both" -ge 100 ] || { echo "differential: gate needs >= 100 units accepted by both"; exit 1; }
[ "$neg" -ge 100 ] || { echo "differential: expected the known-bad neg units to be refused"; exit 1; }
exit 0
