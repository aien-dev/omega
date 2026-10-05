#!/bin/bash
# FB-1 cut 3c diagnostic 2: real-model run with dev fallback so the chip error text is visible. Not killed by us.
set -u
cd ~/workspace/hive-worktrees/sc-fb1-cut3c
export AIEN_OMEGA_DIR=$HOME/workspace/hive-worktrees/omega-pin-2636409 AIEN_PHYSICS_DIR=$HOME/workspace/hive-worktrees/qual-physics-6d7cf0d
OUT=$HOME/workspace/evidence-out/FB1-CUT3C-21f995d
AIEN_DEV_FALLBACK=1 AIEN_E2E_CHECKPOINT=$HOME/models/TinyLlama-1.1B-Chat-v1.0 AIEN_STRICT_RECEIPT=$OUT/diag2-receipt.json \
cargo test -p aien-inference-runtime --release --test strict_real_model omega_vs_reference_real_model -- --ignored --nocapture --test-threads=1 > $OUT/diag2.log 2>&1; echo "diag2 exit $?" >> $OUT/diag2.log
