#!/bin/sh
# Bit-blaster semantics against the producer's compiled code (omega#446, finding 4 of the PR #448
# review). Writes one tiny OSC program per blastable operation and type, compiles each with oscc
# (run as a black box: IR bytes, AArch64 code and entry table), adds the LRAT gate programs, and
# runs test_c2_blast_native, which compares checker2's CNF with the code input by input (see that
# file for the method and the negative controls).
#   op_<op>_<T>          fn f(a: T, b: T, c: T) -> T ensures result != c { return a <op> b; }
#                        add sub mul and or xor shl (all 8 integer types), shr (unsigned only)
#   op_<cmp>_<T>         fn f(a: T, b: T, c: bool) -> bool ensures result != c { return a <cmp> b; }
#   op_neg_<T> (signed), op_bnot_<T>, op_lnot_bool, op_land_bool, op_lor_bool, op_beq_bool
#   op_cast_<S>_<T>      every ordered pair of distinct integer types
# Any program oscc refuses, or that checker2 does not blast, fails the run.
# Usage: blast_native.sh <oscc> <test_c2_blast_native> <work-dir>
set -u
OSCC=$1 H=$2 W=$3
D=$(dirname "$0")/lrat
rm -rf "$W" && mkdir -p "$W/src" || exit 2
types="u8 u16 u32 u64 i8 i16 i32 i64"
fail=0
gen() { # gen <name> <params> <ret> <expr>
    printf 'fn f(%s) -> %s ensures result != c {\n    return %s;\n}\n' "$2" "$3" "$4" >"$W/src/$1.osc"
}
for t in $types; do
    for op in add:+ sub:- mul:'*' and:'&' or:'|' xor:'^' shl:'<<' shr:'>>'; do
        n=${op%%:*} s=${op#*:}
        case "$n:$t" in shr:i*) continue ;; esac
        gen "op_${n}_$t" "a: $t, b: $t, c: $t" "$t" "a $s b"
    done
    for op in eq:== ne:!= lt:'<' le:'<=' gt:'>' ge:'>='; do
        gen "op_${op%%:*}_$t" "a: $t, b: $t, c: bool" bool "a ${op#*:} b"
    done
    case $t in i*) gen "op_neg_$t" "a: $t, c: $t" "$t" "-a" ;; esac
    gen "op_bnot_$t" "a: $t, c: $t" "$t" "~a"
    for t2 in $types; do
        [ "$t" = "$t2" ] || gen "op_cast_${t}_$t2" "a: $t, c: $t2" "$t2" "a as $t2"
    done
done
gen op_lnot_bool "a: bool, c: bool" bool "!a"
gen op_land_bool "a: bool, b: bool, c: bool" bool "a && b"
gen op_lor_bool "a: bool, b: bool, c: bool" bool "a || b"
gen op_beq_bool "a: bool, b: bool, c: bool" bool "a == b"
cp "$D"/*.osc "$W/src/"
for src in "$W"/src/*.osc; do
    b=$(basename "$src" .osc)
    "$OSCC" "$src" "$W/$b" >"$W/src/$b.log" 2>&1 || { echo "blast_native: FAIL oscc refused $b: $(head -c 300 "$W/src/$b.log")"; fail=$((fail + 1)); }
done
# negative controls: CNF of the first program against the code of the second (same signature)
cat >"$W/controls" <<'CTL'
op_add_u8 op_sub_u8
op_and_u16 op_or_u16
op_shl_u8 op_shr_u8
op_lt_i8 op_le_i8
op_mul_i64 op_add_i64
op_neg_i32 op_bnot_i32
op_eq_u64 op_ne_u64
op_cast_i8_u16 op_cast_i8_i16
CTL
echo "blast_native: $(ls "$W"/src/*.osc | wc -l) programs written, $fail refused by oscc"
"$H" "$W" || fail=$((fail + 1))
[ "$fail" -eq 0 ] || { echo "blast_native: FAIL"; exit 1; }
echo "blast_native: PASS"
