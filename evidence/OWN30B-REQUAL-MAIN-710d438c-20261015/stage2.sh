#!/bin/bash
# Stage 2: #457 fix variant (build + chip, one variable: the platform change), then OWN-30B gates.
set -u
# reconv rerun: tools/run_gpu_reconv_chip.sh is committed mode 100644 (rc 126 Permission denied in the series), so run it through sh
( cd $HOME/workspace/omega-requal-main && quietlock hold --owner OWN30B --minutes 20 --reason "omega gate reconv at omega main (sh)" -- flock /tmp/aien-gb10.lock env PHYSICS_DIR=$HOME/workspace/pkv514-pins/physics sh tools/run_gpu_reconv_chip.sh $HOME/workspace/requal-0c63/omega-gates/reconv ) > $HOME/workspace/requal-0c63/omega-gates/reconv-sh.out 2>&1; echo "reconv-sh rc=$? $(grep -E "PASS|FAIL" $HOME/workspace/requal-0c63/omega-gates/reconv-sh.out | tail -1)" | tee -a $HOME/workspace/requal-0c63/omega-gates/summary.txt
# cross-base: the prefill_attn gate at the pre-merge RC d0889a94 (gate, test and src byte-identical to main for these files)
RC=$HOME/workspace/omega-rc-d0889a94; ER=$HOME/workspace/requal-0c63/omega-gates-rc; mkdir -p $ER
( cd $RC && quietlock hold --owner OWN30B --minutes 20 --reason "omega prefill_attn gate at pre-merge RC" -- flock /tmp/aien-gb10.lock tools/run_gpu_prefill_attn_chip.sh $ER/prefill_attn $HOME/workspace/pkv514-pins/physics ) > $ER/prefill_attn.out 2>&1; echo "rc prefill_attn rc=$? $(tail -1 $ER/prefill_attn.out)"
I=$HOME/workspace/investigations/2026-10-15-omega457-l2-stale; F=$HOME/workspace/omega-457-fix
P=$HOME/workspace/pkv514-pins/physics; NV=$P/third_party/nvidia-open-580.173.02
quietlock hold --owner OWN30B --minutes 20 --reason "omega #457 fix variant build" -- sh -c "
  cd $F && make PHYSICS_DIR=$P -j10 build/libomega_gpu.a > $I/fix-build.log 2>&1 && git -C $F diff > $I/fix-variant.diff && sha256sum build/libomega_gpu.a > $I/fix-lib.sha256 &&
  cc -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -I$F/src -I$P/m16 -I$P/nvrm -I$NV/src/common/sdk/nvidia/inc -I$NV/kernel-open/common/inc -I$NV/kernel-open/nvidia-uvm -I$NV/src/nvidia/arch/nvalloc/unix/include -ffp-contract=off -o $I/l2_stale_test_fix $I/src/l2_stale_test.c $F/build/libomega_gpu.a -lpthread -lm >> $I/fix-build.log 2>&1" ; echo "fix build rc=$?"
cat > $I/runs/run-fix.sh <<'EOS'
#!/bin/bash
# variant=fix (omega-457-fix platform change only), same 4 cases x 5 rounds as run-all.sh
cd "$(dirname "$0")/.."
for r in 1 2 3 4 5; do for m in rewrite realloc; do for c in 0 1; do
  f=runs/fix-$m-c$c-r$r.log
  { echo "# $(date -u +%FT%TZ) ./l2_stale_test_fix --mode $m --cached $c --reps 3"; ./l2_stale_test_fix --mode $m --cached $c --reps 3; echo "# exit $?"; } > $f 2>&1
  grep -h '^STALE\|INVALID\|^FAIL' $f | sed "s|^|fix r$r |"
done; done; done
EOS
chmod +x $I/runs/run-fix.sh
[ -x $I/l2_stale_test_fix ] && quietlock hold --owner OWN30B --minutes 20 --reason "omega #457 fix variant chip" -- flock /tmp/aien-gb10.lock $I/runs/run-fix.sh > $I/runs/summary-fix.txt 2>&1; echo "fix chip rc=$?"
for v in red fix; do W=$HOME/workspace/omega-457-$v; ( cd $W && quietlock hold --owner OWN30B --minutes 20 --reason "omega #457 regression gate $v" -- flock /tmp/aien-gb10.lock tools/run_gpu_l2_stale_chip.sh $I/gate-$v $P ) > $I/gate-$v.out 2>&1; echo "gate $v rc=$? $(tail -1 $I/gate-$v.out)"; done
for g in "fp8 tools/run_gpu_fp8_chip.sh $I/fixgates/fp8 $P" "decode_graph_cached tools/run_gpu_decode_graph_chip.sh $I/fixgates/decode_graph_cached cached $P" "cmdlist tools/run_gpu_cmdlist_chip.sh $I/fixgates/cmdlist $P"; do set -- $g; n=$1; shift; mkdir -p $I/fixgates; ( cd $F && quietlock hold --owner OWN30B --minutes 20 --reason "omega #457 fix no-regression $n" -- flock /tmp/aien-gb10.lock "$@" ) > $I/fixgates/$n.out 2>&1; echo "fixgate $n rc=$? $(tail -1 $I/fixgates/$n.out)"; done
EV=$HOME/workspace/sc-requal-main/docs/inference/evidence/own30b-requal-main-20261015 bash $HOME/workspace/requal-0c63/own30b-gates.sh
echo "stage2 end $(date -u +%FT%TZ)"
