#!/bin/bash
# numeric_oracle_mutations.sh -- proves the Gate 5 CPU tier is not vacuous.
# Each mutation below changes one piece of arithmetic (or the exit-status rule)
# in a scratch copy, rebuilds the CPU-only Gate 5 test and runs it. The run
# must exit nonzero and report the named test ID as FAIL. Before the
# independent oracle existed, the EXP/LOG and host-instruction mutations
# passed every test because reference and CPU tier shared the same code.
# Host only: opens no device, writes only under a temporary directory.
# Run: make test-numeric-sweep (runs this after the CHECK sweep), or this file.
set -u
HERE=$(cd -P "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
SRCS="tests/test_omega_numeric.c src/omega_numeric.c src/omega_numeric_provenance.c
      src/omega_blackwell_encoder.c src/omega_blackwell_codegen.c src/omega_blackwell_matmul.c
      src/omega_blackwell_qmd.c src/sha256.c"

# name | file | exact text | replacement | test ID that must FAIL (or UNDECLARED_SKIP)
MUTATIONS=(
"exp_coefficient|src/omega_numeric.c|const float c5 = 0.0083333338f;|const float c5 = 0.0093333338f;|CPU_TIER_INDEPENDENT_ORACLE"
"exp_drop_ln2_lo|src/omega_numeric.c|float r = (x - fk * LN2_HI) - fk * LN2_LO;|float r = (x - fk * LN2_HI) - fk * 0.0f * LN2_LO;|CPU_TIER_INDEPENDENT_ORACLE"
"log_coefficient|src/omega_numeric.c|const float c1 = 0.333333343f;|const float c1 = 0.343333343f;|CPU_TIER_INDEPENDENT_ORACLE"
"cpu_fadd_is_fsub|src/omega_numeric.c|case OMEGA_NOP_FADD: __asm__ volatile(\"fadd|case OMEGA_NOP_FADD: __asm__ volatile(\"fsub|CPU_TIER_INDEPENDENT_ORACLE"
"ref_fmul_rounds_twice|src/omega_numeric.c|float omega_ref_fmul(float a, float b) { return a * b; }|float omega_ref_fmul(float a, float b) { return (float)((double)a * (double)b * (1.0 + 0x1p-40)); }|CPU_TIER_INDEPENDENT_ORACLE"
"ref_ffma_is_fmsub|src/omega_numeric.c|__asm__(\"fmadd %s0|__asm__(\"fmsub %s0|CPU_TIER_INDEPENDENT_ORACLE"
"cpu_i2f_unsigned|src/omega_numeric.c|\"scvtf %s0, %w1\"|\"ucvtf %s0, %w1\"|CPU_TIER_INDEPENDENT_ORACLE"
"cpu_rcp_is_fmul|src/omega_numeric.c|case OMEGA_NOP_MUFU_RCP: __asm__ volatile(\"fdiv|case OMEGA_NOP_MUFU_RCP: __asm__ volatile(\"fmul|CPU_TIER_INDEPENDENT_ORACLE"
"ref_lds_identity|src/omega_numeric.c|r = a[i ^ (OMEGA_NUMERIC_CTA_THREADS - 1u)];|r = a[i];|CPU_TIER_INDEPENDENT_ORACLE"
"undeclared_skip|tests/test_omega_numeric.c|    printf(\"\\nGate 5 Results:|    skip(\"NOT_A_CHIP_ONLY_ID\", \"mutation\");@NL@    printf(\"\\nGate 5 Results:|UNDECLARED_SKIP"
)

total=0 killed=0 survived=0 broken=0
for m in "${MUTATIONS[@]}"; do
    IFS='|' read -r name file from to want <<< "$m"
    total=$((total + 1))
    rm -rf "$TMP/tree"; mkdir -p "$TMP/tree/src" "$TMP/tree/tests"
    cp "$HERE"/src/*.c "$HERE"/src/*.h "$TMP/tree/src/"
    cp "$HERE"/tests/test_omega_numeric.c "$HERE"/tests/numeric_oracle.h "$TMP/tree/tests/"
    FROM=$from TO=$to perl -0pi -e '$n += s/\Q$ENV{FROM}\E/$ENV{TO} =~ s{\@NL\@}{\n}gr/ge; END { $? = $n == 1 ? 0 : 3 }' "$TMP/tree/$file"
    if [ $? -ne 0 ]; then
        echo "  [BROKEN]   $name: text not found exactly once in $file"; broken=$((broken + 1)); continue
    fi
    # shellcheck disable=SC2086
    if ! (cd "$TMP/tree" && gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off -Isrc \
            -DOMEGA_NUMERIC_CPU_ONLY -o "$TMP/t" $SRCS) > "$TMP/build.log" 2>&1; then
        echo "  [BROKEN]   $name: scratch copy does not build"; head -5 "$TMP/build.log"; broken=$((broken + 1)); continue
    fi
    "$TMP/t" > "$TMP/run.log" 2>&1; rc=$?
    if [ "$rc" -ne 0 ] && { grep -qx "\[FAIL\] $want" "$TMP/run.log" || grep -q "^Gate 5 Verdict: $want " "$TMP/run.log"; }; then
        echo "  [KILLED]   $name: exit $rc, $want"; killed=$((killed + 1))
    else
        echo "  [SURVIVED] $name: exit $rc, $want not reported"; survived=$((survived + 1))
    fi
done
echo "oracle mutations: $total, $killed caught, $survived survived, $broken broken"
[ "$total" -gt 0 ] && [ "$killed" -eq "$total" ]
