#!/bin/sh
# MIXED_ALGEBRA_DIGITAL_V1 closure gate (spec/mixed-algebra-digital-v1.md).
# Run through `make gate-mixed-algebra-digital-v1` from the repository root.
#
#   correctness leg -> timed bench leg (quiet flag held) -> selection over the
#   stored receipts (driver) -> separate-process reproduction -> wrapper
#   receipt evidence/MIXED_ALGEBRA/digital_v1/<sha256>.json (clean tree only).
#
# Environment: DV1_GATE, DV1_GATE_ASAN (driver binaries, set by make);
#   MA_DV1_REUSE_RUN=<run id> reuses evidence/MIXED_ALGEBRA/runs/<id>/
#   instead of running the bench (mode "reuse"); optional
#   MA_DV1_REUSE_SHA256="<run1 sha256> <run2 sha256>" pins those receipts to
#   externally recorded digests (e.g. from a published wrapper receipt).
# Exit: 0 PASS, 3 FAIL (receipt written), 4 refused (quiet flag present),
#   other = error.
set -u

MAKE_CMD=${MAKE:-make}
OUT=build/ma-dv1
QUIET="$HOME/workspace/.spark-quiet"
EV=evidence/MIXED_ALGEBRA
CONT1=$EV/ma3_bench_run1.json
CONT2=$EV/ma3_bench_run2.json

[ -n "${DV1_GATE:-}" ] && [ -n "${DV1_GATE_ASAN:-}" ] || { echo "run via make gate-mixed-algebra-digital-v1"; exit 2; }
[ -f spec/mixed-algebra-digital-v1.md ] || { echo "run from the omega repository root"; exit 2; }
mkdir -p "$OUT" || exit 2
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
WORK="$OUT/$STAMP"
mkdir "$WORK" || exit 2

dirty_now() {
    a=$(git status --porcelain --untracked-files=no | grep -c .)
    b=$(git status --porcelain -- src tests tools spec Makefile | grep -c .)
    if [ "$a" = 0 ] && [ "$b" = 0 ]; then echo 0; else echo 1; fi
}

DIRTY0=$(dirty_now)
COMMIT=$(git rev-parse HEAD)
echo "gate: commit $COMMIT tree_dirty=$DIRTY0"

# ---- correctness leg ------------------------------------------------------
CORR="$WORK/correctness.txt"
: > "$CORR"
run_check() { # label target summary-grep
    log="$WORK/$2.log"
    if $MAKE_CMD --no-print-directory "$2" > "$log" 2>&1; then st=PASS; else st=FAIL; fi
    line=$(grep -E "$3" "$log" | tr '\n' ';' | sed 's/;$//')
    [ -n "$line" ] || { line="no summary line"; st=FAIL; }
    echo "$st $1: $line" >> "$CORR"
    echo "gate: $st $1: $line"
}
run_check "test-algebra" test-algebra "^OMA ALGEBRA:"
run_check "test-algebra-asan" test-algebra-asan "^OMA ALGEBRA:"
run_check "test-realize (plain+asan)" test-realize "^MA2 test-realize"
run_check "test-turing (plain+asan)" test-turing "^test-turing:"
run_check "bottom coverage (plain+asan)" test-ma-digital-bottom "^MA_DIGITAL_V1 bottom"

# ---- measurement leg ------------------------------------------------------
if [ -n "${MA_DV1_REUSE_RUN:-}" ]; then
    MODE=reuse
    RUN_ID=$MA_DV1_REUSE_RUN
else
    MODE=fresh
    RUN_ID="$STAMP"
    if [ -e "$QUIET" ]; then
        echo "gate: REFUSED: $QUIET exists (another timed measurement). Wait and rerun; nothing written."
        exit 4
    fi
    touch "$QUIET" || exit 2
    trap 'rm -f "$QUIET"' EXIT INT TERM HUP
    echo "gate: quiet flag held; bench-mixed-algebra MA2_RUN_ID=$RUN_ID"
    $MAKE_CMD --no-print-directory bench-mixed-algebra MA2_RUN_ID="$RUN_ID" > "$WORK/bench.log" 2>&1
    brc=$?
    rm -f "$QUIET"
    trap - EXIT INT TERM HUP
    tail -3 "$WORK/bench.log"
    if [ $brc -ne 0 ]; then
        echo "FAIL bench-mixed-algebra: exit $brc (see $WORK/bench.log)" >> "$CORR"
    fi
fi
R1=$EV/runs/$RUN_ID/ma2_bench_run1.json
R2=$EV/runs/$RUN_ID/ma2_bench_run2.json
[ -f "$R1" ] && [ -f "$R2" ] || { echo "gate: missing receipts in $EV/runs/$RUN_ID"; exit 2; }

# Pin the receipt bytes now (right after the bench wrote them, or when a reuse
# run is chosen). The driver re-hashes against these before ingest and before
# writing the wrapper; any change in between is an error (exit 2), not a
# verdict. Reuse mode may pin externally recorded digests instead with
# MA_DV1_REUSE_SHA256="<run1 sha256> <run2 sha256>".
if [ "$MODE" = reuse ] && [ -n "${MA_DV1_REUSE_SHA256:-}" ]; then
    set -- $MA_DV1_REUSE_SHA256
    [ $# -eq 2 ] || { echo "gate: MA_DV1_REUSE_SHA256 needs two digests"; exit 2; }
    S1=$1; S2=$2
else
    S1=$(sha256sum "$R1" | cut -c1-64)
    S2=$(sha256sum "$R2" | cut -c1-64)
fi
S3=$(sha256sum "$CONT1" | cut -c1-64)
S4=$(sha256sum "$CONT2" | cut -c1-64)
DV1_EXPECT_SHA256="$R1=$S1 $R2=$S2 $CONT1=$S3 $CONT2=$S4"
export DV1_EXPECT_SHA256
echo "gate: pinned receipts $R1=$S1 $R2=$S2"

DIRTY1=$(dirty_now)
DIRTY=0
[ "$DIRTY0" = 0 ] && [ "$DIRTY1" = 0 ] && [ "$(git rev-parse HEAD)" = "$COMMIT" ] || DIRTY=1
git status --porcelain --untracked-files=all | sed -n 's/^?? //p' > "$WORK/untracked.txt"

# ---- selection reproduction in separate processes -------------------------
"$DV1_GATE" digests "$R1" "$R2" > "$WORK/digests_proc1.txt" || exit 2
"$DV1_GATE" digests "$R1" "$R2" > "$WORK/digests_proc2.txt" || exit 2
"$DV1_GATE_ASAN" digests "$R1" "$R2" > "$WORK/digests_asan.txt" || exit 2

# ---- decision + wrapper receipt -------------------------------------------
RCPT="$WORK/receipt.json"
DV1_COMMIT=$COMMIT DV1_TREE_DIRTY=$DIRTY DV1_MODE=$MODE DV1_RUN_ID=$RUN_ID DV1_CORRECTNESS="$CORR" \
DV1_UNTRACKED="$WORK/untracked.txt" \
DV1_REPRO_FILES="process_2=$WORK/digests_proc1.txt process_3=$WORK/digests_proc2.txt asan_process=$WORK/digests_asan.txt" \
    "$DV1_GATE" receipt "$RCPT" "$R1" "$R2" "$CONT1" "$CONT2"
grc=$?
[ $grc -eq 0 ] || [ $grc -eq 3 ] || { echo "gate: driver error $grc"; exit 2; }

if [ "$DIRTY" != 0 ]; then
    echo "gate: tree dirty; receipt kept at $RCPT only (not published to evidence/)"
    exit $grc
fi
H=$(sha256sum "$RCPT" | cut -c1-64)
DEST=$EV/digital_v1/$H.json
mkdir -p $EV/digital_v1
if [ -e "$DEST" ]; then echo "gate: $DEST exists; refusing to overwrite"; exit 2; fi
cp "$RCPT" "$DEST" || exit 2
echo "gate: receipt $DEST"
echo "gate: fresh receipts $R1 $R2"
$MAKE_CMD --no-print-directory check-mixed-algebra-evidence || exit 2
exit $grc
