#!/bin/bash
# FB-1 cut 6 part A gate: CUDA backend deleted, Omega is the default GPU path.
# Host checks, chip parity, strict TinyLlama gate (no AIEN_GPU_BACKEND switch), strict receipt gate. Not killed by us.
set -u
cd ~/workspace/hive-worktrees/sc-fb1-cut6
OUT=$HOME/workspace/evidence-out/FB1-CUT6A
git rev-parse HEAD > $OUT/sha.txt; git status --porcelain | wc -l >> $OUT/sha.txt
unset AIEN_GPU_BACKEND
( cargo fmt --all -- --check && AIEN_FORCE_CPU_STUB=1 cargo clippy -p aien-inference-abi -p aien-omega-gpu -p aien-inference-runtime --tests -- -D warnings \
  && AIEN_FORCE_CPU_STUB=1 AIEN_DEV_FALLBACK=1 cargo test -p aien-omega-gpu -p aien-inference-abi -p aien-inference-runtime ) > $OUT/host.log 2>&1; echo "host exit $?" >> $OUT/host.log
export AIEN_OMEGA_DIR=$HOME/workspace/hive-worktrees/omega-pin-3d4648c AIEN_PHYSICS_DIR=$HOME/workspace/hive-worktrees/qual-physics-6d7cf0d
cargo test -p aien-inference-abi --release --test omega_backend_parity -- --ignored --nocapture --test-threads=1 > $OUT/parity.log 2>&1; echo "parity exit $?" >> $OUT/parity.log
AIEN_E2E_CHECKPOINT=$HOME/models/TinyLlama-1.1B-Chat-v1.0 AIEN_STRICT_RECEIPT=$OUT/strict-receipt-cmp.json \
cargo test -p aien-inference-runtime --release --test strict_real_model omega_vs_reference_real_model -- --ignored --nocapture --test-threads=1 > $OUT/gate.log 2>&1; echo "gate exit $?" >> $OUT/gate.log
AIEN_E2E_CHECKPOINT=$HOME/models/TinyLlama-1.1B-Chat-v1.0 AIEN_STRICT_RECEIPT=$OUT/strict-receipt.json \
cargo test -p aien-inference-runtime --release --test strict_real_model strict_real_model_gate -- --ignored --nocapture --test-threads=1 > $OUT/default.log 2>&1; echo "default exit $?" >> $OUT/default.log
tail -1 $OUT/host.log; tail -1 $OUT/parity.log; tail -1 $OUT/gate.log; tail -1 $OUT/default.log
# Zero-CUDA gate on the production binary (Omega linked), with its negative control
( bash scripts/zero-cuda-gate.sh --self-test && cargo build -q --release -p aien-cli && bash scripts/zero-cuda-gate.sh target/release/aien-cli ) > $OUT/zero-cuda.log 2>&1; echo "zero-cuda exit $?" >> $OUT/zero-cuda.log
tail -1 $OUT/zero-cuda.log
