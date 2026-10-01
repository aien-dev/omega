#!/bin/sh
# COMPOSITION-2 gate wrapper: refuses a dirty tree, builds and runs the gate
# against the committed source, stores the receipt under its content hash:
#   evidence/COMPOSITION-2/<sha256-of-receipt>.json
# Extra arguments are passed to make (e.g. AIENOS_LOCK_REPO=...).
# --gpu (first argument): the GPU tier, both Skills executed on the GB10.
# Run it only while holding the quiet flag (~/workspace/.spark-quiet).
set -eu
bintarget=composition-gate-bin
if [ "${1:-}" = "--gpu" ]; then
    bintarget=composition-gate-gpu-bin
    shift
fi
root=$(git rev-parse --show-toplevel)
cd "$root"
if [ -n "$(git status --porcelain --untracked-files=normal)" ]; then
    echo "composition_gate: working tree is dirty; commit first" >&2
    git status --short >&2
    exit 2
fi
commit=$(git rev-parse HEAD)
make "$@" "$bintarget" >/dev/null
bin=$(make -s "$@" "print-$bintarget")
tmp=$(mktemp /tmp/composition_gate_receipt.XXXXXX)
set +e
"./$bin" "$commit" "$tmp"
status=$?
set -e
if [ ! -s "$tmp" ]; then
    echo "composition_gate: no receipt written (exit $status)" >&2
    rm -f "$tmp"
    exit 2
fi
sum=$(sha256sum "$tmp" | cut -d' ' -f1)
mkdir -p evidence/COMPOSITION-2
out="evidence/COMPOSITION-2/$sum.json"
mv "$tmp" "$out"
chmod 0644 "$out"
verdict=$(sed -n 's/.*"verdict": "\([A-Z]*\)".*/\1/p' "$out" | head -n 1)
echo "receipt: $out"
echo "verdict: $verdict (commit $commit)"
exit $status
