#!/bin/sh
# Lane 32: negative build test. The production program must REFUSE, at build
# time, every test-only piece (docs/r16-production-entry-point.md). Each case
# compiles with the production flags (no AIEN_TEST_BUILD) and must FAIL with
# its guard's message; three controls must SUCCEED, so a failure is the guard,
# not a broken setup. Run by `make test-prod-refuses-test-pieces` (rx-host CI).
#
# Environment (set by the Makefile): CC, CFLAGS, ARGUS_SRC, OUT, MAKE, PROD_BIN.
set -u
: "${CC:=cc}" "${PROD_BIN:?}" "${CFLAGS?}" "${ARGUS_SRC:?}" "${OUT:?}" "${MAKE:=make}"
mkdir -p "$OUT" || exit 2
INC="-Isrc -Itests/runtime -Itests/fabric -Isrc/runtime -Isrc/fabric"
ARGUSF="-DRX_ARGUS=2 -DRX_ARGUS_AUTHORITY_OBSERVER -I$ARGUS_SRC"
PROD="$ARGUSF $INC"
bad=0; n=0
expect_fail() {  # tag message-regex compile-args...
    tag=$1; want=$2; shift 2; n=$((n + 1))
    # shellcheck disable=SC2086
    if $CC $CFLAGS $PROD "$@" -c -o "$OUT/$tag.o" > "$OUT/$tag.log" 2>&1; then
        echo "R16 refuse FAIL: $tag compiled in the production build (it must not)"; bad=1
    elif grep -Eq "$want" "$OUT/$tag.log"; then
        echo "R16 refuse: $tag refused at build time ($(grep -Eo "$want" "$OUT/$tag.log" | head -1))"
    else
        echo "R16 refuse FAIL: $tag failed to compile, but not on its guard ($want):"
        head -5 "$OUT/$tag.log"; bad=1
    fi
}
expect_ok() {  # tag compile-args...
    tag=$1; shift; n=$((n + 1))
    # shellcheck disable=SC2086
    if $CC $CFLAGS "$@" -c -o "$OUT/$tag.o" > "$OUT/$tag.log" 2>&1; then
        echo "R16 refuse: control $tag compiles"
    else
        echo "R16 refuse FAIL: control $tag does not compile:"; head -5 "$OUT/$tag.log"; bad=1
    fi
}

guard='test and simulation only: build with -DAIEN_TEST_BUILD=1'
expect_fail fab_loopback "$guard" src/fabric/fab_loopback.c
expect_fail fab_hmac "$guard" src/fabric/fab_hmac.c
expect_fail fab_dispatch "$guard" src/fabric/fab_dispatch.c
printf '#include "fab_living_phase.h"\nint main(void) { return 0; }\n' > "$OUT/living_phase.c"
expect_fail fab_living_phase "$guard" "$OUT/living_phase.c"
expect_fail rxc_test_hooks 'RXC_TEST_HOOKS .*needs -DAIEN_TEST_BUILD=1' -DRXC_TEST_HOOKS src/runtime/rx_compose.c
# ARGUS flags left out: the production R13 source refuses to build.
n=$((n + 1)); tag=r13_without_argus
# shellcheck disable=SC2086
if $CC $CFLAGS $INC tests/runtime/rx_r13_living.c -c -o "$OUT/$tag.o" > "$OUT/$tag.log" 2>&1; then
    echo "R16 refuse FAIL: $tag compiled without ARGUS (it must not)"; bad=1
elif grep -q "production R13 program links ARGUS" "$OUT/$tag.log"; then
    echo "R16 refuse: $tag refused at build time (production R13 program links ARGUS)"
else
    echo "R16 refuse FAIL: $tag failed, but not on its guard:"; head -5 "$OUT/$tag.log"; bad=1
fi
# Controls: the production R13 source and rx_compose.c compile in the
# production build; the Fabric loopback compiles in the test build.
expect_ok r13_production $PROD tests/runtime/rx_r13_living.c
expect_ok rx_compose_production $PROD src/runtime/rx_compose.c
expect_ok fab_loopback_testbuild -DAIEN_TEST_BUILD=1 -Isrc -Isrc/runtime -Isrc/fabric src/fabric/fab_loopback.c

# The Makefile refuses the flag in CFLAGS for the production binaries.
n=$((n + 1))
if $MAKE --no-print-directory -n -B "CFLAGS=$CFLAGS -DAIEN_TEST_BUILD=1" "${PROD_BIN:?}" \
        > "$OUT/make_flag.log" 2>&1; then
    echo "R16 refuse FAIL: make accepted AIEN_TEST_BUILD in CFLAGS for the production program"; bad=1
elif grep -q 'the production program never builds with AIEN_TEST_BUILD' "$OUT/make_flag.log"; then
    echo "R16 refuse: make refuses AIEN_TEST_BUILD in CFLAGS for the production program"
else
    echo "R16 refuse FAIL: make failed, but not on the production guard:"; head -5 "$OUT/make_flag.log"; bad=1
fi

if [ $bad -ne 0 ]; then
    echo "R16 prod: PROD_REFUSES_TEST_PIECES=FAIL ($n cases)"; exit 1
fi
echo "R16 prod: PROD_REFUSES_TEST_PIECES=PASS ($n cases: 6 refusals, 3 controls, 1 make guard)"
