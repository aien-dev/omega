#!/bin/bash
# CAND-2 A7b: rebuild the R15 programs, then R15 silicon once. Guard, snapshots and step recorder as cand2_ladder.sh.
set -u
OMEGA_SHA=c62f47b5ac178311d7d8f91c4f52a298a1754ec8; CODE_SHA=79a805d162bfded8c5ce5a4c14f7c29e79025f39
PHYS_SHA=6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf
W=$HOME/workspace/cand3-campaign/cand2; O=$W/wt-omega-chip; P=$W/wt-physics-chip
OUT=$HOME/workspace/evidence-out/CAND2-LIVING-79a805d/A7b-R15
export PHYSICS_DIR=$P AIENOS_LOCK_REPO=$HOME/workspace/r16-survey/aienos ARGUS_REPO=$HOME/workspace/aienos-argus
cd "$O" || exit 2
eval "$(sed -n '/^snap() {/,/^}/p' "$W/cand2_ladder.sh")"
eval "$(sed -n '/^step() {/,/^}/p' "$W/cand2_ladder.sh")"
# guard as cand2_ladder.sh, except untracked files are allowed (A7's R15 run directory is preserved untracked)
[ "$(git -C "$O" rev-parse HEAD)" = "$OMEGA_SHA" ] || { echo "omega HEAD wrong"; exit 2; }
[ "$(git -C "$P" rev-parse HEAD)" = "$PHYS_SHA" ] || { echo "physics HEAD wrong"; exit 2; }
[ -z "$(git -C "$O" status --porcelain --untracked-files=no)" ] || { echo "omega tracked tree dirty"; exit 2; }
git -C "$O" diff --quiet $CODE_SHA HEAD -- . ":(exclude)spec/r16-orchestrator-retirement-map.md" ":(glob,exclude)**/.crumb" ":(exclude).crumb" || { echo "code differs"; exit 2; }
mkdir -p "$OUT"
step R15-build make r15-perf-silicon
sha256sum build/rx_r15_perf_silicon build/rx_r15_perf_silicon_nodigest build/r15_reduce > "$OUT/R15-build/executables.sha256" 2>&1
step R15-silicon tools/r15_qualify.sh silicon
ls -td evidence/R15/raw/*-c62f47b5ac17-silicon | head -1 > "$OUT/R15-silicon/run-dir.txt"
