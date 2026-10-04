#!/bin/sh
# extract.sh: read the preserved GB10 campaign evidence (read-only) and print the
# label-free TSV that pd1_convert consumes. Nothing is synthesised: a field that
# was not recorded is emitted as the "not recorded" sentinel.
#
# The join tables below are the ONLY place a structural label (arm, directory
# prefix, variant name) is turned into an opaque knob value. They are the PD-1
# answer key for knob q0 and must never be shipped to the learner lane; this
# script lives with the harness, not with the learner.
#
# Column meaning (opaque to the learner):
#   src  campaign id: 1 per-launch probe, 2 gate A/B under load, 3 lifecycle A/B, 4 single-trial variants
#   q0   knob derived from arm / directory prefix (0 or 1; 255 not recorded)
#   q1   variant ordinal where a variant field exists (255 otherwise)
#   q2   q3  not recorded in any campaign (255)
#   q4   ordinal position inside the run series (launch index or rep number)
#   q5   not recorded (INT64_MIN)
#   r0   latency in micro units (ms * 1000), -1 if not recorded
#   r1   outcome class: 0 ok, 1 fail
#   cov  host load average * 1000 at campaign start when recorded, else 4294967295
set -eu
EV=${EV:-$HOME/workspace/evidence-out}
NA8=255; NA32=4294967295; NA64=-9223372036854775808

# ---- src 1: E1-STALL-PROBE, per-launch marker2_ms in launch order (stderr) ----
P=$EV/E1-STALL-PROBE/20261002T193710Z
for arm in A B; do
  case $arm in A) q0=1;; B) q0=0;; esac          # join: arm A = default allocation, arm B = alternative build define
  grep '^GB10_PROBE ' "$P/arm_$arm.stderr" | awk -v q0=$q0 -v na8=$NA8 -v na32=$NA32 -v na64=$NA64 '
    { for (i=1;i<=NF;i++) if ($i ~ /^marker2_ms=/) { split($i,a,"="); ms=a[2] }
      printf "1\t%d\t%d\t%d\t%d\t%d\t%s\t%d\t0\t%s\n", q0, na8, na8, na8, NR-1, na64, int(ms*1000+0.5), na32 }'
done

# ---- src 2: M18-UNCACHED A/B, per-run rc under a documented host load ----
M=$EV/M18-UNCACHED-20261003
load=$(grep -m1 -oE 'load average: [0-9.]+' "$M/campaign.txt" | awk '{print int($3*1000+0.5)}')
[ -n "$load" ] || load=$NA32
for d in "$M"/baseline-m18-L* "$M"/fix-m18-L*; do
  case $(basename "$d") in baseline-*) q0=1;; fix-*) q0=0;; esac   # join: directory prefix
  awk -v q0=$q0 -v na8=$NA8 -v na64=$NA64 -v cov=$load '
    /^rep=/ { rep=$1; sub("rep=","",rep); rc=$NF; sub("rc=","",rc); cls=(rc=="0")?0:1
      printf "2\t%d\t%d\t%d\t%d\t%d\t%s\t-1\t%d\t%s\n", q0, na8, na8, na8, rep, na64, cls, cov }' "$d/exit-codes.txt"
done

# ---- src 3: WORLD-UNCACHED, per-run rc and wait_ms where a gate printed it ----
W=$EV/WORLD-UNCACHED-20261003
for d in "$W"/red-001 "$W"/green-001 "$W"/baseline-m18-* "$W"/fix-m18-*; do
  [ -f "$d/exit-codes.txt" ] || continue
  case $(basename "$d") in red-*|baseline-*) q0=1;; green-*|fix-*) q0=0;; esac   # join: directory prefix
  ms=$(cat "$d"/*.log 2>/dev/null | grep -m1 -oE 'wait_ms=[0-9.]+' | cut -d= -f2 || true)
  if [ -n "$ms" ]; then r0=$(awk -v m="$ms" 'BEGIN{print int(m*1000+0.5)}'); else r0=-1; fi
  awk -v q0=$q0 -v na8=$NA8 -v na64=$NA64 -v na32=$NA32 -v r0=$r0 '
    /^rep=/ { rep=$1; sub("rep=","",rep); rc=$NF; sub("rc=","",rc); cls=(rc=="0")?0:1
      printf "3\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%d\t%s\n", q0, na8, na8, na8, rep, na64, (rep==1?r0:-1), cls, na32 }' "$d/exit-codes.txt"
done

# ---- src 4: CHIPWAIT-ASTRA result.json, one trial per variant; allocation attribute NOT recorded ----
A=$EV/CHIPWAIT-ASTRA-20261003/result.json
grep -oE '"variant":"[a-z0-9]+","result":"[A-Z]+"' "$A" | awk -F'"' -v na8=$NA8 -v na64=$NA64 -v na32=$NA32 '
  { v=$4; res=$8; cls=(res=="PASS")?0:1
    printf "4\t%d\t%d\t%d\t%d\t%d\t%s\t-1\t%d\t%s\n", na8, NR-1, na8, na8, NR, na64, cls, na32 }'
