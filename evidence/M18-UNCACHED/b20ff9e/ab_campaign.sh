#!/bin/bash
# M18 cache A/B under a documented host load: 4 alternating batches (baseline main 4cca032, then the fix),
# 5 M18 runs each, same load for both arms. One queued heavy job. Never kills chip processes; the load
# loops are our own sha256sum processes and stop by timeout.
set -u
E=$HOME/workspace/evidence-out/M18-UNCACHED-20261003
B=$HOME/workspace/hive-worktrees/omega-world-baseline-4cca032
F=$HOME/workspace/hive-worktrees/omega-m18-uncached
LOOPS=${LOOPS:-16}; SECS=${SECS:-4200}
echo "campaign start $(date -u +%FT%TZ) baseline=$(git -C $B rev-parse --short HEAD) fix=$(git -C $F rev-parse --short HEAD) loops=$LOOPS" >> $E/campaign.txt
uptime >> $E/campaign.txt
$HOME/.claude/jobs/0cb8c31e/tmp/m18_load.sh "$LOOPS" "$SECS" &
LOADPID=$!
sleep 20; uptime >> $E/campaign.txt
for i in 1 2 3 4; do
  (cd $B && $E/runjob.sh baseline-m18-L$i 5 --run-m18-gates) > $E/batch-baseline-L$i.out 2>&1
  (cd $F && $E/runjob.sh fix-m18-L$i 5 --run-m18-gates) > $E/batch-fix-L$i.out 2>&1
  uptime >> $E/campaign.txt
done
pkill -f 'sha256sum /dev/zero' 2>/dev/null; wait $LOADPID 2>/dev/null
echo "campaign end $(date -u +%FT%TZ)" >> $E/campaign.txt
for arm in baseline fix; do t=0; f=0; for d in $E/${arm}-m18-L[1-4]; do n=$(wc -l < $d/exit-codes.txt); x=$(grep -vc 'rc=0' $d/exit-codes.txt); t=$((t+n)); f=$((f+x)); done; echo "$arm: $f/$t runs failed; gates: $(grep -h '\[FAIL\]' $E/${arm}-m18-L[1-4]/*.log 2>/dev/null | awk '{print $2}' | sed 's/OMEGA_BW_MATMUL_//' | sort | uniq -c | tr '\n' ';')" | tee -a $E/campaign.txt; done
