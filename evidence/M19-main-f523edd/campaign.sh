#!/bin/bash
# M19 foundation qualification on omega main f523edd (HD-18 + I37 + I11 merged) with physics 6d7cf0d:
# Gate 5 (numeric), Gates 3+4 (forge), Gates 1+2 (M19R --record), then Gate 14 combine.
# Adapted from ~/.claude/jobs/9f8a3f74/tmp/m19_main_run.sh (2026-10-02) with new pins and dirs only.
set -u
O=/home/drakestapleton/workspace/hive-worktrees/qual-omega-f523edd
P=/home/drakestapleton/workspace/hive-worktrees/qual-physics-6d7cf0d
D=$HOME/workspace/evidence-out/M19-main-f523edd
mkdir -p "$D"
OC=$(git -C "$O" rev-parse HEAD); PC=$(tr -d '[:space:]' < "$O/physics.lock"); PH=$(git -C "$P" rev-parse HEAD)
echo "M19 start $(date -u +%FT%TZ) omega=$OC physics.lock=$PC physics_dir_head=$PH"
[ "$OC" = f523edd547d8c43a919df1be5ecbf746d25fc2b4 ] || { echo "WRONG OMEGA HEAD $OC"; exit 2; }
[ "$PC" = 6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf ] || { echo "WRONG PHYSICS PIN $PC"; exit 2; }
[ "$PH" = "$PC" ] || { echo "PHYSICS DIR HEAD $PH != LOCK $PC"; exit 2; }
[ -z "$(git -C "$O" status --porcelain)" ] || { echo "OMEGA TREE DIRTY"; git -C "$O" status --porcelain; exit 2; }
[ -z "$(git -C "$P" status --porcelain)" ] || { echo "PHYSICS TREE DIRTY"; git -C "$P" status --porcelain; exit 2; }
uname -a > "$D/device.txt"; ls -la /dev/nvidia* >> "$D/device.txt" 2>&1
step() { n=$1; shift; s=$(date +%s); echo "== $n start $(date -u +%FT%TZ)"; if "$@" > "$D/$n.log" 2>&1; then echo "== $n PASS ($(( $(date +%s)-s ))s)"; else rc=$?; echo "== $n FAIL rc=$rc ($(( $(date +%s)-s ))s); tail:"; tail -n 20 "$D/$n.log"; echo "M19 ABORT at $n $(date -u +%FT%TZ)"; exit $rc; fi; }
cd "$O" || exit 2
step NUM   "$O/tests/run_numeric_gates.sh" --omega-candidate "$OC" --physics-candidate "$PC" --physics-dir "$P" --evidence-dir "$D/NUM" --record
step FORGE "$P/tests/run_forge_gates.sh" --omega-dir "$O" --omega-candidate "$OC" --physics-candidate "$PC" --evidence-dir "$D/FORGE" --record
step M19R  "$O/tools/m19r_qualify.sh" --omega-candidate "$OC" --physics-candidate "$PC" --physics-dir "$P" --record
PREV=$(ls -t "$O"/build/qual-runs/*/receipt-preview.json 2>/dev/null | head -1)
[ -n "$PREV" ] || { echo "no receipt-preview.json from M19R"; exit 3; }
mkdir -p "$D/M19R"; git -C "$O" status --porcelain -- evidence/M19R | awk '{print $2}' | while read -r f; do mv "$O/$f" "$D/M19R/"; done; ls "$D/M19R"
step G14   "$O/tools/gate14_combine.sh" --omega-candidate "$OC" --physics-candidate "$PC" --evidence-dir "$D/G14" "$PREV" "$D"/FORGE/*.json "$D"/NUM/*.json
echo "M19 DONE $(date -u +%FT%TZ)"; ls "$D"/G14 "$D"/NUM "$D"/FORGE
(cd "$D" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
