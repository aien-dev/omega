#!/bin/bash
# FB-1 cut 3d gate: host checks, then chip parity (matmul, elementwise, attention), then strict TinyLlama. Not killed by us.
set -u
cd ~/workspace/hive-worktrees/sc-fb1-cut3d
OUT=$HOME/workspace/evidence-out/FB1-CUT3D-02dfa2e
git rev-parse HEAD > $OUT/sha.txt; git status --porcelain | wc -l >> $OUT/sha.txt
( cargo fmt --all -- --check && AIEN_FORCE_CPU_STUB=1 cargo clippy -p aien-inference-abi -p aien-omega-gpu --tests -- -D warnings \
  && AIEN_FORCE_CPU_STUB=1 AIEN_DEV_FALLBACK=1 cargo test -p aien-omega-gpu -p aien-inference-abi ) > $OUT/host.log 2>&1; echo "host exit $?" >> $OUT/host.log
export AIEN_OMEGA_DIR=$HOME/workspace/hive-worktrees/omega-pin-3d4648c AIEN_PHYSICS_DIR=$HOME/workspace/hive-worktrees/qual-physics-6d7cf0d
cargo test -p aien-inference-abi --release --test omega_backend_parity -- --ignored --nocapture --test-threads=1 > $OUT/parity.log 2>&1; echo "parity exit $?" >> $OUT/parity.log
AIEN_E2E_CHECKPOINT=$HOME/models/TinyLlama-1.1B-Chat-v1.0 AIEN_STRICT_RECEIPT=$OUT/strict-receipt.json \
cargo test -p aien-inference-runtime --release --test strict_real_model omega_vs_reference_real_model -- --ignored --nocapture --test-threads=1 > $OUT/gate.log 2>&1; echo "gate exit $?" >> $OUT/gate.log
tail -1 $OUT/host.log; tail -1 $OUT/parity.log; tail -1 $OUT/gate.log
