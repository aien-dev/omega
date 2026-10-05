#!/bin/bash
# numeric_oracle_mutations.sh -- proves the Gate 5 CPU tier is not vacuous.
# Each mutation below changes one piece of arithmetic (or the exit-status rule)
# in a scratch copy, rebuilds the CPU-only Gate 5 test and runs it. The run
# must exit nonzero and report the named test ID as FAIL. Before the
# independent oracle existed, the EXP/LOG and host-instruction mutations
# passed every test because reference and CPU tier shared the same code.
# Host only: opens no device, writes only under a temporary directory.
# Run: make test-numeric-sweep (runs this after the CHECK sweep), or this file.
#
# This file is a thin adapter over tools/mutation_runner.sh. The mutant set is
# the table below (id~file~edit~test ID that must FAIL, edit kind "replace":
# R:<exact text>@>@<replacement>, @NL@ = newline). The runner takes ONE test
# command per call, and this sweep needs a different named FAIL id per mutant,
# so the adapter calls the runner once per row (ONLY=<id>) and maps the runner's
# stderr line back to the legacy [KILLED]/[SURVIVED]/[BROKEN] lines. The judge
# script exits nonzero (KILLED) only if the test binary exited nonzero AND the
# named id is reported; it records the binary's exit status for the legacy line.
set -u
HERE=$(cd -P "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# One line on purpose: $SRCS is interpolated into the runner's -b string, which
# the runner executes with sh -c, so a newline here would split the gcc command.
SRCS="tests/test_omega_numeric.c src/omega_numeric.c src/omega_numeric_provenance.c src/omega_numeric_divsqrt_gb10.c src/omega_blackwell_encoder.c src/omega_blackwell_codegen.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c src/sha256.c"

# id ~ file ~ edit ~ test ID that must FAIL (or UNDECLARED_SKIP)
cat > "$TMP/rows" <<'ROWS_EOF'
exp_coefficient~src/omega_numeric.c~R:const float c5 = 0.0083333338f;@>@const float c5 = 0.0093333338f;~CPU_TIER_INDEPENDENT_ORACLE
exp_drop_ln2_lo~src/omega_numeric.c~R:float r = (x - fk * LN2_HI) - fk * LN2_LO;@>@float r = (x - fk * LN2_HI) - fk * 0.0f * LN2_LO;~CPU_TIER_INDEPENDENT_ORACLE
log_coefficient~src/omega_numeric.c~R:const float c1 = 0.333333343f;@>@const float c1 = 0.343333343f;~CPU_TIER_INDEPENDENT_ORACLE
cpu_fadd_is_fsub~src/omega_numeric.c~R:case OMEGA_NOP_FADD: __asm__ volatile("fadd@>@case OMEGA_NOP_FADD: __asm__ volatile("fsub~CPU_TIER_INDEPENDENT_ORACLE
ref_fmul_rounds_twice~src/omega_numeric.c~R:float omega_ref_fmul(float a, float b) { return a * b; }@>@float omega_ref_fmul(float a, float b) { return (float)((double)a * (double)b * (1.0 + 0x1p-40)); }~CPU_TIER_INDEPENDENT_ORACLE
ref_ffma_is_fmsub~src/omega_numeric.c~R:__asm__("fmadd %s0@>@__asm__("fmsub %s0~CPU_TIER_INDEPENDENT_ORACLE
cpu_i2f_unsigned~src/omega_numeric.c~R:"scvtf %s0, %w1"@>@"ucvtf %s0, %w1"~CPU_TIER_INDEPENDENT_ORACLE
cpu_rcp_is_fmul~src/omega_numeric.c~R:case OMEGA_NOP_MUFU_RCP: __asm__ volatile("fdiv@>@case OMEGA_NOP_MUFU_RCP: __asm__ volatile("fmul~CPU_TIER_INDEPENDENT_ORACLE
ref_lds_identity~src/omega_numeric.c~R:r = a[i ^ (OMEGA_NUMERIC_CTA_THREADS - 1u)];@>@r = a[i];~CPU_TIER_INDEPENDENT_ORACLE
ref_rni_ties_away~src/omega_numeric.c~R:default:        if (frac == 3@>@default:        if (frac >= 2) mag++; else if (frac == 3~E1_SCALAR_INDEPENDENT_ORACLE
cpu_gtu_is_gt~src/omega_numeric.c~R:CPU_SEL("hi")@>@CPU_SEL("gt")~E1_SCALAR_INDEPENDENT_ORACLE
ref_ffma_int_no_sticky~src/omega_numeric.c~R:(lost ? 1U : 0U)@>@(lost ? 0U : 0U)~E1_SCALAR_BOUNDARY_VALUES
undeclared_skip~tests/test_omega_numeric.c~R:    printf("\nGate 5 Results:@>@    skip("NOT_A_CHIP_ONLY_ID", "mutation");@NL@    printf("\nGate 5 Results:~UNDECLARED_SKIP
ROWS_EOF

cat > "$TMP/judge.sh" <<'JUDGE_EOF'
#!/bin/sh
# usage: judge.sh <tmpdir> <test id that must FAIL>
"$1/t" > "$1/run.log" 2>&1; rc=$?
echo "$rc" > "$1/rc"
if [ "$rc" -ne 0 ] && { grep -qx "\[FAIL\] $2" "$1/run.log" || grep -q "^Gate 5 Verdict: $2 " "$1/run.log"; }; then exit 1; fi
exit 0
JUDGE_EOF

total=0 killed=0 survived=0 broken=0
while IFS='~' read -r name file edit want; do
    [ -n "$name" ] || continue
    total=$((total + 1))
    rm -f "$TMP/rc" "$TMP/build.log"
    printf '%s~%s~%s~%s\n' "$name" "$file" "$edit" "$want" > "$TMP/row"
    # shellcheck disable=SC2086
    ONLY=$name "$HERE/tools/mutation_runner.sh" -k table -m "$TMP/row" \
        -d "$HERE" -c "src tests/test_omega_numeric.c tests/numeric_oracle.h" \
        -b "gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off -Isrc -DOMEGA_NUMERIC_CPU_ONLY -o $TMP/t $SRCS > $TMP/build.log 2>&1" \
        -t "sh $TMP/judge.sh $TMP $want" -o "$TMP/out.json" > /dev/null 2> "$TMP/runner.err" < /dev/null
    status=$(sed -n "s/^$name \([A-Z_]*\)  .*/\1/p" "$TMP/runner.err" | head -1)
    rc=$(cat "$TMP/rc" 2>/dev/null)
    case $status in
        KILLED) echo "  [KILLED]   $name: exit $rc, $want"; killed=$((killed + 1));;
        SURVIVED) echo "  [SURVIVED] $name: exit $rc, $want not reported"; survived=$((survived + 1));;
        *)
            if grep -q "^$name ERROR  .*(build failed" "$TMP/runner.err"; then
                echo "  [BROKEN]   $name: scratch copy does not build"; head -5 "$TMP/build.log"
            else
                echo "  [BROKEN]   $name: text not found exactly once in $file"
            fi
            broken=$((broken + 1));;
    esac
done < "$TMP/rows"
echo "oracle mutations: $total, $killed caught, $survived survived, $broken broken"
[ "$total" -gt 0 ] && [ "$killed" -eq "$total" ]
