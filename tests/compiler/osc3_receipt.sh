#!/bin/sh
# OSC-3 compiler slice receipt, one per item (docs/osc/OSC-3-DESIGN.md).
# OSC-3 slice; not a general Omega compiler; no self-hosting.
# Usage: tests/compiler/osc3_receipt.sh ITEM
#   ITEM: review | split | handles | dropflags | effects | identity | module
#  1. refuses a dirty tree: receipts record an exact commit
#  2. runs `make test-compiler-full` (everything, full counts) into a temporary OUT_DIR,
#     and `make test-compiler-quick` (wall time recorded; quick is never the evidence)
#  3. compiles every golden program with oscc; records IR and code digests and compares
#     every program that the OSC-2 baseline receipt lists (tests/compiler/osc3_baseline.json
#     names it) against its OSC-2 digests: identical unless OSC3_CODE_CHANGE_OK=1 and the
#     item states why (the receipt then lists the programs that changed)
#  4. runs the M6/M9/M14 gates (PHYSICS_DIR must point at the pinned physics checkout)
#     and compares each normalised gate log sha256 with the OSC-2 encoder receipt values
#  5. records every "^<item evidence>" line the test binaries print for this item
#     (lines starting with "osc3 <ITEM>:"), plain and ASan
#  6. writes evidence/OSC-3/receipts/osc3-<ITEM>-<sha256>.json; never overwrites evidence
set -eu
ITEM=${1:-}
case "$ITEM" in review|split|handles|dropflags|effects|identity|module) ;; *) echo "osc3-receipt: REFUSED: unknown ITEM '$ITEM'" >&2; exit 2 ;; esac
ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
cd "$ROOT"
EVD=evidence/OSC-3/receipts
BASEREC=$(ls evidence/OSC-2/receipts/osc2-encoder-*.json | head -1)
die() { echo "osc3-receipt: REFUSED: $*" >&2; exit 2; }
[ -n "${PHYSICS_DIR:-}" ] && [ -d "$PHYSICS_DIR" ] || die "PHYSICS_DIR must point at the pinned physics checkout (M6/M9/M14 gates are part of every OSC-3 receipt)"
[ -z "$(git status --porcelain --untracked-files=all)" ] || die "dirty tree; commit first (receipts record an exact commit)"
COMMIT=$(git rev-parse HEAD)
BASE=$(git merge-base HEAD origin/main) || die "no merge base with origin/main"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
O="$T/build/compiler"
nice make --no-print-directory OUT_DIR="$T/build" PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 test-compiler-full \
    >"$T/make.log" 2>&1 || { tail -30 "$T/make.log"; die "make test-compiler-full failed"; }
grep -q '^test-compiler: PASS' "$T/make.log" || die "no test-compiler (full) PASS line"
q0=$(date +%s)
nice make --no-print-directory OUT_DIR="$T/build" PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 test-compiler-quick \
    >"$T/quick.log" 2>&1 || { tail -30 "$T/quick.log"; die "make test-compiler-quick failed"; }
q1=$(date +%s)
grep -q '^test-compiler-quick: PASS' "$T/quick.log" || die "no test-compiler-quick PASS line"
QSECS=$((q1 - q0))

MSUM=$(grep '^OSC0B_MODEL_SEED=' "$O/model.out" | tail -1)
CLINE=$(grep '^golden=' "$O/compiler.out" | tail -1 | sed 's/ time=.*//')
CLINE_ASAN=$(grep '^golden=' "$O/compiler_asan.out" | tail -1 | sed 's/ time=.*//')
DLINE=$(grep '^programs=' "$O/determinism.out" | tail -1)
for v in "$MSUM" "$CLINE" "$CLINE_ASAN" "$DLINE"; do [ -n "$v" ] || die "cannot parse test output"; done
fz() { grep "^$1 fuzz:" "$2" | tail -1; }
for k in contract struct arena; do
    for f in "$O/compiler.out" "$O/compiler_asan.out"; do
        l=$(fz $k "$f"); [ -n "$l" ] || die "no $k fuzz line in $f"
        case "$k:$l" in contract:*) ;; *" mismatches=0") ;; *) die "$k fuzz mismatches in $f" ;; esac
    done
done
for f in "$O/compiler.out" "$O/compiler_asan.out"; do
    case "$(grep '^runtime model replay:' "$f" | tail -1)" in *" rejected=0") ;; *) die "runtime model replay rejected runs ($f)" ;; esac
done
case "$(grep '^legacy a64 differential:' "$O/legacy_a64.out" | tail -1)" in *" mode=full "*" mismatches=0") ;; *) die "legacy a64 full differential" ;; esac

# golden corpus vs the OSC-2 baseline receipt
# every program the OSC-2 baseline receipt lists must still exist (a deleted or renamed
# golden program must not silently drop out of the comparison)
for bp in $(grep -o '"program": "[^"]*"' "$BASEREC" | sed 's/"program": "//; s/"$//'); do
    [ -f "$bp" ] || die "OSC-2 baseline golden program $bp is missing (renamed or deleted)"
done
: >"$T/corpus.txt"; : >"$T/changed.txt"
NSAME=0; NBASE=0
for f in tests/compiler/progs/*.osc; do
    out=$("$O/oscc" "$f") || die "oscc refused golden program $f"
    ir=$(echo "$out" | sed -n 's/^ir_sha256=//p'); code=$(echo "$out" | sed -n 's/^code_sha256=//p')
    printf '%s %s %s %s\n' "$f" "$(sha256sum "$f" | cut -d' ' -f1)" "$ir" "$code" >>"$T/corpus.txt"
    b=$(grep "\"program\": \"$f\"" "$BASEREC" || true)
    if [ -n "$b" ]; then
        NBASE=$((NBASE + 1))
        case "$b" in *"\"ir_sha256\": \"$ir\", \"code_sha256\": \"$code\""*) NSAME=$((NSAME + 1)) ;; *) echo "$f" >>"$T/changed.txt" ;; esac
    fi
done
NCHG=$(wc -l <"$T/changed.txt" | tr -d ' ')
NBL=$(grep -c "\"program\": " "$BASEREC"); [ "$NBASE" = "$NBL" ] || die "compared $NBASE of $NBL OSC-2 baseline programs"
[ "$NCHG" = 0 ] || [ "${OSC3_CODE_CHANGE_OK:-0}" = 1 ] || { cat "$T/changed.txt"; die "$NCHG OSC-2 golden programs compile differently (set OSC3_CODE_CHANGE_OK=1 only if the item states why)"; }
CORPUS_SHA=$(sha256sum "$T/corpus.txt" | cut -d' ' -f1)
NPROG=$(wc -l <"$T/corpus.txt" | tr -d ' ')
NNEG=$(ls tests/compiler/neg/*.osc | wc -l | tr -d ' ')

# M6/M9/M14 gates, normalised log hash vs the OSC-2 encoder receipt
G=""
rm -rf build/osc3-receipt-gates; mkdir -p build/osc3-receipt-gates
for m in m6 m9 m14; do
    nice make --no-print-directory OUT_DIR=build/osc3-receipt-gates PHYSICS_DIR="$PHYSICS_DIR" "test-$m" >"$T/$m.log" 2>&1 \
        || { tail -20 "$T/$m.log"; die "test-$m failed"; }
    tot=$(grep 'TOTAL GATES' "$T/$m.log" | head -1 | sed 's/.*TOTAL GATES: \([0-9]*\) | PASSED: \([0-9]*\) | FAILED: \([0-9]*\).*/{"total": \1, "passed": \2, "failed": \3}/')
    case "$tot" in "{\"total\": "*", \"failed\": 0}") ;; *) die "test-$m: no gate total line with failed=0" ;; esac
    sed -n "/omegatool --run/,\$p" "$T/$m.log" | sed 's|\./build/[^/ ]*/omegatool|./OUT/omegatool|g' >"$T/$m.norm"
    gsha=$(sha256sum "$T/$m.norm" | cut -d' ' -f1)
    grep -q "\"$m\": {[^}]*\"gate_log_sha256\": \"$gsha\"" "$BASEREC" || die "test-$m gate log differs from the OSC-2 baseline ($gsha)"
    G="$G\"$m\": ${tot%\}}, \"gate_log_sha256\": \"$gsha\", \"identical_to_osc2_baseline\": true}, "
done
GATES="{${G%, }}"

esc() { sed 's/\\/\\\\/g; s/"/\\"/g'; }
R="$T/receipt.json"
{
  printf '{\n'
  printf '  "schema": "omega.osc3.compiler.receipt.v1",\n'
  printf '  "verdict": "PASS",\n'
  printf '  "item": "%s",\n' "$ITEM"
  printf '  "statement": "OSC-3 slice; not a general Omega compiler; no self-hosting.",\n'
  printf '  "design": "docs/osc/OSC-3-DESIGN.md",\n'
  printf '  "repo_commit": "%s",\n' "$COMMIT"
  printf '  "base_commit": "%s",\n' "$BASE"
  printf '  "tree_clean": true,\n'
  printf '  "host_arch": "%s",\n' "$(uname -m)"
  printf '  "c_compiler": "%s",\n' "$(${CC:-cc} --version 2>/dev/null | head -1 | esc)"
  printf '  "test_compiler_full": "PASS",\n'
  printf '  "test_compiler_quick": {"result": "PASS", "wall_seconds": %s, "budget_seconds": 60, "note": "nice, one make job; smoke counts, not evidence"},\n' "$QSECS"
  printf '  "osc0b_model": "%s",\n' "$(echo "$MSUM" | esc)"
  printf '  "compiler": "%s",\n' "$(echo "$CLINE" | esc)"
  printf '  "compiler_asan": "%s",\n' "$(echo "$CLINE_ASAN" | esc)"
  for k in contract struct arena; do
    printf '  "%s_fuzz": "%s",\n' "$k" "$(fz $k "$O/compiler.out" | esc)"
    printf '  "%s_fuzz_asan": "%s",\n' "$k" "$(fz $k "$O/compiler_asan.out" | esc)"
  done
  printf '  "runtime_model_replay": "%s",\n' "$(grep '^runtime model replay:' "$O/compiler.out" | tail -1 | esc)"
  printf '  "runtime_model_replay_asan": "%s",\n' "$(grep '^runtime model replay:' "$O/compiler_asan.out" | tail -1 | esc)"
  printf '  "legacy_a64_differential": "%s",\n' "$(grep '^legacy a64 differential:' "$O/legacy_a64.out" | tail -1 | esc)"
  printf '  "item_evidence": [\n'
  { grep -h "^osc3 $ITEM:" "$O/compiler.out" | sed 's/^/plain: /'; grep -h "^osc3 $ITEM:" "$O/compiler_asan.out" | sed 's/^/asan: /'; } | esc | sed 's/.*/    "&",/' >"$T/ev.txt" || true
  sed '$ s/,$//' "$T/ev.txt"
  printf '  ],\n'
  printf '  "determinism": "%s",\n' "$(echo "$DLINE" | esc)"
  printf '  "golden_programs": %s,\n' "$NPROG"
  printf '  "negative_programs": %s,\n' "$NNEG"
  printf '  "osc2_baseline_receipt": "%s",\n' "$BASEREC"
  printf '  "osc2_programs_identical": "%s of %s",\n' "$NSAME" "$NBASE"
  printf '  "osc2_programs_changed": [%s],\n' "$(sed 's/.*/"&"/' "$T/changed.txt" | paste -sd, -)"
  printf '  "golden_corpus_sha256": "%s",\n' "$CORPUS_SHA"
  printf '  "golden_corpus": [\n'
  n=0
  while read -r f src ir code; do
      n=$((n + 1)); sep=,; [ "$n" = "$NPROG" ] && sep=
      printf '    {"program": "%s", "source_sha256": "%s", "ir_sha256": "%s", "code_sha256": "%s"}%s\n' "$f" "$src" "$ir" "$code" "$sep"
  done <"$T/corpus.txt"
  printf '  ],\n'
  printf '  "m6_m9_m14_gates": %s,\n' "$GATES"
  printf '  "self_host": "no: the OSC-3 compiler slice cannot compile any part of itself (docs/osc/OSC-1-SELF-HOST-STATEMENT.md)",\n'
  printf '  "timing_measured": "quick wall time only"\n'
  printf '}\n'
} >"$R"
mkdir -p "$EVD"
RSHA=$(sha256sum "$R" | cut -d' ' -f1)
OUT="$EVD/osc3-$ITEM-$RSHA.json"
[ -e "$OUT" ] && { echo "osc3-receipt: identical receipt already present: $OUT"; exit 0; }
cp "$R" "$OUT"
echo "osc3-receipt: PASS receipt=$OUT"
