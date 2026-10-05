#!/usr/bin/env bash
# Build omegatool from a copy of this tree at a distinctive path against $PHYSICS_DIR and
# prove the binary contains neither the copy's absolute path nor the physics path, and
# that the same bytes come out of a second copy at a different path. Host only, no chip.
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
phys=$(realpath "${PHYSICS_DIR:-../physics}")
tmp=$(mktemp -d /tmp/omega_nopath_XXXXXX); trap 'rm -rf "$tmp"' EXIT
dig=""
for n in alpha-checkout beta/deeper-checkout; do
	d="$tmp/$n"; mkdir -p "$d"
	tar -C "$here" --exclude=./build --exclude=./.git --exclude=./target -cf - . | tar -C "$d" -xf -
	make -C "$d" -j4 PHYSICS_DIR="$phys" PHYSICS_LOCK_CHECK=0 OUT_DIR=build/np build/np/omegatool >/dev/null
	b="$d/build/np/omegatool"
	for needle in "$d" "$phys" "$tmp"; do
		if grep -aqF -- "$needle" "$b"; then echo "FAIL: omegatool embeds '$needle'"; exit 1; fi
	done
	s=$(sha256sum "$b" | cut -c1-64)
	if [ -n "$dig" ] && [ "$dig" != "$s" ]; then echo "FAIL: omegatool differs between checkout paths ($dig vs $s)"; exit 1; fi
	dig=$s
done
echo "test-omegatool-path-embed: PASS (no checkout or physics path in omegatool; identical across two checkout paths, sha256 $dig)"
