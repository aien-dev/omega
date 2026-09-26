#!/bin/sh
set -eu
OMEGA_DIR=$(cd "$(dirname "$0")/../.." && pwd -P)
TOOL="$OMEGA_DIR/build/omegatool"

echo "Running OMEGA Canonicalization tests..."
"$TOOL" --run-gates | grep -E "OMEGA_CANONICAL_ENCODING_PASS.*PASS"
echo "Canonicalization tests PASS."
