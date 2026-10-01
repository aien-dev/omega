#!/bin/sh
# M23 G1 search-trace corpus gate (lane 7). Correctness only: no timing is measured.
#  1. refuses a dirty tree (tracked or untracked changes outside build/)
#  2. builds the searchtrace tool and runs test-searchtrace (unit, refusal, ASan/UBSan, G3 format)
#  3. generates the corpus from the frozen task set twice and compares byte for byte
#  4. strict verify + replay; hooks-off equivalence on the frozen set
#  5. M9 and M14 gates on omegatool built from this tree (hooks compiled in, not installed)
#  6. stores the corpus as evidence/M23/corpus/corpus-<digest>.txt and writes a
#     content-addressed receipt evidence/M23/receipts/m23-corpus-<sha256>.json
# Never overwrites existing evidence: an existing file must hold identical bytes.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
cd "$ROOT"
TASKSET=tests/searchtrace/frozen_taskset_v1.txt
TOOL=build/searchtrace/searchtrace
EVD=evidence/M23
PHYSICS_DIR=${PHYSICS_DIR:-../physics}

die() { echo "m23-corpus-gate: REFUSED: $*" >&2; exit 2; }

[ -z "$(git status --porcelain --untracked-files=all)" ] || die "dirty tree; commit first (receipts record an exact commit)"
COMMIT=$(git rev-parse HEAD)
case "$COMMIT" in *[!0-9a-f]*|"") die "cannot read HEAD";; esac

make -s searchtrace test-searchtrace >/tmp/m23gate.$$.unit 2>&1 || { tail -20 /tmp/m23gate.$$.unit; die "test-searchtrace failed"; }
UNIT=$(grep -c '^st_corpus: .* PASS$\|^st_holdout: .* PASS$' /tmp/m23gate.$$.unit || true)
[ "$UNIT" = 4 ] || die "expected 4 unit PASS lines (plain+asan x corpus+holdout), got $UNIT"

T1=$(mktemp -d)
trap 'rm -rf "$T1" /tmp/m23gate.$$.*' EXIT
"$TOOL" run "$TASKSET" "$T1/run1.txt" >"$T1/run1.log"
"$TOOL" run "$TASKSET" "$T1/run2.txt" >"$T1/run2.log"
cmp -s "$T1/run1.txt" "$T1/run2.txt" || die "corpus differs between two runs"
VER=$("$TOOL" verify "$T1/run1.txt" "$TASKSET")
case "$VER" in "verify PASS digest="*) ;; *) die "verify: $VER";; esac
DIGEST=$(echo "$VER" | sed 's/.*digest=\([0-9a-f]*\).*/\1/')
STEPS=$(echo "$VER" | sed 's/.* steps=\([0-9]*\).*/\1/')
REPLAY=$("$TOOL" replay "$TASKSET" "$T1/run1.txt") || die "replay: $REPLAY"
HOOK=$("$TOOL" hookcheck "$TASKSET") || die "hookcheck: $HOOK"

make -s PHYSICS_DIR="$PHYSICS_DIR" build/omegatool >/dev/null 2>"$T1/build.err" || { tail -5 "$T1/build.err"; die "omegatool build failed"; }
./build/omegatool --run-m9-gates >"$T1/m9.log" 2>&1 || { tail -15 "$T1/m9.log"; die "M9 gates failed"; }
./build/omegatool --run-m14-gates >"$T1/m14.log" 2>&1 || { tail -15 "$T1/m14.log"; die "M14 gates failed"; }
tot() { sed -n 's/.*TOTAL GATES: *\([0-9]*\) *| *PASSED: *\([0-9]*\) *| *FAILED: *\([0-9]*\).*/\1 \2 \3/p' "$1" | tail -1; }
set -- $(tot "$T1/m9.log"); M9T=${1:-0}; M9P=${2:-0}; M9F=${3:-x}
set -- $(tot "$T1/m14.log"); M14T=${1:-0}; M14P=${2:-0}; M14F=${3:-x}
[ "$M9F" = 0 ] && [ "$M9P" = "$M9T" ] && [ "$M9T" -gt 0 ] || die "M9 totals not all PASS ($M9P/$M9T)"
[ "$M14F" = 0 ] && [ "$M14P" = "$M14T" ] && [ "$M14T" -gt 0 ] || die "M14 totals not all PASS ($M14P/$M14T)"

mkdir -p "$EVD/corpus" "$EVD/receipts"
CORPUS="$EVD/corpus/corpus-$DIGEST.txt"
if [ -e "$CORPUS" ]; then cmp -s "$CORPUS" "$T1/run1.txt" || die "$CORPUS exists with different bytes"
else cp "$T1/run1.txt" "$CORPUS"; fi
CFILE=$(sha256sum "$CORPUS" | cut -d' ' -f1)
TSHA=$(sha256sum "$TASKSET" | cut -d' ' -f1)
EST=$(pgrep -c est_load || true)

R="$T1/receipt.json"
{
  printf '{\n'
  printf '  "schema": "omega.m23.searchtrace.corpus.receipt.v1",\n'
  printf '  "verdict": "PASS",\n'
  printf '  "repo_commit": "%s",\n' "$COMMIT"
  printf '  "tree_clean": true,\n'
  printf '  "host_arch": "%s",\n' "$(uname -m)"
  printf '  "taskset": {"path": "%s", "sha256": "%s"},\n' "$TASKSET" "$TSHA"
  printf '  "corpus": {"path": "%s", "end_digest": "%s", "file_sha256": "%s", "steps": %s},\n' "$CORPUS" "$DIGEST" "$CFILE" "$STEPS"
  printf '  "two_runs_byte_identical": true,\n'
  printf '  "replay": "%s",\n' "$(echo "$REPLAY" | cut -d' ' -f2)"
  printf '  "hook_equivalence": "%s",\n' "$(echo "$HOOK" | cut -d' ' -f2)"
  printf '  "unit_and_refusal_tests": "PASS (plain + ASan/UBSan, corpus + G3 holdout)",\n'
  printf '  "m9_gates": {"exit": 0, "total": %s, "passed": %s, "failed": %s},\n' "$M9T" "$M9P" "$M9F"
  printf '  "m14_gates": {"exit": 0, "total": %s, "passed": %s, "failed": %s},\n' "$M14T" "$M14P" "$M14F"
  printf '  "timing_measured": false,\n'
  printf '  "est_load_processes_at_run": %s\n' "$EST"
  printf '}\n'
} >"$R"
RSHA=$(sha256sum "$R" | cut -d' ' -f1)
OUT="$EVD/receipts/m23-corpus-$RSHA.json"
[ -e "$OUT" ] || cp "$R" "$OUT"
echo "m23-corpus-gate: PASS corpus=$DIGEST steps=$STEPS receipt=$OUT"
