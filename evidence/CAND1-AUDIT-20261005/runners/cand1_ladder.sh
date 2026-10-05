#!/bin/bash
# L7-LIVING chip ladder on CAND-0 omega e5593ae + physics 6d7cf0d.
# usage: L7-chip-ladder.sh ladder | chipwait | m18
# Runs ONLY from the forge (lanes.sh queue --heavy). Never wrapped in timeout; never kills anything.
# Shared machine: no quiet flag is taken (no approval); every step records the concurrent GPU/CPU load.
set -u
OMEGA_SHA=${1:?usage: cand1_ladder.sh <omega sha: cb06d08 or a map-only descendant> <ladder|chipwait|m18>}; CODE_SHA=cb06d081901c03063945a99ffed1c58d33397e9c; shift
PHYS_SHA=6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf
W=$HOME/workspace/overnight-1005
O=$W/wt-CAND1-omega-chip
P=$W/wt-CAND1-physics-chip
OUT=$HOME/workspace/evidence-out/CAND1-LIVING-cb06d08
export PHYSICS_DIR=$P AIENOS_LOCK_REPO=$HOME/workspace/r16-survey/aienos ARGUS_REPO=$W/aienos ARGUS_STREAMS=$OUT/argus-streams
export R16_REPO_OMEGA=$O R16_REPO_SOVEREIGN_CORE=$W/cand1-survey/aien-sovereign-core \
  R16_REPO_AEGIS_RUNTIME=$W/cand1-survey/aegis-runtime R16_REPO_AIENOS=$W/cand1-survey/aienos \
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
  git -C "$O" diff --quiet $CODE_SHA HEAD -- . ":(exclude)spec/r16-orchestrator-retirement-map.md" ":(glob,exclude)**/.crumb" ":(exclude).crumb" || { echo "omega HEAD differs from CAND-1 code $CODE_SHA beyond the R16 map and crumbs"; exit 2; }
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
*) echo "usage: $0 ladder|chipwait|m18"; exit 2 ;;
esac
