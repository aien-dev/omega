#!/bin/bash
# Host self-test for tools/window_lane_guard.sh (omega #315). No chip, no make clean is run.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/wlg.XXXXXX"); trap 'rm -rf "$T"' EXIT
FAILS=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; FAILS=$((FAILS + 1)); fi; }
. "$HERE/tools/window_lane_guard.sh"
OUT=$T/out; B=$T/build
mkdir -p "$B/qual-runs/run1/R11" "$OUT"
printf '{"gate":"PASS"}\n' > "$B/qual-runs/run1/R11/rx_aien_faculty_receipt.json"

wl_begin "$OUT" r11
wl_guard_clean "$OUT" chipwait 2>/dev/null; rc=$?
check "chipwait refused while the r11 receipt is unarchived (exit 2)" '[ $rc = 2 ]'
check "refusal names the lane" 'wl_guard_clean "$OUT" chipwait 2>&1 | grep -q "lane r11 has an unarchived receipt"'
check "a lane is not refused by itself" 'wl_guard_clean "$OUT" r11'

wl_r11_archive "$OUT" "$B"; rc=$?
sha=$(sha256sum "$B/qual-runs/run1/R11/rx_aien_faculty_receipt.json" | cut -c1-64)
check "r11 archive exits 0" '[ $rc = 0 ]'
check "archived under its sha256 name" '[ -f "$OUT/R11-living/rx_aien_faculty_receipt.$sha.json" ]'
check "archived copy is identical" 'cmp -s "$OUT/R11-living/rx_aien_faculty_receipt.$sha.json" "$B/qual-runs/run1/R11/rx_aien_faculty_receipt.json"'
check "chipwait allowed after archiving" 'wl_guard_clean "$OUT" chipwait'
rm -rf "$B"   # what make clean does
check "receipt survives make clean" '[ -s "$OUT/R11-living/rx_aien_faculty_receipt.$sha.json" ]'
check "archiving twice keeps the same single file" 'mkdir -p "$B/qual-runs/run1/R11" && printf "{\"gate\":\"PASS\"}\n" > "$B/qual-runs/run1/R11/rx_aien_faculty_receipt.json" && wl_r11_archive "$OUT" "$B" && [ "$(ls "$OUT/R11-living" | wc -l)" = 1 ]'

rm -rf "$B"; wl_begin "$OUT" r11b
wl_r11_archive "$OUT" "$B" 2>/dev/null; rc=$?
check "missing receipt: archive fails (exit 1)" '[ $rc = 1 ]'
check "missing receipt: clean still refused for that lane" '! wl_guard_clean "$OUT" chipwait 2>/dev/null'
wl_no_receipt "$OUT" r11b
check "explicit no-receipt mark releases the guard" 'wl_guard_clean "$OUT" chipwait'

echo "window_lane_guard_test: $FAILS failures"
[ "$FAILS" = 0 ]
