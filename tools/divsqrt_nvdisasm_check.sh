#!/bin/bash
# divsqrt_nvdisasm_check.sh -- offline provenance for the E1 row 7 kernels.
# Builds the CPU-only test, dumps the DIV, SQRT, EXP2 and LOG2 kernels Omega's own encoder
# emits, disassembles every word with nvdisasm -b SM121 and compares the text
# line by line with omega_ds_listing (what the encoder says each word is).
# Any difference fails. nvdisasm is an offline decoder only; nothing it prints
# is copied into the kernel. No device is opened.
# Run: make test-divsqrt-nvdisasm   (or run this file). Writes only to a temp dir.
set -u
HERE=$(cd -P "$(dirname "$0")/.." && pwd)
NVDISASM=${NVDISASM:-/usr/local/cuda/bin/nvdisasm}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
[ -x "$NVDISASM" ] || { echo "VERDICT NOT_RUN: $NVDISASM missing"; exit 2; }
make -s -C "$HERE" build/test_omega_divsqrt_gb10_cpu >/dev/null || { echo "VERDICT FAIL: build"; exit 1; }
"$HERE/build/test_omega_divsqrt_gb10_cpu" --dump "$TMP" || { echo "VERDICT FAIL: dump"; exit 1; }
echo "nvdisasm: $("$NVDISASM" --version | grep -o "release [0-9.]*, V[0-9.]*")"
rc=0
for op in div sqrt exp2 log2 sigmoid tanh; do
    "$NVDISASM" -b SM121 "$TMP/$op.bin" > "$TMP/$op.raw" 2> "$TMP/$op.err" || { echo "[FAIL] $op: nvdisasm error"; cat "$TMP/$op.err"; rc=1; continue; }
    # keep "/*addr*/ text ;" lines, normalise spacing, drop the hex comments
    sed -n 's|^[[:space:]]*/\*\([0-9a-f]\{4\}\)\*/[[:space:]]*\(.*;\).*$|\1 \2|p' "$TMP/$op.raw" \
        | sed 's/[[:space:]]\{1,\}/ /g; s/ ;$/ ;/' > "$TMP/$op.got"
    words=$(wc -l < "$TMP/$op.lst")
    if diff -u "$TMP/$op.lst" "$TMP/$op.got" > "$TMP/$op.diff"; then
        echo "[PASS] $op: all $words words decode to the encoder's text"
    else
        echo "[FAIL] $op: nvdisasm text differs from the encoder's listing"; head -40 "$TMP/$op.diff"; rc=1
    fi
done
[ $rc = 0 ] && echo "VERDICT PASS" || echo "VERDICT FAIL"
exit $rc
