#!/bin/bash
# Chip gates at omega main for WIDEPF (pattern of evidence/OWN30B-REQUAL-MAIN-710d438c-20261015/omega-gates.sh)
set -u
D=$HOME/workspace/investigations/2026-10-15-WIDEPF; O=$D/omega-main; P=$HOME/workspace/pkv514-pins/physics; E=$D/runs/gates
mkdir -p "$E"; S=$E/summary.txt
echo "omega $(git -C $O rev-parse HEAD) dirty=$(git -C $O status --porcelain --untracked-files=no | wc -l) physics $(git -C $P rev-parse HEAD) dirty=$(git -C $P status --porcelain | wc -l) start $(date -u +%FT%TZ)" >> "$S"
g() { local name=$1; shift; local t0; t0=$(date -u +%FT%TZ)
  ( cd "$O" && quietlock hold --owner WIDEPF --minutes 20 --reason "omega460 gate $name at omega main" -- flock /tmp/aien-gb10.lock "$@" ) > "$E/$name.out" 2>&1
  local rc=$?
  echo "$name rc=$rc start=$t0 end=$(date -u +%FT%TZ) verdict: $(grep -E 'PASS|FAIL' "$E/$name.out" | tail -1)" >> "$S"; }
g matmul_api   tools/run_gpu_matmul_api_chip.sh   "$E/matmul_api"   "$P"
g prefill_attn tools/run_gpu_prefill_attn_chip.sh "$E/prefill_attn" "$P"
g l1g          env PHYS="$P" OUT_DIR="$D/l1g-build" tools/run_l1g_chip.sh "$E/l1g" qmma pfgemm qgemm
echo "end $(date -u +%FT%TZ)" >> "$S"
