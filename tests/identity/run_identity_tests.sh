#!/bin/sh
set -eu
OMEGA_DIR=$(cd "$(dirname "$0")/../.." && pwd -P)
TOOL="$OMEGA_DIR/build/omegatool"

echo "Running OMEGA Identity and Representation Independence tests..."
"$TOOL" --reference-demonstrate-arithmetic
echo "Identity tests PASS."
