#!/bin/sh
# Content check for MIXED_ALGEBRA_DIGITAL_V1 wrapper receipts
# (spec/mixed-algebra-digital-v1.md, post-run addenda 2026-09-29).
#
# For every evidence/MIXED_ALGEBRA/digital_v1/<name>.json under ROOT
# (default: current directory = repository root):
#   1. sha256 of the file bytes must equal <name>;
#   2. every receipt the wrapper cites ("path": ..., "sha256": ... in its
#      receipts array) must exist under ROOT and re-hash to that digest.
# Complements check-mixed-algebra-evidence (which only compares against git
# HEAD): this one also catches a tampered file that was committed.
# Exit: 0 PASS, 1 FAIL, 2 error.
set -u
ROOT=${1:-.}
DIR="$ROOT/evidence/MIXED_ALGEBRA/digital_v1"
[ -d "$DIR" ] || { echo "check-mixed-algebra-digital-v1-evidence: no $DIR"; exit 2; }
nw=0 nr=0 bad=0
for f in "$DIR"/*.json; do
    [ -f "$f" ] || continue
    nw=$((nw + 1))
    name=$(basename "$f" .json)
    h=$(sha256sum "$f" | cut -c1-64)
    if [ "$h" != "$name" ]; then
        echo "FAIL: $f hashes to $h, not its name"
        bad=$((bad + 1))
    fi
    pairs=$(sed -n 's/.*"path": "\([^"]*\)", "sha256": "\([0-9a-f]\{64\}\)".*/\1 \2/p' "$f")
    if [ -z "$pairs" ]; then
        echo "FAIL: $f cites no receipts"
        bad=$((bad + 1))
        continue
    fi
    cited=0
    while read -r p d; do
        cited=$((cited + 1))
        nr=$((nr + 1))
        if [ ! -f "$ROOT/$p" ]; then
            echo "FAIL: $name cites missing receipt $p"
            bad=$((bad + 1))
            continue
        fi
        rh=$(sha256sum "$ROOT/$p" | cut -c1-64)
        if [ "$rh" != "$d" ]; then
            echo "FAIL: $name cites $p with sha256 $d, file now hashes to $rh"
            bad=$((bad + 1))
        fi
    done <<PAIRS
$pairs
PAIRS
    echo "  $name: self-hash ok=$([ "$h" = "$name" ] && echo yes || echo no), $cited cited receipts checked"
done
[ "$nw" -gt 0 ] || { echo "check-mixed-algebra-digital-v1-evidence: FAIL (no wrapper receipts)"; exit 1; }
if [ "$bad" -ne 0 ]; then
    echo "check-mixed-algebra-digital-v1-evidence: FAIL ($bad problems, $nw wrappers, $nr cited receipts)"
    exit 1
fi
echo "check-mixed-algebra-digital-v1-evidence: PASS ($nw wrappers, $nr cited receipts re-hashed)"
