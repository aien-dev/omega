#!/bin/bash
# CAND-3 chip ladder. Copied from cand2_ladder.sh (CAND-2) with these changes, nothing else:
#   - constants come from cand3_env.sh (placeholders filled at freeze; refuses while one is left);
#   - A7 defect fixed: no lane relies on a build made by another lane. chipwait runs `make clean`, which removes
#     build/; the r15 lane now builds its own programs (make r15-perf-silicon) before r15_qualify.sh, and every
#     lane that runs a built program builds it first (r11, m18, ladder via make targets);
#   - every build step records the sha256 of the executables it produced and compares them with the double-build
#     digests (informational: the window builds in build/, the double build in build/rb);
#   - the r15 lane refuses unless ALLOW_R15=1: R15 is declared NOT ATTEMPTED in tonight's window (no energy reader);
#   - the ladder lane now includes the R16 G6 operator tests through tools/r16_qualify.sh (host, mutants, silicon).
# usage: cand3_ladder.sh <omega harness sha> ladder|r11|chipwait|m18|r15
# Runs ONLY from the forge (lanes.sh queue --heavy). Never wrapped in timeout; never kills anything.
# Run under quietlock hold; every step still records the GPU/CPU load.
set -u
. "$(dirname "$0")/cand3_env.sh" || exit 2
OMEGA_SHA=${1:?usage: cand3_ladder.sh <omega harness sha> <ladder|r11|chipwait|m18|r15>}; CODE_SHA=$OMEGA_FINAL; shift
[ "$OMEGA_SHA" = "$HARNESS_SHA" ] || { echo "harness sha $OMEGA_SHA is not HARNESS_SHA $HARNESS_SHA"; exit 2; }
O=$W/wt-omega-chip
P=$W/wt-physics-chip
OUT=$EVID/CAND3-LIVING-$(echo "$CODE_SHA" | cut -c1-7)-${WINDOW_TAG:?WINDOW_TAG not set (W1 is sealed; each window has its own directory)}
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
  git -C "$O" diff --quiet $CODE_SHA HEAD -- . ":(exclude)spec/r16-orchestrator-retirement-map.md" ":(glob,exclude)**/.crumb" ":(exclude).crumb" || { echo "omega HEAD differs from CAND-3 code $CODE_SHA beyond the R16 map and crumbs"; exit 2; }
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
hashbins() { # $1 gate dir name, rest = executables (paths relative to $O). Records sha256; compares with the double build (info only).
  local g=$1; shift; local d=$OUT/$g; mkdir -p "$d"
  sha256sum "$@" > "$d/executables.sha256" 2>&1
  local f n k b
  for f in "$@"; do n=$(basename "$f"); k=$(case $n in omegatool) echo omega-runtime;; *) echo "omega-$(echo "$n" | tr _ -)";; esac)
    b=$(awk -v k="$k" '$1==k{print $2}' "$BUILD_DIGESTS" 2>/dev/null); a=$(sha256sum "$f" 2>/dev/null | cut -c1-64)
    if [ -z "$b" ]; then echo "INFO $n has no double-build digest"; elif [ "$a" = "$b" ]; then echo "MATCH $n double-build digest"; else echo "DIFFER $n window=$a double-build=$b"; fi
  done > "$d/executables-vs-double-build.txt"
}
mkdir -p "$OUT"; guard
case "${1:-}" in
ladder)
  snap "$OUT/00-identity" start
  # R16 harness: G1-G5 + G7 ladder (R1-R11 host, R12/R13/R14 host+silicon, R15 parity host+silicon,
  # R15 G7 host, R15 receipt test) + G3 authpath host+silicon + G6 operator control (host, mutants, silicon, omega #309).
  # Stops later silicon after one silicon FAIL.
  step R16-ladder env R16_OUT_DIR="$OUT/R16-ladder/raw" R16_EXPECT_COMMIT=$OMEGA_SHA tools/r16_qualify.sh
  hashbins R16-ladder build/rx_r13_living_host build/rx_r13_living_silicon build/rx_operator
  cp -a build/qual-runs "$OUT/qual-runs-after-R16" 2>/dev/null
  step R13-testbuild-silicon env OMEGA_CANDIDATE_COMMIT=$OMEGA_SHA make test-r13-testbuild-silicon
  hashbins R13-testbuild-silicon build/rx_r13_living_testbuild_silicon
  cp -a build/qual-runs "$OUT/qual-runs-after-R13" 2>/dev/null
  step prod-hygiene-silicon make test-prod-hygiene-silicon
  step COMPOSITION-2-GPU make test-composition-gate-gpu
  hashbins COMPOSITION-2-GPU build/rx_composition_gate_gpu
  [ -s build/composition_gate_gpu_receipt.json ] && cp build/composition_gate_gpu_receipt.json \
     "$OUT/COMPOSITION-2-GPU/receipt-$(sha256sum build/composition_gate_gpu_receipt.json | cut -c1-64).json"
  guard; echo "ladder done" >> "$OUT/LADDER-SUMMARY.txt" ;;
chipwait)
  # CHIPWAIT campaign: 3 x tools/m19r_qualify.sh full mode (M19R Gates 1/2 incl. the LONG_SOAK
  # >= 100000-cycle endurance criterion), predeclared 3/3 rule from tools/m19r_campaign.sh. No --record.
  # m19r_qualify runs `make clean` in this worktree and removes build/: no later lane may use a build made before it.
  step CHIPWAIT tools/chipwait_campaign.sh --omega-candidate $OMEGA_SHA --physics-candidate $PHYS_SHA \
     --physics-dir "$P" --campaign-dir "$OUT/CHIPWAIT/campaign" --runs 3
  guard; echo "chipwait done" >> "$OUT/LADDER-SUMMARY.txt" ;;
m18)
  # M18 matmul gates (src/omega_blackwell_gates.c, --run-m18-gates). Builds omegatool itself.
  step M18-build make build/omegatool
  hashbins M18-build build/omegatool
  step M18 ./build/omegatool --run-m18-gates
  guard; echo "m18 done" >> "$OUT/LADDER-SUMMARY.txt" ;;
r11)
  # R11 living under load (tests/runtime/rx_r11_aien.c). Runs only inside its own quietlock hold:
  # quietlock exports QUIETLOCK_HOLD and the flag line carries hold=<id> (quiet_flag_is_mine); under any
  # other quiet flag it prints "living run not exercised", which is a refusal, never a pass.
  step R11-build make build/rx_r11_aien_test
  hashbins R11-build build/rx_r11_aien_test
  sleep 60   # let the 1-minute load average settle after the build
  step R11-living ./build/rx_r11_aien_test
  guard; echo "r11 done" >> "$OUT/LADDER-SUMMARY.txt" ;;
r15)
  # R15 silicon on its own. NOT RUN IN THE 2026-10-06 WINDOW (declared NOT ATTEMPTED: the SPBM energy reader is not
  # loaded; the preflight would refuse). A7 fix (CAND-2): this lane BUILDS its own programs first, because the
  # chipwait lane's `make clean` removes build/ and r15_qualify.sh needs build/rx_r15_perf_silicon. It is a separate,
  # later, separately declared attempt: set ALLOW_R15=1 only after that attempt is declared.
  [ "${ALLOW_R15:-0}" = 1 ] || { echo "r15 lane refused: R15 is not part of this window (set ALLOW_R15=1 for its own declared attempt)"; exit 2; }
  step R15-build make r15-perf-silicon
  hashbins R15-build build/rx_r15_perf_silicon build/rx_r15_perf_silicon_nodigest build/r15_reduce
  for x in rx_r15_perf_silicon rx_r15_perf_silicon_nodigest r15_reduce; do
    [ -x build/$x ] || { echo "R15-build did not produce build/$x: R15 NOT_RUN (harness defect, not an instrument refusal)" | tee -a "$OUT/LADDER-SUMMARY.txt"; exit 2; }
  done
  step R15-silicon tools/r15_qualify.sh silicon
  guard; echo "r15 done" >> "$OUT/LADDER-SUMMARY.txt" ;;
*) echo "usage: $0 <sha> ladder|r11|chipwait|m18|r15"; exit 2 ;;
esac
