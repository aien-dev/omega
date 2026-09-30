#!/bin/sh
# indep_source_digest.sh: independent_scorer_source_sha256 of EXP-001 (rule turing.cal.indep_source.v1).
#
#   sh calibration/scripts/indep_source_digest.sh [--lines]
#
# Listing = for every regular file under tools/turing_verify_indep/ (the lane D independent scorer: src/,
# SPEC_GAPS.md), one line '<sha256 of the file, 64 lowercase hex>  <path relative to
# tools/turing_verify_indep>' + LF, sorted by path in LC_ALL=C byte order. The digest is SHA-256 of the listing
# bytes. A build/ directory (made if someone runs the scorer's own Makefile in place) is skipped; make
# turing-verify-indep builds outside the tree. --lines prints the listing.
# The value is written into candidate_manifest.json by freeze_candidate.sh and checked by freeze_receipt.sh and
# by turing-cal-eval in sealed mode. No Python: POSIX sh + find + sha256sum + sort.
set -eu
dir=$(cd "$(dirname "$0")/../.." && pwd)
src="$dir/tools/turing_verify_indep"
[ -d "$src" ] || { echo "indep_source_digest: REFUSED: $src missing" >&2; exit 1; }
list=$(cd "$src" && find . -path ./build -prune -o -type f -print | sed 's|^\./||' | LC_ALL=C sort | while IFS= read -r p; do
    printf '%s  %s\n' "$(sha256sum "$p" | cut -c1-64)" "$p"
done)
if [ "${1:-}" = "--lines" ]; then printf '%s\n' "$list"; else printf '%s\n' "$list" | sha256sum | cut -c1-64; fi
