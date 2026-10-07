#!/bin/sh
# omega#323 / #324 GB10 window (approved by merge control 3649dd). Run under quietlock hold. Never killed.
# 1 RED   baseline (omega 9a20ca8 untouched) ctx 1,17,256, expect Xid 31 at ctx256 (host waits up to 600 s)
# 2 GREEN padded (9a20ca8 + e09adff) ctx 1,17,256, five fresh processes
# 3 padded hd128 full battery (gpu_attention_test --hd 128)
# 4 #324 head hd64 regression battery (tools/run_gpu_attention_chip.sh)
# stage a = 1-3 (hold 1), stage b = 4 (hold 2); 5 runs in both
# 5 kernel log (Xid) for the whole window
set -u
STAGE=${1:?stage a or b}
I=$HOME/workspace/investigations/2026-10-07-hd128-ctx256-stall; R=$I/runs/window-1$STAGE; C=$HOME/workspace/hive/CODEPAD
T0=$(date -u +%FT%TZ); echo "$T0" > $R/start.txt
{ for w in wt-base wt-pad omega; do echo "$w $(git -C $C/$w rev-parse HEAD) dirty=$(git -C $C/$w status --porcelain --untracked-files=no | wc -l)"; done
  echo "physics $(git -C $C/physics rev-parse HEAD)"; nvidia-smi --query-gpu=name,driver_version,utilization.gpu --format=csv,noheader
  nvidia-smi --query-compute-apps=pid,name --format=csv,noheader; uname -r
  sha256sum $I/src/va_camera_base $I/src/va_camera_pad $C/wt-base/build/libomega_gpu.a $C/wt-pad/build/libomega_gpu.a $C/wt-pad/build/gpu_attention_test
  journalctl -k --no-pager -o short-iso | grep -c 'Xid' | sed 's/^/xid_lines_before=/'; } > $R/ground.txt 2>&1
if [ "$STAGE" = a ]; then
$I/src/va_camera_base baseline 1 17 256 > $R/1-red-baseline.log 2>&1; echo "red rc=$?" >> $R/status.txt
for n in 1 2 3 4 5; do $I/src/va_camera_pad padded 1 17 256 > $R/2-green-padded-$n.log 2>&1; echo "green$n rc=$?" >> $R/status.txt; done
(cd $C/wt-pad && ./build/gpu_attention_test --hd 128 --out $R/3-hd128-padded-receipt.json > $R/3-hd128-padded.log 2>&1); echo "hd128 battery rc=$?" >> $R/status.txt
fi
if [ "$STAGE" = b ]; then
(cd $C/omega && sh tools/run_gpu_attention_chip.sh $R/4-hd64-pr324 > $R/4-hd64-pr324.log 2>&1); echo "hd64 regression rc=$?" >> $R/status.txt
fi
journalctl -k --no-pager -o short-iso --since "$(date -d "$T0" '+%F %T')" > $R/5-kernel-log.txt 2>&1
date -u +%FT%TZ > $R/end.txt
cd $R && sha256sum $(find . -type f ! -name SHA256SUMS | sort) > SHA256SUMS
