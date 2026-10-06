#!/bin/bash
# Waits (never interrupts) until no qemu runs, 1-min load < 1.5 and no quiet hold; then runs the declared window once.
cd /home/drakestapleton/workspace/cand3-campaign/cand3 || exit 2
log=window-start-w2.log
while :; do
  q=$(pgrep -c qemu-system); l=$(cut -d' ' -f1 /proc/loadavg)
  if [ "$q" = 0 ] && awk "BEGIN{exit !($l<1.5)}" && quietlock check >/dev/null 2>&1; then break; fi
  echo "$(date -u +%FT%TZ) waiting qemu=$q load1=$l" >> $log; sleep 60
done
./cand3_setup_trees.sh >> $log 2>&1 || { echo "$(date -u +%FT%TZ) setup check FAILED: window not started" >> $log; exit 3; }
echo "$(date -u +%FT%TZ) starting window load1=$(cut -d' ' -f1 /proc/loadavg)" >> $log
quietlock hold --owner cand3-campaign --minutes 90 --reason "CAND-3 declared window W2, DECLARED-ATTEMPTS-w2 sha256 ef42d9c8" -- env WINDOW_TAG=w2 ./run_window.sh > window-w2.out 2>&1
echo "$(date -u +%FT%TZ) window exit $?" >> $log
