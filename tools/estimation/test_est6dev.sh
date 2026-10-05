#!/bin/sh
# Tests for est6dev and est6opchar (pre-freeze development tools). Reads only the committed v5 D1
# development folder; every held-out path must be refused before any file is opened.
# usage: test_est6dev.sh <est6dev> <est6opchar>
set -eu
E=${1:?est6dev binary}
O=${2:?est6opchar binary}
D1=evidence/EST5/raw/20261001T153156Z-est5-fit-silicon
fail=0
n=0
chk() { n=$((n+1)); if [ "$1" = ok ]; then :; else echo "FAIL $2"; fail=1; fi; }

# 1. held-out refusals (path tags and the v5 D2 date tag), exit status 2, nothing opened
for p in evidence/EST5/raw/20261005T112838Z-est5-heldout-silicon evidence/X/raw/zz-est6-heldout-silicon evidence/EST3C/raw/zz-est3c-heldout-q; do
  rc=0
  "$E" g1pit --raw "$p/machine-state.ndjson" --marks "$p/machine-state-marks.txt" --dyn 1 --q 10000 --lam 0.1 --nu 1.25 --c 0.7 >/dev/null 2>&1 || rc=$?
  [ "$rc" = 2 ] && chk ok x || chk bad "g1pit did not refuse held-out path $p (rc $rc)"
  rc=0
  "$E" g1s --raw "$p/machine-state.ndjson" --marks "$p/machine-state-marks.txt" --dyn 1 --q 10000 --lam 0.1 --nu 1.25 >/dev/null 2>&1 || rc=$?
  [ "$rc" = 2 ] && chk ok x || chk bad "g1s did not refuse held-out path $p (rc $rc)"
done

# 1b. the D2 SHA backstops equal the committed d2.sha256 (hash list only; no D2 file is opened)
D2S=docs/estimation/receipts/est-v5/d2.sha256
for pair in "D2_RAW_SHA machine-state.ndjson" "D2_MARKS_SHA machine-state-marks.txt"; do
  set -- $pair
  want=$(awk -v n="$2" '$2 == n { print $1 }' "$D2S")
  got=$(sed -n "s/^#define $1 \"\([0-9a-f]*\)\"$/\1/p" tools/estimation/est6dev.c)
  [ -n "$want" ] && [ "$want" = "$got" ] && chk ok x || chk bad "$1 in est6dev.c ($got) != d2.sha256 $2 ($want)"
done

# 2. schedule generator reproduces the v5 protocol text (seed 0xE5C5D1: idle 406, L6 929, L12 655, L18 710)
"$E" sched 0xE5C5D1 2700 | grep -q '^segments 39 idle 406 L6 929 L12 655 L18 710$' && chk ok x || chk bad "schedule totals for 0xE5C5D1"
"$E" sched 0xE5C5D1 2700 | grep -q '^schedule 0 0 74$' && chk ok x || chk bad "schedule first segment 0xE5C5D1"
# ... and the recorded schedule.txt of the v5 D1 folder (seed 0xE5C5D2) line for line
"$E" sched 0xE5C5D2 2700 | grep '^schedule' | cut -d' ' -f2- > "${TMPDIR:-/tmp}/e6dev.sched.$$"
grep '^schedule' $D1/schedule.txt | cut -d' ' -f2- | cmp -s - "${TMPDIR:-/tmp}/e6dev.sched.$$" && chk ok x || chk bad "schedule 0xE5C5D2 vs D1 schedule.txt"
rm -f "${TMPDIR:-/tmp}/e6dev.sched.$$"
# balance rule: 0xE5C5D1 fails (L6 929 > 900), pick moves +1 until it passes
"$E" sched 0xE5C5D1 2700 | grep -q '^balance FAIL$' && chk ok x || chk bad "balance FAIL expected"
"$E" pick 0xE6C6D1 2700 | grep -q '^picked 0xe6c6d3 after 2 failures$' && chk ok x || chk bad "pick 0xE6C6D1"


# 2b. schedule rule SR-1 (domain separated by run label) against the old chained rule
PLAN="2700 F=0xE6C6D1 H1=0xD6E6C7 H2=0xD6E6C8"
T6=${TMPDIR:-/tmp}/e6dev.plan.$$
# RED: the old rule (seed + 1 chain) sends H1 and H2 to the same seed and schedule; the plan tool refuses it
rc=0; "$E" plan6 --rule old $PLAN > $T6.old || rc=$?
[ "$rc" = 1 ] && grep -q '^COLLISION H1 H2 effective 0xd6e6cb$' $T6.old && chk ok x || chk bad "old rule must reproduce the H1/H2 collision at 0xd6e6cb (rc $rc)"
grep '^label H1 ' $T6.old | grep -q 'segments 41 idle 522 L6 572 L12 857 L18 749' && chk ok x || chk bad "old rule H1 schedule totals"
# GREEN: SR-1 gives distinct effective seeds and distinct schedules
rc=0; "$E" plan6 $PLAN > $T6.a || rc=$?
[ "$rc" = 0 ] && grep -q '^distinct PASS$' $T6.a && chk ok x || chk bad "SR-1 plan not distinct (rc $rc)"
h1=$(grep '^schedule H1 ' $T6.a | cut -d' ' -f3-); h2=$(grep '^schedule H2 ' $T6.a | cut -d' ' -f3-)
[ -n "$h1" ] && [ -n "$h2" ] && [ "$h1" != "$h2" ] && chk ok x || chk bad "SR-1 H1 and H2 schedules equal"
e1=$(grep '^label H1 ' $T6.a | cut -d' ' -f6); e2=$(grep '^label H2 ' $T6.a | cut -d' ' -f6)
[ "$e1" != "$e2" ] && chk ok x || chk bad "SR-1 H1 and H2 effective seeds equal"
# determinism: same input, same output, and a label's schedule does not depend on the other labels in the plan
"$E" plan6 $PLAN > $T6.b; cmp -s $T6.a $T6.b && chk ok x || chk bad "SR-1 plan not deterministic"
"$E" plan6 2700 H2=0xD6E6C8 | grep '^schedule H2 ' > $T6.c
grep '^schedule H2 ' $T6.a | cmp -s - $T6.c && chk ok x || chk bad "SR-1 H2 schedule depends on the other labels in the plan"
"$E" plan6 2700 A=0xD6E6C7 B=0xD6E6C7 | grep -q '^distinct PASS$' && chk ok x || chk bad "domain separation: same declared seed, two labels"
# balance and structure properties hold for every label (totals sum to 2700, each level 400..900, segments 20..120 but a clipped last)
for L in F H1 H2; do
  grep "^label $L " $T6.a | grep -q 'balance PASS' && chk ok x || chk bad "balance $L"
  grep "^schedule $L " $T6.a | awk '{ d[NR]=$5; lv[$4]+=$5; s+=$5 } END { ok=(s==2700); for (i=1;i<=NR;i++) { if (d[i]>120 || d[i]<1) ok=0; if (d[i]<20 && i<NR) ok=0 } for (k in lv) if (lv[k]<400 || lv[k]>900) ok=0; exit ok?0:1 }' && chk ok x || chk bad "structure $L (sum 2700, levels 400..900, segments 20..120, last may be clipped)"
done
rm -f $T6.old $T6.a $T6.b $T6.c

# 3. G1 on v5 D1 with the v5 parameters reproduces params.txt (n 2652, bin0 0.075981, cov95 0.952950, mean z 0.046045)
out=$("$E" g1pit --raw $D1/machine-state.ndjson --marks $D1/machine-state-marks.txt --dyn 1 --q 10000 --lam 0.1 --nu 1.25 --c 0.7 --boot 200)
echo "$out" | grep -q '^n_scored 2652 unscorable 0$' && chk ok x || chk bad "n_scored"
echo "$out" | grep -q '^stat pit_bin0 0.075981 ' && chk ok x || chk bad "pit_bin0 vs params.txt"
echo "$out" | grep -q '^stat coverage95 0.952950 ' && chk ok x || chk bad "coverage95 vs params.txt"
echo "$out" | grep -q '^mean_z_midpit 0.046045$' && chk ok x || chk bad "mean z vs params.txt"
# deterministic bootstrap
out2=$("$E" g1pit --raw $D1/machine-state.ndjson --marks $D1/machine-state-marks.txt --dyn 1 --q 10000 --lam 0.1 --nu 1.25 --c 0.7 --boot 200)
[ "$out" = "$out2" ] && chk ok x || chk bad "bootstrap not deterministic"

# 4. G1S at kappa 1.0 reproduces the G1 in-sample log score (-1.962811)
"$E" g1s --raw $D1/machine-state.ndjson --marks $D1/machine-state-marks.txt --dyn 1 --q 10000 --lam 0.1 --nu 1.25 | grep -q '^insample kappa 1.0 best_c 0.70 mean_logscore -1.962811 ' && chk ok x || chk bad "g1s kappa 1.0 log score"

# 5. simulation tool self test
"$O" selftest | grep -q 'selftest: PASS' && chk ok x || chk bad "est6opchar selftest"
a=$("$O" run --case a --reps 50 --boot 300) ; b=$("$O" run --case a --reps 50 --boot 300)
[ "$a" = "$b" ] && chk ok x || chk bad "est6opchar not deterministic"

if [ "$fail" = 0 ]; then echo "test-est6dev: PASS ($n checks)"; else exit 1; fi
