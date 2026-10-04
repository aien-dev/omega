#!/bin/bash
# tools/probe_gpu_open.sh OUT_DIR [N]
# FB-1 cut 4b flake investigation: does the FIRST chip call of a fresh process fail
# in the device-open path (seen twice: cut 1b run 2, cut 3c attempt 1)? Each sample
# is a fresh process (build/gpu_session_probe). Variants, N samples each, in order:
#   baseline      one probe at a time, idle gap 200 ms between processes
#   back_to_back  the next probe starts the instant the previous one exits (the sighting:
#                 "another chip job had just used the chip")
#   overlap       a holder keeps a device open (hold 300 ms) while the probe opens its own
#   after_latch   NOT RUN (would need a deliberate stall; never leave the chip uncertain)
# Negative control first: the probe must report a failure when it cannot open the
# control nodes (file-descriptor limit 3 -> open() fails), with the stage named.
# Run through the heavy queue only (lanes.sh queue --heavy); never kill it.
set -u
OUT=${1:?out dir}; N=${2:-30}
cd "$(dirname "$0")/.." || exit 2
P=${PHYSICS_DIR:-../physics}
mkdir -p "$OUT"
make PHYSICS_DIR="$P" build/gpu_session_probe > "$OUT/build.log" 2>&1 || { echo "PROBE BUILD FAIL"; exit 1; }
PR=./build/gpu_session_probe
LOG="$OUT/probe.log"; : > "$LOG"
{ uname -a; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null; echo "omega $(git rev-parse HEAD) physics $(git -C "$P" rev-parse HEAD)"; date -u +%FT%TZ; } > "$OUT/device.txt"
echo "## negative_control (fd limit 3: /dev/nvidiactl cannot be opened)" | tee -a "$LOG"
( ulimit -n 3; $PR --label negative_control ) 2>&1 | tee -a "$LOG"
neg_rc=${PIPESTATUS[0]}
echo "negative_control exit=$neg_rc (must be non-zero with a named stage)" | tee -a "$LOG"
fails=0; total=0
run_variant() { # name
  local name=$1 f=0
  echo "## $name N=$N" | tee -a "$LOG"
  for i in $(seq 1 "$N"); do
    if [ "$name" = overlap ]; then $PR --label holder --hold-ms 300 >> "$LOG" 2>&1 & sleep 0.05; fi
    $PR --label "$name#$i" 2>&1 | tee -a "$LOG" | grep -q 'rc=OK' || f=$((f+1))
    [ "$name" = overlap ] && wait
    [ "$name" = baseline ] && sleep 0.2
  done
  echo "$name: failures $f / $N" | tee -a "$LOG"
  fails=$((fails+f)); total=$((total+N))
}
run_variant baseline
run_variant back_to_back
run_variant overlap
grep -c 'rc=OK' "$LOG" > "$OUT/ok_count.txt"
grep 'rc=' "$LOG" | grep -v 'rc=OK' | grep -v negative_control > "$OUT/failures.txt"
echo "end $(date -u +%FT%TZ)" >> "$OUT/device.txt"
cp tests/gpu_session_probe.c src/omega_gpu_session.c tools/probe_gpu_open.sh "$OUT/"
(cd "$OUT" && sha256sum -- * > SHA256SUMS)
echo "PROBE_GPU_OPEN: negative_control_exit=$neg_rc first_call_failures=$fails/$total (details $OUT/failures.txt)"
[ "$neg_rc" != 0 ] || { echo "PROBE_GPU_OPEN: INSTRUMENT FAIL (negative control passed)"; exit 3; }
