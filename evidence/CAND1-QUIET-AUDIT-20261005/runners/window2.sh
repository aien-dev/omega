#!/bin/bash
# Window 2: R11 living + R15 silicon acceptance on overlay 81efb09 = CAND-1 80ca5d4 + omega#286 harness fix (src identical to cb06d08).
# Runs under one quietlock hold (Drake's 2026-10-05 campaign prompt = approval). Never killed, never wrapped in timeout.
set -u
CODE_SHA=cb06d081901c03063945a99ffed1c58d33397e9c; PHYS_SHA=6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf
W=$HOME/workspace/overnight-1005; O=$HOME/workspace/cand2-campaign/wt-cand1-overlay; P=$W/wt-CAND1-physics-chip
OUT=${OUT:?}; mkdir -p "$OUT"; cd "$O" || exit 2
export PHYSICS_DIR=$P AIENOS_R7_DIR=$W/wt-CAND1-aienos
snap() { local d=$1/machine-$2; mkdir -p "$d"; date -u +%FT%TZ > "$d/time.txt"
  nvidia-smi > "$d/nvidia-smi.txt" 2>&1
  nvidia-smi --query-gpu=name,driver_version,pci.bus_id,uuid,temperature.gpu,utilization.gpu,clocks.sm,power.draw --format=csv > "$d/gpu-identity.csv" 2>&1
  nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv > "$d/gpu-processes.csv" 2>&1
  ps -eo pid,user,pcpu,pmem,etime,args --sort=-pcpu | head -40 | cut -c1-200 > "$d/top-cpu.txt"
  cat /proc/loadavg > "$d/loadavg.txt"; uname -a > "$d/uname.txt"; cat /etc/machine-id > "$d/machine-id.txt"
  pgrep -c qemu > "$d/qemu-count.txt"; pgrep -c -f 'cargo|rustc|cc1|clangd' > "$d/build-proc-count.txt"
  cat ~/workspace/.spark-quiet > "$d/quiet-flag.txt" 2>&1
  { echo "omega $(git -C "$O" rev-parse HEAD) dirty=[$(git -C "$O" status --porcelain --untracked-files=no | head -3 | tr '\n' ' ')]";
    echo "physics $(git -C "$P" rev-parse HEAD)"; echo "physics.lock $(head -1 "$O/physics.lock")"; } > "$d/heads.txt"
  dmesg 2>/dev/null | grep -ci xid > "$d/xid-count.txt" || true; }
guard() { [ "$(git -C "$P" rev-parse HEAD)" = "$PHYS_SHA" ] || { echo "physics HEAD wrong"; exit 2; }
  [ -z "$(git -C "$O" status --porcelain --untracked-files=no)" ] || { echo "omega dirty"; exit 2; }
  [ "$(git -C "$O" rev-parse HEAD)" = 81efb0941a5a27738e46431069cff2a8943d850d ] || { echo "overlay HEAD wrong"; exit 2; }; git -C "$O" diff --quiet $CODE_SHA HEAD -- src physics.lock aienos.lock || { echo "runtime sources differ from CAND-1"; exit 2; }; }
step() { local g=$1; shift; local d=$OUT/$g; mkdir -p "$d"; snap "$d" before; echo "$*" > "$d/command.txt"
  local s=$(date -u +%s); date -u +%FT%TZ > "$d/start.txt"
  "$@" > "$d/stdout.log" 2> "$d/stderr.log" < /dev/null; local rc=$?
  date -u +%FT%TZ > "$d/end.txt"; echo $rc > "$d/exit.txt"; echo $(( $(date -u +%s) - s )) > "$d/seconds.txt"; snap "$d" after
  echo "$g rc=$rc secs=$(cat "$d/seconds.txt")" | tee -a "$OUT/SUMMARY.txt"; }
guard; snap "$OUT/00-identity" start
sha256sum build/rx_r11_aien_test build/rx_r15_perf_silicon build/rx_r15_perf_silicon_nodigest build/r15_reduce > "$OUT/00-identity/executables.sha256"
sleep 60   # let the 1-minute load average settle after the builds
step R11-living ./build/rx_r11_aien_test
step R15-silicon tools/r15_qualify.sh silicon
ls -td evidence/R15/raw/*-$(git rev-parse HEAD | cut -c1-12)-silicon | head -1 > "$OUT/R15-silicon/run-dir.txt"
guard; snap "$OUT/99-identity" end; echo "window done" >> "$OUT/SUMMARY.txt"
