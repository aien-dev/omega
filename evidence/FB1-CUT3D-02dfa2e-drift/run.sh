#!/bin/bash
# Teacher-forced logit drift, reference vs omega (cut 3d at 02dfa2e + uncommitted logit_drift.rs). Not killed by us.
cd ~/workspace/hive-worktrees/sc-fb1-cut3d
O=$HOME/workspace/evidence-out/FB1-CUT3D-02dfa2e-drift
git rev-parse HEAD > $O/sha.txt; git status --porcelain >> $O/sha.txt
export AIEN_OMEGA_DIR=$HOME/workspace/hive-worktrees/omega-pin-3d4648c AIEN_PHYSICS_DIR=$HOME/workspace/hive-worktrees/qual-physics-6d7cf0d
AIEN_E2E_CHECKPOINT=$HOME/models/TinyLlama-1.1B-Chat-v1.0 cargo test -p aien-inference-runtime --release --test logit_drift -- --ignored --nocapture --test-threads=1 > $O/drift.log 2>&1; echo "drift exit $?" >> $O/drift.log
tail -1 $O/drift.log
