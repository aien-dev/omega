#!/bin/sh
# Deterministic generator of valid OSC units for the checker2 gate (omega#446): writes
# <out>/gen_NN.osc for NN in 00..(count-1). Each unit varies the scalar type, array length, loop
# bound and shape (loops, borrows, structs, drop-flag moves, casts, contracts) by its index. No
# randomness: the same count gives the same files. The producer (oscc) must accept every one;
# that is checked by tests/checker2/differential.sh, not assumed here.
# Usage: sh tests/checker2/gen_valid.sh <out-dir> [count]
set -eu
OUT=$1
N=${2:-48}
mkdir -p "$OUT"
set -- u8 u16 u32 u64 i8 i16 i32 i64
i=0
while [ "$i" -lt "$N" ]; do
    k=$((i % 8 + 1))
    T=$(eval echo "\${$k}")
    L=$((i % 7 + 2))
    B=$((10 + i * 3))
    C=$((i % 5 + 1))
    f=$(printf '%s/gen_%02d.osc' "$OUT" "$i")
    case $((i % 6)) in
    0) cat > "$f" <<EOF
// entry: main
// generated shape 0: owned array, for loop, while-bound loop ($T, len $L, bound $B)
fn main(x: $T) -> $T {
    let a: own [$T; $L] = alloc(x);
    for j in 0 .. $L { a[j] = a[j] ^ (j as $T); }
    let mut n: u32 = 0;
    while n < $C bound $B { n = n + 1; }
    let r: $T = a[0] | a[$((L - 1))];
    return r;
}
EOF
    ;;
    1) cat > "$f" <<EOF
// entry: main
// generated shape 1: shared and mutable borrows into helpers ($T, len $L)
fn fill(a: &mut [$T; $L], v: $T) {
    for j in 0 .. $L { a[j] = v; }
}
fn first(a: &[$T; $L]) -> $T { return a[0]; }
fn main(v: $T) -> $T {
    let a: own [$T; $L] = alloc(v);
    fill(&mut a, v);
    let r: $T = first(&a);
    return r;
}
EOF
    ;;
    2) cat > "$f" <<EOF
// entry: main
// generated shape 2: struct with scalar and array fields ($T, len $L)
struct S$i { tag: u8, vals: [$T; $L], ok: bool }
fn main(v: $T, t: u8) -> $T {
    let s: own S$i = S$i { tag: t, vals: [v; $L], ok: t > $C };
    if s.ok { s.vals[0] = v & v; }
    s.tag = s.tag | 1;
    return s.vals[$((L - 1))];
}
EOF
    ;;
    3) cat > "$f" <<EOF
// entry: main
// generated shape 3: conditional move with a drop flag ($T, len $L)
fn take(x: own [$T; $L]) -> $T { return x[0]; }
fn main(c: bool, v: $T) -> $T {
    let a: own [$T; $L] = alloc(v);
    let mut r: $T = v;
    if c { r = take(a); }
    return r;
}
EOF
    ;;
    4) cat > "$f" <<EOF
// entry: main
// generated shape 4: casts, shifts, comparisons and a counted loop ($T, bound $B)
fn main(x: $T, s: u8) -> u64 {
    let mut acc: u64 = 0;
    let mut n: u16 = 0;
    while n < $C bound $B {
        acc = acc + ((x as u64) >> (s & 7));
        n = n + 1;
    }
    if (x as u64) >= acc { return acc; }
    return acc << 1;
}
EOF
    ;;
    5) cat > "$f" <<EOF
// entry: main
// generated shape 5: contracts and a chain of calls ($T)
fn clamp(v: $T, hi: $T) -> $T requires hi >= 0 ensures result <= hi {
    if v > hi { return hi; }
    return v;
}
fn main(v: $T) -> $T {
    let a: $T = clamp(v, $C);
    let b: $T = clamp(a, $((C + 1)));
    return a + b;
}
EOF
    ;;
    esac
    i=$((i + 1))
done
