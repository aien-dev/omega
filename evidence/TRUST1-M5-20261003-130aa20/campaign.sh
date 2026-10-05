#!/bin/bash
# TR-02: TRUST-1 + M5 software qualification on aienos main 130aa20, read-only, with QEMU suites.
cd /home/drakestapleton/workspace/hive-worktrees/aienos-tr02 || exit 2
echo "TR02 start $(date -u +%FT%TZ) aienos=$(git rev-parse HEAD)"
bash scripts/trust1_m5_qualify.sh --with-qemu --out /home/drakestapleton/workspace/evidence-out/TRUST1-M5-20261003-130aa20
rc=$?
echo "TR02 rc=$rc $(date -u +%FT%TZ)"
exit $rc
