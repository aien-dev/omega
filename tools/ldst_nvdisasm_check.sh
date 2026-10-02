#!/bin/bash
# ldst_nvdisasm_check.sh -- offline provenance for the E1 row 2 load/store kernels.
# Builds the CPU-only test, dumps every table kernel (src/omega_numeric_ldst_gb10.c: load/store
# kinds, offsets, strides), disassembles each with nvdisasm -b SM121 and compares the text line by
# line with omega_ldst_listing (what the encoder says each word is). Any difference fails.
# nvdisasm is an offline decoder only; nothing it prints is copied into a kernel. No device.
# Run: make test-ldst-nvdisasm   Writes only to a temp dir.
set -u
HERE=$(cd -P "$(dirname "$0")/.." && pwd)
NVDISASM=${NVDISASM:-/usr/local/cuda/bin/nvdisasm}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
[ -x "$NVDISASM" ] || { echo "VERDICT NOT_RUN: $NVDISASM missing"; exit 2; }
make -s -C "$HERE" build/test_omega_ldst_gb10_cpu >/dev/null || { echo "VERDICT FAIL: build"; exit 1; }
"$HERE/build/test_omega_ldst_gb10_cpu" --dump "$TMP" >/dev/null || { echo "VERDICT FAIL: dump"; exit 1; }
echo "nvdisasm: $("$NVDISASM" --version | grep -o "release [0-9.]*, V[0-9.]*")"
rc=0; n=0; bad=0
for bin in "$TMP"/k*.bin; do
    k=$(basename "$bin" .bin)
    "$NVDISASM" -b SM121 "$bin" > "$TMP/$k.raw" 2> "$TMP/$k.err" || { echo "[FAIL] $k: nvdisasm error"; cat "$TMP/$k.err"; rc=1; bad=$((bad+1)); continue; }
    sed -n 's|^[[:space:]]*/\*\([0-9a-f]\{4\}\)\*/[[:space:]]*\(.*;\).*$|\1 \2|p' "$TMP/$k.raw" \
        | sed 's/[[:space:]]\{1,\}/ /g; s/ ;$/ ;/' > "$TMP/$k.got"
    n=$((n+1))
    if ! diff -u "$TMP/$k.lst" "$TMP/$k.got" > "$TMP/$k.diff"; then
        echo "[FAIL] $k: nvdisasm text differs from the encoder's listing"; head -20 "$TMP/$k.diff"; rc=1; bad=$((bad+1))
    fi
done
echo "[$([ $bad = 0 ] && echo PASS || echo FAIL)] ldst: $((n-bad)) of $n kernels decode, every word, to the encoder's text"
[ $n -gt 0 ] || rc=1
[ $rc = 0 ] && echo "VERDICT PASS" || echo "VERDICT FAIL"
exit $rc
