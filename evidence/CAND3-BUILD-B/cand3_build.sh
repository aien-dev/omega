#!/usr/bin/env bash
# CAND-3 clean-worktree double build (host only, no chip, no QEMU). Adapted from cand2_build.sh (CAND-2), same recipe
# (CAND-0.gates.md section 2, sovereign-core scripts/repro-build.sh). Each item is built, hashed, its outputs deleted,
# built again and hashed again. Run it from two different roots to prove path independence, then compare the two
# digests.txt files with cand3_compare.sh (cross-root equality of every required artifact).
# Usage: cand3_build.sh <label> <root> <omega sha> <aienos sha> <physics sha> <sovereign-core sha>
# Differences from cand2_build.sh (the rest is verbatim):
#   1. evidence directory CAND3-BUILD-<label>; every digest is also written to digests.txt as "name sha256" (machine
#      readable, for cand3_compare.sh);
#   2. pins: besides sovereign-core omega.lock, omega's own physics.lock and aienos.lock must equal the shas passed in
#      (the consumed revisions, not heads), and argus.lock's commit must exist in the argus clone;
#   3. NEW production artifacts: rx_operator (the operator client, tools/rx_operator.c, omega #309) and the production
#      R13 program for host and silicon (rx_r13_living_host, rx_r13_living_silicon; they now contain
#      src/runtime/rx_operator.c and are what the operator host/silicon tests start);
#   4. NEW window executables, so a window result can be tied to bytes: rx_r13_living_testbuild_silicon, rx_r11_aien_test,
#      rx_composition_gate_gpu, rx_r15_perf_silicon, rx_r15_perf_silicon_nodigest, r15_reduce. Built twice, compared
#      like the rest; the window scripts re-hash them after the window's own build and must match.
set -u
LABEL=$1; O=$2; OM_SHA=$3; AO_SHA=$4; PH_SHA=$5; SC_SHA=$6
mkdir -p "$O"; O=$(cd "$O" && pwd)
export CARGO_HOME=$O/cargo-home
OMW=$O/wt-omega; AOW=$O/wt-aienos; PHW=$O/wt-physics; SCW=$O/wt-sc
E=/home/drakestapleton/workspace/evidence-out/CAND3-BUILD-$LABEL; mkdir -p "$E"; cp "$0" "$E/"
S=$E/summary.txt; : > "$S"; DG=$E/digests.txt; : > "$DG"
say() { echo "$*" | tee -a "$S"; }
wt() { # repo-source dir sha
	[ -d "$2" ] || git -C "$1" worktree add -q --detach "$2" "$3" || return 1
	[ "$(git -C "$2" rev-parse HEAD)" = "$3" ] && [ -z "$(git -C "$2" status --porcelain --untracked-files=no)" ] || { say "CHECKOUT_FAIL $2 not clean at $3"; return 1; }
}
cmp2() { # name file1digest file2digest ; records the digest for the cross-root comparison
	if [ -n "$2" ] && [ "$2" = "$3" ]; then say "SAME $1 $2"; echo "$1 $2" >> "$DG"; else say "DIFF $1 run1=$2 run2=$3"; fi
}
h() { [ -f "$1" ] && sha256sum "$1" | cut -c1-64; }
ARGUS_REPO_DIR=/home/drakestapleton/workspace/aienos-argus
git -C /home/drakestapleton/workspace/r16-survey/physics fetch -q origin
wt /home/drakestapleton/workspace/r16-survey/physics "$PHW" $PH_SHA || exit 3
wt /home/drakestapleton/workspace/omega "$OMW" $OM_SHA || exit 3
wt /home/drakestapleton/workspace/r16-survey/aienos "$AOW" $AO_SHA || exit 3
git -C /home/drakestapleton/workspace/r16-survey/aien-sovereign-core fetch -q origin
wt /home/drakestapleton/workspace/r16-survey/aien-sovereign-core "$SCW" "$SC_SHA" || exit 3
# pins: the consumed revisions
[ "$(tr -d '[:space:]' < "$SCW/omega.lock")" = $OM_SHA ] || { say "PIN_FAIL sc omega.lock != $OM_SHA"; exit 3; }
[ "$(grep -v '^#' "$OMW/physics.lock" | head -1 | tr -d '[:space:]')" = $PH_SHA ] || { say "PIN_FAIL omega physics.lock != $PH_SHA"; exit 3; }
[ "$(grep -v '^#' "$OMW/aienos.lock" | head -1 | tr -d '[:space:]')" = $AO_SHA ] || { say "PIN_FAIL omega aienos.lock != $AO_SHA"; exit 3; }
AG_SHA=$(grep -v '^#' "$OMW/argus.lock" | head -1 | tr -d '[:space:]')
git -C "$ARGUS_REPO_DIR" cat-file -e "$AG_SHA^{commit}" || { say "PIN_FAIL argus.lock $AG_SHA not in $ARGUS_REPO_DIR"; exit 3; }
[ -f "$OMW/tools/rx_operator.c" ] && [ -f "$OMW/src/runtime/rx_operator.c" ] || { say "PIN_FAIL omega $OM_SHA has no operator client or runtime (omega #309 not in this commit)"; exit 3; }
(cd "$SCW" && cargo fetch --locked) > "$E/cargo-fetch.log" 2>&1 || { say "FETCH_FAIL cargo fetch"; exit 3; }
{ echo "rustc: $(rustc -vV | tr "\n" " ")"; echo "cargo: $(cargo -V)"; echo "cc: $(cc --version | head -1)"; echo "make: $(make -v | head -1)"; echo "ld: $(ld --version | head -1)"; echo "CARGO_HOME=$CARGO_HOME"; } > "$E/toolchain.txt" 2>&1
say "CAND-3 build ($LABEL, root $O) $(date -u +%FT%TZ) sc=$SC_SHA omega=$OM_SHA aienos=$AO_SHA physics=$PH_SHA argus=$AG_SHA host=$(uname -srm)"
export AIENOS_LOCK_REPO=/home/drakestapleton/workspace/r16-survey/aienos
# physics
for r in 1 2; do (cd "$PHW" && ./m2_build.sh) > "$E/physics-$r.log" 2>&1; echo "rc=$?" >> "$E/physics-$r.log"
	eval "ph$r=\$(h $PHW/physics.bin) am$r=\$(h $PHW/atlas_m2.bin)"; done
cmp2 physics-boot-bin "$ph1" "$ph2"; cmp2 atlas-m2-boot-bin "$am1" "$am2"
[ -z "$(git -C "$PHW" status --porcelain --untracked-files=no)" ] && say "physics boot bins equal committed files" || say "DIFF physics tree dirty after build"
# omega: runtime tool and GPU engine library (as CAND-2), then the new production and window executables
XB="rx_operator rx_r13_living_host rx_r13_living_silicon rx_r13_living_testbuild_silicon rx_r11_aien_test rx_composition_gate_gpu rx_r15_perf_silicon rx_r15_perf_silicon_nodigest r15_reduce"
for r in 1 2; do rm -rf "$OMW/build/rb"
	make -C "$OMW" -j6 PHYSICS_DIR="$PHW" OUT_DIR=build/rb all libomega_gpu > "$E/omega-$r.log" 2>&1; echo "rc=$?" >> "$E/omega-$r.log"
	eval "ot$r=\$(h $OMW/build/rb/omegatool) gl$r=\$(h $OMW/build/rb/libomega_gpu.a)"
	for n in $XB; do
		make -C "$OMW" -j6 PHYSICS_DIR="$PHW" OUT_DIR=build/rb ARGUS_REPO="$ARGUS_REPO_DIR" "build/rb/$n" > "$E/omega-$n-$r.log" 2>&1; echo "rc=$?" >> "$E/omega-$n-$r.log"
		eval "x_${n}_$r=\$(h $OMW/build/rb/$n)"; done; done
cmp2 omega-runtime "$ot1" "$ot2"; cmp2 omega-gpu-engine-lib "$gl1" "$gl2"
for n in $XB; do eval "a=\$x_${n}_1 b=\$x_${n}_2"; cmp2 "omega-$(echo "$n" | tr _ -)" "$a" "$b"; done
ar t "$OMW/build/rb/libomega_gpu.a" | grep -E '^(omegatool|omega_blackwell_gates|omega_world_gates)\.o$' && say "DIFF libomega_gpu.a still carries gate objects" || say "libomega_gpu.a carries no gate objects (G6)"
# the production programs must not carry test pieces, and the operator client must exist as an executable file
for n in rx_operator rx_r13_living_host rx_r13_living_silicon; do [ -x "$OMW/build/rb/$n" ] || say "DIFF $n is not an executable file"; done
# sovereign-core, production config
for r in 1 2; do rm -rf "$SCW/target-rb"
	(cd "$SCW" && env -u AIEN_DEV_FALLBACK AIEN_OMEGA_DIR="$OMW" AIEN_PHYSICS_DIR="$PHW" CARGO_TARGET_DIR="$SCW/target-rb" \
		scripts/repro-build.sh --offline -vv -p aien-cli -p aien-proof -p aien-test) > "$E/sc-$r.log" 2>&1; echo "rc=$?" >> "$E/sc-$r.log"
	eval "cli$r=\$(h $SCW/target-rb/release/aien-cli) prf$r=\$(h $SCW/target-rb/release/aien-proof) tst$r=\$(h $SCW/target-rb/release/aien-test)"; done
cmp2 aien-cli-native-release "$cli1" "$cli2"; cmp2 aien-proof "$prf1" "$prf2"; cmp2 aien-test "$tst1" "$tst2"
grep -q 'cargo:rustc-cfg=has_omega_gpu' "$E/sc-2.log" && say "native engine linked (has_omega_gpu)" || say "DIFF native engine NOT linked (stub build)"
grep -aq 'OmegaGb10Backend (native Omega engine, no CUDA, NVIDIA GB10 sm_121)' "$SCW/target-rb/release/aien-cli" && say "aien-cli carries the native backend string" || say "DIFF aien-cli lacks native backend string"
n=$(grep -ac "$HOME" "$SCW/target-rb/release/aien-cli"); c=$(grep -ac "${CARGO_HOME:-$HOME/.cargo}/registry" "$SCW/target-rb/release/aien-cli"); [ "$c" = 0 ] && say "aien-cli has no cargo registry paths (G13)" || say "DIFF aien-cli has $c cargo registry path lines (G13 not fixed)"; say "NOTE aien-cli lines containing the home directory: $n"
(cd "$SCW" && bash scripts/zero-cuda-gate.sh --self-test && bash scripts/zero-cuda-gate.sh target-rb/release/aien-cli) > "$E/zero-cuda.log" 2>&1 && say "PASS zero-cuda aien-cli" || say "FAIL zero-cuda aien-cli"
# aienos C kernel
for r in 1 2; do rm -rf "$AOW/target/native-kernel"
	{ make -C "$AOW/native/kernel" -j4 && make -C "$AOW/native/kernel" -j4 full; } > "$E/aienos-$r.log" 2>&1; echo "rc=$?" >> "$E/aienos-$r.log"
	eval "ck$r=\$(h $AOW/target/native-kernel/BOOTAA64.EFI) fb$r=\$(h $AOW/target/native-kernel/full/BOOTAA64.EFI)"; done
cmp2 aienos-ck-core-image "$ck1" "$ck2"; cmp2 aienos-boot-image "$fb1" "$fb2"
for d in "$PHW" "$OMW" "$AOW" "$SCW"; do [ -z "$(git -C "$d" status --porcelain --untracked-files=no)" ] || say "DIFF dirty after build: $d"; done
sort -o "$DG" "$DG"; say "digests recorded: $(wc -l < "$DG") (expected 18 = 9 CAND-2 items + 9 new omega executables)"
[ "$(wc -l < "$DG")" = 18 ] || say "DIFF digest count is not 18"
grep -q '^DIFF\|^FAIL\|CHECKOUT_FAIL\|PIN_FAIL\|FETCH_FAIL' "$S" && { say "VERDICT FAIL"; exit 1; } || { say "VERDICT PASS"; exit 0; }
