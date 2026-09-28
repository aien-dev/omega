#!/bin/bash
# burst.sh -- follow-up to run.sh: is the X925 slowdown under sustained load a
# real clock reduction (on-CPU time stays ~100%, cycles fall) or core sharing
# (on-CPU time falls)? Does reading the SPBM telemetry perturb it? Does a rest
# restore the full-speed burst? Same quiet-machine protocol as run.sh.
set -u
OUT=${1:?out dir}
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$HERE/hwchar
mkdir -p "$OUT"
QUIET=$HOME/workspace/.spark-quiet
say() { echo "$(date +%H:%M:%S) $*" | tee -a "$OUT/progress.log"; }
finish() {
    sudo -n sysctl -q kernel.perf_event_paranoid=4 >/dev/null 2>&1
    echo "paranoid restored: $(cat /proc/sys/kernel/perf_event_paranoid)" >> "$OUT/progress.log"
    rm -f "$QUIET"
    say "quiet marker removed; done"
}
trap finish EXIT
echo "R15 hardware characterization (burst follow-up) $(date -u +%FT%TZ) by the R15 session" > "$QUIET"
say "quiet marker up; waiting for load < 1.0"
for i in $(seq 1 90); do
    awk -v l="$(cut -d' ' -f1 /proc/loadavg)" 'BEGIN{exit !(l < 1.0)}' && break
    sleep 10
done
say "load at start: $(cat /proc/loadavg)"
sudo -n sysctl -q kernel.perf_event_paranoid=2 >/dev/null 2>&1
say "A: X925 ref 20 s, SPBM read each second"
"$BIN" sustain 7 ref 20 > "$OUT/a-spbm.jsonl"
say "rest 20 s"; sleep 20
say "B: X925 ref 20 s, SPBM never read"
HWCHAR_NO_SPBM=1 "$BIN" sustain 7 ref 20 > "$OUT/b-nospbm.jsonl"
say "rest 20 s"; sleep 20
say "C: X925 quad4 20 s on another X925 core (cpu 16)"
"$BIN" sustain 16 quad4 20 > "$OUT/c-cpu16.jsonl"
say "D: A725 ref 20 s (control)"
"$BIN" sustain 2 ref 20 > "$OUT/d-a725.jsonl"
say "load at end: $(cat /proc/loadavg)"
