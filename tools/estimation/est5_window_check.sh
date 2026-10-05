#!/bin/sh
# ESTIMATION v5 window verdict for one collection folder (protocol est-v5.md section 4).
#   est5_window_check.sh <folder> <seconds>
# Exit 0 and "WINDOW_CLEAN" when process-monitor.log has no FOREIGN line and at least 90 %
# of the expected 2 s samples ("S" lines); otherwise exit 1 and "WINDOW_VOID <reason>".
d=${1:?folder}
secs=${2:?seconds}
log=$d/process-monitor.log
[ -f "$log" ] || { echo "WINDOW_VOID no process-monitor.log"; exit 1; }
nf=$(grep -c ' FOREIGN ' "$log")
ns=$(grep -c ' S ' "$log")
need=$(( secs / 2 * 9 / 10 ))
if [ "$nf" -gt 0 ]; then echo "WINDOW_VOID $nf foreign build/test/gate process samples"; exit 1; fi
if [ "$ns" -lt "$need" ]; then echo "WINDOW_VOID monitor logged $ns samples, need at least $need"; exit 1; fi
echo "WINDOW_CLEAN samples=$ns foreign=0"
exit 0
