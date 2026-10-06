#!/usr/bin/env bash
# Cross-root equality: every required artifact must have the same digest in both roots' digests.txt.
# Usage: cand3_compare.sh <evidence dir A> <evidence dir B>   (e.g. CAND3-BUILD-A CAND3-BUILD-B)
set -u
A=$1/digests.txt; B=$2/digests.txt
for f in "$A" "$B"; do [ -s "$f" ] || { echo "FAIL missing or empty $f"; exit 2; }; done
if [ "$(wc -l < "$A")" = 18 ] && [ "$(wc -l < "$B")" = 18 ] && cmp -s "$A" "$B"; then
	echo "PASS cross-root: all 18 required artifacts identical from both roots"; exit 0
fi
echo "FAIL cross-root:"; diff "$A" "$B"; exit 1
