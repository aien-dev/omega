#!/bin/bash
# CAND-2 chip ladder: omega code 79a805d (+ map-only descendant) + physics 6d7cf0d. Copied from cand1_ladder.sh.
# usage: cand2_ladder.sh <omega harness sha> ladder|r11|chipwait|m18|r15
# Runs ONLY from the forge (lanes.sh queue --heavy). Never wrapped in timeout; never kills anything.
# Run under quietlock hold (campaign approval, announced on the mindmap); every step still records the GPU/CPU load.
set -u
OMEGA_SHA=${1:?usage: cand2_ladder.sh <omega sha: 79a805d or a map-only descendant> <ladder|chipwait|m18>}; CODE_SHA=79a805d162bfded8c5ce5a4c14f7c29e79025f39; shift
PHYS_SHA=6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf
W=$HOME/workspace/cand3-campaign/cand2
O=$W/wt-omega-chip
P=$W/wt-physics-chip
OUT=$HOME/workspace/evidence-out/CAND2-LIVING-79a805d
export PHYSICS_DIR=$P AIENOS_LOCK_REPO=$HOME/workspace/r16-survey/aienos ARGUS_REPO=$HOME/workspace/aienos-argus ARGUS_STREAMS=$OUT/argus-streams
export R16_REPO_OMEGA=$O R16_REPO_SOVEREIGN_CORE=$W/survey/aien-sovereign-core \
  R16_REPO_AEGIS_RUNTIME=$W/survey/aegis-runtime R16_REPO_AIENOS=$W/survey/aienos \
  R16_REPO_PHYSICS=$P
cd "$O" || exit 2
snap() { # $1 dir, $2 tag
  local d=$1/machine-$2; mkdir -p "$d"
  date -u +%FT%TZ > "$d/time.txt"
  nvidia-smi > "$d/nvidia-smi.txt" 2>&1
  nvidia-smi --query-gpu=name,driver_version,pci.bus_id,uuid,temperature.gpu,utilization.gpu,clocks.sm --format=csv > "$d/gpu-identity.csv" 2>&1
  nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv > "$d/gpu-processes.csv" 2>&1
  ps -eo pid,user,pcpu,pmem,etime,comm --sort=-pcpu | head -30 > "$d/top-cpu.txt"
  uptime > "$d/uptime.txt"; uname -a > "$d/uname.txt"
  { echo "omega $(git -C "$O" rev-parse HEAD) dirty=[$(git -C "$O" status --porcelain | head -3 | tr '\n' ' ')]";
    echo "physics $(git -C "$P" rev-parse HEAD) dirty=[$(git -C "$P" status --porcelain | head -3 | tr '\n' ' ')]";
    echo "physics.lock $(head -1 "$O/physics.lock")"; } > "$d/heads.txt"
  dmesg 2>/dev/null | grep -ci 'xid' > "$d/xid-count.txt" || true
}
guard() {
  [ "$(git -C "$O" rev-parse HEAD)" = "$OMEGA_SHA" ] || { echo "omega HEAD is not $OMEGA_SHA"; exit 2; }
  [ "$(git -C "$P" rev-parse HEAD)" = "$PHYS_SHA" ] || { echo "physics HEAD is not $PHYS_SHA"; exit 2; }
  [ -z "$(git -C "$O" status --porcelain)" ] || { echo "omega tree dirty"; exit 2; }
  git -C "$O" diff --quiet $CODE_SHA HEAD -- . ":(exclude)spec/r16-orchestrator-retirement-map.md" ":(glob,exclude)**/.crumb" ":(exclude).crumb" || { echo "omega HEAD differs from CAND-2 code $CODE_SHA beyond the R16 map and crumbs"; exit 2; }
}
step() { # $1 gate dir name, rest = command
  local g=$1; shift; local d=$OUT/$g
  mkdir -p "$d"; snap "$d" before
  echo "$*" > "$d/command.txt"
  local s=$(date -u +%s)
  "$@" > "$d/stdout.log" 2> "$d/stderr.log" < /dev/null
  local rc=$?
  echo "$rc" > "$d/exit.txt"; echo "$(( $(date -u +%s) - s ))" > "$d/seconds.txt"
  snap "$d" after
  echo "$g rc=$rc secs=$(cat "$d/seconds.txt")" | tee -a "$OUT/LADDER-SUMMARY.txt"
}
mkdir -p "$OUT"; guard
case "${1:-}" in
ladder)
  snap "$OUT/00-identity" start
  # R16 harness: G1-G5 + G7 ladder (R1-R11 host, R12/R13/R14 host+silicon, R15 parity host+silicon,
  # R15 G7 host, R15 receipt test) + G3 authpath host+silicon. Stops later silicon after one silicon FAIL.
  step R16-ladder env R16_OUT_DIR="$OUT/R16-ladder/raw" R16_EXPECT_COMMIT=$OMEGA_SHA tools/r16_qualify.sh
  cp -a build/qual-runs "$OUT/qual-runs-after-R16" 2>/dev/null
  step R13-testbuild-silicon env OMEGA_CANDIDATE_COMMIT=$OMEGA_SHA make test-r13-testbuild-silicon
  cp -a build/qual-runs "$OUT/qual-runs-after-R13" 2>/dev/null
  step prod-hygiene-silicon make test-prod-hygiene-silicon
  step COMPOSITION-2-GPU make test-composition-gate-gpu
  [ -s build/composition_gate_gpu_receipt.json ] && cp build/composition_gate_gpu_receipt.json \
     "$OUT/COMPOSITION-2-GPU/receipt-$(sha256sum build/composition_gate_gpu_receipt.json | cut -c1-64).json"
  guard; echo "ladder done" >> "$OUT/LADDER-SUMMARY.txt" ;;
chipwait)
  # CHIPWAIT campaign: 3 x tools/m19r_qualify.sh full mode (M19R Gates 1/2 incl. the LONG_SOAK
  # >= 100000-cycle endurance criterion), predeclared 3/3 rule from tools/m19r_campaign.sh. No --record.
  # m19r_qualify runs `make clean` in this worktree: keep this lane AFTER the ladder lane.
  step CHIPWAIT tools/chipwait_campaign.sh --omega-candidate $OMEGA_SHA --physics-candidate $PHYS_SHA \
     --physics-dir "$P" --campaign-dir "$OUT/CHIPWAIT/campaign" --runs 3
  guard; echo "chipwait done" >> "$OUT/LADDER-SUMMARY.txt" ;;
m18)
  # M18 matmul gates (src/omega_blackwell_gates.c, --run-m18-gates). Last chip PASS: M18-UNCACHED b20ff9e
  # (merged ccdfef3); submit.c changed since (ea60ea6, 5e29b82). Builds omegatool first.
  step M18-build make build/omegatool
  step M18 ./build/omegatool --run-m18-gates
  guard; echo "m18 done" >> "$OUT/LADDER-SUMMARY.txt" ;;
r11)
  # R11 living under load (tests/runtime/rx_r11_aien.c). Runs only inside its own quietlock hold:
  # quietlock exports QUIETLOCK_HOLD and the flag line carries hold=<id> (quiet_flag_is_mine); under any
  # other quiet flag it prints "living run not exercised", which is a refusal, never a pass.
  step R11-build make build/rx_r11_aien_test
  sha256sum build/rx_r11_aien_test > "$OUT/R11-build/rx_r11_aien_test.sha256"
  sleep 60   # let the 1-minute load average settle after the build
  step R11-living ./build/rx_r11_aien_test
  guard; echo "r11 done" >> "$OUT/LADDER-SUMMARY.txt" ;;
r15)
  # R15 silicon on its own (the R16 ladder stops later silicon after one FAIL). Since omega #295 the
  # energy preflight refuses before a window is used when the SPBM instrument is absent: that is
  # recorded as INSTRUMENT_UNAVAILABLE, never as a performance result.
  step R15-silicon tools/r15_qualify.sh silicon
  guard; echo "r15 done" >> "$OUT/LADDER-SUMMARY.txt" ;;
*) echo "usage: $0 <sha> ladder|r11|chipwait|m18|r15"; exit 2 ;;
esac
