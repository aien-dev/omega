#!/bin/bash
# Gate 14 full run: omega 8024e9a + physics e95e3ed (physics.lock). Sequential;
# each leg takes /tmp/aien-gb10.lock itself. Then the combiner.
O=$HOME/workspace/hive-worktrees/omega-gate14-8024e9a
P=$HOME/workspace/hive-worktrees/physics-gate14-e95e3ed
D=$HOME/workspace/evidence-out/GATE14-8024e9a
OC=$(git -C "$O" rev-parse HEAD)
PC=$(tr -d '[:space:]' < "$O/physics.lock")
[ "$(git -C "$P" rev-parse HEAD)" = "$PC" ] || { echo "physics worktree not at physics.lock"; echo "done"; exit 1; }
echo "start $(date -u +%FT%TZ) omega=$OC physics=$PC"
echo "== LEG gate5 numeric $(date -u +%FT%TZ)"
"$O/tests/run_numeric_gates.sh" --omega-candidate "$OC" --physics-candidate "$PC" --physics-dir "$P" \
  --evidence-dir "$D/OMEGA-NUMERIC-0" --record > "$D/gate5.out" 2>&1
echo "gate5 rc=$? $(date -u +%FT%TZ)"
echo "== LEG gate3_4 forge $(date -u +%FT%TZ)"
"$P/tests/run_forge_gates.sh" --omega-dir "$O" --omega-candidate "$OC" --physics-candidate "$PC" \
  --evidence-dir "$D/FORGE-GATES" --record > "$D/gate3_4.out" 2>&1
echo "gate3_4 rc=$? $(date -u +%FT%TZ)"
echo "== LEG gate1_2 m19r full $(date -u +%FT%TZ)"
"$O/tools/m19r_qualify.sh" --omega-candidate "$OC" --physics-candidate "$PC" --physics-dir "$P" > "$D/gate1_2.out" 2>&1
echo "gate1_2 rc=$? $(date -u +%FT%TZ)"
run=$(ls -td "$O"/build/qual-runs/2*/ | head -1)
echo "last qual run: $run"
mkdir -p "$D/M19R" && cp "$run/receipt-preview.json" "$D/M19R/receipt-preview.json" 2>/dev/null
echo "== COMBINE $(date -u +%FT%TZ)"
"$O/tools/gate14_combine.sh" --omega-candidate "$OC" --physics-candidate "$PC" --evidence-dir "$D/GATE14-FOUNDATION" \
  "$D/M19R/receipt-preview.json" "$D"/FORGE-GATES/*.json "$D"/OMEGA-NUMERIC-0/*.json > "$D/combine.out" 2>&1
echo "combine rc=$? $(date -u +%FT%TZ)"
echo "done $(date -u +%FT%TZ)"
