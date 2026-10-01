#!/bin/sh
# M20 OMEGA_TENSOR mutation sweep. For each rule marked MUT:<name> in
# src/tensor/*.c, copy src/tensor to a scratch directory, break that one rule,
# rebuild the CPU test against the copy and require the test to FAIL.
# A mutation that does not apply, or that the tests do not catch, fails the
# sweep. CPU only, no device. Shell only (no Python).
set -u
cd "$(dirname "$0")/.." || exit 2
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/tensor-mut.XXXXXX") || exit 2
trap 'rm -rf "$SCRATCH"' EXIT INT TERM
DEPS="src/omega_numeric_reduce.c src/omega_numeric.c src/omega_numeric_provenance.c src/omega_blackwell_encoder.c \
src/omega_blackwell_codegen.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c src/sha256.c"
FLAGS="-std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -ffp-contract=off -O2 -DOMEGA_NUMERIC_CPU_ONLY"

# name|file|sed expression (applied only to the line carrying MUT:<name>)
# Reduction order and padding mutations live in the E1 WP-D suite
# (test-numeric-reduce-cpu, RED_DIFFERENT_ORDER_CAUGHT): the seam calls it.
MUTATIONS='RELEASE_BUMP|omega_tensor.c|s/(\*gen)++/(void)0/
STORAGE_GEN_CHECK|omega_tensor.c|s/ || s->gen != h.generation//
BCAST_ZERO_STRIDE|omega_tensor.c|s/o.strides\[d\] = 0;/o.strides[d] = in->strides[sd];/
PERMUTE_STRIDES|omega_tensor.c|s/in->strides\[perm\[d\]\]/in->strides[d]/
SLICE_OFFSET|omega_tensor.c|s/start\[d\], in->strides\[d\]/start[d], 1/'

# Baseline: the unmutated build must PASS, otherwise every mutant would
# look "caught" (e.g. the test refusing an unsuitable FP environment).
if ! gcc $FLAGS -Isrc -Isrc/tensor -o "$SCRATCH/base" tests/test_omega_tensor.c \
    src/tensor/omega_tensor.c src/tensor/omega_tensor_cpu.c src/tensor/omega_tensor_reduce_seam.c $DEPS \
    > "$SCRATCH/base.log" 2>&1 || ! "$SCRATCH/base" > "$SCRATCH/base.run" 2>&1; then
    head -5 "$SCRATCH/base.log" "$SCRATCH/base.run" 2>/dev/null
    echo "tensor mutation sweep: FAIL (unmutated baseline does not pass)"; exit 1
fi
fail=0
total=0
old_ifs=$IFS
IFS='
'
for m in $MUTATIONS; do
    IFS=$old_ifs
    IFS='|' read -r name file expr <<EOM
$m
EOM
    total=$((total + 1))
    rm -rf "$SCRATCH/tensor" && cp -r src/tensor "$SCRATCH/tensor"
    sed -i "/MUT:$name/ $expr" "$SCRATCH/tensor/$file"
    if cmp -s "src/tensor/$file" "$SCRATCH/tensor/$file"; then
        echo "MUTATION $name: NOT APPLIED (marker or pattern missing)"; fail=1; continue
    fi
    if ! gcc $FLAGS -Isrc -I"$SCRATCH/tensor" -o "$SCRATCH/t" tests/test_omega_tensor.c \
        "$SCRATCH/tensor/omega_tensor.c" "$SCRATCH/tensor/omega_tensor_cpu.c" \
        "$SCRATCH/tensor/omega_tensor_reduce_seam.c" $DEPS > "$SCRATCH/build.log" 2>&1; then
        head -5 "$SCRATCH/build.log"
        echo "MUTATION $name: does not build (fix the sed expression)"; fail=1; continue
    fi
    if "$SCRATCH/t" > "$SCRATCH/run.log" 2>&1; then
        echo "MUTATION $name: NOT CAUGHT (tests still pass)"; fail=1
    else
        echo "MUTATION $name: caught ($(grep -c '^FAIL' "$SCRATCH/run.log") failing checks)"
    fi
done
IFS=$old_ifs
if [ "$fail" -ne 0 ]; then echo "tensor mutation sweep: FAIL"; exit 1; fi
echo "tensor mutation sweep: PASS ($total of $total mutations caught)"
