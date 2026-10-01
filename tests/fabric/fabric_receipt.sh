#!/bin/bash
# fabric_receipt.sh -- receipt for the Fabric F5-0 exit gate (mk/fabric.mk).
#
#   tests/fabric/fabric_receipt.sh [out-dir]     (default: evidence/F5-0)
#
# Refuses a dirty tree. Runs `make test-fabric` twice from a clean build dir
# (the second run must print the same replay digest), then writes a NEW file
# named by the SHA-256 of its own bytes. Never rewrites an existing receipt.
# Verdict PASS only when both runs print F5_0_FABRIC_LOOPBACK_PASS with equal
# digests and HEAD did not move. Host-only, loopback only: this receipt
# qualifies nothing physical and no real transport. Shell + coreutils only.
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd)
cd "$HERE" || exit 2
OUTDIR=${1:-evidence/F5-0}

if [ -n "$(git status --porcelain)" ]; then
    echo "fabric_receipt: refusing: working tree is dirty (commit first)" >&2
    exit 3
fi
head0=$(git rev-parse HEAD)
tree0=$(git rev-parse 'HEAD^{tree}')
run_id=$(date -u +%Y%m%dT%H%M%SZ)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

run() {   # $1 = label
    rm -rf build/fabric
    make --no-print-directory test-fabric > "$W/$1.log" 2>&1
    echo $?
}
rc1=$(run run1)
rc2=$(run run2)
dig() { awk -v k="$2" '$1 == k {print $2}' "$W/$1.log" | tail -n 1; }
d1=$(dig run1 replay_digest); d2=$(dig run2 replay_digest)
v1=$(grep -c '^F5_0_FABRIC_LOOPBACK_PASS$' "$W/run1.log"); v2=$(grep -c '^F5_0_FABRIC_LOOPBACK_PASS$' "$W/run2.log")
asan1=$(grep -c '^test-fabric: ASan/UBSan run PASS$' "$W/run1.log")
purity=$(grep -c '^fabric-purity: ' "$W/run1.log")
checks=$(awk '$1 == "checks" {print $2; exit}' "$W/run1.log")
head1=$(git rev-parse HEAD)
dirty1=$(git status --porcelain | grep -v '^?? build/' || true)

reasons=""
[ "$rc1" = 0 ] && [ "$rc2" = 0 ] || reasons="$reasons make-failed($rc1,$rc2)"
[ "$v1" = 1 ] && [ "$v2" = 1 ] || reasons="$reasons verdict-line-missing"
[ -n "$d1" ] && [ "$d1" = "$d2" ] || reasons="$reasons replay-digest-differs"
[ "$asan1" = 1 ] || reasons="$reasons asan-run-missing"
[ "$purity" = 1 ] || reasons="$reasons purity-missing"
[ "$head0" = "$head1" ] || reasons="$reasons head-moved"
[ -z "$dirty1" ] || reasons="$reasons tree-dirty-after"
verdict=PASS; [ -z "$reasons" ] || verdict=FAIL

log_sha=$(cat "$W/run1.log" "$W/run2.log" | sha256sum | awk '{print $1}')
src_sha=$(cat src/fabric/*.c src/fabric/*.h tests/fabric/fabric_test.c mk/fabric.mk | sha256sum | awk '{print $1}')
mkdir -p "$OUTDIR"
cat > "$W/receipt.json" <<JSON
{
  "gate": "F5_0_FABRIC_LOOPBACK",
  "verdict": "$verdict",
  "reasons": "$(echo $reasons)",
  "scope": "host-only, in-process loopback transport, HMAC stand-in authenticator; no real network, no TRUST-1 signature, no hardware qualification",
  "commit": "$head0",
  "tree": "$tree0",
  "run_id": "$run_id",
  "command": "make test-fabric (twice, clean build/fabric each time)",
  "checks_per_binary_run": "${checks:-null}",
  "replay_digest_run1": "${d1:-null}",
  "replay_digest_run2": "${d2:-null}",
  "transcript": "$(dig run1 transcript)",
  "state_A": "$(dig run1 state_A)",
  "state_B": "$(dig run1 state_B)",
  "state_C": "$(dig run1 state_C)",
  "fabric_sources_sha256": "$src_sha",
  "logs_sha256": "$log_sha",
  "host": "$(uname -srm)"
}
JSON
name=$(sha256sum "$W/receipt.json" | awk '{print $1}')
if [ -e "$OUTDIR/$name.json" ]; then
    echo "fabric_receipt: $OUTDIR/$name.json already exists; not rewritten" >&2
else
    cp "$W/receipt.json" "$OUTDIR/$name.json"
fi
echo "$verdict $OUTDIR/$name.json"
[ "$verdict" = PASS ]
