#!/bin/sh
set -eu
OMEGA_DIR=$(cd "$(dirname "$0")/../.." && pwd -P)
TOOL="$OMEGA_DIR/build/omegatool"

echo "Running OMEGA Constraints and Physics Authority Law tests..."
"$TOOL" --demonstrate-physics
"$TOOL" --run-gates | grep -E "OMEGA_CONSTRAINT_PASS.*PASS"
echo "Constraints tests PASS."
