#!/bin/bash
# train_receipt.sh -- receipt for the M22 optimizer substrate + E5 provenance
# tests (mk/train.mk).
#
#   tests/train/train_receipt.sh [out-dir]     (default: evidence/M22/receipts)
#
# Refuses a dirty tree. Runs `make test-train` twice from a clean build dir;
# both runs (plain + ASan/UBSan builds each) must print the same replay digest.
# Writes a NEW file named by the SHA-256 of its own bytes; never rewrites one.
# Verdict vocabulary: the SUBSTRATE tests PASS or FAIL; M22 itself is always
# NOT QUALIFIED here (no tensor/autodiff integration; waits M20/M21).
# Host-only, single core. Shell + coreutils only.
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd)
cd "$HERE" || exit 2
OUTDIR=${1:-evidence/M22/receipts}

if [ -n "$(git status --porcelain)" ]; then
    echo "train_receipt: refusing: working tree is dirty (commit first)" >&2
    exit 3
fi
head0=$(git rev-parse HEAD)
tree0=$(git rev-parse 'HEAD^{tree}')
run_id=$(date -u +%Y%m%dT%H%M%SZ)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

run() {   # $1 = label
    rm -rf build/train
    make --no-print-directory test-train > "$W/$1.log" 2>&1
    local rc=$?
    cp build/train/plain.log "$W/$1.plain.log" 2>/dev/null
    echo $rc
}
rc1=$(run run1)
rc2=$(run run2)
dig() { awk -v k="$2" '$1 == k {print $2}' "$W/$1" | tail -n 1; }
d1=$(dig run1.log replay_digest); d2=$(dig run2.log replay_digest)
h1=$(dig run1.log replay_dispatch_head)
pass1=$(grep -c '^M22_SUBSTRATE_TESTS_PASS$' "$W/run1.log"); pass2=$(grep -c '^M22_SUBSTRATE_TESTS_PASS$' "$W/run2.log")
both=$(grep -c '^test-train: plain and ASan/UBSan PASS, replay digest equal across builds$' "$W/run1.log")
checks=$(awk '$1 == "checks" {print $2; exit}' "$W/run1.log")
reverified=$(dig run1.log replay_steps_reverified)
race=$(grep '^reader_race ' "$W/run1.plain.log" | head -n 1)
crash_old=$(grep -c '^crash .*recovered=OLD' "$W/run1.plain.log")
crash_new=$(grep -c '^crash .*recovered=NEW' "$W/run1.plain.log")
cost=$(grep '^cost ' "$W/run1.plain.log" | sed 's/"//g' | tr '\n' ';')
head1=$(git rev-parse HEAD)
dirty1=$(git status --porcelain | grep -v '^?? build/' || true)

reasons=""
[ "$rc1" = 0 ] && [ "$rc2" = 0 ] || reasons="$reasons make-failed($rc1,$rc2)"
[ "$pass1" = 2 ] && [ "$pass2" = 2 ] || reasons="$reasons verdict-line-missing"
[ "$both" = 1 ] || reasons="$reasons cross-build-replay-missing"
[ -n "$d1" ] && [ "$d1" = "$d2" ] || reasons="$reasons replay-digest-differs"
[ "$crash_old" = 9 ] && [ "$crash_new" = 2 ] || reasons="$reasons crash-matrix($crash_old,$crash_new)"
[ "$head0" = "$head1" ] || reasons="$reasons head-moved"
[ -z "$dirty1" ] || reasons="$reasons tree-dirty-after"
verdict=PASS; [ -z "$reasons" ] || verdict=FAIL

log_sha=$(cat "$W/run1.log" "$W/run2.log" | sha256sum | awk '{print $1}')
src_sha=$(cat src/train/*.c src/train/*.h tests/train/test_train.c mk/train.mk | sha256sum | awk '{print $1}')
mkdir -p "$OUTDIR"
cat > "$W/receipt.json" <<JSON
{
  "schema": "omega.m22.train-substrate.receipt.v1",
  "gate": "M22_SUBSTRATE",
  "m22_status": "M22 NOT QUALIFIED: optimizer substrate + SGD path only; no tensor/autodiff integration (waits M20/M21)",
  "substrate_tests_verdict": "$verdict",
  "reasons": "$(echo $reasons)",
  "scope": "host-only, single core; process-crash fault injection (fork + _exit at 8 points, plus torn-file simulation of not-yet-current files); NOT a power-loss test, NOT a GPU or hardware qualification; SGD with caller-supplied gradients (hand-written quadratic), Adam/AdamW deferred",
  "commit": "$head0",
  "tree": "$tree0",
  "tree_clean": true,
  "run_id": "$run_id",
  "command": "make test-train (twice, clean build/train each time; each run = plain -O2 build + ASan/UBSan -O1 build)",
  "checks_per_binary_run": "${checks:-null}",
  "replay_digest_run1": "${d1:-null}",
  "replay_digest_run2": "${d2:-null}",
  "replay_dispatch_head": "${h1:-null}",
  "replay_steps_reverified": "${reverified:-null}",
  "crash_matrix": {"recovered_old": $crash_old, "recovered_new": $crash_new},
  "reader_race": "$race",
  "provenance_cost": "$cost",
  "train_sources_sha256": "$src_sha",
  "logs_sha256": "$log_sha",
  "host": "$(uname -srm)"
}
JSON
name=$(sha256sum "$W/receipt.json" | awk '{print $1}')
if [ -e "$OUTDIR/$name.json" ]; then
    echo "train_receipt: $OUTDIR/$name.json already exists; not rewritten" >&2
    exit 0
fi
cp "$W/receipt.json" "$OUTDIR/$name.json"
echo "train_receipt: $verdict -> $OUTDIR/$name.json"
[ "$verdict" = PASS ]
