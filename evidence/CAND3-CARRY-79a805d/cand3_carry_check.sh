#!/usr/bin/env bash
# Carry proof for CAND-3 (host only, no chip): rebuild, at the CAND-2 omega code commit, every executable that a CAND-2
# chip or QEMU result used, and compare its digest with the CAND-3 digest from cand3_build.sh. A result may be carried
# only if the digest is identical (and its inputs unchanged: physics, aienos, model, fixtures, see CAND-3.gates.md).
# Same recipe, same toolchain and same make variables as cand3_build.sh, one build per target.
# Usage: cand3_carry_check.sh <root> <cand2 omega sha 79a805d...> <physics sha> <CAND3-BUILD evidence dir>
# A target that does not exist at the old commit prints NO_TARGET (it is new in CAND-3, so nothing to carry).
set -u
O=$1; OLD=$2; PH=$3; C3=$4
mkdir -p "$O"; O=$(cd "$O" && pwd)
OMW=$O/wt-omega-old; PHW=$O/wt-physics; E=/home/drakestapleton/workspace/evidence-out/CAND3-CARRY-$(echo "$OLD" | cut -c1-7); mkdir -p "$E"; cp "$0" "$E/"
R=$E/result.txt; : > "$R"
[ -d "$OMW" ] || git -C /home/drakestapleton/workspace/omega worktree add -q --detach "$OMW" "$OLD" || exit 3
[ -d "$PHW" ] || git -C /home/drakestapleton/workspace/r16-survey/physics worktree add -q --detach "$PHW" "$PH" || exit 3
[ "$(git -C "$OMW" rev-parse HEAD)" = "$OLD" ] || { echo "wrong omega"; exit 3; }
export AIENOS_LOCK_REPO=/home/drakestapleton/workspace/r16-survey/aienos
rm -rf "$OMW/build/rb"
for n in omegatool libomega_gpu.a rx_r13_living_host rx_r13_living_silicon rx_r13_living_testbuild_silicon rx_r11_aien_test rx_composition_gate_gpu rx_r15_perf_silicon rx_r15_perf_silicon_nodigest r15_reduce; do
	make -C "$OMW" -j6 PHYSICS_DIR="$PHW" OUT_DIR=build/rb ARGUS_REPO=/home/drakestapleton/workspace/aienos-argus "build/rb/$n" > "$E/build-$n.log" 2>&1 \
		|| { echo "NO_TARGET_OR_BUILD_FAIL $n (see build-$n.log)" | tee -a "$R"; continue; }
	old=$(sha256sum "$OMW/build/rb/$n" | cut -c1-64)
	key=$(case $n in omegatool) echo omega-runtime;; libomega_gpu.a) echo omega-gpu-engine-lib;; *) echo "omega-$(echo "$n" | tr _ -)";; esac)
	new=$(awk -v k="$key" '$1==k{print $2}' "$C3/digests.txt")
	if [ -n "$new" ] && [ "$old" = "$new" ]; then echo "IDENTICAL $n $old (CAND-2 code == CAND-3)" | tee -a "$R"
	else echo "CHANGED $n cand2=$old cand3=$new" | tee -a "$R"; fi
done
