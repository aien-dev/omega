#!/bin/bash
# tests/run_numeric_gates.sh -- Gate 5 (OMEGA-NUMERIC-0) runner
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd -P)
PHYSICS_DIR=${PHYSICS_DIR:-"$(cd "$REPO_ROOT/../physics-forge-hwid" && pwd -P)"}
LOCK_FILE=/tmp/aien-gb10.lock

cd "$REPO_ROOT"

echo "=== M19R OMEGA-NUMERIC-0 Gate Runner ==="
echo "Timestamp: $(date -u +"%Y-%m-%dT%H:%M:%SZ")"
echo "Host: $(uname -n) ($(uname -m))"
echo "Physics dir: $PHYSICS_DIR"

CFLAGS="-O2 -Wall -Wextra -Werror \
  -I./src \
  -I$PHYSICS_DIR/forge -I$PHYSICS_DIR/nvrm -I$PHYSICS_DIR/m16 \
  -I$PHYSICS_DIR/third_party/nvidia-open-580.173.02/src/common/sdk/nvidia/inc \
  -I$PHYSICS_DIR/third_party/nvidia-open-580.173.02/kernel-open/common/inc \
  -I$PHYSICS_DIR/third_party/nvidia-open-580.173.02/kernel-open/nvidia-uvm \
  -I$PHYSICS_DIR/third_party/nvidia-open-580.173.02/src/nvidia/arch/nvalloc/unix/include"

SRCS="src/omega_blackwell_codegen.c \
      src/omega_blackwell_encoder.c \
      src/omega_numeric_provenance.c \
      src/omega_numeric.c \
      src/forge_realization.c \
      src/aegis_verification.c \
      src/sha256.c \
      src/omega_blackwell_matmul.c \
      src/omega_blackwell_qmd.c \
      $PHYSICS_DIR/forge/forge_descriptor.c \
      $PHYSICS_DIR/forge/forge_realize.c \
      $PHYSICS_DIR/sha256_clean.c \
      $PHYSICS_DIR/nvrm/nvrm.c \
      $PHYSICS_DIR/m16/m16_native.c"

echo -e "\n[*] Compiling test_omega_numeric (without libm)..."
gcc $CFLAGS $SRCS tests/test_omega_numeric.c -o tests/test_omega_numeric

echo -e "\n[*] Auditing zero-libm compliance..."
nm -u tests/test_omega_numeric | grep -E "sqrt|sin|cos|exp|log|pow|round|fabs|fma" && {
    echo "[-] FAILED: libm mathematical symbols detected in sovereign binary!"
    exit 1
} || echo "[+] ZERO LIBM CONFIRMED: No libm symbols found in sovereign binary."

echo -e "\n[*] Running Gate 5: OMEGA-NUMERIC-0 under GPU lock ($LOCK_FILE)..."
flock "$LOCK_FILE" ./tests/test_omega_numeric

echo -e "\n=== M19R OMEGA-NUMERIC-0 Gate Complete ==="
