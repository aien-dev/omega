#!/bin/bash
# CAND-3 A7c / G7 attempt. Declared in DECLARED-ATTEMPT-A7c.md before it ran. Never kills, never retries, runs once.
# usage: cand3_r15c.sh phase1   (reader facts, R15 build, section-14 correctness reruns, R15 silicon, receipt-A)
#        cand3_r15c.sh phase2 <final R15 receipt path>   (R16 harness once with R16_R15_RECEIPT set: G7; G8 reported NOT_RUN by the script)
set -u
. "$(dirname "$0")/cand3_env.sh" || exit 2
CODE_SHA=$OMEGA_FINAL; OMEGA_SHA=$HARNESS_SHA
O=$W/wt-omega-chip; P=$W/wt-physics-chip
OUT=$EVID/CAND3-LIVING-$(echo "$CODE_SHA" | cut -c1-7)-A7c
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
phase1)
  mkdir -p "$OUT/00-declared" "$OUT/00-reader"
  cp -a "$W/DECLARED-ATTEMPT-A7c.md" "$W/cand3_r15c.sh" "$W/cand3_env.sh" "$W/cand3_ladder.sh" "$OUT/00-declared/"
  sha256sum "$W/DECLARED-ATTEMPT-A7c.md" "$W/cand3_r15c.sh" "$W/cand3_env.sh" "$W/cand3_ladder.sh" > "$OUT/00-declared/sha256.txt"
  echo "QUIETLOCK_HOLD=${QUIETLOCK_HOLD:-unset} start=$(date -u +%FT%TZ)" > "$OUT/00-declared/window.txt"
  snap "$OUT/00-reader" start
  { uname -r; cat /sys/devices/system/cpu/../../../proc/cmdline 2>/dev/null | head -1; mokutil --sb-state 2>&1; cat /sys/kernel/security/lockdown 2>&1
    lsmod | grep aien_spbm; for h in /sys/class/hwmon/hwmon*; do echo "$h $(cat $h/name)"; done
    modinfo aien_spbm_readonly 2>&1 | head -20
    sha256sum $HOME/workspace/omega-r15/research/m15/spbm/aien_spbm_readonly.ko
    journalctl -k --no-pager 2>/dev/null | grep -i -E "spbm|module verification"; } > "$OUT/00-reader/reader-facts.txt" 2>&1
  step 00-reader-preflight tools/r15_machine_state.sh energy-preflight
  step R15-build make r15-perf-silicon
  hashbins R15-build build/rx_r15_perf_silicon build/rx_r15_perf_silicon_nodigest build/r15_reduce
  for x in rx_r15_perf_silicon rx_r15_perf_silicon_nodigest r15_reduce; do
    [ -x build/$x ] || { echo "R15-build did not produce build/$x: NOT_RUN (harness defect)" | tee -a "$OUT/LADDER-SUMMARY.txt"; exit 2; }
  done
  # section 14 correctness reruns on the candidate, sequential, each recorded; none is retried
  RD=$OUT/R15-reruns; mkdir -p "$RD"; : > "$RD/rc.txt"
  declare -A rc
  for t in test-r3 test-r7 test-r8 test-r9 test-r10 test-r11 test-r12 test-r12-silicon test-r13-host test-r13-silicon test-r14-host test-r14-silicon test-r15-parity-host test-r15-parity-silicon; do
    step R15-reruns/$t env OMEGA_CANDIDATE_COMMIT=$OMEGA_SHA make $t
    rc[$t]=$(cat "$OUT/R15-reruns/$t/exit.txt"); echo "$t ${rc[$t]}" >> "$RD/rc.txt"
  done
  v() { [ "${rc[$1]:-1}" = 0 ] && echo PASS || echo FAIL; }
  printf '{"candidate_commit":"%s","tree_dirty":false,"when":"%s","R3":"%s","R7":"%s","R8":"%s","R9":"%s","R10":"%s","R11":"%s","R12_host":"%s","R12_silicon":"%s","R13_host":"%s","R13_silicon":"%s","R14_host":"%s","R14_silicon":"%s","R15_SEQ_parity":"%s","R15_SEQ_parity_host":"%s","R15_SEQ_parity_silicon":"%s","make_targets":"test-r3 test-r7 test-r8 test-r9 test-r10 test-r11 test-r12 test-r12-silicon test-r13-host test-r13-silicon test-r14-host test-r14-silicon test-r15-parity-host test-r15-parity-silicon, run sequentially under quietlock; R12 host and silicon ran as test-r12 and test-r12-silicon; PASS means exit 0"}\n' \
    "$OMEGA_SHA" "$(date -u +%FT%TZ)" "$(v test-r3)" "$(v test-r7)" "$(v test-r8)" "$(v test-r9)" "$(v test-r10)" "$(v test-r11)" "$(v test-r12)" "$(v test-r12-silicon)" "$(v test-r13-host)" "$(v test-r13-silicon)" "$(v test-r14-host)" "$(v test-r14-silicon)" \
    "$([ "$(v test-r15-parity-host)$(v test-r15-parity-silicon)" = PASSPASS ] && echo PASS || echo FAIL)" "$(v test-r15-parity-host)" "$(v test-r15-parity-silicon)" > "$RD/reruns.json"
  guard
  step R15-silicon env R15_OUT_BASE="$OUT/R15-raw" AIENOS_R7_DIR="$W/survey/aienos" PHYSICS_DIR="$P" tools/r15_qualify.sh silicon
  RAW=$(ls -td "$OUT"/R15-raw/*-silicon 2>/dev/null | head -1); echo "$RAW" > "$OUT/R15-silicon/run-dir.txt"
  if [ -n "$RAW" ] && [ -s "$RAW/summary.json" ]; then
    step R15-receipt-A tools/r15_receipt.sh "$RAW" "$OUT/R15-receipt-A" "$OMEGA_SHA" "$RD/reruns.json"
  else echo "no summary.json: no receipt (recorded)" | tee -a "$OUT/LADDER-SUMMARY.txt"; fi
  echo "post-run reader state: $(cat /sys/class/hwmon/hwmon3/name 2>&1) $(lsmod | grep aien_spbm)" >> "$OUT/LADDER-SUMMARY.txt"
  guard; echo "phase1 done $(date -u +%FT%TZ)" >> "$OUT/LADDER-SUMMARY.txt" ;;
phase2)
  REC=${2:?final R15 receipt path}; [ -s "$REC" ] || { echo "no receipt $REC"; exit 2; }
  step R16-G7 env R16_OUT_DIR="$OUT/R16-G7/raw" R16_EXPECT_COMMIT=$OMEGA_SHA R16_R15_RECEIPT="$REC" tools/r16_qualify.sh
  hashbins R16-G7 build/rx_r13_living_host build/rx_r13_living_silicon build/rx_operator
  guard; echo "phase2 done $(date -u +%FT%TZ)" >> "$OUT/LADDER-SUMMARY.txt" ;;
*) echo "usage: $0 phase1 | phase2 <receipt>"; exit 2 ;;
esac
