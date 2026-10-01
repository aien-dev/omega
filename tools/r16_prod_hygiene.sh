#!/bin/sh
# Lane 32: production program hygiene (docs/r16-production-entry-point.md).
#
# Usage: tools/r16_prod_hygiene.sh host|silicon <production-r13-binary>
#   R16_PROD_NO_RUN=1: static checks only (link map and strings); the ARGUS
#   probe is not run (used by tools/r16_authpath.sh check 2b).
#
# Checks, stopping at the first failure:
#   1. link map: no test-build piece. No aien_test_build_* marker (Fabric
#      loopback transport, HMAC stand-in, in-process dispatcher, Fabric living
#      phase, rx_compose test hooks), no Fabric symbol (fab_*), no Fabric
#      living phase (fl_*) and no composition test fixture (fx_*).
#   2. strings: no fixed test key (fl-key-*) and no test-build marker text.
#   3. ARGUS linked: the runtime producer (rx_argus_*), the authority observer
#      wrap (__wrap_aienos_cap_start) and the pinned argus_core/argus_detect.
#   4. host run (unless R16_PROD_NO_RUN=1): `<binary> --argus-probe` starts one
#      authority through the wrapped aienos_cap_start and must report
#      ARGUS_OBSERVING=PASS; with RX_ARGUS_AUTO=0 (ARGUS not started) the same
#      binary must refuse to run (fail closed). The probe runs no living
#      episode and never starts the graphics processor.
set -u
mode=${1:-}; bin=${2:-}
if [ $# -ne 2 ] || { [ "$mode" != host ] && [ "$mode" != silicon ]; }; then
    echo "usage: $0 host|silicon <production-r13-binary>" >&2
    exit 2
fi
fail() { echo "R16 prod FAIL: $*"; echo "R16 prod: PROD_HYGIENE=FAIL mode=$mode"; exit 1; }
[ -x "$bin" ] || fail "binary missing: $bin"
n=$(basename "$bin")
tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT

# 1. link map ------------------------------------------------------------------
nm "$bin" > "$tmp/nm" 2>&1 || fail "nm failed on $bin"
awk '{print $NF}' "$tmp/nm" > "$tmp/syms"
grep -qx 'main' "$tmp/syms" || fail "$n has no symbol table (stripped?); cannot check its link map"
if grep -E '^(aien_test_build_|fab_|fl_|fx_)' "$tmp/syms" > "$tmp/bad-syms"; then
    fail "$n links test-build pieces: $(head -5 "$tmp/bad-syms" | tr '\n' ' ')"
fi

# 2. strings -------------------------------------------------------------------
if strings -a "$bin" | grep -E 'fl-key-|AIEN_TEST_BUILD piece:' > "$tmp/bad-strings"; then
    fail "$n embeds test keys or test-build pieces: $(head -3 "$tmp/bad-strings" | tr '\n' ' ')"
fi

# 3. ARGUS linked ----------------------------------------------------------------
for s in rx_argus_active rx_argus_shutdown rx_argus_stats __wrap_aienos_cap_start \
         argus_core_init argus_core_ingest argus_detect_run; do
    grep -qx "$s" "$tmp/syms" || fail "$n does not link ARGUS ($s missing)"
done
printf 'link map clean, ARGUS linked (rx_argus, authority observer wrap, argus_core, argus_detect)' > "$tmp/summary"

# 4. host run of the ARGUS probe ---------------------------------------------------
if [ "${R16_PROD_NO_RUN:-0}" != 1 ]; then
    env -u RX_ARGUS_AUTO -u RX_ARGUS_CONSUMER -u RX_ARGUS_STREAM -u RX_ARGUS_SUMMARY \
        "$bin" --argus-probe > "$tmp/probe.out" 2>&1
    rc=$?
    sed 's/^/R16 prod   /' "$tmp/probe.out"
    [ $rc -eq 0 ] && grep -q '^R13 ARGUS probe: ARGUS_OBSERVING=PASS$' "$tmp/probe.out" \
        || fail "ARGUS probe did not observe (exit $rc)"
    RX_ARGUS_AUTO=0 "$bin" --argus-probe > "$tmp/probe-off.out" 2>&1
    rc=$?
    if [ $rc -eq 0 ] || grep -q 'ARGUS_OBSERVING=PASS' "$tmp/probe-off.out"; then
        fail "with ARGUS not started the program still ran (exit $rc): it must refuse to run unobserved"
    fi
    printf '; ARGUS probe observing; refuses to run unobserved' >> "$tmp/summary"
fi
echo "R16 prod: $(cat "$tmp/summary")"
echo "R16 prod: PROD_HYGIENE=PASS mode=$mode binary=$n"
