#!/bin/sh
set -eu
OMEGA_DIR=$(cd "$(dirname "$0")/../.." && pwd -P)
TOOL="$OMEGA_DIR/build/omegatool"

echo "Running OMEGA Malformed Object Refusal tests..."
# The gate harness verifies malformed object refusal
"$TOOL" --run-gates | grep -E "OMEGA_MALFORMED_OBJECT_REFUSAL_PASS.*PASS"
echo "Malformed refusal tests PASS."
