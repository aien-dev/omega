#!/bin/bash
# diagnostic wrapper: run-one.sh with the camera csv prefix set to the run name
set -u
D=/home/drakestapleton/workspace/investigations/2026-10-16-kt1-speed
name=$1; shift
export KT1_DIAG_PREFIX="$D/runs/$name"
nvidia-smi --query-gpu=timestamp,clocks.sm,temperature.gpu,utilization.gpu,power.draw --format=csv,noheader -lms 20 > "$D/runs/$name-smi.csv" 2>&1 &
smi=$!
"$D/run-one.sh" "$name" "$@"
rc=$?
kill $smi 2>/dev/null; wait $smi 2>/dev/null
exit $rc
