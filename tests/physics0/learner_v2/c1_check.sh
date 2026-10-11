#!/bin/bash
# C1 acceptance on a receipt made by `make physics0-learner-v2-dev` (lines P0V2F learner=v1|v2 ...).
# Acceptance (seeds 1-5): v2 hidden flag fires on L6 and stays silent on L0-L5 and null; v2 test errors on L0-L5/null are not worse than v1.
# Also reported (not gated): the same over seeds 1-25, and v1's own flag rate.
f=$1
key() { awk -v L="$2" -v V="$3" -v LO="$4" -v HI="$5" '$1=="P0V2F" && $2=="learner="L && $3=="level="V { for(i=1;i<=NF;i++){split($i,a,"=");k[a[1]]=a[2]} if (k["seed"]+0>=LO && k["seed"]+0<=HI) print k["seed"], k["'$1'"] }' "$f"; }
fail=0
for lv in L0 L1 L2 L3 L4 L5 L6 null; do
  for rng in "1 5" "1 25"; do
    set -- $rng
    n1=$(key hidden v1 $lv $1 $2 | awk '{s+=$2} END{print s+0}'); n2=$(key hidden v2 $lv $1 $2 | awk '{s+=$2} END{print s+0}')
    tot=$(key hidden v2 $lv $1 $2 | wc -l)
    worse=$(paste -d' ' <(key test_roll20 v1 $lv $1 $2) <(key test_roll20 v2 $lv $1 $2) <(key test_one v1 $lv $1 $2) <(key test_one v2 $lv $1 $2) | awk '$4>$2 || $8>$6 {n++} END{print n+0}')
    echo "C1-CHECK level=$lv seeds=$1..$2 n=$tot hidden_fired_v1=$n1 hidden_fired_v2=$n2 v2_test_error_worse_than_v1=$worse"
    if [ "$1 $2" = "1 5" ]; then
      if [ $lv = L6 ]; then [ "$n2" = "$tot" ] || fail=1; else [ "$n2" = 0 ] && [ "$worse" = 0 ] || fail=1; fi
    fi
  done
done
if [ $fail = 0 ]; then echo "C1-ACCEPT: PASS (v2 flag fires on L6 seeds 1-5, silent on L0-L5 and null; no worse test error on L0-L5 and null)"; else echo "C1-ACCEPT: FAIL"; fi
exit $fail
