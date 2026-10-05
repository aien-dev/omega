#!/bin/bash
# FB-1 cut 3b chip gate: parity then real-model run. Not killed by us.
set -u
cd ~/workspace/hive-worktrees/sc-fb1-cut3b
export AIEN_OMEGA_DIR=$HOME/workspace/hive-worktrees/omega-pin-5e29b82 AIEN_PHYSICS_DIR=$HOME/workspace/hive-worktrees/qual-physics-6d7cf0d
OUT=$HOME/workspace/evidence-out/FB1-CUT3B-wip
git rev-parse HEAD > $OUT/sha.txt
cargo test -p aien-inference-abi --release --test omega_backend_parity -- --ignored --nocapture --test-threads=1 > $OUT/parity.log 2>&1; echo "parity exit $?" >> $OUT/parity.log
AIEN_E2E_CHECKPOINT=$HOME/models/TinyLlama-1.1B-Chat-v1.0 AIEN_STRICT_RECEIPT=$OUT/strict-receipt.json \
cargo test -p aien-inference-runtime --release --test strict_real_model omega_vs_reference_real_model -- --ignored --nocapture --test-threads=1 > $OUT/gate.log 2>&1; echo "gate exit $?" >> $OUT/gate.log
