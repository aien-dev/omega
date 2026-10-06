#!/bin/bash
# G7b wrapper: runs inside quietlock hold. Waits (never kills) up to 10 min for 1-min load < 1.8, then runs G7 once.
cd /home/drakestapleton/workspace/cand3-campaign/cand3 || exit 2
OUT=/home/drakestapleton/workspace/evidence-out/CAND3-LIVING-f816473-G7b
mkdir -p $OUT/00-declared
cp -a DECLARED-ATTEMPT-G7b.md cand3_g7b.sh cand3_env.sh cand3_ladder.sh g7b_run.sh $OUT/00-declared/
sha256sum DECLARED-ATTEMPT-G7b.md cand3_g7b.sh cand3_env.sh cand3_ladder.sh g7b_run.sh > $OUT/00-declared/sha256.txt
diff cand3_r15c.sh cand3_g7b.sh > $OUT/00-declared/script-diff-vs-r15c.txt
echo "QUIETLOCK_HOLD=${QUIETLOCK_HOLD:-unset} start=$(date -u +%FT%TZ)" > $OUT/00-declared/window.txt
T=$OUT/load-trace.txt; : > $T
for i in $(seq 0 20); do
  l=$(cut -d' ' -f1 /proc/loadavg); echo "$(date -u +%FT%TZ) load1=$l" >> $T
  if awk "BEGIN{exit !($l<1.8)}"; then echo "START load1=$l" >> $T; ok=1; break; fi
  sleep 30
done
if [ "${ok:-0}" != 1 ]; then
  echo "NOT_RUN: load never below 1.8 in 10 minutes" >> $T; ps -eo pcpu,user,cmd --sort=-pcpu | head >> $T; exit 4
fi
exec ./cand3_g7b.sh phase2 /home/drakestapleton/workspace/evidence-out/CAND3-LIVING-f816473-A7c/R15-receipt-B/90fdebb8bd2c8ff988c6d93afb764c3466bd6c3575cd8176fc605378fdb53fdd.json
