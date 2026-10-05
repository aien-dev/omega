#!/bin/bash
# FB-1 cut 3c diagnostic: mixed matmul then elementwise. Not killed by us.
set -u
cd ~/workspace/hive-worktrees/sc-fb1-cut3c
export AIEN_OMEGA_DIR=$HOME/workspace/hive-worktrees/omega-pin-2636409 AIEN_PHYSICS_DIR=$HOME/workspace/hive-worktrees/qual-physics-6d7cf0d
OUT=$HOME/workspace/evidence-out/FB1-CUT3C-21f995d
AIEN_DEV_FALLBACK=1 cargo test -p aien-inference-abi --release --test omega_backend_parity chip_mixed -- --ignored --nocapture --test-threads=1 > $OUT/diag.log 2>&1; echo "diag exit $?" >> $OUT/diag.log
