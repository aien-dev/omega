#!/bin/bash
set -u
D=/home/drakestapleton/workspace/investigations/2026-10-16-kt1-speed
name=$1; bin=$2; shift 2
out="$D/runs/$name"
test ! -e "$out.json" || exit 2
{ date -u +%FT%TZ; sha256sum "$bin"; nvidia-smi --query-gpu=clocks.sm,clocks.mem,temperature.gpu,utilization.gpu --format=csv,noheader; } > "$out.meta"
"$bin" --mutant 2 --no-bw --out "$out-negative.json" > "$out-negative.log" 2>&1
neg=$?
echo "negative_exit=$neg" >> "$out.meta"
[ "$neg" = 1 ] && jq -e '.forced_mutant_2_differing > 0' "$out-negative.json" >/dev/null || exit 3
"$bin" --bw-only --reps 5 "$@" --out "$out.json" > "$out.log" 2>&1
rc=$?
echo "exit=$rc" >> "$out.meta"
jq -c '{floor:.launch_floor_in_list, cold:[.cold[]? | {n,k,median_ns,minus_floor_of_practical_237_2,pass}]}' "$out.json"
echo "$name exit=$rc negative=$neg"
