#!/bin/bash
# CAND-3 G7b replacement attempt. Declared in DECLARED-ATTEMPT-G7b.md before it ran. Never kills, never retries, runs once.
# usage: cand3_r15c.sh phase1   (reader facts, R15 build, section-14 correctness reruns, R15 silicon, receipt-A)
#        cand3_g7b.sh phase2 <final R15 receipt path>   (R16 harness once with R16_R15_RECEIPT set: G7; G8 reported NOT_RUN by the script)
set -u
. "$(dirname "$0")/cand3_env.sh" || exit 2
CODE_SHA=$OMEGA_FINAL; OMEGA_SHA=$HARNESS_SHA
O=$W/wt-omega-chip; P=$W/wt-physics-chip
OUT=$EVID/CAND3-LIVING-$(echo "$CODE_SHA" | cut -c1-7)-G7b
export PHYSICS_DIR=$P AIENOS_LOCK_REPO=$HOME/workspace/r16-survey/aienos ARGUS_REPO=$HOME/workspace/aienos-argus ARGUS_STREAMS=$OUT/argus-streams
export R16_REPO_OMEGA=$O R16_REPO_SOVEREIGN_CORE=$W/survey/aien-sovereign-core \
  R16_REPO_AEGIS_RUNTIME=$W/survey/aegis-runtime R16_REPO_AIENOS=$W/survey/aienos R16_REPO_PHYSICS=$P
cd "$O" || exit 2
eval "$(sed -n '/^snap() {/,/^}/p' "$W/cand3_ladder.sh")"
eval "$(sed -n '/^guard() {/,/^}/p' "$W/cand3_ladder.sh")"
eval "$(sed -n '/^step() {/,/^}/p' "$W/cand3_ladder.sh")"
eval "$(sed -n '/^hashbins() {/,/^}/p' "$W/cand3_ladder.sh")"
mkdir -p "$OUT"; guard
case "${1:-}" in
phase2)
  REC=${2:?final R15 receipt path}; [ -s "$REC" ] || { echo "no receipt $REC"; exit 2; }
  step R16-G7 env R16_OUT_DIR="$OUT/R16-G7/raw" R16_EXPECT_COMMIT=$OMEGA_SHA R16_R15_RECEIPT="$REC" tools/r16_qualify.sh
  hashbins R16-G7 build/rx_r13_living_host build/rx_r13_living_silicon build/rx_operator
  guard; echo "phase2 done $(date -u +%FT%TZ)" >> "$OUT/LADDER-SUMMARY.txt" ;;
*) echo "usage: $0 phase2 <receipt>"; exit 2 ;;
esac
