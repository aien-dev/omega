#!/bin/sh
# Tests for est6dev and est6opchar (pre-freeze development tools). Reads only the committed v5 D1
# development folder; every held-out path must be refused before any file is opened.
# usage: test_est6dev.sh <est6dev> <est6opchar>
set -eu
E=${1:?est6dev binary}
O=${2:?est6opchar binary}
D1=evidence/EST5/raw/20261001T153156Z-est5-fit-silicon
fail=0
chk() { if [ "$1" = ok ]; then :; else echo "FAIL $2"; fail=1; fi; }

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

if [ "$fail" = 0 ]; then echo "test-est6dev: PASS"; else exit 1; fi
