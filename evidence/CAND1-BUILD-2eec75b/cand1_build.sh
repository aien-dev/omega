#!/usr/bin/env bash
# CAND-1 clean-worktree double build (host only, no chip, no QEMU), same recipe as CAND-0.gates.md section 2,
# plus sovereign-core scripts/repro-build.sh (G13 path remap). Each item is built, hashed, its outputs deleted,
# built again and hashed again. Usage: cand1_build.sh <sovereign-core sha>
set -u
SC_SHA=$1
O=/home/drakestapleton/workspace/overnight-1005
OM_SHA=cb06d081901c03063945a99ffed1c58d33397e9c; AO_SHA=bbad5e4250e57f8cbd1be4cf1390109aa65ef92c; PH_SHA=6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf
OMW=$O/wt-CAND1-omega; AOW=$O/wt-CAND1-aienos; PHW=$O/wt-CAND1-physics; SCW=$O/wt-CAND1-sc-${SC_SHA:0:7}
E=/home/drakestapleton/workspace/evidence-out/CAND1-BUILD-${SC_SHA:0:7}; mkdir -p "$E"; cp "$0" "$E/"
S=$E/summary.txt; : > "$S"
say() { echo "$*" | tee -a "$S"; }
wt() { # repo-source dir sha
	[ -d "$2" ] || git -C "$1" worktree add -q --detach "$2" "$3" || return 1
	[ "$(git -C "$2" rev-parse HEAD)" = "$3" ] && [ -z "$(git -C "$2" status --porcelain --untracked-files=no)" ] || { say "CHECKOUT_FAIL $2 not clean at $3"; return 1; }
}
cmp2() { # name file1digest file2digest
	if [ -n "$2" ] && [ "$2" = "$3" ]; then say "SAME $1 $2"; else say "DIFF $1 run1=$2 run2=$3"; fi
}
h() { [ -f "$1" ] && sha256sum "$1" | cut -c1-64; }
git -C /home/drakestapleton/workspace/r16-survey/physics fetch -q origin
wt /home/drakestapleton/workspace/r16-survey/physics "$PHW" $PH_SHA || exit 3
wt /home/drakestapleton/workspace/omega "$OMW" $OM_SHA || exit 3
wt /home/drakestapleton/workspace/r16-survey/aienos "$AOW" $AO_SHA || exit 3
git -C /home/drakestapleton/workspace/r16-survey/aien-sovereign-core fetch -q origin
wt /home/drakestapleton/workspace/r16-survey/aien-sovereign-core "$SCW" "$SC_SHA" || exit 3
[ "$(tr -d '[:space:]' < "$SCW/omega.lock")" = $OM_SHA ] || { say "PIN_FAIL sc omega.lock != $OM_SHA"; exit 3; }
say "CAND-1 build $(date -u +%FT%TZ) sc=$SC_SHA omega=$OM_SHA aienos=$AO_SHA physics=$PH_SHA host=$(uname -srm)"
export AIENOS_LOCK_REPO=/home/drakestapleton/workspace/r16-survey/aienos
# physics
for r in 1 2; do (cd "$PHW" && ./m2_build.sh) > "$E/physics-$r.log" 2>&1; echo "rc=$?" >> "$E/physics-$r.log"
	eval "ph$r=\$(h $PHW/physics.bin) am$r=\$(h $PHW/atlas_m2.bin)"; done
cmp2 physics-boot-bin "$ph1" "$ph2"; cmp2 atlas-m2-boot-bin "$am1" "$am2"
[ -z "$(git -C "$PHW" status --porcelain --untracked-files=no)" ] && say "physics boot bins equal committed files" || say "DIFF physics tree dirty after build"
# omega
for r in 1 2; do rm -rf "$OMW/build/rb"
	make -C "$OMW" -j6 PHYSICS_DIR="$PHW" OUT_DIR=build/rb all libomega_gpu > "$E/omega-$r.log" 2>&1; echo "rc=$?" >> "$E/omega-$r.log"
	eval "ot$r=\$(h $OMW/build/rb/omegatool) gl$r=\$(h $OMW/build/rb/libomega_gpu.a)"; done
cmp2 omega-runtime "$ot1" "$ot2"; cmp2 omega-gpu-engine-lib "$gl1" "$gl2"
ar t "$OMW/build/rb/libomega_gpu.a" | grep -E '^(omegatool|omega_blackwell_gates|omega_world_gates)\.o$' && say "DIFF libomega_gpu.a still carries gate objects" || say "libomega_gpu.a carries no gate objects (G6)"
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
grep -q '^DIFF\|^FAIL\|CHECKOUT_FAIL\|PIN_FAIL' "$S" && { say "VERDICT FAIL"; exit 1; } || { say "VERDICT PASS"; exit 0; }
