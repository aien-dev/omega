#!/bin/sh
set -eu
OMEGA_DIR=$(cd "$(dirname "$0")/../.." && pwd -P)
TOOL="$OMEGA_DIR/build/omegatool"

echo "Running OMEGA Relations tests..."
"$TOOL" --run-gates | grep -E "OMEGA_RELATION_PASS.*PASS"
echo "Relations tests PASS."
