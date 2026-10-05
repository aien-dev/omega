#!/bin/sh
# ESTIMATION v5 window tests: the collector refuses without the flag, a foreign build is
# recorded and voids the window, a clean run is clean, the checker rejects a dead monitor.
# Uses a stub load generator and a scratch HOME; reads thermal sysfs files only.
set -u
here=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
pass=0; fail=0
ok() { pass=$((pass + 1)); }
bad() { echo "FAIL: $1" >&2; fail=$((fail + 1)); }
mkdir -p "$W/home/workspace"
printf '#!/bin/sh\nsleep "$2"\n' > "$W/est_stub"; chmod +x "$W/est_stub"
# 1. no flag: refuse with 3
HOME=$W/home sh "$here/est5_collect.sh" smoke 4 1 "$W/raw" "$W/est_stub" > /dev/null 2>&1; [ $? -eq 3 ] && ok || bad "no flag not refused"
# 2. flag for another gate: refuse with 3
printf 'owner x\ngate other\n' > "$W/home/workspace/.spark-quiet"
HOME=$W/home sh "$here/est5_collect.sh" smoke 4 1 "$W/raw" "$W/est_stub" > /dev/null 2>&1; [ $? -eq 3 ] && ok || bad "wrong gate not refused"
printf 'owner lane44\ngate est-v5\n' > "$W/home/workspace/.spark-quiet"
# 3. clean 8 s smoke: exit 0, WINDOW_CLEAN, flag untouched, folder named -est5-smoke-silicon
out=$(HOME=$W/home sh "$here/est5_collect.sh" smoke 8 1 "$W/raw" "$W/est_stub" 2> /dev/null); rc=$?
[ $rc -eq 0 ] && ok || bad "clean run rc=$rc"
grep -q '^WINDOW_CLEAN' "$out/WINDOW" 2> /dev/null && ok || bad "clean run not WINDOW_CLEAN"
[ -f "$W/home/workspace/.spark-quiet" ] && ok || bad "collector removed the flag"
case "$out" in *-est5-smoke-silicon) ok;; *) bad "folder name $out";; esac
# 4. a foreign build (a process named make) during the run: exit 7 and WINDOW_VOID
cp /bin/sleep "$W/make"
( sleep 3; "$W/make" 4 ) &
sleep 1
out=$(HOME=$W/home sh "$here/est5_collect.sh" smoke 10 1 "$W/raw" "$W/est_stub" 2> /dev/null); rc=$?
wait
[ $rc -eq 7 ] && ok || bad "foreign make not void, rc=$rc"
grep -q '^WINDOW_VOID' "$out/WINDOW" 2> /dev/null && ok || bad "foreign make not WINDOW_VOID"
grep -q ' FOREIGN .* make$' "$out/process-monitor.log" && ok || bad "foreign make not logged"
# 5. a foreign build already running at start: refuse with 3
( "$W/make" 6 ) &
sleep 1
HOME=$W/home sh "$here/est5_collect.sh" smoke 4 1 "$W/raw" "$W/est_stub" > /dev/null 2>&1; [ $? -eq 3 ] && ok || bad "pre-existing make not refused"
wait
# 6. checker: dead monitor and missing log
mkdir "$W/dead"; : > "$W/dead/process-monitor.log"
sh "$here/est5_window_check.sh" "$W/dead" 2700 > /dev/null; [ $? -eq 1 ] && ok || bad "dead monitor not void"
sh "$here/est5_window_check.sh" "$W/nolog" 2700 > /dev/null; [ $? -eq 1 ] && ok || bad "missing log not void"
echo "test_est5_window: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
