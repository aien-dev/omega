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
#   4. every retired --demonstrate-<name> (all 16 reference modes, read from
#      omegatool's reference_modes table, plus living-matvec) is rejected.
#   5. every new name (--reference-demonstrate-<name>, --legacy-oracle-living-
#      matvec) is accepted by the dispatcher (--reference-dispatch-dry-run
#      resolves without running), and --reference-demonstrate-arithmetic
#      (CPU only) really runs. Usage lists exactly the table's modes (3).
#   6. the production entry point is documented
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
if [ $rc -ne 0 ] && printf '%s\n' "$out" | head -1 | grep -q '^Usage:' && [ "$lines" -le 4 ] &&
   ! printf '%s\n' "$out" | grep -qE 'PASS|FAIL|Result'; then
    ok "omegatool default: usage only, exit $rc, nothing run"
else bad "omegatool with no argument ran something (exit $rc, $lines lines)"; fi

# The reference modes, read from the one table the dispatcher uses.
modes=$(awk '/^static const ReferenceMode reference_modes\[\] = \{/ {on = 1; next}
             on && /^\};/ {exit}
             on { if (match($0, /\{"[a-z0-9-]+"/)) print substr($0, RSTART + 2, RLENGTH - 3) }' \
        tools/omegatool.c)
nmodes=$(printf '%s\n' "$modes" | grep -c .)
if [ "$nmodes" -eq 16 ]; then ok "reference mode table: $nmodes modes"
else bad "reference mode table has $nmodes modes, expected 16"; fi

# 3b. usage lists exactly the table's modes.
listed=$(printf '%s\n' "$out" | tr ' ' '\n' | sed -n 's/^--reference-demonstrate-//p' | sort)
if [ "$listed" = "$(printf '%s\n' "$modes" | sort)" ]; then
    ok "usage lists all $nmodes reference modes and no others"
else bad "usage and the reference mode table differ:"; printf 'usage: %s\ntable: %s\n' \
    "$(echo $listed)" "$(echo $modes)"; fi

# 4 ---------------------------------------------------------------------------
# Every retired --demonstrate-<name> is rejected. The resolver (dry run, same
# dispatch, nothing run) is asked for all of them; the real binary is run on
# the ones that cannot touch the chip (never the accelerator or Blackwell names:
# if a regression accepted one, it would start a chip run).
nrej=0
for m in $modes living-matvec; do
    old=--demonstrate-$m
    o=$("$tool" --reference-dispatch-dry-run "$old" 2>&1); rc=$?
    if [ $rc -ne 0 ] && printf '%s\n' "$o" | grep -q "Unknown argument: $old"; then
        nrej=$((nrej + 1))
    else bad "old mode $old accepted by the dispatcher (exit $rc)"; fi
    case $m in accelerator*|blackwell*) continue ;; esac
    o=$("$tool" "$old" 2>&1); rc=$?
    if [ $rc -eq 0 ] || ! printf '%s\n' "$o" | grep -q "Unknown argument: $old"; then
        bad "old mode $old accepted by the binary (exit $rc)"
    fi
done
[ "$nrej" -eq $((nmodes + 1)) ] && ok "all $nrej retired --demonstrate-* names rejected"

# 5 ---------------------------------------------------------------------------
# Every new name is accepted by the real dispatcher (dry run: resolved, not
# run), and one CPU-only mode really runs.
nacc=0
for n in $(printf -- '--reference-demonstrate-%s\n' $modes) --legacy-oracle-living-matvec; do
    o=$("$tool" --reference-dispatch-dry-run "$n" 2>&1); rc=$?
    if [ $rc -eq 0 ] && printf '%s\n' "$o" | grep -q "^dispatch: $n accepted"; then
        nacc=$((nacc + 1))
    else bad "new mode $n not accepted by the dispatcher (exit $rc)"; fi
done
[ "$nacc" -eq $((nmodes + 1)) ] && ok "all $nacc new names accepted by the dispatcher"
o=$("$tool" --reference-demonstrate-arithmetic 2>&1); rc=$?
if [ $rc -eq 0 ] && ! printf '%s\n' "$o" | grep -q 'Unknown argument' &&
   printf '%s\n' "$o" | grep -q 'PASS'; then
    ok "--reference-demonstrate-arithmetic runs (exit 0)"
else bad "--reference-demonstrate-arithmetic did not run (exit $rc)"; fi

# 6 ---------------------------------------------------------------------------
doc=docs/r16-production-entry-point.md
if [ -f "$doc" ] && grep -q 'test-r13-silicon' "$doc" && grep -q 'test-r14-silicon' "$doc" &&
   grep -q -- '--reference-demonstrate-' "$doc" && grep -q -- '--legacy-oracle-living-matvec' "$doc"; then
    ok "production entry point documented in $doc"
else bad "$doc missing or incomplete"; fi

if [ $fails -eq 0 ]; then echo "R16 gate: R16_G5_SURFACE=PASS"; exit 0; fi
echo "R16 gate: R16_G5_SURFACE=FAIL ($fails)"; exit 1
