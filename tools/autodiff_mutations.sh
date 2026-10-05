#!/bin/sh
# M21 OMEGA_AUTODIFF mutation sweep. For each rule marked MUT:<name> in
# src/autodiff/*.c, copy src/autodiff to a scratch directory, break that one
# rule, rebuild the CPU test against the copy and require the test to FAIL.
# A mutation that does not apply, does not build, or that the tests do not
# catch, fails the sweep. The source tree is never edited. CPU only, no
# device. Shell only (no Python).
set -u
cd "$(dirname "$0")/.." || exit 2
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/autodiff-mut.XXXXXX") || exit 2
trap 'rm -rf "$SCRATCH"' EXIT INT TERM
TENSOR="src/tensor/omega_tensor.c src/tensor/omega_tensor_cpu.c src/tensor/omega_tensor_reduce_seam.c \
src/omega_numeric_reduce.c src/omega_numeric_transc.c"
DEPS="src/omega_numeric.c src/omega_numeric_provenance.c src/omega_blackwell_encoder.c \
src/omega_blackwell_codegen.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c src/sha256.c"
FLAGS="-std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -ffp-contract=off -O2 -DOMEGA_NUMERIC_CPU_ONLY"

# name|file|sed expression (applied only to the line carrying MUT:<name>)
# DIV_FORMULA computes gb as -((g * y) / b) instead of -(g * (y / b)): the same
# real number with the same rounding count, so it stays inside the per-op
# contract and the 1e-3 FD bound; it is aimed at the bit-exact layer.
MUTATIONS='TAPE_WALK|omega_autodiff.c|s/root + 1; i-- > 0/root; i-- > 0/
ACCUMULATE|omega_autodiff.c|s/omega_tensor_binary(t->ctx, OMEGA_TB_ADD, n->grad, contrib, &sum)/omega_tensor_contiguous(t->ctx, contrib, \&sum)/
UNBROADCAST|omega_autodiff.c|s/if (target->shape/if (0 \&\& target->shape/
SUB_NEGATE|omega_autodiff.c|s/-1\.0f/1.0f/
MUL_OPERAND|omega_autodiff.c|s/t->nodes\[n->in1\]\.value/t->nodes[n->in0].value/
MATMUL_TRANSPOSE|omega_autodiff.c|s/matmul(t->ctx, g, tmp, &c)/matmul(t->ctx, g, nb->value, \&c)/
SQRT_TWICE|omega_autodiff.c|s/omega_tensor_binary(t->ctx, OMEGA_TB_ADD, n->value, n->value, &tmp)/omega_tensor_contiguous(t->ctx, n->value, \&tmp)/
MEAN_DIVISOR|omega_autodiff.c|s/(float)len/(float)(len + 1)/
MAX_ROUTE|omega_autodiff.c|s/return a > b;/return a < b;/
MAX_TIE|omega_autodiff.c|s/return a > b;/return a >= b;/
DIV_B_SIGN|omega_autodiff.c|s/-1\.0f/1.0f/
DIV_FORMULA|omega_autodiff.c|{s/OMEGA_TB_DIV, n->value, nb->value, &tmp)/OMEGA_TB_MUL, g, n->value, \&tmp)/;s/OMEGA_TB_MUL, g, tmp, &c2)/OMEGA_TB_DIV, tmp, nb->value, \&c2)/}'

# Runs on the shared runner tools/mutation_runner.sh (marker reader). This
# script is a thin adapter: it feeds the rows above, then maps the runner's
# per-mutant stderr lines back to the legacy stdout lines and exit codes.
# Rows name src/autodiff/<file>, relative to the runner's scratch tree.
BUILD="gcc $FLAGS -Isrc -Isrc/tensor -Isrc/autodiff -o t tests/test_omega_autodiff.c src/autodiff/omega_autodiff.c \
$TENSOR $DEPS"
printf '%s\n' "$MUTATIONS" | sed 's#^\([A-Z_]*\)|#\1|src/autodiff/#' > "$SCRATCH/rows"
tools/mutation_runner.sh -k marker -m "$SCRATCH/rows" -d . -c "src tests/test_omega_autodiff.c" \
    -b "$BUILD" -t ./t -f FAIL -B > "$SCRATCH/runner.out" 2> "$SCRATCH/runner.err"
if grep -q 'unmutated baseline' "$SCRATCH/runner.out"; then
    cat "$SCRATCH/runner.err"
    echo "autodiff mutation sweep: FAIL (unmutated baseline does not pass)"; exit 1
fi
fail=0
total=0
while IFS= read -r line; do
    case $line in
        *' KILLED '*|*' SURVIVED '*|*' ERROR '*) ;;
        *) continue;;
    esac
    name=${line%% *}
    rest=${line#* }; status=${rest%% *}
    note=${line#* (}; note=${note%)}
    total=$((total + 1))
    case $status in
        KILLED)
            n=0
            case $note in *' failing case(s)'*) n=${note%% failing case(s)*};; esac
            echo "MUTATION $name: caught ($n failing checks)";;
        SURVIVED) echo "MUTATION $name: NOT CAUGHT (tests still pass)"; fail=1;;
        *)
            case $note in
                'edit did not apply'*) echo "MUTATION $name: NOT APPLIED (marker or pattern missing)";;
                *) echo "$note"; echo "MUTATION $name: does not build (fix the sed expression)";;
            esac
            fail=1;;
    esac
done < "$SCRATCH/runner.err"
if [ "$total" -lt 5 ]; then echo "autodiff mutation sweep: FAIL (fewer than 5 mutations)"; exit 1; fi
if [ "$fail" -ne 0 ]; then echo "autodiff mutation sweep: FAIL"; exit 1; fi
echo "autodiff mutation sweep: PASS ($total of $total mutations caught)"
