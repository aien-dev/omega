#!/usr/bin/env bash
# CAND-2 prep: does the omegatool path fix make all 9 candidate artifacts location-independent?
# Host only, no chip, no QEMU. Recipe = CAND1-RECON-2eec75b/cand1_recon_build.sh, run in TWO checkout
# locations (A twice, B once), each with fresh GitHub clones and a fresh empty CARGO_HOME.
# Usage: pathfix_build.sh <out-dir> <omega-sha> <sovereign-core-sha>
set -u
E=$1; OM_SHA=$2; SC_SHA=$3
PIN_SHA=cb06d081901c03063945a99ffed1c58d33397e9c  # omega commit pinned by sovereign-core omega.lock (CAND-1); sc cannot build against a new omega until its lock is bumped (CAND-2 freeze)
AO_SHA=bbad5e4250e57f8cbd1be4cf1390109aa65ef92c; PH_SHA=6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf
LOCS="/home/drakestapleton/workspace/cand3-campaign/build-A /home/drakestapleton/workspace/cand3-campaign/build-B/nested/dir"
mkdir -p "$E"; cp "$0" "$E/"
S=$E/summary.txt; : > "$S"; D=$E/digests.tsv; : > "$D"
say() { echo "$*" | tee -a "$S"; }
h() { [ -f "$1" ] && sha256sum "$1" | cut -c1-64; }
wt() { [ -d "$2" ] || { git clone -q "https://github.com/aien-dev/$1.git" "$2" && git -C "$2" checkout -q --detach "$3"; } || return 1
	[ "$(git -C "$2" rev-parse HEAD)" = "$3" ] && [ -z "$(git -C "$2" status --porcelain --untracked-files=no)" ] || { say "CHECKOUT_FAIL $2 not at $3"; return 1; }; }
rec() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >> "$D"; }  # loc run artifact sha
i=0
for L in $LOCS; do i=$((i+1)); tag=loc$i
	case $i in 1) RUNS="1 2";; *) RUNS="1";; esac
	mkdir -p "$L"; export CARGO_HOME=$L/cargo-home; rm -rf "$CARGO_HOME"; mkdir -p "$CARGO_HOME"
	say "[$tag] $L CARGO_HOME=$CARGO_HOME entries_before_fetch=$(find "$CARGO_HOME" -mindepth 1 | wc -l)"
	PHW=$L/physics; OMW=$L/omega; OMP=$L/omega-pinned; AOW=$L/aienos; SCW=$L/aien-sovereign-core
	wt physics "$PHW" $PH_SHA && wt omega "$OMW" $OM_SHA && wt omega "$OMP" $PIN_SHA && wt aienos "$AOW" $AO_SHA && wt aien-sovereign-core "$SCW" $SC_SHA || exit 3
	[ "$(tr -d "[:space:]" < "$SCW/omega.lock")" = $PIN_SHA ] || { say "PIN_FAIL sc omega.lock != $PIN_SHA"; exit 3; }
	(cd "$SCW" && cargo fetch --locked) > "$E/$tag-cargo-fetch.log" 2>&1 || { say "FETCH_FAIL $tag"; exit 3; }
	{ echo "rustc: $(rustc -vV | tr "\n" " ")"; echo "cargo: $(cargo -V)"; echo "cc: $(cc --version | head -1)"; echo "make: $(make -v | head -1)"; echo "ld: $(ld --version | head -1)"; echo "mojo: $(command -v mojo >/dev/null && mojo --version || echo not-used)"; echo "CARGO_HOME=$CARGO_HOME"; echo "host=$(uname -srm)"; } > "$E/$tag-toolchain.txt" 2>&1
	say "[$tag] built at $(date -u +%FT%TZ) omega=$OM_SHA sc=$SC_SHA aienos=$AO_SHA physics=$PH_SHA"
	export AIENOS_LOCK_REPO=$AOW
	for r in $RUNS; do
		(cd "$PHW" && nice -n 10 ./m2_build.sh) > "$E/$tag-physics-$r.log" 2>&1; echo "rc=$?" >> "$E/$tag-physics-$r.log"
		rec $tag $r physics-boot-bin "$(h $PHW/physics.bin)"; rec $tag $r atlas-m2-boot-bin "$(h $PHW/atlas_m2.bin)"
		rm -rf "$OMW/build/rb"
		nice -n 10 make -C "$OMW" -j6 PHYSICS_DIR="$PHW" OUT_DIR=build/rb all libomega_gpu > "$E/$tag-omega-$r.log" 2>&1; echo "rc=$?" >> "$E/$tag-omega-$r.log"
		rec $tag $r omega-runtime "$(h $OMW/build/rb/omegatool)"; rec $tag $r omega-gpu-engine-lib "$(h $OMW/build/rb/libomega_gpu.a)"
		rm -rf "$SCW/target-rb"
		(cd "$SCW" && env -u AIEN_DEV_FALLBACK AIEN_OMEGA_DIR="$OMP" AIEN_PHYSICS_DIR="$PHW" CARGO_TARGET_DIR="$SCW/target-rb" \
			nice -n 10 scripts/repro-build.sh --offline -vv -p aien-cli -p aien-proof -p aien-test) > "$E/$tag-sc-$r.log" 2>&1; echo "rc=$?" >> "$E/$tag-sc-$r.log"
		rec $tag $r aien-cli-native-release "$(h $SCW/target-rb/release/aien-cli)"; rec $tag $r aien-proof "$(h $SCW/target-rb/release/aien-proof)"; rec $tag $r aien-test "$(h $SCW/target-rb/release/aien-test)"
		rm -rf "$AOW/target/native-kernel"
		{ nice -n 10 make -C "$AOW/native/kernel" -j4 && nice -n 10 make -C "$AOW/native/kernel" -j4 full; } > "$E/$tag-aienos-$r.log" 2>&1; echo "rc=$?" >> "$E/$tag-aienos-$r.log"
		rec $tag $r aienos-ck-core-image "$(h $AOW/target/native-kernel/BOOTAA64.EFI)"; rec $tag $r aienos-boot-image "$(h $AOW/target/native-kernel/full/BOOTAA64.EFI)"
		say "[$tag run $r] done"
	done
	n=$(grep -ac "$L" "$SCW/target-rb/release/aien-cli"); say "[$tag] aien-cli lines containing its checkout path: $n"
	# location-path scan of omegatool, both checkout and physics paths
	for needle in "$L" "$PHW" "$OMW"; do grep -aqF -- "$needle" "$OMW/build/rb/omegatool" && say "DIFF [$tag] omegatool embeds $needle" || say "[$tag] omegatool does not contain $needle"; done
	for d in "$PHW" "$OMW" "$OMP" "$AOW" "$SCW"; do [ -z "$(git -C "$d" status --porcelain --untracked-files=no)" ] || say "DIFF dirty after build: $d"; done
done
# comparisons: every artifact must have exactly one distinct digest across all (loc, run) pairs
for a in $(cut -f3 "$D" | sort -u); do
	n=$(awk -F'\t' -v a=$a '$3==a{print $4}' "$D" | sort -u | wc -l); c=$(awk -F'\t' -v a=$a '$3==a' "$D" | wc -l)
	v=$(awk -F'\t' -v a=$a '$3==a{print $4}' "$D" | sort -u | head -1)
	[ "$n" = 1 ] && [ -n "$v" ] && say "SAME $a x$c $v" || say "DIFF $a distinct=$n (see digests.tsv)"
done
grep -q '^DIFF\|CHECKOUT_FAIL\|FAIL' "$S" && { say "VERDICT FAIL"; exit 1; } || { say "VERDICT PASS"; exit 0; }
