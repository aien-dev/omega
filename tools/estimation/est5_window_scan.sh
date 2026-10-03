#!/bin/sh
# ESTIMATION v5 window scan: print one line per foreign build/test/chip-gate process
# currently running, "FOREIGN <pid> <ppid> <comm>", excluding the collector's own load
# generator (pid $1, its children), the scan's own ancestors (the collector and whatever
# launched it) and the scan itself. Prints nothing when the machine is clean.
#   est5_window_scan.sh [own-load-pid]
own=${1:-0}
anc=" "
p=$$
while [ -n "$p" ] && [ "$p" -gt 1 ] 2> /dev/null; do
    anc="$anc$p "
    p=$(awk '{ sub(/^.*\) /, ""); print $2 }' "/proc/$p/stat" 2> /dev/null)
done
ps -eo pid=,ppid=,comm= | awk -v own="$own" -v anc="$anc" '
$3 ~ /^(make|gmake|cc|gcc|g\+\+|c\+\+|cc1|cc1plus|clang.*|ld|ld\..*|as|collect2|cmake|ninja|ctest|cargo|rustc|qemu-system.*|test_.*|est_load|est[0-9a-z_]*|verify_all.*|run_gate.*|t)$/ {
    if ($1 == own || $2 == own) next
    if (index(anc, " " $1 " ")) next
    print "FOREIGN", $1, $2, $3
}'
