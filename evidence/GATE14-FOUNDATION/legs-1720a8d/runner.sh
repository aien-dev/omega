#!/bin/bash
# Gate 14 leg on omega 1720a8d + physics e95e3ed (clean worktrees). usage: g14leg-1720a8d.sh forge|m19r
# Never resets/closes the device. On any non-zero exit it snapshots dmesg/devnodes and leaves state alone.
set -u
H=$HOME/workspace/hive-worktrees
O=$H/omega-g14-1720a8d; P=$H/physics-g14-e95e3ed
D=$HOME/workspace/evidence-out/G14-1720a8d
OC=1720a8d42bca03181be5da69317ec38fc52ea9bf; PC=e95e3ed2a86fe4bffe4d954fa94c27dfb5284280
leg=$1; mkdir -p "$D"
[ "$(git -C $O rev-parse HEAD)" = "$OC" ] && [ "$(git -C $P rev-parse HEAD)" = "$PC" ] || { echo "WRONG HEADS"; exit 1; }
[ -z "$(git -C $O status --porcelain)" ] && [ -z "$(git -C $P status --porcelain)" ] || { echo "DIRTY tree before run"; exit 1; }
start=$(date -u +%FT%TZ); echo "$leg start $start" >> $D/timings.txt
case $leg in
 forge) "$P/tests/run_forge_gates.sh" --omega-dir "$O" --omega-candidate $OC --physics-candidate $PC \
          --evidence-dir "$D/FORGE-GATES" --record > $D/gate3_4.out 2>&1; rc=$?;;
 m19r)  "$O/tools/m19r_qualify.sh" --omega-candidate $OC --physics-candidate $PC --physics-dir "$P" > $D/gate1_2.out 2>&1; rc=$?
        run=$(ls -td "$O"/build/qual-runs/2*/ | head -1); mkdir -p $D/M19R; cp "$run/receipt-preview.json" $D/M19R/receipt-preview.json 2>/dev/null
        cp -r "$run" $D/m19r-run-dir 2>/dev/null;;
esac
end=$(date -u +%FT%TZ); echo "$leg end $end rc=$rc" >> $D/timings.txt
if [ $rc -ne 0 ]; then { date -u; dmesg 2>&1 | tail -200; } > $D/$leg.dmesg.txt; ls -l /dev/nvidia* > $D/$leg.devnodes.txt 2>&1; fi
echo "porcelain after: omega[$(git -C $O status --porcelain)] physics[$(git -C $P status --porcelain)]"
tail -15 $D/*$([ $leg = forge ] && echo gate3_4 || echo gate1_2).out
exit $rc
