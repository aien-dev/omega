#!/bin/sh
# R16-G5 API/build surface (spec/r16-orchestrator-retirement.md §5 R16-G5).
#
# Usage: tests/r16_surface/run.sh <omegatool-binary>
#
# Checks, host only (no mode that touches the graphics processor is run):
#   1. no old legacy mode name survives in code, scripts, the Makefile or the
#      README: omegatool's legacy modes are `--legacy-oracle-*` (class A,
#      retired) and `--reference-demonstrate-*` (class D, reference oracle).
#      Allowed: the inventory tool's detector pattern and one history comment.
#   2. the SEQ reference loop is named and called only in rx_seq_reference.*:
#      no production source under src/ calls rx_seq_* or names
#      run_until_complete.
#   3. no legacy mode is omegatool's default: with no argument it prints usage
#      and exits nonzero, running nothing.
#   4. the old mode names are rejected by the binary.
#   5. the production entry point is documented
#      (docs/r16-production-entry-point.md names the R13/R14 targets).
set -u
tool=${1:-build/omegatool}
fails=0
ok()   { echo "ok   $*"; }
bad()  { echo "FAIL $*"; fails=$((fails + 1)); }

# 1 ---------------------------------------------------------------------------
hits=$(git grep -n -e '--demonstrate-' -- tools src tests Makefile README.md \
        ':!tools/r16_loop_inventory.c' ':!tests/r16_surface/run.sh' \
        ':!tests/r16_inventory/fixture' \
    | grep -v '^tools/omegatool.c:[0-9]*: \* --legacy-oracle-living-matvec; the old --demonstrate-living-matvec mode is$')
if [ -z "$hits" ]; then ok "old mode names: none in code, scripts, Makefile or README"
else bad "old mode names still present:"; echo "$hits" | head -20; fi

# 2 ---------------------------------------------------------------------------
calls=$(git grep -nE 'rx_seq_[a-z_]+\(|run_until_complete' -- 'src/*.c' 'src/*.h' \
        ':!src/runtime/rx_seq_reference.c' ':!src/runtime/rx_seq_reference.h' \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|/\*|//)')
if [ -z "$calls" ]; then ok "SEQ reference loop: named and called only in rx_seq_reference.*"
else bad "SEQ reference loop called from production sources:"; echo "$calls" | head -20; fi

# 3 ---------------------------------------------------------------------------
[ -x "$tool" ] || { bad "binary missing: $tool"; echo "R16 gate: R16_G5_SURFACE=FAIL"; exit 1; }
out=$("$tool" 2>&1); rc=$?
lines=$(printf '%s\n' "$out" | wc -l)
if [ $rc -ne 0 ] && printf '%s\n' "$out" | head -1 | grep -q '^Usage:' && [ "$lines" -le 3 ] &&
   ! printf '%s\n' "$out" | grep -qE 'PASS|FAIL|Result'; then
    ok "omegatool default: usage only, exit $rc, nothing run"
else bad "omegatool with no argument ran something (exit $rc, $lines lines)"; fi

# 4 ---------------------------------------------------------------------------
for old in --demonstrate-living-matvec --demonstrate-arithmetic --demonstrate-accelerator; do
    o=$("$tool" "$old" 2>&1); rc=$?
    if [ $rc -ne 0 ] && printf '%s\n' "$o" | grep -q "Unknown argument: $old"; then
        ok "old mode $old rejected (exit $rc)"
    else bad "old mode $old accepted (exit $rc)"; fi
done

# 5 ---------------------------------------------------------------------------
doc=docs/r16-production-entry-point.md
if [ -f "$doc" ] && grep -q 'test-r13-silicon' "$doc" && grep -q 'test-r14-silicon' "$doc" &&
   grep -q -- '--reference-demonstrate-' "$doc" && grep -q -- '--legacy-oracle-living-matvec' "$doc"; then
    ok "production entry point documented in $doc"
else bad "$doc missing or incomplete"; fi

if [ $fails -eq 0 ]; then echo "R16 gate: R16_G5_SURFACE=PASS"; exit 0; fi
echo "R16 gate: R16_G5_SURFACE=FAIL ($fails)"; exit 1
